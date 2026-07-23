// daemon/noise/noise_metrics.cpp
// ④NoiseMetrics 实现。架构依据：docs/noise/architecture-design.md §3.6。
#include "noise_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace noise {

// IEEE 754 float <-> uint32_t 位转换（与 IDenoisePlugin set_dry_wet
// 同一做法）。
static inline uint32_t float_to_bits(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(float));
  return bits;
}
static inline float bits_to_float(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof(float));
  return f;
}

// 安全 dBFS：rms <= 0 返回 -100dBFS（静音下限，不产生 -inf）。
static inline float rms_to_dbfs(float rms) {
  if (rms <= 0.0f)
    return -100.0f;
  return 20.0f * std::log10(rms);
}

NoiseMetrics::NoiseMetrics() {
  // denoise_dry_wet_bits_ 默认 1.0f（IEEE 754 位模式）。
  denoise_dry_wet_bits_.store(float_to_bits(1.0f), std::memory_order_relaxed);
}

void NoiseMetrics::set_denoise_state(bool enabled, float dry_wet) {
  denoise_enabled_.store(enabled, std::memory_order_relaxed);
  denoise_dry_wet_bits_.store(float_to_bits(dry_wet),
                              std::memory_order_relaxed);
}

void NoiseMetrics::set_ref_result(float similarity,
                                  float noise_db,
                                  float delay_ms) {
  // Spec4 T5：comparison 线程写入 ref_* 字段。持锁与 collect() 互斥。
  // Spec4 T4：置 ref_configured_=true（首次后持久），告警引擎据此评估
  // ref_similarity 规则（避免未配置时 ref_similarity=0.0 误报 < 0.8）。
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  latest_.ref_similarity = similarity;
  latest_.ref_noise_db = noise_db;
  latest_.ref_delay_ms = delay_ms;
  ref_configured_ = true;
}

void NoiseMetrics::collect(const DenoiseResult& denoise,
                           const NoiseDetectionResult& detection,
                           const NoiseAnalysisResult& analysis,
                           float input_rms,
                           float denoised_rms) {
  // Spec3 Task 3：持锁写 latest_ + history_，与 HTTP 读路径互斥。
  // Phase 1 simple mutex - HTTP 读罕见，非 contends。Phase 3.6 改 seqlock
  // 做 lock-free RT（arch §11 待决项）。RT 路径持锁开销：单次非竞争
  // std::mutex lock/unlock ~25ns @ modern x86，48000Hz/480sample 帧预算
  // ~10ms，占比 < 0.001%。
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  // denoise（① DenoiseResult）保留供未来 VAD-probability 指标使用
  // （review Minor #3：参数签名按 brief 要求，此处标记 reserved）。
  (void)denoise;
  // ② 检测结果
  latest_.is_noisy = detection.is_noisy;
  latest_.noise_confidence = detection.confidence;
  latest_.estimated_snr_db = detection.estimated_snr_db;

  // ③ 分析结果
  latest_.noise_type = analysis.primary_type;
  latest_.noise_type_confidence = analysis.primary_confidence;
  latest_.is_mixed = analysis.is_mixed;
  // Spec5 T3：主结果来源层（l1/l2/l3）。
  latest_.noise_type_source = analysis.noise_type_source;
  // candidates：vector -> 定长 array（最多 3，避免 per-call 堆分配）。
  size_t n = std::min(analysis.candidates.size(), kMaxNoiseCandidates);
  for (size_t i = 0; i < n; ++i) {
    latest_.noise_candidates[i].type = analysis.candidates[i].type;
    latest_.noise_candidates[i].confidence = analysis.candidates[i].confidence;
  }
  latest_.noise_candidates_count = n;
  latest_.noise_level_dbfs = analysis.noise_level_dbfs;
  latest_.spectral_centroid_hz = analysis.spectral_centroid_hz;
  latest_.spectral_flatness = analysis.spectral_flatness;
  latest_.hum_strength_db = analysis.hum_strength_db;

  // ① 降噪效果
  // atomic 读 denoise_enabled / dry_wet（控制线程 set_denoise_state 写）。
  latest_.denoise_enabled = denoise_enabled_.load(std::memory_order_relaxed);
  latest_.denoise_dry_wet =
      bits_to_float(denoise_dry_wet_bits_.load(std::memory_order_relaxed));
  latest_.input_level_dbfs = rms_to_dbfs(input_rms);
  latest_.output_level_dbfs = rms_to_dbfs(denoised_rms);
  // noise_reduction_db = 20·log10(input_rms / denoised_rms)。
  // Guard divide-by-zero：denoised_rms <= 0 时设为 0（不产生 inf/nan，
  // brief D-S3.9 要求）。
  if (input_rms > 0.0f && denoised_rms > 0.0f) {
    latest_.noise_reduction_db = 20.0f * std::log10(input_rms / denoised_rms);
  } else {
    latest_.noise_reduction_db = 0.0f;
  }

  // 告警判定：Spec4 T4 升级为告警引擎评估（D-S4.2）。
  // collect() 不再直接设 is_alerting（Spec3 基础 OR 逻辑已移除）。
  // evaluate_alerts() 在 on_period_end（collect 之后）调用，产出
  // is_alerting + alert_level + 去抖状态。collect() 仅更新原始指标字段。

  // timestamp：用帧计数作为相对时间戳（非墙钟，足够 /history 序列化用）。
  latest_.timestamp_ms = frame_counter_;

  // 60s history ring：每 kHistorySampleIntervalFrames 帧采样一次。
  // review Minor #5：guard modulo-by-zero（测试 hook 可能传 0）。
  ++frame_counter_;
  if (history_sample_interval_ > 0 &&
      frame_counter_ % history_sample_interval_ == 0) {
    history_.push_back(latest_);
    if (history_.size() > kMaxHistorySize)
      history_.pop_front();
  }
}

// ── Spec4 T4：告警规则引擎实现（D-S4.2 + arch §3.6 规则表）──────────────
// 5 条规则，取最高级别作为"期望级别"（desired_level）：
//   1. noise_level_dbfs > alert_threshold_dbfs(-20 的 2/3 位置 = -20) ->
//   Critical
//      （arch §3.6：noise_level_dbfs > -20 -> Critical）
//   2. noise_level_dbfs > alert_threshold_dbfs(-30) -> Warning
//      （arch §3.6：noise_level_dbfs > -30 -> Warning）
//   3. estimated_snr_db < snr_alert_threshold_db(10) -> Warning
//   4. ref_similarity < ref_similarity_threshold(0.8) -> Warning
//      （GUARDED: 仅当 ref_configured_=true 才评估）
//   5. hum_strength_db > hum_alert_threshold_db(-40) -> Info
//
// 去抖（D-S4.2）：连续 N period（alert_debounce_periods）满足某级才 raise；
//   连续 N period 不满足才 clear。避免单 period 抖动导致频繁 raise/clear。
//   实现：desired_level 每次评估计算（瞬时）。raise_count_ 计数连续满足
//   desired_level 的 period 数；clear_count_ 计数连续 None 的 period 数。
//   状态转换：
//     - 未告警 -> raise：desired != None 连续 N period（raise_count_ >= N）
//     - 已告警 -> clear：desired == None 连续 N period（clear_count_ >= N）
//     - 已告警且 desired 级别变化（如 Warning -> Critical）：立即切换
//       （去抖已满足，级别变化是即时升级，不重新去抖 - 避免降级延迟）
//     - 已告警且 desired 降级（Critical -> Warning）：保持当前高级别
//       直到 clear（保守，不降级 - 避免抖动；UI 可看 alert_level 判断）
//       注：实际取 max(raised, desired) 会更保守，但需求是"去抖 N period
//       raise/clear"，级别变化用即时切换更合理。这里实现：
//       desired 升级或同级 -> 保持 raise（计数 reset 到 N）
//       desired 降级且 != None -> 保持 raised（不降级，直到 clear）
//       desired == None -> clear_count_++，达到 N 才 clear
std::optional<AlertEvent> NoiseMetrics::evaluate_alerts(uint8_t sensor_id) {
  std::lock_guard<std::mutex> lock(metrics_mutex_);

  // 计算当前 period 的期望告警级别（5 条规则取最高）。
  AlertLevel desired = AlertLevel::None;
  std::string rule;
  std::string message;

  // 规则 1+2：noise_level_dbfs（Critical > Warning）
  // arch §3.6：> -20 Critical，> -30 Warning。
  // 用 alert_threshold_dbfs 字段（默认 -30）作为 Warning 阈值；
  // Critical 阈值 = alert_threshold_dbfs + 10（默认 -20，与 arch §3.6 一致）。
  // 这样用户配置 alert_threshold_dbfs 时两级自动适配。
  if (latest_.noise_level_dbfs > latest_.alert_threshold_dbfs + 10.0f) {
    desired = AlertLevel::Critical;
    rule = "noise_level_dbfs";
    message = "noise level critical";
  } else if (latest_.noise_level_dbfs > latest_.alert_threshold_dbfs) {
    desired = AlertLevel::Warning;
    rule = "noise_level_dbfs";
    message = "noise level high";
  }

  // 规则 3：estimated_snr_db < snr_alert_threshold_db -> Warning
  // 仅当 desired < Warning 时评估（Warning 不被更低级覆盖）。
  if (desired < AlertLevel::Warning &&
      latest_.estimated_snr_db < latest_.snr_alert_threshold_db) {
    desired = AlertLevel::Warning;
    rule = "estimated_snr_db";
    message = "SNR low";
  }

  // 规则 4：ref_similarity < ref_similarity_threshold -> Warning
  // GUARDED：仅当 ref_configured_=true（RefComparator 已配置并写入过 ref_*）
  // 才评估。未配置时 ref_similarity 保持默认 0.0，0.0 < 0.8 会误报。
  if (desired < AlertLevel::Warning && ref_configured_ &&
      latest_.ref_similarity < latest_.ref_similarity_threshold) {
    desired = AlertLevel::Warning;
    rule = "ref_similarity";
    message = "reference similarity low";
  }

  // 规则 5：hum_strength_db > hum_alert_threshold_db -> Info
  // 仅当 desired < Info 时评估。
  if (desired < AlertLevel::Info &&
      latest_.hum_strength_db > latest_.hum_alert_threshold_db) {
    desired = AlertLevel::Info;
    rule = "hum_strength_db";
    message = "hum detected";
  }

  // 规则 6（Spec5 T2）：plugin_degraded -> Warning。
  // ONNX 降噪插件反复失败已切 passthrough（denoise 不可用，音频仍直通）。
  // 仅当 desired < Warning 时评估（不被更低级覆盖）。
  if (desired < AlertLevel::Warning && latest_.plugin_degraded) {
    desired = AlertLevel::Warning;
    rule = "plugin_degraded";
    message = "denoise plugin degraded to passthrough";
  }

  const uint32_t N = latest_.alert_debounce_periods;
  // 防御性：N=0 时去抖禁用，单 period 即 raise/clear（仍可用但无去抖）。
  const uint32_t debounce = (N > 0) ? N : 1;

  std::optional<AlertEvent> event;

  if (raised_level_ == AlertLevel::None) {
    // 当前未告警：检查是否达到 raise 条件。
    if (desired != AlertLevel::None) {
      ++alert_raise_count_;
      alert_clear_count_ = 0;
      if (alert_raise_count_ >= debounce) {
        // 达到去抖阈值 -> raise。
        raised_level_ = desired;
        latest_.is_alerting = true;
        latest_.alert_level = desired;
        AlertEvent ev;
        ev.sensor_id = sensor_id;
        ev.level = desired;
        ev.rule = rule;
        ev.message = message;
        ev.raised_at_ms = frame_counter_;
        ev.is_active = true;
        alert_history_.push_back(ev);
        if (alert_history_.size() > kMaxAlertHistorySize)
          alert_history_.pop_front();
        event = ev;
        // raise 后重置计数（下次 clear 需重新计数）。
        alert_raise_count_ = 0;
      }
    } else {
      // 持续无告警，重置 raise 计数。
      alert_raise_count_ = 0;
    }
  } else {
    // 当前已告警（raised_level_ != None）。
    if (desired == AlertLevel::None) {
      // 期望无告警 -> clear 计数。
      ++alert_clear_count_;
      alert_raise_count_ = 0;
      if (alert_clear_count_ >= debounce) {
        // 达到去抖阈值 -> clear。
        AlertEvent ev;
        ev.sensor_id = sensor_id;
        ev.level = AlertLevel::None;
        ev.rule = "recovered";
        ev.message = "alert cleared";
        ev.raised_at_ms = frame_counter_;
        ev.is_active = false;
        alert_history_.push_back(ev);
        if (alert_history_.size() > kMaxAlertHistorySize)
          alert_history_.pop_front();
        event = ev;
        raised_level_ = AlertLevel::None;
        latest_.is_alerting = false;
        latest_.alert_level = AlertLevel::None;
        alert_clear_count_ = 0;
      }
    } else if (desired > raised_level_) {
      // 期望级别升级（如 Warning -> Critical）：即时升级，不重新去抖。
      // 升级是更严重状态，不应延迟（保守安全）。
      raised_level_ = desired;
      latest_.alert_level = desired;
      AlertEvent ev;
      ev.sensor_id = sensor_id;
      ev.level = desired;
      ev.rule = rule;
      ev.message = message + " (escalated)";
      ev.raised_at_ms = frame_counter_;
      ev.is_active = true;
      alert_history_.push_back(ev);
      if (alert_history_.size() > kMaxAlertHistorySize)
        alert_history_.pop_front();
      event = ev;
      alert_clear_count_ = 0;
    } else {
      // 期望同级或降级（desired <= raised_level_ 且 desired != None）：
      // 保持当前 raised_level_（不降级，直到 clear）。
      // 重置 clear 计数（仍有告警条件）。
      alert_clear_count_ = 0;
    }
  }

  // 同步 is_alerting + alert_level 到 latest_（确保 get_snapshot 返回一致）。
  latest_.is_alerting = (raised_level_ != AlertLevel::None);
  latest_.alert_level = raised_level_;

  return event;
}

}  // namespace noise

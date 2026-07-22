// daemon/noise/noise_manager.cpp
// 架构依据：docs/noise/architecture-design.md §3.7。
#include "noise_manager.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include <boost/foreach.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "noise_status.hpp"  // write_atomic
#include "noise_template_db.hpp"

namespace noise {

NoiseManager::NoiseManager(NoiseAudioBridge& bridge) : bridge_(bridge) {
  // Spec3 T8b（C2 修复）：向 Bridge 注册 period 生命周期回调，使
  // PcmCaptureService provider 回调能驱动 on_period_begin/on_period_end
  // （每个 ALSA period 恰好一次，全局，非 per-sink）。
  // 此前 register_frame_provider 是 stub，生产 pipeline 永不运行 on_frame。
  bridge_.set_period_lifecycle_callbacks([this]() { on_period_begin(); },
                                         [this]() { on_period_end(); });
}

NoiseManager::~NoiseManager() = default;

bool NoiseManager::add_sensor(uint8_t sensor_id,
                              uint8_t sink_id,
                              const NoiseSensorConfig& cfg) {
  // 控制线程：COW 复制当前表 -> 加 sensor -> 原子 publish -> 旧表入 retire
  std::lock_guard<std::mutex> lock(ctrl_mutex_);
  const SensorTable* current = sensor_table_.load();
  auto new_table = std::make_shared<SensorTable>(*current);

  SensorContext ctx;
  ctx.sink_id = sink_id;
  ctx.denoise_enabled = cfg.denoise_enabled;
  // ① DenoiseProcessor：构造即装 PassthroughPlugin（plugin_ 永不为空）。
  ctx.denoise = std::make_shared<DenoiseProcessor>();
  // 若 cfg 指定非 passthrough 插件，切换（如 "rnnoise"）。
  if (!cfg.plugin_name.empty() && cfg.plugin_name != "passthrough") {
    ctx.denoise->switch_plugin(cfg.plugin_name);
    // 控制线程立即回收 retired slot（若 switch 成功，旧 slot 已入 retire）。
    ctx.denoise->drain_retire();
  }
  // dry_wet 应用（控制线程 -> plugin->set_dry_wet）。
  ctx.denoise->set_dry_wet(cfg.dry_wet);
  // ② NoiseDetector（监测角色，SimpleEnergyVad）。
  ctx.detector = std::make_shared<NoiseDetector>();
  ctx.detector->set_sensitivity(cfg.sensitivity);
  // ③ NoiseAnalyzer（L1 规则式 + L2 模板匹配）。
  ctx.analyzer = std::make_shared<NoiseAnalyzer>();
  // ④ NoiseMetrics（Spec3 Task 2 真聚合，替 Spec2 stub）。
  // set_denoise_state 用 atomic 写，collect() RT 路径 atomic 读 -> 无竞争。
  ctx.metrics = std::make_shared<NoiseMetrics>();
  ctx.metrics->set_denoise_state(cfg.denoise_enabled, cfg.dry_wet);
  // 共享观测状态：shared_ptr 使 COW 表复制时旧表/新表共享同一计数器/结果，
  // on_frame 在 pinned 旧表上递增会反映到新表（stub_call_count_for_test 读
  // 新表）。与 detector/analyzer 等 shared_ptr 成员同语义。
  ctx.last_analysis = std::make_shared<NoiseAnalysisResult>();
  ctx.frame_count = std::make_shared<std::atomic<size_t>>(0);
  // Spec3 Task 7 path A：plugin reset 计数（控制线程 on_capture_thread_joined
  // 写，plugin_reset_count_for_test 读，shared_ptr 跨 COW 表共享）。
  ctx.reset_count = std::make_shared<std::atomic<size_t>>(0);
  // Spec3 Task 4：保存原始 cfg 供 save_status 序列化（arch §7.4）。
  ctx.cfg = cfg;

  (*new_table)[sensor_id] = std::move(ctx);
  auto old = sensor_table_.publish(std::move(new_table));
  retire_queue_.retire(std::move(old), sensor_table_.epoch());
  // 控制线程回收（勿在 RT 路径调，reclaim_older_than 持 mutex）
  retire_queue_.reclaim_older_than(sensor_table_.epoch());
  // Spec3 T8b（C2 修复）：向 Bridge 注册 FrameProvider，使 PcmCaptureService
  // 分发的 ALSA period 帧经 Bridge 解复用后路由到 on_frame（arch §3.7 L791
  // "向 Bridge 注册 FrameProvider"）。此前 add_sensor 不注册 frame provider，
  // 生产 pipeline 永不运行 on_frame -> metrics 留默认 -> /denoised 404。
  // channel_map 默认 {0}（Phase 1 单通道，arch §4.2 "channels 恒为 1"）。
  // NoiseSensorConfig 无 map 字段，Phase 1 固定 channel 0。
  bridge_.register_frame_provider(
      sink_id, {0},
      [this](uint8_t sid, const float* frames, size_t n, uint8_t /*ch*/) {
        on_frame(sid, frames, n);
      });
  // Spec3 Task 4：变更即保存（arch §7.6）。status_file_ 空时 no-op。
  save_status();
  return true;
}

bool NoiseManager::switch_plugin(uint8_t sensor_id, const std::string& name) {
  // 控制线程：lookup sensor -> denoise->switch_plugin(name) + drain_retire。
  std::lock_guard<std::mutex> lock(ctrl_mutex_);
  const SensorTable* current = sensor_table_.load();
  auto it = current->find(sensor_id);
  if (it == current->end() || !it->second.denoise)
    return false;
  bool ok = it->second.denoise->switch_plugin(name);
  // drain_retire 控制线程专用：回收穿越 >=2 静止点的 retired PluginSlot。
  // switch_plugin 后立即调用，确保旧插件析构（ONNX session teardown /
  // rnnoise_destroy，毫秒级）在控制线程完成，不堆积在 retire_list_。
  it->second.denoise->drain_retire();
  // Spec3 Task 4：变更即保存。更新 cfg.plugin_name（mutable，arch §7.4）。
  if (ok) {
    it->second.cfg.plugin_name = name;
    save_status();
  }
  return ok;
}

bool NoiseManager::remove_sensor(uint8_t sensor_id) {
  // 控制线程：COW 复制当前表 -> erase sensor -> publish -> retire 旧表。
  // 同 add_sensor 的 COW 模式（arch §3.7 L860 读路径无锁约束）。
  std::lock_guard<std::mutex> lock(ctrl_mutex_);
  const SensorTable* current = sensor_table_.load();
  auto it = current->find(sensor_id);
  if (it == current->end())
    return false;  // 不存在
  // Spec3 T8b（C2 修复）：注销 Bridge FrameProvider，停止向该 sink 分发帧。
  bridge_.unregister_frame_provider(it->second.sink_id);
  auto new_table = std::make_shared<SensorTable>(*current);
  new_table->erase(sensor_id);
  auto old = sensor_table_.publish(std::move(new_table));
  retire_queue_.retire(std::move(old), sensor_table_.epoch());
  retire_queue_.reclaim_older_than(sensor_table_.epoch());
  // Spec3 Task 4：变更即保存（arch §7.6）。
  save_status();
  return true;
}

bool NoiseManager::enable_sensor(uint8_t sensor_id, bool enabled) {
  // 控制线程：COW 复制 -> set enabled -> publish -> retire 旧表。
  // 同 add_sensor/remove_sensor 的 COW 模式（不直接改 pinned 表）。
  std::lock_guard<std::mutex> lock(ctrl_mutex_);
  const SensorTable* current = sensor_table_.load();
  auto it = current->find(sensor_id);
  if (it == current->end())
    return false;
  auto new_table = std::make_shared<SensorTable>(*current);
  (*new_table)[sensor_id].enabled = enabled;
  auto old = sensor_table_.publish(std::move(new_table));
  retire_queue_.retire(std::move(old), sensor_table_.epoch());
  retire_queue_.reclaim_older_than(sensor_table_.epoch());
  // Spec3 Task 4：变更即保存（arch §7.6）。
  save_status();
  return true;
}

bool NoiseManager::set_dry_wet(uint8_t sensor_id, float dry_wet) {
  // 控制线程：lookup sensor -> denoise->set_dry_wet（plugin 原子 setter，
  // 不 COW，同 switch_plugin 参数变更先例）+ metrics->set_denoise_state
  // 更新快照 dry_wet（供 HTTP /sensor 响应）。
  std::lock_guard<std::mutex> lock(ctrl_mutex_);
  const SensorTable* current = sensor_table_.load();
  auto it = current->find(sensor_id);
  if (it == current->end() || !it->second.denoise)
    return false;
  it->second.denoise->set_dry_wet(dry_wet);
  if (it->second.metrics)
    it->second.metrics->set_denoise_state(it->second.denoise_enabled, dry_wet);
  // Spec3 Task 4：变更即保存。更新 cfg.dry_wet（mutable，arch §7.4）。
  it->second.cfg.dry_wet = dry_wet;
  save_status();
  return true;
}

bool NoiseManager::set_param(uint8_t sensor_id,
                             const std::string& key,
                             const std::string& value) {
  // 控制线程：lookup sensor -> denoise->set_param（plugin 原子 setter）。
  std::lock_guard<std::mutex> lock(ctrl_mutex_);
  const SensorTable* current = sensor_table_.load();
  auto it = current->find(sensor_id);
  if (it == current->end() || !it->second.denoise)
    return false;
  bool ok = it->second.denoise->set_param(key, value);
  // Spec3 Task 4：变更即保存。set_param 成功时持久化（arch §7.6）。
  // 不跟踪 generic param map（YAGNI，Phase 1 仅 dry_wet/plugin_name）。
  if (ok)
    save_status();
  return ok;
}

void NoiseManager::on_period_begin() {
  // period 顶部 load 快照，整 period 内 on_frame 复用
  pinned_table_ = sensor_table_.load();
  for (auto& [id, ctx] : *pinned_table_) {
    (void)id;
    if (ctx.denoise)
      ctx.denoise->on_period_begin();
    if (ctx.metrics)
      ctx.metrics->on_period_begin();
  }
}

void NoiseManager::on_frame(uint8_t sink_id,
                            const float* frames,
                            size_t frame_size) {
  // PTP 未锁时跳过处理（arch §3.7 L862 ②）
  if (!ptp_locked_.load())
    return;
  if (pinned_table_ == nullptr)
    return;
  // 按 sink_id 路由到对应 sensor
  for (auto& [id, ctx] : *pinned_table_) {
    (void)id;
    if (ctx.sink_id != sink_id)
      continue;
    if (!ctx.denoise || !ctx.detector || !ctx.analyzer || !ctx.metrics)
      return;
    // 计数 on_frame 调用（#3 兼容：stub_call_count_for_test 用此字段）。
    // shared_ptr<atomic<size_t>>: 跨表共享（COW 复制后旧/新表同一计数器）。
    if (ctx.frame_count)
      ctx.frame_count->fetch_add(1, std::memory_order_relaxed);

    // ① DenoiseProcessor.process：写 back_（original/denoised/noise）。
    //    返回 n = 实际输出样本数（plugin 可能因首帧延迟返回 < n_in）。
    DenoiseResult denoise_result;
    size_t n = ctx.denoise->process(frames, frame_size, &denoise_result);
    // 当前 period 的数据视图（back_，刚写入）。get_current_output 与
    // get_output 的区别见 denoise_processor.hpp 注释。
    const DenoiseOutput* out = ctx.denoise->get_current_output();

    // ② NoiseDetectionResult 构建（分析源选择 §3.3.1）：
    //   denoise_enabled=true  -> RNNoise VAD 为主，Detector VAD 辅助（SF/SNR
    //                            交叉验证），VAD 取 RNNoise。
    //   denoise_enabled=false -> Detector VAD 为唯一源。
    NoiseDetectionResult detection{};
    if (ctx.denoise_enabled) {
      // RNNoise VAD 为主（denoise_result.has_vad 时取其概率）。
      detection.is_speech =
          denoise_result.has_vad && (denoise_result.vad_probability > 0.5f);
      // 同时调 Detector 做监测（SF/SNR 用于交叉验证），但 VAD 不覆盖。
      NoiseDetectionResult det_monitoring =
          ctx.detector->process_frame(frames, frame_size);
      detection.spectral_flatness = det_monitoring.spectral_flatness;
      detection.estimated_snr_db = det_monitoring.estimated_snr_db;
      detection.confidence = det_monitoring.confidence;
      detection.is_noisy = det_monitoring.is_noisy;
    } else {
      detection = ctx.detector->process_frame(frames, frame_size);
    }

    // ③ 分析源选择（arch §3.3.1）：
    //   denoise_enabled=true  -> NoisePCM (out->noise = original - denoised)
    //                            纯噪声分量，分类最准
    //   denoise_enabled=false -> OriginalPCM (frames)
    const float* analysis_pcm = frames;
    size_t analysis_n = frame_size;
    if (ctx.denoise_enabled && out != nullptr && out->noise != nullptr) {
      analysis_pcm = out->noise;
      analysis_n = n;
    }
    NoiseAnalysisResult ar =
        ctx.analyzer->analyze(analysis_pcm, analysis_n, detection);
    *ctx.last_analysis =
        ar;  // 供 get_analysis_result_for_test（共享指针，跨表可见）

    // ④ NoiseMetrics 真聚合（Spec3 Task 2，替 Spec2 stub no-op）。
    // input_rms = RMS(frames)（原始 PCM），denoised_rms = RMS(out->denoised)
    // （降噪 PCM）。out 可能为 nullptr（首帧或异常），此时 denoised_rms=0，
    // collect() 内部 guard divide-by-zero（noise_reduction_db=0）。
    float input_rms = 0.0f;
    for (size_t i = 0; i < frame_size; ++i)
      input_rms += frames[i] * frames[i];
    input_rms = std::sqrt(input_rms / static_cast<float>(frame_size));
    float denoised_rms = 0.0f;
    if (out != nullptr && out->denoised != nullptr && n > 0) {
      for (size_t i = 0; i < n; ++i)
        denoised_rms += out->denoised[i] * out->denoised[i];
      denoised_rms = std::sqrt(denoised_rms / static_cast<float>(n));
    }
    ctx.metrics->collect(denoise_result, detection, ar, input_rms,
                         denoised_rms);
    break;
  }
}

void NoiseManager::on_period_end() {
  if (pinned_table_ != nullptr) {
    for (auto& [id, ctx] : *pinned_table_) {
      (void)id;
      if (ctx.denoise)
        ctx.denoise->on_period_end();
      if (ctx.metrics)
        ctx.metrics->on_period_end();
    }
  }
  pinned_table_ = nullptr;
  sensor_table_.advance_epoch();
}

void NoiseManager::on_ptp_unlocked() {
  // arch §3.7 L862 path A：仅置标志，不直接调 plugin->reset()（会与 RT
  // process() 竞态）。真实 reset 由 on_capture_thread_joined() 在
  // PcmCaptureService join capture 线程后调用（capture 线程静止 -> 无 in-flight
  // process()）。 不设 path B（arch L862：SCHED_OTHER 下 RT 线程可能被抢占在
  // process 中途 致 epoch 不推进，path B 或 livelock 或与停滞 process 竞态）。
  ptp_locked_.store(false);
  reset_pending_.store(true);
}

void NoiseManager::on_ptp_locked() {
  // Spec3 Task 6b（C1 修复）：on_ptp_unlocked 的对偶。置 ptp_locked_=true
  // 启用 pipeline（on_frame 不再短路）+ 清 reset_pending_（若有先前 unlock
  // 残留的 pending reset，PTP 重新锁定后不再需要）。均 atomic，无锁。
  // 生产环境由 PcmCaptureService::on_ptp_status_change 经
  // ptp_status_forward_cb_("locked") 转发调用。此前 ptp_locked_ 仅由 test hook
  // set_ptp_locked_for_test 设置，生产 pipeline 永不运行（C1）。
  ptp_locked_.store(true);
  reset_pending_.store(false);
}

void NoiseManager::on_capture_thread_joined() {
  // arch §3.7 L862 path A gate：PcmCaptureService 在 PTP unlock 时
  // snd_pcm_drop+close+join capture 线程后回调本方法（控制线程）。
  // 前置条件（由调用方保证）：capture 线程已 join，不再调 on_frame -> 无
  // in-flight process() -> 安全 reset plugin 有状态成员（RNNoise
  // DenoiseState / DTLN LSTM / DF STFT 缓冲）。
  //
  // reset_pending_=false 时 no-op：避免 PcmCaptureService 在非 PTP-unlock
  // 路径的 stop_capture（如 terminate）误触发 reset，也避免重复 reset。
  if (!reset_pending_.load())
    return;
  // 控制线程遍历当前 sensor 表（RCU load，安全）。每个 sensor 的 denoise
  // processor 经 RcuPtr<PluginSlot> load 当前 plugin -> reset()。
  // plugin->reset() 实现须仅清内部状态（不分配/不释放，RT-safe 契约），
  // 控制线程调用安全。
  const SensorTable* tbl = sensor_table_.load();
  if (tbl != nullptr) {
    for (auto& [id, ctx] : *tbl) {
      (void)id;
      if (ctx.denoise) {
        // DenoiseProcessor 暴露 reset() 转发到当前 plugin slot（控制线程
        // load rcu_ptr_，安全）。
        ctx.denoise->reset_plugin();
        if (ctx.reset_count)
          ctx.reset_count->fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  reset_pending_.store(false);
}

// ── Spec3 Task 6 Streamer 三路数据通路（arch §4.4）──────────────────────
// Streamer / HTTP 控制线程调用：返回 front 缓冲（previous period）。
// RCU load sensor 表（控制线程读，安全），lookup by sink_id -> denoise->
// get_output()（DenoiseProcessor::get_output 返回 front_view_）。
const DenoiseOutput* NoiseManager::get_denoise_output(uint8_t sink_id) const {
  const SensorTable* tbl = sensor_table_.load();
  if (tbl == nullptr)
    return nullptr;
  for (const auto& [id, ctx] : *tbl) {
    (void)id;
    if (ctx.sink_id != sink_id)
      continue;
    // denoise 关 -> 返回 nullptr（HTTP /denoised 404，arch §5.2）。
    if (!ctx.denoise_enabled || !ctx.denoise)
      return nullptr;
    return ctx.denoise->get_output();
  }
  return nullptr;
}

size_t NoiseManager::sensor_count_for_test() const {
  const SensorTable* tbl = sensor_table_.load();
  return tbl ? tbl->size() : 0;
}

size_t NoiseManager::stub_call_count_for_test(uint8_t sensor_id) const {
  const SensorTable* tbl = sensor_table_.load();
  if (tbl == nullptr)
    return 0;
  auto it = tbl->find(sensor_id);
  if (it == tbl->end())
    return 0;
  // Task 7: 返回 on_frame 调用次数（shared_ptr<atomic<size_t>>，跨表共享）。
  // 保留 Task 1 方法名以兼容既有测试。
  const auto& fc = it->second.frame_count;
  return fc ? fc->load(std::memory_order_relaxed) : 0;
}

size_t NoiseManager::plugin_reset_count_for_test(uint8_t sensor_id) const {
  // Spec3 Task 7 path A：返回 sensor 的 plugin->reset() 累计调用次数。
  const SensorTable* tbl = sensor_table_.load();
  if (tbl == nullptr)
    return 0;
  auto it = tbl->find(sensor_id);
  if (it == tbl->end())
    return 0;
  const auto& rc = it->second.reset_count;
  return rc ? rc->load(std::memory_order_relaxed) : 0;
}

NoiseAnalysisResult NoiseManager::get_analysis_result_for_test(
    uint8_t sensor_id) const {
  const SensorTable* tbl = sensor_table_.load();
  if (tbl == nullptr)
    return NoiseAnalysisResult{};
  auto it = tbl->find(sensor_id);
  if (it == tbl->end())
    return NoiseAnalysisResult{};
  // shared_ptr<NoiseAnalysisResult>：on_frame 写 -> test 读（同表或 COW
  // 副本）。
  const auto& la = it->second.last_analysis;
  return la ? *la : NoiseAnalysisResult{};
}

NoiseMetricsSnapshot NoiseManager::get_metrics_for_test(
    uint8_t sensor_id) const {
  const SensorTable* tbl = sensor_table_.load();
  if (tbl == nullptr)
    return NoiseMetricsSnapshot{};
  auto it = tbl->find(sensor_id);
  if (it == tbl->end() || !it->second.metrics)
    return NoiseMetricsSnapshot{};
  return it->second.metrics->snapshot_for_test();
}

std::deque<NoiseMetricsSnapshot> NoiseManager::get_history_for_test(
    uint8_t sensor_id) const {
  const SensorTable* tbl = sensor_table_.load();
  if (tbl == nullptr)
    return {};
  auto it = tbl->find(sensor_id);
  if (it == tbl->end() || !it->second.metrics)
    return {};
  return it->second.metrics->get_history_for_test();
}

// ── Spec3 Task 3 同步读路径实现 ─────────────────────────────────────────
// 见 noise_manager.hpp 注释：HTTP 控制线程调用，metrics->get_snapshot() 持锁
// 与 collect() (RT 写) 互斥。SensorContext
// 字段（sink_id/enabled/denoise_enabled） 经 RCU 表发布后稳定，不需额外锁。
bool NoiseManager::get_sensor_info(uint8_t sensor_id, SensorInfo& out) const {
  const SensorTable* tbl = sensor_table_.load();
  if (tbl == nullptr)
    return false;
  auto it = tbl->find(sensor_id);
  if (it == tbl->end())
    return false;
  const auto& ctx = it->second;
  out.id = sensor_id;
  out.sink_id = ctx.sink_id;
  out.enabled = ctx.enabled;
  out.denoise_enabled = ctx.denoise_enabled;
  if (ctx.metrics)
    out.metrics = ctx.metrics->get_snapshot();  // 持 metrics_mutex_
  else
    out.metrics = NoiseMetricsSnapshot{};
  return true;
}

std::vector<std::pair<uint8_t, SensorInfo>> NoiseManager::list_sensor_infos()
    const {
  std::vector<std::pair<uint8_t, SensorInfo>> result;
  const SensorTable* tbl = sensor_table_.load();
  if (tbl == nullptr)
    return result;
  for (const auto& [id, ctx] : *tbl) {
    SensorInfo info;
    info.id = id;
    info.sink_id = ctx.sink_id;
    info.enabled = ctx.enabled;
    info.denoise_enabled = ctx.denoise_enabled;
    if (ctx.metrics)
      info.metrics = ctx.metrics->get_snapshot();  // 持 metrics_mutex_
    result.emplace_back(id, std::move(info));
  }
  return result;
}

bool NoiseManager::get_metrics_snapshot(uint8_t sensor_id,
                                        NoiseMetricsSnapshot& out) const {
  const SensorTable* tbl = sensor_table_.load();
  if (tbl == nullptr)
    return false;
  auto it = tbl->find(sensor_id);
  if (it == tbl->end() || !it->second.metrics)
    return false;
  out = it->second.metrics->get_snapshot();  // 持 metrics_mutex_
  return true;
}

std::vector<NoiseMetricsSnapshot> NoiseManager::get_history_snapshot(
    uint8_t sensor_id) const {
  const SensorTable* tbl = sensor_table_.load();
  if (tbl == nullptr)
    return {};
  auto it = tbl->find(sensor_id);
  if (it == tbl->end() || !it->second.metrics)
    return {};
  auto hist = it->second.metrics->get_history();  // 持 metrics_mutex_
  return std::vector<NoiseMetricsSnapshot>(hist.begin(), hist.end());
}

// ── Spec3 Task 4 持久化实现（arch §7.6）────────────────────────────────
// JSON 输出 = 手工拼接（与 daemon/json.cpp + noise_http.cpp 同一模式）：
// 数字/bool 不加引号，字符串加引号 + escape_json（共享自 noise_status.hpp，
// review Minor #5 去重）。
// 不用 boost::property_tree::write_json（会将所有值引号化，违反约定）。
// 输入用 boost::property_tree::ptree + read_json（daemon 既有模式）。

bool NoiseManager::save_status() const {
  // Gate 1: status_file_ 空时 no-op（arch §7.3 注：空字符串禁用持久化）。
  // 既有 T2/T3 测试调用 add_sensor 等不设 status_file_，gate 使 save-on-change
  // 不可见，不污染 worktree 也不写文件。
  if (status_file_.empty())
    return false;
  // Gate 2: load_in_progress_ 置位时跳过（review Minor #7）。
  // load_status 内每次 add_sensor 会触发 save_status，若不跳过会写 N 次
  // 中间态（progressively-larger partial state）。load 结束后由
  // load_status 末尾显式调一次 save_status 持久化最终状态。
  if (load_in_progress_.load(std::memory_order_relaxed))
    return false;
  const SensorTable* tbl = sensor_table_.load();
  if (tbl == nullptr)
    return false;
  // 序列化 sensors 为 noise_status.json（arch §7.4 格式）。
  // SensorContext.cfg 是 mutable，const 方法可读。RT 线程不读 cfg -> 无竞争。
  std::ostringstream ss;
  ss << "{\n  \"sensors\": [";
  bool first = true;
  for (const auto& [id, ctx] : *tbl) {
    if (!first)
      ss << ",";
    first = false;
    ss << "\n    {" << "\n      \"id\": " << static_cast<unsigned>(id)
       << ",\n      \"sink_id\": " << static_cast<unsigned>(ctx.sink_id)
       << ",\n      \"enabled\": " << (ctx.enabled ? "true" : "false")
       << ",\n      \"denoise_enabled\": "
       << (ctx.cfg.denoise_enabled ? "true" : "false")
       << ",\n      \"denoise_plugin\": \"" << escape_json(ctx.cfg.plugin_name)
       << "\"" << ",\n      \"denoise_dry_wet\": " << ctx.cfg.dry_wet
       << ",\n      \"sensitivity\": " << ctx.cfg.sensitivity << "\n    }";
  }
  ss << "\n  ],\n  \"global\": {\n    \"noise_max_sensors\": 16\n  }\n}\n";
  return write_atomic(status_file_, ss.str());
}

bool NoiseManager::save_status_on_exit() {
  // review Minor #2：持 ctrl_mutex_ 防止与并发 HTTP 控制操作（set_dry_wet 等）
  // 竞态改 ctx.cfg。shutdown 序列无嵌套控制调用，无死锁。
  std::lock_guard<std::mutex> lock(ctrl_mutex_);
  return save_status();
}

bool NoiseManager::load_status(const std::string& noise_status_file) {
  // review Important #1：模板持久化是调用方职责（T6 wiring
  //   template_db->load(config.get_noise_template_dir())），本方法仅加载
  //   sensors。
  // 设置 status_file_ 供后续 save-on-change 使用。
  if (!noise_status_file.empty())
    status_file_ = noise_status_file;
  if (noise_status_file.empty())
    return false;
  // 文件不存在视为首次启动，非错误（返回 false 表示未加载任何传感器）。
  if (!std::filesystem::exists(noise_status_file))
    return false;
  // review Minor #7：置位 load_in_progress_，跳过 add_sensor 内部 save_status
  // 的 N 次中间态写入。load 结束后一次性 save_status 持久化最终状态。
  load_in_progress_.store(true, std::memory_order_relaxed);
  bool sensors_ok = true;
  try {
    boost::property_tree::ptree pt;
    std::ifstream in(noise_status_file, std::ios::binary);
    if (!in.is_open()) {
      std::cerr << "NoiseManager::load_status: cannot open "
                << noise_status_file << std::endl;
      sensors_ok = false;
    } else {
      boost::property_tree::read_json(in, pt);
      BOOST_FOREACH (const boost::property_tree::ptree::value_type& v,
                     pt.get_child("sensors")) {
        uint8_t sensor_id = static_cast<uint8_t>(v.second.get<unsigned>("id"));
        NoiseSensorConfig cfg;
        cfg.denoise_enabled = v.second.get<bool>("denoise_enabled", false);
        cfg.plugin_name =
            v.second.get<std::string>("denoise_plugin", "passthrough");
        cfg.dry_wet = v.second.get<float>("denoise_dry_wet", 1.0f);
        cfg.sensitivity = v.second.get<float>("sensitivity", 1.0f);
        uint8_t sink_id =
            static_cast<uint8_t>(v.second.get<unsigned>("sink_id"));
        add_sensor(sensor_id, sink_id, cfg);
        // review Minor #6：add_sensor 默认 enabled=true，若 JSON 中
        // enabled=false 需 enable_sensor 修正。enable_sensor 会触发
        // save_status（但 load_in_progress_ 置位 -> 跳过），与 add_sensor
        // 的 save_status 一样被 gate 拦截。load 结束后一次性持久化。
        bool enabled = v.second.get<bool>("enabled", true);
        if (!enabled)
          enable_sensor(sensor_id, false);
      }
    }
  } catch (const boost::property_tree::json_parser::json_parser_error& je) {
    std::cerr << "NoiseManager::load_status: JSON parse error at line "
              << je.line() << ": " << je.message() << std::endl;
    sensors_ok = false;
  } catch (const std::exception& e) {
    std::cerr << "NoiseManager::load_status: error: " << e.what() << std::endl;
    sensors_ok = false;
  }
  // 清除 load_in_progress_，然后一次性持久化最终状态（review Minor #7）。
  load_in_progress_.store(false, std::memory_order_relaxed);
  if (sensors_ok)
    save_status();
  return sensors_ok;
}

}  // namespace noise

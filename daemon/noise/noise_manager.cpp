// daemon/noise/noise_manager.cpp
// 架构依据：docs/noise/architecture-design.md §3.7。
#include "noise_manager.hpp"

#include <chrono>
#include <cmath>
#include <thread>
#include <utility>

namespace noise {

NoiseManager::NoiseManager(NoiseAudioBridge& bridge) : bridge_(bridge) {}

NoiseManager::~NoiseManager() {
  // 等待 housekeeper async 任务完成，避免 use-after-free（任务捕获 this）。
  if (reset_future_.valid()) {
    reset_future_.wait();
  }
}

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

  (*new_table)[sensor_id] = std::move(ctx);
  auto old = sensor_table_.publish(std::move(new_table));
  retire_queue_.retire(std::move(old), sensor_table_.epoch());
  // 控制线程回收（勿在 RT 路径调，reclaim_older_than 持 mutex）
  retire_queue_.reclaim_older_than(sensor_table_.epoch());
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
  return ok;
}

bool NoiseManager::remove_sensor(uint8_t sensor_id) {
  // 控制线程：COW 复制当前表 -> erase sensor -> publish -> retire 旧表。
  // 同 add_sensor 的 COW 模式（arch §3.7 L860 读路径无锁约束）。
  std::lock_guard<std::mutex> lock(ctrl_mutex_);
  const SensorTable* current = sensor_table_.load();
  if (current->find(sensor_id) == current->end())
    return false;  // 不存在
  auto new_table = std::make_shared<SensorTable>(*current);
  new_table->erase(sensor_id);
  auto old = sensor_table_.publish(std::move(new_table));
  retire_queue_.retire(std::move(old), sensor_table_.epoch());
  retire_queue_.reclaim_older_than(sensor_table_.epoch());
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
  return it->second.denoise->set_param(key, value);
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
  // 不直接调 plugin->reset()（会与 RT process() 竞态）。
  // 置位后由 housekeeper 延迟 reset（arch §3.7 L862）。
  ptp_locked_.store(false);
  reset_pending_.store(true);
  // Task 7 简化：仍用延迟清标志（真实 plugin->reset() + PcmCaptureService
  // join 在 Spec3 path A 实装）。#5: 重复 on_ptp_unlocked() 会阻塞 <=200ms
  // （旧 future 析构等待）。Task 7 改为独立 housekeeper 线程（Spec3）。
  reset_future_ = std::async(std::launch::async, [this]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    reset_pending_.store(false);
  });
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

}  // namespace noise

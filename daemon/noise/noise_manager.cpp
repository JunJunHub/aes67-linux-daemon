// daemon/noise/noise_manager.cpp
// 架构依据：docs/noise/architecture-design.md §3.7。
#include "noise_manager.hpp"

#include <algorithm>
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

// Spec4 T3：SSE metrics 事件需要 noise_type_to_string，声明在 noise_http.hpp。
// 不 include noise_http.hpp（避免 manager -> http 反向依赖），forward declare。
namespace noise {
std::string noise_type_to_string(NoiseType type);
}  // namespace noise

namespace noise {

NoiseManager::NoiseManager(NoiseAudioBridge& bridge) : bridge_(bridge) {
  // Spec3 T8b（C2 修复）：向 Bridge 注册 period 生命周期回调，使
  // PcmCaptureService provider 回调能驱动 on_period_begin/on_period_end
  // （每个 ALSA period 恰好一次，全局，非 per-sink）。
  // 此前 register_frame_provider 是 stub，生产 pipeline 永不运行 on_frame。
  bridge_.set_period_lifecycle_callbacks([this]() { on_period_begin(); },
                                         [this]() { on_period_end(); });
}

NoiseManager::~NoiseManager() {
  // Spec4 T5：析构时 join comparison 线程（R-S4.7）。
  // 不依赖 PTP 联动（on_capture_thread_joined 只在 PTP unlock 路径触发），
  // 析构是确定性的 shutdown 路径，必须保证线程退出。
  stop_comparison_thread();
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
  // Spec4 Task 3：为 sensor 创建 SSE broadcaster 实例（per-sensor）。
  // metrics/pcm_denoised/pcm_noise 各一。alert 用全局 alert_broadcaster_。
  // 持 sse_mutex_ 保护 sse_broadcasters_ map（与 push_sse_events 互斥）。
  {
    std::lock_guard<std::mutex> sse_lock(sse_mutex_);
    sse_broadcasters_[sensor_id] = SensorBroadcasters{
        std::make_shared<SseBroadcaster>(),  // metrics
        std::make_shared<SseBroadcaster>(),  // pcm_denoised
        std::make_shared<SseBroadcaster>()   // pcm_noise
    };
  }
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
  // Spec4 Task 3：移除该 sensor 的 SSE broadcaster 实例。
  // 已订阅的 SSE handler 仍持 shared_ptr<queue>，可 drain 残留事件后退出
  // （broadcaster 析构后 push 不再投递）。handler 的 releaser 调 unsubscribe
  // 时发现已不存在，no-op 返回 false。持 sse_mutex_ 保护 map。
  {
    std::lock_guard<std::mutex> sse_lock(sse_mutex_);
    sse_broadcasters_.erase(sensor_id);
  }
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
      break;
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

  // Spec4 T5：RefComparator 帧路由（arch §6.3.2 L1513）。
  // on_frame 末尾，ADDITIVE 不改 ①②③④。若 sink_id 是某 RefComparator 的
  // ref/cmp 源，调 write_ref/write_cmp（memcpy 快，不计 RT 重活，风险 9）。
  // 查找用 ref_routing_ map（sink_id -> {comparator_id, role}）。
  // 持 ref_mutex_（~0.5μs memcpy，与 NoiseMetrics::collect 同模式，不影响
  // RT 预算）。一个 sink 可同时是多个 comparator 的输入，遍历所有匹配路由。
  route_to_ref_comparators(sink_id, frames, frame_size);
}

void NoiseManager::on_period_end() {
  // 注意：pinned_table_ 在本方法末尾置空。SSE push 须在 advance_epoch 前
  // 访问 pinned_table_ 的 sensor 数据（metrics 快照 + DenoiseOutput front）。
  // push 在 advance_epoch 后也无妨（数据已 swap 到 front，仍可读），但
  // 当前实现：push 在 advance_epoch 前，使用 pinned_table_ 的 ctx.metrics
  // 快照 + ctx.denoise->get_output()（previous period front，刚被 swap）。
  if (pinned_table_ != nullptr) {
    for (auto& [id, ctx] : *pinned_table_) {
      (void)id;
      if (ctx.denoise)
        ctx.denoise->on_period_end();
      if (ctx.metrics)
        ctx.metrics->on_period_end();
    }
    // Spec4 Task 3：SSE push（D-S4.1，非阻塞）。
    // 在 denoise/metrics on_period_end（swap front/back + collect 完成）后，
    // advance_epoch 前调用 push_sse_events。此时 front 缓冲已是本 period
    // 数据（swap 后），metrics latest_ 已更新（collect 写入）。
    push_sse_events(*pinned_table_);
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
  // Spec4 T5（R-S4.7）：暂停 comparison 线程（capture 静止后不再访问 ring
  // buffer，避免与 capture 线程竞争）。真实 join 由 on_capture_thread_joined
  // 触发。仅置 running=false，不 join（避免在 PTP 回调线程中阻塞）。
  comparison_running_.store(false, std::memory_order_relaxed);
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
  // Spec4 T5（R-S4.7）：恢复 comparison 线程（若有 comparator 注册）。
  // on_capture_thread_joined 已 stop_comparison_thread（join 线程），
  // 此处需 restart 若有 comparator。start_comparison_thread 检查 joinable，
  // 若线程仍在运行（未被 stop）则 no-op；若已 stop 则重启。
  // 必须持 ref_mutex_ 保护 ref_comparators_ 访问（与 HTTP 控制线程的
  // add/remove_ref_comparator 并发）。但 start_comparison_thread 在 ref_mutex_
  // 外调用：若在锁内调 start（持 ref_mutex_ 等
  // comparison_thread_mutex_），与 stop（持 comparison_thread_mutex_ 等
  // join -> loop 需 ref_mutex_ 退出）形成 3 方死锁。锁外调 start 使两锁不嵌套。
  bool has_comparators = false;
  {
    std::lock_guard<std::mutex> lock(ref_mutex_);
    has_comparators = !ref_comparators_.empty();
  }
  if (has_comparators) {
    start_comparison_thread();
    comparison_running_.store(true, std::memory_order_relaxed);
  }
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
  // Spec4 T5（R-S4.7）：capture 线程已静止，join comparison 线程（若有）。
  // comparison 线程访问 ring buffer，capture 线程静止后无并发写入，
  // 但 comparison 线程可能仍在 try_process 中读 ring。join 确保它退出。
  // 注意：这里 stop 会让线程退出，on_ptp_locked 时需要 restart。
  // 但 PTP unlock -> on_capture_thread_joined 是 shutdown 路径的前置，
  // 若随后 on_ptp_locked 恢复，start_comparison_thread 会重启线程。
  stop_comparison_thread();
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
  // Spec4 T1（D-S4.7）：序列化持久化写路径，防并发 save_status 竞争同一
  // tmp 文件（write_atomic 写 path+".tmp"）。lock order: ctrl_mutex_ ->
  // save_mutex_（add_sensor 持 ctrl_mutex_ 后调 save_status），save_status
  // 单独持 save_mutex_ 无死锁。
  std::lock_guard<std::mutex> lk(save_mutex_);
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
  bool parse_ok = true;
  try {
    boost::property_tree::ptree pt;
    std::ifstream in(noise_status_file, std::ios::binary);
    if (!in.is_open()) {
      std::cerr << "NoiseManager::load_status: cannot open "
                << noise_status_file << std::endl;
      // I/O 错误（文件存在但无法打开）非 parse 失败，返回 false。
      load_in_progress_.store(false, std::memory_order_relaxed);
      return false;
    }
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
      uint8_t sink_id = static_cast<uint8_t>(v.second.get<unsigned>("sink_id"));
      add_sensor(sensor_id, sink_id, cfg);
      // review Minor #6：add_sensor 默认 enabled=true，若 JSON 中
      // enabled=false 需 enable_sensor 修正。enable_sensor 会触发
      // save_status（但 load_in_progress_ 置位 -> 跳过），与 add_sensor
      // 的 save_status 一样被 gate 拦截。load 结束后一次性持久化。
      bool enabled = v.second.get<bool>("enabled", true);
      if (!enabled)
        enable_sensor(sensor_id, false);
    }
  } catch (const boost::property_tree::json_parser::json_parser_error& je) {
    std::cerr << "NoiseManager::load_status: JSON parse error at line "
              << je.line() << ": " << je.message()
              << " - 降级为空配置，不阻塞 daemon 启动" << std::endl;
    parse_ok = false;
  } catch (const std::exception& e) {
    std::cerr << "NoiseManager::load_status: error: " << e.what()
              << " - 降级为空配置，不阻塞 daemon 启动" << std::endl;
    parse_ok = false;
  }
  // D-S4.7：parse 失败后清空已加载的中间 sensors（若 parse 中途部分
  // add_sensor 了）。add_sensor 逐条发布 COW 表，中途失败时前几条 sensor
  // 已在表中。用 remove_sensor 逐条回滚（复用 COW + unregister_frame_provider
  // + retire 逻辑）。save_status 被 load_in_progress_ gate 拦截（仍置位）。
  if (!parse_ok) {
    const SensorTable* current = sensor_table_.load();
    if (current != nullptr) {
      std::vector<uint8_t> ids;
      ids.reserve(current->size());
      for (const auto& [id, ctx] : *current)
        ids.push_back(id);
      for (uint8_t id : ids)
        remove_sensor(id);
    }
  }
  // 清除 load_in_progress_，然后一次性持久化最终状态（review Minor #7）。
  load_in_progress_.store(false, std::memory_order_relaxed);
  if (parse_ok)
    save_status();
  // D-S4.7: parse 失败时降级为空配置，返回 true 不阻塞 daemon 启动。
  // 文件不存在（首次启动）已在上文 return false。I/O 错误（cannot open）
  // 已在上文 return false。
  return true;
}

// ── Spec4 T5：RefComparator 参考音比对实现（arch §3.5 + §6.3.2）──────
// 控制线程 API + comparison 线程。算法在 ref_comparator.cpp。

uint8_t NoiseManager::add_ref_comparator(uint8_t ref_sink_id,
                                         uint8_t cmp_sink_id) {
  // D-S4.8：additive 新增方法。控制线程调用。
  if (ref_sink_id == cmp_sink_id)
    return 0;  // ref == cmp 无意义
  uint8_t id = 0;
  {
    std::lock_guard<std::mutex> lock(ref_mutex_);
    // 创建 comparator 实例。
    id = next_comparator_id_++;
    if (next_comparator_id_ == 0)  // 1-based 溢出回绕（理论极限 255）
      next_comparator_id_ = 1;
    RefComparatorEntry entry;
    entry.id = id;
    entry.comparator =
        std::make_shared<RefComparator>(ref_sink_id, cmp_sink_id);
    ref_comparators_.push_back(std::move(entry));
    // 注册路由：ref_sink -> {id, role=0}，cmp_sink -> {id, role=1}。
    ref_routing_[ref_sink_id].push_back({id, 0});
    ref_routing_[cmp_sink_id].push_back({id, 1});
  }
  // Lazy-start comparison 线程（首个 comparator 时）。在 ref_mutex_ 外调用
  // start_comparison_thread（同 on_ptp_locked，避免 3 方死锁）。
  start_comparison_thread();
  return id;
}

bool NoiseManager::remove_ref_comparator(uint8_t comparator_id) {
  std::lock_guard<std::mutex> lock(ref_mutex_);
  auto it = std::find_if(ref_comparators_.begin(), ref_comparators_.end(),
                         [comparator_id](const RefComparatorEntry& e) {
                           return e.id == comparator_id;
                         });
  if (it == ref_comparators_.end())
    return false;
  // 注销路由：从 ref_routing_ 中移除指向此 comparator 的条目。
  for (auto& [sink_id, routes] : ref_routing_) {
    routes.erase(std::remove_if(routes.begin(), routes.end(),
                                [comparator_id](const RefRoute& r) {
                                  return r.comparator_id == comparator_id;
                                }),
                 routes.end());
  }
  // 清理空 routes 的 sink 条目。
  for (auto rit = ref_routing_.begin(); rit != ref_routing_.end();) {
    if (rit->second.empty())
      rit = ref_routing_.erase(rit);
    else
      ++rit;
  }
  ref_comparators_.erase(it);
  // 若无 comparator 残留，comparison 线程在下个 loop 检测到空表后退出。
  // 不主动 stop（避免在 remove 路径中 join 线程的复杂性；stop 在析构/
  // on_capture_thread_joined 时统一处理）。
  return true;
}

std::vector<NoiseManager::RefComparatorInfo>
NoiseManager::list_ref_comparators() const {
  std::vector<RefComparatorInfo> out;
  std::lock_guard<std::mutex> lock(ref_mutex_);
  out.reserve(ref_comparators_.size());
  for (const auto& e : ref_comparators_) {
    RefComparatorInfo info;
    info.id = e.id;
    if (e.comparator) {
      info.ref_sink_id = e.comparator->ref_sink_id();
      info.cmp_sink_id = e.comparator->cmp_sink_id();
      info.delay_anomaly = e.comparator->delay_anomaly();
    }
    out.push_back(info);
  }
  return out;
}

bool NoiseManager::get_ref_result_for_test(uint8_t comparator_id,
                                           RefCompareResult& out) const {
  std::lock_guard<std::mutex> lock(ref_mutex_);
  auto it = std::find_if(ref_comparators_.begin(), ref_comparators_.end(),
                         [comparator_id](const RefComparatorEntry& e) {
                           return e.id == comparator_id;
                         });
  if (it == ref_comparators_.end())
    return false;
  out = it->last_result;
  return true;
}

bool NoiseManager::get_ref_overflow_for_test(uint8_t comparator_id,
                                             size_t& ref_overflow,
                                             size_t& cmp_overflow) const {
  std::lock_guard<std::mutex> lock(ref_mutex_);
  auto it = std::find_if(ref_comparators_.begin(), ref_comparators_.end(),
                         [comparator_id](const RefComparatorEntry& e) {
                           return e.id == comparator_id;
                         });
  if (it == ref_comparators_.end() || !it->comparator)
    return false;
  ref_overflow = it->comparator->ref_overflow_count();
  cmp_overflow = it->comparator->cmp_overflow_count();
  return true;
}

bool NoiseManager::wait_comparison_done_for_test(uint32_t timeout_ms) {
  // 测试钩子：等待 comparison 线程完成一次处理。
  comparison_done_.store(false, std::memory_order_relaxed);
  trigger_comparison_for_test();
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (comparison_done_.load(std::memory_order_relaxed))
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

void NoiseManager::route_to_ref_comparators(uint8_t sink_id,
                                            const float* frames,
                                            size_t frame_size) {
  // on_frame 末尾调用（arch §6.3.2 L1513）。ADDITIVE：不改 ①②③④。
  // 持 ref_mutex_ 查路由表 + 调 comparator->write_ref/write_cmp（memcpy 快）。
  // mutex 持有时间：map lookup (~0.1μs) + memcpy (~0.5μs for 480 floats)，
  // 与 NoiseMetrics::collect 持 metrics_mutex_ 同模式，不影响 RT 预算。
  std::lock_guard<std::mutex> lock(ref_mutex_);
  auto it = ref_routing_.find(sink_id);
  if (it == ref_routing_.end())
    return;
  for (const auto& route : it->second) {
    // 查 comparator 实例。
    auto cit = std::find_if(ref_comparators_.begin(), ref_comparators_.end(),
                            [&route](const RefComparatorEntry& e) {
                              return e.id == route.comparator_id;
                            });
    if (cit == ref_comparators_.end() || !cit->comparator)
      continue;
    if (route.role == 0) {
      cit->comparator->write_ref(frames, frame_size);
    } else {
      cit->comparator->write_cmp(frames, frame_size);
    }
  }
}

void NoiseManager::comparison_loop() {
  // SCHED_OTHER（非 RT，D-S4.3）。~每 100ms 轮询，遍历 ref_comparators 调
  // try_process()，结果写 metrics->set_ref_result() + last_result。
  // PTP 联动（R-S4.7）：comparison_running_=false 时跳过处理（不访问 ring，
  // 避免 capture 静止后竞争），但线程仍在 loop（等待 running=true 恢复）。
  while (!comparison_stop_.load(std::memory_order_relaxed)) {
    bool triggered =
        comparison_trigger_.exchange(false, std::memory_order_relaxed);
    if (comparison_running_.load(std::memory_order_relaxed)) {
      std::vector<std::pair<std::shared_ptr<RefComparator>, uint8_t>> snapshots;
      uint8_t cmp_sink_to_metric = 0;
      // 在锁内拷贝 comparator 列表 + 关联的 sink_id（用于写 metrics）。
      // 锁外调 try_process（避免长时间持锁，NLMS 计算可能 ~ms 级）。
      {
        std::lock_guard<std::mutex> lock(ref_mutex_);
        snapshots.reserve(ref_comparators_.size());
        for (const auto& e : ref_comparators_) {
          if (e.comparator) {
            snapshots.emplace_back(e.comparator, e.id);
          }
        }
      }
      // 锁外处理每个 comparator。
      // results 收集后在锁内写回 last_result。
      struct ResultEntry {
        uint8_t id;
        RefCompareResult result;
      };
      std::vector<ResultEntry> results;
      for (auto& [comp, id] : snapshots) {
        auto r = comp->try_process();
        if (r.has_value()) {
          results.push_back({id, r.value()});
        }
      }
      // 锁内写回 last_result + 写 metrics（cmp_sink 的 NoiseMetrics）。
      if (!results.empty()) {
        std::lock_guard<std::mutex> lock(ref_mutex_);
        for (const auto& r : results) {
          auto it = std::find_if(
              ref_comparators_.begin(), ref_comparators_.end(),
              [&r](const RefComparatorEntry& e) { return e.id == r.id; });
          if (it != ref_comparators_.end()) {
            it->last_result = r.result;
            // 写 cmp_sink 的 metrics（备链路是比对结果的归属 sink）。
            if (it->comparator) {
              cmp_sink_to_metric = it->comparator->cmp_sink_id();
              const SensorTable* tbl = sensor_table_.load();
              if (tbl != nullptr) {
                for (const auto& [sid, ctx] : *tbl) {
                  (void)sid;
                  if (ctx.sink_id == cmp_sink_to_metric && ctx.metrics) {
                    ctx.metrics->set_ref_result(r.result.similarity,
                                                r.result.noise_db,
                                                r.result.delay_ms);
                    break;
                  }
                }
              }
            }
          }
        }
      }
      comparison_done_.store(true, std::memory_order_relaxed);
    }
    if (!triggered) {
      // 非触发模式：100ms 轮询间隔。
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void NoiseManager::start_comparison_thread() {
  // 调用方不再持有 ref_mutex_（on_ptp_locked / add_ref_comparator 均在释放
  // ref_mutex_ 后调用本方法）。仅持 comparison_thread_mutex_ 保护
  // comparison_thread_ 的 joinable/assign。与 ref_mutex_ 不嵌套 -> 无死锁。
  std::lock_guard<std::mutex> lock(comparison_thread_mutex_);
  if (comparison_thread_.joinable())
    return;
  comparison_stop_.store(false, std::memory_order_relaxed);
  // 若 PTP 已锁，立即启用处理；否则线程 loop 等待 running=true。
  if (ptp_locked_.load(std::memory_order_relaxed)) {
    comparison_running_.store(true, std::memory_order_relaxed);
  }
  comparison_thread_ = std::thread([this]() { comparison_loop(); });
}

void NoiseManager::stop_comparison_thread() {
  // comparison_stop_ / comparison_running_ 是 atomic，先置位让 loop 退出。
  // 再持 comparison_thread_mutex_ 做 join。不持 ref_mutex_ -> loop 可获取
  // ref_mutex_ 退出 -> join 返回 -> 无死锁。
  comparison_stop_.store(true, std::memory_order_relaxed);
  comparison_running_.store(false, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(comparison_thread_mutex_);
  if (comparison_thread_.joinable()) {
    comparison_thread_.join();
  }
}

// ── Spec4 Task 3：SSE broadcaster 访问器 + on_period_end push ──────────
// 架构依据：docs/superpowers/specs/noise-spec4-design.md D-S4.1 +
//   docs/noise/architecture-design.md §5.1（SSE 端点）+ §11 风险 9/17。
//
// RT 非阻塞设计（风险 9）：
// - push_sse_events 在 on_period_end（capture 线程）调用。
// - SseBroadcaster::push 内部：持 subscribers_mutex_ 拷贝 shared_ptr 列表
//   （短临界区），锁外遍历各队列 try_push（mutex try_lock，满则 drop oldest）。
// - metrics JSON 组装：手工拼接（与 metrics_to_json 同模式），无堆分配
//   （stringstream 局部变量，编译器可能 RVO）。
// - PCM base64 编码：每 period 一次，帧大小 480 样本 * 2 bytes = 960 bytes
//   -> base64 ~1280 bytes。编码耗时 ~μs 级（查表），可接受（不超 RT 预算）。
//   若未来帧变大或路数增多，可将 base64 移到 SSE handler 线程（push 原始
//   PCM bytes，handler 编码）。
//
// metrics push 节拍（D-S4.5）：复用 kHistorySampleIntervalFrames（~1s）。
// 每 N 次 on_period_end push 一次 metrics 快照（避免每 period 都 push）。

namespace {

// Base64 编码表（RFC 4648）。
constexpr const char* kBase64Table =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// base64 编码（PCM bytes -> SSE base64 chunk）。
// 输入：原始 bytes（const char* + len）。输出：base64 字符串。
// 用于 PCM SSE：denoised/noise float 样本 -> S16 -> base64。
// 每 3 字节 -> 4 字符，末尾 padding '='。
std::string base64_encode(const char* data, size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  while (i + 2 < len) {
    uint32_t v = (static_cast<uint8_t>(data[i]) << 16) |
                 (static_cast<uint8_t>(data[i + 1]) << 8) |
                 static_cast<uint8_t>(data[i + 2]);
    out.push_back(kBase64Table[(v >> 18) & 0x3f]);
    out.push_back(kBase64Table[(v >> 12) & 0x3f]);
    out.push_back(kBase64Table[(v >> 6) & 0x3f]);
    out.push_back(kBase64Table[v & 0x3f]);
    i += 3;
  }
  if (i < len) {
    uint32_t v = static_cast<uint8_t>(data[i]) << 16;
    if (i + 1 < len)
      v |= static_cast<uint8_t>(data[i + 1]) << 8;
    out.push_back(kBase64Table[(v >> 18) & 0x3f]);
    out.push_back(kBase64Table[(v >> 12) & 0x3f]);
    out.push_back(i + 1 < len ? kBase64Table[(v >> 6) & 0x3f] : '=');
    out.push_back('=');
  }
  return out;
}

// float 样本 -> S16 LE bytes（与 Streamer::encode_denoise_pcm 同转换）。
// 输入：const float* + count。输出：std::string（S16 LE bytes）。
// 用于 PCM SSE：denoised/noise 路的 float PCM -> S16 -> base64。
std::string float_to_s16_le_bytes(const float* samples, size_t count) {
  std::string bytes;
  bytes.reserve(count * 2);
  for (size_t i = 0; i < count; ++i) {
    float s = samples[i];
    // clamp [-1.0, 1.0]
    if (s > 1.0f)
      s = 1.0f;
    else if (s < -1.0f)
      s = -1.0f;
    int16_t s16 = static_cast<int16_t>(s * 32767.0f);
    bytes.push_back(static_cast<char>(s16 & 0xff));
    bytes.push_back(static_cast<char>((s16 >> 8) & 0xff));
  }
  return bytes;
}

}  // namespace

std::shared_ptr<SseBroadcaster> NoiseManager::get_metrics_broadcaster(
    uint8_t sensor_id) {
  std::lock_guard<std::mutex> lock(sse_mutex_);
  auto it = sse_broadcasters_.find(sensor_id);
  if (it == sse_broadcasters_.end())
    return nullptr;
  return it->second.metrics;
}

std::shared_ptr<SseBroadcaster> NoiseManager::get_pcm_broadcaster(
    uint8_t sensor_id,
    bool denoised) {
  std::lock_guard<std::mutex> lock(sse_mutex_);
  auto it = sse_broadcasters_.find(sensor_id);
  if (it == sse_broadcasters_.end())
    return nullptr;
  return denoised ? it->second.pcm_denoised : it->second.pcm_noise;
}

size_t NoiseManager::metrics_broadcaster_count_for_test(
    uint8_t sensor_id) const {
  std::lock_guard<std::mutex> lock(sse_mutex_);
  auto it = sse_broadcasters_.find(sensor_id);
  if (it == sse_broadcasters_.end())
    return 0;
  return it->second.metrics ? it->second.metrics->subscriber_count() : 0;
}

size_t NoiseManager::pcm_broadcaster_dropped_for_test(uint8_t sensor_id,
                                                      bool denoised) const {
  std::lock_guard<std::mutex> lock(sse_mutex_);
  auto it = sse_broadcasters_.find(sensor_id);
  if (it == sse_broadcasters_.end())
    return 0;
  auto* bc =
      denoised ? it->second.pcm_denoised.get() : it->second.pcm_noise.get();
  return bc ? bc->total_dropped() : 0;
}

void NoiseManager::push_sse_events(const SensorTable& table) {
  // 遍历 sensor 表，push metrics + PCM 到对应 broadcaster。
  // metrics 节拍：每 period push 一次（~128ms/event）。
  // D-S4.5 原设计 ~1s/event（复用 kHistorySampleIntervalFrames=100），但
  // on_period_end 每 ALSA period（~128ms）调用一次，100 periods = 12.8s 太慢。
  // 改为每 period push（128ms 间隔），SSE handler drain + UI 轮询可接受。
  // PCM 节拍：每 period push 一次。

  // 持 sse_mutex_ 快照 broadcaster 指针列表（短临界区：map lookup +
  // 指针拷贝）， 锁外 push（push 内部各队列 try_lock 非阻塞，不持
  // sse_mutex_）。 与 route_to_ref_comparators 持 ref_mutex_ 同模式（风险
  // 9：短临界区 mutex 不影响 RT 预算，重活在锁外）。
  struct BcEntry {
    uint8_t sensor_id;
    std::shared_ptr<SseBroadcaster> metrics;
    std::shared_ptr<SseBroadcaster> pcm_denoised;
    std::shared_ptr<SseBroadcaster> pcm_noise;
  };
  std::vector<BcEntry> bc_list;
  {
    std::lock_guard<std::mutex> lock(sse_mutex_);
    bc_list.reserve(sse_broadcasters_.size());
    for (const auto& [sid, bcs] : sse_broadcasters_) {
      bc_list.push_back({sid, bcs.metrics, bcs.pcm_denoised, bcs.pcm_noise});
    }
  }

  for (const auto& [sensor_id, ctx] : table) {
    // 查找该 sensor 的 broadcaster 快照。
    BcEntry* bc = nullptr;
    for (auto& e : bc_list) {
      if (e.sensor_id == sensor_id) {
        bc = &e;
        break;
      }
    }
    if (bc == nullptr)
      continue;

    // metrics push（每 period，~128ms 间隔）
    if (bc->metrics && ctx.metrics) {
      NoiseMetricsSnapshot snap = ctx.metrics->get_snapshot();
      // 组装 SSE 事件 JSON（与 metrics_to_json 同字段，外加 sensor_id）。
      // 手工拼接（reuse noise_http.cpp escape_json 模式，但此处字段值
      // 均为数字/枚举，无需转义）。
      std::ostringstream ss;
      ss << "data: {\"sensor_id\": " << static_cast<unsigned>(sensor_id)
         << ", \"noise_level_dbfs\": " << snap.noise_level_dbfs
         << ", \"noise_type\": \"" << noise_type_to_string(snap.noise_type)
         << "\", \"noise_type_confidence\": " << snap.noise_type_confidence
         << ", \"is_mixed\": " << (snap.is_mixed ? "true" : "false")
         << ", \"estimated_snr_db\": " << snap.estimated_snr_db
         << ", \"denoise_enabled\": "
         << (snap.denoise_enabled ? "true" : "false")
         << ", \"denoise_dry_wet\": " << snap.denoise_dry_wet
         << ", \"noise_reduction_db\": " << snap.noise_reduction_db
         << ", \"alert_threshold_dbfs\": " << snap.alert_threshold_dbfs
         << ", \"is_alerting\": " << (snap.is_alerting ? "true" : "false")
         << ", \"spectral_centroid_hz\": " << snap.spectral_centroid_hz
         << ", \"spectral_flatness\": " << snap.spectral_flatness
         << ", \"hum_strength_db\": " << snap.hum_strength_db
         << ", \"ref_similarity\": " << snap.ref_similarity
         << ", \"ref_noise_db\": " << snap.ref_noise_db
         << ", \"ref_delay_ms\": " << snap.ref_delay_ms << "}\n\n";
      bc->metrics->push(ss.str());
    }

    // PCM push（每 period）
    // denoise 关 -> 不 push PCM（与 /denoised /noise 路由 404 语义一致）。
    // get_output() 返回 previous period 的 front（on_period_end 已 swap）。
    if (ctx.denoise_enabled && ctx.denoise) {
      const DenoiseOutput* out = ctx.denoise->get_output();
      if (out != nullptr && out->frame_count > 0) {
        // denoised PCM
        if (bc->pcm_denoised && out->denoised != nullptr) {
          std::string s16 =
              float_to_s16_le_bytes(out->denoised, out->frame_count);
          std::string b64 = base64_encode(s16.data(), s16.size());
          std::string event = "data: {\"sensor_id\": " +
                              std::to_string(static_cast<unsigned>(sensor_id)) +
                              ", \"channel\": \"denoised\", \"frame_count\": " +
                              std::to_string(out->frame_count) +
                              ", \"pcm_base64\": \"" + b64 + "\"}\n\n";
          bc->pcm_denoised->push(event);
        }
        // noise PCM
        if (bc->pcm_noise && out->noise != nullptr) {
          std::string s16 = float_to_s16_le_bytes(out->noise, out->frame_count);
          std::string b64 = base64_encode(s16.data(), s16.size());
          std::string event = "data: {\"sensor_id\": " +
                              std::to_string(static_cast<unsigned>(sensor_id)) +
                              ", \"channel\": \"noise\", \"frame_count\": " +
                              std::to_string(out->frame_count) +
                              ", \"pcm_base64\": \"" + b64 + "\"}\n\n";
          bc->pcm_noise->push(event);
        }
      }
    }
  }
}

}  // namespace noise

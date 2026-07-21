// daemon/noise/noise_manager.hpp
// 架构依据：docs/noise/architecture-design.md §3.7。
// Spec2 1.4b：NoiseManager 骨架 - RcuPtr sensor_table + 帧路由 + PTP-unlock
// 联动。 Task 7 替换 stub 为真实 DenoiseProcessor/NoiseDetector/NoiseAnalyzer
// （①②③④ 链路接入，arch §3.7 L817 on_frame + §3.3.1 分析源选择）。
#ifndef NOISE_NOISE_MANAGER_HPP_
#define NOISE_NOISE_MANAGER_HPP_

#include <atomic>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "denoise_processor.hpp"
#include "noise_analyzer.hpp"
#include "noise_audio_bridge.hpp"
#include "noise_detector.hpp"
#include "noise_metrics.hpp"
#include "rcu_ptr.hpp"

namespace noise {

// 传感器降噪配置（arch §3.7 引用但未定义，此处补全）。
// Task 7 的 switch_plugin/set_dry_wet 等使用这些字段。
struct NoiseSensorConfig {
  bool denoise_enabled{false};
  std::string plugin_name{"passthrough"};
  float dry_wet{1.0f};
  float sensitivity{1.0f};
};

// Spec3 Task 3：HTTP 控制线程读取的 sensor 信息快照。
// 由 get_sensor_info() / list_sensor_infos() 填充，组合 sensor 配置（sink_id/
// enabled/denoise_enabled，从 SensorContext 读）+ 最新 metrics 快照（从
// NoiseMetrics::get_snapshot() 持锁读）。POD，拷贝安全。
struct SensorInfo {
  uint8_t id{0};
  uint8_t sink_id{0};
  bool enabled{true};
  bool denoise_enabled{false};
  NoiseMetricsSnapshot metrics{};
};

// per-sensor 处理上下文（arch §3.7 L842-848）。
// Task 7 完整版：聚合真实 detector/analyzer/denoise/metrics。
// Spec3 Task 2：④NoiseMetrics 替换 Spec2 stub，真聚合 ①②③ 链路结果。
struct SensorContext {
  uint8_t sink_id{0};
  std::shared_ptr<NoiseDetector> detector;
  std::shared_ptr<NoiseAnalyzer> analyzer;
  std::shared_ptr<DenoiseProcessor> denoise;
  std::shared_ptr<NoiseMetrics> metrics;  // ④ Spec3 Task 2 真聚合
  // last_analysis / frame_count 用 shared_ptr 包裹：
  // SensorTable 经 RcuPtr<const SensorTable> publish，add_sensor 时 COW 复制
  // 旧表 -> 加新 sensor -> publish。若 last_analysis/frame_count 是 direct
  // 成员，COW 浅拷贝后旧表/新表各持独立副本，on_frame 在 pinned 旧表上递增
  // 不会反映到新表（stub_call_count_for_test 读新表 -> 看不到增量）。
  // shared_ptr 使旧表/新表共享同一计数器/结果对象，与 detector/analyzer 等
  // shared_ptr 成员语义一致（arch §3.7 L842-848 论证）。
  // mutable: const SensorContext& 上可改（RcuPtr<const SensorTable> 约束）。
  mutable std::shared_ptr<NoiseAnalysisResult>
      last_analysis;  // for get_analysis_result_for_test
  // 分析源选择由 cfg.denoise_enabled 决定（arch §3.3.1）。
  bool denoise_enabled{false};
  // Spec3 Task 2：sensor 启用/禁用（enable_sensor 切换，§5.4 "enabled" 字段）。
  bool enabled{true};
  // #3 兼容：用于 stub_call_count_for_test（保留 Task 1 测试钩子名称，
  // Task 7 真实处理器无 StubProcessor.call_count，改用 on_frame 调用计数）。
  // atomic<size_t>: on_frame 写 (capture 线程) + stub_call_count_for_test 读
  // (控制线程) 跨线程，atomic 保证可见性且无数据竞争。
  mutable std::shared_ptr<std::atomic<size_t>> frame_count;
};

// 不可变 sensor 表：控制线程建新表原子换，RT 线程周期顶部 load 快照。
using SensorTable = std::map<uint8_t, SensorContext>;

class NoiseManager {
 public:
  explicit NoiseManager(NoiseAudioBridge& bridge);
  ~NoiseManager();

  // 传感器生命周期（控制线程调用）
  bool add_sensor(uint8_t sensor_id,
                  uint8_t sink_id,
                  const NoiseSensorConfig& cfg);
  // Spec3 Task 2：传感器删除/启用（arch §3.7 L805-808）。
  // COW 复制表 -> mutate -> publish -> retire 旧表（同 add_sensor 模式）。
  bool remove_sensor(uint8_t sensor_id);
  bool enable_sensor(uint8_t sensor_id, bool enabled);

  // 降噪配置路由到对应 sensor 的 processor（控制线程调用，arch §3.7
  // L810-812）。 Task 7 实现：lookup sensor -> denoise->switch_plugin(name) +
  // drain_retire （回收 retired PluginSlot，避免泄漏；drain_retire
  // 控制线程专用）。
  bool switch_plugin(uint8_t sensor_id, const std::string& name);
  // Spec3 Task 2：dry_wet/param 路由到 plugin 原子 setter（不 COW，同
  // switch_plugin 的参数变更先例 -- plugin 内部用 atomic 成员）。
  // 同时更新 metrics 的 denoise 状态快照（set_denoise_state）。
  bool set_dry_wet(uint8_t sensor_id, float dry_wet);
  bool set_param(uint8_t sensor_id,
                 const std::string& key,
                 const std::string& value);

  // 帧回调入口（Bridge 的 capture 线程调用，按 sink_id 路由到对应 sensor）
  // frames 为单通道连续帧（Bridge 已按 channel_map 解复用，channels 恒为 1）。
  // Task 7 ①②③④ 链路（Phase 1 单线程，arch §6.2）：
  //   ① denoise->process(frames, ..., &result) -> 写
  //   back_（original/denoised/noise） ② detector->process_frame(...) ->
  //   监测角色（denoise_enabled 时 VAD 主源
  //      为 RNNoise；否则 Detector VAD 为唯一源）
  //   ③ analyzer->analyze(NoisePCM 或 OriginalPCM, detection) -> 分类
  //      分析源选择（arch §3.3.1）：
  //        denoise_enabled=true  -> out->noise (NoisePCM = original-denoised)
  //        denoise_enabled=false -> frames (OriginalPCM)
  //   ④ metrics->collect(denoise_result, detection, ar, input_rms,
  //      denoised_rms) -- Spec3 Task 2 真聚合（替 Spec2 stub no-op）。
  //      input_rms = RMS(frames)，denoised_rms = RMS(out->denoised)。
  void on_frame(uint8_t sink_id, const float* frames, size_t frame_size);

  // ── period 生命周期钩子（capture 线程在 ALSA period 边界调用，BL2）──
  // on_period_begin: period 顶部 load sensor_table_ RcuPtr 快照 ->
  // pinned_table_， 并对所有 sensor 的 denoise/metrics 调 on_period_begin()。
  //   整 period 内 on_frame 复用 pinned_table_，不每帧原子操作。
  void on_period_begin();
  // on_period_end: period 结尾，对每个 sensor 的 denoise/metrics 调
  // on_period_end()（denoise swap front/back + advance_epoch），
  //   再 pinned_table_ = nullptr + sensor_table_.advance_epoch()。
  void on_period_end();

  // PTP 失锁联动：置 ptp_locked_=false + reset_pending_=true，
  // housekeeper 延迟 200ms 后清 reset_pending_。
  // Task 7 替换为真实 plugin->reset() + PcmCaptureService join（Spec3）。
  void on_ptp_unlocked();

  // 测试钩子（spec §D 接受此模式）
  size_t sensor_count_for_test() const;
  bool is_ptp_locked_for_test() const { return ptp_locked_.load(); }
  // #2: ptp_locked_ 默认 false（arch §3.7 L855 安全默认），测试显式置位模拟
  // PTP 锁定。生产环境由 bridge 回调管理（Spec3 wiring）。
  void set_ptp_locked_for_test(bool locked) { ptp_locked_.store(locked); }
  // #4: 观察 reset_pending_ 状态（on_ptp_unlocked 置位，housekeeper 200ms
  // 后清）。
  bool is_reset_pending_for_test() const { return reset_pending_.load(); }
  // #3: 读取指定 sensor 的 on_frame 调用次数。
  // Task 7 真实处理器无 StubProcessor.call_count，改用
  // SensorContext.frame_count （on_frame 内递增）。保留 Task 1
  // 测试钩子名称以兼容既有测试。
  size_t stub_call_count_for_test(uint8_t sensor_id) const;
  // Task 7 测试钩子：返回 sensor 最近一次 ③ 分析结果。未找到返回默认。
  NoiseAnalysisResult get_analysis_result_for_test(uint8_t sensor_id) const;
  // Spec3 Task 2 测试钩子：返回 sensor ④ metrics 最新快照/历史。
  // 未找到 sensor 返回默认 snapshot / 空 history。
  NoiseMetricsSnapshot get_metrics_for_test(uint8_t sensor_id) const;
  std::deque<NoiseMetricsSnapshot> get_history_for_test(
      uint8_t sensor_id) const;

  // ── Spec3 Task 3 同步读路径（HTTP 控制线程调用）──
  // 与 collect() (RT 写) 互斥：metrics->get_snapshot() 持 metrics_mutex_。
  // sensor_table_.load() 是 RCU 读，安全。控制线程写 (add_sensor 等) 走 COW
  // 不改旧表，读路径看到的 SensorContext 字段（sink_id/enabled 等）稳定。
  // get_sensor_info: 返回单个 sensor 的 SensorInfo（配置 + metrics 快照）。
  //   未找到 sensor 返回 false。
  // list_sensor_infos: 返回所有 sensor 的 SensorInfo 列表（GET
  // /api/noise/sensors）。 get_metrics_snapshot: 聚焦 accessor - 仅返回 metrics
  // 快照，不读 sensor 配置（GET /api/noise/sensor/:id/metrics，Spec3 Task3
  // review Minor #4）。未找到 sensor 返回 false。 get_history_snapshot:
  // 返回 history 拷贝（GET /api/noise/sensor/:id/history）。
  bool get_sensor_info(uint8_t sensor_id, SensorInfo& out) const;
  std::vector<std::pair<uint8_t, SensorInfo>> list_sensor_infos() const;
  bool get_metrics_snapshot(uint8_t sensor_id, NoiseMetricsSnapshot& out) const;
  std::vector<NoiseMetricsSnapshot> get_history_snapshot(
      uint8_t sensor_id) const;

 private:
  // 原子插槽 + 静止点回收。构造即 publish 空表，load() 永不为空（Spec1
  // 约束3）。
  RcuPtr<const SensorTable> sensor_table_{std::make_shared<SensorTable>()};
  RetireQueue<const SensorTable> retire_queue_;
  // period 顶部 load 的快照（裸指针），整 period 内复用，on_period_end 置空。
  const SensorTable* pinned_table_{nullptr};
  NoiseAudioBridge& bridge_;  // #6: held for Task 2-3 FrameProvider
                              // registration (Spec3 wiring)
  std::mutex
      ctrl_mutex_;  // 仅保护控制线程的建表/换表；帧回调走 RCU 读，绝不持此锁
  // arch §3.7 L855：安全默认 false（假设未锁，直到 PTP 回调确认锁定）。
  std::atomic<bool> ptp_locked_{false};
  std::atomic<bool> reset_pending_{false};
  std::future<void> reset_future_;  // 持有 housekeeper async 任务
};

}  // namespace noise

#endif  // NOISE_NOISE_MANAGER_HPP_

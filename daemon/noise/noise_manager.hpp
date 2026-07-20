// daemon/noise/noise_manager.hpp
// 架构依据：docs/noise/architecture-design.md §3.7。
// Spec2 1.4b：NoiseManager 骨架 - RcuPtr sensor_table + 帧路由 + PTP-unlock
// 联动。 Task 7 替换 stub 为真实 DenoiseProcessor/NoiseDetector/NoiseAnalyzer。
#ifndef NOISE_NOISE_MANAGER_HPP_
#define NOISE_NOISE_MANAGER_HPP_

#include <atomic>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "noise_audio_bridge.hpp"
#include "noise_metrics_stub.hpp"
#include "rcu_ptr.hpp"
#include "stub_processor.hpp"

namespace noise {

// 传感器降噪配置（arch §3.7 引用但未定义，此处补全）。
// Task 7 的 switch_plugin/set_dry_wet 等使用这些字段。
struct NoiseSensorConfig {
  bool denoise_enabled{false};
  std::string plugin_name{"passthrough"};
  float dry_wet{1.0f};
  float sensitivity{1.0f};
};

// per-sensor 处理上下文（1.4b stub 版本）。
// Task 7 替换为完整版（detector/analyzer/denoise/metrics）。
struct SensorContext {
  uint8_t sink_id{0};
  std::shared_ptr<StubProcessor> stub;
  std::shared_ptr<NoiseMetricsStub> metrics;
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

  // 帧回调入口（Bridge 的 capture 线程调用，按 sink_id 路由到对应 sensor）
  // frames 为单通道连续帧（Bridge 已按 channel_map 解复用，channels 恒为 1）。
  void on_frame(uint8_t sink_id, const float* frames, size_t frame_size);

  // ── period 生命周期钩子（capture 线程在 ALSA period 边界调用，BL2）──
  // on_period_begin: period 顶部 load sensor_table_ RcuPtr 快照 ->
  // pinned_table_，
  //   并对所有 sensor 的 stub/metrics 调 on_period_begin()。
  //   整 period 内 on_frame 复用 pinned_table_，不每帧原子操作。
  void on_period_begin();
  // on_period_end: period 结尾，对每个 sensor 的 stub/metrics 调
  // on_period_end()，
  //   再 pinned_table_ = nullptr + sensor_table_.advance_epoch()。
  void on_period_end();

  // PTP 失锁联动：置 ptp_locked_=false + reset_pending_=true，
  // housekeeper 延迟 200ms 后清 reset_pending_（stub 无插件可 reset）。
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
  // #3: 读取指定 sensor 的 stub process() 调用次数。Task 7 真实处理器无此钩子。
  size_t stub_call_count_for_test(uint8_t sensor_id) const;

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

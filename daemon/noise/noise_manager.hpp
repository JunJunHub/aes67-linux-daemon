// daemon/noise/noise_manager.hpp
// 架构依据：docs/noise/architecture-design.md §3.7。
// Spec2 1.4b：NoiseManager 骨架 - RcuPtr sensor_table + 帧路由 + PTP-unlock
// 联动。 Task 7 替换 stub 为真实 DenoiseProcessor/NoiseDetector/NoiseAnalyzer
// （①②③④ 链路接入，arch §3.7 L817 on_frame + §3.3.1 分析源选择）。
#ifndef NOISE_NOISE_MANAGER_HPP_
#define NOISE_NOISE_MANAGER_HPP_

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "denoise_processor.hpp"
#include "noise_analyzer.hpp"
#include "noise_audio_bridge.hpp"
#include "noise_detector.hpp"
#include "noise_metrics.hpp"
#include "rcu_ptr.hpp"
#include "ref_comparator.hpp"

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
  // Spec3 Task 4：持久化用的原始配置快照（arch §7.4）。
  // mutable: 控制线程（set_dry_wet/switch_plugin）可通过 const SensorContext&
  // 更新此字段（RCU 表发布后 const，mutable 允许改）。RT 线程不读此字段，
  // 无数据竞争。save_status() 读取此字段序列化到 noise_status.json。
  mutable NoiseSensorConfig cfg;
  // #3 兼容：用于 stub_call_count_for_test（保留 Task 1 测试钩子名称，
  // Task 7 真实处理器无 StubProcessor.call_count，改用 on_frame 调用计数）。
  // atomic<size_t>: on_frame 写 (capture 线程) + stub_call_count_for_test 读
  // (控制线程) 跨线程，atomic 保证可见性且无数据竞争。
  mutable std::shared_ptr<std::atomic<size_t>> frame_count;
  // Spec3 Task 7 path A：plugin->reset() 累计调用次数（控制线程写 +
  // plugin_reset_count_for_test 读，跨线程但同 atomic）。
  // shared_ptr 包裹理由同 frame_count：COW 表复制后旧/新表共享同一计数器，
  // test 读新表可见控制线程在 on_capture_thread_joined 的递增。
  mutable std::shared_ptr<std::atomic<size_t>> reset_count;
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

  // ── Spec4 T5：RefComparator 参考音比对（arch §3.5 + §6.3.2）──
  // add_ref_comparator：配置一对 (ref_sink, cmp_sink) 的参考比对。
  //   控制线程调用。创建 RefComparator 实例 + 注册路由（ref_sink/cmp_sink ->
  //   comparator_id），并 lazy-start comparison 线程（首个 comparator 时）。
  //   返回 comparator_id（1-based，0 表示失败：ref_sink == cmp_sink 或满）。
  uint8_t add_ref_comparator(uint8_t ref_sink_id, uint8_t cmp_sink_id);
  // remove_ref_comparator：注销 comparator + 路由。控制线程调用。
  //   若是最后一个 comparator，comparison 线程在下个循环检测到空表后退出。
  bool remove_ref_comparator(uint8_t comparator_id);
  // list_ref_comparators：返回所有已配置 comparator 的信息（HTTP GET 用）。
  struct RefComparatorInfo {
    uint8_t id{0};
    uint8_t ref_sink_id{0};
    uint8_t cmp_sink_id{0};
    bool delay_anomaly{false};
  };
  std::vector<RefComparatorInfo> list_ref_comparators() const;
  // 测试钩子：直接获取 comparator 的最新比对结果（不经 metrics 快照）。
  //   未找到返回 false。
  bool get_ref_result_for_test(uint8_t comparator_id,
                               RefCompareResult& out) const;
  // 测试钩子：获取 comparator 的溢出计数（ref/cmp 各一路）。
  bool get_ref_overflow_for_test(uint8_t comparator_id,
                                 size_t& ref_overflow,
                                 size_t& cmp_overflow) const;
  // 测试钩子：comparison 线程是否在运行。
  bool is_comparison_thread_running_for_test() const {
    return comparison_running_.load(std::memory_order_relaxed);
  }
  // 测试钩子：同步触发一次 comparison 线程的 try_process（绕过 100ms 轮询）。
  //   用于测试中确定性验证结果进 metrics 快照。非阻塞：仅置标志，
  //   comparison 线程下个循环检测到后处理。
  void trigger_comparison_for_test() { comparison_trigger_.store(true); }
  // 测试钩子：等待 comparison 线程完成一次处理（最多 timeout_ms）。
  bool wait_comparison_done_for_test(uint32_t timeout_ms = 2000);

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

  // PTP 失锁联动（arch §3.7 L862 path A，Spec3 Task 7 替 Spec2 std::async
  // stub）。 仅置 ptp_locked_=false + reset_pending_=true（均 atomic，无锁）。
  // 不直接调 plugin->reset()（会与 RT process() 竞态）。真实 reset 由
  // on_capture_thread_joined() 在 PcmCaptureService join capture 线程后调用。
  void on_ptp_unlocked();
  // Spec3 Task 6b（C1 修复）：on_ptp_unlocked 的对偶。置 ptp_locked_=true +
  // 清 reset_pending_。生产环境由 PcmCaptureService::on_ptp_status_change 经
  // ptp_status_forward_cb_ 转发 "locked" 调用（FAKE_DRIVER 下 fake "unlocked"
  // 被转译为 "locked"，§4.3）。 修复 C1：此前 ptp_locked_ 仅由 test hook 设置，
  // 生产 pipeline 永不运行。
  void on_ptp_locked();
  // path A gate：PcmCaptureService 在 PTP unlock 时 snd_pcm_drop+close+join
  // capture 线程后回调本方法（控制线程）。capture 线程已静止 -> 无 in-flight
  // process() -> 安全 per-sensor plugin->reset() + 清 reset_pending_。
  // reset_pending_=false 时 no-op（避免重复 reset）。#ifdef _USE_NOISE_
  // 外不可用。
  void on_capture_thread_joined();

  // ── Spec3 Task 6 Streamer 三路数据通路（arch §4.4）──
  // get_denoise_output(sink_id): 返回指定 sink 的 DenoiseProcessor front 缓冲
  //   （previous period 的 DenoiseOutput，与 DenoiseProcessor::get_output()
  //   同义）。Streamer / HTTP 线程调用：cross-thread 读 front 安全（RT 路径
  //   写 back_，on_period_end swap front/back，读者 acquire 与 swap release
  //   配对，arch §4.4 约束1）。 返回 nullptr 若：
  //   - 无 sensor 匹配 sink_id
  //   - sensor 的 denoise_enabled=false（denoise 关 -> HTTP /denoised 404）
  //   返回的 DenoiseOutput* 在下次 on_period_end swap 前有效（arch §11
  //   风险23）。
  const DenoiseOutput* get_denoise_output(uint8_t sink_id) const;

  // 测试钩子（spec §D 接受此模式）
  size_t sensor_count_for_test() const;
  bool is_ptp_locked_for_test() const { return ptp_locked_.load(); }
  // #2: ptp_locked_ 默认 false（arch §3.7 L855 安全默认），测试显式置位模拟
  // PTP 锁定。生产环境由 bridge 回调管理（Spec3 wiring）。
  void set_ptp_locked_for_test(bool locked) { ptp_locked_.store(locked); }
  // #4: 观察 reset_pending_ 状态（on_ptp_unlocked 置位，
  // on_capture_thread_joined 清）。
  bool is_reset_pending_for_test() const { return reset_pending_.load(); }
  // Spec3 Task 7 path A 测试钩子：模拟 PcmCaptureService join capture 线程后
  // 回调。生产环境由 PcmCaptureService::on_ptp_status_change("unlocked") 路径
  // 在 stop_capture() join 后调用 on_capture_thread_joined()。
  void on_capture_thread_joined_for_test() { on_capture_thread_joined(); }
  // Spec3 Task 7 path A 测试钩子：返回 sensor 的 plugin->reset() 累计调用次数。
  // 未找到 sensor 返回 0。
  size_t plugin_reset_count_for_test(uint8_t sensor_id) const;
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

  // ── Spec3 Task 4 持久化（arch §7.6）──
  // load_status(file)：启动时加载传感器配置。
  //   file 空字符串 -> no-op 返回 false。
  //   文件不存在 -> 返回 false（非错误，首次启动）。
  //   JSON 解析失败 -> 返回 false 并 stderr 告警（不抛异常）。
  //   成功 -> 逐条 add_sensor 重建 sensors 表，末尾一次性 save_status。
  //   返回 true 表示传感器加载成功。
  //
  //   模板持久化是调用方的职责（review Important #1）：
  //   NoiseManager 不持有 NoiseTemplateDB（T5 设计：db 作为独立参数传入
  //   register_noise_template_routes）。T6 wiring 负责：
  //     template_db->load(config.get_noise_template_dir());
  //     noise_manager->load_status(config.get_noise_status_file());
  bool load_status(const std::string& noise_status_file);
  // save_status()：序列化 sensors 表为 noise_status.json via write_atomic
  //   （arch §7.1 + §7.4）。status_file_ 空时 no-op 返回 false。
  //   load_in_progress_ 置位时 no-op（review Minor #7：load 期间跳过
  //   add_sensor 触发的中间态写入，末尾一次性持久化）。
  //   const: 仅读 sensor_table_ RCU 快照 + status_file_ 成员，不改状态。
  //   RT 安全：不持 ctrl_mutex_（RcuPtr::load 原子），不阻塞音频线程。
  bool save_status() const;
  // save_status_on_exit()：daemon shutdown 序列调用。
  //   review Minor #2：持 ctrl_mutex_ 防止与并发 HTTP 控制操作竞态
  //   （set_dry_wet 等可能改 ctx.cfg）。shutdown 无嵌套控制调用，无死锁。
  //   非 const：需持 ctrl_mutex_（mutex 非 mutable，const 方法无法加锁）。
  //   shutdown 序列在非 const 上下文调用，无需 const。
  bool save_status_on_exit();
  // 测试钩子（spec §D）：设置持久化文件路径。生产环境由 Config 注入。
  void set_status_file_for_test(const std::string& file) {
    status_file_ = file;
  }
  const std::string& get_status_file_for_test() const { return status_file_; }

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
  std::atomic<bool> reset_pending_{
      false};  // Spec3 Task 4：持久化文件路径（arch §7.6）。
  // 空字符串禁用 save_status（no-op）。由 Config::noise_status_file_ 注入
  // （T6 wiring）或 set_status_file_for_test 设置。
  // mutable: save_status() const 方法可读（不写，仅读路径）。
  // 控制线程写（set_status_file_for_test）与 save_status() 读之间无竞态
  // （前者仅在 init/test 调用，后者在控制线程调用）。
  mutable std::string status_file_;
  // Spec3 Task4 review Minor #7：load 进行中标志。
  // load_status 进入时置 true，退出时置 false。save_status() 检查此标志
  // 并早返回（跳过 add_sensor 触发的 N 次中间态写入），load 结束后
  // 一次性 save_status 持久化最终状态。避免 N×I/O + partial-state 写入。
  // mutable: load_status() 非只读（改此标志），save_status() const 读此标志。
  // atomic: load_status (控制线程写) 与 save_status (控制线程读，由
  // add_sensor 内部调用) 跨函数但同线程，atomic 保证可见性。
  mutable std::atomic<bool> load_in_progress_{false};
  // Spec4 T1（D-S4.7）：save_status 并发写安全 mutex。
  // 序列化持久化写路径，防止并发 save_status（control 线程"变更即写" +
  // 直接 save_status 调用）竞争同一 tmp 文件导致损坏。仅保护持久化写路径，
  // 不影响 RT。lock order: ctrl_mutex_ -> save_mutex_（add_sensor 持
  // ctrl_mutex_ 后调 save_status），save_status 单独持 save_mutex_ 无死锁。
  mutable std::mutex save_mutex_;

  // ── Spec4 T5：RefComparator 注册表 + comparison 线程 ──
  // comparator_id -> RefComparator 实例。控制线程（add/remove）写，
  // comparison 线程读。mutex 保护（低频控制平面，非 RT）。
  // 一个 sink 可同时是多个 comparator 的输入（如主链路作为多个备链路的
  // 参考），on_frame 按 sink_id 查路由表写入对应 ring buffer。
  struct RefComparatorEntry {
    uint8_t id{0};
    std::shared_ptr<RefComparator> comparator;
    RefCompareResult last_result{};
  };
  std::vector<RefComparatorEntry> ref_comparators_;
  mutable std::mutex ref_mutex_;
  // 路由表：sink_id -> {(comparator_id, role)}。on_frame 末尾查此表
  // 决定是否写 ref/cmp ring。role: 0=ref, 1=cmp。
  // 不用 RCU：ref_comparators 是低频控制平面对象（add/remove 罕见），
  // on_frame 末尾的 ring 写入持 ref_mutex_ 的时间极短（~0.5μs memcpy），
  // 与 NoiseMetrics::collect 持 metrics_mutex_ 同模式，不影响 RT 预算。
  struct RefRoute {
    uint8_t comparator_id{0};
    uint8_t role{0};  // 0=ref, 1=cmp
  };
  std::map<uint8_t, std::vector<RefRoute>> ref_routing_;
  uint8_t next_comparator_id_{1};  // 1-based，0 = 无效

  // comparison 线程（D-S4.3）：SCHED_OTHER，~每 100ms 轮询，遍历
  // ref_comparators 调 try_process()，结果写 metrics->set_ref_result()。
  // comparison_thread_mutex_ 仅保护 comparison_thread_ 的
  // joinable/assign/join，与 ref_mutex_ 完全不嵌套（任何线程永不同时持有两把
  // 锁）。调用方（on_ptp_locked / add_ref_comparator）在释放 ref_mutex_ 后才调
  // start_comparison_thread；stop_comparison_thread 仅持
  // comparison_thread_mutex_（不持 ref_mutex_）。这避免 3 方死锁：
  // start 持 ref_mutex_ 等 comparison_thread_mutex_、stop 持
  // comparison_thread_mutex_ 等 join、loop 等 ref_mutex_ 退出。
  std::thread comparison_thread_;
  std::mutex comparison_thread_mutex_;
  std::atomic<bool> comparison_running_{false};
  std::atomic<bool> comparison_stop_{false};
  // 测试钩子：置位触发 comparison 线程立即处理（绕过 100ms 轮询）。
  std::atomic<bool> comparison_trigger_{false};
  // 测试钩子：comparison 线程完成一次处理后置位（wait_comparison_done 用）。
  std::atomic<bool> comparison_done_{false};

  void comparison_loop();
  void start_comparison_thread();
  void stop_comparison_thread();
  // on_frame 末尾调用：查 ref_routing_ 表，将帧 memcpy 进对应 comparator 的
  // ref/cmp ring buffer。ADDITIVE，不改 ①②③④。
  void route_to_ref_comparators(uint8_t sink_id,
                                const float* frames,
                                size_t frame_size);
};

}  // namespace noise

#endif  // NOISE_NOISE_MANAGER_HPP_

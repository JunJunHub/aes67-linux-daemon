// daemon/noise/noise_metrics.hpp
// ④NoiseMetrics - 指标聚合。架构依据：docs/noise/architecture-design.md
// §3.6 NoiseMetrics - 指标聚合 + §5.4 响应示例字段。
// Spec3 Task 2：替换 Spec2 的 NoiseMetricsStub 占位，聚合 ①②③ 链路结果到
// per-sensor NoiseMetricsSnapshot + 60s history ring。
//
// 线程模型（arch §3.7 L860 帧回调线程安全）：
//   - collect() 在 RT 路径（on_frame capture 线程）调用，更新 latest_ 快照。
//     Spec3 Task 3：collect() 持 metrics_mutex_ 写 latest_ + history_，与 HTTP
//     读路径互斥。Phase 1 simple mutex - HTTP 读罕见（UI 手动轮询），非
//     contends。 Phase 3.6 改 seqlock 做 lock-free RT（arch §11 待决项）。
//     history_ 每 kHistorySampleIntervalFrames 帧 push 一次（~1s），deque
//     push 的少量分配可接受（非每帧）。
//   - set_denoise_state() 在控制线程调用（add_sensor / set_dry_wet），
//     用 atomic 写 denoise_enabled_ / denoise_dry_wet_bits_，collect() 用
//     atomic 读 -> 无数据竞争。
//   - get_snapshot() / get_history()：Spec3 Task 3 新增的同步读路径，HTTP 控制
//     线程调用。持 metrics_mutex_ 读 latest_/history_ 副本。
//   - snapshot_for_test() / get_history_for_test()：Spec2
//   单线程测试钩子，无锁。
//     保留以兼容 T2 既有单线程 unit tests。
#ifndef NOISE_NOISE_METRICS_HPP_
#define NOISE_NOISE_METRICS_HPP_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

#include "denoise_plugin.hpp"  // DenoiseResult
#include "noise_analyzer.hpp"  // NoiseAnalysisResult, NoiseType
#include "noise_detector.hpp"  // NoiseDetectionResult

namespace noise {

// 候选噪声类型快照（arch §5.4 noise_candidates 数组元素）。
// NoiseAnalysisResult.candidates 是 std::vector，这里用定长 array 避免
// RT 路径 per-call 堆分配（arch §3.3.6 最多 3 个候选）。
constexpr size_t kMaxNoiseCandidates = 3;
struct NoiseTypeCandidateSnapshot {
  NoiseType type{NoiseType::Unknown};
  float confidence{0.0f};
};

// 60s history ring（arch §3.6）。
constexpr size_t kMaxHistorySize = 60;
// 1s 采样间隔（@48kHz / 480 样本/帧 = 100 帧 = 1s）。
constexpr size_t kHistorySampleIntervalFrames = 100;

// Spec4 T4：告警级别（arch §3.6 规则表，三级 + None）。
// 数字有序：Critical > Warning > Info > None，便于比较取最高级。
enum class AlertLevel : uint8_t {
  None = 0,
  Info = 1,
  Warning = 2,
  Critical = 3,
};

// Spec4 T4：告警事件（arch §C 新增告警事件 JSON）。
// raise/clear 时 push 到 alert_history_ + alert_broadcaster_。
// raised_at_ms 用 NoiseMetrics::frame_counter_（与 snapshot.timestamp_ms
// 同源，相对帧计数，非墙钟）。
struct AlertEvent {
  uint8_t sensor_id{0};
  AlertLevel level{
      AlertLevel::None};  // 本次事件的级别（raise=new level, clear=None）
  std::string rule;       // 触发规则名（noise_level_dbfs/snr_db/
                          // hum_strength_db/ref_similarity）
  std::string message;       // 人可读描述
  uint64_t raised_at_ms{0};  // 帧计数时间戳
  bool is_active{true};      // true=raise, false=clear
};

// 告警历史 ring 容量（per-sensor in-memory，D-S4.2: Phase 2 不持久化）。
constexpr size_t kMaxAlertHistorySize = 100;

// per-sensor 指标快照（arch §3.6 + §5.4 响应字段）。
// 字段名按 §5.4 响应示例（noise_type / noise_type_confidence /
// denoise_dry_wet / alert_threshold_dbfs / is_alerting 等），
// 与 HTTP API JSON 字段对齐。
struct NoiseMetricsSnapshot {
  // 时间戳（collect 调用时的相对帧计数，非墙钟；供 /history 序列化）
  uint64_t timestamp_ms{0};

  // ② 检测结果（NoiseDetector）
  bool is_noisy{false};
  float noise_confidence{0.0f};  // 检测置信度（是否含噪声）
  float estimated_snr_db{0.0f};

  // ③ 分析结果（NoiseAnalyzer）
  NoiseType noise_type{NoiseType::Unknown};
  float noise_type_confidence{0.0f};  // 分类置信度（区别于 noise_confidence）
  bool is_mixed{false};
  // Spec5 T3（D-S5.8）：主结果来源层 "l1"|"l2"|"l3"（默认空=l1）。
  // 由 collect() 从 NoiseAnalysisResult.noise_type_source 拷入。source="l3"
  // 时权威类型为 l3_match_type（模板 label），noise_type 仍为 L1 的 Unknown。
  std::string noise_type_source;
  std::array<NoiseTypeCandidateSnapshot, kMaxNoiseCandidates>
      noise_candidates{};
  size_t noise_candidates_count{0};
  float noise_level_dbfs{-100.0f};
  float spectral_centroid_hz{0.0f};
  float spectral_flatness{0.0f};
  float hum_strength_db{-100.0f};

  // ① 降噪效果（DenoiseProcessor）
  bool denoise_enabled{false};
  float denoise_dry_wet{1.0f};
  float input_level_dbfs{-100.0f};
  float output_level_dbfs{-100.0f};
  float noise_reduction_db{0.0f};

  // Spec4 T5：参考比对结果（arch §3.6 L740-743 设计，Spec2/3 未实现）。
  // 未配置 RefComparator 时保持默认（T4 告警引擎据此跳过 ref 规则）。
  // 由 NoiseManager 的 comparison 线程经 metrics->set_ref_result() 写入
  // （低频 ~2s 一次，持 metrics_mutex_ 与 collect() 互斥）。
  float ref_similarity{0.0f};
  float ref_noise_db{-100.0f};
  float ref_delay_ms{0.0f};

  // 告警（D-S3.4：noise_level_dbfs > alert_threshold_dbfs
  // OR hum_strength_db > hum_alert_threshold_db）
  // Spec4 T4：升级为告警引擎评估结果（D-S4.2）。is_alerting bool 语义不变
  // （"是否告警中"），调用方无需改。引擎用 5 条规则 + 三级 + 去抖。
  float alert_threshold_dbfs{-30.0f};
  float hum_alert_threshold_db{-40.0f};
  // Spec4 T4 新增配置字段（additive，sensor 配置，D-S4.8）。
  // 默认值与 arch §3.6 规则表一致。序列化进 noise_status.json sensors 项。
  float snr_alert_threshold_db{10.0f};
  float ref_similarity_threshold{0.8f};
  uint32_t alert_debounce_periods{3};
  bool is_alerting{false};
  // Spec4 T4：当前告警级别（引擎评估结果）。None=不告警。
  // 与 is_alerting 语义对应（level != None <-> is_alerting=true）。
  AlertLevel alert_level{AlertLevel::None};
  // Spec5 T2：降噪插件已降级（ONNX 反复失败 -> 切 passthrough）。
  // 由 NoiseManager housekeeper（on_period_end）在切 passthrough 后置 true，
  // 在 switch_plugin 到非 passthrough 插件且该插件恢复 kOk 后清。告警引擎
  // 据此评估 plugin_degraded 规则（Warning）。
  bool plugin_degraded{false};
};

// ④NoiseMetrics - 聚合 ①②③ 链路结果到 NoiseMetricsSnapshot。
// per-sensor 实例，由 SensorContext 持有（shared_ptr<NoiseMetrics>）。
class NoiseMetrics {
 public:
  NoiseMetrics();

  // period 生命周期钩子（与 NoiseMetricsStub 接口对齐，Spec2 既有调用点保留）。
  void on_period_begin() {}
  void on_period_end() {}

  // ④ collect：聚合 ①②③ 链路结果。RT 路径（on_frame 调用）。
  //   denoise      = ① DenoiseResult（VAD 概率等）
  //   detection    = ② NoiseDetectionResult（is_noisy, SNR, SF）
  //   analysis     = ③ NoiseAnalysisResult（类型, 置信度, dBFS, hum）
  //   input_rms    = 原始 PCM RMS（frames 或 DenoiseOutput.original）
  //   denoised_rms = 降噪 PCM RMS（DenoiseOutput.denoised）
  // 每次 call 更新 latest_；每 kHistorySampleIntervalFrames 帧 push 一份到
  // history_ deque（~1s 间隔，capped 60 entries）。
  void collect(const DenoiseResult& denoise,
               const NoiseDetectionResult& detection,
               const NoiseAnalysisResult& analysis,
               float input_rms,
               float denoised_rms);

  // 控制线程：设置降噪状态（denoise_enabled / dry_wet）。
  // add_sensor / set_dry_wet 调用。atomic 写，collect() atomic 读 -> 无竞争。
  void set_denoise_state(bool enabled, float dry_wet);

  // Spec4 T5：comparison 线程写入参考比对结果到 latest_ 快照。
  // 持 metrics_mutex_ 与 collect() (RT 写) 互斥。低频调用（~2s 一次）。
  // 不写入 history_（history 由 collect() 在 RT 路径采样，避免 comparison
  // 线程对 deque 的并发 push）。下次 collect() 会把当前 ref_* 值带入
  // history 采样点。
  // set_ref_result 同时置 ref_configured_=true（首次调用后），T4 告警引擎
  // 据此判断是否评估 ref_similarity 规则（避免未配置时误报）。
  void set_ref_result(float similarity, float noise_db, float delay_ms);

  // Spec4 T4 review fix：控制线程清除 ref_configured_ + 重置 ref_* 字段。
  // remove_ref_comparator 在移除最后一个监控某 cmp_sink 的 comparator 后
  // 调用，使 ref 规则停止评估（避免 stale ref_similarity 卡死告警去抖）。
  // 持 metrics_mutex_ 与 set_ref_result / collect 互斥。
  void clear_ref_configured() {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    ref_configured_ = false;
    latest_.ref_similarity = 0.0f;
    latest_.ref_noise_db = -100.0f;
    latest_.ref_delay_ms = 0.0f;
  }

  // Spec4 T4：控制线程设置告警配置（add_sensor 时调用）。
  // 写入 latest_ 的 snr_alert_threshold_db / ref_similarity_threshold /
  // alert_debounce_periods 字段。持 metrics_mutex_ 与 collect() 互斥。
  void set_alert_config(float snr_threshold_db,
                        float ref_similarity_threshold,
                        uint32_t debounce_periods) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    latest_.snr_alert_threshold_db = snr_threshold_db;
    latest_.ref_similarity_threshold = ref_similarity_threshold;
    latest_.alert_debounce_periods = debounce_periods;
  }

  // Spec5 T2：控制线程置/清 plugin_degraded 标志（housekeeper 切 passthrough
  // 后置 true；switch_plugin 到非 passthrough 且恢复后清）。持 metrics_mutex_
  // 与 collect 互斥。告警引擎 evaluate_alerts 据此评估 plugin_degraded 规则。
  void set_plugin_degraded(bool degraded) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    latest_.plugin_degraded = degraded;
  }

  // ── Spec4 T4：告警规则引擎（D-S4.2 + arch §3.6 规则表）──
  // evaluate_alerts：在 on_period_end（collect 之后）调用，per-sensor 评估
  //   5 条规则，产出当前 AlertLevel + 去抖计数。轻量（比较 + 计数），无 socket
  //   I/O。返回 raise/clear 事件（若状态发生变化，供调用方 push 到
  //   alert_broadcaster_）。
  //   评估时机：on_period_end 内，collect() 写完 latest_ 后。持 metrics_mutex_
  //   读 latest_（与 HTTP 读路径互斥，但同 collect 持锁点一致 -> 无嵌套锁）。
  //   返回 std::optional<AlertEvent>：raise 事件（is_active=true）、clear 事件
  //   （is_active=false）或 nullopt（无状态变化）。
  //   sensor_id 参数：填入 AlertEvent.sensor_id（NoiseMetrics 不持有自己的
  //   sensor_id，由调用方传入）。
  std::optional<AlertEvent> evaluate_alerts(uint8_t sensor_id);

  // Spec4 T4：告警历史 ring 访问器（HTTP GET /api/noise/alerts 用）。
  // 返回拷贝（持 metrics_mutex_ 与 alert_history_ 互斥）。
  std::deque<AlertEvent> get_alert_history() const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    return alert_history_;
  }

  // Spec3 Task 3 同步读路径（HTTP 控制线程调用）。
  // 持 metrics_mutex_ 读 latest_ / history_ 副本，与 collect() 写互斥。
  // 返回值是拷贝，调用方在锁外使用。Phase 1 simple mutex（arch §11 待决项
  // 提到 Phase 3.6 改 seqlock）。
  NoiseMetricsSnapshot get_snapshot() const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    return latest_;
  }
  std::deque<NoiseMetricsSnapshot> get_history() const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    return history_;
  }

  // 测试钩子（spec §D 接受此模式）。
  // 返回 latest_ 副本。Spec2 单线程测试，collect 后读无竞态。无锁。
  // Spec3 Task 3 HTTP 控制线程读改用 get_snapshot()（持锁）。
  NoiseMetricsSnapshot snapshot_for_test() const { return latest_; }
  std::deque<NoiseMetricsSnapshot> get_history_for_test() const {
    return history_;
  }
  // 设置 history 采样间隔（测试加速，默认 kHistorySampleIntervalFrames=100）。
  // 仅测试用，须在首次 collect() 前设置。
  void set_history_sample_interval_for_test(size_t frames) {
    history_sample_interval_ = frames;
  }

 private:
  NoiseMetricsSnapshot latest_;
  std::deque<NoiseMetricsSnapshot> history_;
  size_t frame_counter_{0};
  size_t history_sample_interval_{kHistorySampleIntervalFrames};
  // 控制线程写 -> RT 线程读（atomic，lock-free）。
  std::atomic<bool> denoise_enabled_{false};
  // IEEE 754 float 位模式存 atomic<uint32_t>（与 IDenoisePlugin 同一做法，
  // std::atomic<float> 不保证 lock-free，arch §3.4 denoise_plugin.hpp 注释）。
  std::atomic<uint32_t> denoise_dry_wet_bits_;
  // Spec3 Task 3：guards latest_ + history_。RT 写 (collect) vs HTTP 读
  // (get_snapshot/get_history) 互斥。mutable: const 方法 (get_snapshot) 可锁。
  mutable std::mutex metrics_mutex_;

  // ── Spec4 T4：告警引擎状态（per-sensor，D-S4.2）──
  // 去抖计数器：连续满足某级的 period 数（raise）/ 连续不满足的 period 数
  // （clear）。达到 alert_debounce_periods 时才 raise/clear。
  // raise_count_：当前级别连续满足的 period 数（重置为 0 当级别变化）。
  // clear_count_：连续无告警的 period 数（仅当已告警时计数）。
  size_t alert_raise_count_{0};
  size_t alert_clear_count_{0};
  // 当前已 raise 的告警级别（None 表示未告警）。用于检测状态变化。
  AlertLevel raised_level_{AlertLevel::None};
  // Spec4 T5/T4：RefComparator 是否已配置并写入过 ref_* 字段。
  // set_ref_result 首次调用后置 true。clear_ref_configured() 重置为 false
  // （remove_ref_comparator 在无剩余 comparator 监控此 sink 时调用）。
  // T4 告警引擎据此判断是否评估 ref_similarity 规则。未配置时
  // ref_similarity 保持默认 0.0（避免误报：0.0 < 0.8 阈值会触发告警）。
  bool ref_configured_{false};
  // 告警历史 ring（per-sensor in-memory，D-S4.2: Phase 2 不持久化）。
  // evaluate_alerts 在 raise/clear 时 push 到此 deque（capped
  // kMaxAlertHistorySize）。HTTP GET /api/noise/alerts 查询。
  // 受 metrics_mutex_ 保护（与 latest_/history_ 同锁，evaluate_alerts 写 +
  // get_alert_history 读互斥）。
  std::deque<AlertEvent> alert_history_;
};

}  // namespace noise

#endif  // NOISE_NOISE_METRICS_HPP_

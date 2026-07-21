// daemon/noise/noise_metrics.hpp
// ④NoiseMetrics - 指标聚合。架构依据：docs/noise/architecture-design.md
// §3.6 NoiseMetrics - 指标聚合 + §5.4 响应示例字段。
// Spec3 Task 2：替换 Spec2 的 NoiseMetricsStub 占位，聚合 ①②③ 链路结果到
// per-sensor NoiseMetricsSnapshot + 60s history ring。
//
// 线程模型（arch §3.7 L860 帧回调线程安全）：
//   - collect() 在 RT 路径（on_frame capture 线程）调用，更新 latest_ 快照。
//     无锁写：latest_ 为 per-sensor 实例，单写者（capture 线程）。
//     history_ 每 kHistorySampleIntervalFrames 帧 push 一次（~1s），deque
//     push 的少量分配可接受（非每帧）。
//   - set_denoise_state() 在控制线程调用（add_sensor / set_dry_wet），
//     用 atomic 写 denoise_enabled_ / denoise_dry_wet_bits_，collect() 用
//     atomic 读 -> 无数据竞争。
//   - snapshot_for_test() / get_history_for_test()：测试钩子。单线程测试中
//     collect 后读，无竞态。Task 3 HTTP 控制线程读需加同步（本 task 不涉及）。
#ifndef NOISE_NOISE_METRICS_HPP_
#define NOISE_NOISE_METRICS_HPP_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>

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

  // 告警（D-S3.4：noise_level_dbfs > alert_threshold_dbfs
  // OR hum_strength_db > hum_alert_threshold_db）
  float alert_threshold_dbfs{-30.0f};
  float hum_alert_threshold_db{-40.0f};
  bool is_alerting{false};
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

  // 测试钩子（spec §D 接受此模式）。
  // 返回 latest_ 副本。测试为单线程（collect 后读，无竞态）。
  // Task 3 HTTP 控制线程读需加同步（本 task 不涉及）。
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
};

}  // namespace noise

#endif  // NOISE_NOISE_METRICS_HPP_

// daemon/noise/noise_metrics_stub.hpp
// ④NoiseMetrics stub 占位（Spec3 1.9 实装真聚合）。Spec2 仅占位使 SensorContext
// 完整。 collect() 签名与真实 NoiseMetrics 对齐（arch §3.6）：接收 ③ 分析结果
// ar、 ②检测结果 detection、①降噪结果 denoise_result，Spec3 在此基础上聚合
// 时间序列 + 触发告警 + SSE 推送。
#ifndef NOISE_NOISE_METRICS_STUB_HPP_
#define NOISE_NOISE_METRICS_STUB_HPP_

#include "denoise_plugin.hpp"  // DenoiseResult
#include "noise_analyzer.hpp"  // NoiseAnalysisResult
#include "noise_detector.hpp"  // NoiseDetectionResult

class NoiseMetricsStub {
 public:
  void on_period_begin() {}
  void on_period_end() {}
  // Spec2 stub：no-op。Spec3 1.9 实装真聚合（arch §3.6 NoiseMetricsSnapshot）。
  void collect(const noise::NoiseAnalysisResult& /*ar*/,
               const noise::NoiseDetectionResult& /*detection*/,
               const noise::DenoiseResult& /*denoise_result*/) {}
};

#endif  // NOISE_NOISE_METRICS_STUB_HPP_

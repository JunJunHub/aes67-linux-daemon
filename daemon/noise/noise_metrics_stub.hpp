// daemon/noise/noise_metrics_stub.hpp
// ④NoiseMetrics stub 占位（Spec3 1.9 实装真聚合）。Spec2 仅占位使 SensorContext
// 完整。
#ifndef NOISE_NOISE_METRICS_STUB_HPP_
#define NOISE_NOISE_METRICS_STUB_HPP_

class NoiseMetricsStub {
 public:
  void on_period_begin() {}
  void on_period_end() {}
  void collect(/* NoiseAnalysisResult, NoiseDetectionResult, DenoiseResult */) {
  }
};

#endif  // NOISE_NOISE_METRICS_STUB_HPP_

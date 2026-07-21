// daemon/noise/noise_analyzer.hpp
// 噪声特征分析:L1 规则式分类 + Bark 频带 + 逐帧 FrameFeatures 环形缓冲。
// 架构依据:docs/noise/architecture-design.md §3.3 L461-627。
// Spec2 1.7:NoiseType/AnalysisSource/NoiseTypeCandidate/NoiseAnalysisResult
// structs + NoiseAnalyzer 类签名逐字采用 §3.3.6 L546-599;
// FrameFeatures struct 采用 §3.3.7 L610-619。
#ifndef NOISE_NOISE_ANALYZER_HPP_
#define NOISE_NOISE_ANALYZER_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "noise_detector.hpp"  // NoiseDetectionResult

namespace noise {

// 噪声类型(arch §3.3.6 L548)。
enum class NoiseType {
  Clean,
  White,
  Pink,
  Hum50Hz,
  Hum60Hz,
  Impulse,
  Broadband,
  Digital,
  Unknown
};

// 分析输入源(arch §3.3.6 L551)。
enum class AnalysisSource { OriginalPCM, NoisePCM, ResidualPCM };

// 候选噪声类型 + 置信度(arch §3.3.6 L554-557)。
struct NoiseTypeCandidate {
  NoiseType type;    // 候选噪声类型
  float confidence;  // 置信度 [0, 1]
};

// 噪声分析结果(arch §3.3.6 L559-579)。
struct NoiseAnalysisResult {
  // 主结果(最高置信度候选)
  NoiseType primary_type;
  float primary_confidence;  // 主类型置信度 [0, 1]

  // top-N 候选(按置信度降序),用于混合噪声场景
  // 最多 3 个,仅包含 confidence > 0.1 的候选
  std::vector<NoiseTypeCandidate> candidates;

  // 是否为混合噪声(多个候选置信度接近)
  // 判定条件:candidates.size() >= 2 && candidates[1].confidence > 0.3
  bool is_mixed;

  // 量化指标
  float noise_level_dbfs;      // 噪声级 (dBFS)
  float spectral_centroid_hz;  // 频谱质心
  float spectral_flatness;     // 频谱平坦度 [0, 1],暴露给 UI
  float hum_strength_db;       // 工频哼声强度 (dB)
  float impulse_count;         // 脉冲计数/秒
  std::array<float, 32> band_energy;  // 1/3 倍频程能量(L2 模板匹配特征向量)
};

// 逐帧特征(arch §3.3.7 L610-619)。
// 每帧做完 FFT + 特征提取后,只将特征向量推入环形缓冲,不缓冲原始 PCM。
struct FrameFeatures {
  std::array<float, 32> bark_energy;  // L2 模板匹配输入
  float spectral_flatness;            // L1 白噪声/宽带判定
  float spectral_centroid_hz;         // 噪声"亮度"
  float noise_level_dbfs;             // 噪声级
  float hum_strength_db;              // 工频哼声强度
  float impulse_count;                // 脉冲计数
  NoiseType l1_type;                  // L1 主类型(enum class,int-sized)
  float l1_confidence;                // L1 主类型置信度
};

// 噪声分析器:L1 规则式频谱分析 + Bark 频带 + 逐帧特征环形缓冲。
// 架构依据:arch §3.3 L461-627。
class NoiseAnalyzer {
 public:
  NoiseAnalysisResult analyze(const float* frames,
                              size_t frame_size,
                              const NoiseDetectionResult& detection);
  void set_analysis_window_ms(uint32_t ms);  // 分析窗口(默认 2000ms)

 private:
  // L1: 规则式分类(各规则输出置信度)
  std::vector<NoiseTypeCandidate> classify_rule_based(
      const std::vector<float>& power_spectrum,
      size_t fft_n,
      float sample_rate,
      const float* frames,
      size_t frame_size,
      float spectral_flatness);

  // 逐帧特征环形缓冲(§3.3.7)
  std::vector<FrameFeatures> feature_ring_;
  size_t ring_head_{0};
  size_t ring_count_{0};
  uint32_t analysis_window_ms_{2000};
  static constexpr size_t kRingCapacity = 200;  // 2s @ 10ms/frame

  // 窗口聚合(§3.3.7 L625):加权平均 bark_energy -> L2 输入
  void aggregate_window();
};

}  // namespace noise

#endif  // NOISE_NOISE_ANALYZER_HPP_

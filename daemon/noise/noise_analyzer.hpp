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
#include <memory>
#include <string>
#include <vector>

#include "noise_detector.hpp"  // NoiseDetectionResult

namespace noise {

class MlClassifier;        // Spec5 T3：L3 ML 分类器（前向声明，.cpp 内 include 全定义）
class NoiseTemplateDB;     // Spec5 T3：L3 kNN 检索的模板库（前向声明）

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

  // Spec5 T3（D-S5.8）：L3 ML 分类结果。仅当 L1+L2 均未识别
  // （primary_confidence < 阈值）且 MlClassifier 可用时填充。
  // noise_type_source 标识主结果的来源层："l1"（L1 规则式，默认）
  // / "l2"（Bark 模板）/ "l3"（VGGish ML）。当 source="l3" 时，主类型权威值
  // 为 l3_match_type（模板 label），primary_type 仍为 L1 的 Unknown。
  std::string l3_match_type;      // L3 匹配到的模板 label（空=未触发/未匹配）
  float l3_similarity{0.0f};      // L3 余弦相似度 [-1, 1]
  std::string noise_type_source;  // 主结果来源层 "l1"|"l2"|"l3"（默认空=l1）
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

// Spec3 Task 5：一次性 Bark 频谱提取（无环形缓冲状态）。
// 供 add_template_from_wav 调用：从一段完整 PCM（WAV 文件）提取 32 维 Bark
// 频带能量。内部按 kFftSize(512) 分帧、逐帧 FFT + Bark 频带累加、跨帧平均。
// 与 NoiseAnalyzer::analyze() 共享同一 Bark 频带映射 + FFT 实现（DRY）。
//   pcm: 浮点 PCM 样本（int16 /32768 归一化）。
//   n: 样本数。
//   sample_rate: 采样率（Phase 1 仅支持 48000；其他返回全零数组）。
std::array<float, 32> compute_bark_spectrum(const float* pcm,
                                            size_t n,
                                            uint32_t sample_rate);

// 噪声分析器:L1 规则式频谱分析 + Bark 频带 + 逐帧特征环形缓冲。
// 架构依据:arch §3.3 L461-627。
class NoiseAnalyzer {
 public:
  NoiseAnalyzer();
  ~NoiseAnalyzer();  // out-of-line：ml_classifier_ shared_ptr<MlClassifier> 需完整类型
  NoiseAnalysisResult analyze(const float* frames,
                              size_t frame_size,
                              const NoiseDetectionResult& detection);
  void set_analysis_window_ms(uint32_t ms);  // 分析窗口(默认 2000ms)

  // Spec5 T3（D-S5.8）：注入 L3 ML 分类器。空 shared_ptr -> L3 跳过（L1+L2 不受
  // 影响，additive 向后兼容）。控制线程调用（add_sensor 后），由 NoiseManager
  // 转发同一 shared_ptr 给所有 sensor 的 analyzer 共享。
  void set_ml_classifier(std::shared_ptr<MlClassifier> ml);
  // Spec5 T3：注入模板库（L3 kNN 检索用）。空 -> L3 即使触发也无模板可检索。
  // DB 自带 recursive_mutex 保护 capture 线程读 vs HTTP 写。
  void set_template_db(std::shared_ptr<NoiseTemplateDB> db);
  // L3 触发阈值：primary_confidence < 阈值时调 L3。默认 0.5。
  void set_l3_confidence_threshold(float t) { l3_threshold_ = t; }

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

  // ── Spec5 T3：L3 ML 分类层（D-S5.8）──
  // L3 分类器（可选，空则跳过）。
  std::shared_ptr<MlClassifier> ml_classifier_;
  // 模板库（L3 kNN 检索用，可选，空则 L3 无模板可检索）。
  std::shared_ptr<NoiseTemplateDB> template_db_;
  float l3_threshold_{0.5f};  // L1+L2 未识别阈值（primary_confidence < 此值触发 L3）

  // L3 嵌入所需的 0.96s @48k PCM 环形缓冲。analyze() 每帧追加 analysis PCM，
  // 攒满 46080 样本后 L3 触发时取最新一窗送 MlClassifier::embed。
  // 单 capture 线程独占读写（per-sensor analyzer），无并发。
  static constexpr size_t kVggishWindowSamples = 46080;  // 0.96s @48k
  std::vector<float> pcm_ring_;
  size_t pcm_ring_head_{0};
  size_t pcm_ring_count_{0};
  // L3 节流：触发后冷却若干帧（~0.96s），避免持续未知噪声时每帧都跑 ONNX。
  size_t l3_cooldown_{0};
  static constexpr size_t kL3CooldownFrames = 96;  // 0.96s @10ms/帧
  // L3 触发判定 + classify 调用 + l3_* 字段填充。在 analyze() 末尾调用。
  void maybe_run_l3_(NoiseAnalysisResult& result,
                     const float* frames,
                     size_t frame_size);
};

}  // namespace noise

#endif  // NOISE_NOISE_ANALYZER_HPP_

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

#include "ml_classifier.hpp"   // MlClassifier, MlResult, MlTypeScore
#include "noise_detector.hpp"  // NoiseDetectionResult

namespace noise {

class NoiseTemplateDB;  // L2 kNN 检索的模板库（前向声明）

// 噪声类型(arch §3.3.6 L548)。
enum class NoiseType {
  Clean,      // 干净（无噪声）
  White,      // 白噪声（平坦频谱）
  Pink,       // 粉红噪声（-3dB/oct 斜率）
  Hum50Hz,    // 50Hz 工频哼声
  Hum60Hz,    // 60Hz 工频哼声
  Impulse,    // 脉冲噪声
  Broadband,  // 宽带噪声
  Digital,    // 数字噪声（高频异常）
  Unknown     // 未知
};

// 分析输入源(arch §3.3.6 L551)。
enum class AnalysisSource { OriginalPCM, NoisePCM, ResidualPCM };

// L1 候选噪声类型 + 置信度(arch §3.3.6 L554-557)。
struct NoiseTypeCandidate {
  NoiseType type;    // 候选噪声类型
  float confidence;  // 置信度 [0, 1]
};

// 噪声分析结果。L1/L2/L3 三层并行上报，各有独立字段。
struct NoiseAnalysisResult {
  // VAD 结果（从 detection 拷入，供 metrics 上报）
  bool is_speech{false};

  // ── L1 规则式分类（频域特征：White/Pink/Hum/Broadband 等）──
  NoiseType primary_type{NoiseType::Unknown};
  float primary_confidence{0.0f};              // L1 主类型置信度 [0, 1]
  std::vector<NoiseTypeCandidate> candidates;  // top-3 候选（confidence > 0.1）
  bool is_mixed{
      false};  // candidates.size() >= 2 && candidates[1].confidence > 0.3

  // ── L2 Bark 模板匹配（用户自定义噪声模板）──
  // 每帧用 32 维 Bark 频带能量与模板库做余弦相似度匹配。
  // similarity > 0.75 才视为匹配；无模板库 / 无匹配 -> 空值。
  uint32_t l2_match_id{0};    // 匹配到的模板 id（0=未匹配）
  std::string l2_match_name;  // 模板名称
  float l2_similarity{0.0f};  // 余弦相似度 [0, 1]

  // ── L3 YAMNet 分类（端到端声源识别）──
  std::string ml_noise_type;              // YAMNet top-1 类名
  float ml_noise_score{0.0f};             // YAMNet top-1 分数 [0, 1]
  std::vector<MlTypeScore> ml_top_types;  // YAMNet top-3（白名单内）

  // ── 量化指标 ──
  float noise_level_dbfs{-120.0f};    // 噪声级 (dBFS)
  float spectral_centroid_hz{0.0f};   // 频谱质心
  float spectral_flatness{0.0f};      // 频谱平坦度 [0, 1]
  float hum_strength_db{0.0f};        // 工频哼声强度 (dB)
  float impulse_count{0.0f};          // 脉冲计数/秒
  std::array<float, 32> band_energy;  // 1/3 倍频程能量（L2 特征向量）
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
  ~NoiseAnalyzer();  // out-of-line：ml_classifier_ shared_ptr<MlClassifier>
                     // 需完整类型
  // 分析一帧音频。
  //   frames/frame_size：L1/L2 分析输入（降噪开启时为噪声分量，关闭时为原始
  //   PCM） detection：VAD + SNR + SF 检测结果
  //   original_pcm/original_n：原始音频（L3 用，YAMNet
  //   多标签分类需完整混合音频。
  //     为空时 L3 回退到 frames。降噪开启时 L3 应传原始 PCM 而非噪声分量，
  //     因为 YAMNet 能同时识别语音+噪声，白名单过滤掉 Speech 后报告噪声类型。）
  NoiseAnalysisResult analyze(const float* frames,
                              size_t frame_size,
                              const NoiseDetectionResult& detection,
                              const float* original_pcm = nullptr,
                              size_t original_n = 0);

  // 注入 L3 ML 分类器。空 shared_ptr -> L3 跳过（L1 不受影响）。
  void set_ml_classifier(std::shared_ptr<MlClassifier> ml);
  // 注入 L2 模板库。空 shared_ptr -> L2 跳过。
  void set_template_db(std::shared_ptr<NoiseTemplateDB> db);

 private:
  // L1: 规则式分类(各规则输出置信度)
  std::vector<NoiseTypeCandidate> classify_rule_based(
      const std::vector<float>& power_spectrum,
      size_t fft_n,
      float sample_rate,
      const float* frames,
      size_t frame_size,
      float spectral_flatness);

  // ── L2 Bark 模板匹配（用户自定义噪声模板）──
  std::shared_ptr<NoiseTemplateDB> template_db_;

  // ── L3 ML 分类层（YAMNet 端到端分类，独立于 L1）──
  // L3 分类器（可选，空则跳过）。
  std::shared_ptr<MlClassifier> ml_classifier_;

  // L3 分类所需的 3s @48k PCM 环形缓冲。analyze() 每帧追加 analysis PCM，
  // 攒满 144000 样本后 L3 触发时取最新一窗送 MlClassifier::classify。
  // 单 capture 线程独占读写（per-sensor analyzer），无并发。
  static constexpr size_t kYamnetWindowSamples = 144000;  // 3s @48k
  std::vector<float> pcm_ring_;
  size_t pcm_ring_head_{0};
  size_t pcm_ring_count_{0};
  // L3 节流：触发后冷却若干帧（~3s），避免持续未知噪声时每帧都跑 ONNX。
  size_t l3_cooldown_{0};
  static constexpr size_t kL3CooldownFrames = 300;  // 3s @10ms/帧
  // L3 结果持久化：L3 触发频率低（每 3s 一次），但 metrics 快照每帧更新。
  // 若不持久化，L3 结果仅在触发帧出现（1/300），其余帧被默认空值覆盖。
  // last_l3_* 保存最近一次 L3 命中结果，每帧恢复到 result 中。
  std::string last_l3_type_;
  float last_l3_score_{0.0f};
  std::vector<MlTypeScore> last_l3_top_types_;
  bool l3_has_result_{false};
  // L3 触发判定 + classify 调用 + ml_* 字段填充。在 analyze() 中调用。
  // frames/frame_size 为 L3 分析输入（优先原始音频）。
  void maybe_run_l3_(NoiseAnalysisResult& result,
                     const float* frames,
                     size_t frame_size);
};

}  // namespace noise

#endif  // NOISE_NOISE_ANALYZER_HPP_

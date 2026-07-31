// daemon/noise/ml_classifier.hpp
// L3 ML 分类层 -- YAMNet ONNX 端到端多标签分类（D-S5.8 +
// identification §4/§10 决策5，VGGish -> YAMNet 迁移）。
//
// 职责单一：把一段 3s @48k PCM 重采样到 16k（48000 样本），送 YAMNet ONNX
// 推理，取 scores [6,521] 均值后按噪声类别白名单过滤 top-N，返回直接分类
// 结果。无需模板库（与 VGGish+kNN 方案的区别）。
//
// 触发模型（arch §3.3 L3）：L3 仅在 L1 规则式 + L2 Bark 模板**均未识别**
// （primary_confidence < 阈值，默认 0.5）时由 NoiseAnalyzer::analyze() 调用，
// 故低频。但 classify() 仍须 RT 安全：ONNX Run() 全程 try/catch，绝不向 RT
// 抛异常（与 T2 adapter 同一 RT 契约）。
//
// 模型签名约定（YAMNet ONNX）：
//   输入 = new_input [1, 48000]（3s @16kHz 波形，float32）
//   输出（按 index 绑定）：
//     [0] embeddings     [6, 1024]  -- 6 帧 × 1024 维嵌入（备用）
//     [1] log_mel_spectrogram [336, 64]  -- 中间表示（不使用）
//     [2] scores         [6, 521]   -- 6 帧 × 521 类 AudioSet 分数
//   classify() 取 scores 6 帧均值 -> [521]，白名单过滤后 top-3。
//
// 噪声类别白名单：521 类中约 40 个噪声相关类别（排除语音、音乐、动物等），
// 避免输出无关结果。白名单硬编码于 .cpp。
//
// class_map CSV：YAMNet class_map.csv 提供 index -> display_name 映射。
// init() 时从模型同目录自动查找 yamnet_class_map.csv 加载。
//
// 模型缺失降级（D-S5.8 隐含）：ml_model_path 为空或加载失败时
// available()=false， classify() 返回 nullopt；NoiseAnalyzer 据此跳过 L3，
// L1+L2 链路不受影响（additive，向后兼容）。
//
// 线程模型：MlClassifier 由 NoiseManager 持 shared_ptr，所有 sensor 的
// NoiseAnalyzer 共享同一实例。仅 capture 线程调 classify()（per-sensor 独占
// 调用，无并发）；Ort::Session per-classifier 单实例，不在 RT 线程析构
// （随 NoiseManager shared_ptr 在 main 栈展开释放，先于 OnnxEnv 静态析构）。
#ifndef NOISE_ML_CLASSIFIER_HPP_
#define NOISE_ML_CLASSIFIER_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace noise {

// YAMNet 分类结果项。
struct MlTypeScore {
  std::string type_name;  // AudioSet 类名（如 "Air conditioning"）
  float score{0.0f};      // YAMNet 分数 [0, 1]
};

// L3 ML 分类结果。
struct MlResult {
  std::string type_name;               // top-1 类名（空=未分类）
  float score{0.0f};                   // top-1 分数
  std::vector<MlTypeScore> top_types;  // top-3（白名单内，score > 0.1）
};

// YAMNet 嵌入维度（备用，当前不调用）。
constexpr size_t kYamnetEmbedDim = 1024;

// YAMNet ML 分类器：ONNX 端到端多标签分类。
class MlClassifier {
 public:
  // 构造时不加载模型（供测试 fake 子类无需模型路径）。模型由 init(path) 加载，
  // 失败 available()=false（不抛异常）。out-of-line：Ort::Session 在头内仅前向
  // 声明（不完整类型），析构需完整类型（与 T2 DtlnAdapter 同一手法）。
  MlClassifier();
  explicit MlClassifier(const std::string& model_path);
  virtual ~MlClassifier();

  MlClassifier(const MlClassifier&) = delete;
  MlClassifier& operator=(const MlClassifier&) = delete;

  // 加载 YAMNet ONNX 模型 + class_map CSV。路径空或加载失败返回 false。
  // model_path 同目录下查找 yamnet_class_map.csv（可选，缺失时用 index 数字）。
  // 重复调用：已 loaded 时返回 false（不重载）。
  bool init(const std::string& model_path);

  // 模型是否就绪（加载成功）。false 时 classify 返回 nullopt。
  bool available() const;

  // 3s @48k PCM -> YAMNet -> 噪声类型分类。
  //   pcm48k/n：48kHz 单声道浮点 PCM（int16/32768 归一化）。
  //   返回 MlResult（top-1 + top-3）；模型未就绪或 Run 异常时返回 nullopt。
  //   RT 安全：Run 异常被 try/catch 吞掉（返回 nullopt）。
  //   virtual：测试用 fake 子类覆写以计数 classify 调用次数。
  virtual std::optional<MlResult> classify(const float* pcm48k, size_t n) const;

  // 提取 3s @48k PCM 的 1024 维 YAMNet 嵌入（备用，当前不调用）。
  //   返回 1024 维嵌入；模型未就绪或 Run 异常时返回全零（RT 安全，不抛）。
  //   virtual：测试用 fake 子类覆写以注入确定性嵌入。
  virtual std::vector<float> embed(const float* pcm48k, size_t n) const;

 private:
  // 加载 class_map CSV（index -> display_name）。model_path 同目录查找。
  void load_class_map(const std::string& model_path);

  struct Impl;  // PImpl 持有 ONNX session + 重采样器 + class_map
  std::unique_ptr<Impl> impl_;
  // Resampler 有内部状态，不能并发调用。多 sensor 的 per-sink 线程
  // 共享同一 MlClassifier 实例，用 mutex 保护重采样 + ONNX Run。
  mutable std::mutex classify_mutex_;
};

}  // namespace noise

#endif  // NOISE_ML_CLASSIFIER_HPP_

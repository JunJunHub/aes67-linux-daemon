// daemon/noise/ml_classifier.hpp
// Spec5 Task 3：L3 ML 分类层 -- VGGish ONNX 嵌入 + kNN 检索（D-S5.8 +
// identification §4/§10 决策5）。
//
// 职责单一：把一段 0.96s @48k PCM 经 log-mel -> VGGish ONNX -> 128 维嵌入，
// 再对模板库中 feature_type=vggish 的模板做 kNN/余弦相似度检索，返回最佳匹配。
// 复用 Spec5 T2 的 onnx_session（进程级 Ort::Env + CreateOnnxSession RAII）。
//
// 触发模型（arch §3.3 L3）：L3 仅在 L1 规则式 + L2 Bark 模板**均未识别**
// （primary_confidence < 阈值，默认 0.5）时由 NoiseAnalyzer::analyze() 调用，
// 故低频。但 classify() 仍须 RT 安全：ONNX Run() 全程 try/catch，绝不向 RT
// 抛异常（与 T2 adapter 同一 RT 契约）。
//
// 模型签名约定（identification §4.1 + 设计 D-S5.8）：
//   VGGish ONNX 输入 = 0.96s log-mel 频谱图，形状 [1, 96, 64]
//     （96 帧 × 64 mel bins，@16kHz：frame=25ms/400smp，hop=10ms/160smp，
//      0.96s=15360smp；mel 125-7500Hz；log1p 功率谱）。
//   输出 = [1, 128] 嵌入向量。
//   按 index 绑定 I/O（名字随导出版本变化，index 稳定，与 T2 adapter 同）。
//
// 模型缺失降级（D-S5.8 隐含）：ml_model_path 为空或加载失败时
// available()=false， embed() 返回全零、classify() 返回 nullopt；NoiseAnalyzer
// 据此跳过 L3， L1+L2 链路不受影响（additive，向后兼容）。
//
// 线程模型：MlClassifier 由 NoiseManager 持 shared_ptr，所有 sensor 的
// NoiseAnalyzer 共享同一实例。仅 capture 线程调 classify()（per-sensor 独占
// 调用，无并发）；Ort::Session per-classifier 单实例，不在 RT 线程析构
// （随 NoiseManager shared_ptr 在 main 栈展开释放，先于 OnnxEnv 静态析构）。
#ifndef NOISE_ML_CLASSIFIER_HPP_
#define NOISE_ML_CLASSIFIER_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "noise_analyzer.hpp"  // NoiseType

namespace noise {

class NoiseTemplateDB;  // 前向声明：classify 仅在签名引用，.cpp 内 include
                        // 全定义

// L3 检索结果。VGGish 模板以字符串 label 标注噪声类型（Phase 3 不做 label->
// NoiseType 映射，type 语义即 label；与 L2 的 NoiseType 区分）。
struct L3Match {
  uint32_t template_id{0};  // 匹配到的 vggish 模板 id（0=无）
  std::string label;        // 模板 label（噪声类型名）
  float similarity{0.0f};   // 余弦相似度 [-1, 1]
};

// VGGish 嵌入维度（identification §4.1）。
constexpr size_t kVggishEmbedDim = 128;

// L3 ML 分类器：VGGish ONNX 嵌入 + kNN 检索。
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

  // 加载 VGGish ONNX 模型。路径空或加载失败返回 false（不抛异常）。
  // 重复调用：已 loaded 时返回 false（不重载）。
  bool init(const std::string& model_path);

  // 模型是否就绪（加载成功）。false 时 embed 返回全零、classify 返回 nullopt。
  // out-of-line：查询 impl_->session 需 Impl 完整类型（头内仅前向声明）。
  bool available() const;

  // 提取 0.96s @48k PCM 的 128 维 VGGish 嵌入。
  //   pcm48k/n：48kHz 单声道浮点 PCM（int16/32768 归一化）。
  //   返回 128 维嵌入；模型未就绪或 Run 异常时返回全零（RT 安全，不抛）。
  // virtual：测试用 fake 子类覆写以注入确定性嵌入 / 计数调用。
  virtual std::array<float, kVggishEmbedDim> embed(const float* pcm48k,
                                                   size_t n) const;

  // 嵌入 -> kNN 检索模板库（feature_type=vggish 模板）-> 最佳匹配。
  //   k=5 最近邻：对所有 vggish 模板算余弦相似度，取最高者；> 阈值返回。
  //   模型未就绪 / 无 vggish 模板 / 最高相似度 <= 阈值 -> 返回 nullopt。
  //   RT 安全：Run 异常被 embed 吞掉（返回全零 -> classify 退化为 nullopt）。
  //   virtual：测试用 fake 子类覆写以计数 classify 调用次数。
  virtual std::optional<L3Match> classify(
      const float* pcm48k,
      size_t n,
      const NoiseTemplateDB& templates) const;

 private:
  // 缓存 I/O 名字（init 时按 index 取一次，避免每帧悬垂；与 T2 adapter 同）。
  std::string in_name_;
  std::string out_name_;

  // log-mel 滤波器组 [kMelBins][nfft/2+1]（init 时惰性预算，embed 首调建）。
  // mutable：embed 是 const，首次调用惰性建表（线程不安全首次 -- 由
  // NoiseManager 保证 init 后单 capture 线程调 embed，无并发首建）。
  void ensure_mel_filterbank() const;
  struct Impl;  // PImpl 持有 mel 矩阵 + 重采样器暂存（避免头暴露 Ort 依赖）
  std::unique_ptr<Impl> impl_;

 protected:
  // Ort::Session 仅前向声明（头内不完整）。unique_ptr<Impl> 间接持有 session，
  // 使本类可拷贝安全地放 shared_ptr 且头不暴露 onnxruntime_cxx_api.h。
  // session 放 impl_ 内（构造需 Ort::Env，.cpp 内完成）。
};

}  // namespace noise

#endif  // NOISE_ML_CLASSIFIER_HPP_

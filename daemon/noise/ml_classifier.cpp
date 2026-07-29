// daemon/noise/ml_classifier.cpp
// L3 ML 分类实现 -- YAMNet ONNX 端到端多标签分类（VGGish -> YAMNet 迁移）。
//
// 推理流水线：
//   3s @48k PCM(144000smp) -> 重采样 16k(48000smp) ->
//   YAMNet ONNX Run -> scores [6,521] 均值 -> [521] ->
//   白名单过滤 -> 降序排序 -> top-3（score > 0.1）-> MlResult
//
// RT 安全契约（与 T2 adapter 同）：ONNX Run() 全程 try/catch，绝不向 RT 抛
// 异常；失败 classify 返回 nullopt（L3 退化为未识别，L1+L2 不受影响）。
// NaN/Inf 守卫：分数 sanitize 为有限值。
//
// 模型签名（yamnet_3s.onnx）：
//   输入[0] = new_input [1, 48000]  float32
//   输出[0] = embeddings [6, 1024]
//   输出[1] = log_mel_spectrogram [336, 64]
//   输出[2] = scores [6, 521]
//   按 index 绑定 I/O（名字随导出版本变化，index 稳定，与 T2 adapter 同）。
#include "ml_classifier.hpp"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include "onnx_session.hpp"
#include "resampler.hpp"

namespace noise {

namespace {
// YAMNet 参数。
constexpr uint32_t kYamnetSampleRate = 16000;  // YAMNet native 采样率
constexpr size_t kYamnetInputSamples = 48000;  // 3s @16k
constexpr size_t kYamnetNumClasses = 521;      // AudioSet 类别数
constexpr size_t kYamnetNumFrames = 6;         // 3s -> 6 帧（0.5s/帧）
constexpr size_t kTopN = 6;                    // 返回 top-6
constexpr float kMinScore = 0.05f;             // 最低报告分数

// sanitize：非有限 -> 0。
inline float sanitize(float v) {
  return std::isfinite(v) ? v : 0.0f;
}

// 噪声相关类别白名单（AudioSet index）。
// 排除语音、音乐、动物等非噪声类别。
// 来源：yamnet_class_map.csv 中与噪声诊断相关的 index。
constexpr int kNoiseClassIndices[] = {
    32,   // Humming -- 嗡鸣
    79,   // Hiss -- 嘶嘶声
    105,  // Roar -- 轰鸣
    125,  // Buzz -- 蜂鸣
    130,  // Rattle -- 咔嗒声
    277,  // Wind -- 风声
    279,  // Wind noise (microphone) -- 麦克风风噪
    282,  // Water -- 水声
    286,  // Stream -- 溪流声
    293,  // Crackle -- 爆裂声
    300,  // Motor vehicle (road) -- 机动车
    320,  // Motorcycle -- 摩托车
    321,  // Traffic noise, roadway noise -- 交通噪声
    323,  // Train -- 火车
    330,  // Aircraft engine -- 飞机引擎
    331,  // Jet engine -- 喷气引擎
    337,  // Engine -- 引擎
    338,  // Light engine (high frequency) -- 轻型引擎（高频）
    342,  // Medium engine (mid frequency) -- 中型引擎（中频）
    343,  // Heavy engine (low frequency) -- 重型引擎（低频）
    344,  // Engine knocking -- 引擎爆震
    353,  // Knock -- 敲击
    355,  // Squeak -- 吱吱声
    371,  // Vacuum cleaner -- 吸尘器
    382,  // Alarm -- 警报
    390,  // Siren -- 警笛
    392,  // Buzzer -- 蜂鸣器
    406,  // Mechanical fan -- 机械风扇
    407,  // Air conditioning -- 空调
    428,  // Burst, pop -- 爆裂、砰声
    434,  // Crack -- 断裂声
    438,  // Liquid -- 液体声
    443,  // Pour -- 倾倒声
    444,  // Trickle, dribble -- 滴漏声
    448,  // Pump (liquid) -- 泵（液体）
    454,  // Thump, thud -- 沉闷撞击
    469,  // Scrape -- 刮擦声
    478,  // Clang -- 哐当声
    482,  // Whir -- 旋转嗡声
    485,  // Clicking -- 咔哒声
    487,  // Rumble -- 隆隆声
    490,  // Hum -- 嗡嗡声
    504,  // Outside, rural or natural -- 户外/自然环境声
    507,  // Noise -- 噪声（通用）
    508,  // Environmental noise -- 环境噪声
    509,  // Static -- 静电噪声
    510,  // Mains hum -- 工频哼声
    514,  // White noise -- 白噪声
    515,  // Pink noise -- 粉红噪声
};
constexpr size_t kNoiseClassCount =
    sizeof(kNoiseClassIndices) / sizeof(kNoiseClassIndices[0]);
}  // namespace

// PImpl：把 onnxruntime 依赖 + 重采样器 + class_map 隔离在 .cpp。
struct MlClassifier::Impl {
  std::unique_ptr<Ort::Session> session;
  std::string in_name;
  std::string scores_name;
  std::string embeddings_name;
  // 48k->16k 重采样器（有内部状态，不能并发调用 -> classify_mutex_ 保护）。
  std::unique_ptr<Resampler> downsample;
  // class_map：AudioSet index -> display_name。
  std::unordered_map<int, std::string> class_map;
};

MlClassifier::MlClassifier() : impl_(std::make_unique<Impl>()) {}

MlClassifier::MlClassifier(const std::string& model_path)
    : impl_(std::make_unique<Impl>()) {
  init(model_path);
}

MlClassifier::~MlClassifier() = default;

bool MlClassifier::available() const {
  return impl_ && impl_->session != nullptr;
}

void MlClassifier::load_class_map(const std::string& model_path) {
  // 从 model_path 同目录查找 yamnet_class_map.csv。
  // 路径格式：/path/to/yamnet_3s.onnx -> /path/to/yamnet_class_map.csv
  auto last_sep = model_path.find_last_of("/\\");
  std::string dir =
      (last_sep != std::string::npos) ? model_path.substr(0, last_sep) : ".";
  std::string csv_path = dir + "/yamnet_class_map.csv";

  std::ifstream f(csv_path);
  if (!f.is_open()) {
    // 回退：尝试环境变量或已知路径。
    const char* env = std::getenv("NOISE_MODELS_DIR");
    if (env) {
      csv_path = std::string(env) + "/yamnet_class_map.csv";
      f.open(csv_path);
    }
  }
  if (!f.is_open())
    return;  // CSV 缺失：用 index 数字作为类名

  std::string line;
  std::getline(f, line);  // 跳过 header
  while (std::getline(f, line)) {
    // 格式：index,mid,display_name
    // display_name 可能含逗号（引号包裹），如 "Trickle, dribble"
    // 解析：index（到第一个逗号）, mid（到第二个逗号）, name（剩余全部）
    size_t c1 = line.find(',');
    if (c1 == std::string::npos)
      continue;
    size_t c2 = line.find(',', c1 + 1);
    if (c2 == std::string::npos)
      continue;
    std::string idx_str = line.substr(0, c1);
    // name = 第三个逗号后的全部内容
    std::string name = line.substr(c2 + 1);
    // 去除 Windows 换行符 \r（先于引号处理，因 \r 可能在引号之后）。
    if (!name.empty() && name.back() == '\r')
      name.pop_back();
    // 去除首尾引号（CSV 引号包裹的字段）。
    if (!name.empty() && name.front() == '"')
      name = name.substr(1);
    if (!name.empty() && name.back() == '"')
      name.pop_back();
    try {
      int idx = std::stoi(idx_str);
      impl_->class_map[idx] = name;
    } catch (...) {
      continue;
    }
  }
}

bool MlClassifier::init(const std::string& model_path) {
  if (impl_->session)
    return false;  // 已加载，不重载
  if (model_path.empty())
    return false;
  impl_->session = CreateOnnxSession(model_path);
  if (!impl_->session)
    return false;
  // 按 index 缓存 I/O 名字。
  impl_->in_name = OnnxInputName(*impl_->session, 0);
  // scores 是第 3 个输出（index=2）。
  impl_->scores_name = OnnxOutputName(*impl_->session, 2);
  // embeddings 是第 1 个输出（index=0），备用。
  impl_->embeddings_name = OnnxOutputName(*impl_->session, 0);
  if (impl_->in_name.empty() || impl_->scores_name.empty()) {
    impl_->session.reset();
    return false;
  }
  // 重采样器：48k -> 16k（YAMNet native）。
  impl_->downsample = std::make_unique<Resampler>(48000, kYamnetSampleRate);
  // 加载 class_map CSV。
  load_class_map(model_path);
  return true;
}

std::optional<MlResult> MlClassifier::classify(const float* pcm48k,
                                               size_t n) const {
  if (!available() || pcm48k == nullptr || n == 0)
    return std::nullopt;

  try {
    // 1. 48k -> 16k 重采样（局部 buffer，线程安全：多 sensor 并发调用）。
    const size_t cap = impl_->downsample->max_output_for_input(n);
    std::vector<float> pcm16k(cap);
    // Resampler 有内部状态，不能并发调用 -> 用 mutex 保护。
    std::lock_guard<std::mutex> lock(classify_mutex_);
    size_t n16 = impl_->downsample->process(pcm48k, n, pcm16k.data(), cap);
    if (n16 == 0)
      return std::nullopt;

    // 2. 构造输入张量 [1, 48000]。
    // YAMNet 输入固定 48000 样本；不足补零，超出截断。
    std::vector<float> input(kYamnetInputSamples, 0.0f);
    size_t copy_n = std::min(n16, kYamnetInputSamples);
    std::memcpy(input.data(), pcm16k.data(), copy_n * sizeof(float));

    const Ort::MemoryInfo& mi = OnnxMemoryInfo();
    int64_t in_shape[] = {1, static_cast<int64_t>(kYamnetInputSamples)};
    Ort::Value in_tensor = Ort::Value::CreateTensor<float>(
        mi, input.data(), input.size(), in_shape, 2);
    const char* in_names[1] = {impl_->in_name.c_str()};
    const char* out_names[1] = {impl_->scores_name.c_str()};

    // 3. ONNX Run（只取 scores 输出）。
    auto outputs = impl_->session->Run(Ort::RunOptions{nullptr}, in_names,
                                       &in_tensor, 1, out_names, 1);
    if (outputs.empty())
      return std::nullopt;

    const float* scores = outputs[0].GetTensorMutableData<float>();
    auto out_info = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
    // scores 形状 [6, 521] 或 [N, 521]。
    size_t num_frames = (out_info.size() >= 2) ? out_info[0] : kYamnetNumFrames;
    size_t num_classes =
        (out_info.size() >= 2) ? out_info[1] : kYamnetNumClasses;
    if (num_classes == 0)
      return std::nullopt;

    // 4. 6 帧分数取均值 -> [521]。
    std::vector<float> mean_scores(num_classes, 0.0f);
    for (size_t fr = 0; fr < num_frames; ++fr) {
      for (size_t c = 0; c < num_classes; ++c) {
        mean_scores[c] += sanitize(scores[fr * num_classes + c]);
      }
    }
    for (size_t c = 0; c < num_classes; ++c)
      mean_scores[c] /= static_cast<float>(num_frames);

    // 5. 白名单过滤 + 降序排序 -> top-3。
    std::vector<MlTypeScore> filtered;
    for (size_t i = 0; i < kNoiseClassCount; ++i) {
      int idx = kNoiseClassIndices[i];
      if (idx < 0 || static_cast<size_t>(idx) >= num_classes)
        continue;
      float s = mean_scores[idx];
      if (s < kMinScore)
        continue;
      MlTypeScore ts;
      ts.score = s;
      auto it = impl_->class_map.find(idx);
      ts.type_name = (it != impl_->class_map.end())
                         ? it->second
                         : "class_" + std::to_string(idx);
      filtered.push_back(std::move(ts));
    }
    std::sort(filtered.begin(), filtered.end(),
              [](const MlTypeScore& a, const MlTypeScore& b) {
                return a.score > b.score;
              });

    if (filtered.empty())
      return std::nullopt;

    // 6. 构造 MlResult。
    MlResult result;
    result.type_name = filtered[0].type_name;
    result.score = filtered[0].score;
    size_t top_n = std::min(filtered.size(), kTopN);
    result.top_types.assign(filtered.begin(), filtered.begin() + top_n);
    return result;
  } catch (...) {
    // RT 契约：Run/张量异常不抛出，返回 nullopt。
    return std::nullopt;
  }
}

std::vector<float> MlClassifier::embed(const float* pcm48k, size_t n) const {
  std::vector<float> out(kYamnetEmbedDim, 0.0f);
  if (!available() || pcm48k == nullptr || n == 0)
    return out;

  try {
    // 1. 48k -> 16k 重采样（局部 buffer + mutex，线程安全）。
    const size_t cap = impl_->downsample->max_output_for_input(n);
    std::vector<float> pcm16k(cap);
    std::lock_guard<std::mutex> lock(classify_mutex_);
    size_t n16 = impl_->downsample->process(pcm48k, n, pcm16k.data(), cap);
    if (n16 == 0)
      return out;

    // 2. 构造输入张量 [1, 48000]。
    std::vector<float> input(kYamnetInputSamples, 0.0f);
    size_t copy_n = std::min(n16, kYamnetInputSamples);
    std::memcpy(input.data(), pcm16k.data(), copy_n * sizeof(float));

    const Ort::MemoryInfo& mi = OnnxMemoryInfo();
    int64_t in_shape[] = {1, static_cast<int64_t>(kYamnetInputSamples)};
    Ort::Value in_tensor = Ort::Value::CreateTensor<float>(
        mi, input.data(), input.size(), in_shape, 2);
    const char* in_names[1] = {impl_->in_name.c_str()};
    const char* out_names[1] = {impl_->embeddings_name.c_str()};

    // 3. ONNX Run（取 embeddings 输出）。
    auto outputs = impl_->session->Run(Ort::RunOptions{nullptr}, in_names,
                                       &in_tensor, 1, out_names, 1);
    if (outputs.empty())
      return out;

    const float* emb = outputs[0].GetTensorMutableData<float>();
    size_t out_n = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
    // embeddings [6, 1024] -> 取最后一帧（最新）。
    size_t frame_size = kYamnetEmbedDim;
    size_t num_frames = out_n / frame_size;
    if (num_frames == 0)
      return out;
    size_t offset = (num_frames - 1) * frame_size;
    size_t copy = std::min(frame_size, kYamnetEmbedDim);
    for (size_t i = 0; i < copy; ++i)
      out[i] = sanitize(emb[offset + i]);
  } catch (...) {
    out.assign(kYamnetEmbedDim, 0.0f);
  }
  return out;
}

}  // namespace noise

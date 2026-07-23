// daemon/noise/ml_classifier.cpp
// Spec5 Task 3：L3 ML 分类实现 -- VGGish ONNX 嵌入 + kNN 检索（D-S5.8）。
//
// 嵌入流水线（identification §4.1 VGGish 预处理，canonical 配置）：
//   0.96s @48k PCM(46080smp) -> 重采样 16k(15360smp) ->
//   STFT(frame=400/25ms, hop=160/10ms, Hann, nfft=512) -> 96 帧 ->
//   功率谱 |STFT|² -> 64 mel bins(125-7500Hz) -> log1p ->
//   [1,96,64] log-mel -> VGGish ONNX -> [1,128] 嵌入。
//
// RT 安全契约（与 T2 adapter 同）：ONNX Run() 全程 try/catch，绝不向 RT 抛
// 异常；失败 embed 返回全零、classify 返回 nullopt（L3 退化为未识别，L1+L2
// 不受影响）。NaN/Inf 守卫：嵌入元素 sanitize 为有限值。
//
// 模型签名约定见 ml_classifier.hpp。无法在无模型环境下验证此签名/mel 配置
// 是否与最终 VGGish ONNX 严格匹配 -- 按 index 绑定 I/O（名字稳健）+ canonical
// mel 参数，模型缺失时相关测试 SKIP。此为 Spec5 T3 已知 debt（见 report）。
#include "ml_classifier.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include "fft.hpp"
#include "noise_template_db.hpp"
#include "onnx_session.hpp"
#include "resampler.hpp"

namespace noise {

namespace {
// VGGish 预处理参数（identification §4.1 canonical）。
constexpr uint32_t kVggishSampleRate = 16000;  // VGGish native 采样率
constexpr size_t kVggishFrame = 400;           // 25ms @16k
constexpr size_t kVggishHop = 160;             // 10ms @16k
constexpr size_t kVggishNfft = 512;            // STFT FFT 点数（radix-2）
constexpr size_t kVggishNumFrames = 96;        // 0.96s -> 96 帧
constexpr size_t kVggishMelBins = 64;          // mel 滤波器组 bin 数
constexpr float kVggishMelLo = 125.0f;         // mel 下限 (Hz)
constexpr float kVggishMelHi = 7500.0f;        // mel 上限 (Hz)
constexpr float kVggishLogOffset = 1.0f;       // log1p 偏移（稳定零谱）

// hz -> mel（HTK 公式：2595·log10(1+hz/700)）。
inline float hz_to_mel(float hz) {
  return 2595.0f * std::log10(1.0f + hz / 700.0f);
}
// mel -> hz。
inline float mel_to_hz(float mel) {
  return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f);
}

// sanitize：非有限 -> 0。
inline float sanitize(float v) {
  return std::isfinite(v) ? v : 0.0f;
}
}  // namespace

// PImpl：把 onnxruntime 依赖 + mel 矩阵 + 重采样器隔离在 .cpp，
// 使 ml_classifier.hpp 不 include onnxruntime_cxx_api.h（与 T2 adapter
// 同手法）。
struct MlClassifier::Impl {
  std::unique_ptr<Ort::Session> session;
  std::string in_name;
  std::string out_name;
  // mel 滤波器组 [kVggishMelBins][nfft/2+1=257]（惰性建）。
  std::vector<std::vector<float>> mel_filterbank;
  bool mel_built{false};
  // 48k->16k 重采样器（复用 T1 Resampler 原语）。
  std::unique_ptr<Resampler> downsample;
  // embed 暂存（复用容量，避免每次堆分配）。
  std::vector<float> mel16;          // 重采样后 16k 样本
  std::vector<float> logmel;         // [96*64] log-mel 平面
  std::vector<float> scratch_frame;  // 单帧功率谱暂存
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

bool MlClassifier::init(const std::string& model_path) {
  if (impl_->session)
    return false;  // 已加载，不重载
  if (model_path.empty())
    return false;
  impl_->session = CreateOnnxSession(model_path);
  if (!impl_->session)
    return false;
  // 按 index 缓存 I/O 名字（名字随导出版本变化，index 稳定，与 T2 adapter
  // 同）。
  impl_->in_name = OnnxInputName(*impl_->session, 0);
  impl_->out_name = OnnxOutputName(*impl_->session, 0);
  if (impl_->in_name.empty() || impl_->out_name.empty()) {
    impl_->session.reset();
    return false;
  }
  // 重采样器：48k -> 16k（VGGish native）。
  impl_->downsample = std::make_unique<Resampler>(48000, kVggishSampleRate);
  return true;
}

// 惰性构建 mel 滤波器组 [64][257]。每行一个 mel 三角滤波器，覆盖 fft bins。
void MlClassifier::ensure_mel_filterbank() const {
  if (impl_->mel_built)
    return;
  const size_t nbins = kVggishNfft / 2 + 1;  // 257
  impl_->mel_filterbank.assign(kVggishMelBins, std::vector<float>(nbins, 0.0f));
  const float mel_lo = hz_to_mel(kVggishMelLo);
  const float mel_hi = hz_to_mel(kVggishMelHi);
  // 64 个 mel bin -> 66 个 mel 边缘点。
  for (size_t m = 0; m < kVggishMelBins; ++m) {
    float mel_left = mel_lo + (mel_hi - mel_lo) * static_cast<float>(m) /
                                  static_cast<float>(kVggishMelBins);
    float mel_center = mel_lo + (mel_hi - mel_lo) * static_cast<float>(m + 1) /
                                    static_cast<float>(kVggishMelBins);
    float mel_right = mel_lo + (mel_hi - mel_lo) * static_cast<float>(m + 2) /
                                   static_cast<float>(kVggishMelBins);
    float f_left = mel_to_hz(mel_left);
    float f_center = mel_to_hz(mel_center);
    float f_right = mel_to_hz(mel_right);
    float row_sum = 0.0f;
    for (size_t k = 0; k < nbins; ++k) {
      float f = static_cast<float>(k) * (static_cast<float>(kVggishSampleRate) /
                                         static_cast<float>(kVggishNfft));
      if (f < f_left || f > f_right) {
        continue;  // 三角滤波器外
      }
      float w = 0.0f;
      if (f <= f_center) {
        float denom = (f_center - f_left);
        w = (denom > 1e-12f) ? (f - f_left) / denom : 0.0f;
      } else {
        float denom = (f_right - f_center);
        w = (denom > 1e-12f) ? (f_right - f) / denom : 0.0f;
      }
      impl_->mel_filterbank[m][k] = w;
      row_sum += w;
    }
    // 归一化行（能量均衡，与 librosa normalize=None 不同但对 kNN 余弦相似度
    // 无影响 -- 余弦对放缩不敏感）。
    if (row_sum > 1e-12f) {
      for (size_t k = 0; k < nbins; ++k)
        impl_->mel_filterbank[m][k] /= row_sum;
    }
  }
  impl_->mel_built = true;
}

std::array<float, kVggishEmbedDim> MlClassifier::embed(const float* pcm48k,
                                                       size_t n) const {
  std::array<float, kVggishEmbedDim> out{};
  out.fill(0.0f);
  if (!available() || pcm48k == nullptr || n == 0)
    return out;

  try {
    ensure_mel_filterbank();
    // 1. 48k -> 16k 重采样。
    const size_t cap = impl_->downsample->max_output_for_input(n);
    if (impl_->mel16.size() < cap)
      impl_->mel16.resize(cap);
    size_t n16 =
        impl_->downsample->process(pcm48k, n, impl_->mel16.data(), cap);
    if (n16 == 0)
      return out;

    // 2. STFT -> log-mel [96][64]。
    impl_->logmel.assign(kVggishNumFrames * kVggishMelBins, 0.0f);
    const size_t nbins = kVggishNfft / 2 + 1;  // 257
    // Hann 窗（每帧同窗，预算一次）。
    std::vector<float> hann(kVggishFrame);
    for (size_t i = 0; i < kVggishFrame; ++i)
      hann[i] = 0.5f - 0.5f * std::cos(2.0f * fft::kPi * static_cast<float>(i) /
                                       static_cast<float>(kVggishFrame - 1));
    if (impl_->scratch_frame.size() < kVggishNfft)
      impl_->scratch_frame.assign(kVggishNfft, 0.0f);

    for (size_t fr = 0; fr < kVggishNumFrames; ++fr) {
      size_t start = fr * kVggishHop;
      // 取一帧（零填充超出部分）。
      std::vector<std::complex<float>> X(kVggishNfft,
                                         std::complex<float>(0.0f, 0.0f));
      for (size_t i = 0; i < kVggishFrame; ++i) {
        float s = (start + i < n16) ? impl_->mel16[start + i] : 0.0f;
        X[i] = std::complex<float>(s * hann[i], 0.0f);
      }
      // radix-2 FFT（512 是 2 的幂）。
      fft::FftRadix2(X, -1);
      // 功率谱 -> mel -> log1p。
      for (size_t m = 0; m < kVggishMelBins; ++m) {
        float e = 0.0f;
        for (size_t k = 0; k < nbins; ++k) {
          e += impl_->mel_filterbank[m][k] * std::norm(X[k]);
        }
        impl_->logmel[fr * kVggishMelBins + m] =
            std::log1p(e * kVggishLogOffset);
      }
    }

    // 3. 绑定输入张量 [1, 96, 64] 并 Run。
    const Ort::MemoryInfo& mi = OnnxMemoryInfo();
    int64_t in_shape[] = {1, static_cast<int64_t>(kVggishNumFrames),
                          static_cast<int64_t>(kVggishMelBins)};
    Ort::Value in_tensor = Ort::Value::CreateTensor<float>(
        mi, impl_->logmel.data(), impl_->logmel.size(), in_shape, 3);
    const char* in_names[1] = {impl_->in_name.c_str()};
    const char* out_names[1] = {impl_->out_name.c_str()};
    auto outputs = impl_->session->Run(Ort::RunOptions{nullptr}, in_names,
                                       &in_tensor, 1, out_names, 1);
    if (outputs.empty())
      return out;
    const float* emb = outputs[0].GetTensorMutableData<float>();
    size_t out_n = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
    size_t copy_n = std::min(out_n, kVggishEmbedDim);
    for (size_t i = 0; i < copy_n; ++i)
      out[i] = sanitize(emb[i]);
  } catch (...) {
    // RT 契约：Run/张量异常不抛出，返回全零（L3 退化为未识别）。
    out.fill(0.0f);
  }
  return out;
}

std::optional<L3Match> MlClassifier::classify(
    const float* pcm48k,
    size_t n,
    const NoiseTemplateDB& templates) const {
  if (!available())
    return std::nullopt;
  auto emb = embed(pcm48k, n);
  // 全零嵌入（Run 失败/静音）-> 无法检索。
  float norm = 0.0f;
  for (float v : emb)
    norm += v * v;
  if (norm <= 0.0f)
    return std::nullopt;
  auto [tid, sim] = templates.match_vggish(emb);
  if (tid == 0)
    return std::nullopt;
  L3Match m;
  m.template_id = tid;
  m.similarity = sim;
  if (const Template* t = templates.get_template(tid))
    m.label = t->name;
  return m;
}

}  // namespace noise

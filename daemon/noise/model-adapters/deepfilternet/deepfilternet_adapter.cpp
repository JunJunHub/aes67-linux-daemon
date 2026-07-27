// daemon/noise/model-adapters/deepfilternet/deepfilternet_adapter.cpp
// DeepFilterNet3 适配器实现 + 静态注册（"deepfilternet"）。
// 架构依据：docs/noise/denoise-plugin-architecture.md §3.3。
//
// **当前实现状态（Spec6 T2）**：
// init() 加载 enc/df_dec/erb_dec 三子图并校验签名，构造 ERB 滤波器组与
// vorbis 窗。process() 完整实现 libDF 的逐帧流式信号处理（STFT/特征/norm
// 状态/三子图编排/深度滤波应用/ISTFT overlap-add/lookahead 缓冲）。
//
// **Spec6 T2（D-S6.6）DFN deep-filter non-causal 重写**：
// 对照 DeepFilterNet/df/modules.py DfOp + spec_pad + assign_df（non-causal
// window [i-2..i+2] + coef[0] 配最旧帧 + alpha blend）。modules.py 未 vendored
// 于本 worktree，实现依据 spec §T2 设计描述 + 测试（行为权威定义，见
// apply_df_op）。延迟 (coefs, gain, mask) kDfLookahead=2 帧以对齐
// non-causal window，warmup=0（延迟由 df_delay_buf_ 提供，总算法延迟不变 =
// hop*(1+kDfLookahead)=1440）。真实 DFN 模型 fidelity 验证需模型（CI skip）。
#include "deepfilternet_adapter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <utility>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include "denoise_plugin_factory.hpp"
#include "onnx_session.hpp"

namespace noise {

namespace {

inline uint32_t float_to_bits(float f) noexcept {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  return bits;
}
inline float bits_to_float(uint32_t bits) noexcept {
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}
inline float sanitize(float s) noexcept {
  if (!std::isfinite(s))
    return 0.0f;
  if (s > 1.0f)
    return 1.0f;
  if (s < -1.0f)
    return -1.0f;
  return s;
}

// ERB 频率换算（libdf freq2erb/erb2freq，Glasberg-Moore ERB 公式）。
inline float freq2erb(float f) {
  return 9.265f * std::log1p(f / (24.7f * 9.265f));
}
inline float erb2freq(float e) {
  return 24.7f * 9.265f * (std::exp(e / 9.265f) - 1.0f);
}

// feat_erb / feat_cplx 的指数均值归一化 alpha（libdf calc_norm_alpha）。
// alpha = exp(-hop / (sr * tau))，tau=norm_tau=1。
inline float calc_norm_alpha(uint32_t sr, size_t hop, float tau) {
  return std::exp(-static_cast<float>(hop) / (static_cast<float>(sr) * tau));
}

// mean_norm 初始化（libdf MEAN_NORM_INIT = [-2.77, -3.0]）。
constexpr float kMeanNormInitMin = -2.77f;
constexpr float kMeanNormInitMax = -3.0f;
// unit_norm 初始化（libdf UNIT_NORM_INIT = [1e-6, 1.0]）。
constexpr float kUnitNormInitMin = 1e-6f;
constexpr float kUnitNormInitMax = 1.0f;

}  // namespace

DeepFilterNetAdapter::DeepFilterNetAdapter() = default;
DeepFilterNetAdapter::~DeepFilterNetAdapter() = default;

void DeepFilterNetAdapter::init_erb_fb_() {
  // libdf erb_fb：每 ERB band 的频点数，sum == kFreq=481。
  const float nyq = static_cast<float>(kSr) / 2.0f;
  const float freq_width = static_cast<float>(kSr) / static_cast<float>(kFft);
  const float erb_low = freq2erb(0.0f);
  const float erb_high = freq2erb(nyq);
  const float step = (erb_high - erb_low) / static_cast<float>(kNbErb);
  erb_.assign(kNbErb, 0);
  size_t prev_freq = 0;
  int freq_over = 0;
  const int min_nb_freqs = 2;
  for (size_t i = 1; i <= kNbErb; ++i) {
    const float f = erb2freq(erb_low + static_cast<float>(i) * step);
    const size_t fb = static_cast<size_t>(std::round(f / freq_width));
    int nb_freqs =
        static_cast<int>(fb) - static_cast<int>(prev_freq) - freq_over;
    if (nb_freqs < min_nb_freqs) {
      freq_over = min_nb_freqs - nb_freqs;
      nb_freqs = min_nb_freqs;
    } else {
      freq_over = 0;
    }
    erb_[i - 1] = static_cast<size_t>(nb_freqs);
    prev_freq = fb;
  }
  erb_[kNbErb - 1] += 1;  // kFreq = kFft/2+1 个频点
  const size_t total = std::accumulate(erb_.begin(), erb_.end(), size_t{0});
  const long too_large = static_cast<long>(total) - static_cast<long>(kFreq);
  if (too_large > 0)
    erb_[kNbErb - 1] -= static_cast<size_t>(too_large);
}

void DeepFilterNetAdapter::init_window_() {
  // libdf vorbis 窗：sin(pi/2 *
  // sin^2(pi*(n+0.5)/(N/2)))。wnorm=1/(N^2/(2*hop))。
  const double pi = 3.14159265358979323846;
  const size_t wh = kFft / 2;
  window_.assign(kFft, 0.0f);
  for (size_t i = 0; i < kFft; ++i) {
    const double sinv = std::sin(0.5 * pi * (static_cast<double>(i) + 0.5) /
                                 static_cast<double>(wh));
    window_[i] = static_cast<float>(std::sin(0.5 * pi * sinv * sinv));
  }
  wnorm_ =
      1.0f / (static_cast<float>(kFft * kFft) / static_cast<float>(2 * kHop));
}

bool DeepFilterNetAdapter::init(const PluginConfig& cfg) {
  if (initialized_)
    return false;

  // 模型目录：cfg.onnx_model_dir 或 cfg.model_path 所在目录。
  std::string dir = cfg.onnx_model_dir;
  if (dir.empty() && !cfg.model_path.empty()) {
    auto slash = cfg.model_path.find_last_of('/');
    dir = (slash == std::string::npos) ? "." : cfg.model_path.substr(0, slash);
  }
  if (dir.empty())
    return false;
  std::string d = dir;
  if (!d.empty() && d.back() != '/')
    d += '/';

  enc_ = CreateOnnxSession(d + "enc.onnx");
  df_dec_ = CreateOnnxSession(d + "df_dec.onnx");
  erb_dec_ = CreateOnnxSession(d + "erb_dec.onnx");
  if (!enc_ || !df_dec_ || !erb_dec_)
    return false;

  // 缓存 I/O 名字（按 index 绑定，名字随导出版本变化）。
  enc_in0_ = OnnxInputName(*enc_, 0);  // feat_erb
  enc_in1_ = OnnxInputName(*enc_, 1);  // feat_spec
  const size_t enc_n_out = enc_->GetOutputCount();
  enc_out_names_.resize(enc_n_out);
  enc_out_c_.resize(enc_n_out);
  for (size_t i = 0; i < enc_n_out; ++i) {
    enc_out_names_[i] = OnnxOutputName(*enc_, i);
    enc_out_c_[i] = enc_out_names_[i].c_str();
  }
  enc_in_c_ = {enc_in0_.c_str(), enc_in1_.c_str()};

  const size_t df_n_in = df_dec_->GetInputCount();
  df_in_names_.resize(df_n_in);
  df_in_c_.resize(df_n_in);
  for (size_t i = 0; i < df_n_in; ++i) {
    df_in_names_[i] = OnnxInputName(*df_dec_, i);
    df_in_c_[i] = df_in_names_[i].c_str();
  }
  const size_t df_n_out = df_dec_->GetOutputCount();
  df_out_names_.resize(df_n_out);
  df_out_c_.resize(df_n_out);
  for (size_t i = 0; i < df_n_out; ++i) {
    df_out_names_[i] = OnnxOutputName(*df_dec_, i);
    df_out_c_[i] = df_out_names_[i].c_str();
  }

  const size_t erb_n_in = erb_dec_->GetInputCount();
  erb_in_names_.resize(erb_n_in);
  erb_in_c_.resize(erb_n_in);
  for (size_t i = 0; i < erb_n_in; ++i) {
    erb_in_names_[i] = OnnxInputName(*erb_dec_, i);
    erb_in_c_[i] = erb_in_names_[i].c_str();
  }
  const size_t erb_n_out = erb_dec_->GetOutputCount();
  erb_out_names_.resize(erb_n_out);
  erb_out_c_.resize(erb_n_out);
  for (size_t i = 0; i < erb_n_out; ++i) {
    erb_out_names_[i] = OnnxOutputName(*erb_dec_, i);
    erb_out_c_[i] = erb_out_names_[i].c_str();
  }

  init_erb_fb_();
  init_window_();

  // 流式状态初始化。
  analysis_mem_.assign(kFft - kHop, 0.0f);   // 480
  synthesis_mem_.assign(kFft - kHop, 0.0f);  // 480
  mean_norm_state_.resize(kNbErb);
  for (size_t i = 0; i < kNbErb; ++i)
    mean_norm_state_[i] =
        kMeanNormInitMin + static_cast<float>(i) *
                               (kMeanNormInitMax - kMeanNormInitMin) /
                               static_cast<float>(kNbErb - 1);
  unit_norm_state_.resize(kNbDf);
  for (size_t i = 0; i < kNbDf; ++i)
    unit_norm_state_[i] =
        kUnitNormInitMin + static_cast<float>(i) *
                               (kUnitNormInitMax - kUnitNormInitMin) /
                               static_cast<float>(kNbDf - 1);
  // Spec6 final review I2：df_spec_history_ 改 ring buffer（预分配 std::array，
  // 零 per-frame heap）。init 预分配每个 slot 的 vector 容量。
  for (auto& v : df_spec_history_)
    v.assign(kNbDf, fft::Complex(0, 0));
  df_spec_history_head_ = 0;

  spec_.resize(kFreq);
  feat_erb_.resize(kNbErb);      // [1,1,1,32]
  feat_spec_.resize(2 * kNbDf);  // [1,2,1,96] = [re(96), im(96)]
  // Spec6 T3：预分配 process_one_frame_ 暂存（零 per-frame heap）。
  spec_m_.resize(kFreq);
  spec_e_.resize(kFreq);
  df_out_.resize(kNbDf);
  // df_delay_ring_ 的 slot 预分配容量。
  for (auto& slot : df_delay_ring_) {
    slot.coefs.assign(kNbDf * kDfOrder * 2, 0.0f);
    slot.spec_m.assign(kFreq, fft::Complex(0, 0));
  }

  float dw = cfg.dry_wet;
  if (dw < 0.0f)
    dw = 0.0f;
  if (dw > 1.0f)
    dw = 1.0f;
  dry_wet_bits_.store(float_to_bits(dw), std::memory_order_relaxed);

  reset();
  initialized_ = true;
  return true;
}

void DeepFilterNetAdapter::reset() {
  std::fill(analysis_mem_.begin(), analysis_mem_.end(), 0.0f);
  std::fill(synthesis_mem_.begin(), synthesis_mem_.end(), 0.0f);
  // norm state 不重置（libdf reset 仅清 analysis/synthesis mem）。
  // Spec6 final review I2：ring buffer 状态重置（清内容 + 归零 head）。
  for (auto& v : df_spec_history_)
    std::fill(v.begin(), v.end(), fft::Complex(0, 0));
  df_spec_history_head_ = 0;
  // Spec6 T3：ring buffer 状态重置。
  df_delay_idx_ = 0;
  df_delay_count_ = 0;
  // Spec6 T3 review Important #4：out ring buffer 重置（零 heap）。
  out_ring_head_ = 0;
  out_ring_tail_ = 0;
  out_ring_count_ = 0;
  in_fifo_.clear();
  out_fifo_.clear();
  in_delay_.clear();
}

const char* DeepFilterNetAdapter::name() const {
  return "deepfilternet";
}
uint32_t DeepFilterNetAdapter::native_sample_rate() const {
  return kSr;
}
uint32_t DeepFilterNetAdapter::algorithmic_latency_samples() const {
  // hop + lookahead*hop = 480 + 2*480 = 1440 @48k = 30ms（实测确认）。
  return kHop * (1 + kDfLookahead);
}
bool DeepFilterNetAdapter::supports_vad() const {
  return false;
}
bool DeepFilterNetAdapter::supports_snr() const {
  return true;
}

bool DeepFilterNetAdapter::process_one_frame_(float& lsnr_out) {
  // 从 in_fifo_ 取 kHop=480 样本。
  // Spec6 T3：input_/buf_ 改预分配成员（零 per-frame heap）。
  for (size_t i = 0; i < kHop; ++i) {
    input_[i] = in_fifo_.front();
    in_fifo_.pop_front();
  }

  const float alpha = calc_norm_alpha(kSr, kHop, 1.0f);

  try {
    // ── 1. 分析 STFT（libdf frame_analysis）──
    // buf = [analysis_mem * window[0:480] ; input * window[480:960]]
    for (size_t i = 0; i < kFft - kHop; ++i)
      buf_[i] = analysis_mem_[i] * window_[i];
    for (size_t i = 0; i < kHop; ++i)
      buf_[kFft - kHop + i] = input_[i] * window_[kFft - kHop + i];
    // analysis_mem <- input（下帧的 prev）
    std::copy(input_.begin(), input_.end(), analysis_mem_.begin());
    // rfft(buf) -> 481 复频点，* wnorm
    // Spec6 T3：Rfft 改输出参数，spec_ 预分配成员复用。
    fft::Rfft(buf_.data(), kFft, spec_.data(), spec_.size());
    for (auto& c : spec_)
      c *= wnorm_;

    // ── 2. 特征提取 ──
    // feat_erb：band_corr(spec,spec) -> 10log10 -> mean_norm（/40）。
    {
      size_t bcsum = 0;
      for (size_t b = 0; b < kNbErb; ++b) {
        const size_t bs = erb_[b];
        float s = 0.0f;
        for (size_t j = 0; j < bs; ++j) {
          const auto& c = spec_[bcsum + j];
          s += (c.real() * c.real() + c.imag() * c.imag());
        }
        s = (s / static_cast<float>(bs) + 1e-10f);
        s = 10.0f * std::log10(s);
        // band_mean_norm_erb：state=state*alpha +
        // s*(1-alpha)；s-=state；s/=40。
        mean_norm_state_[b] = mean_norm_state_[b] * alpha + s * (1.0f - alpha);
        feat_erb_[b] = (s - mean_norm_state_[b]) / 40.0f;
        bcsum += bs;
      }
    }
    // feat_spec：unit_norm(spec[0:96]) -> [re(96), im(96)]。
    for (size_t f = 0; f < kNbDf; ++f) {
      const float re = spec_[f].real();
      const float im = spec_[f].imag();
      const float mag = std::sqrt(re * re + im * im);
      unit_norm_state_[f] = unit_norm_state_[f] * alpha + mag * (1.0f - alpha);
      const float denom = std::sqrt(unit_norm_state_[f]);
      const float inv = (denom > 0.0f) ? (1.0f / denom) : 0.0f;
      feat_spec_[f] = re * inv;          // [0..96) = re
      feat_spec_[kNbDf + f] = im * inv;  // [96..192) = im
    }

    // ── 3. enc.run([feat_erb(1,1,1,32), feat_spec(1,2,1,96)]) ──
    const Ort::MemoryInfo& mi = OnnxMemoryInfo();
    int64_t erb_shape[] = {1, 1, 1, static_cast<int64_t>(kNbErb)};
    int64_t spec_shape[] = {1, 2, 1, static_cast<int64_t>(kNbDf)};
    Ort::Value in_erb = Ort::Value::CreateTensor<float>(
        mi, feat_erb_.data(), feat_erb_.size(), erb_shape, 4);
    Ort::Value in_spec = Ort::Value::CreateTensor<float>(
        mi, feat_spec_.data(), feat_spec_.size(), spec_shape, 4);
    const char* enc_in[2] = {enc_in0_.c_str(), enc_in1_.c_str()};
    Ort::Value enc_inputs[2] = {std::move(in_erb), std::move(in_spec)};
    auto enc_out = enc_->Run(Ort::RunOptions{nullptr}, enc_in, enc_inputs, 2,
                             enc_out_c_.data(), enc_out_c_.size());
    // enc outputs: e0,e1,e2,e3,emb,c0,lsnr (index 0..6, order per export).
    // 读取各输出指针（shape 含 S=1）。
    const float* e0 = enc_out[0].GetTensorMutableData<float>();
    const float* e1 = enc_out[1].GetTensorMutableData<float>();
    const float* e2 = enc_out[2].GetTensorMutableData<float>();
    const float* e3 = enc_out[3].GetTensorMutableData<float>();
    const float* emb = enc_out[4].GetTensorMutableData<float>();  // [1,1,512]
    const float* c0 = enc_out[5].GetTensorMutableData<float>();   // [1,64,1,96]
    const float* lsnr = enc_out[6].GetTensorMutableData<float>();  // [1,1]
    lsnr_out = lsnr ? lsnr[0] : 0.0f;

    // ── 4. df_dec.run([emb, c0]) -> coefs, gain ──
    int64_t emb_shape[] = {1, 1, 512};
    int64_t c0_shape[] = {1, 64, 1, static_cast<int64_t>(kNbDf)};
    Ort::Value in_emb = Ort::Value::CreateTensor<float>(
        mi, const_cast<float*>(emb), 512, emb_shape, 3);
    Ort::Value in_c0 = Ort::Value::CreateTensor<float>(
        mi, const_cast<float*>(c0), 64 * kNbDf, c0_shape, 4);
    const char* df_in[2] = {df_in_c_[0], df_in_c_[1]};
    Ort::Value df_inputs[2] = {std::move(in_emb), std::move(in_c0)};
    auto df_out = df_dec_->Run(Ort::RunOptions{nullptr}, df_in, df_inputs, 2,
                               df_out_c_.data(), df_out_c_.size());
    const float* coefs =
        df_out[0].GetTensorMutableData<float>();                  // [1,1,96,10]
    const float* gain = df_out[1].GetTensorMutableData<float>();  // [1,1,1]

    // ── 5. erb_dec.run([emb, e3, e2, e1, e0]) -> m[1,1,1,32] ──
    // erb_dec 输入顺序（实测签名）：emb, e3, e2, e1, e0。
    int64_t e3_shape[] = {1, 64, 1, 8};
    int64_t e2_shape[] = {1, 64, 1, 8};
    int64_t e1_shape[] = {1, 64, 1, 16};
    int64_t e0_shape[] = {1, 64, 1, 32};
    Ort::Value erb_in_emb = Ort::Value::CreateTensor<float>(
        mi, const_cast<float*>(emb), 512, emb_shape, 3);
    Ort::Value erb_in_e3 = Ort::Value::CreateTensor<float>(
        mi, const_cast<float*>(e3), 64 * 8, e3_shape, 4);
    Ort::Value erb_in_e2 = Ort::Value::CreateTensor<float>(
        mi, const_cast<float*>(e2), 64 * 8, e2_shape, 4);
    Ort::Value erb_in_e1 = Ort::Value::CreateTensor<float>(
        mi, const_cast<float*>(e1), 64 * 16, e1_shape, 4);
    Ort::Value erb_in_e0 = Ort::Value::CreateTensor<float>(
        mi, const_cast<float*>(e0), 64 * 32, e0_shape, 4);
    Ort::Value erb_inputs[5] = {std::move(erb_in_emb), std::move(erb_in_e3),
                                std::move(erb_in_e2), std::move(erb_in_e1),
                                std::move(erb_in_e0)};
    auto erb_out =
        erb_dec_->Run(Ort::RunOptions{nullptr}, erb_in_c_.data(), erb_inputs, 5,
                      erb_out_c_.data(), erb_out_c_.size());
    const float* m = erb_out[0].GetTensorMutableData<float>();  // [1,1,1,32]

    // ── 6. 应用回复谱 ──
    // 6a. ERB 掩蔽：spec_m = spec * interp_band_gain(m, erb_)（全 481 频点）。
    // Spec6 T3：spec_m_ 改预分配成员（零 per-frame heap）。
    std::copy(spec_.begin(), spec_.end(), spec_m_.begin());
    {
      size_t bcsum = 0;
      for (size_t b = 0; b < kNbErb; ++b) {
        const float g = m[b];
        for (size_t j = 0; j < erb_[b]; ++j)
          spec_m_[bcsum + j] *= g;
        bcsum += erb_[b];
      }
    }
    // 6b. Spec6 T2（D-S6.6）：non-causal deep-filter。
    // 对照 DeepFilterNet/df/modules.py DfOp + spec_pad + assign_df：
    //   - non-causal window [i-2..i+2]（5 帧，lookahead=2）
    //   - coef[0] 配最旧帧（window[0]），coef[4] 配最新帧（window[4]）
    //   - alpha blend: spec_f = spec_f*alpha + spec_orig*(1-alpha)
    // 延迟 (coefs, gain, spec_m) kDfLookahead=2 帧：处理帧 t 时，
    // df_delay_buf_ 中 delayed 项是帧 t-2 的 (coefs, gain, spec_m)，用
    // history[2..6] = [t-4, t-3, t-2, t-1, t] 做 non-causal 卷积
    // （window[2] = t-2 = 输出帧，spec_orig = window[2]）。
    // Spec6 T3：DelayedFrame + df_out + window 改预分配成员（零 per-frame
    // heap）。df_delay_buf_ 用 ring buffer + index 替代 deque<push/pop>。
    // Spec6 final review I2：df_spec_history_ 改 ring buffer，零 per-frame
    // heap。 back() = logical[kDfHistorySize-1] = phys[(head_+size-1)%size]。
    auto& hist_back =
        df_spec_history_[(df_spec_history_head_ + kDfHistorySize - 1) %
                         kDfHistorySize];
    for (size_t f = 0; f < kNbDf; ++f)
      hist_back[f] = spec_[f];
    // 写入 ring buffer 当前 slot（覆盖最旧）。
    DelayedFrame& slot = df_delay_ring_[df_delay_idx_];
    std::copy(coefs, coefs + kNbDf * kDfOrder * 2, slot.coefs.data());
    slot.gain = gain[0];
    std::copy(spec_m_.begin(), spec_m_.end(), slot.spec_m.begin());
    df_delay_idx_ = (df_delay_idx_ + 1) % (kDfLookahead + 1);
    ++df_delay_count_;
    // 如果 delay buf 满（> kDfLookahead），取最旧项做 non-causal 卷积。
    if (df_delay_count_ > kDfLookahead) {
      // 最旧项 = 当前写入位置（ring 已前移），即被覆盖前的 slot。
      const DelayedFrame& delayed_frame = df_delay_ring_[df_delay_idx_];
      --df_delay_count_;
      // history 当前布局（back() 已填 spec_）：
      //   [t-6, t-5, t-4, t-3, t-2, t-1, t]
      // non-causal window = history[2..6] = [t-4, t-3, t-2, t-1, t]
      // spec_orig = history[4] = t-2（输出帧的原始 spec，仅前 96 频点）
      // Spec6 final review I1：window_ptrs_ 改预分配成员数组（零 per-frame
      // heap）。logical[i] = phys[(head_+i)%size]。
      for (size_t o = 0; o < kDfOrder; ++o)
        window_ptrs_[o] =
            &df_spec_history_[(df_spec_history_head_ + 2 + o) % kDfHistorySize];
      // spec_e: bins 0..95 用深度滤波，96..481 用 ERB 掩蔽版（delayed
      // spec_m）。
      apply_df_op(window_ptrs_.data(), window_ptrs_.size(),
                  delayed_frame.coefs.data(), delayed_frame.gain,
                  *window_ptrs_[2], df_out_);
      for (size_t f = 0; f < kNbDf; ++f)
        spec_e_[f] = df_out_[f];
      // 余频点（96..481）用 delayed frame 的 ERB 掩蔽版。
      for (size_t f = kNbDf; f < kFreq; ++f)
        spec_e_[f] = delayed_frame.spec_m[f];
      // ── 7. 合成 ISTFT（libdf frame_synthesis）──
      // Spec6 T3：Irfft 改输出参数，time_block_ 预分配成员。
      fft::Irfft(spec_e_.data(), kFreq, kFft, time_block_.data(),
                 time_block_.size());
      for (size_t i = 0; i < kFft; ++i)
        time_block_[i] *= window_[i];
      // Spec6 T3 review Important #3/#4：out_frame 改预分配 ring buffer slot，
      // 零 per-frame heap。直接写入 out_ring_[tail_]，然后推进 tail。
      std::array<float, kHop>& out_slot = out_ring_[out_ring_tail_];
      for (size_t i = 0; i < kHop; ++i)
        out_slot[i] = time_block_[i] + synthesis_mem_[i];
      const size_t split = synthesis_mem_.size() - kHop;
      if (split == 0) {
        for (size_t i = 0; i < kHop; ++i)
          synthesis_mem_[i] = time_block_[kHop + i];
      } else {
        std::rotate(synthesis_mem_.begin(), synthesis_mem_.begin() + kHop,
                    synthesis_mem_.end());
        for (size_t i = 0; i < split; ++i)
          synthesis_mem_[i] += time_block_[kHop + i];
        for (size_t i = split; i < synthesis_mem_.size(); ++i)
          synthesis_mem_[i] = time_block_[kHop + i];
      }
      out_ring_tail_ = (out_ring_tail_ + 1) % kOutRingCap;
      ++out_ring_count_;
    }
    // Spec6 final review I2：ring buffer 推进 head（等价 erase front + push
    // back），零 per-frame heap。下帧 back() = 当前 logical[0]（被覆盖前的最旧
    // slot），写入前被覆盖，无需清零。
    df_spec_history_head_ = (df_spec_history_head_ + 1) % kDfHistorySize;
    return true;
  } catch (...) {
    // ONNX 失败：保持 history 与 delay_buf 同步（两者都不前进），匹配
    // pre-T2 行为（1b7bbd4）。non-causal window 在失败帧处保留旧 spec，
    // 不劣于 pre-T2 causal 同路径；failure 路径输出 orig（D-S5.5
    // passthrough，见 process() 输出阶段 effective_dry_wet=0）。
    return false;
  }
}

size_t DeepFilterNetAdapter::process(const float* in,
                                     size_t n_in,
                                     float* out,
                                     size_t n_out_max,
                                     DenoiseResult* result) {
  const float dry_wet =
      bits_to_float(dry_wet_bits_.load(std::memory_order_relaxed));

  if (!initialized_) {
    size_t n = std::min(n_in, n_out_max);
    for (size_t i = 0; i < n; ++i)
      out[i] = sanitize(in[i]);
    if (result) {
      result->status = ProcessStatus::kBypass;
      result->has_vad = false;
    }
    return n;
  }

  for (size_t i = 0; i < n_in; ++i)
    in_delay_.push_back(in[i]);
  for (size_t i = 0; i < n_in; ++i)
    in_fifo_.push_back(in[i]);

  bool failed = false;
  float lsnr = 0.0f;
  while (in_fifo_.size() >= kHop) {
    if (process_one_frame_(lsnr)) {
      continue;
    }
    failed = true;
    // D-S5.5：失败 hop 填 silence 占位（保持流率），输出阶段 dry_wet 降为
    // 0.0 使该段输出 = orig（in_delay_ 对齐），等价 memcpy passthrough。
    // Spec6 T3 review Important #4：passthrough 改预分配 ring slot（零 heap）。
    std::array<float, kHop>& passthrough_slot = out_ring_[out_ring_tail_];
    passthrough_slot.fill(0.0f);
    out_ring_tail_ = (out_ring_tail_ + 1) % kOutRingCap;
    ++out_ring_count_;
  }

  // Spec6 T2：non-causal deep-filter 的 lookahead 延迟由 df_delay_buf_ 提供
  // （process_one_frame_ 中 size > kDfLookahead 才产出），无需额外的
  // out_ring_ warmup。总算法延迟 = STFT overlap (1 hop) + df_delay_buf
  // lookahead (2 hops) = 3 hops = 1440 samples（与 algorithmic_latency_samples
  // 一致）。
  // Spec6 T3 review Important #4：drain ring buffer -> out_fifo_（零 heap）。
  while (out_ring_count_ > 0) {
    const std::array<float, kHop>& frame = out_ring_[out_ring_head_];
    for (size_t i = 0; i < kHop; ++i)
      out_fifo_.push_back(frame[i]);
    out_ring_head_ = (out_ring_head_ + 1) % kOutRingCap;
    --out_ring_count_;
  }

  // D-S5.5：ONNX 失败时 dry_wet 降为 0.0，输出 = orig（memcpy passthrough）。
  // in_delay_ 与 out_fifo_ 同步 pop（算法延迟对齐），失败 hop 的 silence
  // 对应位置的 in_delay_ 样本即原始输入，混合系数归零即等价 memcpy
  // passthrough。
  const float effective_dry_wet = failed ? 0.0f : dry_wet;
  size_t n_out = std::min(out_fifo_.size(), n_out_max);
  for (size_t i = 0; i < n_out; ++i) {
    float denoised = out_fifo_.front();
    out_fifo_.pop_front();
    float orig = 0.0f;
    if (!in_delay_.empty()) {
      orig = in_delay_.front();
      in_delay_.pop_front();
    }
    out[i] = sanitize(effective_dry_wet * denoised +
                      (1.0f - effective_dry_wet) * orig);
  }

  if (result) {
    result->status = failed ? ProcessStatus::kBypass : ProcessStatus::kOk;
    result->has_vad = false;
    result->has_snr = true;
    result->estimated_snr_db = lsnr;
  }
  return n_out;
}

size_t DeepFilterNetAdapter::flush(float* out, size_t n_out_max) {
  // lookahead=2：补 kDfLookahead 帧零样本触发残余输出。
  // Spec6 T3 review Important #3：zero 改栈数组（零 heap）。
  for (size_t i = 0; i < kDfLookahead; ++i) {
    for (size_t j = 0; j < kHop; ++j)
      in_fifo_.push_back(0.0f);
    float lsnr = 0.0f;
    if (!process_one_frame_(lsnr)) {
      break;
    }
  }
  // 排空 ring buffer -> out_fifo_（Spec6 T3 review Important #4：零 heap）。
  while (out_ring_count_ > 0) {
    const std::array<float, kHop>& frame = out_ring_[out_ring_head_];
    for (size_t i = 0; i < kHop; ++i)
      out_fifo_.push_back(frame[i]);
    out_ring_head_ = (out_ring_head_ + 1) % kOutRingCap;
    --out_ring_count_;
  }
  const float dry_wet =
      bits_to_float(dry_wet_bits_.load(std::memory_order_relaxed));
  size_t n_out = std::min(out_fifo_.size(), n_out_max);
  for (size_t i = 0; i < n_out; ++i) {
    float denoised = out_fifo_.front();
    out_fifo_.pop_front();
    float orig = 0.0f;
    if (!in_delay_.empty()) {
      orig = in_delay_.front();
      in_delay_.pop_front();
    }
    out[i] = sanitize(dry_wet * denoised + (1.0f - dry_wet) * orig);
  }
  return n_out;
}

void DeepFilterNetAdapter::set_dry_wet(float ratio) {
  if (ratio < 0.0f)
    ratio = 0.0f;
  if (ratio > 1.0f)
    ratio = 1.0f;
  dry_wet_bits_.store(float_to_bits(ratio), std::memory_order_relaxed);
}

bool DeepFilterNetAdapter::set_param(const std::string& key,
                                     const std::string& value) {
  // postfilter 等 model-specific 参数（§4.4），简化版暂不接受。
  (void)key;
  (void)value;
  return false;
}

std::string DeepFilterNetAdapter::get_param(const std::string& /*key*/) const {
  return "";
}

// Spec6 T2（D-S6.6）：DFN deep-filter 操作（non-causal + alpha blend）。
// 实现 DfOp + assign_df（对照 DeepFilterNet/df/modules.py）：
//   - non-causal window [i-2..i+2]（5 帧，lookahead=2）
//   - coef[0] 配最旧帧（window[0]），coef[4] 配最新帧（window[4]）
//   - alpha blend: spec_f = spec_f*alpha + spec_orig*(1-alpha)
//     alpha = clamp(gain_alpha, 0, 1)
// 此静态方法为纯函数（无实例状态），供 process_one_frame_ 内部调用 +
// 单元测试直接验证卷积逻辑（无需 ONNX 模型）。
// Spec6 T3：window 改为 const std::vector<Complex>* 指针数组（避免拷贝
// vector）， spec_out 改为引用（调用者预分配）。
// Spec6 final review I1：签名改为裸指针+size（零 per-frame heap，兼容
// std::array 成员与测试侧 std::vector 两种调用路径）。
void DeepFilterNetAdapter::apply_df_op(
    const std::vector<fft::Complex>* const* window,
    size_t window_size,
    const float* coefs,
    float gain_alpha,
    const std::vector<fft::Complex>& spec_orig,
    std::vector<fft::Complex>& spec_out) {
  // alpha = clamp(gain, 0, 1)（assign_df alpha blend）。
  const float alpha = (gain_alpha < 0.0f)   ? 0.0f
                      : (gain_alpha > 1.0f) ? 1.0f
                                            : gain_alpha;
  // Spec6 T3 review Important #5：生产路径调用者（process_one_frame_）在 init()
  // 中预分配 df_out_（resize(kNbDf)），此处 fill 即可，零 realloc。测试路径
  // 传入空 vector -> resize 一次（测试非 RT，可接受）。
  if (spec_out.size() != kNbDf)
    spec_out.resize(kNbDf);
  std::fill(spec_out.begin(), spec_out.end(), fft::Complex(0.0f, 0.0f));
  for (size_t f = 0; f < kNbDf; ++f) {
    // non-causal 卷积：sum_o coefs[o] * window[o][f]
    // o=0 配 window[0]（最旧 = i-2），o=4 配 window[4]（最新 = i+2）。
    float re_out = 0.0f, im_out = 0.0f;
    for (size_t o = 0; o < kDfOrder; ++o) {
      const float cr = coefs[f * 10 + o * 2 + 0];
      const float ci = coefs[f * 10 + o * 2 + 1];
      const auto& s = (*window[o])[f];  // o=0=oldest
      // 复乘 (cr+i*ci)*(sr+i*si) = (cr*sr-ci*si) + i*(cr*si+ci*sr)
      re_out += cr * s.real() - ci * s.imag();
      im_out += cr * s.imag() + ci * s.real();
    }
    // assign_df alpha blend: spec_f = spec_f*alpha + spec*(1-alpha)。
    const fft::Complex spec_f(re_out, im_out);
    spec_out[f] = spec_f * alpha + spec_orig[f] * (1.0f - alpha);
  }
}

}  // namespace noise

// 静态注册：进程加载时把 "deepfilternet" 注册进 DenoisePluginRegistry。
static bool registered = [] {
  noise::DenoisePluginRegistry::instance().register_plugin(
      "deepfilternet", []() -> std::unique_ptr<noise::IDenoisePlugin> {
        return std::make_unique<noise::DeepFilterNetAdapter>();
      });
  return true;
}();

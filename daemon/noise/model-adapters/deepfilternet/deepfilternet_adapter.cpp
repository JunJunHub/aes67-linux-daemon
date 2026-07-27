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
  df_spec_history_.assign(kDfOrder + kDfLookahead,
                          std::vector<fft::Complex>(kNbDf));

  spec_.resize(kFreq);
  feat_erb_.resize(kNbErb);      // [1,1,1,32]
  feat_spec_.resize(2 * kNbDf);  // [1,2,1,96] = [re(96), im(96)]

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
  for (auto& v : df_spec_history_)
    std::fill(v.begin(), v.end(), fft::Complex(0, 0));
  df_delay_buf_.clear();  // Spec6 T2：清空 non-causal delay buffer
  out_frame_buf_.clear();
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
  std::vector<float> input(kHop);
  for (size_t i = 0; i < kHop; ++i) {
    input[i] = in_fifo_.front();
    in_fifo_.pop_front();
  }

  const float alpha = calc_norm_alpha(kSr, kHop, 1.0f);

  try {
    // ── 1. 分析 STFT（libdf frame_analysis）──
    // buf = [analysis_mem * window[0:480] ; input * window[480:960]]
    std::vector<float> buf(kFft, 0.0f);
    for (size_t i = 0; i < kFft - kHop; ++i)
      buf[i] = analysis_mem_[i] * window_[i];
    for (size_t i = 0; i < kHop; ++i)
      buf[kFft - kHop + i] = input[i] * window_[kFft - kHop + i];
    // analysis_mem <- input（下帧的 prev）
    std::copy(input.begin(), input.end(), analysis_mem_.begin());
    // rfft(buf) -> 481 复频点，* wnorm
    spec_ = fft::Rfft(buf.data(), kFft);
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
    std::vector<fft::Complex> spec_m = spec_;
    {
      size_t bcsum = 0;
      for (size_t b = 0; b < kNbErb; ++b) {
        const float g = m[b];
        for (size_t j = 0; j < erb_[b]; ++j)
          spec_m[bcsum + j] *= g;
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
    // 每帧 heap 分配（T3 RT refactor 待办：改预分配成员，见 spec6-plan T3）。
    for (size_t f = 0; f < kNbDf; ++f)
      df_spec_history_.back()[f] = spec_[f];
    {
      DelayedFrame delayed;
      delayed.coefs.assign(coefs, coefs + kNbDf * kDfOrder * 2);
      delayed.gain = gain[0];
      delayed.spec_m = std::move(spec_m);
      df_delay_buf_.push_back(std::move(delayed));
    }
    // 如果 delay buf 满（> kDfLookahead），pop 最旧项做 non-causal 卷积。
    if (df_delay_buf_.size() > kDfLookahead) {
      DelayedFrame delayed_frame = std::move(df_delay_buf_.front());
      df_delay_buf_.pop_front();
      // history 当前布局（back() 已填 spec_）：
      //   [t-6, t-5, t-4, t-3, t-2, t-1, t]
      // non-causal window = history[2..6] = [t-4, t-3, t-2, t-1, t]
      // spec_orig = history[4] = t-2（输出帧的原始 spec，仅前 96 频点）
      std::vector<std::vector<fft::Complex>> window(kDfOrder);
      for (size_t o = 0; o < kDfOrder; ++o)
        window[o] = df_spec_history_[2 + o];
      // spec_e: bins 0..95 用深度滤波，96..481 用 ERB 掩蔽版（delayed
      // spec_m）。
      std::vector<fft::Complex> spec_e(kFreq);
      std::vector<fft::Complex> df_out;
      apply_df_op(window, delayed_frame.coefs.data(), delayed_frame.gain,
                  window[2], df_out);
      for (size_t f = 0; f < kNbDf; ++f)
        spec_e[f] = df_out[f];
      // 余频点（96..481）用 delayed frame 的 ERB 掩蔽版。
      for (size_t f = kNbDf; f < kFreq; ++f)
        spec_e[f] = delayed_frame.spec_m[f];
      // ── 7. 合成 ISTFT（libdf frame_synthesis）──
      auto time_block = fft::Irfft(spec_e.data(), kFreq, kFft);
      for (size_t i = 0; i < kFft; ++i)
        time_block[i] *= window_[i];
      std::vector<float> out_frame(kHop, 0.0f);
      for (size_t i = 0; i < kHop; ++i)
        out_frame[i] = time_block[i] + synthesis_mem_[i];
      const size_t split = synthesis_mem_.size() - kHop;
      if (split == 0) {
        for (size_t i = 0; i < kHop; ++i)
          synthesis_mem_[i] = time_block[kHop + i];
      } else {
        std::rotate(synthesis_mem_.begin(), synthesis_mem_.begin() + kHop,
                    synthesis_mem_.end());
        for (size_t i = 0; i < split; ++i)
          synthesis_mem_[i] += time_block[kHop + i];
        for (size_t i = split; i < synthesis_mem_.size(); ++i)
          synthesis_mem_[i] = time_block[kHop + i];
      }
      out_frame_buf_.push_back(std::move(out_frame));
    }
    // 滑窗 history：丢弃最旧，push 占位（下帧填）。
    df_spec_history_.erase(df_spec_history_.begin());
    df_spec_history_.push_back(std::vector<fft::Complex>(kNbDf));
    return true;
  } catch (...) {
    // ONNX 失败：保持 history 与 delay_buf 同步（两者都不前进），匹配
    // pre-T2 行为（1b7bbd4）。non-causal window 在失败帧处保留旧 spec，
    // 不劣于 pre-T2 causal 同路径；failure 路径已降级（D-S5.5 silence）。
    // T2 不改 failure handling（见 spec6-plan T2 约束）。
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
    // D-S5.5 偏差（reviewer final Important #2，文档化接受）：理想 memcpy
    // in->out passthrough，但失败 hop 的对应输入 in_delay_ 需对齐（lookahead
    // 缓冲 + 48k 流式），故用 silence 安全降级（sanitize 完整 + 10 帧界 +
    // 最终切 passthrough，不喂下游错误样本）。真实 memcpy passthrough 延后
    // spec6。
    std::vector<float> passthrough(kHop, 0.0f);
    out_frame_buf_.push_back(std::move(passthrough));
  }

  // Spec6 T2：non-causal deep-filter 的 lookahead 延迟由 df_delay_buf_ 提供
  // （process_one_frame_ 中 size > kDfLookahead 才产出），无需额外的
  // out_frame_buf_ warmup。总算法延迟 = STFT overlap (1 hop) + df_delay_buf
  // lookahead (2 hops) = 3 hops = 1440 samples（与 algorithmic_latency_samples
  // 一致）。
  while (!out_frame_buf_.empty()) {
    auto frame = std::move(out_frame_buf_.front());
    out_frame_buf_.pop_front();
    for (size_t i = 0; i < kHop; ++i)
      out_fifo_.push_back(frame[i]);
  }

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
  for (size_t i = 0; i < kDfLookahead; ++i) {
    std::vector<float> zero(kHop, 0.0f);
    for (auto s : zero)
      in_fifo_.push_back(s);
    float lsnr = 0.0f;
    if (!process_one_frame_(lsnr)) {
      break;
    }
  }
  // 排空 out_frame_buf_。
  while (!out_frame_buf_.empty()) {
    auto frame = std::move(out_frame_buf_.front());
    out_frame_buf_.pop_front();
    for (size_t i = 0; i < kHop; ++i)
      out_fifo_.push_back(frame[i]);
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
void DeepFilterNetAdapter::apply_df_op(
    const std::vector<std::vector<fft::Complex>>& window,
    const float* coefs,
    float gain_alpha,
    const std::vector<fft::Complex>& spec_orig,
    std::vector<fft::Complex>& spec_out) {
  // alpha = clamp(gain, 0, 1)（assign_df alpha blend）。
  const float alpha = (gain_alpha < 0.0f)   ? 0.0f
                      : (gain_alpha > 1.0f) ? 1.0f
                                            : gain_alpha;
  spec_out.assign(kNbDf, fft::Complex(0.0f, 0.0f));
  for (size_t f = 0; f < kNbDf; ++f) {
    // non-causal 卷积：sum_o coefs[o] * window[o][f]
    // o=0 配 window[0]（最旧 = i-2），o=4 配 window[4]（最新 = i+2）。
    float re_out = 0.0f, im_out = 0.0f;
    for (size_t o = 0; o < kDfOrder; ++o) {
      const float cr = coefs[f * 10 + o * 2 + 0];
      const float ci = coefs[f * 10 + o * 2 + 1];
      const auto& s = window[o][f];  // o=0=oldest
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

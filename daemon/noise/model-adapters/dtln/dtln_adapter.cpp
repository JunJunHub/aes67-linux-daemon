// daemon/noise/model-adapters/dtln/dtln_adapter.cpp
// DTLN 适配器实现 + 静态注册到 DenoisePluginRegistry（"dtln"）。
// 架构依据：docs/noise/denoise-plugin-architecture.md §3.2。
// 算法对齐：breizhn/DTLN/real_time_processing_onnx.py（实测模型签名见 hpp）。
#include "dtln_adapter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
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

// model_1.onnx -> model_2.onnx：".onnx" 前最后一个字符若为 '1' 换 '2'。
// 匹配 DTLN 约定（model_N.onnx）。
std::string derive_sibling_model(const std::string& path) {
  auto dot = path.find_last_of('.');
  if (dot == std::string::npos || dot == 0)
    return path;
  if (path[dot - 1] == '1') {
    std::string s = path;
    s[dot - 1] = '2';
    return s;
  }
  return path;
}

}  // namespace

DtlnAdapter::DtlnAdapter() = default;
DtlnAdapter::~DtlnAdapter() = default;

bool DtlnAdapter::init(const PluginConfig& cfg) {
  if (initialized_)
    return false;

  in_rate_ = cfg.sample_rate_in ? cfg.sample_rate_in : 48000;

  // 模型路径：cfg.model_path 优先（指向 model_1.onnx）；否则用 onnx_model_dir
  // 推导（<dir>/model_1.onnx）。DTLN 无内置默认模型，路径为空时 init 失败
  // -> DenoiseProcessor 切默认插件（passthrough）。
  std::string m1 = cfg.model_path;
  if (m1.empty()) {
    if (cfg.onnx_model_dir.empty())
      return false;
    m1 = cfg.onnx_model_dir;
    if (!m1.empty() && m1.back() != '/' && m1.back() != '\\')
      m1 += '/';
    m1 += "model_1.onnx";
  }
  const std::string m2 = derive_sibling_model(m1);

  sess1_ = CreateOnnxSession(m1);
  sess2_ = CreateOnnxSession(m2);
  if (!sess1_ || !sess2_)
    return false;  // 模型加载失败（路径错/文件损坏/OOM）

  // 缓存 I/O 名字（按 index：避免每帧 OnnxInputName 返回临时 string 的悬垂）。
  m1_in0_ = OnnxInputName(*sess1_, 0);    // mag
  m1_in1_ = OnnxInputName(*sess1_, 1);    // state1
  m1_out0_ = OnnxOutputName(*sess1_, 0);  // mask
  m1_out1_ = OnnxOutputName(*sess1_, 1);  // new_state1
  m2_in0_ = OnnxInputName(*sess2_, 0);    // time block
  m2_in1_ = OnnxInputName(*sess2_, 1);    // state2
  m2_out0_ = OnnxOutputName(*sess2_, 0);  // enhanced
  m2_out1_ = OnnxOutputName(*sess2_, 1);  // new_state2
  if (m1_in0_.empty() || m1_in1_.empty() || m2_in0_.empty() || m2_in1_.empty())
    return false;  // 签名不符

  // 预分配 LSTM 状态后端存储（[1,2,128,2]=512 float，零初始化）。
  state1_data_.assign(512, 0.0f);
  state2_data_.assign(512, 0.0f);

  // 预分配 FFT/重采样暂存容量（运行时复用，resize 不超 capacity -> 无
  // realloc）。
  spec_.resize(kFrame / 2 + 1);   // 257
  mag_.resize(kFrame / 2 + 1);    // 257
  phase_.resize(kFrame / 2 + 1);  // 257
  est_block_.resize(kFrame);      // 512
  down_scratch_.reserve(kFrame * 2);
  up_scratch_.reserve(kFrame * 2);

  // 重采样器：48k↔16k（复用 T1 Resampler 原语）。
  down_ = std::make_unique<Resampler>(in_rate_, 16000);
  up_ = std::make_unique<Resampler>(16000, in_rate_);

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

void DtlnAdapter::reset() {
  // 清空 LSTM 状态 + 缓冲（切换源时调用，§2.2 reset）。session 保留。
  std::fill(state1_data_.begin(), state1_data_.end(), 0.0f);
  std::fill(state2_data_.begin(), state2_data_.end(), 0.0f);
  std::memset(frame_buffer_, 0, sizeof(frame_buffer_));
  std::memset(out_buffer_, 0, sizeof(out_buffer_));
  in_fifo16_.clear();
  out_fifo16_.clear();
  out_fifo48_.clear();
  in_delay48_.clear();
}

const char* DtlnAdapter::name() const {
  return "dtln";
}
uint32_t DtlnAdapter::native_sample_rate() const {
  return 16000;
}

uint32_t DtlnAdapter::algorithmic_latency_samples() const {
  // DTLN 算法延迟 = 1 帧 @16k = 512 样本 = 32ms。换算到输入域（48k）：
  // 512*(48000/16000)=1536。重采样器额外 <<2ms（arch §3.1）未折入
  // （与 T1 Resampler::output_latency 延后处理一致）。kConvergenceMargin
  // (50ms) 覆盖此未折入量，保守不致欠报。
  return 1536;
}
bool DtlnAdapter::supports_vad() const {
  return false;
}
bool DtlnAdapter::supports_snr() const {
  return false;
}

bool DtlnAdapter::process_one_frame_() {
  // 从 in_fifo16_ 取 128 样本，填 frame_buffer_ 尾部（滑窗左移 128 + 追加）。
  // 参考：in_buffer[:-128] = in_buffer[128:]; in_buffer[-128:] = new。
  std::memmove(frame_buffer_, frame_buffer_ + kHop,
               (kFrame - kHop) * sizeof(float));
  for (size_t i = 0; i < kHop; ++i) {
    frame_buffer_[kFrame - kHop + i] = in_fifo16_.front();
    in_fifo16_.pop_front();
  }

  // rfft(frame_buffer_) -> 257 复频点；mag=|·|, phase=∠·。
  // 无归一化/缩放，对齐参考脚本（DTLN 训练于 [-1,1] 音频的原始幅度谱）。
  spec_ = fft::Rfft(frame_buffer_, kFrame);
  for (size_t k = 0; k < spec_.size(); ++k) {
    mag_[k] = std::abs(spec_[k]);
    phase_[k] = std::arg(spec_[k]);
  }

  const Ort::MemoryInfo& mi = OnnxMemoryInfo();
  try {
    // ── model_1：in=[mag(1,1,257), state1(1,2,128,2)] -> [mask, new_state1] ──
    int64_t mag_shape[] = {1, 1, static_cast<int64_t>(spec_.size())};
    int64_t state_shape[] = {1, 2, 128, 2};
    Ort::Value in_mag = Ort::Value::CreateTensor<float>(
        mi, mag_.data(), mag_.size(), mag_shape, 3);
    Ort::Value in_state1 = Ort::Value::CreateTensor<float>(
        mi, state1_data_.data(), state1_data_.size(), state_shape, 4);
    const char* in1_names[2] = {m1_in0_.c_str(), m1_in1_.c_str()};
    const char* out1_names[2] = {m1_out0_.c_str(), m1_out1_.c_str()};
    Ort::Value inputs1[2] = {std::move(in_mag), std::move(in_state1)};
    auto out1 = sess1_->Run(Ort::RunOptions{nullptr}, in1_names, inputs1, 2,
                            out1_names, 2);
    const float* mask_ptr = out1[0].GetTensorMutableData<float>();
    const float* new_state1 = out1[1].GetTensorMutableData<float>();
    // 状态喂回：拷回稳定后端存储，供下帧创建输入张量引用。
    std::memcpy(state1_data_.data(), new_state1,
                state1_data_.size() * sizeof(float));

    // 估计复谱 = in_mag * mask * exp(i*phase)（保相位，掩蔽幅度）。
    for (size_t k = 0; k < spec_.size(); ++k) {
      const float m = mag_[k] * mask_ptr[k];
      const float c = std::cos(phase_[k]);
      const float s = std::sin(phase_[k]);
      spec_[k] = std::complex<float>(m * c, m * s);
    }
    // irfft(spec, 512) -> 时域块。
    auto block = fft::Irfft(spec_.data(), spec_.size(), kFrame);
    std::copy(block.begin(), block.end(), est_block_.begin());

    // ── model_2：in=[time(1,1,512), state2(1,2,128,2)] -> [enhanced,
    // new_state2] ──
    int64_t time_shape[] = {1, 1, static_cast<int64_t>(kFrame)};
    Ort::Value in_time = Ort::Value::CreateTensor<float>(
        mi, est_block_.data(), est_block_.size(), time_shape, 3);
    Ort::Value in_state2 = Ort::Value::CreateTensor<float>(
        mi, state2_data_.data(), state2_data_.size(), state_shape, 4);
    const char* in2_names[2] = {m2_in0_.c_str(), m2_in1_.c_str()};
    const char* out2_names[2] = {m2_out0_.c_str(), m2_out1_.c_str()};
    Ort::Value inputs2[2] = {std::move(in_time), std::move(in_state2)};
    auto out2 = sess2_->Run(Ort::RunOptions{nullptr}, in2_names, inputs2, 2,
                            out2_names, 2);
    const float* enhanced = out2[0].GetTensorMutableData<float>();
    const float* new_state2 = out2[1].GetTensorMutableData<float>();
    std::memcpy(state2_data_.data(), new_state2,
                state2_data_.size() * sizeof(float));

    // overlap-add：out_buffer 左移 128 + 尾部清零 + += enhanced(512)。
    // 参考：out_buffer[:-128]=out_buffer[128:]; out_buffer[-128:]=0;
    // out_buffer+=out_block。
    std::memmove(out_buffer_, out_buffer_ + kHop,
                 (kFrame - kHop) * sizeof(float));
    std::memset(out_buffer_ + kFrame - kHop, 0, kHop * sizeof(float));
    for (size_t i = 0; i < kFrame; ++i)
      out_buffer_[i] += enhanced[i];

    // 取前 128 输出 -> out_fifo16_。
    for (size_t i = 0; i < kHop; ++i)
      out_fifo16_.push_back(out_buffer_[i]);
    return true;
  } catch (...) {
    // RT 契约：Run 异常不抛出，调用方置 kBypass + 直通。
    return false;
  }
}

size_t DtlnAdapter::process(const float* in,
                            size_t n_in,
                            float* out,
                            size_t n_out_max,
                            DenoiseResult* result) {
  const float dry_wet =
      bits_to_float(dry_wet_bits_.load(std::memory_order_relaxed));

  if (!initialized_) {
    // 未 init：直通 in->out，置 kBypass（§2.2 错误降级契约 #1）。
    size_t n = std::min(n_in, n_out_max);
    for (size_t i = 0; i < n; ++i)
      out[i] = sanitize(in[i]);
    if (result) {
      result->status = ProcessStatus::kBypass;
      result->has_vad = false;
    }
    return n;
  }

  // 1. 输入入延迟线（dry_wet 混合对齐：denoised 相对输入有算法延迟，
  //    干路需延迟同等深度）。
  for (size_t i = 0; i < n_in; ++i)
    in_delay48_.push_back(in[i]);

  // 2. 48k -> 16k 重采样，入 in_fifo16_。
  const size_t down_cap = down_->max_output_for_input(n_in);
  if (down_scratch_.size() < down_cap)
    down_scratch_.resize(down_cap);
  const size_t n16 =
      down_->process(in, n_in, down_scratch_.data(), down_scratch_.size());
  for (size_t i = 0; i < n16; ++i)
    in_fifo16_.push_back(down_scratch_[i]);

  // 3. 帧化处理：每凑满 hop=128 跑一帧。
  bool failed = false;
  while (in_fifo16_.size() >= kHop) {
    if (process_one_frame_()) {
      continue;
    }
    // Run 失败：直通本 hop 的 128 输入样本到 out_fifo16_（保持流率对齐），
    // 置 kBypass。DenoiseProcessor 计连续 kBypass -> kError -> 切 passthrough。
    failed = true;
    for (size_t i = 0; i < kHop && !in_fifo16_.empty(); ++i) {
      // 该 hop 已被 process_one_frame_ 消费填入 frame_buffer_，但失败时
      // frame_buffer 的内容无效；用对应输入样本直通（从 in_delay48 间接
      // 不可得 @16k，这里近似用 0 填充：失败帧静音，避免喂下游错误样本）。
      out_fifo16_.push_back(0.0f);
    }
  }

  // 4. 16k -> 48k 重采样，入 out_fifo48_。
  if (!out_fifo16_.empty()) {
    const size_t up_cap = up_->max_output_for_input(out_fifo16_.size());
    if (up_scratch_.size() < up_cap)
      up_scratch_.resize(up_cap);
    // up_->process 需连续缓冲；deque 非连续，先拷到连续缓冲。
    std::vector<float> contig(out_fifo16_.begin(), out_fifo16_.end());
    out_fifo16_.clear();
    const size_t n48 = up_->process(contig.data(), contig.size(),
                                    up_scratch_.data(), up_scratch_.size());
    for (size_t i = 0; i < n48; ++i)
      out_fifo48_.push_back(up_scratch_[i]);
  }

  // 5. 输出：取 min(out_fifo48_.size(), n_out_max)，dry_wet 混合 + sanitize。
  size_t n_out = std::min(out_fifo48_.size(), n_out_max);
  for (size_t i = 0; i < n_out; ++i) {
    float denoised = out_fifo48_.front();
    out_fifo48_.pop_front();
    float orig = 0.0f;
    if (!in_delay48_.empty()) {
      orig = in_delay48_.front();
      in_delay48_.pop_front();
    }
    float s = dry_wet * denoised + (1.0f - dry_wet) * orig;
    out[i] = sanitize(s);
  }

  if (result) {
    result->status = failed ? ProcessStatus::kBypass : ProcessStatus::kOk;
    result->has_vad = false;  // DTLN 不产 VAD
  }
  return n_out;
}

size_t DtlnAdapter::flush(float* out, size_t n_out_max) {
  // DTLN 无 lookahead（df_lookahead=0），残余 = out_fifo48_ 中未取出的样本
  // （算法延迟期积累的输出）。process 已把可用产出存入 out_fifo48_，flush
  // 取出残余。无需补零触发（DTLN 无前视依赖）。
  const float dry_wet =
      bits_to_float(dry_wet_bits_.load(std::memory_order_relaxed));
  size_t n_out = std::min(out_fifo48_.size(), n_out_max);
  for (size_t i = 0; i < n_out; ++i) {
    float denoised = out_fifo48_.front();
    out_fifo48_.pop_front();
    float orig = 0.0f;
    if (!in_delay48_.empty()) {
      orig = in_delay48_.front();
      in_delay48_.pop_front();
    }
    out[i] = sanitize(dry_wet * denoised + (1.0f - dry_wet) * orig);
  }
  return n_out;
}

void DtlnAdapter::set_dry_wet(float ratio) {
  if (ratio < 0.0f)
    ratio = 0.0f;
  if (ratio > 1.0f)
    ratio = 1.0f;
  dry_wet_bits_.store(float_to_bits(ratio), std::memory_order_relaxed);
}

bool DtlnAdapter::set_param(const std::string& /*key*/,
                            const std::string& /*value*/) {
  return false;  // DTLN 当前不支持运行时参数
}

std::string DtlnAdapter::get_param(const std::string& /*key*/) const {
  return "";
}

}  // namespace noise

// 静态注册：进程加载时把 "dtln" 注册进 DenoisePluginRegistry 单例。
// 该 TU 须链入 noise 库（CMakeLists.txt 中 dtln_adapter.cpp 加入 NOISE_SOURCES
// 且 NOISE_PLUGIN_DTLN=ON），否则 switch_plugin("dtln") 失败。
static bool registered = [] {
  noise::DenoisePluginRegistry::instance().register_plugin(
      "dtln", []() -> std::unique_ptr<noise::IDenoisePlugin> {
        return std::make_unique<noise::DtlnAdapter>();
      });
  return true;
}();

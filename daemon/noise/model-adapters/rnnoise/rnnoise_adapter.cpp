// daemon/noise/model-adapters/rnnoise/rnnoise_adapter.cpp
// RNNoise 适配器实现 + 静态注册到 DenoisePluginRegistry（"rnnoise"）。
// 架构依据：docs/noise/denoise-plugin-architecture.md §3.1。
#include "rnnoise_adapter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "denoise_plugin_factory.hpp"

namespace noise {

namespace {

// 将 float bit-cast 为 uint32_t（C++17 无 std::bit_cast,用 memcpy）。
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

// 数值清洗：clamp [-1, 1],NaN/Inf -> 0（§2.2 实时安全契约 #2）。
inline float sanitize(float s) noexcept {
  if (!std::isfinite(s))
    return 0.0f;
  if (s > 1.0f)
    return 1.0f;
  if (s < -1.0f)
    return -1.0f;
  return s;
}

}  // namespace

bool RnnoiseAdapter::init(const PluginConfig& cfg) {
  // 1. 加载模型：cfg.model_path 空 = 内置默认模型（rnnoise_create(nullptr)）;
  //    非空 = 从文件加载自定义模型。RAII：model_ 持有 RNNModel* 析构调
  //    rnnoise_model_free;state_ 持有 DenoiseState* 析构调 rnnoise_destroy。
  //    任何一步失败已分配资源由 unique_ptr 析构自动释放,无部分 init 泄漏。
  if (state_)
    return false;  // 已 init,不允许重复（reset() 显式清空再 init）

  if (cfg.model_path.empty()) {
    state_ = StatePtr(rnnoise_create(nullptr));
  } else {
    // HEAD (70f1d25) API：rnnoise_model_from_filename(const char*)。
    // v0.1.1 用 rnnoise_model_from_file(FILE*),本实现使用 HEAD API。
    RNNModel* raw_model = rnnoise_model_from_filename(cfg.model_path.c_str());
    if (!raw_model)
      return false;  // 模型文件加载失败 -> DenoiseProcessor 切默认插件
    model_ = ModelPtr(raw_model);
    state_ = StatePtr(rnnoise_create(model_.get()));
  }
  if (!state_)
    return false;  // rnnoise_create 失败（OOM 等）

  // 2. dry_wet 初始值（cfg.dry_wet 默认 1.0 = 全降噪）。
  float dw = cfg.dry_wet;
  if (dw < 0.0f)
    dw = 0.0f;
  if (dw > 1.0f)
    dw = 1.0f;
  dry_wet_bits_.store(float_to_bits(dw), std::memory_order_relaxed);

  // 3. 重置内部缓冲状态。
  in_buffer_count_ = 0;
  std::memset(in_buffer_, 0, sizeof(in_buffer_));

  return true;
}

void RnnoiseAdapter::reset() {
  // 销毁并重建 state 以清空 RNNoise 内部 IIR/递归状态。
  // model_ 保留（文件已加载,无需重读）。
  if (model_) {
    state_ = StatePtr(rnnoise_create(model_.get()));
  } else {
    state_ = StatePtr(rnnoise_create(nullptr));
  }
  in_buffer_count_ = 0;
  std::memset(in_buffer_, 0, sizeof(in_buffer_));
}

const char* RnnoiseAdapter::name() const {
  return "rnnoise";
}

uint32_t RnnoiseAdapter::native_sample_rate() const {
  return 48000;  // RNNoise 固定 48kHz
}

uint32_t RnnoiseAdapter::algorithmic_latency_samples() const {
  return 480;  // frame=hop=480,无 overlap-add,延迟 = 1 帧
}

bool RnnoiseAdapter::supports_vad() const {
  return true;  // RNNoise 产 VAD 概率
}

bool RnnoiseAdapter::supports_snr() const {
  return false;  // RNNoise 不产 SNR 估计
}

size_t RnnoiseAdapter::process(const float* in,
                               size_t n_in,
                               float* out,
                               size_t n_out_max,
                               DenoiseResult* result) {
  // 错误降级契约（§2.2 #1）：未 init / state 损坏时不抛异常,
  // 直通 in 到 out 并置 kBypass。
  if (!state_) {
    size_t n = std::min(n_in, n_out_max);
    for (size_t i = 0; i < n; ++i)
      out[i] = sanitize(in[i]);
    if (result) {
      result->status = ProcessStatus::kBypass;
      result->has_vad = false;
    }
    return n;
  }

  // 读取 dry_wet 原子快照（RT 线程,无锁）。
  const float dry_wet =
      bits_to_float(dry_wet_bits_.load(std::memory_order_relaxed));

  // 处理路径：累积输入到 480 样本帧,凑满一帧就调 rnnoise_process_frame。
  // RNNoise frame=hop=480（无 overlap）,每 480 输入 -> 480 输出。
  // 简化：本实现支持任意 n_in,内部循环处理 480 样本块;
  // 不足 480 的尾部累积到 in_buffer_,下次调用补齐（流式收敛保证）。
  //
  // CRITICAL：RNNoise 内部模型按 16-bit PCM 量级训练（参考
  // examples/rnnoise_demo.c 的 short->float 直接赋值,值域 [-32768,
  // 32767]）。输入 [-1,1] 量级信号时 模型几乎不降噪（实测 0.24dB）,16-bit
  // 量级降噪 55+dB。故 adapter 必须 在调用 rnnoise_process_frame 前 *32768
  // 缩放,返回后 /32768 还原。
  constexpr float kPcmScale = 32768.0f;
  size_t out_pos = 0;
  size_t in_pos = 0;

  // 先消费 in_buffer_ 中累积的剩余样本（如果有）。
  if (in_buffer_count_ > 0) {
    size_t need = kFrameSize - in_buffer_count_;
    size_t take = std::min(need, n_in);
    std::memcpy(in_buffer_ + in_buffer_count_, in, take * sizeof(float));
    in_buffer_count_ += take;
    in_pos = take;
    if (in_buffer_count_ == kFrameSize) {
      // 缓冲满,处理一帧。
      if (out_pos + kFrameSize <= n_out_max) {
        float scaled_in[kFrameSize];
        float frame_out[kFrameSize];
        for (size_t i = 0; i < kFrameSize; ++i)
          scaled_in[i] = in_buffer_[i] * kPcmScale;
        float vad = rnnoise_process_frame(state_.get(), frame_out, scaled_in);
        for (size_t i = 0; i < kFrameSize; ++i) {
          float denoised = frame_out[i] / kPcmScale;
          float s = dry_wet * denoised + (1.0f - dry_wet) * in_buffer_[i];
          out[out_pos + i] = sanitize(s);
        }
        out_pos += kFrameSize;
        if (result) {
          result->status = ProcessStatus::kOk;
          result->has_vad = true;
          result->vad_probability = vad;
        }
      }
      in_buffer_count_ = 0;
    }
    // 若缓冲未满,return 当前 out_pos（可能为 0）。
  }

  // 处理剩余的整 480 样本块。
  while (in_pos + kFrameSize <= n_in && out_pos + kFrameSize <= n_out_max) {
    float scaled_in[kFrameSize];
    float frame_out[kFrameSize];
    for (size_t i = 0; i < kFrameSize; ++i)
      scaled_in[i] = in[in_pos + i] * kPcmScale;
    float vad = rnnoise_process_frame(state_.get(), frame_out, scaled_in);
    for (size_t i = 0; i < kFrameSize; ++i) {
      float denoised = frame_out[i] / kPcmScale;
      float s = dry_wet * denoised + (1.0f - dry_wet) * in[in_pos + i];
      out[out_pos + i] = sanitize(s);
    }
    out_pos += kFrameSize;
    in_pos += kFrameSize;
    if (result) {
      result->status = ProcessStatus::kOk;
      result->has_vad = true;
      result->vad_probability = vad;
    }
  }

  // 累积尾部不足一帧的样本到 in_buffer_（下次调用补齐）。
  if (in_pos < n_in) {
    size_t remain = n_in - in_pos;
    size_t cap = kFrameSize - in_buffer_count_;
    size_t take = std::min(remain, cap);
    std::memcpy(in_buffer_ + in_buffer_count_, in + in_pos,
                take * sizeof(float));
    in_buffer_count_ += take;
  }

  return out_pos;
}

size_t RnnoiseAdapter::flush(float* /*out*/, size_t /*n_out_max*/) {
  // RNNoise frame=hop=480,无 overlap-add 残余。
  // in_buffer_ 中累积的不足一帧样本无法凑成完整帧,RNNoise 不支持
  // 部分帧处理,故 flush 返回 0（残余样本被丢弃,符合流式收敛保证：
  // 长期 N 输入 -> N - latency 输出,最后 < 480 样本为算法延迟代价）。
  in_buffer_count_ = 0;
  return 0;
}

void RnnoiseAdapter::set_dry_wet(float ratio) {
  // 控制线程调用,与 RT process() 并发。atomic<uint32_t> + memcpy
  // 保证 lock-free（§2.2 线程安全契约）。
  if (ratio < 0.0f)
    ratio = 0.0f;
  if (ratio > 1.0f)
    ratio = 1.0f;
  dry_wet_bits_.store(float_to_bits(ratio), std::memory_order_relaxed);
}

bool RnnoiseAdapter::set_param(const std::string& /*key*/,
                               const std::string& /*value*/) {
  return false;  // RNNoise 当前不支持运行时参数（无 postfilter）
}

std::string RnnoiseAdapter::get_param(const std::string& /*key*/) const {
  return "";  // 无参数
}

}  // namespace noise

// 静态注册：进程加载时把 "rnnoise" 注册进 DenoisePluginRegistry 单例。
// 该 TU 必须链入 noise 库（CMakeLists.txt 中
// model-adapters/rnnoise/rnnoise_adapter.cpp 加入 NOISE_SOURCES）,否则
// switch_plugin("rnnoise") 失败。
// 注意：lambda 转 PluginCreator（函数指针）要求返回类型精确匹配
// std::unique_ptr<IDenoisePlugin>。unique_ptr<Derived> -> unique_ptr<Base>
// 的隐式转换在 lambda-to-function-pointer 上下文不生效,须显式尾置返回类型。
static bool registered = [] {
  noise::DenoisePluginRegistry::instance().register_plugin(
      "rnnoise", []() -> std::unique_ptr<noise::IDenoisePlugin> {
        return std::make_unique<noise::RnnoiseAdapter>();
      });
  return true;
}();

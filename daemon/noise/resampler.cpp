// daemon/noise/resampler.cpp
// Spec5 Task 1：入口重采样（native ↔ 48k）。架构依据：
//   docs/noise/architecture-design.md §3.1 + §11 风险1。
#include "resampler.hpp"

#include <algorithm>

namespace noise {

// SpeexDSP 桌面质量（5）：native↔48k 延迟 <<1ms，单通道开销可接受。
// 固定不改（YAGNI：T1 仅 native↔48k，无运行期切质量需求）。
static constexpr int kQuality = SPEEX_RESAMPLER_QUALITY_DESKTOP;

Resampler::Resampler(uint32_t in_rate, uint32_t out_rate, uint32_t channels)
    : in_rate_(in_rate),
      out_rate_(out_rate),
      channels_(channels),
      passthrough_(in_rate == out_rate),
      state_(nullptr),
      output_latency_(0) {
  if (passthrough_)
    return;  // 48k 直通：不实例化 SpeexDSP（零成本）。
  int err = 0;
  state_ = speex_resampler_init(channels_, in_rate_, out_rate_, kQuality, &err);
  if (state_ == nullptr || err != RESAMPLER_ERR_SUCCESS) {
    // 初始化失败：降级为 passthrough（拷贝），绝不抛异常 / 崩溃（RT 安全）。
    // state_ 保持 nullptr，process() 走拷贝分支。
    state_ = nullptr;
    passthrough_ = true;
    return;
  }
  // 实时流式：不调 speex_resampler_skip_zeros（保留滤波器初始延迟，首帧
  // 微小 ramp-up，但输出时长与输入一致、无样本丢弃；header 注释推荐实时
  // 处理不 skip）。output_latency 供测试对齐 + max_output_for_input 上界。
  output_latency_ =
      static_cast<size_t>(speex_resampler_get_output_latency(state_));
}

Resampler::~Resampler() {
  if (state_ != nullptr)
    speex_resampler_destroy(state_);
}

size_t Resampler::process(const float* in,
                          size_t in_len,
                          float* out,
                          size_t out_max) {
  if (in_len == 0 || out_max == 0)
    return 0;
  // passthrough（含 init 失败降级）：拷贝 min(in_len, out_max)，零 SpeexDSP。
  if (passthrough_ || state_ == nullptr) {
    size_t n = (in_len < out_max) ? in_len : out_max;
    std::copy(in, in + n, out);
    return n;
  }
  // SpeexDSP 流式：in_avail 进=可消费，出=已消费；out_cap 进=容量，出=已产出。
  spx_uint32_t in_avail = static_cast<spx_uint32_t>(in_len);
  spx_uint32_t out_cap = static_cast<spx_uint32_t>(out_max);
  int rc = speex_resampler_process_float(state_, /*channel_index=*/0, in,
                                         &in_avail, out, &out_cap);
  if (rc != RESAMPLER_ERR_SUCCESS) {
    // 运行期错误（极少见，如 ptr overlap）：降级返回已产出部分，不抛异常。
    // 输入可能未全部消费 -> 残留样本丢失（不崩溃优先，符合 RT 降级约束）。
  }
  return static_cast<size_t>(out_cap);  // 实际产出
}

size_t Resampler::max_output_for_input(size_t in_len) const {
  if (passthrough_)
    return in_len;
  // 稳态产出上界 = ceil(in_len * out / in)，加滤波器输出延迟 + 余量。
  // 确保调用方按此预分配后，process 一次能消费全部输入。
  const size_t steady =
      (static_cast<size_t>(in_len) * out_rate_ + in_rate_ - 1) / in_rate_;
  return steady + output_latency_ + 16;
}

}  // namespace noise

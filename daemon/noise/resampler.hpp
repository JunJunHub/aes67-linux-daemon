// daemon/noise/resampler.hpp
// Spec5 Task 1：入口重采样（native ↔ 48k）。
// 架构依据：docs/noise/architecture-design.md §3.1（入口位置）+ §11 风险1
// （native≠48k 时 ①②③④ 链路失配）。
//
// 职责：参数化、RAII、流式重采样，封装 SpeexDSP。
//   - 参数化采样率对 Resampler(in_rate, out_rate, channels)：T1 用 (native,48k)
//     解 ①②③④ 链路 + RNNoise 的 48kHz 限制；T2 DTLN 复用 (48k,16k)（同一原语）。
//   - RAII：构造即 speex_resampler_init，析构即 speex_resampler_destroy。
//   - 流式：SpeexDSP 维护内部滤波状态跨调用连续（sample-by-sample 的实时流）。
//   - passthrough：in_rate==out_rate（或 SpeexDSP 初始化失败降级）时不实例化
//     SpeexDSP，process() 退化为拷贝，零 SpeexDSP 开销（匹配 "48k 直通零成本"）。
//
// 线程模型：构造/析构在控制线程（add_sensor），process() 在 capture 线程
// （on_frame RT 路径）。SpeexDSP 状态非线程安全：一个 Resampler 实例仅供一个
// 传感器独占，on_frame 单线程顺序调用，无并发。process() 绝不抛异常
// （SpeexDSP 为 C 接口；失败降级为 passthrough 拷贝，符合 RT 安全约束）。
#ifndef NOISE_RESAMPLER_HPP_
#define NOISE_RESAMPLER_HPP_

#include <cstddef>
#include <cstdint>

#include <speex/speex_resampler.h>

namespace noise {

// 参数化流式重采样器（SpeexDSP RAII + passthrough 优化）。
// 非 copyable（持 SpeexResamplerState* 裸指针，浅拷贝会双重释放）；
// 非 movable（无需：SensorContext 用 shared_ptr<Resampler> 持有，见
// noise_manager.hpp）。
class Resampler {
 public:
  // 参数化采样率对。channels 默认 1（noise 模块单通道，arch §4.2
  // "channels 恒为 1"）。in_rate==out_rate 时为 passthrough。
  // quality 固定 SPEEX_RESAMPLER_QUALITY_DESKTOP(5)：native↔48k 重采样延迟
  // <<1ms（arch §3.1），单通道 CPU 开销可接受（arch §11 风险1）。
  Resampler(uint32_t in_rate, uint32_t out_rate, uint32_t channels = 1);
  ~Resampler();

  Resampler(const Resampler&) = delete;
  Resampler& operator=(const Resampler&) = delete;
  Resampler(Resampler&&) = delete;
  Resampler& operator=(Resampler&&) = delete;

  // 流式重采样：in_len 个输入样本 -> 最多 out_max 个输出样本。
  // 返回实际产出样本数。SpeexDSP 维护内部滤波状态，跨调用连续（实时流）。
  //   - in_len：提供的输入样本数（全部尝试消费；若 out_max 不足则部分消费，
  //     残留留待下次——调用方应按 max_output_for_input() 预分配足量 out）。
  //   - out_max：输出缓冲容量。返回值 ≤ out_max。
  //   - 输入/输出缓冲不可重叠（SpeexDSP 约束）。
  //   - passthrough 时拷贝 min(in_len, out_max) 并返回。
  //   - state_ 为 nullptr（init 失败降级）时同 passthrough，绝不抛异常。
  size_t process(const float* in, size_t in_len, float* out, size_t out_max);

  // in_rate==out_rate（或 init 失败降级）时为 true：process 退化为拷贝，
  // 无 SpeexDSP 调用。on_frame 据此走 48k 直通零成本路径。
  bool is_passthrough() const { return passthrough_; }

  uint32_t in_rate() const { return in_rate_; }
  uint32_t out_rate() const { return out_rate_; }
  uint32_t channels() const { return channels_; }

  // 滤波器输出延迟（样本数）。实时流式不调 speex_resampler_skip_zeros，
  // 首帧有此延迟的微小 ramp-up（arch §3.1：<<1ms），但输出时长与输入一致、
  // 无样本丢弃。供测试对齐参考 + max_output_for_input 缓冲上界计算。
  // passthrough 时为 0。
  size_t output_latency() const { return output_latency_; }

  // 给定输入长度，返回输出样本数的保守上界（含滤波器延迟 + 余量），供
  // 调用方预分配输出缓冲，确保 process 一次消费全部输入。passthrough 时
  // 返回 in_len（输出 == 输入）。
  size_t max_output_for_input(size_t in_len) const;

 private:
  uint32_t in_rate_;
  uint32_t out_rate_;
  uint32_t channels_;
  bool passthrough_;
  SpeexResamplerState* state_;  // nullptr 当 passthrough_（含 init 失败降级）
  size_t output_latency_;       // speex_resampler_get_output_latency；passthrough=0
};

}  // namespace noise

#endif  // NOISE_RESAMPLER_HPP_

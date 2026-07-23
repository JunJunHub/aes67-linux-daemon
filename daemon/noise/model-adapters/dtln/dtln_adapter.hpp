// daemon/noise/model-adapters/dtln/dtln_adapter.hpp
// DTLN 降噪插件适配器（架构依据：docs/noise/denoise-plugin-architecture.md
// §3.2）。Spec5 Task 2：双 ONNX 模型串联 + LSTM 状态跨帧喂回 + 48k↔16k
// 重采样（复用 T1 Resampler）+ overlap-add（hop=128 < frame=512，75% 重叠）。
//
// 模型签名（实测，见 docs/noise/denoise-plugin-architecture.md §1.2）：
//   model_1.onnx（STFT 幅度掩蔽）
//     IN  input_2:  [1,1,257]   归一化幅度谱（rfft of 512 样本 = 257 频点）
//     IN  input_3:  [1,2,128,2] LSTM 隐状态（跨帧喂回）
//     OUT activation_2: [1,1,257]  估计掩蔽
//     OUT tf_op_layer_stack_2: [1,2,128,2] 更新后的 LSTM 状态
//   model_2.onnx（时域增强）
//     IN  input_4:  [1,1,512]   第 1 级输出经 irfft 还原的时域块
//     IN  input_5:  [1,2,128,2] LSTM 隐状态
//     OUT conv1d_3: [1,1,512]   增强后的时域块
//     OUT tf_op_layer_stack_5: [1,2,128,2] 更新后的 LSTM 状态
// 按 index 绑定 I/O（名字 input_2/input_3 随导出版本变化，index 稳定）。
//
// 算法（对齐 breizhn/DTLN real_time_processing_onnx.py）：
//   每 hop=128 @16k：
//     1. frame_buffer_(512) 滑窗左移 128，尾部追加新 128 样本
//     2. rfft(frame_buffer_) -> 257 复频点；in_mag=|·|, in_phase=∠·
//     3. model_1.run([in_mag, state1]) -> [mask,
//     new_state1]；state1<-new_state1
//     4. estimated = in_mag*mask*exp(i*in_phase)（保相位，掩蔽幅度）
//     5. irfft(estimated, 512) -> 时域块
//     6. model_2.run([time_block, state2]) -> [enhanced,
//     new_state2]；state2<-new_state2
//     7. out_buffer_(512) 左移 128 清尾，+= enhanced（overlap-add）；取前 128
//     输出
//   无窗函数（DTLN 原始实现直接 rfft/irfft + overlap-add，参考脚本如此）。
//
// RT 契约：所有 Run() try/catch，失败直通 in 并置 kBypass；clamp [-1,1]，
// NaN/Inf->0。LSTM/STFT 缓冲构造时分配，运行时复用（无每帧堆分配）。
#ifndef NOISE_MODEL_ADAPTERS_DTLN_DTLN_ADAPTER_HPP_
#define NOISE_MODEL_ADAPTERS_DTLN_DTLN_ADAPTER_HPP_

#include <atomic>
#include <complex>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "denoise_plugin.hpp"
#include "fft.hpp"
#include "resampler.hpp"

// forward declare Ort::Session（避免在头里 include onnxruntime_cxx_api.h，
// 减少 noise PUBLIC 头的传递依赖；adapter .cpp 内 include）
namespace Ort {
class Session;
}

namespace noise {

class DtlnAdapter : public IDenoisePlugin {
 public:
  // 默认构造与析构均 out-of-line（定义在 .cpp）：类内 = default 的默认构造
  // 会被 inline 定义在头内，其为异常安全生成的成员析构清理代码需实例化
  // unique_ptr<Ort::Session>::~unique_ptr()，而此处 Ort::Session 仅有前向
  // 声明（不完整类型）-> 编译报 incomplete type。定义移至 .cpp（include
  // onnxruntime_cxx_api.h，Ort::Session 完整）即可推迟实例化。
  DtlnAdapter();
  ~DtlnAdapter() override;

  DtlnAdapter(const DtlnAdapter&) = delete;
  DtlnAdapter& operator=(const DtlnAdapter&) = delete;

  bool init(const PluginConfig& cfg) override;
  void reset() override;

  const char* name() const override;
  uint32_t native_sample_rate() const override;
  uint32_t algorithmic_latency_samples() const override;
  bool supports_vad() const override;
  bool supports_snr() const override;

  size_t process(const float* in,
                 size_t n_in,
                 float* out,
                 size_t n_out_max,
                 DenoiseResult* result) override;
  size_t flush(float* out, size_t n_out_max) override;

  void set_dry_wet(float ratio) override;
  bool set_param(const std::string& key, const std::string& value) override;
  std::string get_param(const std::string& key) const override;

 private:
  // 跑一个 DTLN 帧（hop=128 @16k）：填 frame_buffer_ 尾部 128 新样本 ->
  // 双模型串联 -> overlap-add -> 128 输出样本追加到 out_fifo16_。
  // 成功返回 true；Run 异常返回 false（调用方置 kBypass + 直通）。
  bool process_one_frame_();

  // 状态：双 ONNX session（RAII：unique_ptr 析构自动释放；init 失败时已建
  // 的 session 由 unique_ptr 自动释放，无泄漏）。
  std::unique_ptr<Ort::Session> sess1_;
  std::unique_ptr<Ort::Session> sess2_;
  bool initialized_{false};

  // LSTM 状态张量后端存储（[1,2,128,2] = 512 float，跨帧喂回）。
  // 构造时分配，每帧创建引用此存储的 Ort::Value 输入；Run 后把输出状态
  // 拷回此存储供下帧用（稳定后端，避免每帧堆分配）。
  std::vector<float> state1_data_;  // model_1 LSTM 状态
  std::vector<float> state2_data_;  // model_2 LSTM 状态

  // I/O 名字缓存（init 时取一次，避免每帧 OnnxInputName 返回临时 string 的
  // 生命周期问题）。按 index：sess1 in=[mag,state] out=[mask,newstate1]，
  // sess2 in=[time,state] out=[enhanced,newstate2]。
  std::string m1_in0_, m1_in1_, m1_out0_, m1_out1_;
  std::string m2_in0_, m2_in1_, m2_out0_, m2_out1_;

  // dry_wet 原子存储（与 RnnoiseAdapter 同一做法，§2.2 线程安全契约）。
  static_assert(std::atomic<uint32_t>::is_always_lock_free,
                "atomic<uint32_t> must be lock-free for RT safety");
  std::atomic<uint32_t> dry_wet_bits_{0u};  // 默认 1.0f 的 IEEE 754 位模式

  // 16k 域帧化缓冲（DTLN native 16k）。
  static constexpr size_t kFrame = 512;  // frame（STFT 窗）
  static constexpr size_t kHop = 128;    // hop（75% 重叠）
  float frame_buffer_[kFrame]{};         // STFT 滑窗（左移 + 追加）
  float out_buffer_[kFrame]{};           // overlap-add 累积
  // 16k 输入/输出 FIFO（48k 输入经 down_ 重采样入 in_fifo16_，帧化处理产出
  // 入 out_fifo16_，经 up_ 重采样回 48k 输出）。deque 跨调用连续（流式）。
  std::deque<float> in_fifo16_;
  std::deque<float> out_fifo16_;

  // 重采样器：48k↔16k（复用 T1 Resampler，参数化原语）。
  std::unique_ptr<Resampler> down_;  // 48000 -> 16000
  std::unique_ptr<Resampler> up_;    // 16000 -> 48000
  // 48k 输出 FIFO（up_ 产出暂存，process 取 min(可用, n_out_max) 输出）。
  std::deque<float> out_fifo48_;
  // 48k 输入延迟线（dry_wet 混合对齐：denoised 输出相对输入有算法延迟，
  // 干路需延迟同样深度才能正确混合 dry_wet*denoised + (1-dry_wet)*orig）。
  // depth 稳态 = 算法延迟，front 对应当前 denoised 输出的原始样本。
  std::deque<float> in_delay48_;
  // 重采样暂存（capture 线程独占复用，避免每帧分配）。
  std::vector<float> down_scratch_;
  std::vector<float> up_scratch_;

  // FFT 暂存（复用容量，避免每帧 realloc）。
  std::vector<fft::Complex> spec_;  // rfft 输出 257 复频点
  std::vector<float> mag_;          // |spec|（257）
  std::vector<float> phase_;        // ∠spec（257）
  std::vector<float> est_block_;    // irfft 产出时域块（512）

  uint32_t in_rate_{48000};  // 输入采样率（process 的 sample_rate_in）
};

}  // namespace noise

#endif  // NOISE_MODEL_ADAPTERS_DTLN_DTLN_ADAPTER_HPP_

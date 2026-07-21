// daemon/noise/model-adapters/rnnoise/rnnoise_adapter.hpp
// RNNoise 降噪插件适配器（架构依据：
//   docs/noise/denoise-plugin-architecture.md §3.1）。
// Spec2 1.6b：封装 RNNoise C 库,frame=hop=480（无 overlap-add）,
// 产 VAD 概率,延迟 = 480 样本。
//
// RAII：state_ 用 unique_ptr<DenoiseState, RnnoiseDeleter>,
// model_ 用 unique_ptr<RNNModel, RnnoiseModelDeleter>,
// 析构自动 rnnoise_destroy/rnnoise_model_free,杜绝部分 init 泄漏。
//
// 线程安全契约（§2.2 实时安全）：dry_wet_ 用 std::atomic<uint32_t>
// 存储 IEEE 754 位模式,std::memcpy 做 float↔uint32_t 转换。
// 不用 std::atomic<float>：C++ 标准不保证其 lock-free,ARM 平台可能
// 内部用自旋锁致 RT 优先级反转。atomic<uint32_t> 在所有目标平台
// IS_ALWAYS_LOCK_FREE = true。
#ifndef NOISE_MODEL_ADAPTERS_RNNOISE_RNNOISE_ADAPTER_HPP_
#define NOISE_MODEL_ADAPTERS_RNNOISE_RNNOISE_ADAPTER_HPP_

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "denoise_plugin.hpp"
#include "rnnoise.h"

namespace noise {

class RnnoiseAdapter : public IDenoisePlugin {
 public:
  RnnoiseAdapter() = default;
  ~RnnoiseAdapter() override = default;

  // 不可拷贝（持 unique_ptr 资源 + atomic）,可移动（但本接口未要求）。
  RnnoiseAdapter(const RnnoiseAdapter&) = delete;
  RnnoiseAdapter& operator=(const RnnoiseAdapter&) = delete;

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
  // RAII 删除器：析构调用对应 RNNoise C 释放函数。
  struct RnnoiseDeleter {
    void operator()(DenoiseState* st) const noexcept {
      if (st)
        rnnoise_destroy(st);
    }
  };
  struct RnnoiseModelDeleter {
    void operator()(RNNModel* m) const noexcept {
      if (m)
        rnnoise_model_free(m);
    }
  };

  using StatePtr = std::unique_ptr<DenoiseState, RnnoiseDeleter>;
  using ModelPtr = std::unique_ptr<RNNModel, RnnoiseModelDeleter>;

  StatePtr state_;
  ModelPtr model_;  // 仅从文件加载时持有;默认模型为 nullptr

  // dry_wet 原子存储：bit-cast 到 uint32_t（IEEE 754 位模式）。
  // std::atomic<uint32_t>::is_always_lock_free = true on all targets。
  static_assert(std::atomic<uint32_t>::is_always_lock_free,
                "atomic<uint32_t> must be lock-free for RT safety");
  std::atomic<uint32_t> dry_wet_bits_{0u};  // 默认 1.0f 的位模式

  // 累积输入直到 480 样本一帧（rnnoise_get_frame_size()）。
  // 测试用例每次喂 480 样本,实际为单帧路径;此缓冲支持任意长度输入。
  static constexpr size_t kFrameSize = 480;
  float in_buffer_[kFrameSize]{};
  size_t in_buffer_count_{0};
};

}  // namespace noise

#endif  // NOISE_MODEL_ADAPTERS_RNNOISE_RNNOISE_ADAPTER_HPP_

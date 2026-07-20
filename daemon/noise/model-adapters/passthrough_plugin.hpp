// daemon/noise/model-adapters/passthrough_plugin.hpp
// 默认直通插件（架构依据：docs/noise/denoise-plugin-architecture.md §4.2）。
// in->out 直通，algorithmic_latency_samples()=0，denoise_processor 构造时装入，
// 保证 plugin_ 永不为空。
//
// Task 3 简化（per Task 3 brief resolution #8）：
//   - dry_wet_ 用普通 float（passthrough 忽略，输出恒等于输入）。
//   - 真正的 atomic<uint32_t> + memcpy 语义在 Task 4 RnnoiseAdapter 验证。
//   - 无任何参数（set_param 恒返回 false，get_param 恒返回空串）。
#ifndef NOISE_MODEL_ADAPTERS_PASSTHROUGH_PLUGIN_HPP_
#define NOISE_MODEL_ADAPTERS_PASSTHROUGH_PLUGIN_HPP_

#include <cstdint>
#include <string>

#include "denoise_plugin.hpp"

namespace noise {

class PassthroughPlugin : public IDenoisePlugin {
 public:
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
  // Task 3 简化：普通 float 存储（passthrough 不用）。
  float dry_wet_{1.0f};
  uint32_t sample_rate_{48000};
};

}  // namespace noise

#endif  // NOISE_MODEL_ADAPTERS_PASSTHROUGH_PLUGIN_HPP_

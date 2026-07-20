// daemon/noise/model-adapters/passthrough_plugin.cpp
// 默认直通插件实现 + 静态注册到 DenoisePluginRegistry（"passthrough"）。
#include "passthrough_plugin.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "denoise_plugin_factory.hpp"

namespace noise {

bool PassthroughPlugin::init(const PluginConfig& cfg) {
  sample_rate_ = cfg.sample_rate_in;
  dry_wet_ = cfg.dry_wet;
  return true;
}

void PassthroughPlugin::reset() {}

const char* PassthroughPlugin::name() const {
  return "passthrough";
}

uint32_t PassthroughPlugin::native_sample_rate() const {
  return 48000;
}

uint32_t PassthroughPlugin::algorithmic_latency_samples() const {
  return 0;
}

bool PassthroughPlugin::supports_vad() const {
  return false;
}

bool PassthroughPlugin::supports_snr() const {
  return false;
}

size_t PassthroughPlugin::process(const float* in,
                                  size_t n_in,
                                  float* out,
                                  size_t n_out_max,
                                  DenoiseResult* result) {
  // 直通：min(n_in, n_out_max) 样本 memcpy。
  // PassthroughPlugin 输出 == 输入，denoised == original，故 noise = 0。
  // 数值清洗契约（§2.2）：clamp 输出到 [-1,1]，NaN/Inf 替换为 0。
  // 直通场景下输入理应已规范化，但守约仍做一遍。
  size_t n = std::min(n_in, n_out_max);
  for (size_t i = 0; i < n; ++i) {
    float s = in[i];
    if (!std::isfinite(s))
      s = 0.0f;  // NaN/Inf -> 0
    if (s > 1.0f)
      s = 1.0f;
    if (s < -1.0f)
      s = -1.0f;
    out[i] = s;
  }
  if (result)
    result->status = ProcessStatus::kOk;
  return n;
}

size_t PassthroughPlugin::flush(float* /*out*/, size_t /*n_out_max*/) {
  return 0;  // 无内部缓冲，无残余
}

void PassthroughPlugin::set_dry_wet(float ratio) {
  // Task 3 简化：普通 float 存储，passthrough 不消费 dry_wet。
  if (ratio < 0.0f)
    ratio = 0.0f;
  if (ratio > 1.0f)
    ratio = 1.0f;
  dry_wet_ = ratio;
}

bool PassthroughPlugin::set_param(const std::string& /*key*/,
                                  const std::string& /*value*/) {
  return false;  // passthrough 无参数
}

std::string PassthroughPlugin::get_param(const std::string& /*key*/) const {
  return "";  // passthrough 无参数
}

}  // namespace noise

// 静态注册：进程加载时把 "passthrough" 注册进 DenoisePluginRegistry 单例。
// 该 TU 必须链入 noise 库（CMakeLists.txt 中
// model-adapters/passthrough_plugin.cpp 加入 NOISE_SOURCES），否则
// switch_plugin("passthrough") 失败。
// 注意：lambda 转 PluginCreator（函数指针）要求返回类型精确匹配
// std::unique_ptr<IDenoisePlugin>。unique_ptr<Derived> -> unique_ptr<Base>
// 的隐式转换在 lambda-to-function-pointer 上下文不生效，须显式尾置返回类型。
static bool registered = [] {
  noise::DenoisePluginRegistry::instance().register_plugin(
      "passthrough", []() -> std::unique_ptr<noise::IDenoisePlugin> {
        return std::make_unique<noise::PassthroughPlugin>();
      });
  return true;
}();

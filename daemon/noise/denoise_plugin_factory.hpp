// daemon/noise/denoise_plugin_factory.hpp
// 插件工厂与注册（架构依据：docs/noise/denoise-plugin-architecture.md §4.1）。
// Spec2 1.6a：DenoisePluginRegistry 单例 + 静态注册模式。
#ifndef NOISE_DENOISE_PLUGIN_FACTORY_HPP_
#define NOISE_DENOISE_PLUGIN_FACTORY_HPP_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "denoise_plugin.hpp"

namespace noise {

// 插件创建函数类型
using PluginCreator = std::unique_ptr<IDenoisePlugin> (*)();

class DenoisePluginRegistry {
 public:
  static DenoisePluginRegistry& instance();

  // 注册插件（静态初始化时调用）
  void register_plugin(const std::string& name, PluginCreator creator);
  bool has(const std::string& name) const;

  // 创建插件实例
  std::unique_ptr<IDenoisePlugin> create(const std::string& name) const;

  // 列出所有已注册插件
  std::vector<std::string> list() const;

 private:
  std::map<std::string, PluginCreator> creators_;
};

}  // namespace noise

#endif  // NOISE_DENOISE_PLUGIN_FACTORY_HPP_

// daemon/noise/denoise_plugin_factory.cpp
// DenoisePluginRegistry
// 单例实现（架构依据：docs/noise/denoise-plugin-architecture.md §4.1）。
#include "denoise_plugin_factory.hpp"

#include <mutex>

namespace noise {

DenoisePluginRegistry& DenoisePluginRegistry::instance() {
  // C++11 函数局部静态：线程安全初始化（Meyer's singleton）。
  static DenoisePluginRegistry reg;
  return reg;
}

void DenoisePluginRegistry::register_plugin(const std::string& name,
                                            PluginCreator creator) {
  // 静态初始化可能在 main 之前并发（不同 TU），用 mutex 保护写。
  // 简单场景下 std::map 的 operator[] 在静态初始化阶段通常单线程，
  // 但 registry 单例首次访问可能发生在任意 TU 的 static init，加锁更稳。
  static std::mutex m;
  std::lock_guard<std::mutex> lock(m);
  creators_[name] = creator;
}

bool DenoisePluginRegistry::has(const std::string& name) const {
  return creators_.find(name) != creators_.end();
}

std::unique_ptr<IDenoisePlugin> DenoisePluginRegistry::create(
    const std::string& name) const {
  auto it = creators_.find(name);
  if (it == creators_.end())
    return nullptr;
  return it->second();
}

std::vector<std::string> DenoisePluginRegistry::list() const {
  std::vector<std::string> names;
  names.reserve(creators_.size());
  for (const auto& kv : creators_)
    names.push_back(kv.first);
  return names;
}

}  // namespace noise

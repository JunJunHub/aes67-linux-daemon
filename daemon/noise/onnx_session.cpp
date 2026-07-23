// daemon/noise/onnx_session.cpp
// ONNX Runtime 统一后端实现（D-S5.1 + denoise-plugin §9.1）。
#include "onnx_session.hpp"

namespace noise {

OnnxEnv::OnnxEnv()
    // ORT_LOGGING_LEVEL_WARNING：抑制 INFO/VERBOSE 的海量日志（推理每帧
    // 调用，INFO 级会淹没 stderr）。logid "noise" 仅用于 ORT 内部日志标识。
    : env_(ORT_LOGGING_LEVEL_WARNING, "noise") {}

Ort::Env& OnnxEnv::instance() {
  // Meyer's singleton：函数局部 static，首次调用线程安全构造。
  // 析构在程序静态析构阶段，由 main() 生命周期保证晚于所有 Session
  // （Session 是 NoiseManager 持有的局部对象，栈展开先于静态析构）。
  static OnnxEnv env;
  return env.env_;
}

std::unique_ptr<Ort::Session> CreateOnnxSession(const std::string& model_path) {
  // Ort::Session 构造可能抛 Ort::Exception（文件不存在 / 模型损坏 / OOM）。
  // adapter 在 init() 调用本 helper 时 try/catch，失败返回 false 让
  // DenoiseProcessor 切默认插件（§2.2 错误降级契约）。本函数捕获异常返回
  // nullptr，使 adapter 的 init 逻辑统一（不区分异常 vs 返回值）。
  try {
    Ort::SessionOptions opts;
    // intra-op 线程数=1：RT 路径每帧推理，避免 ORT 线程池调度抖动致 xrun。
    // 模型小（DTLN <1M 参数 / DFN ~8MB），单线程推理 <0.5ms（§7.4.2 实测）。
    opts.SetIntraOpNumThreads(1);
    opts.SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_BASIC);
    // 用 new 而非 make_unique：Ort::Session 构造需 (env, path, opts) 三参，
    // make_unique 可直接转发，但显式 new 让异常捕获边界清晰。
    return std::unique_ptr<Ort::Session>(
        new Ort::Session(OnnxEnv::instance(), model_path.c_str(), opts));
  } catch (const Ort::Exception& /*e*/) {
    return nullptr;
  } catch (...) {
    return nullptr;
  }
}

const Ort::MemoryInfo& OnnxMemoryInfo() {
  // 进程级共享：CreateCpu 是轻量构造，但避免每帧重建，缓存为 static。
  // Ort::MemoryInfo 可拷贝（引用计数底层），static 局部线程安全初始化。
  static const Ort::MemoryInfo info =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  return info;
}

std::string OnnxInputName(const Ort::Session& sess, size_t index) {
  // 按 index 取输入名（adapter 用 name 数组绑 Run）。名字随导出版本变化
  // （DTLN model_1 的 input_2/input_3、model_2 的 input_4/input_5），
  // 按 index 绑定比硬编码名字稳健。GetInputNameAllocated 可能抛异常
  // （index 越界等），此处吞掉返回空串，让 adapter 据空串判失败。
  try {
    Ort::AllocatorWithDefaultOptions allocator;
    auto name_ptr = sess.GetInputNameAllocated(index, allocator);
    return name_ptr ? std::string(name_ptr.get()) : std::string();
  } catch (...) {
    return std::string();
  }
}

std::string OnnxOutputName(const Ort::Session& sess, size_t index) {
  try {
    Ort::AllocatorWithDefaultOptions allocator;
    auto name_ptr = sess.GetOutputNameAllocated(index, allocator);
    return name_ptr ? std::string(name_ptr.get()) : std::string();
  } catch (...) {
    return std::string();
  }
}

}  // namespace noise

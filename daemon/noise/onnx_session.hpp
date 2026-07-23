// daemon/noise/onnx_session.hpp
// Spec5 Task 2：ONNX Runtime 统一后端基建（D-S5.1 + denoise-plugin §9.1）。
//
// 职责单一：ONNX Runtime 进程级生命周期 + Session 创建 helper + 公共
// MemoryInfo / SessionOptions。封装 Ort::Env 单例与 CreateOnnxSession，
// 供 DTLN/DeepFilterNet adapter 复用（denoise-plugin §3.2/§3.3 与 §9.1
// "ONNX Runtime 线程安全约定"）。
//
// 线程模型（§9.1）：
//   - Ort::Env 进程级单例，可多线程并发访问。Meyer's singleton（函数局部
//     static），由 main() 生命周期保证析构晚于所有 Ort::Session：
//     NoiseManager（持 DenoiseProcessor -> RcuPtr<PluginSlot> -> adapter ->
//     Ort::Session）作为 main 的 shared_ptr 局部，在 main 栈展开时先析构
//     （释放所有 Session），再到静态析构阶段 OnnxEnv 析构。main.cpp 在
//     NoiseManager 创建前显式 touch OnnxEnv::instance() 固定此序。
//   - Ort::Session per-sensor 独占，仅该 sensor 的 capture 线程调 Run()，
//     无并发。析构（释放图/权重，毫秒级）由 RcuPtr retire 队列延迟到控制
//     线程 housekeeper（§4.2），绝不在 RT 线程析构。
//   - Ort::Value（LSTM/STFT 状态张量）per-sensor 独占，仅 capture 线程读写。
#ifndef NOISE_ONNX_SESSION_HPP_
#define NOISE_ONNX_SESSION_HPP_

#include <memory>
#include <string>

#include <onnxruntime_cxx_api.h>

namespace noise {

// ONNX Runtime 进程级环境单例。首次调用时构造 Ort::Env（ORT_LOGGING_LEVEL
// 选 Warning，抑制大量 verbose 日志）。Meyer's singleton 线程安全初始化。
// 析构在程序静态析构阶段，晚于所有 Session（由 main 生命周期保证）。
class OnnxEnv {
 public:
  OnnxEnv(const OnnxEnv&) = delete;
  OnnxEnv& operator=(const OnnxEnv&) = delete;

  // 返回进程级 Ort::Env 引用。首次调用构造。线程安全（函数局部 static）。
  static Ort::Env& instance();

 private:
  OnnxEnv();
  Ort::Env env_;
};

// 创建一个 Ort::Session（加载模型文件）。失败返回 nullptr（不抛异常）。
// SessionOptions：intra-op 线程数=1（RT 可预测性，避免线程池抖动），
// 关闭图优化级别过高带来的额外内存（用 ORT_ENABLE_BASIC 即可）。
// 返回 unique_ptr<Ort::Session>，由 adapter 用 unique_ptr 持有（RAII，
// 析构自动释放；部分 init 失败时已创建的 session 由 unique_ptr 自动释放）。
std::unique_ptr<Ort::Session> CreateOnnxSession(const std::string& model_path);

// 进程级共享 MemoryInfo（CPU device 0）。adapter 创建输入/输出张量时复用。
const Ort::MemoryInfo& OnnxMemoryInfo();

// 便捷：从 session 的第 index 个输入/输出取 name（C 字符串拷贝到 std::string）。
// 用于按 index 绑定 I/O（名字随导出版本变化如 input_2/input_3，按 index 稳定）。
std::string OnnxInputName(const Ort::Session& sess, size_t index);
std::string OnnxOutputName(const Ort::Session& sess, size_t index);

}  // namespace noise

#endif  // NOISE_ONNX_SESSION_HPP_

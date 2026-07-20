// daemon/noise/denoise_plugin.hpp
// 降噪插件统一接口（架构依据：docs/noise/denoise-plugin-architecture.md
// §2.2）。 Spec2 1.6a：IDenoisePlugin + PluginConfig + DenoiseResult +
// ProcessStatus。
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace noise {

// 处理状态：RT 线程据此决定是否降级、上报、切插件
enum class ProcessStatus {
  kOk,  // 正常降噪输出
  kBypass,  // 插件内部错误，已将 in 直通到 out（降级，不中断音频流）
  kError,  // 不可恢复错误（模型损坏/反复失败），调用方应 flush/重置或切插件
};

// 降噪处理附加结果（可选）
struct DenoiseResult {
  ProcessStatus status{ProcessStatus::kOk};  // 见下方"实时安全契约"
  bool has_vad{false};                       // 该插件是否产出 VAD
  float vad_probability{0};  // VAD 概率 [0,1]，仅 RNNoise 等填充
  bool has_snr{false};       // 该插件是否产出 SNR 估计
  float estimated_snr_db{0};  // 局部 SNR 估计 (dB)，仅 DeepFilterNet 等填充
};

// 插件配置（初始化时传入）
struct PluginConfig {
  std::string model_path;          // 模型文件路径（空=默认模型）
  uint32_t sample_rate_in{48000};  // 输入音频采样率
  uint32_t channels{1};            // 通道数
  float dry_wet{1.0f};             // 干湿比 0=原音 1=全降噪
  // 通用键值参数，各插件自定义（如 postfilter=true）
  std::unordered_map<std::string, std::string> params;
};

// 降噪插件统一接口
class IDenoisePlugin {
 public:
  virtual ~IDenoisePlugin() = default;

  // ── 生命周期 ──
  virtual bool init(const PluginConfig& cfg) = 0;
  virtual void reset() = 0;  // 清空内部状态（切换源时调用）

  // ── 元信息（init 后有效）──
  virtual const char* name() const = 0;
  virtual uint32_t native_sample_rate() const = 0;  // 模型原生采样率
  virtual uint32_t algorithmic_latency_samples()
      const = 0;                          // 算法引入的固定延迟
  virtual bool supports_vad() const = 0;  // 是否产出 VAD（仅 RNNoise）
  virtual bool supports_snr()
      const = 0;  // 是否产出 SNR 估计（仅 DeepFilterNet，复用 lsnr）

  // ── 流式处理（核心）──
  // 输入任意长度样本，输出降噪样本，返回实际输出样本数。
  // out 容量需 >= n_in + latency。result 可选。
  // 注意：首帧可能因算法延迟不立即产出，输出数 <= 输入数是正常的。
  // 调用方契约：DenoiseProcessor 预分配 out 缓冲容量 = max_frame + latency，
  // 每次调用后仅消费返回值 n 个样本（非 n_out_max）。若 n < n_in（首帧延迟
  // 或插件内部缓冲未满），不足部分在后续调用中补回（流式保证：长期输入 N
  // 样本，输出收敛到 N - latency）。flush() 取出残余。
  virtual size_t process(const float* in,
                         size_t n_in,
                         float* out,
                         size_t n_out_max,
                         DenoiseResult* result = nullptr) = 0;

  // ── 实时安全契约（所有 adapter 必须遵守，RT 线程调用）──
  // 1. 错误降级：ONNX 推理抛错/shape 不配/OOM/LSTM 状态发散时，adapter
  //    不得在 RT 线程抛异常，必须将 in 直通到 out（min(n_in, n_out_max) 样本）
  //    并置 result->status = kBypass；不可恢复置 kError。音频流绝不因插件
  //    错误中断。
  // 2. 数值清洗：process() 返回前必须 clamp 输出到 [-1,1]，NaN/Inf 替换为 0。
  //    （NaN 喂 ALSA 放音 = 满幅 DC/爆音，可能损设备。）

  // ── 排空缓冲（停止时调用，取出残余样本）──
  virtual size_t flush(float* out, size_t n_out_max) = 0;

  // ── 动态参数（仅控制线程调用；与 RT process() 并发，实现须线程安全）──
  // dry_wet_ 须为 std::atomic<uint32_t>（存储 IEEE 754 位模式，std::memcpy 做
  // float↔uint32_t 转换）。不直接用 std::atomic<float>：C++ 标准不保证
  // atomic<float> 是 lock-free，ARM 等平台可能内部用自旋锁致 RT 优先级反转。
  // atomic<uint32_t> 保证 lock-free（IS_ALWAYS_LOCK_FREE = true on all
  // targets）。 params 须为不可变快照：set_param 构造新 shared_ptr<const
  // ParamMap> 原子换， process() 每次顶部 load 快照。绝不让控制线程写
  // unordered_map 的同时 RT 读 （rehash 中途遍历 = UB/可能崩）。详见 §4.2
  // 准热切换的线程模型。 set_param 返回 bool：true=参数已接受并生效，false=无效
  // key/value（HTTP 400）。
  virtual void set_dry_wet(float ratio) = 0;  // 0~1
  virtual bool set_param(const std::string& key, const std::string& value) = 0;
  virtual std::string get_param(const std::string& key) const = 0;
};

}  // namespace noise

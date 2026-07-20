// daemon/noise/denoise_processor.hpp
// DenoiseProcessor - 可插拔降噪（架构依据：
//   docs/noise/denoise-plugin-architecture.md §4.2 + §3.4 DenoiseOutput）。
// Spec2 1.6a：框架实现 - RcuPtr 原子插件槽 + 前后双缓冲 + 准热切换静音窗口。
//
// 线程模型（§4.2）：
//   - 控制线程（HTTP，NoiseManager 串行化同 sensor 的控制入口，架构 §3.7）：
//     switch_plugin / set_dry_wet / set_param / set_latency_change_cb /
//     drain_retire。
//   - 音频线程（capture 线程，高频）：on_period_begin -> process×N ->
//     on_period_end。
//     读路径全程无锁：仅在 period 顶部 load 一次快照，整周期复用。
#ifndef NOISE_DENOISE_PROCESSOR_HPP_
#define NOISE_DENOISE_PROCESSOR_HPP_

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "denoise_plugin.hpp"
#include "denoise_plugin_factory.hpp"
#include "rcu_ptr.hpp"

namespace noise {

// DenoiseOutput：架构 §3.4 L652-657，const float* + frame_count（不用
// gsl::span， 受 Global Constraints 约束）。指向 front_ 缓冲的只读视图，下次
// on_period_end swap 前有效（架构 §11 风险23）。
struct DenoiseOutput {
  const float* original{
      nullptr};  // 原始 PCM（per-sensor 拷贝，全 Phase 线程安全）
  const float* denoised{nullptr};  // 降噪 PCM（插件输出，双缓冲覆盖）
  const float* noise{nullptr};  // 噪声 PCM（原始 - 降噪，双缓冲覆盖）
  size_t frame_count{0};
};

class DenoiseProcessor {
 public:
  DenoiseProcessor();

  // ── 控制线程 ──
  // 准热切换：控制线程 create+init 新插件 -> 构造 PluginSlot{plugin,
  // latency+kConvergenceMargin} -> publish（旧 slot 入 retire_list_）-> cb。
  // 成功返回 true；插件名未注册或 init 失败返回 false。
  bool switch_plugin(const std::string& name);

  // 路由到当前活动插件（控制线程 load rcu_ptr_，安全）。
  // Task 3 简化：PassthroughPlugin 用普通 float dry_wet_，忽略 ratio。
  // 真正的 atomic<uint32_t>+memcpy 在 Task 4 RnnoiseAdapter 验证。
  void set_dry_wet(float ratio);
  bool set_param(const std::string& key, const std::string& value);

  // ── 音频线程：周期顶部 pin 一次，整周期复用，不每帧原子操作 ──
  void on_period_begin();

  // 三路输出处理（§4.2）：
  //   ① plugin->process(in, n_in, back_->denoised, max_frame_, result)
  //   ② memcpy back_->original = in（per-sensor 拷贝，全 Phase 线程安全）
  //   ③ back_->noise[i] = original[i] - denoised[i]
  //   ④ 静音过渡：对 pinned_->mute_remaining 单调递减（仅静音 denoised）。
  // 返回写入样本数 n（plugin 返回值）。
  size_t process(const float* in, size_t n_in, DenoiseResult* result);

  // Streamer / SSE 读 front 缓冲（架构 §4.4 数据通路）。
  // 返回的 DenoiseOutput 在下次 on_period_end() swap 前有效。
  const DenoiseOutput* get_output() const;

  // period 结尾：swap front/back + pinned_=nullptr + advance_epoch。
  void on_period_end();

  // 控制线程 housekeeper 定期驱动：释放穿越 ≥2 静止点的旧 slot。
  // 旧插件析构（ONNX session teardown / rnnoise_destroy，毫秒级）在此线程完成。
  void drain_retire();

  std::vector<std::string> list_plugins() const;

  using LatencyChangeCb = std::function<void(uint32_t)>;
  // init-only：运行期不再改，避免 std::function 读写竞态。
  void set_latency_change_cb(LatencyChangeCb cb);

 private:
  // 三路输出缓冲（front/back 双缓冲，构造时按 max_frame_
  // 分配，运行时零堆分配）。 架构 §3.4 的 DenoiseOutput{const float*
  // original/denoised/noise; size_t frame_count} 是其只读视图；DenoiseBuffer
  // 持有 owning float[] 存储。
  struct DenoiseBuffer {
    std::unique_ptr<float[]> original;  // input 副本
    std::unique_ptr<float[]> denoised;  // plugin 输出
    std::unique_ptr<float[]> noise;     // original - denoised
    size_t frame_count{0};
    explicit DenoiseBuffer(size_t cap)
        : original(new float[cap]),
          denoised(new float[cap]),
          noise(new float[cap]) {}
  };

  // 随快照发布的槽：plugin + 该 slot 的静音计数（RT 单调递减）。
  struct PluginSlot {
    std::shared_ptr<IDenoisePlugin> plugin;
    size_t mute_remaining;  // RT 线程递减，min 上限保证永不为负
    PluginSlot(std::shared_ptr<IDenoisePlugin> p, size_t mute)
        : plugin(std::move(p)), mute_remaining(mute) {}
  };

  RcuPtr<PluginSlot> rcu_ptr_;  // 原子插槽 + 静止点回收
  PluginSlot* pinned_{
      nullptr};  // 本周期裸指针快照（RT 持有，on_period_end 置空）
  RetireQueue<PluginSlot> retire_list_;  // 旧 slot 延迟释放队列
  PluginConfig current_config_;
  LatencyChangeCb latency_change_cb_;  // init-only
  // 双缓冲（BL1）：front 供 Streamer/SSE 读，back 供 RT process
  // 写，on_period_end swap。
  std::unique_ptr<DenoiseBuffer> front_;
  std::unique_ptr<DenoiseBuffer> back_;
  mutable DenoiseOutput
      front_view_;  // get_output() 返回的只读视图（指向 front_ 缓冲）
  // Task 3 简化：max_frame_ = 480（= synth::kFrameSize）。
  // PassthroughPlugin latency=0，out 容量 >= n_in = 480 足够。
  // Task 4 RNNoise（latency=480）可能需要重评估 max_frame_（非 Task 3 范围）。
  size_t max_frame_{480};
  // 50ms @48k 收敛余量（架构 §4.2 L668）。
  static constexpr size_t kConvergenceMargin = 2400;
};

}  // namespace noise

#endif  // NOISE_DENOISE_PROCESSOR_HPP_

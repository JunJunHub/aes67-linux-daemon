// daemon/noise/denoise_processor.cpp
// DenoiseProcessor 实现（架构依据：
//   docs/noise/denoise-plugin-architecture.md §4.2 L545-669）。
// arch §4.2 显示方法体 inline 在类中，此处按 Task 3 brief resolution #4
// 拆为 out-of-line（声明在 .hpp，定义在本 .cpp），逻辑保持逐字一致。
#include "denoise_processor.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "model-adapters/passthrough_plugin.hpp"

namespace noise {

DenoiseProcessor::DenoiseProcessor() {
  // 构造即装 PassthroughPlugin（in->out 直通），plugin_ 永不为空。
  // 未 switch_plugin 前 process 直通，杜绝 init 竞态/持久化配置未 apply
  // 时空指针解引用。
  auto passthrough = std::make_shared<PassthroughPlugin>();
  passthrough->init(current_config_);
  rcu_ptr_.publish(std::make_shared<PluginSlot>(std::move(passthrough), 0));
  // 双缓冲分配（max_frame_ = 480，Task 3 简化）。
  front_ = std::make_unique<DenoiseBuffer>(max_frame_);
  back_ = std::make_unique<DenoiseBuffer>(max_frame_);
}

bool DenoiseProcessor::switch_plugin(const std::string& name) {
  auto new_plugin = DenoisePluginRegistry::instance().create(name);
  if (!new_plugin)
    return false;
  if (!new_plugin->init(current_config_))
    return false;
  uint32_t latency = new_plugin->algorithmic_latency_samples();
  // 静音计数随新 slot 走：控制线程发布前置 mute，RT 线程发布后仅递减
  // 自己 pin 到的 slot（仅 RT 写该字段，无 check-then-act
  // 竞态、无无符号下溢）。
  auto new_slot = std::make_shared<PluginSlot>(std::move(new_plugin),
                                               latency + kConvergenceMargin);
  // RcuPtr::publish 返回旧 slot 的 shared_ptr（控制线程侧持有）。
  auto old = rcu_ptr_.publish(std::move(new_slot));  // 原子发布
  // 旧 slot 入 retire 队列：用当前 epoch 作为 retire_epoch。
  // 注意：arch §4.2 L573 写 retire_list_.push(std::move(old)) 但 Spec1 的
  // RetireQueue<T> API 无 push 方法，正确签名为
  // retire(std::shared_ptr<T>, uint64_t retire_epoch)（见 rcu_ptr.hpp）。
  retire_list_.retire(std::move(old), rcu_ptr_.epoch());
  if (latency_change_cb_)
    latency_change_cb_(latency);
  return true;
}

void DenoiseProcessor::set_dry_wet(float ratio) {
  // 控制线程 load rcu_ptr_（publish 的 release 已同步），路由到当前活动插件。
  // Task 3 简化：PassthroughPlugin 用普通 float dry_wet_，无并发竞争。
  // 真正的 atomic<uint32_t>+memcpy 在 Task 4 RnnoiseAdapter 验证。
  PluginSlot* slot = rcu_ptr_.load();
  if (slot && slot->plugin)
    slot->plugin->set_dry_wet(ratio);
}

bool DenoiseProcessor::set_param(const std::string& key,
                                 const std::string& value) {
  // 控制线程路由到当前活动插件；返回插件是否接受 key/value。
  PluginSlot* slot = rcu_ptr_.load();
  if (slot && slot->plugin)
    return slot->plugin->set_param(key, value);
  return false;
}

void DenoiseProcessor::on_period_begin() {
  pinned_ = rcu_ptr_.load();  // 永不为空（构造即 publish PassthroughPlugin）
}

size_t DenoiseProcessor::process(const float* in,
                                 size_t n_in,
                                 DenoiseResult* result) {
  // ① 降噪：plugin 写 back_->denoised（IDenoisePlugin 单输出契约，§2.2）
  size_t n = pinned_->plugin->process(in, n_in, back_->denoised.get(),
                                      max_frame_, result);
  // ② 原始副本：in -> back_->original（per-sensor 拷贝，全 Phase 线程安全，架构
  // §3.4）
  std::memcpy(back_->original.get(), in, n * sizeof(float));
  // ③ 噪声分量：back_->noise = original - denoised（逐样本相减）
  for (size_t i = 0; i < n; ++i)
    back_->noise[i] = back_->original[i] - back_->denoised[i];
  back_->frame_count = n;
  // ④ 静音过渡：对本 slot 的 mute 单调递减（仅 RT 写，min 上限保证永不为负）
  //    仅静音降噪路（denoised），original/noise 保留以供分析/SSE
  if (pinned_->mute_remaining > 0) {
    size_t mute = std::min(pinned_->mute_remaining, n);
    std::memset(back_->denoised.get(), 0, mute * sizeof(float));
    pinned_->mute_remaining -= mute;
  }
  return n;
}

const DenoiseOutput* DenoiseProcessor::get_output() const {
  front_view_.original = front_->original.get();
  front_view_.denoised = front_->denoised.get();
  front_view_.noise = front_->noise.get();
  front_view_.frame_count = front_->frame_count;
  return &front_view_;
}

const DenoiseOutput* DenoiseProcessor::get_current_output() const {
  // 当前 period 数据视图（back_，process() 刚写入）。
  // 用于 ①②③ 链路在 on_frame 内的 in-period 读取：NoiseManager ① process 后
  // 立即 ③ analyze 需读 current period 的 noise/denoised 分量。
  // 与 get_output()（previous period, Streamer/SSE cross-thread）互斥分工。
  back_view_.original = back_->original.get();
  back_view_.denoised = back_->denoised.get();
  back_view_.noise = back_->noise.get();
  back_view_.frame_count = back_->frame_count;
  return &back_view_;
}

void DenoiseProcessor::on_period_end() {
  // front/back swap（release 序）：本 period 写入的 back 变为 front 供下游读。
  // 用 std::swap（非原子）即可：swap（on_period_end）与 front 读（get_output，
  // 经架构 §4.4 的 capture 线程 FrameProvider 回调内 memcpy）均由 capture 线程
  // 单线程驱动，无跨线程竞争。HTTP 线程只读 Streamer 自有 ring（§4.4），
  // 不直接读 front_。架构 §3.4 的 "std::atomic<DenoiseOutput*>" 表述为更早的
  // 宽松描述，以本节单线程 swap + §11 风险17 的"period 回调内 memcpy
  // front"为准。
  std::swap(front_, back_);
  pinned_ = nullptr;  // 释放本周期裸指针（Taste 决策1，不再持 shared_ptr）
  rcu_ptr_.advance_epoch();  // 通知 housekeeper：一个静止点已过
}

void DenoiseProcessor::drain_retire() {
  // housekeeper（控制线程定期驱动）：释放穿越 ≥2 静止点的旧 slot。
  // 旧插件析构（ONNX session teardown / rnnoise_destroy，毫秒级）在此线程完成。
  //
  // RetireQueue::reclaim_older_than(current_epoch) 回收条件：
  //   retire_epoch + 1 < current_epoch  <=>  current_epoch >= retire_epoch + 2
  // 即 retire at epoch E 的 slot 在 epoch >= E+2 时回收（2-epoch grace）。
  //
  // Task 7 carry-forward 修复（Task 3 Important bug）：
  // 原实现传 epoch() - 1，当 epoch()==0 时下溢为 UINT64_MAX，导致所有条目
  // （含刚入队、retire_epoch==0 的）被立即回收。若 switch_plugin 后立即调
  // drain_retire（如 Task 7 add_sensor/switch_plugin 所为），RT 线程可能仍
  // 持有旧 pinned_ 裸指针 -> use-after-free。
  // 修复：传 epoch()，给 2-epoch grace（与 Task 1 NoiseManager
  // reclaim_older_than(epoch()) 一致）。slot retired at E 被 RT pin 直到
  // on_period_end at E+1（advance_epoch 推进到 E+1 -> E+2），在 epoch >= E+2
  // 回收安全（RT 已穿越 2 个静止点，旧裸指针不再被持有）。
  retire_list_.reclaim_older_than(rcu_ptr_.epoch());
}

std::vector<std::string> DenoiseProcessor::list_plugins() const {
  return DenoisePluginRegistry::instance().list();
}

void DenoiseProcessor::set_latency_change_cb(LatencyChangeCb cb) {
  // init-only，运行期不再改，避免 std::function 读写竞态。
  latency_change_cb_ = std::move(cb);
}

}  // namespace noise

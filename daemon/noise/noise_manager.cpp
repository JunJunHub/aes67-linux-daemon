// daemon/noise/noise_manager.cpp
// 架构依据：docs/noise/architecture-design.md §3.7。
#include "noise_manager.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace noise {

NoiseManager::NoiseManager(NoiseAudioBridge& bridge) : bridge_(bridge) {}

NoiseManager::~NoiseManager() {
  // 等待 housekeeper async 任务完成，避免 use-after-free（任务捕获 this）。
  if (reset_future_.valid()) {
    reset_future_.wait();
  }
}

bool NoiseManager::add_sensor(uint8_t sensor_id,
                              uint8_t sink_id,
                              const NoiseSensorConfig& /*cfg*/) {
  // 控制线程：COW 复制当前表 -> 加 sensor -> 原子 publish -> 旧表入 retire
  std::lock_guard<std::mutex> lock(ctrl_mutex_);
  const SensorTable* current = sensor_table_.load();
  auto new_table = std::make_shared<SensorTable>(*current);
  (*new_table)[sensor_id] = SensorContext{
      sink_id,
      std::make_shared<StubProcessor>(),
      std::make_shared<NoiseMetricsStub>(),
  };
  auto old = sensor_table_.publish(std::move(new_table));
  retire_queue_.retire(std::move(old), sensor_table_.epoch());
  // 控制线程回收（勿在 RT 路径调，reclaim_older_than 持 mutex）
  retire_queue_.reclaim_older_than(sensor_table_.epoch());
  return true;
}

void NoiseManager::on_period_begin() {
  // period 顶部 load 快照，整 period 内 on_frame 复用
  pinned_table_ = sensor_table_.load();
  for (auto& [id, ctx] : *pinned_table_) {
    (void)id;
    if (ctx.stub)
      ctx.stub->on_period_begin();
    if (ctx.metrics)
      ctx.metrics->on_period_begin();
  }
}

void NoiseManager::on_frame(uint8_t sink_id,
                            const float* frames,
                            size_t frame_size) {
  // PTP 未锁时跳过处理（arch §3.7 L862 ②）
  if (!ptp_locked_.load())
    return;
  if (pinned_table_ == nullptr)
    return;
  // 按 sink_id 路由到对应 sensor（1.4b stub 调 process）
  for (auto& [id, ctx] : *pinned_table_) {
    (void)id;
    if (ctx.sink_id == sink_id && ctx.stub) {
      ctx.stub->process(sink_id, frames, frame_size);
      break;
    }
  }
}

void NoiseManager::on_period_end() {
  if (pinned_table_ != nullptr) {
    for (auto& [id, ctx] : *pinned_table_) {
      (void)id;
      if (ctx.stub)
        ctx.stub->on_period_end();
      if (ctx.metrics)
        ctx.metrics->on_period_end();
    }
  }
  pinned_table_ = nullptr;
  sensor_table_.advance_epoch();
}

void NoiseManager::on_ptp_unlocked() {
  // 不直接调 plugin->reset()（会与 RT process() 竞态）。
  // 置位后由 housekeeper 延迟 reset（arch §3.7 L862）。
  ptp_locked_.store(false);
  reset_pending_.store(true);
  // Task 1 简化：stub 无插件可 reset，仅延迟清标志。
  // Task 7 替换为 plugin->reset() + PcmCaptureService join（Spec3 path A）。
  // #5: Task 1 最小实现：重复 on_ptp_unlocked() 会阻塞 ≤200ms（旧 future 析构
  // 等待）。Task 7 改为独立 housekeeper 线程。
  reset_future_ = std::async(std::launch::async, [this]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    reset_pending_.store(false);
  });
}

size_t NoiseManager::sensor_count_for_test() const {
  const SensorTable* tbl = sensor_table_.load();
  return tbl ? tbl->size() : 0;
}

size_t NoiseManager::stub_call_count_for_test(uint8_t sensor_id) const {
  const SensorTable* tbl = sensor_table_.load();
  if (tbl == nullptr)
    return 0;
  auto it = tbl->find(sensor_id);
  if (it == tbl->end() || it->second.stub == nullptr)
    return 0;
  return it->second.stub->call_count;
}

}  // namespace noise

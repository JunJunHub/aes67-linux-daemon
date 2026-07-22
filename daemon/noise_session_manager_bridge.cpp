// daemon/noise_session_manager_bridge.cpp
#include "noise_session_manager_bridge.hpp"

#include "pcm_capture_service.hpp"
#include "noise/audio_capture.hpp"

NoiseSessionManagerBridge::NoiseSessionManagerBridge(
    std::shared_ptr<PcmCaptureService> pcm_capture)
    : pcm_capture_(std::move(pcm_capture)),
      convert_buffer_(kMaxPeriodSamples * kMaxChannels, 0.0f) {}

NoiseSessionManagerBridge::~NoiseSessionManagerBridge() {
  // 注销 PcmCaptureService provider（若已注册）。capture 线程在
  // PcmCaptureService::terminate() 后已停止，此处仅清理注册表。
  if (pcm_registered_ && pcm_capture_) {
    pcm_capture_->unregister_provider(pcm_token_);
    pcm_registered_ = false;
  }
}

void NoiseSessionManagerBridge::register_frame_provider(
    uint8_t sink_id,
    const std::vector<uint8_t>& channel_map,
    FrameProvider provider) {
  // 控制线程：COW 复制当前表 -> 加/覆盖 sink entry -> 原子 publish -> retire
  const SinkEntryTable* current = sink_entries_.load();
  auto new_table = std::make_shared<SinkEntryTable>(*current);
  SinkEntry entry;
  entry.channel_map = channel_map;
  entry.provider = std::move(provider);
  (*new_table)[sink_id] = std::move(entry);
  auto old = sink_entries_.publish(std::move(new_table));
  sink_retire_.retire(std::move(old), sink_entries_.epoch());
  sink_retire_.reclaim_older_than(sink_entries_.epoch());

  // 首次注册时，向 PcmCaptureService 注册一个 provider 回调。
  // 该回调遍历所有已注册 sink entry，做 S16->float 解复用 + period 生命周期。
  if (!pcm_registered_ && pcm_capture_) {
    pcm_token_ = pcm_capture_->register_provider(
        [this](const uint8_t* pcm, size_t frame_count, uint8_t channels,
               uint32_t rate) {
          this->on_pcm_frame(pcm, frame_count, channels, rate);
        });
    pcm_registered_ = true;
  }
}

void NoiseSessionManagerBridge::unregister_frame_provider(uint8_t sink_id) {
  // 控制线程：COW 复制 -> erase -> publish -> retire
  const SinkEntryTable* current = sink_entries_.load();
  if (current->find(sink_id) == current->end())
    return;
  auto new_table = std::make_shared<SinkEntryTable>(*current);
  new_table->erase(sink_id);
  auto old = sink_entries_.publish(std::move(new_table));
  sink_retire_.retire(std::move(old), sink_entries_.epoch());
  sink_retire_.reclaim_older_than(sink_entries_.epoch());
}

bool NoiseSessionManagerBridge::is_sink_receiving(uint8_t sink_id) const {
  return pcm_capture_ && pcm_capture_->is_sink_receiving(sink_id);
}

uint32_t NoiseSessionManagerBridge::get_sample_rate() const {
  return pcm_capture_ ? pcm_capture_->get_sample_rate() : 0;
}

uint8_t NoiseSessionManagerBridge::get_sink_channel_count(
    uint8_t sink_id) const {
  return pcm_capture_ ? pcm_capture_->get_sink_channel_count(sink_id) : 0;
}

void NoiseSessionManagerBridge::set_ptp_status_callback(PtpStatusCallback cb) {
  ptp_cb_ = std::move(cb);
}

void NoiseSessionManagerBridge::set_period_lifecycle_callbacks(
    PeriodBeginCallback begin,
    PeriodEndCallback end) {
  period_begin_cb_ = std::move(begin);
  period_end_cb_ = std::move(end);
}

void NoiseSessionManagerBridge::set_sink_add_callback(SinkChangeCallback cb) {
  sink_add_cb_ = std::move(cb);
}
void NoiseSessionManagerBridge::set_sink_remove_callback(
    SinkChangeCallback cb) {
  sink_remove_cb_ = std::move(cb);
}

// PcmCaptureService 全局帧回调（arch §4.2 on_pcm_frame）。
// period 顶部 begin -> per-sink demux + 480 子帧分发 -> period 结尾 end。
void NoiseSessionManagerBridge::on_pcm_frame(const uint8_t* interleaved_pcm,
                                             size_t frame_count,
                                             uint8_t channels,
                                             uint32_t /*sample_rate*/) {
  // §11 风险18：容量断言
  if (frame_count * channels > kMaxPeriodSamples * kMaxChannels)
    return;  // 超容量丢弃（驱动异常，不越界写）

  // 1. period 顶部：NoiseManager::on_period_begin（ONCE，全局）
  if (period_begin_cb_)
    period_begin_cb_();

  // 2. per-sink demux + 480 子帧分发
  const SinkEntryTable* snapshot = sink_entries_.load();
  const int16_t* src = reinterpret_cast<const int16_t*>(interleaved_pcm);
  for (const auto& [sink_id, entry] : *snapshot) {
    if (entry.channel_map.empty())
      continue;
    // Phase 1：单通道解复用（channel_map[0]），channels 恒为 1（arch §4.2
    // "channels 恒为 1"）。
    const uint8_t ch_idx = entry.channel_map[0];
    for (size_t off = 0; off + kSubFrame <= frame_count; off += kSubFrame) {
      // S16_LE 交错 -> float 单通道（归一化到 [-1, 1]）
      for (size_t i = 0; i < kSubFrame; ++i) {
        convert_buffer_[i] =
            static_cast<float>(src[(off + i) * channels + ch_idx]) / 32768.0f;
      }
      entry.provider(sink_id, convert_buffer_.data(), kSubFrame, 1);
    }
    // carry-over（frame_count % kSubFrame 样本）丢弃（Phase 1，arch §6.2.1）。
  }

  // 3. period 结尾：NoiseManager::on_period_end（ONCE，全局）
  if (period_end_cb_)
    period_end_cb_();
}

// uint8_t(S16_LE 交错) -> float 单通道解复用。
void NoiseSessionManagerBridge::test_demux_for_test(const uint8_t* interleaved,
                                                    size_t samples,
                                                    uint8_t channels,
                                                    uint8_t ch_index,
                                                    AudioCapture& cap) {
  // §11 风险18：容量断言
  if (samples * channels > kMaxPeriodSamples * kMaxChannels) {
    return;  // 超容量丢弃（驱动异常，Spec1 不越界写）
  }
  const int16_t* src = reinterpret_cast<const int16_t*>(interleaved);
  // 提取 ch_index 通道到 convert_buffer_ 前 samples 个位置
  for (size_t i = 0; i < samples; ++i) {
    convert_buffer_[i] =
        static_cast<float>(src[i * channels + ch_index]) / 32768.0f;
  }
  cap.on_period_begin();
  cap.on_frame(0, convert_buffer_.data(), samples, 1);
  cap.on_period_end();
}

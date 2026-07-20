// daemon/noise_session_manager_bridge.cpp
#include "noise_session_manager_bridge.hpp"

#include <cstring>

#include "pcm_capture_service.hpp"
#include "noise/audio_capture.hpp"

NoiseSessionManagerBridge::NoiseSessionManagerBridge(
    std::shared_ptr<PcmCaptureService> pcm_capture)
    : pcm_capture_(std::move(pcm_capture)),
      convert_buffer_(kMaxPeriodSamples * kMaxChannels, 0.0f) {}

NoiseSessionManagerBridge::~NoiseSessionManagerBridge() = default;

void NoiseSessionManagerBridge::register_frame_provider(
    uint8_t /*sink_id*/,
    const std::vector<uint8_t>& /*channel_map*/,
    FrameProvider /*provider*/) {
  // Spec1：委托 PcmCaptureService（Spec2 NoiseManager 经此注册）。
  // 完整 channel_map 提取在 Spec2 1.4b 接入时实现，Spec1 仅留接口。
}

void NoiseSessionManagerBridge::unregister_frame_provider(uint8_t /*sink_id*/) {
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
void NoiseSessionManagerBridge::set_sink_add_callback(SinkChangeCallback cb) {
  sink_add_cb_ = std::move(cb);
}
void NoiseSessionManagerBridge::set_sink_remove_callback(
    SinkChangeCallback cb) {
  sink_remove_cb_ = std::move(cb);
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

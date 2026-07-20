// daemon/noise/audio_capture.cpp
#include "audio_capture.hpp"

#include <utility>

bool AudioCapture::start(uint8_t /*sink_id*/,
                         noise::NoiseAudioBridge& /*bridge*/) {
  // Spec1：Bridge 侧 register_frame_provider 由 NoiseSessionManagerBridge
  // 负责。 AudioCapture 仅持回调，帧经 Bridge 解复用后调 on_frame。
  running_ = true;
  return true;
}

bool AudioCapture::stop() {
  running_ = false;
  return true;
}

void AudioCapture::register_callback(FrameCallback cb) {
  frame_cb_ = std::move(cb);
}

void AudioCapture::register_period_callbacks(PeriodBeginCallback begin,
                                             PeriodEndCallback end) {
  period_begin_cb_ = std::move(begin);
  period_end_cb_ = std::move(end);
}

bool AudioCapture::is_running() const {
  return running_;
}

void AudioCapture::on_period_begin() {
  if (period_begin_cb_)
    period_begin_cb_();
}

void AudioCapture::on_period_end() {
  if (period_end_cb_)
    period_end_cb_();
}

void AudioCapture::on_frame(uint8_t /*sink_id*/,
                            const float* frames,
                            size_t frame_size,
                            uint8_t channels) {
  if (frame_cb_)
    frame_cb_(frames, frame_size, channels);
}

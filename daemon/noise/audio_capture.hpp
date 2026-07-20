// daemon/noise/audio_capture.hpp
// 架构依据：docs/noise/architecture-design.md §3.1。
// 决策4：下游经 std::function callback 解耦，不前向依赖 Spec2 的 NoiseManager。
#ifndef NOISE_AUDIO_CAPTURE_HPP_
#define NOISE_AUDIO_CAPTURE_HPP_

#include <cstdint>
#include <functional>

#include "noise_audio_bridge.hpp"

class AudioCapture {
 public:
  using FrameCallback = std::function<
      void(const float* frames, size_t frame_size, uint8_t channel_count)>;
  using PeriodBeginCallback = std::function<void()>;
  using PeriodEndCallback = std::function<void()>;

  bool start(uint8_t sink_id, noise::NoiseAudioBridge& bridge);
  bool stop();
  void register_callback(FrameCallback cb);
  void register_period_callbacks(PeriodBeginCallback begin,
                                 PeriodEndCallback end);
  bool is_running() const;

  // Bridge 在 ALSA period 边界调用（§3.1）：分发前 begin，分发后 end。
  void on_period_begin();
  void on_period_end();
  // Bridge 调用：分发一帧单通道 float。
  void on_frame(uint8_t sink_id,
                const float* frames,
                size_t frame_size,
                uint8_t channels);

 private:
  FrameCallback frame_cb_;
  PeriodBeginCallback period_begin_cb_;
  PeriodEndCallback period_end_cb_;
  bool running_{false};
};

#endif  // NOISE_AUDIO_CAPTURE_HPP_

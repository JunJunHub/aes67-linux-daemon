// daemon/noise_session_manager_bridge.hpp
// 架构依据：docs/noise/architecture-design.md §4.2。
#ifndef DAEMON_NOISE_SESSION_MANAGER_BRIDGE_HPP_
#define DAEMON_NOISE_SESSION_MANAGER_BRIDGE_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "noise/noise_audio_bridge.hpp"

class PcmCaptureService;
class AudioCapture;

class NoiseSessionManagerBridge : public noise::NoiseAudioBridge {
 public:
  explicit NoiseSessionManagerBridge(
      std::shared_ptr<PcmCaptureService> pcm_capture);
  ~NoiseSessionManagerBridge() override;

  void register_frame_provider(uint8_t sink_id,
                               const std::vector<uint8_t>& channel_map,
                               FrameProvider provider) override;
  void unregister_frame_provider(uint8_t sink_id) override;

  bool is_sink_receiving(uint8_t sink_id) const override;
  uint32_t get_sample_rate() const override;
  uint8_t get_sink_channel_count(uint8_t sink_id) const override;

  void set_ptp_status_callback(PtpStatusCallback cb) override;
  void set_sink_add_callback(SinkChangeCallback cb) override;
  void set_sink_remove_callback(SinkChangeCallback cb) override;

  // 测试专用：把交错 uint8_t(S16_LE) 转 float 单通道后喂 AudioCapture 回调。
  void test_demux_for_test(const uint8_t* interleaved,
                           size_t samples,
                           uint8_t channels,
                           uint8_t ch_index,
                           AudioCapture& cap);

 private:
  std::shared_ptr<PcmCaptureService> pcm_capture_;
  PtpStatusCallback ptp_cb_;
  SinkChangeCallback sink_add_cb_;
  SinkChangeCallback sink_remove_cb_;
  // convert_buffer_：按 max_period × max_channels × sizeof(float) 分配（§11
  // 风险18）
  std::vector<float> convert_buffer_;
  static constexpr size_t kMaxPeriodSamples = 6144;
  static constexpr uint8_t kMaxChannels = 8;
};

#endif  // DAEMON_NOISE_SESSION_MANAGER_BRIDGE_HPP_

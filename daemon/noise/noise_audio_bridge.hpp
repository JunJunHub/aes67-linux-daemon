// daemon/noise/noise_audio_bridge.hpp
// 架构依据：docs/noise/architecture-design.md §4.1。
#ifndef NOISE_NOISE_AUDIO_BRIDGE_HPP_
#define NOISE_NOISE_AUDIO_BRIDGE_HPP_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace noise {

class NoiseAudioBridge {
 public:
  virtual ~NoiseAudioBridge() = default;

  using FrameProvider = std::function<void(uint8_t sink_id,
                                           const float* frames,
                                           size_t frame_size,
                                           uint8_t channels)>;
  virtual void register_frame_provider(uint8_t sink_id,
                                       const std::vector<uint8_t>& channel_map,
                                       FrameProvider provider) = 0;
  virtual void unregister_frame_provider(uint8_t sink_id) = 0;

  virtual bool is_sink_receiving(uint8_t sink_id) const = 0;
  virtual uint32_t get_sample_rate() const = 0;
  virtual uint8_t get_sink_channel_count(uint8_t sink_id) const = 0;

  using PtpStatusCallback = std::function<void(const std::string& status)>;
  virtual void set_ptp_status_callback(PtpStatusCallback cb) = 0;
  using SinkChangeCallback = std::function<void(uint8_t sink_id)>;
  virtual void set_sink_add_callback(SinkChangeCallback cb) = 0;
  virtual void set_sink_remove_callback(SinkChangeCallback cb) = 0;
};

}  // namespace noise

#endif  // NOISE_NOISE_AUDIO_BRIDGE_HPP_

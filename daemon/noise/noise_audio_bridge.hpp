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
  // 查询 sink 的 channel_map（来自 SessionManager::StreamSink.map）。
  // 真实场景：用户通过 PUT /api/sink/:id 配置 map 字段。
  // FAKE 场景：sink 配置的 map 指向 fake_pcm_source 目录下对应 channel 的 WAV。
  // 返回空 vector 表示 sink 不存在或未配置 map（调用方用 {0} 兜底）。
  virtual std::vector<uint8_t> get_sink_channel_map(uint8_t sink_id) const = 0;

  using PtpStatusCallback = std::function<void(const std::string& status)>;
  virtual void set_ptp_status_callback(PtpStatusCallback cb) = 0;
  // Spec3 T8b（C2 修复）：period 生命周期回调。Bridge 在 PcmCaptureService
  // provider 回调内调用：begin（period 顶部，demux 前）+ end（period 结尾，
  // demux 后）。NoiseManager 在构造时设置，使 Bridge 能驱动
  // on_period_begin/on_period_end（每个 ALSA period 恰好一次，NoiseManager
  // 全局，非 per-sink）。修复 C2：此前 register_frame_provider 是 stub，
  // 生产 pipeline 永不运行 on_frame。
  using PeriodBeginCallback = std::function<void()>;
  using PeriodEndCallback = std::function<void()>;
  virtual void set_period_lifecycle_callbacks(PeriodBeginCallback begin,
                                              PeriodEndCallback end) = 0;
  using SinkChangeCallback = std::function<void(uint8_t sink_id)>;
  virtual void set_sink_add_callback(SinkChangeCallback cb) = 0;
  virtual void set_sink_remove_callback(SinkChangeCallback cb) = 0;
};

}  // namespace noise

#endif  // NOISE_NOISE_AUDIO_BRIDGE_HPP_

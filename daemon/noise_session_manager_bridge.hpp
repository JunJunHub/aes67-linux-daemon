// daemon/noise_session_manager_bridge.hpp
// 架构依据：docs/noise/architecture-design.md §4.2。
#ifndef DAEMON_NOISE_SESSION_MANAGER_BRIDGE_HPP_
#define DAEMON_NOISE_SESSION_MANAGER_BRIDGE_HPP_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "noise/noise_audio_bridge.hpp"
#include "noise/rcu_ptr.hpp"

class PcmCaptureService;

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
  void set_period_lifecycle_callbacks(PeriodBeginCallback begin,
                                      PeriodEndCallback end) override;
  void set_sink_add_callback(SinkChangeCallback cb) override;
  void set_sink_remove_callback(SinkChangeCallback cb) override;

  // 测试专用：把交错 uint8_t(S16_LE) 转 float 单通道后调 cb 帧回调。
  // 生产路径不经 AudioCapture（C2 fix 直接 on_pcm_frame -> entry.provider
  // -> NoiseManager::on_frame）；AudioCapture 已删，此 helper 改用回调。
  void test_demux_for_test(
      const uint8_t* interleaved,
      size_t samples,
      uint8_t channels,
      uint8_t ch_index,
      const std::function<void(const float*, size_t, uint8_t)>& cb);

 private:
  // PcmCaptureService 全局帧回调（注册为 provider）。
  // 一个 ALSA period 调一次。时序（arch §4.2 BL2）：
  //   1. 入口：period_begin_cb_()  -> NoiseManager::on_period_begin
  //   2. uint8_t->float 转换 + 按 channel_map 解复用 per-sink
  //   3. 对每个 sink 的 480 帧子帧调 FrameProvider -> NoiseManager::on_frame
  //   4. 出口：period_end_cb_()    -> NoiseManager::on_period_end
  void on_pcm_frame(const uint8_t* interleaved_pcm,
                    size_t frame_count,
                    uint8_t channels,
                    uint32_t /*sample_rate*/);

  std::shared_ptr<PcmCaptureService> pcm_capture_;
  PtpStatusCallback ptp_cb_;
  PeriodBeginCallback period_begin_cb_;
  PeriodEndCallback period_end_cb_;
  SinkChangeCallback sink_add_cb_;
  SinkChangeCallback sink_remove_cb_;

  // per-sink 注册表：sink_id -> {channel_map, FrameProvider}（arch §4.2）。
  // 控制线程 COW 建新表原子换，capture 线程 period 顶部 load 快照复用。
  struct SinkEntry {
    std::vector<uint8_t> channel_map;
    FrameProvider provider;
  };
  using SinkEntryTable = std::map<uint8_t, SinkEntry>;
  noise::RcuPtr<const SinkEntryTable> sink_entries_{
      std::make_shared<SinkEntryTable>()};
  noise::RetireQueue<const SinkEntryTable> sink_retire_;

  // PcmCaptureService provider token（首次 register_frame_provider 时注册，
  // 析构时注销）。0 = 未注册。
  uint32_t pcm_token_{0};
  bool pcm_registered_{false};

  // convert_buffer_：按 max_period × max_channels × sizeof(float) 分配（§11
  // 风险18）。on_pcm_frame 用前 kSubFrame 个位置做 S16->float 解复用。
  std::vector<float> convert_buffer_;
  static constexpr size_t kMaxPeriodSamples = 6144;
  static constexpr uint8_t kMaxChannels = 8;
  // DenoiseProcessor max_frame_（480 样本/子帧）。
  static constexpr size_t kSubFrame = 480;
};

#endif  // DAEMON_NOISE_SESSION_MANAGER_BRIDGE_HPP_

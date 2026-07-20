// daemon/pcm_capture_service.hpp
// PCM 分发基础设施：独占 ALSA capture + FrameProvider 分发 + PTP observer。
// 架构依据：docs/noise/architecture-design.md §4.3。
// 编译条件：WITH_NOISE=ON（Taste 决策2，WITH_NOISE=OFF 时 Streamer 沿用上游
// ALSA 路径）。
#ifndef DAEMON_PCM_CAPTURE_SERVICE_HPP_
#define DAEMON_PCM_CAPTURE_SERVICE_HPP_

#include <alsa/asoundlib.h>
#include <atomic>
#include <cstdint>
#include <future>
#include <functional>
#include <memory>
#include <string>

#ifdef _USE_NOISE_
#include "noise/rcu_ptr.hpp"
#include "session_manager.hpp"
#endif

class Config;
class SessionManager;

class PcmCaptureService
#ifdef _USE_NOISE_
    : public std::enable_shared_from_this<PcmCaptureService>
#endif
{
 public:
  using FrameProvider = std::function<void(const uint8_t* interleaved_pcm,
                                           size_t frame_count,
                                           uint8_t channels,
                                           uint32_t sample_rate)>;
  using ProviderToken = uint32_t;

  static std::shared_ptr<PcmCaptureService> create(
      std::shared_ptr<SessionManager> session_manager,
      std::shared_ptr<Config> config);

  bool init();  // 注册 PTP observer + Sink observer
  bool terminate();

  ProviderToken register_provider(FrameProvider provider);
  void unregister_provider(ProviderToken token);

  bool is_sink_receiving(uint8_t sink_id) const;
  uint32_t get_sample_rate() const;
  uint8_t get_sink_channel_count(uint8_t sink_id) const;
  bool is_capturing() const;

  // ── 测试专用（FAKE_DRIVER 下驱动 fake_capture_loop，不依赖
  // SessionManager）──
  static std::shared_ptr<PcmCaptureService> create_for_test();
  void start_fake_for_test(uint32_t sample_rate, uint8_t channels);
  void stop_for_test();

 private:
  explicit PcmCaptureService(std::shared_ptr<SessionManager> session_manager,
                             std::shared_ptr<Config> config)
      : session_manager_(std::move(session_manager)),
        config_(std::move(config)) {}
  explicit PcmCaptureService() = default;  // 测试用

  // 返回 bool 以匹配 SessionManager::PtpStatusObserver / SinkObserver 签名
  // （参考 streamer.cpp:38-50 / streamer.hpp:88-90）
  bool on_ptp_status_change(const std::string& status);
  bool on_sink_add(uint8_t id);
  bool on_sink_remove(uint8_t id);

  bool start_capture();
  bool stop_capture();
  void capture_loop();       // 真实 ALSA（PTP locked 时运行）
  void fake_capture_loop();  // FAKE_DRIVER：从 WAV/静音帧读取

  void dispatch(const uint8_t* pcm,
                size_t frame_count,
                uint8_t channels,
                uint32_t rate);
  static constexpr uint32_t kPeriodSamples = 6144;

  std::shared_ptr<SessionManager> session_manager_;
  std::shared_ptr<Config> config_;
  // 仅 _USE_NOISE_ 用。原子指针：capture_loop（写，snd_pcm_open 后 store）
  // 与 stop_capture（读/写，snd_pcm_drop/close/置 null）跨线程同步（§3.8
  // 同步协议， 消除 brief 裸指针 data race）。stop_capture 的 drop+close
  // 中断阻塞 readi 仍 保留（§11 风险19 设计意图）。
  std::atomic<snd_pcm_t*> capture_handle_{nullptr};
  std::atomic_bool running_{false};
  std::atomic_bool stop_flag_{false};
  std::future<void> capture_future_;
  std::string fake_pcm_source_;

#ifdef _USE_NOISE_
  // FrameProvider 表：控制线程 COW 建新 vector 原子换，capture 线程 period 顶部
  // load 快照。
  struct ProviderEntry {
    ProviderToken token;
    FrameProvider provider;
  };
  noise::RcuPtr<std::vector<ProviderEntry>> providers_;
  std::atomic<ProviderToken> next_token_{1};
  noise::RetireQueue<std::vector<ProviderEntry>> providers_retire_;
  // 测试用 fake 参数
  uint32_t test_rate_{48000};
  uint8_t test_channels_{2};
#endif
};

#endif  // DAEMON_PCM_CAPTURE_SERVICE_HPP_

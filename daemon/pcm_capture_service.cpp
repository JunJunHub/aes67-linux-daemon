// daemon/pcm_capture_service.cpp
// 架构依据：docs/noise/architecture-design.md §4.3 / §11 风险19/21。
#ifdef _USE_NOISE_

#include "pcm_capture_service.hpp"

#include <alsa/asoundlib.h>
#include <boost/log/trivial.hpp>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>
#include <thread>
#include <vector>

#include "config.hpp"
#include "noise/noise_template_db.hpp"  // parse_wav_pcm16_48k_mono（Spec3 T8）
#include "session_manager.hpp"

constexpr uint32_t PcmCaptureService::kPeriodSamples;

std::shared_ptr<PcmCaptureService> PcmCaptureService::create(
    std::shared_ptr<SessionManager> session_manager,
    std::shared_ptr<Config> config) {
  // Spec3 T8b（C2 修复）：publish 初始空 provider 表，使 register_provider
  // 在 init() 前可用（load_status -> add_sensor -> register_frame_provider
  // -> register_provider 发生在 init() 前）。与 create_for_test 同模式。
  auto svc = std::shared_ptr<PcmCaptureService>(
      new PcmCaptureService(std::move(session_manager), std::move(config)));
  svc->providers_.publish(std::make_shared<std::vector<ProviderEntry>>());
  return svc;
}

std::shared_ptr<PcmCaptureService> PcmCaptureService::create_for_test() {
  // 测试用：无 SessionManager/Config，直接驱动 fake_capture_loop。
  // publish 初始空 provider 表，满足 RcuPtr::load() 永不为空契约
  // （production 路径由 init() 完成，测试路径不走 init()）。
  auto svc = std::shared_ptr<PcmCaptureService>(new PcmCaptureService());
  svc->providers_.publish(std::make_shared<std::vector<ProviderEntry>>());
  return svc;
}

std::shared_ptr<PcmCaptureService>
PcmCaptureService::create_for_test_with_config(std::shared_ptr<Config> config) {
  // Spec3 Task 8 E2E：注入 Config 使 fake_capture_loop 能读 fake_pcm_source
  // WAV。 不走 init()（无 SessionManager），测试直接调 start_fake_for_test。
  auto svc = std::shared_ptr<PcmCaptureService>(new PcmCaptureService());
  svc->config_ = std::move(config);
  svc->providers_.publish(std::make_shared<std::vector<ProviderEntry>>());
  return svc;
}

bool PcmCaptureService::init() {
  if (!session_manager_ || !config_)
    return false;
  session_manager_->add_ptp_status_observer(std::bind(
      &PcmCaptureService::on_ptp_status_change, this, std::placeholders::_1));
  session_manager_->add_sink_observer(
      SessionManager::SinkObserverType::add_sink,
      std::bind(&PcmCaptureService::on_sink_add, this, std::placeholders::_1));
  session_manager_->add_sink_observer(
      SessionManager::SinkObserverType::remove_sink,
      std::bind(&PcmCaptureService::on_sink_remove, this,
                std::placeholders::_1));
  // 注：不再此处 publish 空 provider 表。create() 已建立永不为空契约
  // （L31）。load_status 在 init() 之前运行，会经 add_sensor ->
  // register_frame_provider -> register_provider 注册 provider；若此处再
  // publish 空表会清空已注册的 provider，致 dispatch 无 provider 可调，
  // on_pcm_frame/on_frame 永不运行（Spec3 T8b C2 修复）。
  PTPStatus status;
  session_manager_->get_ptp_status(status);
  on_ptp_status_change(status.status);
  return true;
}

bool PcmCaptureService::terminate() {
  stop_capture();
  return true;
}

PcmCaptureService::ProviderToken PcmCaptureService::register_provider(
    FrameProvider provider) {
  ProviderToken token = next_token_.fetch_add(1);
  // COW：复制当前表 -> 追加 -> 原子换
  auto current = providers_.load();
  auto new_table = std::make_shared<std::vector<ProviderEntry>>(*current);
  new_table->push_back({token, std::move(provider)});
  auto old = providers_.publish(new_table);
  providers_retire_.retire(std::move(old), providers_.epoch());
  // 控制线程驱动回收（§3.8 housekeeper，event-driven on publish，YAGNI 无需
  // 独立 housekeeper 线程）。2-epoch 保证：retire 入队后若 RT 线程尚未穿越
  // 2 静止点，reclaim 检查 epoch 差不足 2 -> no-op，更晚的 register/unregister
  // 回收。
  providers_retire_.reclaim_older_than(providers_.epoch());
  return token;
}

void PcmCaptureService::unregister_provider(ProviderToken token) {
  auto current = providers_.load();
  auto new_table = std::make_shared<std::vector<ProviderEntry>>();
  new_table->reserve(current->size());
  for (const auto& e : *current) {
    if (e.token != token)
      new_table->push_back(e);
  }
  auto old = providers_.publish(new_table);
  providers_retire_.retire(std::move(old), providers_.epoch());
  // 同 register_provider：控制线程驱动回收（§3.8 housekeeper）。
  providers_retire_.reclaim_older_than(providers_.epoch());
}

void PcmCaptureService::dispatch(const uint8_t* pcm,
                                 size_t frame_count,
                                 uint8_t channels,
                                 uint32_t rate) {
  // period 顶部 load 快照（整 period 复用，不每帧原子操作）
  auto snapshot = providers_.load();
  for (const auto& e : *snapshot) {
    e.provider(pcm, frame_count, channels, rate);
  }
}

bool PcmCaptureService::is_sink_receiving(uint8_t sink_id) const {
  // SessionManager 无 is_sink_receiving，用 get_sink_status + SinkStreamStatus
  // （session_manager.hpp:70 is_receiving_rtp_packet / :166 get_sink_status）
  if (!session_manager_)
    return false;
  SinkStreamStatus status;
  if (session_manager_->get_sink_status(sink_id, status))
    return false;
  return status.is_receiving_rtp_packet;
}

uint32_t PcmCaptureService::get_sample_rate() const {
  return config_ ? config_->get_sample_rate() : test_rate_;
}

uint8_t PcmCaptureService::get_sink_channel_count(uint8_t sink_id) const {
  // Spec1 最小实现：返回 streamer channels（Phase 1 限定全通道分发）
  return config_ ? config_->get_streamer_channels() : test_channels_;
}

bool PcmCaptureService::is_capturing() const {
  return running_.load();
}

bool PcmCaptureService::on_ptp_status_change(const std::string& status) {
  BOOST_LOG_TRIVIAL(info) << "PcmCaptureService: ptp status " << status;
#ifdef _USE_FAKE_DRIVER_
  // FAKE_DRIVER：PTP 永远 UNLOCKED，忽略 PTP 状态直接起 fake loop（§4.3）。
  // 但需 forward "locked" 给 NoiseManager 使 pipeline 运行（C1 修复）：
  // fake "unlocked" 在功能上等价于 locked（无真实 PTP 可失锁）。
  if (!running_.load() && status == "unlocked") {
    bool ok = start_capture();
    if (ptp_status_forward_cb_)
      ptp_status_forward_cb_("locked");
    return ok;
  }
  return true;
#else
  if (status == "locked") {
    bool ok = start_capture();
    // Spec3 Task 6b（C1 修复）：forward "locked" -> NoiseManager::on_ptp_locked
    // 启用 pipeline。
    if (ptp_status_forward_cb_)
      ptp_status_forward_cb_("locked");
    return ok;
  } else if (status == "unlocked") {
    // arch §3.7 L862 path A：先 forward "unlocked" 置 reset_pending_=true，
    // 再 stop_capture join capture 线程 -> capture_joined_cb_ 回调
    // -> on_capture_thread_joined（控制线程，capture 已静止 -> 安全 reset）。
    if (ptp_status_forward_cb_)
      ptp_status_forward_cb_("unlocked");
    bool ok = stop_capture();
    if (capture_joined_cb_)
      capture_joined_cb_();
    return ok;
  }
  return true;
#endif
}

void PcmCaptureService::set_capture_joined_callback(CaptureJoinedCallback cb) {
  // init-only：运行期不再改，避免 std::function 读写竞态（同
  // DenoiseProcessor::set_latency_change_cb 模式）。
  capture_joined_cb_ = std::move(cb);
}

void PcmCaptureService::set_ptp_status_forward_callback(
    PtpStatusForwardCallback cb) {
  // init-only：同 set_capture_joined_callback 模式。
  ptp_status_forward_cb_ = std::move(cb);
}

bool PcmCaptureService::on_sink_add(uint8_t /*id*/) {
  return true;
}
bool PcmCaptureService::on_sink_remove(uint8_t /*id*/) {
  return true;
}

bool PcmCaptureService::start_capture() {
  if (running_.load())
    return true;
  stop_flag_.store(false);
  running_.store(true);
#ifdef _USE_FAKE_DRIVER_
  capture_future_ =
      std::async(std::launch::async, [this] { fake_capture_loop(); });
#else
  capture_future_ = std::async(std::launch::async, [this] { capture_loop(); });
#endif
  return true;
}

bool PcmCaptureService::stop_capture() {
  if (!running_.load())
    return true;
  stop_flag_.store(true);
  // §11 风险19：控制线程调 snd_pcm_drop()+close() 中断阻塞 readi。
  // acquire 与 capture_loop 的 store(release) 配对，确保看到 open 后的指针。
  snd_pcm_t* h = capture_handle_.load(std::memory_order_acquire);
  if (h) {
    snd_pcm_drop(h);
  }
  if (capture_future_.valid())
    capture_future_.wait();
  if (h) {
    snd_pcm_close(h);
    capture_handle_.store(nullptr, std::memory_order_release);
  }
  running_.store(false);
  return true;
}

// FAKE_DRIVER：模拟 ALSA period 节拍分发（§11 风险21 三规格）。
void PcmCaptureService::fake_capture_loop() {
  BOOST_LOG_TRIVIAL(info) << "PcmCaptureService: fake_capture_loop start";
  // 静音帧缓冲（S16_LE 交错），按 test_rate_/test_channels_ 配置。
  // Spec1：未指定 fake_pcm_source_ 时用内置静音帧（§4.3）。
  const uint8_t channels = test_channels_;
  const uint32_t rate = test_rate_;
  const size_t bytes_per_sample = 2;  // S16_LE
  const size_t period_bytes = kPeriodSamples * channels * bytes_per_sample;
  std::vector<uint8_t> silent(period_bytes, 0);

  // Spec3 Task 8：若 config_->get_fake_pcm_source() 非空，读 WAV（48kHz
  // PCM-16 mono）循环喂帧（替内置静音）。复用 T5 的 parse_wav_pcm16_48k_mono
  // （DRY，不重复 WAV 解析）。
  std::vector<int16_t> wav_samples;  // mono S16
  bool use_wav = false;
  if (config_ && !config_->get_fake_pcm_source().empty()) {
    std::ifstream file(config_->get_fake_pcm_source(), std::ios::binary);
    if (file.is_open()) {
      std::string wav_bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
      std::vector<float> float_samples;
      uint32_t sample_rate = 0;
      if (noise::parse_wav_pcm16_48k_mono(wav_bytes, float_samples,
                                          sample_rate)) {
        wav_samples.reserve(float_samples.size());
        for (float v : float_samples) {
          if (v > 1.0f)
            v = 1.0f;
          if (v < -1.0f)
            v = -1.0f;
          wav_samples.push_back(static_cast<int16_t>(v * 32767.0f));
        }
        use_wav = !wav_samples.empty();
        BOOST_LOG_TRIVIAL(info) << "PcmCaptureService: fake_pcm_source loaded "
                                << wav_samples.size() << " samples from "
                                << config_->get_fake_pcm_source();
      } else {
        BOOST_LOG_TRIVIAL(warning)
            << "PcmCaptureService: failed to parse fake_pcm_source WAV ("
            << config_->get_fake_pcm_source() << "), using silence";
      }
    } else {
      BOOST_LOG_TRIVIAL(warning)
          << "PcmCaptureService: cannot open " << config_->get_fake_pcm_source()
          << ", using silence";
    }
  }

  // period 缓冲：WAV 模式下每 period 从 wav_samples 循环填充；否则用静音帧。
  std::vector<uint8_t> period_buf(period_bytes, 0);
  size_t wav_pos = 0;

  // period 时长 = 6144 / 48000 ≈ 128ms
  const auto period_duration =
      std::chrono::microseconds(kPeriodSamples * 1000000ULL / rate);
  auto next_period = std::chrono::steady_clock::now();
  while (!stop_flag_.load()) {
    if (use_wav) {
      // 从 wav_samples 循环读取 kPeriodSamples 帧，交错复制到 channels 通道。
      for (size_t i = 0; i < kPeriodSamples; ++i) {
        int16_t s = wav_samples[wav_pos];
        wav_pos = (wav_pos + 1) % wav_samples.size();
        for (uint8_t ch = 0; ch < channels; ++ch) {
          std::memcpy(
              period_buf.data() + (i * channels + ch) * bytes_per_sample, &s,
              bytes_per_sample);
        }
      }
      dispatch(period_buf.data(), kPeriodSamples, channels, rate);
    } else {
      dispatch(silent.data(), kPeriodSamples, channels, rate);
    }
    next_period += period_duration;
    std::this_thread::sleep_until(next_period);
  }
  // period 结尾推进 epoch（RT 静止点），供 retire 队列 2-epoch 判断。
  // reclaim_older_than 由控制线程 register/unregister_provider 驱动
  // （§3.8 housekeeper），RT 路径无锁。
  providers_.advance_epoch();
  BOOST_LOG_TRIVIAL(info) << "PcmCaptureService: fake_capture_loop end";
}

// 真实 ALSA capture（PTP locked 时运行）。hw_params 镜像 streamer.cpp
// start_capture。
void PcmCaptureService::capture_loop() {
  BOOST_LOG_TRIVIAL(info) << "PcmCaptureService: capture_loop start";
  constexpr const char* device = "plughw:RAVENNA";
  int err;
  // 局部 handle 中转：snd_pcm_open 要 snd_pcm_t**，不能直接传 atomic 地址。
  // open 成功后立即 store 到 capture_handle_，让 stop_capture 可见以便 drop。
  snd_pcm_t* handle = nullptr;
  if ((err = snd_pcm_open(&handle, device, SND_PCM_STREAM_CAPTURE,
                          SND_PCM_NONBLOCK)) < 0) {
    BOOST_LOG_TRIVIAL(fatal) << "PcmCaptureService: cannot open " << device
                             << ": " << snd_strerror(err);
    running_.store(false);
    return;
  }
  capture_handle_.store(handle, std::memory_order_release);
  snd_pcm_hw_params_t* hw;
  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_hw_params_any(handle, hw);
  snd_pcm_hw_params_set_access(handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(handle, hw, SND_PCM_FORMAT_S16_LE);
  uint32_t rate = config_->get_sample_rate();
  snd_pcm_hw_params_set_rate_near(handle, hw, &rate, 0);
  uint8_t channels = config_->get_streamer_channels();
  snd_pcm_hw_params_set_channels(handle, hw, channels);
  snd_pcm_hw_params(handle, hw);
  snd_pcm_prepare(handle);

  const size_t bytes_per_frame = 2 * channels;  // S16_LE
  std::vector<uint8_t> buf(kPeriodSamples * bytes_per_frame);
  while (!stop_flag_.load()) {
    snd_pcm_sframes_t n = snd_pcm_readi(handle, buf.data(), kPeriodSamples);
    if (n < 0) {
      // stop_flag_ 触发或 xrun：readi 返回错误，检查 stop_flag_ 退出
      if (stop_flag_.load())
        break;
      // 镜像 streamer.cpp:151-180 pcm_read：NONBLOCK 设备无数据时返 -EAGAIN，
      // snd_pcm_recover 不处理 -EAGAIN（只处理 -EINTR/-EPIPE/-ESTRPIPE），
      // 直接 continue 会 100% CPU busy-spin。先 snd_pcm_wait 阻塞等数据。
      if (n == -EAGAIN) {
        snd_pcm_wait(handle, 1000);
        continue;
      }
      snd_pcm_recover(handle, n, 1);
      continue;
    }
    dispatch(buf.data(), static_cast<size_t>(n), channels, rate);
    // period 结尾推进 epoch（RT 静止点）。reclaim_older_than 由控制线程
    // register/unregister_provider 驱动（§3.8 housekeeper），RT 路径无锁。
    providers_.advance_epoch();
  }
  BOOST_LOG_TRIVIAL(info) << "PcmCaptureService: capture_loop end";
}

// 测试专用入口
void PcmCaptureService::start_fake_for_test(uint32_t sample_rate,
                                            uint8_t channels) {
  test_rate_ = sample_rate;
  test_channels_ = channels;
  // 不重置 providers_ 表：create_for_test() 已 publish 初始空表，
  // register_provider() 在此前已追加 provider，重发空表会冲掉已注册 provider。
  start_capture();
}
void PcmCaptureService::stop_for_test() {
  stop_capture();
}

#endif  // _USE_NOISE_

// daemon/noise/tests/noise_test.cpp
// Noise 模块 Boost.Test 入口。Spec1 Task 1 仅放 trivial 占位，
// 后续 task 追加 RcuPtr / PcmCaptureService / Bridge 单测。
#define BOOST_TEST_MODULE noise_test
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_CASE(placeholder) {
  BOOST_CHECK(true);
}

#include "rcu_ptr.hpp"
#include <atomic>
#include <memory>

struct Foo {
  int value;
  explicit Foo(int v) : value(v) {}
};

BOOST_AUTO_TEST_SUITE(rcu_ptr_tests)

BOOST_AUTO_TEST_CASE(publish_load_returns_bare_pointer) {
  noise::RcuPtr<Foo> rcu;
  rcu.publish(std::make_shared<Foo>(42));
  Foo* loaded = rcu.load();
  BOOST_CHECK(loaded != nullptr);
  BOOST_CHECK_EQUAL(loaded->value, 42);
}

BOOST_AUTO_TEST_CASE(publish_replaces_value) {
  noise::RcuPtr<Foo> rcu;
  rcu.publish(std::make_shared<Foo>(1));
  rcu.publish(std::make_shared<Foo>(2));
  BOOST_CHECK_EQUAL(rcu.load()->value, 2);
}

BOOST_AUTO_TEST_CASE(constructor_publishes_init_never_null) {
  noise::RcuPtr<Foo> rcu(std::make_shared<Foo>(99));
  BOOST_CHECK(rcu.load() != nullptr);
  BOOST_CHECK_EQUAL(rcu.load()->value, 99);
}

BOOST_AUTO_TEST_CASE(two_epoch_retire_releases_old_after_two_advances) {
  static std::atomic<int> deleter_count{0};
  deleter_count.store(0);
  struct Tracked {
    int v;
    explicit Tracked(int x) : v(x) {}
  };
  auto make_tracked = [](int v) {
    return std::shared_ptr<Tracked>(new Tracked(v), [](Tracked* p) {
      deleter_count.fetch_add(1);
      delete p;
    });
  };
  noise::RcuPtr<Tracked> rcu;
  rcu.publish(make_tracked(1));
  std::shared_ptr<Tracked> old = rcu.publish(make_tracked(2));
  noise::RetireQueue<Tracked> rq;
  uint64_t retire_epoch = rcu.epoch();  // 0
  rq.retire(std::move(old), retire_epoch);

  rq.reclaim_older_than(rcu.epoch());  // epoch 0
  BOOST_CHECK_EQUAL(deleter_count.load(), 0);
  rcu.advance_epoch();  // epoch 1
  rq.reclaim_older_than(rcu.epoch());
  BOOST_CHECK_EQUAL(deleter_count.load(), 0);
  rcu.advance_epoch();  // epoch 2
  rq.reclaim_older_than(rcu.epoch());
  BOOST_CHECK_EQUAL(deleter_count.load(), 1);
}

BOOST_AUTO_TEST_CASE(const_t_supported) {
  noise::RcuPtr<const Foo> rcu;
  rcu.publish(std::make_shared<Foo>(7));
  const Foo* loaded = rcu.load();
  BOOST_CHECK(loaded != nullptr);
  BOOST_CHECK_EQUAL(loaded->value, 7);
}

BOOST_AUTO_TEST_SUITE_END()

#include "noise_session_manager_bridge.hpp"
#include "noise/audio_capture.hpp"

#include "pcm_capture_service.hpp"
#include <chrono>
#include <thread>

// ── SessionManager 桩 ────────────────────────────────────────────────────
// PcmCaptureService::init() / is_sink_receiving() 引用以下 SessionManager
// 成员。fake_capture_loop 测试走 create_for_test() 旁路 init()，这些方法从不
// 被调用，仅为满足链接器符号解析。
#include "session_manager.hpp"

void SessionManager::add_ptp_status_observer(const PtpStatusObserver&) {}
void SessionManager::add_sink_observer(SinkObserverType, const SinkObserver&) {}
void SessionManager::get_ptp_status(PTPStatus&) const {}
std::error_code SessionManager::get_sink_status(uint32_t,
                                                SinkStreamStatus&) const {
  return {};
}

BOOST_AUTO_TEST_SUITE(bridge_tests)

// uint8_t(S16_LE 交错)->float 转换 + 单通道解复用。
// 构造已知交错 PCM：[ch0_s0, ch1_s0, ch0_s1, ch1_s1, ...]，提取 ch0。
BOOST_AUTO_TEST_CASE(bridge_demux_and_float_conversion) {
  auto pcm_svc = PcmCaptureService::create_for_test();
  NoiseSessionManagerBridge bridge(pcm_svc);

  // 2 通道，4 样本：ch0 = [0, 16384, -16384, 32767]，ch1 全 0
  const int16_t interleaved[8] = {0, 0, 16384, 0, -16384, 0, 32767, 0};
  static std::vector<float> received;
  received.clear();
  AudioCapture cap;
  cap.register_callback([](const float* frames, size_t n, uint8_t ch) {
    BOOST_CHECK_EQUAL(ch, 1);
    for (size_t i = 0; i < n; ++i)
      received.push_back(frames[i]);
  });
  // Bridge 把交错 S16 转 float 单通道后喂 AudioCapture 回调
  bridge.test_demux_for_test(reinterpret_cast<const uint8_t*>(interleaved),
                             4 /*samples*/, 2 /*channels*/, 0 /*ch_index*/,
                             cap);
  BOOST_CHECK_EQUAL(received.size(), 4u);
  BOOST_CHECK_CLOSE(received[0], 0.0f / 32768.0f, 0.01);
  BOOST_CHECK_CLOSE(received[1], 16384.0f / 32768.0f, 0.01);
  BOOST_CHECK_CLOSE(received[2], -16384.0f / 32768.0f, 0.01);
  BOOST_CHECK_CLOSE(received[3], 32767.0f / 32768.0f, 0.01);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(pcm_capture_service_tests)

// fake_capture_loop：注册 stub provider，断言帧回调触发、frame_count==6144。
BOOST_AUTO_TEST_CASE(fake_capture_loop_dispatches_to_provider) {
  // FAKE_DRIVER 模式：PcmCaptureService 忽略 PTP，直接跑 fake_capture_loop。
  // SessionManager/Config 在测试中用最小 stub（见 Step 3 的测试辅助）。
  auto svc = PcmCaptureService::create_for_test();
  static std::atomic<int> callback_count{0};
  static std::atomic<size_t> last_frame_count{0};
  static std::atomic<uint8_t> last_channels{0};
  static std::atomic<uint32_t> last_rate{0};
  auto token =
      svc->register_provider([](const uint8_t* /*pcm*/, size_t frame_count,
                                uint8_t channels, uint32_t rate) {
        callback_count.fetch_add(1);
        last_frame_count.store(frame_count);
        last_channels.store(channels);
        last_rate.store(rate);
      });
  svc->start_fake_for_test(48000, 2);  // 48kHz, 2ch, 内置静音帧
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  svc->stop_for_test();
  svc->unregister_provider(token);
  BOOST_CHECK_GT(callback_count.load(), 0);
  BOOST_CHECK_EQUAL(last_frame_count.load(), 6144u);
  BOOST_CHECK_EQUAL(last_channels.load(), 2);
  BOOST_CHECK_EQUAL(last_rate.load(), 48000u);
}

BOOST_AUTO_TEST_SUITE_END()

#include "noise_manager.hpp"
#include <chrono>
#include <thread>

// ── NoiseAudioBridge 桩 ─────────────────────────────────────────────────
// 最小 stub 实现 noise::NoiseAudioBridge 纯虚，供 NoiseManager 测试构造。
class NoiseAudioBridgeStub : public noise::NoiseAudioBridge {
 public:
  void register_frame_provider(
      uint8_t,
      const std::vector<uint8_t>&,
      noise::NoiseAudioBridge::FrameProvider) override {}
  void unregister_frame_provider(uint8_t) override {}
  bool is_sink_receiving(uint8_t) const override { return false; }
  uint32_t get_sample_rate() const override { return 48000; }
  uint8_t get_sink_channel_count(uint8_t) const override { return 1; }
  void set_ptp_status_callback(
      noise::NoiseAudioBridge::PtpStatusCallback) override {}
  void set_sink_add_callback(
      noise::NoiseAudioBridge::SinkChangeCallback) override {}
  void set_sink_remove_callback(
      noise::NoiseAudioBridge::SinkChangeCallback) override {}
};

BOOST_AUTO_TEST_SUITE(noise_manager_tests)

// 1.4b: 多 sensor 帧路由 + sensor 增删不阻塞 + RcuPtr pin/unpin
BOOST_AUTO_TEST_CASE(noise_manager_routes_frames_to_sensors) {
  NoiseAudioBridgeStub bridge;  // 见上方测试辅助
  noise::NoiseManager mgr(bridge);
  // #2: ptp_locked_ 默认 false，测试显式置 true 模拟 PTP 锁定。
  mgr.set_ptp_locked_for_test(true);
  BOOST_CHECK(mgr.add_sensor(0, 0, noise::NoiseSensorConfig{}));
  BOOST_CHECK(mgr.add_sensor(1, 1, noise::NoiseSensorConfig{}));

  // 合成静音帧喂两个 sink
  float silence[480] = {0};
  mgr.on_period_begin();
  mgr.on_frame(0, silence, 480);
  mgr.on_frame(1, silence, 480);
  mgr.on_period_end();
  // #3: 验证 stub process() 被调用（路由生效）
  BOOST_CHECK_GT(mgr.stub_call_count_for_test(0), 0);
  BOOST_CHECK_GT(mgr.stub_call_count_for_test(1), 0);

  // 增删 sensor 不阻塞帧处理（COW 原子换）
  size_t count_before = mgr.stub_call_count_for_test(0);
  mgr.on_period_begin();
  mgr.add_sensor(2, 2, noise::NoiseSensorConfig{});  // 控制线程换表
  mgr.on_frame(0, silence, 480);  // RT 用 pinned 快照，不受影响
  mgr.on_period_end();
  BOOST_CHECK_EQUAL(mgr.sensor_count_for_test(), 3);
  // #3: 第二个 period 再次路由到 sensor 0，call_count 递增
  BOOST_CHECK_GT(mgr.stub_call_count_for_test(0), count_before);
}

// PTP unlock 置 ptp_locked_=false 后 process 跳过
BOOST_AUTO_TEST_CASE(noise_manager_ptp_unlock_skips_processing) {
  NoiseAudioBridgeStub bridge;
  noise::NoiseManager mgr(bridge);
  mgr.add_sensor(0, 0, noise::NoiseSensorConfig{});
  // #2: ptp_locked_ 默认 false，置 true 模拟 PTP 锁定。
  mgr.set_ptp_locked_for_test(true);
  float silence[480] = {0};
  mgr.on_period_begin();
  mgr.on_frame(0, silence, 480);  // ptp_locked_=true，处理
  mgr.on_period_end();
  // #3: 确认 process() 被调用过
  BOOST_CHECK_GT(mgr.stub_call_count_for_test(0), 0);

  mgr.on_ptp_unlocked();
  BOOST_CHECK(!mgr.is_ptp_locked_for_test());
  // #4: on_ptp_unlocked 立即置位 reset_pending_
  BOOST_CHECK(mgr.is_reset_pending_for_test());

  size_t count_before = mgr.stub_call_count_for_test(0);
  mgr.on_period_begin();
  mgr.on_frame(0, silence, 480);  // ptp_locked_=false，跳过
  mgr.on_period_end();
  // #3: process() 未被调用，call_count 未递增
  BOOST_CHECK_EQUAL(mgr.stub_call_count_for_test(0), count_before);

  // #4: async housekeeper 200ms 后清 reset_pending_（250ms 等待留余量，
  // 此时 future 已完成，析构时 wait() 立即返回，不阻塞）。
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  BOOST_CHECK(!mgr.is_reset_pending_for_test());
}

BOOST_AUTO_TEST_SUITE_END()

#include "noise_detector.hpp"
#include "vad.hpp"
#include "tests/synth_audio.hpp"

BOOST_AUTO_TEST_SUITE(noise_detector_tests)
// SimpleEnergyVad: 学完 15 帧静音 noise floor 后,静音判非语音、speech_like
// 判语音。
BOOST_AUTO_TEST_CASE(vad_detects_speech_vs_silence) {
  noise::SimpleEnergyVad vad;
  float buf[synth::kFrameSize];
  synth::silence(buf, synth::kFrameSize);
  for (int i = 0; i < 15; ++i)
    vad.process(buf, synth::kFrameSize, 48000);              // 学 noise floor
  BOOST_CHECK(!vad.process(buf, synth::kFrameSize, 48000));  // 静音=非语音
  synth::speech_like(buf, synth::kFrameSize);
  BOOST_CHECK(vad.process(buf, synth::kFrameSize, 48000));  // 语音=语音
}

// NoiseDetector SF: 白噪 SF>0.5(频谱平坦)、speech_like SF<0.3(谐波结构)。
BOOST_AUTO_TEST_CASE(detector_spectral_flatness_white_vs_speech) {
  noise::NoiseDetector det;
  float buf[synth::kFrameSize];
  synth::white_noise(buf, synth::kFrameSize, 42);
  auto r1 = det.process_frame(buf, synth::kFrameSize);
  BOOST_CHECK_GT(r1.spectral_flatness, 0.5f);  // 白噪 SF 高
  synth::speech_like(buf, synth::kFrameSize);
  auto r2 = det.process_frame(buf, synth::kFrameSize);
  BOOST_CHECK_LT(r2.spectral_flatness, 0.3f);  // 语音 SF 低
}

BOOST_AUTO_TEST_SUITE_END()

#include "denoise_processor.hpp"
#include "model-adapters/passthrough_plugin.hpp"

BOOST_AUTO_TEST_SUITE(denoise_processor_tests)

// PassthroughPlugin 直通：original/denoised 相同，noise=0
BOOST_AUTO_TEST_CASE(passthrough_plugin_three_output) {
  noise::DenoiseProcessor dp;  // 构造装 PassthroughPlugin
  float in[synth::kFrameSize];
  synth::white_noise(in, synth::kFrameSize, 1);
  noise::DenoiseResult result;
  dp.on_period_begin();
  size_t n = dp.process(in, synth::kFrameSize, &result);
  dp.on_period_end();
  const noise::DenoiseOutput* out = dp.get_output();
  BOOST_CHECK_EQUAL(out->frame_count, n);
  // passthrough: denoised == original, noise == 0
  for (size_t i = 0; i < n; ++i) {
    BOOST_CHECK_CLOSE(out->denoised[i], out->original[i], 0.01);
    BOOST_CHECK_SMALL(out->noise[i], 1e-6f);
  }
}

// 准热切换：switch 到另一 Passthrough -> 静音窗口 -> 恢复
BOOST_AUTO_TEST_CASE(switch_plugin_mute_window) {
  noise::DenoiseProcessor dp;
  float in[synth::kFrameSize];
  synth::white_noise(in, synth::kFrameSize, 2);
  dp.on_period_begin();
  dp.process(in, synth::kFrameSize, nullptr);
  dp.on_period_end();
  BOOST_CHECK(dp.switch_plugin("passthrough"));  // 切到同名（测试用）
  // 切换后首帧静音（mute_remaining > 0）
  dp.on_period_begin();
  dp.process(in, synth::kFrameSize, nullptr);
  dp.on_period_end();
  const noise::DenoiseOutput* out = dp.get_output();
  // denoised 首部应为 0（静音窗口），original/noise 保留
  BOOST_CHECK_SMALL(out->denoised[0], 1e-6f);
}

BOOST_AUTO_TEST_SUITE_END()

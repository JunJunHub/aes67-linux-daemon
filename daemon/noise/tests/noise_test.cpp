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

// Spec2 final review I1:静音(全零输入)不得被误报为噪声。
// 修复前:signal_energy=0 -> noise_floor 兜底 1e-10f -> SNR=-inf clamp 0 ->
// is_noisy = (SF>0.6)||(0<20) = true、confidence=1.0(生产误报:
// PTP unlock、sink 未接收、intentional gaps 均为静音)。
// 修复后:signal_energy < kSilenceEnergyThreshold 早返回 is_noisy=false。
BOOST_AUTO_TEST_CASE(silence_is_not_noisy) {
  noise::NoiseDetector det;
  float buf[synth::kFrameSize];
  synth::silence(buf, synth::kFrameSize);  // 全零
  auto r = det.process_frame(buf, synth::kFrameSize);
  BOOST_CHECK(!r.is_noisy);  // 静音不是噪声
  BOOST_CHECK_EQUAL(r.confidence, 0.0f);
  BOOST_CHECK(!r.is_speech);  // 静音不是语音
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

#include "model-adapters/rnnoise/rnnoise_adapter.hpp"

BOOST_AUTO_TEST_SUITE(rnnoise_adapter_tests)

// RNNoise 降噪量：合成弱语音 + 强白噪,喂 20 帧让模型收敛,
// 断言最后一帧输出 RMS 显著低于输入 RMS（降噪量 > 0,目标 10dB）。
// Resolution #3：brief 原始测试循环后无 BOOST_CHECK（rubric defect）,
// 此处补具体断言 + RMS 测量。10dB = 3.16x RMS 削减;若实测不达,
// 按 plan 降级条款调整合成 SNR 或放宽阈值（记 report）。
BOOST_AUTO_TEST_CASE(rnnoise_reduces_noise_10db) {
  noise::RnnoiseAdapter rnn;
  BOOST_CHECK(rnn.init(noise::PluginConfig{}));
  BOOST_CHECK(rnn.supports_vad());
  BOOST_CHECK_EQUAL(rnn.native_sample_rate(), 48000u);

  // 合成：弱语音 + 强白噪（SNR 低,RNNoise 有显著降噪空间）
  float noisy[synth::kFrameSize];
  synth::speech_like(noisy, synth::kFrameSize);
  float noise_buf[synth::kFrameSize];
  synth::white_noise(noise_buf, synth::kFrameSize, 7);
  for (size_t i = 0; i < synth::kFrameSize; ++i)
    noisy[i] = 0.3f * noisy[i] + 0.7f * noise_buf[i];

  float out[synth::kFrameSize * 2];
  noise::DenoiseResult result;
  // 喂多帧让 RNNoise 收敛
  for (int f = 0; f < 20; ++f) {
    rnn.process(noisy, synth::kFrameSize, out, synth::kFrameSize * 2, &result);
  }
  // 计算输入/输出 RMS（out 已被最后一次 process 覆盖为最新降噪输出）
  double in_sq = 0.0, out_sq = 0.0;
  for (size_t i = 0; i < synth::kFrameSize; ++i) {
    in_sq += static_cast<double>(noisy[i]) * noisy[i];
    out_sq += static_cast<double>(out[i]) * out[i];
  }
  double input_rms = std::sqrt(in_sq / synth::kFrameSize);
  double output_rms = std::sqrt(out_sq / synth::kFrameSize);
  // 最小断言：降噪后能量显著降低（reduction > 0）
  BOOST_CHECK_LT(output_rms, input_rms);
  // 目标：10dB = 3.16x RMS 削减（output_rms * 3.16 < input_rms）
  BOOST_CHECK_LT(output_rms * 3.16, input_rms);
}

BOOST_AUTO_TEST_CASE(rnnoise_outputs_vad) {
  noise::RnnoiseAdapter rnn;
  rnn.init(noise::PluginConfig{});
  float sp[synth::kFrameSize];
  synth::speech_like(sp, synth::kFrameSize);
  float out[synth::kFrameSize * 2];
  noise::DenoiseResult r;
  for (int i = 0; i < 10; ++i)
    rnn.process(sp, synth::kFrameSize, out, synth::kFrameSize * 2, &r);
  BOOST_CHECK(r.has_vad);
  BOOST_CHECK_GE(r.vad_probability, 0.0f);
  BOOST_CHECK_LE(r.vad_probability, 1.0f);
}

BOOST_AUTO_TEST_SUITE_END()

#include "noise_analyzer.hpp"

// Spec2 1.7 NoiseAnalyzer：L1 规则式分类（White/Hum50Hz/Impulse
// 三种合成场景）。 Per task-5-brief + resolution #1：测试中类型需 noise::
// 命名空间限定。
BOOST_AUTO_TEST_SUITE(noise_analyzer_tests)

BOOST_AUTO_TEST_CASE(classify_white_noise) {
  noise::NoiseAnalyzer ana;
  float buf[synth::kFrameSize];
  synth::white_noise(buf, synth::kFrameSize, 3);
  noise::NoiseDetectionResult det;
  det.is_speech = false;
  det.spectral_flatness = 0.8f;
  auto r = ana.analyze(buf, synth::kFrameSize, det);
  BOOST_CHECK(r.primary_type == noise::NoiseType::White);
  BOOST_CHECK_GT(r.primary_confidence, 0.3f);
}

BOOST_AUTO_TEST_CASE(classify_hum_50hz) {
  noise::NoiseAnalyzer ana;
  float buf[synth::kFrameSize];
  synth::hum_50hz(buf, synth::kFrameSize);
  noise::NoiseDetectionResult det;
  det.is_speech = false;
  auto r = ana.analyze(buf, synth::kFrameSize, det);
  BOOST_CHECK(r.primary_type == noise::NoiseType::Hum50Hz);
}

BOOST_AUTO_TEST_CASE(classify_impulse) {
  noise::NoiseAnalyzer ana;
  float buf[synth::kFrameSize];
  synth::impulse(buf, synth::kFrameSize);
  noise::NoiseDetectionResult det;
  det.is_speech = false;
  auto r = ana.analyze(buf, synth::kFrameSize, det);
  BOOST_CHECK(r.primary_type == noise::NoiseType::Impulse);
}

// Spec2 Task5 review Important #1:arch §3.3.1 要求分析 OriginalPCM 时
// 用 VAD 过滤语音段,仅在非语音段做频谱分析。detection.is_speech=true 时
// analyze() 必须跳过分析,返回 Unknown(语音帧不参与噪声分类)。
BOOST_AUTO_TEST_CASE(analyze_skips_speech_frames) {
  noise::NoiseAnalyzer ana;
  float buf[synth::kFrameSize];
  synth::white_noise(buf, synth::kFrameSize, 3);
  noise::NoiseDetectionResult det;
  det.is_speech = true;
  det.spectral_flatness = 0.8f;
  auto r = ana.analyze(buf, synth::kFrameSize, det);
  BOOST_CHECK(r.primary_type == noise::NoiseType::Unknown);
}

// Spec2 Task5 review Important #2 + Minor #5:200Hz 纯音不应被误分类为 Hum。
// 修复前:classify_rule_based 用 FFT bin 1-3(93.75-281.25Hz)粗估产生
// Hum50Hz 候选;同时 Goertzel 路径因 200Hz 在 180Hz(60Hz 3 倍频)产生
// 频谱泄漏,peak_60hz 偏高,触发 Hum60Hz 误判。
// 修复后:classify_rule_based 不再添加 hum 候选(Important #2);Goertzel
// 路径加基频存在性守卫(Minor #5 extension)--要求 max(e50,e60) >= 0.3 *
// hum_peak,200Hz 纯音的基频很弱(1.24 vs hum_peak 10.47 -> ratio 0.118),
// 视为非 hum。
BOOST_AUTO_TEST_CASE(non_hum_tone_not_classified_hum) {
  noise::NoiseAnalyzer ana;
  float buf[synth::kFrameSize];
  // 200Hz 纯音(不是 50/100/150Hz 工频哼声)
  for (size_t i = 0; i < synth::kFrameSize; ++i) {
    float t = static_cast<float>(i) / synth::kSampleRate;
    buf[i] = 0.3f * std::sin(2 * 3.14159265358979f * 200 * t);
  }
  noise::NoiseDetectionResult det;
  det.is_speech = false;
  auto r = ana.analyze(buf, synth::kFrameSize, det);
  BOOST_CHECK(r.primary_type != noise::NoiseType::Hum50Hz);
  BOOST_CHECK(r.primary_type != noise::NoiseType::Hum60Hz);
}

BOOST_AUTO_TEST_SUITE_END()

#include "noise_template_db.hpp"
#include <array>

// Spec2 1.8 NoiseTemplateDB：L2 模板匹配 - Bark 32 频带 + 余弦相似度。
// Per task-6-brief + resolution #1：测试中 NoiseTemplateDB 需 noise::
// 命名空间限定。
BOOST_AUTO_TEST_SUITE(template_db_tests)

BOOST_AUTO_TEST_CASE(add_match_remove_template) {
  noise::NoiseTemplateDB db;
  std::array<float, 32> feat{};
  for (auto& f : feat)
    f = 0.5f;
  auto id = db.add_template("空调噪声", feat);
  BOOST_CHECK_GT(id, 0u);
  // 匹配：相同特征 -> 相似度 1.0
  auto [match_id, sim] = db.match(feat);
  BOOST_CHECK_EQUAL(match_id, id);
  BOOST_CHECK_GT(sim, 0.9f);
  // 不匹配：正交特征
  std::array<float, 32> diff{};
  diff[0] = 1.0f;
  auto [mid2, sim2] = db.match(diff);
  BOOST_CHECK_LT(sim2, 0.5f);
  // 删除
  BOOST_CHECK(db.remove_template(id));
  auto [mid3, sim3] = db.match(feat);
  BOOST_CHECK_EQUAL(mid3, 0u);  // 无匹配
}

BOOST_AUTO_TEST_CASE(list_templates) {
  noise::NoiseTemplateDB db;
  std::array<float, 32> f{};
  db.add_template("风扇", f);
  db.add_template("空调", f);
  auto list = db.list_templates();
  BOOST_CHECK_EQUAL(list.size(), 2u);
}

BOOST_AUTO_TEST_SUITE_END()

// Spec2 Task 7: ①②③ 链路集成 - NoiseManager 接入真实
// DenoiseProcessor/NoiseDetector/NoiseAnalyzer。Phase 1 单线程串行执行
// (arch §6.2):on_frame 内 ①denoise->process -> ②detector->process_frame
// (RNNoise VAD 主,denoise_enabled=true 时) -> ③analyzer->analyze(NoisePCM,
// 纯噪声分量分类) -> ④metrics_stub->collect(no-op)。
// 分析源选择(arch §3.3.1):denoise_enabled=true -> NoisePCM(original-denoised,
// 纯噪声,分类最准); false -> OriginalPCM + Detector VAD 过滤语音段。
BOOST_AUTO_TEST_SUITE(noise_pipeline_integration_tests)

// ①②③ 链路:合成白噪 -> 降噪(RNNoise) -> 噪声 PCM 分量 -> 分类=White。
// 断言:经过 20 帧收敛后,NoiseAnalyzer 主类型 = White(频谱平坦,SF>0.7)。
// 这是 Spec2 端到端集成门控:Task 1-6 各组件通过此测试验证协同正确。
BOOST_AUTO_TEST_CASE(pipeline_white_noise_classification) {
  NoiseAudioBridgeStub bridge;
  noise::NoiseManager mgr(bridge);
  // #2: ptp_locked_ 默认 false(arch §3.7 安全默认),测试显式置 true
  // 模拟 PTP 锁定。未置位时 on_frame 跳过所有 ①②③ 处理。
  mgr.set_ptp_locked_for_test(true);
  noise::NoiseSensorConfig cfg;
  cfg.denoise_enabled = true;
  BOOST_CHECK(mgr.add_sensor(0, 0, cfg));
  // 切到 rnnoise 插件(默认构造装 PassthroughPlugin,需显式切换)。
  BOOST_CHECK(mgr.switch_plugin(0, "rnnoise"));

  // 合成强白噪(seed=9,与 rnnoise_adapter_tests 不同的 seed 以避免重复序列)
  float noisy[synth::kFrameSize];
  synth::white_noise(noisy, synth::kFrameSize, 9);

  // 喂 20 帧让 RNNoise 收敛 + NoiseAnalyzer 窗口积累。
  // 单 period 内 20 帧(Phase 1 单线程,arch §6.2)。
  mgr.on_period_begin();
  for (int f = 0; f < 20; ++f)
    mgr.on_frame(0, noisy, synth::kFrameSize);
  mgr.on_period_end();

  // 测试钩子:获取 sensor 0 最近一次 ③ 分析结果。
  auto result = mgr.get_analysis_result_for_test(0);
  // 白噪 SF 趋近 1.0 -> classify_rule_based 触发 White 候选。
  // denoise_enabled=true -> 用 NoisePCM(original-denoised)做分析,纯噪声分量,
  // RNNoise 不会将白噪误判为语音(VAD 概率低 -> is_speech=false)。
  BOOST_CHECK(result.primary_type == noise::NoiseType::White);
}

BOOST_AUTO_TEST_SUITE_END()

// Spec3 Task 2: ④NoiseMetrics 真聚合 - 聚合 ①②③ 链路结果到 per-sensor
// NoiseMetricsSnapshot + 60s history ring + NoiseManager 4 控制方法。
#include "noise_metrics.hpp"

BOOST_AUTO_TEST_SUITE(noise_metrics_tests)

// ④ 聚合 ①②③ 结果：denoise/detection/analysis 聚合到 snapshot。
// 断言：noise_type/noise_type_confidence 来自 ③，is_noisy/estimated_snr_db
// 来自 ②，noise_reduction_db = 20log10(input_rms/denoised_rms)。
BOOST_AUTO_TEST_CASE(metrics_aggregates_123_results) {
  noise::NoiseMetrics m;
  noise::DenoiseResult dr;
  dr.has_vad = true;
  dr.vad_probability = 0.3f;
  noise::NoiseDetectionResult det;
  det.is_noisy = true;
  det.estimated_snr_db = 15.0f;
  noise::NoiseAnalysisResult ar;
  ar.primary_type = noise::NoiseType::White;
  ar.primary_confidence = 0.8f;
  ar.noise_level_dbfs = -35.0f;
  ar.hum_strength_db = -70.0f;
  ar.spectral_flatness = 0.85f;
  ar.spectral_centroid_hz = 4000.0f;
  m.collect(dr, det, ar, /*input_rms=*/0.1f, /*denoised_rms=*/0.01f);
  auto snap = m.snapshot_for_test();
  BOOST_CHECK(snap.noise_type == noise::NoiseType::White);
  BOOST_CHECK_CLOSE(snap.noise_type_confidence, 0.8f, 0.01);
  BOOST_CHECK(snap.is_noisy);
  BOOST_CHECK_CLOSE(snap.estimated_snr_db, 15.0f, 0.01);
  BOOST_CHECK_GT(snap.noise_reduction_db, 10.0f);  // 20log10(0.1/0.01)=20dB
  BOOST_CHECK(!snap.is_alerting);  // -35dBFS < -30 threshold -> not alerting
}

// 告警：noise_level_dbfs=-20dBFS > -30dBFS 阈值 -> is_alerting=true。
BOOST_AUTO_TEST_CASE(metrics_alerts_when_loud) {
  noise::NoiseMetrics m;
  noise::DenoiseResult dr;
  noise::NoiseDetectionResult det;
  det.is_noisy = true;
  noise::NoiseAnalysisResult ar;
  ar.noise_level_dbfs = -20.0f;  // loud
  m.collect(dr, det, ar, 0.5f, 0.5f);
  BOOST_CHECK(m.snapshot_for_test().is_alerting);  // -20 > -30 threshold
}

// NoiseManager 4 控制方法：remove_sensor / enable_sensor / set_dry_wet /
// set_param。
BOOST_AUTO_TEST_CASE(noise_manager_remove_enable_set_methods) {
  NoiseAudioBridgeStub bridge;
  noise::NoiseManager mgr(bridge);
  BOOST_CHECK(mgr.add_sensor(0, 0, noise::NoiseSensorConfig{}));
  BOOST_CHECK(mgr.remove_sensor(0));
  BOOST_CHECK(!mgr.remove_sensor(0));  // 已删
  mgr.add_sensor(0, 0, noise::NoiseSensorConfig{});
  BOOST_CHECK(mgr.enable_sensor(0, false));
  BOOST_CHECK(mgr.set_dry_wet(0, 0.5f));
}

// 60s history ring：每 N 帧采样一次，capped at 60 entries。
BOOST_AUTO_TEST_CASE(metrics_history_populates_and_caps) {
  noise::NoiseMetrics m;
  m.set_history_sample_interval_for_test(1);  // 每 call 采样一次（测试加速）
  noise::DenoiseResult dr;
  noise::NoiseDetectionResult det;
  noise::NoiseAnalysisResult ar;
  ar.noise_level_dbfs = -40.0f;
  for (int i = 0; i < 70; ++i) {  // 70 calls -> 70 entries, capped at 60
    m.collect(dr, det, ar, 0.1f, 0.05f);
  }
  auto hist = m.get_history_for_test();
  BOOST_CHECK_EQUAL(hist.size(), 60u);
}

BOOST_AUTO_TEST_SUITE_END()

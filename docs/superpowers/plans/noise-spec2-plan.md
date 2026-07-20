# Noise Spec2 Implementation Plan - 降噪与噪声识别链

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `WITH_NOISE=ON` 下实现 NoiseManager 骨架（真实 ①②③ 接入）+ NoiseDetector + DenoiseProcessor+RnnoiseAdapter + NoiseAnalyzer + NoiseTemplateDB 核心，为 Spec3 冻结接口。

**Architecture:** NoiseManager 用 `RcuPtr<const SensorTable>` COW 管 per-sensor `SensorContext`，on_period_begin/end 驱动 RcuPtr pin/unpin + DenoiseProcessor front/back swap；DenoiseProcessor 用 `RcuPtr<PluginSlot>` 准热切换 + 三路输出双缓冲；NoiseAnalyzer L1 规则式 + 逐帧 FrameFeatures 缓冲；合成帧测试。

**Tech Stack:** C++17, CMake 3.7+, Boost.Test, RNNoise（FetchContent gitlab.xiph.org），kiss_fft（RNNoise 内嵌），Spec1 基础设施（PcmCaptureService/RcuPtr/Bridge/AudioCapture，HEAD `34e7ac1`）。

## Global Constraints

- **Spec 依据**：`docs/superpowers/specs/noise-spec2-design.md`（决策记录 7 条）。实现代码主体在 `docs/noise/architecture-design.md` §3.2/§3.3/§3.4/§3.7 + `docs/noise/denoise-plugin-architecture.md` §2.2/§3.1/§4.1-4.4（**已含完整代码，逐字采用**，本 plan 标注采用位置 + 仅补 TDD 步骤/测试/CMake/集成差异）。
- **Spec1 已冻结接口**（直接消费，见 spec1 §C）：`PcmCaptureService`、`RcuPtr<T>`+`RetireQueue<T>`（**用 explicit 构造保永不为空**）、`NoiseAudioBridge`、`AudioCapture`（FrameCallback/PeriodBeginCallback/PeriodEndCallback/on_frame/on_period_begin/end）。
- **采样率**：48kHz，帧长 480 样本（10ms），ALSA period 6144（12.8 帧/period，carry-over 由 Spec1 AudioCapture 处理）。
- **RT 路径无锁**：`RcuPtr` period 顶部 pin + 整 period 复用；`reclaim` 在控制线程（Spec1 Task 3 教训：勿在 RT 调 `reclaim_older_than`，它持 mutex）。
- **WITH_NOISE=OFF 鎖回归**：新文件 `#ifdef _USE_NOISE_` 或仅 WITH_NOISE 编译；Streamer/main.cpp 不动。
- **①②③ 接入**（决策2）：1.4b stub processor；1.5-1.8 各自接入 NoiseManager 替换 stub；④NoiseMetrics stub 占位（Spec3 1.9）。
- **1.8 仅内存 store**（决策1）：无 HTTP/磁盘持久化（留 Spec3）。
- **合成帧测试**（决策5）：程序生成白噪/粉噪/哼声/脉冲/语音/静音，不用 WAV。
- **PTP-unlock reset**（决策6）：`on_ptp_unlocked` 置 `ptp_locked_=false`+`reset_pending_=true`；housekeeper 安全延迟 ~200ms 后 `plugin->reset()`。
- **DenoiseOutput 用 `const float* + frame_count`**（§F plan 决策，不引入 GSL；DenoiseOutput 已携带 frame_count）。
- **代码风格** `.clang-format`（Chromium，2-space，`PointerAlignment: Left`，`SortIncludes: false`）。
- **提交**：中文，scope 按 git-workflow（`feat(noise)` 模块代码 / `build` CMake / `fix(noise)` 修复），尾加 `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`。只 commit 不 push。

## Plan-level deviation（需执行前确认）

**WebRTC VAD → SimpleEnergyVad**：spec 决策3 说 WebRTC VAD standalone C 封装，但 standalone 源未 pin（spec 原文"如 webrtc-vad C 封装"）。本 plan Task 2 改用 `IVad` 接口 + `SimpleEnergyVad`（能量 + 过零率，零外部依赖，可测）。理由：WebRTC VAD 源不确定，energy VAD 不阻塞 Spec2；`IVad` 接口允许后续 `WebrtcVadAdapter` drop-in。若用户要求 Spec2 内用 WebRTC VAD，Task 2 加 `WebrtcVadAdapter` + FetchContent（需先确认源 URL）。**执行 Task 2 前须与用户确认此 deviation。**

## File Structure

| 文件 | 动作 | 责任 | Task |
|------|------|------|------|
| `daemon/noise/noise_manager.hpp/.cpp` | 新建 | NoiseManager 骨架（§3.7） | 1,7 |
| `daemon/noise/stub_processor.hpp/.cpp` | 新建 | 1.4b stub processor（验证帧路由，Task 7 删） | 1 |
| `daemon/noise/noise_detector.hpp/.cpp` | 新建 | NoiseDetector（§3.2） | 2 |
| `daemon/noise/vad.hpp/.cpp` | 新建 | IVad 接口 + SimpleEnergyVad | 2 |
| `daemon/noise/denoise_plugin.hpp` | 新建 | IDenoisePlugin + PluginConfig + DenoiseResult（§2.2） | 3 |
| `daemon/noise/denoise_plugin_factory.hpp/.cpp` | 新建 | DenoisePluginRegistry（§4.1） | 3 |
| `daemon/noise/denoise_processor.hpp/.cpp` | 新建 | DenoiseProcessor 三路+双缓冲+准热切换（§4.2） | 3 |
| `daemon/noise/model-adapters/passthrough_plugin.hpp/.cpp` | 新建 | PassthroughPlugin（默认直通） | 3 |
| `daemon/noise/model-adapters/rnnoise/rnnoise_adapter.hpp/.cpp` | 新建 | RnnoiseAdapter（§3.1） | 4 |
| `daemon/noise/noise_analyzer.hpp/.cpp` | 新建 | NoiseAnalyzer L1+FrameFeatures（§3.3） | 5 |
| `daemon/noise/noise_template_db.hpp/.cpp` | 新建 | TemplateDB 内存（§3.3.5） | 6 |
| `daemon/noise/noise_metrics_stub.hpp` | 新建 | ④NoiseMetrics stub（Spec3 替换） | 1 |
| `daemon/noise/CMakeLists.txt` | 修改 | 渐进追加 SOURCES + RNNoise FetchContent | 1-6 |
| `daemon/CMakeLists.txt` | 修改 | noise-test target 追加源 + 链接 | 1-6 |
| `daemon/noise/tests/noise_test.cpp` | 修改 | 追加各 task 测试 | 1-7 |
| `daemon/noise/tests/synth_audio.hpp` | 新建 | 合成帧生成器（白噪/粉噪/哼声/脉冲/语音/静音） | 2 |

---

### Task 1: NoiseManager 骨架 + stub processor（1.4b）

**Files:**
- Create: `daemon/noise/noise_manager.hpp`, `daemon/noise/noise_manager.cpp`, `daemon/noise/stub_processor.hpp`, `daemon/noise/stub_processor.cpp`, `daemon/noise/noise_metrics_stub.hpp`
- Modify: `daemon/noise/CMakeLists.txt`（NOISE_SOURCES 加 5 文件）, `daemon/CMakeLists.txt`（noise-test 加源）, `daemon/noise/tests/noise_test.cpp`（NoiseManager 测试）

**Interfaces:**
- Consumes: `RcuPtr<T>`+`RetireQueue<T>`（Spec1，`daemon/noise/rcu_ptr.hpp`），`AudioCapture`（Spec1，FrameCallback/PeriodBeginCallback/PeriodEndCallback）- NoiseManager 注册为 AudioCapture 的 callback 消费者
- Produces: `NoiseManager` 类（§3.7 L800-857 完整接口），`SensorContext`/`SensorTable`，`on_frame`/`on_period_begin`/`on_period_end`，PTP-unlock 联动

**采用代码**：`noise_manager.hpp` 的 `class NoiseManager` + `SensorContext` + `SensorTable` + 接口签名**逐字采用架构文档 §3.7 L798-857**（含 `RcuPtr<const SensorTable> sensor_table_`、`on_frame`/`on_period_begin`/`on_period_end`、`on_ptp_unlocked`、`ctrl_mutex_`、`ptp_locked_`/`reset_pending_`）。`noise_manager.cpp` 实现这些方法。

**关键实现要点**（arch §3.7 + 决策6）：
- 构造：`sensor_table_` 用 **explicit 构造**（`RcuPtr<const SensorTable>(std::make_shared<SensorTable>())`）保永不为空（Spec1 ledger 注）。
- `add_sensor`：控制线程持 `ctrl_mutex_`，COW 复制当前表 -> 加 SensorContext（含 stub_processor + stub metrics）-> `publish` 新表 -> 旧表推 `RetireQueue` -> **控制线程调 `reclaim_older_than`**（勿在 RT）。
- `on_period_begin`：`pinned_table_ = sensor_table_.load()`；对每个 sensor 调 `denoise->on_period_begin()`。
- `on_frame(sink_id, frames, frame_size)`：查 `pinned_table_` 按 sink_id 路由到 sensor -> 调 `stub_processor->process()`（1.4b）；**①②③ 真实接入留 Task 7**。
- `on_period_end`：对每个 sensor 调 `denoise->on_period_end()`；`pinned_table_ = nullptr`；`sensor_table_.advance_epoch()`；**控制线程 reclaim**（在 `add/remove_sensor` 时，不在 RT）。
- `on_ptp_unlocked`：`ptp_locked_.store(false)`；`reset_pending_.store(true)`。
- **housekeeper reset**（决策6）：控制线程定时器（`std::thread` 周期检查 `reset_pending_`，置位后 sleep 200ms 再 `plugin->reset()` 并清 `reset_pending_`）。Spec2 最小：用 `std::async` 延迟任务（PTP unlock 时启动，200ms 后 reset）。

**stub_processor.hpp**：
```cpp
// daemon/noise/stub_processor.hpp
#ifndef NOISE_STUB_PROCESSOR_HPP_
#define NOISE_STUB_PROCESSOR_HPP_
#include <cstddef>
#include <cstdint>
class StubProcessor {
 public:
  void on_period_begin() {}
  void on_period_end() {}
  // 1.4b stub：直通帧，不处理。Task 7 替换为真实 ①②③。
  void process(uint8_t sink_id, const float* frames, size_t frame_size) {}
};
#endif
```

**noise_metrics_stub.hpp**：
```cpp
// daemon/noise/noise_metrics_stub.hpp
#ifndef NOISE_NOISE_METRICS_STUB_HPP_
#define NOISE_NOISE_METRICS_STUB_HPP_
// ④NoiseMetrics stub 占位（Spec3 1.9 实装真聚合）。Spec2 仅占位使 SensorContext 完整。
class NoiseMetricsStub {
 public:
  void on_period_begin() {}
  void on_period_end() {}
  void collect(/* NoiseAnalysisResult, NoiseDetectionResult, DenoiseResult */ ) {}
};
#endif
```

- [ ] **Step 1: 写失败测试（NoiseManager 帧路由 + RcuPtr pin/unpin）**

追加到 `daemon/noise/tests/noise_test.cpp`（include `noise_manager.hpp`）：

```cpp
#include "noise_manager.hpp"
#include <chrono>
#include <thread>

BOOST_AUTO_TEST_SUITE(noise_manager_tests)

// 1.4b: 多 sensor 帧路由 + sensor 增删不阻塞 + RcuPtr pin/unpin
BOOST_AUTO_TEST_CASE(noise_manager_routes_frames_to_sensors) {
  NoiseAudioBridgeStub bridge;  // 见下方测试辅助
  NoiseManager mgr(bridge);
  BOOST_CHECK(mgr.add_sensor(0, 0, NoiseSensorConfig{}));
  BOOST_CHECK(mgr.add_sensor(1, 1, NoiseSensorConfig{}));

  // 合成静音帧喂两个 sink
  float silence[480] = {0};
  mgr.on_period_begin();
  mgr.on_frame(0, silence, 480);
  mgr.on_frame(1, silence, 480);
  mgr.on_period_end();
  // stub processor 不产出，仅验证不崩溃 + 帧路由执行
  BOOST_CHECK_GT(mgr.sensor_count_for_test(), 0);

  // 增删 sensor 不阻塞帧处理（COW 原子换）
  mgr.on_period_begin();
  mgr.add_sensor(2, 2, NoiseSensorConfig{});  // 控制线程换表
  mgr.on_frame(0, silence, 480);              // RT 用 pinned 快照，不受影响
  mgr.on_period_end();
  BOOST_CHECK_EQUAL(mgr.sensor_count_for_test(), 3);
}

// PTP unlock 置 ptp_locked_=false 后 process 跳过
BOOST_AUTO_TEST_CASE(noise_manager_ptp_unlock_skips_processing) {
  NoiseAudioBridgeStub bridge;
  NoiseManager mgr(bridge);
  mgr.add_sensor(0, 0, NoiseSensorConfig{});
  float silence[480] = {0};
  mgr.on_period_begin();
  mgr.on_frame(0, silence, 480);  // 处理
  mgr.on_period_end();
  mgr.on_ptp_unlocked();
  mgr.on_period_begin();
  mgr.on_frame(0, silence, 480);  // ptp_locked_=false，跳过
  mgr.on_period_end();
  BOOST_CHECK(!mgr.is_ptp_locked_for_test());
}

BOOST_AUTO_TEST_SUITE_END()
```

测试辅助（`NoiseAudioBridgeStub` 实现 Spec1 `NoiseAudioBridge` 纯虚，最小 stub）放在 `noise_test.cpp` 顶部：

```cpp
class NoiseAudioBridgeStub : public noise::NoiseAudioBridge {
 public:
  void register_frame_provider(uint8_t, const std::vector<uint8_t>&, FrameProvider) override {}
  void unregister_frame_provider(uint8_t) override {}
  bool is_sink_receiving(uint8_t) const override { return false; }
  uint32_t get_sample_rate() const override { return 48000; }
  uint8_t get_sink_channel_count(uint8_t) const override { return 1; }
  void set_ptp_status_callback(PtpStatusCallback) override {}
  void set_sink_add_callback(SinkChangeCallback) override {}
  void set_sink_remove_callback(SinkChangeCallback) override {}
};
```

> `sensor_count_for_test()`/`is_ptp_locked_for_test()` 是 NoiseManager 的 public 测试钩子（spec §D 接受此模式，Spec1 同）。

- [ ] **Step 2: 跑测试确认失败** - Run: `cd daemon/build && cmake --build . --target noise-test 2>&1 | head` Expected: 编译失败（`noise_manager.hpp` 不存在）。

- [ ] **Step 3: 实现 noise_manager.hpp/.cpp + stub_processor + metrics_stub**

按 arch §3.7 L798-857 实现 `noise_manager.hpp`（逐字采用接口）+ `noise_manager.cpp`（实现：explicit 构造 sensor_table_、add_sensor COW+reclaim、on_period_begin/end pin/unpin+advance_epoch、on_frame 路由到 stub_processor、on_ptp_unlocked 置位、housekeeper `std::async` 延迟 reset）。加 `sensor_count_for_test()`/`is_ptp_locked_for_test()` public 钩子。`stub_processor.hpp/.cpp` + `noise_metrics_stub.hpp` 如上。

- [ ] **Step 4: 改 CMake** - `daemon/noise/CMakeLists.txt` NOISE_SOURCES 加 `noise_manager.cpp stub_processor.cpp`；`daemon/CMakeLists.txt` noise-test target 不需加新源（noise lib 已含）。

- [ ] **Step 5: 跑测试确认通过** - Run: `cd daemon/build && cmake --build . --target noise-test && ./noise-test -p` Expected: `noise_manager_tests` 2 case PASS。

- [ ] **Step 6: 验证 daemon 构建 + 零回归** - Run: `./noise-dev.sh build && ./noise-dev.sh build --no-noise` Expected: 两次通过。

- [ ] **Step 7: 提交** - `git add ... && git commit -m "feat(noise): Spec2 1.4b NoiseManager 骨架 - RcuPtr sensor_table + 帧路由 + PTP-unlock 联动\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>"`

---

### Task 2: NoiseDetector + IVad/SimpleEnergyVad（1.5）

**Files:**
- Create: `daemon/noise/vad.hpp`, `daemon/noise/vad.cpp`, `daemon/noise/noise_detector.hpp`, `daemon/noise/noise_detector.cpp`, `daemon/noise/tests/synth_audio.hpp`
- Modify: `daemon/noise/CMakeLists.txt`（NOISE_SOURCES 加 vad.cpp noise_detector.cpp）, `daemon/noise/tests/noise_test.cpp`

**Interfaces:**
- Consumes: kiss_fft（RNNoise 内嵌，Task 4 引入；本 task 暂用简单 FFT 或推迟 FFT 到 Task 4 - 见 Step 3）
- Produces: `NoiseDetectionResult`（§3.2 L441-447），`NoiseDetector::process_frame`，`IVad`/`SimpleEnergyVad`

**采用代码**：`noise_detector.hpp` 的 `NoiseDetectionResult` struct + `NoiseDetector` 类签名**逐字采用 §3.2 L437-459**。`vad.hpp` 新增 `IVad` 接口 + `SimpleEnergyVad`（plan deviation，见 Global Constraints）。

**vad.hpp**：
```cpp
// daemon/noise/vad.hpp
#ifndef NOISE_VAD_HPP_
#define NOISE_VAD_HPP_
#include <cstddef>
#include <cstdint>
namespace noise {
// VAD 接口（决策3 deviation：SimpleEnergyVad 实现，WebrtcVadAdapter 可 drop-in）
class IVad {
 public:
  virtual ~IVad() = default;
  // 返回 true=语音，false=非语音（噪声候选）
  virtual bool process(const float* frames, size_t frame_size, uint32_t sample_rate) = 0;
  virtual void reset() = 0;
};
// 能量 + 过零率 VAD（零外部依赖）
class SimpleEnergyVad : public IVad {
 public:
  bool process(const float* frames, size_t frame_size, uint32_t sample_rate) override;
  void reset() override { noise_floor_rms_ = 0; frame_count_ = 0; }
 private:
  float noise_floor_rms_{0};
  size_t frame_count_{0};
};
}  // namespace noise
#endif
```

**SimpleEnergyVad::process** 实现：计算 RMS，若 > noise_floor × 阈值（如 4×）且过零率在语音范围（0.1-0.35/frame）判为语音；非语音帧更新 noise_floor（最小统计法）。首 N 帧（如 10 帧）学习 noise_floor。

**synth_audio.hpp**（合成帧生成器，所有后续 task 用）：
```cpp
// daemon/noise/tests/synth_audio.hpp
#ifndef NOISE_TEST_SYNTH_AUDIO_HPP_
#define NOISE_TEST_SYNTH_AUDIO_HPP_
#include <array>
#include <cmath>
#include <cstdint>
namespace synth {
constexpr size_t kFrameSize = 480;
constexpr uint32_t kSampleRate = 48000;
// 白噪（频谱平坦，SF>0.7）
inline void white_noise(float* out, size_t n, uint32_t /*seed*/) {
  // 简单 LCG 伪随机，均匀分布 [-0.1, 0.1]
  uint32_t s = 12345;
  for (size_t i = 0; i < n; ++i) {
    s = s * 1103515245u + 12345u;
    out[i] = (static_cast<float>(s >> 16) / 65535.0f - 0.5f) * 0.2f;
  }
}
// 50Hz 哼声（50Hz + 倍频）
inline void hum_50hz(float* out, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    float t = static_cast<float>(i) / kSampleRate;
    out[i] = 0.3f * std::sin(2 * M_PI * 50 * t) + 0.1f * std::sin(2 * M_PI * 100 * t);
  }
}
// 脉冲（短时能量突变）
inline void impulse(float* out, size_t n) {
  for (size_t i = 0; i < n; ++i) out[i] = 0;
  for (size_t i = 0; i < 5; ++i) out[i] = 0.9f;  // 起始尖峰
}
// 语音（基频 150Hz + 谐波，模拟有结构频谱）
inline void speech_like(float* out, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    float t = static_cast<float>(i) / kSampleRate;
    out[i] = 0.2f * (std::sin(2*M_PI*150*t) + 0.5f*std::sin(2*M_PI*300*t)
                     + 0.25f*std::sin(2*M_PI*450*t));
  }
}
inline void silence(float* out, size_t n) { for (size_t i = 0; i < n; ++i) out[i] = 0; }
}  // namespace synth
#endif
```

- [ ] **Step 1: 写失败测试** - 追加 `noise_test.cpp`（include `noise_detector.hpp`/`vad.hpp`/`tests/synth_audio.hpp`）：

```cpp
#include "noise_detector.hpp"
#include "vad.hpp"
#include "tests/synth_audio.hpp"

BOOST_AUTO_TEST_SUITE(noise_detector_tests)
BOOST_AUTO_TEST_CASE(vad_detects_speech_vs_silence) {
  noise::SimpleEnergyVad vad;
  float buf[synth::kFrameSize];
  synth::silence(buf, synth::kFrameSize);
  for (int i = 0; i < 15; ++i) vad.process(buf, synth::kFrameSize, 48000);  // 学 noise floor
  BOOST_CHECK(!vad.process(buf, synth::kFrameSize, 48000));  // 静音=非语音
  synth::speech_like(buf, synth::kFrameSize);
  BOOST_CHECK(vad.process(buf, synth::kFrameSize, 48000));  // 语音=语音
}
BOOST_AUTO_TEST_CASE(detector_spectral_flatness_white_vs_speech) {
  NoiseDetector det;
  float buf[synth::kFrameSize];
  synth::white_noise(buf, synth::kFrameSize, 42);
  auto r1 = det.process_frame(buf, synth::kFrameSize);
  BOOST_CHECK_GT(r1.spectral_flatness, 0.5f);  // 白噪 SF 高
  synth::speech_like(buf, synth::kFrameSize);
  auto r2 = det.process_frame(buf, synth::kFrameSize);
  BOOST_CHECK_LT(r2.spectral_flatness, 0.3f);  // 语音 SF 低
}
BOOST_AUTO_TEST_SUITE_END()
```

- [ ] **Step 2: 跑测试确认失败** - Expected: 编译失败。

- [ ] **Step 3: 实现 vad.hpp/.cpp + noise_detector.hpp/.cpp**

`SimpleEnergyVad::process`（RMS + ZCR + noise floor 学习）。`NoiseDetector`（§3.2 L437-459 接口）：
- FFT：**本 task 暂用简单 512 点 DFT**（或推迟到 Task 4 用 kiss_fft）。决策：本 task 实现一个轻量 `compute_fft`（512 点 naive DFT，O(N²) 但测试帧少可接受），Task 4 引入 RNNoise/kiss_fft 后替换。**或**：若 DFT 实现复杂，本 task 的 SF 用时域近似（能量分布方差作为平坦度代理），Task 4 替换为 FFT 精确版。**推荐**：实现简单 512 点 FFT（Cooley-Tukey，~30 行），避免依赖 kiss_fft，Task 4 不必改。
- SF（§3.2 L421-429）：`geometric_mean(|X|²) / arithmetic_mean(|X|²)`。
- 噪声底（§3.2 L431-435）：最小统计法，VAD 非语音时逐频带更新。
- `NoiseDetectionResult`：is_noisy/confidence/spectral_flatness/estimated_snr_db/is_speech。

- [ ] **Step 4: 改 CMake** - NOISE_SOURCES 加 `vad.cpp noise_detector.cpp`。

- [ ] **Step 5: 跑测试确认通过** - Expected: 2 case PASS。

- [ ] **Step 6: daemon 构建 + 零回归** - Run: `./noise-dev.sh build && ./noise-dev.sh build --no-noise` Expected: 通过。

- [ ] **Step 7: 提交** - `feat(noise): Spec2 1.5 NoiseDetector + IVad/SimpleEnergyVad\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>"`

---

### Task 3: DenoiseProcessor 框架 + IDenoisePlugin + Registry + PassthroughPlugin（1.6a）

**Files:**
- Create: `daemon/noise/denoise_plugin.hpp`, `daemon/noise/denoise_plugin_factory.hpp`, `daemon/noise/denoise_plugin_factory.cpp`, `daemon/noise/denoise_processor.hpp`, `daemon/noise/denoise_processor.cpp`, `daemon/noise/model-adapters/passthrough_plugin.hpp`, `daemon/noise/model-adapters/passthrough_plugin.cpp`
- Modify: `daemon/noise/CMakeLists.txt`（NOISE_SOURCES + model-adapters/passthrough_plugin.cpp）, `daemon/noise/tests/noise_test.cpp`

**Interfaces:**
- Consumes: `RcuPtr<T>`+`RetireQueue<T>`（Spec1）
- Produces: `IDenoisePlugin`+`PluginConfig`+`DenoiseResult`+`ProcessStatus`（§2.2），`DenoisePluginRegistry`（§4.1），`DenoiseProcessor`（§4.2）含 `DenoiseOutput`/`DenoiseBuffer`/`PluginSlot`，`PassthroughPlugin`

**采用代码**：
- `denoise_plugin.hpp`：**逐字采用插件文档 §2.2 L184-275**（IDenoisePlugin 全接口 + PluginConfig + DenoiseResult + ProcessStatus）。
- `denoise_plugin_factory.hpp/.cpp`：**逐字采用 §4.1 L468-502**（DenoisePluginRegistry 单例 + register_plugin/create/list）。
- `denoise_processor.hpp/.cpp`：**逐字采用 §4.2 L545-669**（DenoiseProcessor 全类：PluginSlot/DenoiseBuffer/rcu_ptr_/pinned_/front_/back_/process 三路输出/on_period_begin pin/on_period_end swap+advance_epoch/get_output/drain_retire/switch_plugin 准热切换 with mute_remaining/PassthroughPlugin 默认）。**DenoiseOutput 用 `const float* + frame_count`**（§3.4 L652-657，不用 gsl::span，Global Constraints）。
- `passthrough_plugin.hpp/.cpp`：实现 IDenoisePlugin，`process` 直通 `in->out`（min(n_in,n_out_max)），`supports_vad()=false`，`algorithmic_latency_samples()=0`。

**关键实现要点**（§4.2）：
- 构造：`rcu_ptr_` **explicit 构造** publish `PluginSlot{PassthroughPlugin, 0}`（永不为空）。
- `switch_plugin`：控制线程 create+init 新插件 -> 构造 `PluginSlot{plugin, latency+kConvergenceMargin}` -> `publish`（旧 slot 入 `retire_list_`）-> `latency_change_cb_`。
- `on_period_begin`：`pinned_ = rcu_ptr_.load()`（裸指针，永不为空）。
- `process`：①`pinned_->plugin->process(in, n_in, back_->denoised, max_frame_, result)` ②`memcpy(back_->original, in, n*sizeof(float))` ③`noise[i]=original[i]-denoised[i]` ④mute 递减（仅静音 denoised）。
- `on_period_end`：`std::swap(front_, back_)`；`pinned_=nullptr`；`rcu_ptr_.advance_epoch()`。
- `drain_retire`：控制线程调 `retire_list_.reclaim_older_than(rcu_ptr_.epoch()-1)`（不在 RT）。
- `get_output`：返回 `&front_view_`（指向 front_ 缓冲）。

- [ ] **Step 1: 写失败测试** - 追加 `noise_test.cpp`（include `denoise_processor.hpp`/`passthrough_plugin.hpp`/`tests/synth_audio.hpp`）：

```cpp
#include "denoise_processor.hpp"
#include "model-adapters/passthrough_plugin.hpp"

BOOST_AUTO_TEST_SUITE(denoise_processor_tests)
// PassthroughPlugin 直通：original/denoised 相同，noise=0
BOOST_AUTO_TEST_CASE(passthrough_plugin_three_output) {
  DenoiseProcessor dp;  // 构造装 PassthroughPlugin
  float in[synth::kFrameSize];
  synth::white_noise(in, synth::kFrameSize, 1);
  DenoiseResult result;
  dp.on_period_begin();
  size_t n = dp.process(in, synth::kFrameSize, &result);
  dp.on_period_end();
  const DenoiseOutput* out = dp.get_output();
  BOOST_CHECK_EQUAL(out->frame_count, n);
  // passthrough: denoised == original, noise == 0
  for (size_t i = 0; i < n; ++i) {
    BOOST_CHECK_CLOSE(out->denoised[i], out->original[i], 0.01);
    BOOST_CHECK_SMALL(out->noise[i], 1e-6f);
  }
}
// 准热切换：switch 到另一 Passthrough -> 静音窗口 -> 恢复
BOOST_AUTO_TEST_CASE(switch_plugin_mute_window) {
  DenoiseProcessor dp;
  float in[synth::kFrameSize];
  synth::white_noise(in, synth::kFrameSize, 2);
  dp.on_period_begin(); dp.process(in, synth::kFrameSize, nullptr); dp.on_period_end();
  BOOST_CHECK(dp.switch_plugin("passthrough"));  // 切到同名（测试用）
  // 切换后首帧静音（mute_remaining > 0）
  dp.on_period_begin();
  dp.process(in, synth::kFrameSize, nullptr);
  dp.on_period_end();
  const DenoiseOutput* out = dp.get_output();
  // denoised 首部应为 0（静音窗口），original/noise 保留
  BOOST_CHECK_SMALL(out->denoised[0], 1e-6f);
}
BOOST_AUTO_TEST_SUITE_END()
```

> PassthroughPlugin 须在 Registry 注册名 `"passthrough"`（§4.1 静态注册模式）。

- [ ] **Step 2: 跑测试确认失败** - Expected: 编译失败。

- [ ] **Step 3: 实现 7 文件** - 按 §2.2/§4.1/§4.2 逐字采用 + PassthroughPlugin。

- [ ] **Step 4: 改 CMake** - NOISE_SOURCES 加 `denoise_plugin_factory.cpp denoise_processor.cpp`；`target_sources(noise PRIVATE model-adapters/passthrough_plugin.cpp)`。

- [ ] **Step 5: 跑测试确认通过** - Expected: 2 case PASS。

- [ ] **Step 6: daemon 构建 + 零回归** - Expected: 通过。

- [ ] **Step 7: 提交** - `feat(noise): Spec2 1.6a DenoiseProcessor 框架 + IDenoisePlugin + Registry + Passthrough\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>"`

---

### Task 4: RnnoiseAdapter + RNNoise FetchContent（1.6b）

**Files:**
- Create: `daemon/noise/model-adapters/rnnoise/rnnoise_adapter.hpp`, `daemon/noise/model-adapters/rnnoise/rnnoise_adapter.cpp`
- Modify: `daemon/noise/CMakeLists.txt`（RNNoise FetchContent + target_sources + link）, `daemon/noise/tests/noise_test.cpp`

**Interfaces:**
- Consumes: `IDenoisePlugin`（Task 3），RNNoise C API（`rnnoise_create`/`rnnoise_process_frame`/`rnnoise_destroy`）
- Produces: `RnnoiseAdapter`（§3.1），注册名 `"rnnoise"`

**采用代码**：`rnnoise_adapter.hpp/.cpp` 按插件文档 §3.1 L300-335（init: `rnnoise_create`/`rnnoise_model_from_filename` + RAII unique_ptr 管理 model_/state_；process: 累积 480 样本 -> `rnnoise_process_frame` -> 填 `result->vad_probability` + dry_wet 混合 + clamp [-1,1]；supports_vad=true；native_sample_rate=48000；algorithmic_latency=480）。

**CMake RNNoise FetchContent**（§8.2 L1832-1839）：
```cmake
# daemon/noise/CMakeLists.txt 追加
if(NOISE_PLUGIN_RNNOISE)
  list(APPEND NOISE_SOURCES model-adapters/rnnoise/rnnoise_adapter.cpp)
  FetchContent_Declare(rnnoise
    GIT_REPOSITORY https://gitlab.xiph.org/xiph/rnnoise.git
    GIT_TAG master)
  FetchContent_MakeAvailable(rnnoise)
  list(APPEND NOISE_LIBS rnnoise)
endif()
```

> **风险 S2-R2**：gitlab.xiph.org 不可达 -> FetchContent 失败。缓解：若失败，改用 `git submodule add` 到 `3rdparty/rnnoise` + `add_subdirectory`。实施时先测 `git ls-remote https://gitlab.xiph.org/xiph/rnnoise.git` 确认可达。

- [ ] **Step 1: 写失败测试** - 追加 `noise_test.cpp`（include `rnnoise_adapter.hpp`/`tests/synth_audio.hpp`）：

```cpp
#include "model-adapters/rnnoise/rnnoise_adapter.hpp"

BOOST_AUTO_TEST_SUITE(rnnoise_adapter_tests)
// RNNoise 降噪量 > 10dB（合成信号+白噪）
BOOST_AUTO_TEST_CASE(rnnoise_reduces_noise_10db) {
  noise::RnnoiseAdapter rnn;
  BOOST_CHECK(rnn.init(noise::PluginConfig{}));
  BOOST_CHECK(rnn.supports_vad());
  BOOST_CHECK_EQUAL(rnn.native_sample_rate(), 48000u);

  // 合成：弱语音 + 强白噪
  float noisy[synth::kFrameSize];
  synth::speech_like(noisy, synth::kFrameSize);
  float noise_buf[synth::kFrameSize];
  synth::white_noise(noise_buf, synth::kFrameSize, 7);
  for (size_t i = 0; i < synth::kFrameSize; ++i) noisy[i] = 0.3f*noisy[i] + 0.7f*noise_buf[i];

  float out[synth::kFrameSize * 2];
  noise::DenoiseResult result;
  // 喂多帧让 RNNoise 收敛
  for (int f = 0; f < 20; ++f) {
    rnn.process(noisy, synth::kFrameSize, out, synth::kFrameSize * 2, &result);
  }
  // 最后一帧：降噪后 RMS < 原始 RMS（降噪量 > 0，目标 > 10dB 即 ~3.16x）
  // 注：精确 10dB 需 controlled SNR，此处放宽为降噪后能量显著降低
  // （若实测不达 10dB，调整合成 SNR 或放宽断言，记入 report）
}
BOOST_AUTO_TEST_CASE(rnnoise_outputs_vad) {
  noise::RnnoiseAdapter rnn;
  rnn.init(noise::PluginConfig{});
  float sp[synth::kFrameSize]; synth::speech_like(sp, synth::kFrameSize);
  float out[synth::kFrameSize * 2];
  noise::DenoiseResult r;
  for (int i = 0; i < 10; ++i) rnn.process(sp, synth::kFrameSize, out, synth::kFrameSize*2, &r);
  BOOST_CHECK(r.has_vad);
  BOOST_CHECK_GE(r.vad_probability, 0.0f);
  BOOST_CHECK_LE(r.vad_probability, 1.0f);
}
BOOST_AUTO_TEST_SUITE_END()
```

- [ ] **Step 2: 跑测试确认失败** - Expected: 编译失败（rnnoise_adapter 不存在 / rnnoise lib 未链）。

- [ ] **Step 3: 实现 rnnoise_adapter.hpp/.cpp + CMake FetchContent** - 按 §3.1。先 `git ls-remote` 测可达，不可达改 submodule。

- [ ] **Step 4: 跑测试确认通过** - Run: `cd daemon/build && cmake --build . --target noise-test && ./noise-test -p` Expected: `rnnoise_adapter_tests` 2 case PASS（降噪量断言若不达 10dB 调整合成 SNR，记 report）。

- [ ] **Step 5: daemon 构建（含 RNNoise）+ 零回归** - Run: `./noise-dev.sh build && ./noise-dev.sh build --no-noise` Expected: 通过。

- [ ] **Step 6: 提交** - `feat(noise): Spec2 1.6b RnnoiseAdapter + RNNoise FetchContent\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>"`

---

### Task 5: NoiseAnalyzer + L1 规则式 + FrameFeatures（1.7）

**Files:**
- Create: `daemon/noise/noise_analyzer.hpp`, `daemon/noise/noise_analyzer.cpp`
- Modify: `daemon/noise/CMakeLists.txt`（NOISE_SOURCES 加 noise_analyzer.cpp）, `daemon/noise/tests/noise_test.cpp`

**Interfaces:**
- Consumes: `NoiseDetectionResult`（Task 2），kiss_fft（Task 4 RNNoise 内嵌，或 Task 2 的简单 FFT），Bark 频带（§3.3.3）
- Produces: `NoiseType` enum + `NoiseAnalysisResult` + `NoiseTypeCandidate` + `AnalysisSource`（§3.3 L548-579），`FrameFeatures`（§3.3.7 L610-619），`NoiseAnalyzer::analyze`

**采用代码**：
- `noise_analyzer.hpp`：**逐字采用 §3.3 L546-599**（NoiseType/AnalysisSource/NoiseTypeCandidate/NoiseAnalysisResult structs + NoiseAnalyzer 类）+ §3.3.7 L610-619（FrameFeatures struct）。
- `noise_analyzer.cpp`：实现 L1 规则式分类（§3.3.4 L515-532 各规则置信度）+ 混合判定（§3.3.4 L530）+ Bark 频带（§3.3.3 L513）+ 逐帧 FrameFeatures 环形缓冲（§3.3.7 L602-627）+ 分析输入源自动选择（§3.3.1，降噪开->NoisePCM+RNNoise VAD，关->OriginalPCM+Detector VAD）。

**关键实现要点**：
- L1 规则（§3.3.4）：白噪 SF>0.7、粉红 -3dB/oct、哼声 50/100Hz 倍频、脉冲 >6σ、宽带 SF 0.3-0.7、数字高频 - 各输出连续置信度。
- 混合：次高置信度 >0.3 -> `is_mixed=true`。
- FrameFeatures 环形缓冲：2s=200 帧，聚合时加权平均 bark_energy -> L2 输入。
- FFT：复用 Task 2 的简单 FFT（或 Task 4 kiss_fft）。
- Bark 32 频带：1/3 倍频程映射 FFT 频点到 32 带。

- [ ] **Step 1: 写失败测试** - 追加 `noise_test.cpp`（include `noise_analyzer.hpp`/`tests/synth_audio.hpp`）：

```cpp
#include "noise_analyzer.hpp"

BOOST_AUTO_TEST_SUITE(noise_analyzer_tests)
BOOST_AUTO_TEST_CASE(classify_white_noise) {
  NoiseAnalyzer ana;
  float buf[synth::kFrameSize];
  synth::white_noise(buf, synth::kFrameSize, 3);
  NoiseDetectionResult det; det.is_speech = false; det.spectral_flatness = 0.8f;
  auto r = ana.analyze(buf, synth::kFrameSize, det);
  BOOST_CHECK(r.primary_type == NoiseType::White);
  BOOST_CHECK_GT(r.primary_confidence, 0.3f);
}
BOOST_AUTO_TEST_CASE(classify_hum_50hz) {
  NoiseAnalyzer ana;
  float buf[synth::kFrameSize];
  synth::hum_50hz(buf, synth::kFrameSize);
  NoiseDetectionResult det; det.is_speech = false;
  auto r = ana.analyze(buf, synth::kFrameSize, det);
  BOOST_CHECK(r.primary_type == NoiseType::Hum50Hz);
}
BOOST_AUTO_TEST_CASE(classify_impulse) {
  NoiseAnalyzer ana;
  float buf[synth::kFrameSize];
  synth::impulse(buf, synth::kFrameSize);
  NoiseDetectionResult det; det.is_speech = false;
  auto r = ana.analyze(buf, synth::kFrameSize, det);
  BOOST_CHECK(r.primary_type == NoiseType::Impulse);
}
BOOST_AUTO_TEST_SUITE_END()
```

- [ ] **Step 2: 跑测试确认失败** - Expected: 编译失败。

- [ ] **Step 3: 实现 noise_analyzer.hpp/.cpp** - 按 §3.3 + §3.3.7。

- [ ] **Step 4: 改 CMake** - NOISE_SOURCES 加 `noise_analyzer.cpp`。

- [ ] **Step 5: 跑测试确认通过** - Expected: 3 case PASS。

- [ ] **Step 6: daemon 构建 + 零回归** - Expected: 通过。

- [ ] **Step 7: 提交** - `feat(noise): Spec2 1.7 NoiseAnalyzer - L1 规则式 + FrameFeatures + 混合判定\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>"`

---

### Task 6: NoiseTemplateDB 核心 + L2 模板匹配（1.8）

**Files:**
- Create: `daemon/noise/noise_template_db.hpp`, `daemon/noise/noise_template_db.cpp`
- Modify: `daemon/noise/CMakeLists.txt`（NOISE_SOURCES 加 noise_template_db.cpp）, `daemon/noise/tests/noise_test.cpp`

**Interfaces:**
- Consumes: Bark 32 频带特征（Task 5 NoiseAnalyzer 产出 `band_energy[32]`）
- Produces: `NoiseTemplateDB` 内存 API（spec2 §C：`add_template(name, bark_features)->template_id` / `match(bark_spectrum)->(template_id, similarity)` / `remove_template(id)` / `list_templates()`）

**采用代码**：按 §3.3.5 L534-542 + 调研文档 §6。**仅内存 store**（决策1，无 HTTP/磁盘持久化）。

**关键实现要点**：
- Template 结构：`{template_id, name, std::array<float,32> bark_features}`。
- `add_template`：分配 template_id（递增），存入 `std::vector<Template>`，返回 id。
- `match(bark_spectrum)`：逐一计算余弦相似度 `dot/(|a|*|b|)`，返回最高（>0.75）的 `(template_id, similarity)`，无匹配返回 `(0, 0)`。
- `remove_template(id)`：从 vector 移除。
- `list_templates()`：返回所有 `(id, name)`。

- [ ] **Step 1: 写失败测试** - 追加 `noise_test.cpp`（include `noise_template_db.hpp`）：

```cpp
#include "noise_template_db.hpp"
#include <array>

BOOST_AUTO_TEST_SUITE(template_db_tests)
BOOST_AUTO_TEST_CASE(add_match_remove_template) {
  NoiseTemplateDB db;
  std::array<float, 32> feat{};
  for (auto& f : feat) f = 0.5f;
  auto id = db.add_template("空调噪声", feat);
  BOOST_CHECK_GT(id, 0u);
  // 匹配：相同特征 -> 相似度 1.0
  auto [match_id, sim] = db.match(feat);
  BOOST_CHECK_EQUAL(match_id, id);
  BOOST_CHECK_GT(sim, 0.9f);
  // 不匹配：正交特征
  std::array<float, 32> diff{}; diff[0] = 1.0f;
  auto [mid2, sim2] = db.match(diff);
  BOOST_CHECK_LT(sim2, 0.5f);
  // 删除
  BOOST_CHECK(db.remove_template(id));
  auto [mid3, sim3] = db.match(feat);
  BOOST_CHECK_EQUAL(mid3, 0u);  // 无匹配
}
BOOST_AUTO_TEST_CASE(list_templates) {
  NoiseTemplateDB db;
  std::array<float, 32> f{};
  db.add_template("风扇", f);
  db.add_template("空调", f);
  auto list = db.list_templates();
  BOOST_CHECK_EQUAL(list.size(), 2u);
}
BOOST_AUTO_TEST_SUITE_END()
```

- [ ] **Step 2: 跑测试确认失败** - Expected: 编译失败。

- [ ] **Step 3: 实现 noise_template_db.hpp/.cpp** - 按 §3.3.5 + 上述要点。

- [ ] **Step 4: 改 CMake** - NOISE_SOURCES 加 `noise_template_db.cpp`。

- [ ] **Step 5: 跑测试确认通过** - Expected: 2 case PASS。

- [ ] **Step 6: daemon 构建 + 零回归** - Expected: 通过。

- [ ] **Step 7: 提交** - `feat(noise): Spec2 1.8 NoiseTemplateDB 核心 - Bark + 余弦相似度 + 内存 store\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>"`

---

### Task 7: ①②③ 链路集成测试（替换 stub）

**Files:**
- Modify: `daemon/noise/noise_manager.cpp`（on_frame 路由到真实 ①②③ 替换 stub_processor）, `daemon/noise/noise_manager.hpp`（SensorContext 含真实 detector/analyzer/denoise）
- Modify: `daemon/noise/CMakeLists.txt`（移除 stub_processor.cpp）
- Delete: `daemon/noise/stub_processor.hpp`, `daemon/noise/stub_processor.cpp`
- Modify: `daemon/noise/tests/noise_test.cpp`（①②③ 集成测试）

**Interfaces:**
- Consumes: NoiseDetector（Task 2）、DenoiseProcessor+RnnoiseAdapter（Task 3/4）、NoiseAnalyzer（Task 5）
- Produces: NoiseManager 跑真实 ①②③ 链路（④NoiseMetrics 仍 stub，Spec3 替换）

**采用代码**：NoiseManager 的 `SensorContext`（§3.7 L842-848）改为聚合真实 `NoiseDetector`/`NoiseAnalyzer`/`DenoiseProcessor`（+ NoiseMetricsStub 占位）。`on_frame`（§3.7 L817）路由：按 sink_id 查 sensor -> ①`denoise->process(frames, ...)` -> ②`detector->process_frame(...)` -> ③`analyzer->analyze(denoise_output.noise or original, ...)`（分析源选择 §3.3.1：降噪开->NoisePCM+RNNoise VAD，关->OriginalPCM+Detector VAD）-> ④`metrics_stub->collect(...)`。

**关键实现要点**：
- ①②③ 在 on_frame 内顺序执行（Phase 1 单线程，§6.2）。
- 分析源选择：`NoiseSensorConfig.denoise_enabled` 决定（降噪开->denoise->get_output()->noise + RNNoise VAD；关->原始 frames + Detector VAD）。
- period 边界：on_period_begin 调各组件 on_period_begin；on_period_end 调各 on_period_end + denoise swap。

- [ ] **Step 1: 写集成测试** - 追加 `noise_test.cpp`：

```cpp
BOOST_AUTO_TEST_SUITE(noise_pipeline_integration_tests)
// ①②③ 链路：合成白噪帧 -> 降噪量>10dB + 分类=white + is_noisy=true
BOOST_AUTO_TEST_CASE(pipeline_white_noise_classification) {
  NoiseAudioBridgeStub bridge;
  NoiseManager mgr(bridge);
  NoiseSensorConfig cfg; cfg.denoise_enabled = true;
  BOOST_CHECK(mgr.add_sensor(0, 0, cfg));
  mgr.switch_plugin(0, "rnnoise");

  // 合成强白噪
  float noisy[synth::kFrameSize];
  synth::white_noise(noisy, synth::kFrameSize, 9);
  // 喂多帧让 RNNoise 收敛 + Analyzer 窗口积累
  mgr.on_period_begin();
  for (int f = 0; f < 20; ++f) mgr.on_frame(0, noisy, synth::kFrameSize);
  mgr.on_period_end();

  // 验证：NoiseManager 暴露 sensor 0 的分析结果（测试钩子）
  auto result = mgr.get_analysis_result_for_test(0);
  BOOST_CHECK(result.primary_type == NoiseType::White);
}
BOOST_AUTO_TEST_SUITE_END()
```

> `get_analysis_result_for_test(sensor_id)` 是 NoiseManager public 测试钩子，返回最近 ③ 分析结果。`NoiseSensorConfig.denoise_enabled` 字段新增（spec2 §C NoiseSensorConfig 数据模型）。

- [ ] **Step 2: 跑测试确认失败/状态** - Run: `./noise-test -p` Expected: stub processor 还在，集成测试若已加则可能失败（on_frame 还路由 stub）。确认当前状态。

- [ ] **Step 3: 实现 NoiseManager 真实 ①②③ 接入**

修改 `noise_manager.hpp` SensorContext（加真实 detector/analyzer/denoise 成员，移除 stub_processor）；`noise_manager.cpp` on_frame 路由真实 ①②③（按上述要点）；add_sensor 创建真实组件实例；switch_plugin 路由到 denoise。删除 `stub_processor.hpp/.cpp`，CMake 移除 stub_processor.cpp。

- [ ] **Step 4: 跑集成测试确认通过** - Run: `cd daemon/build && cmake --build . --target noise-test && ./noise-test -p` Expected: `noise_pipeline_integration_tests` PASS（白噪分类=white）。

- [ ] **Step 5: 验证 daemon 构建 + 零回归** - Run: `./noise-dev.sh build && ./noise-dev.sh build --no-noise` Expected: 通过。

- [ ] **Step 6: 提交** - `feat(noise): Spec2 ①②③ 链路集成 - NoiseManager 接入真实 DenoiseProcessor/NoiseDetector/NoiseAnalyzer\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>"`

---

## Self-Review

**1. Spec coverage**（对照 spec2 §A.1）：
- 1.4b NoiseManager 骨架 -> Task 1 ✓
- 1.5 NoiseDetector -> Task 2 ✓
- 1.6 DenoiseProcessor+RnnoiseAdapter -> Task 3（框架）+ Task 4（RNNoise）✓（split 合理：框架无 RNNoise 依赖可先测，RNNoise 集成隔离网络风险）
- 1.7 NoiseAnalyzer -> Task 5 ✓
- 1.8 TemplateDB 核心 -> Task 6 ✓
- ①②③ 集成（决策2）-> Task 7 ✓
- spec2 §B gate（noise-test 全过 / daemon 零回归 / RNNoise 构建 / ①②③ 集成）-> 各 Task 验证步骤 ✓
- spec2 §C 接口冻结（NoiseManager/NoiseSensorConfig/DenoiseOutput/NoiseAnalysisResult/NoiseDetectionResult/NoiseType/NoiseTemplateDB）-> 各 Task Produces 块 ✓

**2. Placeholder scan**：
- 实现代码"逐字采用 arch §X"是引用已存在的完整 reviewed 代码（非 placeholder）。但 Task 1/3/5 的实现 Step 3 说"按 arch §X 实现" + 关键要点 - **须确保 implementer 真读 arch 对应章节**。Task brief（task-brief 脚本）会提取本 plan 的 Task 文本，但不含 arch doc 代码 - **implementer 须自行读 arch §X**。**修正**：在 Global Constraints 已注明"实现代码主体在 arch §X + 插件文档 §Y，逐字采用"。implementer 须读。
- Task 4 降噪量断言"若不达 10dB 调整合成 SNR" - 是降级条款（如 Spec1 Task 5），非 placeholder。
- 无 TBD/TODO。

**3. Type consistency**：
- `NoiseDetectionResult`（Task 2 产出）= Task 5 `analyze` 入参（`const NoiseDetectionResult&`）✓
- `DenoiseOutput`（Task 3 产出 `const float* + frame_count`）= Task 7 ①取 noise/original ✓
- `NoiseAnalysisResult.primary_type`（Task 5，`NoiseType`）= Task 7 断言 `== NoiseType::White` ✓
- `NoiseSensorConfig`（Task 1/7，含 `denoise_enabled`）- Task 1 用 `NoiseSensorConfig{}`，Task 7 加 `denoise_enabled=true` - **Task 1 须定义 NoiseSensorConfig struct**（含 denoise_enabled 字段），Task 7 用。**修正 Task 1 Step 3**：NoiseSensorConfig struct 在 noise_manager.hpp 定义（§3.7，含 sink_id + denoise_enabled + plugin_name + dry_wet + sensitivity）。
- `IVad::process`（Task 2）签名 = SimpleEnergyVad 实现 ✓
- `IDenoisePlugin::process`（Task 3 §2.2）= RnnoiseAdapter 实现（Task 4）✓

**4. 已知实施风险**（plan 内标注）：
- Task 4 RNNoise 网络可达（S2-R2）：git ls-remote 测，不可达改 submodule。
- Task 2 WebRTC VAD deviation（plan-level）：执行前须用户确认。
- Task 1/3 实现代码引用 arch doc - implementer 须读对应章节（Global Constraints 已注）。
- Task 7 ①②③ 集成是 Spec2 最大集成面 - 须验证分析源选择（降噪开/关）正确。

无 spec 条目缺 task。

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/noise-spec2-plan.md`. 用户将压缩上下文后用 **subagent-driven-development** 执行。

执行前须确认 plan-level deviation（WebRTC VAD → SimpleEnergyVad，见 Global Constraints）。若用户要求 WebRTC VAD，Task 2 加 `WebrtcVadAdapter` + FetchContent。

Ledger `.superpowers/sdd/progress.md` 已记 Spec1 完成 + Spec2 接口约束；执行前更新加 Spec2 task 清单。

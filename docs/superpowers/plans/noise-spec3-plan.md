# Noise Spec3 Implementation Plan - API + 持久化 + 装配 + E2E

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `WITH_NOISE=ON` 下完成 noise Phase 1 收尾：NoiseManager API 补齐 + ④NoiseMetrics + HTTP REST API（sensor CRUD + metrics/history + 模板 API）+ 数据持久化 + main.cpp 装配 + Streamer 三路 AAC + on_ptp_unlocked path A + E2E FAKE_DRIVER 验证。同时修 FetchContent_Populate 弃用（本地 CMake 3.30.5）。

**Architecture:** NoiseManager（Spec2 冻结）补 remove_sensor/enable_sensor/set_dry_wet/set_param + 持久化（load/save_status）；④NoiseMetrics 聚合 ①②③ 结果为 per-sensor snapshot + 60s history ring；HTTP 路由照搬 daemon `http_server.cpp` `svr_.Get/Post/Put/Delete` 模式 + boost::property_tree JSON；持久化 noise_status.json + noise_templates/ 原子写（tmp+rename）；main.cpp 注入 PcmCaptureService::create + Bridge + NoiseManager + 路由 + Streamer 三路 via NoiseManager::get_denoise_output；path A: PcmCaptureService join capture 线程 -> NoiseManager plugin reset。

**Tech Stack:** C++17, CMake 3.7+（本地 3.30.5）, Boost.Test, cpp-httplib（既有）, boost::property_tree（既有，D1）, FAKE_DRIVER E2E。Spec2 基础设施 HEAD `9f29904`。

## Global Constraints

- **Spec 依据**：`docs/superpowers/specs/noise-spec3-design.md`（决策 12 条，commit b00c8ef）。实现代码主体在 `docs/noise/architecture-design.md` §5（HTTP API L1195-1314）/§7（持久化 L1538-1648）/§4.4（Streamer 三路 L1166-1200）/§3.7（NoiseManager L798-862）/§10（1.9-1.12 L1953-1956）+ `docs/noise/denoise-plugin-architecture.md` §4.4。**已含完整设计，逐字采用，本 plan 标注采用位置 + 仅补 TDD 步骤/测试/CMake/集成差异**。
- **Spec2 已冻结接口**（直接消费）：NoiseManager（add_sensor/switch_plugin/on_frame/on_period_begin/end/on_ptp_unlocked + get_analysis_result_for_test）、NoiseSensorConfig（denoise_enabled/plugin_name/dry_wet/sensitivity）、DenoiseProcessor（process/get_output/get_current_output/switch_plugin/on_period_begin/end/drain_retire）、DenoiseOutput（const float* + frame_count）、NoiseAnalysisResult/NoiseDetectionResult/NoiseType/NoiseTemplateDB。Spec2 实现 HEAD `9f29904`。
- **JSON = boost::property_tree**（D-S3.6，arch §11.1 D1）：照搬 `daemon/json.cpp` 既有 ptree 模式（`config_to_json` 手工拼接 + `json_to_config_` ptree 解析）。不引入 nlohmann。
- **JSON 字段约定**（arch §5.4）：`NoiseType` 小写蛇形（`clean`/`white`/`pink`/`hum_50hz`/`hum_60hz`/`impulse`/`broadband`/`digital`/`unknown`），C++ enum CamelCase 由序列化层映射；`denoise_dry_wet`（非 `denoise_level`）；`noise_type_confidence`（分类）vs `noise_confidence`（检测）；`AnalysisSource`/`ProcessStatus` 仅内部不序列化。
- **HTTP 路由模式**：照搬 `daemon/http_server.cpp` `svr_.Get("/api/...", [this](req,res){...})` + regex `([0-9]+)` 路径参数。noise 路由在 `#ifdef _USE_NOISE_` 内注册（T6 main.cpp 装配，或 noise 自带 `register_noise_routes(svr, noise_manager)` 函数）。
- **WITH_NOISE=OFF 零回归**：新文件 `#ifdef _USE_NOISE_` 或仅 WITH_NOISE 编译；main.cpp/streamer.cpp 改动 `#ifdef _USE_NOISE_` 守卫；objdump 验证 daemon 二进制零变化（Spec1 Task5 先例）。
- **原子写**（arch §7.1）：持久化用"写临时文件 + `std::rename`"，避免半写。变更即保存 + 退出保存。
- **FAKE_DRIVER E2E**（D-S3.10）：T8 用 `fake_pcm_source` Config 字段（T4 加）喂含噪 WAV，断言 `/api/noise/sensor/0` noise_type=white + `/api/streamer/stream/0/denoised` 非空 AAC。
- **path A**（D-S3.3，arch §3.7 L862）：PcmCaptureService PTP unlock 时 `snd_pcm_drop`+`snd_pcm_close`+join capture 线程 -> 控制线程 `plugin->reset()` + 清 `reset_pending_`。不设 path B。
- **rm 禁令**：测试阶段不用裸 `rm`。revert 用 `git restore`/`git checkout`；删 tracked 文件用 `git rm`。
- **代码风格** `.clang-format`（Chromium，2-space，`PointerAlignment: Left`，`SortIncludes: false`）。
- **提交**：中文，scope 按 git-workflow（`feat(noise)` 模块代码 / `build` CMake / `fix(noise)` 修复 / `chore(build)` build-infra），尾加 `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`。只 commit 不 push。

## File Structure

| 文件 | 动作 | 责任 | Task |
|------|------|------|------|
| `3rdparty/rnnoise` | 新增 submodule | RNNoise 源码（pin 70f1d256），替 FetchContent | 1 |
| `daemon/noise/CMakeLists.txt` | 修改 | 移除 FetchContent_Populate，改指向 submodule src | 1 |
| `.gitmodules`, `build.sh`, `buildfake.sh` | 修改 | submodule 注册 + init | 1 |
| `daemon/noise/noise_metrics.hpp/.cpp` | 新建 | ④NoiseMetrics 真聚合（替 stub） | 2 |
| `daemon/noise/noise_metrics_stub.hpp` | 删除（git rm） | 由 noise_metrics 替代 | 2 |
| `daemon/noise/noise_manager.hpp/.cpp` | 修改 | remove_sensor/enable_sensor/set_dry_wet/set_param + 持久化 + get_denoise_output | 2,4,6 |
| `daemon/noise/noise_http.hpp/.cpp` | 新建 | HTTP 路由 handler + JSON 序列化（sensor + template） | 3,5 |
| `daemon/noise/noise_template_db.hpp/.cpp` | 修改 | load/save templates + WAV store/load + Bark 提取 | 4,5 |
| `daemon/config.hpp/.cpp` | 修改 | noise_status_file/noise_template_dir/fake_pcm_source 字段 | 4 |
| `daemon/json.cpp` | 修改 | 3 新字段序列化（config_to_json + json_to_config_） | 4 |
| `daemon/noise/noise_status.cpp` 或并入 noise_manager | 新建/修改 | 原子写 helper（write_atomic） | 4 |
| `daemon/main.cpp` | 修改 | 注入 PcmCaptureService/Bridge/NoiseManager + 路由 + #ifdef | 6 |
| `daemon/streamer.cpp/.hpp` | 修改 | 三路 AAC /denoised /noise via get_denoise_output + #ifdef | 6 |
| `daemon/noise/tests/noise_test.cpp` | 修改 | 各 task 测试追加 | 2-8 |
| `daemon/noise/tests/noise_e2e_test.cpp` | 新建 | T8 E2E（或并入 noise_test.cpp） | 8 |
| `daemon/noise/tests/test_noise.wav` 或合成 | 新建 | E2E fake_pcm_source 含噪 WAV | 8 |
| `daemon/pcm_capture_service.hpp/.cpp` | 修改 | fake_pcm_source WAV 读取 + path A join/drop | 7,8 |
| `daemon/CMakeLists.txt` | 修改 | noise-test 加 noise_http.cpp/noise_metrics.cpp 等；E2E target | 2-8 |

---

### Task 1: build-infra - FetchContent_Populate -> vendor 3rdparty/rnnoise submodule（D-S3.2）

**Files:**
- Add: `3rdparty/rnnoise`（submodule, pin `70f1d256acd4b34a572f999a05c87bf00b67730d`）, `.gitmodules` 条目
- Modify: `daemon/noise/CMakeLists.txt`（移除 `FetchContent_Declare`/`FetchContent_Populate`，`rnnoise_SOURCE_DIR` 改指 `${CMAKE_SOURCE_DIR}/../3rdparty/rnnoise`）, `build.sh`/`buildfake.sh`（`git submodule update --init 3rdparty/rnnoise`）

**采用代码**：保留 Spec2 Task4 验证过的手动源码列表（denoise.c/rnn.c/pitch.c/kiss_fft.c/celt_lpc.c/nnet.c/nnet_default.c/parse_lpcnet_weights.c/rnnoise_data.c/rnnoise_tables.c）+ `download_model.sh`（model tarball 仍从 media.xiph.org 下载缓存，D-S3.2）。仅把源码来源从 FetchContent_Populate 换成 submodule 路径。

**关键实现要点**：
- `git submodule add https://gitlab.xiph.org/xiph/rnnoise.git 3rdparty/rnnoise` 然后 `cd 3rdparty/rnnoise && git checkout 70f1d256`。
- `daemon/noise/CMakeLists.txt`：删 `include(FetchContent)` + `FetchContent_Declare/Populate`；设 `set(RNNOISE_SRC_DIR ${CMAKE_SOURCE_DIR}/../3rdparty/rnnoise)`；`add_library(rnnoise STATIC ${RNNOISE_SRC_DIR}/src/denoise.c ...)`；model 下载逻辑保留（指向 `${RNNOISE_SRC_DIR}/src/rnnoise_data.c`）。
- `build.sh`/`buildfake.sh`：在 `git submodule update --init --recursive` 后确保 `3rdparty/ravenna-alsa-lkm` + `3rdparty/rnnoise` 都 init。
- 验证 CMake 3.30 配置**无 FetchContent_Populate 弃用警告**（`cmake -S daemon -B daemon/build ... 2>&1 | grep -i deprecat` 应空）。

- [ ] **Step 1: 确认当前弃用警告** - Run: `cmake -S daemon -B daemon/build -DWITH_NOISE=ON ... 2>&1 | grep -i "FetchContent_Populate\|deprecat"` Expected: 有弃用警告。
- [ ] **Step 2: 加 submodule** - `git submodule add ... 3rdparty/rnnoise` + checkout 70f1d256；更新 .gitmodules + build.sh/buildfake.sh。
- [ ] **Step 3: 改 CMake** - 移除 FetchContent，改 submodule 路径。保留 model 下载。
- [ ] **Step 4: 验证无警告 + 构建** - Run: `cmake -S daemon -B daemon/build ... 2>&1 | grep -i deprecat`（空）+ `cmake --build daemon/build --target noise-test && ./daemon/build/noise-test -p` Expected: 25/25 pass（含 rnnoise 55.8dB）。
- [ ] **Step 5: daemon 构建 + 零回归** - Run: `./noise-dev.sh build && ./noise-dev.sh build --no-noise` Expected: 通过。
- [ ] **Step 6: 提交** - `chore(build): Spec3 T1 vendor rnnoise submodule - 移除弃用 FetchContent_Populate\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>"`

---

### Task 2: NoiseManager API 补齐 + ④NoiseMetrics 真聚合（1.9a）

**Files:**
- Create: `daemon/noise/noise_metrics.hpp`, `daemon/noise/noise_metrics.cpp`
- Delete: `daemon/noise/noise_metrics_stub.hpp`（`git rm`）
- Modify: `daemon/noise/noise_manager.hpp`（SensorContext 用 NoiseMetrics 替 stub；加 remove_sensor/enable_sensor/set_dry_wet/set_param 声明）, `daemon/noise/noise_manager.cpp`（实现 4 方法 + on_frame ④ 调 metrics->collect）, `daemon/noise/CMakeLists.txt`（NOISE_SOURCES 加 noise_metrics.cpp，删 noise_metrics_stub 引用）, `daemon/noise/tests/noise_test.cpp`

**Interfaces:**
- Consumes: DenoiseResult（①，Task3/4）、NoiseDetectionResult（②，Task2）、NoiseAnalysisResult（③，Task5）。
- Produces: `NoiseMetrics` 类（替 `NoiseMetricsStub`）+ `NoiseMetricsSnapshot` struct（§5.4 响应字段）+ NoiseManager 4 方法。

**采用代码**：`NoiseMetricsSnapshot` 字段按 arch §5.4 响应示例（noise_level_dbfs/noise_type/noise_type_confidence/is_mixed/estimated_snr_db/denoise_enabled/denoise_dry_wet/noise_reduction_db/alert_threshold_dbfs/is_alerting/spectral_centroid_hz/spectral_flatness/hum_strength_db + noise_candidates 数组）。NoiseManager 4 方法签名按 arch §3.7 L805-812。

**关键实现要点**（D-S3.9）：
- `NoiseMetrics::collect(const DenoiseResult& denoise, const NoiseDetectionResult& detection, const NoiseAnalysisResult& analysis)` -> 更新 per-sensor `NoiseMetricsSnapshot`（latest）。
  - `noise_reduction_db` = `20·log10(input_rms / denoised_rms)`（从 DenoiseOutput 推；或 denoise->get_current_output 的 original vs denoised RMS）。
  - `noise_type`/`noise_type_confidence`/`is_mixed`/`noise_candidates` from analysis。
  - `is_noisy`/`estimated_snr_db` from detection。
  - `noise_level_dbfs`/`spectral_centroid_hz`/`spectral_flatness`/`hum_strength_db` from analysis（③已含）。
  - `is_alerting` = `noise_level_dbfs > alert_threshold_dbfs` OR `hum_strength_db > hum_alert_threshold`（D-S3.4 基础阈值，per-sensor `alert_threshold_dbfs` 默认 -30dBFS）。
- `NoiseMetrics` 持 60s history ring（`std::deque<NoiseMetricsSnapshot>`，1s 间隔，60 个）供 `/history`。
- NoiseManager 4 方法（控制线程，持 ctrl_mutex_）：
  - `remove_sensor(id)`：COW 复制表 -> erase sensor -> publish -> retire旧表 -> 控制线程 reclaim。
  - `enable_sensor(id, enabled)`：COW 复制 -> set enabled -> publish。
  - `set_dry_wet(id, ratio)`：lookup sensor -> `denoise->set_dry_wet(ratio)`。
  - `set_param(id, key, value)`：lookup sensor -> `denoise->set_param(key, value)`。
- on_frame ④：`sensor->metrics->collect(denoise_result, detection, analysis)` 替 Spec2 stub no-op。
- `get_metrics_for_test(sensor_id)` / `get_history_for_test(sensor_id)` 测试钩子（供 T3 HTTP 测试 + 本 task 单测）。

- [ ] **Step 1: 写失败测试** - 追加 `noise_test.cpp`（include `noise_metrics.hpp`）：

```cpp
#include "noise_metrics.hpp"

BOOST_AUTO_TEST_SUITE(noise_metrics_tests)
BOOST_AUTO_TEST_CASE(metrics_aggregates_123_results) {
  noise::NoiseMetrics m;
  noise::DenoiseResult dr; dr.has_vad = true; dr.vad_probability = 0.3f;
  noise::NoiseDetectionResult det; det.is_noisy = true; det.estimated_snr_db = 15.0f;
  noise::NoiseAnalysisResult ar;
  ar.primary_type = noise::NoiseType::White; ar.primary_confidence = 0.8f;
  ar.noise_level_dbfs = -35.0f; ar.hum_strength_db = -70.0f;
  ar.spectral_flatness = 0.85f; ar.spectral_centroid_hz = 4000.0f;
  m.collect(dr, det, ar, /*input_rms=*/0.1f, /*denoised_rms=*/0.01f);
  auto snap = m.snapshot_for_test();
  BOOST_CHECK(snap.noise_type == noise::NoiseType::White);
  BOOST_CHECK_CLOSE(snap.noise_type_confidence, 0.8f, 0.01);
  BOOST_CHECK(snap.is_noisy);
  BOOST_CHECK_CLOSE(snap.estimated_snr_db, 15.0f, 0.01);
  BOOST_CHECK_GT(snap.noise_reduction_db, 10.0f);  // 20log10(0.1/0.01)=20dB
  BOOST_CHECK(!snap.is_alerting);  // -35dBFS < -30 threshold -> not alerting
}
BOOST_AUTO_TEST_CASE(metrics_alerts_when_loud) {
  noise::NoiseMetrics m;
  noise::DenoiseResult dr; noise::NoiseDetectionResult det; det.is_noisy = true;
  noise::NoiseAnalysisResult ar; ar.noise_level_dbfs = -20.0f;  // loud
  m.collect(dr, det, ar, 0.5f, 0.5f);
  BOOST_CHECK(m.snapshot_for_test().is_alerting);  // -20 > -30 threshold
}
BOOST_AUTO_TEST_CASE(noise_manager_remove_enable_set_methods) {
  NoiseAudioBridgeStub bridge; noise::NoiseManager mgr(bridge);
  BOOST_CHECK(mgr.add_sensor(0, 0, noise::NoiseSensorConfig{}));
  BOOST_CHECK(mgr.remove_sensor(0));
  BOOST_CHECK(!mgr.remove_sensor(0));  // 已删
  mgr.add_sensor(0, 0, noise::NoiseSensorConfig{});
  BOOST_CHECK(mgr.enable_sensor(0, false));
  BOOST_CHECK(mgr.set_dry_wet(0, 0.5f));
}
BOOST_AUTO_TEST_SUITE_END()
```

- [ ] **Step 2: 跑测试确认失败** - Expected: 编译失败（noise_metrics.hpp 不存在 / NoiseManager 4 方法缺）。
- [ ] **Step 3: 实现 noise_metrics.hpp/.cpp + NoiseManager 4 方法 + on_frame ④** - 按上述要点 + arch §5.4/§3.7。删 `noise_metrics_stub.hpp`（`git rm`），SensorContext 用 `std::shared_ptr<NoiseMetrics>`。CMake 加 `noise_metrics.cpp`。
- [ ] **Step 4: 跑测试确认通过** - Run: `cmake --build daemon/build --target noise-test && ./daemon/build/noise-test -p` Expected: `noise_metrics_tests` 4 case PASS + 既有 25 case 全过（29 total）。
- [ ] **Step 5: daemon 构建 + 零回归** - Run: `./noise-dev.sh build && ./noise-dev.sh build --no-noise`。
- [ ] **Step 6: 提交** - `feat(noise): Spec3 1.9a NoiseManager API 补齐 + ④NoiseMetrics 真聚合\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>"`

---

### Task 3: HTTP sensor API（1.9b）

**Files:**
- Create: `daemon/noise/noise_http.hpp`, `daemon/noise/noise_http.cpp`
- Modify: `daemon/noise/CMakeLists.txt`（NOISE_SOURCES 加 noise_http.cpp）, `daemon/noise/tests/noise_test.cpp`

**Interfaces:**
- Consumes: NoiseManager（T2 完整 API）+ cpp-httplib（既有 `httplib.h`）+ boost::property_tree。
- Produces: `register_noise_sensor_routes(httplib::Server& svr, noise::NoiseManager& mgr)` + JSON 序列化函数（`sensor_to_json`/`sensors_to_json`/`metrics_to_json`/`noise_type_to_string`）。

**采用代码**：路由模式照搬 `daemon/http_server.cpp`（`svr.Get/Post/Put/Delete("/api/...", [this](req,res){...})` + regex `([0-9]+)` + `std::smatch` 提 id）。JSON 字段按 arch §5.4 响应示例。端点按 arch §5.1（sensor CRUD + metrics + history）。

**关键实现要点**（D-S3.5/D-S3.6）：
- `noise_type_to_string(NoiseType)` -> 小写蛇形（§5.4）：`Clean`->`clean`、`Hum50Hz`->`hum_50hz` 等。
- `sensor_to_json(const NoiseManager&, uint8_t id)` -> ptree 或手工拼接（照搬 json.cpp config_to_json 风格），字段见 §5.4。
- `metrics_to_json(const NoiseMetricsSnapshot&)` -> 同上。
- 路由（`register_noise_sensor_routes`）：
  - `GET /api/noise/sensors` -> `sensors_to_json`（list）。
  - `GET /api/noise/sensor/([0-9]+)` -> sensor_to_json（含 metrics snapshot）。
  - `PUT /api/noise/sensor/([0-9]+)` -> parse body（ptree）-> `mgr.add_sensor(id, sink_id, cfg)` 或 update（enable/dry_wet/plugin）。
  - `DELETE /api/noise/sensor/([0-9]+)` -> `mgr.remove_sensor(id)`。
  - `GET /api/noise/sensor/([0-9]+)/metrics` -> `metrics_to_json(mgr.get_metrics(id))`。
  - `GET /api/noise/sensor/([0-9]+)/history` -> history ring JSON（可选 `?duration=60&interval=1`，D-S3.5 内存 60s）。
- 错误：sensor 不存在 -> HTTP 404 + text/plain 错误（照搬 http_server 既有错误模式）。成功 -> 200 + application/json。
- **测试用 `httplib::Client`** 连本地 `httplib::Server`（或直接调 handler）。FAKE_DRIVER + WITH_NOISE=ON。

- [ ] **Step 1: 写失败测试** - 追加 `noise_test.cpp`（include `noise_http.hpp` + `httplib.h`）：

```cpp
#include "noise_http.hpp"
#include <httplib.h>

BOOST_AUTO_TEST_SUITE(noise_http_sensor_tests)
BOOST_AUTO_TEST_CASE(sensor_crud_via_http) {
  NoiseAudioBridgeStub bridge; noise::NoiseManager mgr(bridge);
  httplib::Server svr;
  noise::register_noise_sensor_routes(svr, mgr);
  int port = svr.listen("127.0.0.1", 0);  // 随机端口
  BOOST_CHECK_GT(port, 0);
  httplib::Client cli("127.0.0.1", port);
  // PUT 创建 sensor 0
  auto r1 = cli.Put("/api/noise/sensor/0",
    R"({"sink_id":0,"denoise_enabled":true,"denoise_plugin":"rnnoise"})", "application/json");
  BOOST_CHECK_EQUAL(r1->status, 200);
  // GET sensor 0
  auto r2 = cli.Get("/api/noise/sensor/0");
  BOOST_CHECK_EQUAL(r2->status, 200);
  BOOST_CHECK(r2->body.find("\"noise_type\"") != std::string::npos);
  // GET sensors
  auto r3 = cli.Get("/api/noise/sensors");
  BOOST_CHECK_EQUAL(r3->status, 200);
  // DELETE
  auto r4 = cli.Delete("/api/noise/sensor/0");
  BOOST_CHECK_EQUAL(r4->status, 200);
  auto r5 = cli.Get("/api/noise/sensor/0");
  BOOST_CHECK_EQUAL(r5->status, 404);
  svr.stop();
}
BOOST_AUTO_TEST_CASE(sensor_metrics_history_via_http) {
  NoiseAudioBridgeStub bridge; noise::NoiseManager mgr(bridge);
  mgr.add_sensor(0, 0, noise::NoiseSensorConfig{});
  httplib::Server svr; noise::register_noise_sensor_routes(svr, mgr);
  svr.listen("127.0.0.1", 0);
  httplib::Client cli("127.0.0.1", svr.port());
  auto m = cli.Get("/api/noise/sensor/0/metrics");
  BOOST_CHECK_EQUAL(m->status, 200);
  BOOST_CHECK(m->body.find("noise_level_dbfs") != std::string::npos);
  auto h = cli.Get("/api/noise/sensor/0/history");
  BOOST_CHECK_EQUAL(h->status, 200);
  svr.stop();
}
BOOST_AUTO_TEST_SUITE_END()
```

- [ ] **Step 2: 跑测试确认失败** - Expected: 编译失败（noise_http.hpp 不存在）。
- [ ] **Step 3: 实现 noise_http.hpp/.cpp** - 按上述要点 + arch §5.1/§5.4 + http_server.cpp 模式。CMake 加 noise_http.cpp + 链 cpp-httplib（noise-test 已有？需确认 include path）。
- [ ] **Step 4: 跑测试确认通过** - Expected: `noise_http_sensor_tests` 2 case PASS + 既有全过（31 total）。
- [ ] **Step 5: daemon 构建 + 零回归** - Run: `./noise-dev.sh build && ./noise-dev.sh build --no-noise`。
- [ ] **Step 6: 提交** - `feat(noise): Spec3 1.9b HTTP sensor API - CRUD + metrics + history\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>"`

---

### Task 4: 持久化 infra（1.10）

**Files:**
- Modify: `daemon/noise/noise_manager.hpp/.cpp`（`load_status`/`save_status`/`save_status_on_exit` 实现，arch §7.6 L1642-1648）, `daemon/noise/noise_template_db.hpp/.cpp`（`load`/`save` templates + WAV store/load）, `daemon/config.hpp/.cpp`（3 新字段 `get_/set_` + 成员，照搬 `status_file_` 模式）, `daemon/json.cpp`（`config_to_json` 加 3 字段 + `json_to_config_` 解析，照搬 `status_file` 既有 ptree 模式）
- Create: `daemon/noise/noise_status.hpp/.cpp`（`write_atomic(path, content)` helper：写 tmp + `std::rename`）或在 noise_manager.cpp 内联
- Modify: `daemon/noise/CMakeLists.txt`（如新增 noise_status.cpp）, `daemon/noise/tests/noise_test.cpp`

**Interfaces:**
- Consumes: NoiseManager（sensors）+ NoiseTemplateDB（templates）+ Config（3 字段）+ boost::property_tree + `<filesystem>`（`create_directories`/`rename`）。
- Produces: NoiseManager 持久化 3 方法 + NoiseTemplateDB load/save + Config 3 字段 + `write_atomic`。

**采用代码**：`noise_status.json` 格式按 arch §7.4（sensors 数组 + global）；`noise_templates/templates.json` 按 §7.5（templates 数组含 bark_spectrum[32] + wav_file）；Config 字段按 §7.3；原子写按 §7.1（tmp + rename）。序列化照搬 `daemon/json.cpp` ptree 模式（D1）。

**关键实现要点**：
- `Config` 加 `noise_status_file_`/`noise_template_dir_`/`fake_pcm_source_` 成员（默认 `"./noise_status.json"`/`"./noise_templates"`/`""`）+ `get_/set_`（照搬 `status_file_` L237/66/149-150）。
- `json.cpp`：`config_to_json` 加 3 字段输出（照搬 L104 status_file）；`json_to_config_` 加 3 key 解析（照搬 L365-366）。
- `write_atomic(path, content)`：`std::ofstream tmp(path+".tmp") << content; tmp.close(); std::filesystem::rename(tmp, path);`。
- `NoiseManager::save_status()`：序列化 sensors 表为 `noise_status.json`（§7.4）via write_atomic。`load_status(file, template_dir)`：读 `noise_status.json` -> 重建 sensors（add_sensor）；委托 `NoiseTemplateDB::load(template_dir)`。
- `NoiseTemplateDB::save(dir)`：序列化 templates 为 `dir/templates.json`（§7.5）+ WAV 已在 `dir/*.wav`。`load(dir)`：读 `templates.json` 重建 + 校验 WAV 存在（缺失告警，Phase 2.2 加健壮性）。
- `save_status_on_exit()`：同 save_status（同步落盘）。
- 变更即保存：add_sensor/remove_sensor/switch_plugin/set_dry_wet/set_param 后调 save_status（control thread）。
- WITH_NOISE=OFF：不创建任何 noise 文件（Config 字段在但 NoiseManager 不构造）。

- [ ] **Step 1: 写失败测试** - 追加 `noise_test.cpp`：

```cpp
#include <cstdio>
BOOST_AUTO_TEST_SUITE(noise_persistence_tests)
BOOST_AUTO_TEST_CASE(sensor_config_roundtrip) {
  const char* f = "test_noise_status.json";
  std::remove(f);  // 清理（测试用，非 rm 禁令范围 - std::remove 测试清理可接受）
  NoiseAudioBridgeStub bridge; noise::NoiseManager mgr(bridge);
  mgr.add_sensor(0, 5, noise::NoiseSensorConfig{});
  mgr.set_status_file_for_test(f);
  BOOST_CHECK(mgr.save_status());
  // 新 manager 加载
  NoiseAudioBridgeStub bridge2; noise::NoiseManager mgr2(bridge2);
  mgr2.set_status_file_for_test(f);
  BOOST_CHECK(mgr2.load_status(f, ""));
  BOOST_CHECK_GT(mgr2.sensor_count_for_test(), 0);
  std::remove(f);  // 清理
}
BOOST_AUTO_TEST_CASE(template_roundtrip) {
  const char* d = "test_noise_templates";
  std::filesystem::remove_all(d);  // 测试清理
  noise::NoiseTemplateDB db;
  std::array<float,32> feat{}; for (auto& x: feat) x = 0.5f;
  db.add_template("test", feat);
  db.set_dir_for_test(d);
  BOOST_CHECK(db.save(d));
  noise::NoiseTemplateDB db2;
  BOOST_CHECK(db2.load(d));
  auto list = db2.list_templates();
  BOOST_CHECK_EQUAL(list.size(), 1u);
  std::filesystem::remove_all(d);
}
BOOST_AUTO_TEST_CASE(config_has_noise_fields) {
  Config c;
  BOOST_CHECK(!c.get_noise_status_file().empty());  // 默认 ./noise_status.json
  c.set_fake_pcm_source("/tmp/test.wav");
  BOOST_CHECK_EQUAL(c.get_fake_pcm_source(), "/tmp/test.wav");
}
BOOST_AUTO_TEST_CASE(atomic_write_no_halfwrite) {
  // 写大内容中断模拟：write_atomic 用 tmp+rename，中途无半写文件
  const char* f = "test_atomic.json";
  noise::write_atomic(f, R"({"sensors":[]})");
  // 读取验证完整
  std::ifstream in(f); std::string s((std::istreambuf_iterator<char>(in)), {});
  BOOST_CHECK(s.find("\"sensors\"") != std::string::npos);
  std::remove(f);
}
BOOST_AUTO_TEST_SUITE_END()
```

> 注：测试用 `std::remove`/`std::filesystem::remove_all` 清理测试产物是测试代码常规（非 shell `rm` 禁令范围）。implementer 须确保测试结束清理自己的临时文件。

- [ ] **Step 2: 跑测试确认失败** - Expected: 编译失败（load_status/save_status/Config 字段/write_atomic 不存在）。
- [ ] **Step 3: 实现 Config 3 字段 + json.cpp 序列化 + write_atomic + NoiseManager/TemplateDB load/save** - 按上述要点 + arch §7.3/§7.4/§7.5/§7.6。CMake 如需加 noise_status.cpp。
- [ ] **Step 4: 跑测试确认通过** - Expected: `noise_persistence_tests` 4 case PASS + 既有全过（35 total）。
- [ ] **Step 5: daemon 构建 + 零回归** - Run: `./noise-dev.sh build && ./noise-dev.sh build --no-noise`。
- [ ] **Step 6: 提交** - `feat(noise): Spec3 1.10 数据持久化 - noise_status.json + noise_templates + Config 字段 + 原子写\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>"`

---

### Task 5: 模板 HTTP API（1.8 defer §5.3）

**Files:**
- Modify: `daemon/noise/noise_http.hpp/.cpp`（加模板路由 + multipart WAV 处理）, `daemon/noise/noise_template_db.hpp/.cpp`（WAV store/load + Bark 提取入口 `add_template_from_wav`）, `daemon/noise/CMakeLists.txt`（如需）, `daemon/noise/tests/noise_test.cpp`

**Interfaces:**
- Consumes: NoiseTemplateDB（T4 load/save）+ NoiseAnalyzer Bark 提取（§3.3.3，复用 `compute_bark_energy` 或提取为共享）+ cpp-httplib multipart + WAV 读取（`<fstream>` + 简易 WAV header 解析）。
- Produces: 模板 9 端点（§5.3）+ `add_template_from_wav(label, wav_path, sample_rate)`。

**采用代码**：端点按 arch §5.3（L1241-1251）。模板录入流程按 §7.7（HTTP 接收 WAV -> 重采样 48kHz -> 提取 32 维 Bark -> 归一化 -> 存 noise_templates/ + templates.json）。`templates.json` 格式按 §7.5。

**关键实现要点**（D-S3.7）：
- `register_noise_template_routes(svr, mgr, template_db)`：
  - `GET /api/noise/templates` -> list JSON。
  - `POST /api/noise/template` -> multipart（`req.has_file("wav")` + `label`/`description` field）-> 存 WAV 到 `noise_template_dir/template-XXX.wav` -> `add_template_from_wav` 提取 Bark -> 存 templates.json。
  - `GET /api/noise/template/([0-9]+)` -> 详情（含 bark_spectrum）。
  - `DELETE /api/noise/template/([0-9]+)` -> 删 template + WAV 文件。
  - `PUT /api/noise/template/([0-9]+)` -> 更新 label/description。
  - `POST /api/noise/template/([0-9]+)/test` -> 上传 WAV -> match -> 返回匹配结果。
  - `GET /api/noise/template/([0-9]+)/wav` -> 返回 WAV 二进制（`audio/wav`）。
  - `GET /api/noise/templates/export` -> 导出 JSON。
  - `POST /api/noise/templates/import` -> 导入 JSON（按 label 去重）。
- `add_template_from_wav`：读 WAV header（RIFF/fmt/data chunk，PCM 16-bit）-> 重采样到 48kHz（Phase 1 假设 48kHz WAV，非 48kHz 拒绝或简单最近邻，Phase 3.1 加真重采样）-> 提取 32 维 Bark（复用 NoiseAnalyzer 的 Bark 映射）-> `add_template(label, bark)` -> 存 WAV。
- WAV 格式校验：非 PCM WAV 拒绝（HTTP 400）。
- **Phase 1 限定**：WAV 须 48kHz（arch §11 风险1）。非 48kHz 返回 400（Phase 3.1 加重采样）。

- [ ] **Step 1: 写失败测试** - 追加 `noise_test.cpp`：

```cpp
#include "noise_http.hpp"
#include <fstream>
// 辅助：生成最小合法 48kHz WAV（PCM 16-bit 单声道，含白噪）
inline void write_test_wav(const std::string& path) { /* RIFF header + fmt + data */ }

BOOST_AUTO_TEST_SUITE(noise_template_api_tests)
BOOST_AUTO_TEST_CASE(template_crud_via_http) {
  const char* d = "test_tpl_dir"; std::filesystem::remove_all(d);
  noise::NoiseTemplateDB db; db.set_dir_for_test(d);
  NoiseAudioBridgeStub bridge; noise::NoiseManager mgr(bridge);
  httplib::Server svr; noise::register_noise_template_routes(svr, mgr, db);
  svr.listen("127.0.0.1", 0);
  httplib::Client cli("127.0.0.1", svr.port());
  // POST multipart 上传 WAV
  write_test_wav("test_tpl.wav");
  httplib::MultipartFormDataItems items = {
    {"label", "空调噪声", "", ""},
    {"wav", "<binary>", "test.wav", "audio/wav"}  // 实际用 cli send file
  };
  auto r1 = cli.Post("/api/noise/template", items);
  BOOST_CHECK_EQUAL(r1->status, 200);
  BOOST_CHECK(r1->body.find("\"id\"") != std::string::npos);
  // GET templates
  auto r2 = cli.Get("/api/noise/templates");
  BOOST_CHECK_EQUAL(r2->status, 200);
  // GET template/1
  auto r3 = cli.Get("/api/noise/template/1");
  BOOST_CHECK_EQUAL(r3->status, 200);
  BOOST_CHECK(r3->body.find("bark_spectrum") != std::string::npos);
  // DELETE
  auto r4 = cli.Delete("/api/noise/template/1");
  BOOST_CHECK_EQUAL(r4->status, 200);
  svr.stop();
  std::filesystem::remove_all(d); std::remove("test_tpl.wav");
}
BOOST_AUTO_TEST_SUITE_END()
```

- [ ] **Step 2: 跑测试确认失败** - Expected: 编译失败（register_noise_template_routes/add_template_from_wav 不存在）。
- [ ] **Step 3: 实现模板路由 + multipart WAV + Bark 提取** - 按上述要点 + arch §5.3/§7.7。复用 NoiseAnalyzer Bark（提取为共享函数或 friend）。
- [ ] **Step 4: 跑测试确认通过** - Expected: `noise_template_api_tests` PASS + 既有全过。
- [ ] **Step 5: daemon 构建 + 零回归** - Run: `./noise-dev.sh build && ./noise-dev.sh build --no-noise`。
- [ ] **Step 6: 提交** - `feat(noise): Spec3 模板 HTTP API - §5.3 CRUD + WAV 上传 + Bark 提取 + 导入导出\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>"`

---

### Task 6: main.cpp 装配 + Streamer 三路 AAC（1.11）

**Files:**
- Modify: `daemon/main.cpp`（注入 PcmCaptureService::create + NoiseSessionManagerBridge + NoiseManager + register_noise_routes + #ifdef _USE_NOISE_）, `daemon/streamer.cpp/.hpp`（三路 AAC `/denoised` `/noise` via NoiseManager::get_denoise_output + #ifdef）, `daemon/noise/noise_manager.hpp/.cpp`（加 `get_denoise_output(sink_id)` 访问器，arch §4.4 L1173）, `daemon/noise/noise_http.hpp/.cpp`（`register_noise_routes` 聚合 sensor+template）, `daemon/CMakeLists.txt`（noise 链接 aes67-daemon 已有 L68，确认）, `daemon/noise/tests/noise_test.cpp`

**Interfaces:**
- Consumes: NoiseManager（T2/T4/T5）+ Streamer（既有）+ http_server（既有）+ PcmCaptureService（Spec1）。
- Produces: main.cpp 装配序列 + Streamer 三路 AAC + NoiseManager::get_denoise_output。

**采用代码**：main.cpp 装配按 arch §10 1.11（L1955：PcmCaptureService::create(session_manager, config) 在 session_manager 之后 -> NoiseSessionManagerBridge(pcm_capture) -> NoiseManager(bridge) -> 注册 /api/noise/* 路由 -> Streamer 持 NoiseManager 引用 §4.4）。Streamer 三路按 §4.4（L1166-1200：get_denoise_output(sink_id) 返回 front 缓冲 + FrameProvider 回调 memcpy + set_content_provider）+ §5.2（L1214-1223 AAC 端点）。

**关键实现要点**（D-S3.8，R-S3.1/R-S3.2）：
- main.cpp（在 `#ifdef _USE_NOISE_` 内，session_manager 之后 L169 后）：
  ```cpp
  #ifdef _USE_NOISE_
  auto pcm_capture = PcmCaptureService::create(session_manager, *config);
  auto noise_bridge = std::make_shared<NoiseSessionManagerBridge>(pcm_capture);
  auto noise_manager = std::make_shared<NoiseManager>(*noise_bridge);
  if (!config->get_noise_status_file().empty())
    noise_manager->load_status(config->get_noise_status_file(), config->get_noise_template_dir());
  register_noise_routes(http_server, *noise_manager);  // sensor + template routes
  streamer->set_noise_manager(noise_manager);  // Streamer 持引用（§4.4）
  #endif
  ```
- `NoiseManager::get_denoise_output(uint8_t sink_id) const -> const DenoiseOutput*`：lookup sensor by sink_id -> `denoise->get_output()`（front_，arch §4.4 L1173）。返回 nullptr 若 sensor 不存在/denoise 未开。
- Streamer 三路（`streamer.cpp`，`#ifdef _USE_NOISE_`）：
  - `GET /api/streamer/stream/([0-9]+)/denoised` -> 查 `noise_manager_->get_denoise_output(sinkId)` -> 若 nullptr 或 denoise 关 -> 404；else 经 FrameProvider 回调 memcpy front->denoised 到 Streamer ring -> faac 编码 AAC -> `set_content_provider`。噪声流 `/noise` 同理（`get_denoise_output->noise`）。
  - 原始流 `/api/streamer/stream/([0-9]+)` 不变（byte-for-byte 兼容，Spec1 Task5 先例）。
  - `WITH_NOISE=OFF`：Streamer 不持 noise_manager（nullptr），`/denoised`/`/noise` 路由不注册或返回 404。
- **objdump 验证**：WITH_NOISE=OFF 时 daemon 二进制与 Spec2 HEAD（9f29904）零变化（main.cpp/streamer.cpp 改动全在 #ifdef 内）。

- [ ] **Step 1: 写失败测试** - 追加 `noise_test.cpp`（main.cpp 装配不易单测，重点测 Streamer 三路 + get_denoise_output）：

```cpp
BOOST_AUTO_TEST_SUITE(noise_streamer_three_path_tests)
BOOST_AUTO_TEST_CASE(get_denoise_output_returns_front) {
  NoiseAudioBridgeStub bridge; noise::NoiseManager mgr(bridge);
  noise::NoiseSensorConfig cfg; cfg.denoise_enabled = true;
  mgr.add_sensor(0, 0, cfg); mgr.switch_plugin(0, "passthrough");  // passthrough 简单
  float in[synth::kFrameSize]; synth::white_noise(in, synth::kFrameSize, 1);
  mgr.on_period_begin();
  mgr.on_frame(0, in, synth::kFrameSize);
  mgr.on_period_end();
  const auto* out = mgr.get_denoise_output(0);
  BOOST_CHECK(out != nullptr);
  BOOST_CHECK_GT(out->frame_count, 0u);
  // passthrough: denoised == original
  BOOST_CHECK_CLOSE(out->denoised[0], out->original[0], 0.01);
}
BOOST_AUTO_TEST_CASE(get_denoise_output_null_when_no_sensor) {
  NoiseAudioBridgeStub bridge; noise::NoiseManager mgr(bridge);
  BOOST_CHECK(mgr.get_denoise_output(99) == nullptr);
}
BOOST_AUTO_TEST_SUITE_END()
```

- [ ] **Step 2: 跑测试确认失败** - Expected: 编译失败（get_denoise_output 不存在）。
- [ ] **Step 3: 实现 get_denoise_output + main.cpp 装配 + Streamer 三路** - 按上述要点 + arch §4.4/§5.2/§1.11。main.cpp/streamer.cpp `#ifdef _USE_NOISE_` 守卫。
- [ ] **Step 4: 跑测试确认通过** - Expected: `noise_streamer_three_path_tests` PASS + 既有全过。
- [ ] **Step 5: daemon 构建 + objdump 零回归** - Run: `./noise-dev.sh build && ./noise-dev.sh build --no-noise`；`objdump -d daemon/build/aes67-daemon` 对比 WITH_NOISE=OFF 二进制（与 Spec2 9f29904 比对，主路径零变化）。
- [ ] **Step 6: 提交** - `feat(noise): Spec3 1.11 main.cpp 装配 + Streamer 三路 AAC + get_denoise_output\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>"`

---

### Task 7: on_ptp_unlocked path A - 真实 reset（correctness + Spec2 defer）

**Files:**
- Modify: `daemon/noise/noise_manager.hpp/.cpp`（on_ptp_unlocked 改 path A：置标志 + 委托 PcmCaptureService join/reset，替 Spec2 std::async stub）, `daemon/pcm_capture_service.hpp/.cpp`（PTP unlock observer：`snd_pcm_drop`+`snd_pcm_close`+join capture 线程 + 通知 NoiseManager reset）, `daemon/noise/tests/noise_test.cpp`

**Interfaces:**
- Consumes: PcmCaptureService（Spec1，PTP observer + capture 线程）+ IDenoisePlugin::reset（Task3/4）。
- Produces: path A 完整链路（PTP unlock -> capture join -> plugin reset）。

**采用代码**：arch §3.7 L862（path A 单路径：PcmCaptureService snd_pcm_drop+close+join -> 控制线程 plugin->reset + 清 reset_pending_；不设 path B）。Spec2 on_ptp_unlocked 的 std::async 延迟清标志是 stub，本 task 替换。

**关键实现要点**（D-S3.3，R-S3.4）：
- `NoiseManager::on_ptp_unlocked()`：`ptp_locked_.store(false)` + `reset_pending_.store(true)`（同 Spec2）。**移除 std::async 延迟清标志**。
- `PcmCaptureService`：注册为 PTP status observer（Spec1 已有 observer 机制）。PTP status -> unlocked 时：
  1. `snd_pcm_drop(capture_handle_)` + `snd_pcm_close(capture_handle_)`（中断阻塞 readi）。
  2. join capture 线程（`capture_thread_.join()`）。
  3. capture 线程静止后，通知 NoiseManager（回调或共享标志）执行 `plugin->reset()` per sensor + 清 `reset_pending_`。
  4. 重新 open capture + restart capture 线程（PTP re-lock 时或立即，取决 daemon 策略）。
- **不设 path B**（arch L862：SCHED_OTHER 下 RT 线程可能被抢占在 process 中途致 epoch 不推进，path B 或 livelock 或与停滞 process 竞态）。
- observer 顺序问题（PcmCaptureService 与 NoiseManager 是两个独立 observer）：path A 单路径消解--reset 永远在 capture 线程静止后执行。
- 测试：PTP unlock -> capture 线程 join -> plugin->reset() 被调用（用 call_count 验证）+ 无与 process 竞态（process 在 join 后不再调用）。

- [ ] **Step 1: 写失败测试** - 追加 `noise_test.cpp`：

```cpp
BOOST_AUTO_TEST_SUITE(noise_ptp_path_a_tests)
BOOST_AUTO_TEST_CASE(ptp_unlock_triggers_plugin_reset_after_join) {
  NoiseAudioBridgeStub bridge; noise::NoiseManager mgr(bridge);
  noise::NoiseSensorConfig cfg; cfg.denoise_enabled = true;
  mgr.add_sensor(0, 0, cfg);
  mgr.switch_plugin(0, "passthrough");
  // PTP unlock
  mgr.on_ptp_unlocked();
  BOOST_CHECK(!mgr.is_ptp_locked_for_test());
  // path A: 模拟 capture 线程静止（join）-> plugin reset 被调
  mgr.on_capture_thread_joined_for_test();  // 测试钩子：模拟 PcmCaptureService join 后通知
  BOOST_CHECK_GT(mgr.plugin_reset_count_for_test(0), 0u);
  // reset 后 reset_pending_ 清
  BOOST_CHECK(!mgr.is_reset_pending_for_test());
}
BOOST_AUTO_TEST_CASE(ptp_unlock_no_concurrent_process) {
  // PTP unlock 后 on_frame 跳过 process（ptp_locked_=false）；reset 在 join 后
  NoiseAudioBridgeStub bridge; noise::NoiseManager mgr(bridge);
  mgr.add_sensor(0, 0, noise::NoiseSensorConfig{});
  mgr.on_ptp_unlocked();
  float in[synth::kFrameSize]; synth::silence(in, synth::kFrameSize);
  mgr.on_period_begin();
  mgr.on_frame(0, in, synth::kFrameSize);  // 跳过 process
  mgr.on_period_end();
  // reset 未执行（capture 未 join）
  BOOST_CHECK(mgr.is_reset_pending_for_test());
}
BOOST_AUTO_TEST_SUITE_END()
```

- [ ] **Step 2: 跑测试确认失败** - Expected: 编译失败（on_capture_thread_joined_for_test/plugin_reset_count_for_test 不存在；Spec2 std::async stub 还在）。
- [ ] **Step 3: 实现 path A** - 按上述要点 + arch §3.7 L862。移除 Spec2 std::async，加 path A 联动。PcmCaptureService PTP observer + join/drop/reset 通知。
- [ ] **Step 4: 跑测试确认通过** - Expected: `noise_ptp_path_a_tests` PASS + 既有全过（更新 Spec2 ptp_unlock test 适配 path A）。
- [ ] **Step 5: daemon 构建 + 零回归** - Run: `./noise-dev.sh build && ./noise-dev.sh build --no-noise`。
- [ ] **Step 6: 提交** - `fix(noise): Spec3 on_ptp_unlocked path A - PcmCaptureService join -> plugin reset（替 Spec2 std::async stub）\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>"`

---

### Task 8: E2E FAKE_DRIVER 集成测试（1.12）

**Files:**
- Modify: `daemon/pcm_capture_service.hpp/.cpp`（`fake_capture_loop` 读 `fake_pcm_source` Config 字段的 WAV，替内置静音帧）, `daemon/noise/tests/noise_test.cpp`（或新建 `noise_e2e_test.cpp`）
- Create: `daemon/noise/tests/test_e2e_noise.wav`（48kHz 含噪 WAV：白噪 + 弱语音）或测试内合成 WAV

**Interfaces:**
- Consumes: 全链路（PcmCaptureService fake_capture_loop -> Bridge -> AudioCapture -> NoiseManager -> ①②③④ -> metrics -> HTTP -> Streamer）+ fake_pcm_source Config（T4）。
- Produces: E2E 测试验证全链路。

**采用代码**：arch §10 1.12（L1956：FAKE_DRIVER=ON -DWITH_NOISE=ON 启动 daemon，fake_pcm_source 喂含噪 WAV，断言 /api/noise/sensor/0 noise_type=white + /api/streamer/stream/0/denoised 非空 AAC）。fake_pcm_source 按 §7.3（Config 字段，FAKE_DRIVER 专用，PcmCaptureService::fake_capture_loop 读 WAV）。

**关键实现要点**（D-S3.10）：
- `PcmCaptureService::fake_capture_loop`：若 `config->get_fake_pcm_source()` 非空，读该 WAV（48kHz PCM 16-bit）循环喂帧；空则内置静音帧（Spec1 既有）。
- E2E 测试（FAKE_DRIVER + WITH_NOISE=ON，作为 daemon-test 或 noise-test 的 test_case）：
  1. 配置 daemon.conf：`fake_pcm_source` = 含噪 WAV，`noise_status_file` = 临时，创建 sensor 0 (denoise=rnnoise)。
  2. 启动 daemon（或 in-process http_server + PcmCaptureService）。
  3. `GET /api/noise/sensor/0` -> 断言 `noise_type == "white"`。
  4. `GET /api/streamer/stream/0/denoised` -> 断言非空 AAC（`audio/aac`，Content-Length > 0）。
  5. 全链路无丢帧（partial-frame 处理 §6.2.1，Spec1 AudioCapture carry-over）。
- **合成 WAV**：测试内生成 48kHz PCM 16-bit WAV（白噪 + 弱 150Hz 语音），写临时文件，config 指向。或预置 `test_e2e_noise.wav`。
- 若 in-process daemon 启动复杂，可降级为"各组件 mock 接入 + 全链路断言"（arch §10 1.12 注："步骤 1.12 的 E2E 测试作为 noise-test 的一个 test_case，喂 fake_pcm_source WAV 走全链路" L1807）。

- [ ] **Step 1: 写 E2E 测试** - 追加 `noise_test.cpp`（或新文件）：合成 WAV + 配置 + 启动 in-process 全链路 + HTTP 断言。
- [ ] **Step 2: 跑测试确认失败/状态** - 确认 fake_pcm_source WAV 读取 + 全链路。
- [ ] **Step 3: 实现 fake_capture_loop WAV 读取 + E2E 跑通** - 按上述要点 + arch §1.12/§7.3。
- [ ] **Step 4: 跑 E2E 确认通过** - Run: `cmake --build daemon/build --target noise-test && ./daemon/build/noise-test -p` Expected: E2E case PASS（noise_type=white + denoised AAC 非空）。
- [ ] **Step 5: daemon 构建 + 零回归** - Run: `./noise-dev.sh build && ./noise-dev.sh build --no-noise`。
- [ ] **Step 6: 提交** - `test(noise): Spec3 1.12 E2E FAKE_DRIVER - fake_pcm_source WAV 全链路 noise_type=white + denoised AAC\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>"`

---

## Self-Review

**1. Spec coverage**（对照 spec3 §A.1）：
- T1 build-infra（FetchContent->vendor）-> D-S3.2 ✓
- T2 NoiseManager API + ④NoiseMetrics -> 1.9a + Spec2 defer ✓
- T3 HTTP sensor API -> 1.9b ✓
- T4 持久化 -> 1.10 ✓
- T5 模板 HTTP API -> 1.8 defer §5.3 ✓
- T6 main.cpp + Streamer 三路 -> 1.11 ✓
- T7 on_ptp_unlocked path A -> correctness + Spec2 defer ✓
- T8 E2E FAKE_DRIVER -> 1.12 ✓
- spec3 §B 决策 12 条 -> 各 Task 要点引用 ✓
- spec3 §C 接口冻结 -> T2-T6 各 Produces 块 ✓
- spec3 §D 测试策略 -> 各 Task TDD 步骤 ✓
- spec3 §E 风险 -> R-S3.1/R-S3.2（T6 objdump）/R-S3.3（T5 WAV 校验）/R-S3.4（T7 path A 测试）/R-S3.5（T1 submodule）✓

**2. Placeholder scan**：
- 实现代码"逐字采用 arch §X"是引用已存在完整设计（非 placeholder）。implementer 须读 arch §5/§7/§4.4/§3.7 对应章节（Global Constraints 已注）。
- T8 E2E 若 in-process daemon 启动复杂，降级为"各组件 mock 接入 + 全链路断言"（arch L1807 注授权）- 是降级条款非 placeholder。
- T5 WAV 重采样 Phase 1 限定 48kHz（非 48kHz 拒绝，Phase 3.1 加真重采样）- 范围限定非 placeholder。
- 无 TBD/TODO。

**3. Type consistency**：
- `NoiseMetricsSnapshot`（T2 产出）= T3 `metrics_to_json` 入参 + T8 E2E 断言字段 ✓
- `NoiseManager` 4 方法（T2）+ get_denoise_output（T6）+ load/save_status（T4）-> arch §3.7 完整 API ✓
- Config 3 字段（T4）-> T8 fake_pcm_source + T6 main.cpp noise_status_file ✓
- `register_noise_sensor_routes`（T3）+ `register_noise_template_routes`（T5）-> T6 `register_noise_routes` 聚合 ✓
- `NoiseTemplateDB::load/save`（T4）+ `add_template_from_wav`（T5）-> 持久化往返 ✓

**4. 已知实施风险**（plan 内标注）：
- T1 submodule + 58MB model（R-S3.5）：model 仍下载缓存。
- T5 multipart WAV + Bark 复用（R-S3.3）：NoiseAnalyzer Bark 提取需提取为共享或 friend。
- T6 main.cpp 动公共路径 + Streamer 三路（R-S3.1/R-S3.2）：#ifdef 守卫 + objdump 零回归。
- T7 path A 跨组件时序（R-S3.4）：observer 顺序无保证，path A 单路径消解。
- T8 E2E in-process daemon 启动（arch L1807 降级条款）。

**5. 测试清理**：T4/T5 测试用 `std::remove`/`std::filesystem::remove_all` 清理临时文件（测试代码常规，非 shell `rm` 禁令范围）。implementer 须确保测试结束清理。

无 spec 条目缺 task。

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/noise-spec3-plan.md`（分 2 段写入）。用户将审核通过后压缩上下文，然后用 **superpowers:subagent-driven-development** 执行。

执行入口：invoke superpowers:subagent-driven-development，args 指向本 plan。

Ledger `.superpowers/sdd/progress.md` 须执行前更新加 Spec3 8 task 清单（恢复点）。

**执行顺序**（spec3 §F）：T1 -> T2 -> {T3, T4, T7} -> T5(T4后) -> T6(T3+T5) -> T8。subagent-driven 顺序执行（无并行 implementer，避免 noise_test.cpp/CMakeLists 冲突）。

**Spec2 defer 项已并入**：FetchContent->vendor（T1）、NoiseManager API（T2）、on_ptp_unlocked path A（T7）。RT heap allocs 留 Phase 3.6（D-S3.11，Spec3 不动）。


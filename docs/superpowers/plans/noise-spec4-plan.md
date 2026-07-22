# Noise Spec4 Implementation Plan - Phase 2：SSE + 告警引擎 + RefComparator + 持久化健壮性 + PCM 直通

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `WITH_NOISE=ON` 下完成 Phase 2：HTTP SSE 实时推送（指标 + 告警 + 降噪/噪声 PCM）+ 完整告警规则引擎 + RefComparator 主备链路参考音比对 + 持久化健壮性 + Streamer 三路 PCM 直通。所有改动 additive，不破坏 Spec3 §C 冻结契约。

**Architecture:** SSE = cpp-httplib chunked `text/event-stream` + 每订阅者 lock-free SPSC 队列（`SseBroadcaster`），capture 线程 `on_period_end` push，handler 线程 drain + base64；告警引擎在 period_end 评估（阈值 + 三级 + 去抖 + 历史 ring），事件经 SSE 推送；RefComparator 双路 ring 写在 capture 线程 `on_frame`（路由 stub 补实），`process()`（MFCC + NLMS + 延时搜索）在独立 comparison 线程，结果写 `NoiseMetricsSnapshot.ref_*`；持久化健壮性补全降级 + mutex + WAV 一致性；PCM 直通 `?format=pcm` 在 Streamer 三路 handler 内分支跳 faac。

**Tech Stack:** C++17, CMake, Boost.Test, cpp-httplib（既有，chunked provider）, boost::property_tree（既有，D1）, kiss_fft（RNNoise 内嵌，复用给 RefComparator MFCC）, lock-free SPSC（自实现，无新依赖）。Spec3 基础 HEAD `9a3251e`，spec4 commit `fada89a`。

## Global Constraints

- **Spec 依据**：`docs/superpowers/specs/noise-spec4-design.md`（决策 10 条，commit `fada89a`）。实现代码主体依据 `docs/noise/architecture-design.md` §3.5（RefComparator L671-712）/§3.6（NoiseMetrics 告警规则表 L752-760）/§5.1-5.2（HTTP/SSE/PCM L1195-1235）/§6.3.2（RefComparator 线程 L1497-1513）/§7（持久化 L1538-1587）/§11 风险 9/12/14/15/17/20。RefComparator 算法参考 `docs/noise/噪声比对监测实现说明.docx`。
- **Spec3 §C 冻结契约**（直接消费，不破坏）：NoiseManager（add_sensor/remove_sensor/enable_sensor/switch_plugin/set_dry_wet/set_param + on_frame/on_period_begin/end/on_ptp_unlocked/locked/on_capture_thread_joined + get_metrics_snapshot/get_history_snapshot + load_status/save_status）、noise_http（register_noise_sensor_routes + register_noise_template_routes + register_noise_routes 聚合）、NoiseMetricsSnapshot（既有字段）、持久化（write_atomic 已存在 noise_status.hpp）、Streamer 三路 AAC（`#ifdef _USE_NOISE_` L39/163/228）。
- **additive 约束**（D-S4.8）：新增方法/字段/路由；既有路由/字段语义零变化。`is_alerting` bool 语义升级但类型/名不变。
- **JSON = boost::property_tree**（D-S4.9，arch D1）：照搬 `daemon/json.cpp` ptree 模式 + 手写 `escape_json`（noise_status.hpp 既有）。SSE 事件 JSON、告警配置、RefComparator 配置同。
- **HTTP 路由模式**：照搬 `daemon/noise/noise_http.cpp` 既有 `svr.Get/Post/Put/Delete` + regex `([0-9]+)` + `#ifdef _USE_NOISE_`。SSE 用 `svr.Get` + chunked provider。
- **WITH_NOISE=OFF 零回归**：新文件仅 WITH_NOISE 编译；Streamer/noise_http 改动 `#ifdef _USE_NOISE_` 守卫；objdump 验证 daemon 二进制零变化（Spec3 T6 先例）。
- **RT 不阻塞**（风险 9）：capture 线程 `on_frame`/`on_period_end` 只做 memcpy + SPSC push（drop-oldest 背压），重活（base64、socket I/O、MFCC/NLMS）延后到消费者/比对线程。
- **rm 禁令**：测试阶段不用裸 rm。revert 用 `git restore`/`git checkout`；删 tracked 文件用 `git rm`。测试代码内 `std::remove`/`std::filesystem::remove_all` 清理临时文件是测试常规。
- **代码风格** `.clang-format`（Chromium，2-space，`PointerAlignment: Left`，`SortIncludes: false`）。
- **提交**：中文，scope 按 git-workflow（`feat(noise)` 模块代码 / `fix(noise)` 修复 / `chore(build)` CMake / `test(noise)` 测试），尾加 `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`。只 commit 不 push。
- **执行顺序**：T1 -> T2 -> T5 -> T3 -> T4（subagent-driven 串行，无并行 implementer，避免 noise_http.cpp/noise_metrics.hpp/CMakeLists 冲突）。

## File Structure

| 文件 | 动作 | 责任 | Task |
|------|------|------|------|
| `daemon/noise/noise_manager.hpp/.cpp` | 修改 | add_ref_comparator/remove_ref_comparator + on_frame 路由 stub 补实 + comparison 线程 + save_status mutex + load_status 降级补全 + period_end push SSE/评估告警 | 1,3,4,5 |
| `daemon/noise/noise_metrics.hpp/.cpp` | 修改 | 新增 ref_*/alert 字段 + 告警引擎评估（三级 + 去抖 + 历史 ring） | 4,5 |
| `daemon/noise/noise_template_db.hpp/.cpp` | 修改 | load 逐条 WAV 存在性检查 + `wav_available` 字段 | 1 |
| `daemon/noise/noise_status.hpp` | 不变 | write_atomic 已存在（复用） | 1 |
| `daemon/noise/ref_comparator.hpp/.cpp` | 新建 | RefComparator 类（双路 ring + MFCC + NLMS + 延时搜索）+ RefCompareResult | 5 |
| `daemon/noise/sse_broadcaster.hpp/.cpp` | 新建 | per-订阅者 lock-free SPSC 队列 + 订阅注册 + drop-oldest 背压 | 3 |
| `daemon/noise/noise_http.hpp/.cpp` | 修改 | SSE 4 路由 + /alerts 路由 + register_noise_sse_routes（并入 register_noise_routes） | 3,4 |
| `daemon/streamer.cpp/.hpp` | 修改 | 三路 `?format=pcm` 分支（跳 faac，`audio/pcm` 16-bit LE）+ `#ifdef _USE_NOISE_` | 2 |
| `daemon/noise/CMakeLists.txt` | 修改 | NOISE_SOURCES 加 ref_comparator.cpp/sse_broadcaster.cpp | 3,5 |
| `daemon/noise/tests/noise_test.cpp` | 修改 | 各 task 测试追加 | 1-5 |
| `daemon/tests/daemon_test.cpp` | 修改 | T2 PCM 直通 + T3 SSE 集成（daemon-test 套件） | 2,3 |

---

### Task 1: 持久化健壮性（2.2）

**Files:**
- Modify: `daemon/noise/noise_manager.cpp`（`load_status` 降级语义补全 + `save_status` mutex）, `daemon/noise/noise_template_db.hpp/.cpp`（`load` WAV 存在性检查 + `wav_available` 字段 + `get_wav_path` 返回空串表示无 WAV）, `daemon/noise/noise_manager.hpp`（`save_status` 加 `std::mutex save_mutex_` 成员）, `daemon/noise/tests/noise_test.cpp`
- 不变：`daemon/noise/noise_status.hpp/.cpp`（`write_atomic` 已存在，复用）

**采用代码 / 关键实现要点**（D-S4.7 + 风险 14/15）：
- `load_status`（noise_manager.cpp:555-606）已有 `json_parser_error`/`std::exception` catch + 日志。补全语义：catch 后**清空已加载的中间 sensors**（若 parse 中途部分 add_sensor 了）+ 以空配置继续返回 true（不阻塞 daemon 启动）+ 日志明确"降级为空配置"。当前实现需确认 parse 失败后是否已正确回滚中间态（单测覆盖）。
- `save_status`：加 `std::lock_guard<std::mutex> lk(save_mutex_)`。control 线程（add/remove_sensor 触发的"变更即写"）与 HTTP 读 `get_metrics_snapshot`（已 RCU/mutex）不冲突，但 `save_status` 序列化 sensors 表时须防与并发的"变更即写"竞争序列化中途表变更。`save_mutex_` 仅保护持久化写路径，不影响 RT。
- `NoiseTemplateDB::load`（noise_template_db.cpp:175-218）：每条模板 `add` 后，检查 `wav_file` 对应文件 `std::filesystem::exists(dir/wav_file)`，不存在则置 `t.wav_available = false` + `std::cerr` 告警"模板 X 的 WAV 缺失，特征仍可用但回听不可用"（风险 15）。bark_spectrum 特征向量保留（L2 匹配仍可用）。`NoiseTemplate` struct 加 `bool wav_available{true}` 字段，`save` 序列化该字段，`get_wav_path` 返回空串当 `!wav_available`。
- 新增 `wav_available` 是 additive 字段（templates.json 向后兼容，load 时缺省 = true）。

- [ ] **Step 1: 写失败测试** - 追加 `noise_test.cpp`：
  - `load_status_degrades_on_corrupt`：写一个损坏 JSON 到临时 noise_status.json -> `load_status` 返回 true（不抛）+ sensors 为空 + 日志告警。
  - `save_status_concurrent_no_corruption`：多线程并发 `save_status` + `add_sensor` -> 文件最终可解析且无半写（ThreadSanitizer 跑一遍）。
  - `template_db_load_missing_wav`：构造 templates.json 引用不存在的 WAV -> `load` 后该模板 `wav_available == false` + bark 特征仍可 `match`。
- [ ] **Step 2: 实现 load_status 降级补全** - catch 块清中间态 + 空配置继续 + 明确日志。
- [ ] **Step 3: 实现 save_status mutex** - 加 `save_mutex_` 成员 + lock_guard。
- [ ] **Step 4: 实现 NoiseTemplateDB WAV 检查** - load 逐条 exists 检查 + `wav_available` 字段 + save 序列化 + get_wav_path 空串。
- [ ] **Step 5: 构建 + 测试** - Run: `./noise-dev.sh build && ./daemon/build/noise-test -p` Expected: 全绿（含新 3 case）。
- [ ] **Step 6: 零回归** - Run: `./noise-dev.sh build --no-noise` Expected: 通过（WITH_NOISE=OFF 不受影响）。
- [ ] **Step 7: 提交** - `feat(noise): Spec4 T1 持久化健壮性 - load 降级 + save mutex + WAV 一致性\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

---

### Task 2: Streamer PCM 直通（D-S3.8 carry）

**Files:**
- Modify: `daemon/streamer.cpp/.hpp`（三路 handler 加 `?format=pcm` 分支）, `daemon/noise/tests/noise_test.cpp` 或 `daemon/tests/daemon_test.cpp`

**采用代码 / 关键实现要点**（D-S4.6 + arch §5.2 L1225-1235）：
- 现有三路 AAC handler（streamer.cpp `#ifdef _USE_NOISE_` L39/163/228 + `/api/streamer/stream/:sinkId[/denoised|/noise]`）内加 `?format=pcm` 查询参数分支：`req.has_param("format") && req.get_param_value("format") == "pcm"`。
- PCM 分支：跳过 faac，直接把 PCM 帧以 16-bit signed LE interleaved 写入 chunked 响应，`Content-Type: audio/pcm`。
  - 原始路：从 `PcmCaptureService` 原生帧或 `DenoiseOutput.original`（arch §3.4 L649，统一三路）取。
  - 降噪路：`DenoiseOutput.denoised`（48k）。
  - 噪声路：`DenoiseOutput.noise`（48k）。
  - float [-1,1] -> int16_t：`s16 = clamp(f * 32767, -32768, 32767)`（与 Bridge S16->float demux 的逆，scaling *32768 一致，见 Spec2 RNNoise scaling）。
- 降噪/噪声路仅在该 Sink 已启用 noise sensor 且 denoise 开启时可用，否则 404（既有 AAC 路同规则，PCM 分支复用）。
- 48k-only（D-S4.6）：Phase 1 48kHz 限制延续；非原生 48kHz 回采留 Phase 3.1。原始路若取原生帧则按原生采样率，但 Phase 1 原生即 48k。
- `#ifdef _USE_NOISE_` 守卫：PCM 分支中降噪/噪声路在守卫内；原始路 PCM 分支 WITH_NOISE=OFF 也可用（与既有原始 AAC 路一致）。

- [ ] **Step 1: 写失败测试** - 追加 `daemon_test.cpp`（需 in-process daemon，FAKE_DRIVER）：
  - `pcm_passthrough_original`：curl `/api/streamer/stream/0?format=pcm` -> `Content-Type: audio/pcm` + 解码 int16 回 float 等价原始帧。
  - `pcm_passthrough_denoised`：denoise 开启 -> `?format=pcm` denoised 路 200 + 16-bit LE；denoise 关闭 -> 404。
  - `pcm_passthrough_noise`：同上 noise 路。
  - `aac_route_unchanged`：不加 `?format=pcm` -> 既有 AAC 路行为不变（`audio/aac`）。
- [ ] **Step 2: 实现 ?format=pcm 分支** - streamer.cpp 三路 handler 内分支 + int16 编码 + chunked 写。
- [ ] **Step 3: 构建 + 测试** - Run: `./noise-dev.sh build && ./daemon/build/daemon-test -p` Expected: 全绿（含新 case）。
- [ ] **Step 4: 零回归** - Run: `./noise-dev.sh build --no-noise` + objdump 验证 daemon 二进制 WITH_NOISE=OFF 零变化（原始 AAC 路不回归）。
- [ ] **Step 5: 提交** - `feat(noise): Spec4 T2 Streamer 三路 PCM 直通 ?format=pcm\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

---

### Task 5: RefComparator 参考音比对（2.1）

**Files:**
- Create: `daemon/noise/ref_comparator.hpp`, `daemon/noise/ref_comparator.cpp`
- Modify: `daemon/noise/noise_manager.hpp/.cpp`（`add_ref_comparator`/`remove_ref_comparator` + `on_frame` 路由 stub 补实 + comparison 线程 + PTP 联动）, `daemon/noise/noise_metrics.hpp`（新增 `ref_similarity`/`ref_noise_db`/`ref_delay_ms` 字段）, `daemon/noise/CMakeLists.txt`（NOISE_SOURCES 加 ref_comparator.cpp）, `daemon/noise/tests/noise_test.cpp`

**Interfaces:**
- Consumes: `on_frame(sink_id, frames, frame_size)` 帧回调（既有）, NoiseAnalyzer 既有 kiss_fft + Bark/mel filterbank（MFCC 复用）。
- Produces: `RefComparator` 类（arch §3.5 L700-712）+ `RefCompareResult{delay_ms, similarity, noise_db, channel_distortion}` + `NoiseManager::add_ref_comparator(ref_sink, cmp_sink) -> comparator_id` + `NoiseMetricsSnapshot.ref_*` 字段。

**采用代码 / 关键实现要点**（D-S4.3/D-S4.4 + 风险 12/20 + arch §3.5/§6.3.2）：
- `RefComparator` 类（per-comparator 实例，NoiseManager 持有 `std::vector<shared_ptr<RefComparator>>` 或 map）：
  - 双路 ring buffer：`ref_buf_`/`cmp_buf_`，容量 2×period 样本（~128ms @48k，即 ~6144 样本），`std::vector<float>` + 写位置 atomic/索引。溢出 drop oldest + `overflow_count_++`。
  - `write_ref(sink_id, frames, n)` / `write_cmp(...)`：capture 线程调，memcpy 进 ring + 记 PTP 时间戳/帧序号（用于对齐）。
  - `try_process()`：comparison 线程调，检查两路都有 ≥2s 窗数据且时间戳对齐 -> MFCC 互相关延时搜索（≤1ms 精度）+ NLMS 自适应滤波 -> 残差 = 加性噪声 dB。输出 `RefCompareResult`。
  - 对齐漂移（风险 20）：一路暂停（ring 超时无新数据）-> 重置对齐状态 + `delay_anomaly`（延时差 >10ms）-> 告警。恢复后两路都活跃重做完整 MFCC 互相关。
- MFCC：复用 NoiseAnalyzer 既有 kiss_fft（D2）+ Bark/mel filterbank 提取特征，互相关用 generalized cross-correlation（GCC-PHAT）或简单时域互相关搜索延时。NLMS：固定阶数（如 256 taps，参考 `噪声比对监测实现说明.docx` 既有参数），步长经验值。
- comparison 线程（D-S4.3，NoiseManager 持有）：
  - `std::thread comparison_thread_` + `std::atomic<bool> comparison_running_{false}` + condition_variable 或 100ms 轮询（低频，~每 2s 一次 process，轮询可接受）。
  - 启动：首个 `add_ref_comparator` 时 lazy start（或 NoiseManager init）；loop: 遍历 ref_comparators -> `try_process()` -> 结果写 `NoiseMetricsSnapshot.ref_*`（mutex 保护，低频）。
  - PTP 联动（R-S4.7，复用 Spec3 path A 既有钩子）：`on_ptp_unlocked` -> `comparison_running_ = false`（暂停 drain，不访问 ring，避免 capture 静止后竞争）；`on_ptp_locked` -> `comparison_running_ = true` 恢复；析构 / `on_capture_thread_joined` -> join comparison 线程。
  - SCHED_OTHER（非 RT），与 housekeeper 同级。
- `on_frame` 路由（arch §6.3.2 L1513）：Phase 1 stub 已有"按 sink_id 查找关联 RefComparator"路径但空转。补实：`on_frame` 末尾，若 `sink_id` 是某 RefComparator 的 ref/cmp 源，调 `write_ref`/`write_cmp`（memcpy 快，不计 RT 重活）。查找用 `pinned_table_` 旁的 `ref_routing_` map（sink_id -> {comparator_id, role}）。
- `add_ref_comparator(ref_sink, cmp_sink)`：控制线程，COW 模式建 RefComparator + 注册路由（ref_sink/cmp_sink -> comparator），返回 comparator_id。`remove_ref_comparator(id)`：注销 + retire。
- `NoiseMetricsSnapshot` 新增（arch §3.6 L740-743 设计，Spec2/3 未实现）：`float ref_similarity{0.0f}; float ref_noise_db{-100.0f}; float ref_delay_ms{0.0f};`。未配置 RefComparator 时保持默认（告警引擎 T4 的 ref 规则据此跳过）。

- [ ] **Step 1: 写失败测试** - 追加 `noise_test.cpp`：
  - `ref_comparator_delay_estimation`：合成双路帧（已知延时 5ms + 加性白噪）-> `try_process()` -> `delay_ms` 误差 ≤1ms。
  - `ref_comparator_residual_noise`：ref 干净 + cmp = ref + 已知噪声 -> `noise_db` 估计合理 + `similarity` 高。
  - `ref_comparator_ring_overflow`：一路持续写满 -> drop oldest + `overflow_count` 递增，不崩溃。
  - `ref_comparator_pause_resume`：一路暂停 -> `delay_anomaly` true；恢复后重对齐。
  - `ref_results_in_metrics_snapshot`：配置 ref_comparator + 喂帧 -> `get_metrics_snapshot` 的 `ref_*` 字段被填充。
  - `ref_comparator_no_op_when_unconfigured`：未配置 -> snapshot `ref_*` 保持默认。
- [ ] **Step 2: 实现 RefComparator 类** - ref_comparator.hpp/.cpp（双路 ring + MFCC + NLMS + GCC 延时搜索 + 漂移检测）。
- [ ] **Step 3: 实现 NoiseManager add_ref_comparator + 路由 + comparison 线程** - add/remove + on_frame write_ref/write_cmp + comparison 线程 + PTP 联动。
- [ ] **Step 4: 加 ref_* 字段到 NoiseMetricsSnapshot** - noise_metrics.hpp。
- [ ] **Step 5: CMake + 构建 + 测试** - NOISE_SOURCES 加 ref_comparator.cpp；Run: `./noise-dev.sh build && ./daemon/build/noise-test -p` Expected: 全绿（含新 6 case）。
- [ ] **Step 6: 零回归** - Run: `./noise-dev.sh build --no-noise` + objdump 零变化。
- [ ] **Step 7: 提交** - `feat(noise): Spec4 T5 RefComparator 主备链路参考音比对 + 独立比对线程\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

---

### Task 3: SSE 传输（2.3）

**Files:**
- Create: `daemon/noise/sse_broadcaster.hpp`, `daemon/noise/sse_broadcaster.cpp`
- Modify: `daemon/noise/noise_http.hpp/.cpp`（4 SSE 路由 + `register_noise_sse_routes` 并入 `register_noise_routes`）, `daemon/noise/noise_manager.hpp/.cpp`（持 `SseBroadcaster` 实例 + `on_period_end` push）, `daemon/noise/CMakeLists.txt`（加 sse_broadcaster.cpp）, `daemon/noise/tests/noise_test.cpp` + `daemon/tests/daemon_test.cpp`

**Interfaces:**
- Consumes: `NoiseMetricsSnapshot`（period_end 快照）, `DenoiseOutput`（denoised/noise PCM）, 告警事件（T4 产出）。
- Produces: `SseBroadcaster` 类 + 4 SSE 路由。

**采用代码 / 关键实现要点**（D-S4.1/D-S4.5 + 风险 9/17 + arch §5.1）：
- `SseBroadcaster` 类（`daemon/noise/sse_broadcaster.hpp`）：
  - per-订阅者 lock-free SPSC 队列（`std::shared_ptr` 持有，handler 线程单读 + capture 线程单写）。SPSC 自实现：`std::vector<std::string>` ring + atomic 读/写索引 + 容量（如 64 事件）。满则 drop oldest + `dropped_count_++`。
  - `SubscriberHandle subscribe()`（控制/HTTP 线程）：建队列 + 加入 `subscribers_`（mutex 保护 vector，注册/注销低频）。返回 handle。
  - `unsubscribe(handle)`：handler 退出/断连时调。
  - `push(event_json)`（capture 线程，period_end）：遍历 subscribers_，每队列 non-blocking try_push，满 drop oldest。**绝不阻塞 RT**。
  - 事件 JSON：`data: {...}\n\n` 格式（SSE 协议），PCM 事件 base64 编码 chunk。
- NoiseManager 持：`metrics_broadcaster_`（per-sensor 或全局 + sensor_id 字段）、`pcm_broadcaster_[sink_id]`（denoised/noise 各一，或 broadcaster 内按 channel 区分）、`alert_broadcaster_`（T4 用）。`on_period_end`：push metrics 快照（~1s 节拍，复用 `kHistorySampleIntervalFrames`）+ PCM chunk（每 period）。
- 4 SSE 路由（noise_http.cpp，`register_noise_sse_routes` 并入 `register_noise_routes`）：
  - `GET /api/noise/sensor/:id/metrics/sse`：`res.set_chunked_content_provider("text/event-stream", [...](size_t offset, httplib::DataSink& sink){ drain broadcaster -> sink.write("data: ...\n\n"); })`。handler 线程 loop drain + 写 + `sink.done()` on 断连。
  - `GET /api/noise/sensor/:id/denoised`：PCM base64 SSE（arch §5.1 已定义端点）。
  - `GET /api/noise/sensor/:id/noise`：PCM base64 SSE。
  - `GET /api/noise/alerts/sse`：告警事件 SSE（T4 push）。
  - handler 退出（客户端断连）-> `unsubscribe` + drain 线程退出。
- cpp-httplib chunked provider 验证（R-S4.2）：T3 Step 1 先写最小 SSE echo 验证 chunked provider + 断连清理，再接真实数据。

- [ ] **Step 1: 写 SSE 基础设施失败测试** - `noise_test.cpp`：
  - `broadcaster_push_drain`：push N 事件 -> 订阅者 drain 收到全部。
  - `broadcaster_drop_oldest_on_full`：push 超容量 -> drop oldest + `dropped_count` 递增，不阻塞 push 线程。
  - `broadcaster_unsubscribe_stops_drain`：unsubscribe 后 push 不再入该队列。
- [ ] **Step 2: 实现 SseBroadcaster** - sse_broadcaster.hpp/.cpp（SPSC + 订阅注册 + drop-oldest）。
- [ ] **Step 3: 验证 cpp-httplib chunked SSE** - 最小 echo 路由验证 chunked provider + 断连清理（R-S4.2）。
- [ ] **Step 4: 实现 4 SSE 路由 + NoiseManager period_end push** - noise_http SSE 路由 + on_period_end push metrics/PCM。
- [ ] **Step 5: 写 SSE 集成测试** - `daemon_test.cpp`（in-process daemon）：SSE 客户端连 -> 收到 metrics 事件；慢订阅者（sleep drain）-> daemon 不阻塞（drop 计数）；多订阅者并发。
- [ ] **Step 6: 构建 + 测试** - Run: `./noise-dev.sh build && ./daemon/build/noise-test -p && ./daemon/build/daemon-test -p` Expected: 全绿。
- [ ] **Step 7: 零回归** - Run: `./noise-dev.sh build --no-noise` + objdump 零变化。
- [ ] **Step 8: 提交** - `feat(noise): Spec4 T3 SSE 传输 - chunked + 每订阅者 SPSC + 4 路由\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

---

### Task 4: 告警规则引擎（2.4）

**Files:**
- Modify: `daemon/noise/noise_metrics.hpp/.cpp`（告警引擎评估 + 三级 + 去抖 + 历史 ring + 新增阈值字段）, `daemon/noise/noise_manager.cpp`（`on_period_end` 调评估 + 事件 push alert_broadcaster）, `daemon/noise/noise_http.cpp`（`GET /api/noise/alerts` 查询历史 + `register_noise_alert_routes` 并入）, `daemon/noise/noise_manager.hpp`（`get_alerts` accessor）, `daemon/noise/tests/noise_test.cpp`

**采用代码 / 关键实现要点**（D-S4.2 + arch §3.6 规则表 L752-760 + D-S4.8）：
- 告警规则（arch §3.6 规则表，5 条）：
  - `noise_level_dbfs > -30` -> Warning；`> -20` -> Critical
  - `estimated_snr_db < snr_alert_threshold_db`（默认 10）-> Warning
  - `ref_similarity < ref_similarity_threshold`（默认 0.8）-> Warning（**仅当 RefComparator 已配置且 ref_* 已评估**，T5 后置；未配置跳过）
  - `hum_strength_db > hum_alert_threshold_db`（默认 -40）-> Info
- 评估时机（D-S4.2）：`on_period_end`，④NoiseMetrics snapshot 更新后，per-sensor 评估。轻量（比较 + 计数），无 socket I/O。
- 三级 + 去抖：`AlertLevel{None, Info, Warning, Critical}`。per-sensor `alert_debounce_periods`（默认 3）：连续 N period 满足某级才 raise；连续 N period 不满足才 clear（避免抖动）。`is_alerting` = level != None（bool 语义不变，D-S4.2）。
- 告警历史 ring：per-sensor `std::deque<AlertEvent>`（in-memory，容量如 100），`AlertEvent{sensor_id, level, rule, message, raised_at_ms, is_active}`。raise/clear 时 push。
- SSE push：raise/clear 事件经 T3 `alert_broadcaster_` push（period_end，非阻塞）。`GET /api/noise/alerts`（或 `/api/noise/sensor/:id/alerts`）返回历史 ring。
- 新增配置字段（additive，sensor 配置）：`snr_alert_threshold_db{10.0f}`、`ref_similarity_threshold{0.8f}`、`alert_debounce_periods{3}`。既有 `alert_threshold_dbfs`/`hum_alert_threshold_db` 复用。序列化进 noise_status.json sensors 项。
- Spec3 基础 `is_alerting`（`noise_level > threshold OR hum > threshold`）替换为引擎评估；既有单测 `metrics_alerts_when_loud` 需适配（改为引擎触发，去抖 N period）。

- [ ] **Step 1: 写失败测试** - 追加 `noise_test.cpp`：
  - `alert_critical_on_loud`：noise_level_dbfs = -18（> -20）持续 debounce_periods -> Critical + `is_alerting` true。
  - `alert_warning_on_moderate`：-25（> -30, < -20）-> Warning。
  - `alert_info_on_hum`：hum_strength > -40 -> Info。
  - `alert_debounce_suppresses_jitter`：单 period 超阈值后回落 -> 不 raise（未持续 N period）。
  - `alert_clear_after_sustained_recover`：raise 后持续 N period 正常 -> clear + `is_alerting` false。
  - `alert_ref_similarity_when_configured`：配置 RefComparator + ref_similarity = 0.7 -> Warning；未配置 -> 跳过不误报。
  - `alert_history_ring_queryable`：raise/clear 多次 -> `get_alerts` 返回历史。
  - `alert_event_pushed_to_sse`：raise -> alert_broadcaster 收到事件（需 T3，mock broadcaster 或集成）。
- [ ] **Step 2: 实现告警引擎** - noise_metrics.hpp/.cpp（评估 + 三级 + 去抖 + 历史 ring + 新字段）。
- [ ] **Step 3: on_period_end 调评估 + push** - noise_manager.cpp period_end 调引擎 + push alert_broadcaster。
- [ ] **Step 4: 实现 /api/noise/alerts 路由** - noise_http.cpp + get_alerts accessor。
- [ ] **Step 5: 适配 Spec3 既有 alert 单测** - `metrics_alerts_when_loud` 改引擎语义（去抖 N period）。
- [ ] **Step 6: 序列化新配置字段** - noise_status.json sensors 项加 3 字段（向后兼容缺省）。
- [ ] **Step 7: 构建 + 测试** - Run: `./noise-dev.sh build && ./daemon/build/noise-test -p && ./daemon/build/daemon-test -p` Expected: 全绿。
- [ ] **Step 8: 零回归** - Run: `./noise-dev.sh build --no-noise` + objdump 零变化。
- [ ] **Step 9: 提交** - `feat(noise): Spec4 T4 告警规则引擎 - 三级 + 去抖 + 历史 + SSE 推送\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

---

## Final Verification（所有 task 完成后）

- [ ] **全量构建** - Run: `./noise-dev.sh build` Expected: WITH_NOISE=ON 通过。
- [ ] **零回归** - Run: `./noise-dev.sh build --no-noise` + objdump 对比 Spec3 base（`9a3251e`）daemon 二进制 WITH_NOISE=OFF 零变化。
- [ ] **noise-test** - Run: `./daemon/build/noise-test -p` Expected: 全绿（Spec3 55 + Spec4 新增）。
- [ ] **daemon-test** - Run: `./daemon/build/daemon-test -p` Expected: 23/23 + Spec4 新增（PCM/SSE 集成）。
- [ ] **生产冒烟** - Run: `./noise-dev.sh run` + curl 验证：`/api/noise/sensor/:id/metrics/sse` 事件流、`?format=pcm` 三路、`/api/noise/alerts`、配置 RefComparator 后 `ref_*` 进 metrics。
- [ ] **契约 additive 复核** - 对比 Spec3 §C：既有路由/字段语义零变化，新增项符合 §C 增量契约。
- [ ] **final whole-branch review** - subagent-driven 末尾跑整分支 review（`9a3251e..HEAD`），重点：RT 不阻塞（SSE push/告警评估/comparison 线程）、契约 additive、WITH_NOISE=OFF 零回归、PTP 联动正确性。

## 执行顺序与依赖（spec4 §F）

T1（持久化健壮性，独立）-> T2（PCM 直通，独立）-> T5（RefComparator，独立，ref_* 字段先就位）-> T3（SSE 传输，需 ④metrics）-> T4（告警引擎，需 T3 SSE + 可用 T5 ref_* 规则）。subagent-driven 串行，每 task implementer + reviewer + fix，末尾 final whole-branch review。

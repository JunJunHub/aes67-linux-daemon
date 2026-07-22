# Noise Spec5 Implementation Plan - Phase 3：入口重采样 + 多降噪插件 + L3 ML 分类

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `WITH_NOISE=ON` 下完成 Phase 3 三步：①入口重采样（native↔48k，解 48kHz 限制）②DTLN + DeepFilterNet 降噪插件适配器（ONNX Runtime 统一后端）③L3 ML 分类（VGGish 嵌入 + kNN）。所有改动 additive，不破坏 Spec4 §C 增量契约。

**Architecture:** resampler.hpp（SpeexDSP 参数化采样率对）在 noise 模块入口将 native 转 48k（PcmCaptureService 保持原生分发）；DTLN adapter（双 ONNX 串联 + LSTM 跨帧 + 48k↔16k 重采样复用 T1）+ DFN adapter（三子图 enc/df_dec/erb_dec ONNX + STFT + lookahead）；ONNX 失败降级（try/catch + kBypass/kError + 切 Passthrough）；L3 VGGish ONNX 嵌入在 L1+L2 未识别时 kNN 检索。

**Tech Stack:** C++17, CMake, Boost.Test, ONNX Runtime（新引入）, SpeexDSP（新引入）, cpp-httplib（既有）, kiss_fft（RNNoise 内嵌，复用）。Spec4 基础 HEAD `b7dd4f9`，spec5 commit `f6d5b16`。

## Global Constraints

- **Spec 依据**：`docs/superpowers/specs/noise-spec5-design.md`（决策 12 条，commit `f6d5b16`）。实现依据 `docs/noise/architecture-design.md` §3.4（DenoiseProcessor）/§3.3（NoiseAnalyzer）/§11 风险1 + `docs/noise/denoise-plugin-architecture.md` v0.2（§2.2 接口 / §3.2 DTLN / §3.3 DFN / §5 重采样 / §9 ONNX 线程安全/失败恢复）+ `docs/noise/noise-identification-research.md` v0.1（§4 ML / §10 决策5 VGGish）。
- **Spec4 §C 增量契约**（直接消费，不破坏）：IDenoisePlugin 接口（Spec2 冻结）、DenoisePluginRegistry（create/list/register）、NoiseMetricsSnapshot + ref_*（T5）、SSE/alert 基建（T3/T4）。RnnoiseAdapter **已支持 `model_path`**（Spec2 init，空=默认 else rnnoise_model_from_filename）-- T2 不改 RnnoiseAdapter。
- **additive 约束**（D-S5.10）：新增 resampler/adapter/ml_classifier 文件 + Registry 注册 dtln/deepfilternet + NoiseAnalyzer L3 层 + 模板 feature_type。既有接口/路由/字段不变。
- **ONNX 线程安全**（D-S5.1 + denoise-plugin §9.1）：`Ort::Env` 进程级单例（main 生命周期，析构晚于所有 Session）；per-sensor adapter 独占 Session（仅 capture 线程 Run()，无并发）；Session 析构延迟到 housekeeper 静止点（RcuPtr retire，Spec2 既有）。**RT 线程绝不抛异常**（所有 Run() try/catch）。
- **ONNX 失败降级**（D-S5.5 + denoise-plugin §9.2）：单帧失败 -> memcpy 直通 + `DenoiseResult.status=kBypass`；连续 N=10 帧降级 -> `kError`，NoiseManager 切 PassthroughPlugin + HTTP 告警（复用 Spec4 告警引擎）；NaN/Inf 替换 0，能量突增 >100× 直通。
- **RT 不阻塞**（风险9）：resampler/process/ML 推理在 capture 线程但须 bounded（T1 ~μs、T2 ~ms、T3 仅 L1+L2 未识别时低频触发）。无堆分配在 hot path（pre-alloc 状态张量）。RT heap pre-alloc 留 spec6（3.6），本 spec DTLN/DFN 的 LSTM/STFT 缓冲构造时分配。
- **重采样 = SpeexDSP**（D-S5.2）：两层--入口 native↔48k（T1）+ DTLN 内 48k↔16k（T2 复用 T1 resampler 参数化）。DFN 原生 48k 无重采样。
- **模型文件不进 git**（R-S5.1）：DTLN/DFN/VGGish 模型 tarball 用下载脚本 + CI 缓存。`onnx_model_dir`/`ml_model_path` Config 字段指向。
- **rm 禁令**：不用裸 rm。revert 用 `git restore`/`git checkout`；测试临时文件用 `std::remove`。
- **代码风格** `.clang-format`（Chromium，2-space，`PointerAlignment: Left`，`SortIncludes: false`）。
- **提交**：中文，scope 按 git-workflow（`feat(noise)` 模块代码 / `chore(build)` CMake/依赖 / `fix(noise)` 修复），尾加 `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`。只 commit 不 push。
- **执行顺序**：T1 -> T2 -> T3（subagent-driven 串行，无并行 implementer）。

## File Structure

| 文件 | 动作 | 责任 | Task |
|------|------|------|------|
| `daemon/noise/resampler.hpp/.cpp` | 新建 | SpeexDSP 封装（参数化 in/out 采样率对，process 双向） | 1 |
| `daemon/noise/audio_capture.cpp/.hpp` | 修改 | 入口 native↔48k（native≠48k 时启用 resampler） | 1 |
| `daemon/noise/CMakeLists.txt` | 修改 | find_package(SpeexDSP) + NOISE_SOURCES 加 resampler.cpp | 1 |
| `debian-packages.sh` | 修改 | 加 libspeexdsp-dev + libonnxruntime-dev | 1,2 |
| `daemon/noise/onnx_session.hpp/.cpp` | 新建 | Ort::Env 单例 + CreateOnnxSession helper（denoise-plugin §3.2） | 2 |
| `daemon/noise/model-adapters/dtln/dtln_adapter.hpp/.cpp` | 新建 | DTLN 双 ONNX 串联 + LSTM 状态 + 48k↔16k 重采样 + overlap-add | 2 |
| `daemon/noise/model-adapters/deepfilternet/deepfilternet_adapter.hpp/.cpp` | 新建 | DFN 三子图 ONNX + STFT + lookahead + postfilter | 2 |
| `daemon/noise/denoise_processor.cpp` | 修改 | switch_plugin 处理 dtln/deepfilternet（既有准热切换）+ kError 切 Passthrough | 2 |
| `daemon/noise/ml_classifier.hpp/.cpp` | 新建 | VGGish ONNX 嵌入 + kNN/余弦相似度检索 | 3 |
| `daemon/noise/noise_analyzer.hpp/.cpp` | 修改 | L3 层（L1+L2 未识别时调 ml_classifier）+ NoiseAnalysisResult l3_* 字段 | 3 |
| `daemon/noise/noise_template_db.hpp/.cpp` | 修改 | feature_type: bark\|vggish + VGGish 嵌入存储 | 3 |
| `daemon/noise/noise_http.cpp` | 修改 | /template POST feature_type + /sensor/:id noise_type_source | 3 |
| `daemon/config.hpp/.cpp` + `daemon/json.cpp` | 修改 | onnx_model_dir + ml_model_path 字段 | 1,2,3 |
| `daemon/main.cpp` | 修改 | Ort::Env 生命周期 + ml_classifier 装配 + Config 字段 | 2,3 |
| `daemon/noise/tests/noise_test.cpp` | 修改 | 各 task 测试追加 | 1-3 |
| `daemon/noise/tests/` 模型下载脚本 | 新建 | DTLN/DFN/VGGish 模型 tarball 下载（CI 缓存） | 2,3 |

---

### Task 1: 入口重采样（3.1）

**Files:**
- Create: `daemon/noise/resampler.hpp`, `daemon/noise/resampler.cpp`
- Modify: `daemon/noise/audio_capture.cpp/.hpp`（入口 native↔48k）, `daemon/noise/CMakeLists.txt`（find_package SpeexDSP + NOISE_SOURCES）, `debian-packages.sh`（libspeexdsp-dev）, `daemon/config.hpp/.cpp` + `daemon/json.cpp`（暂不加 Config 字段，T1 用 daemon sample_rate 判断）

**采用代码 / 关键实现要点**（D-S5.2 + arch §11 风险1）：
- `Resampler` 类（`resampler.hpp`）：参数化采样率对（`Resampler(uint32_t in_rate, uint32_t out_rate, uint32_t channels=1)`）。封装 `speex_resampler_init` + `speex_resampler_process` + `speex_resampler_destroy`（RAII）。`process(const float* in, size_t in_len, float* out, size_t out_max) -> size_t`（返回输出样本数）。**参数化使其可复用**：T1 用 (native, 48k)，T2 DTLN 用 (48k, 16k)。
- 入口位置：`AudioCapture`（或 Bridge on_pcm_frame 解复用后）将 native 采样率帧经 Resampler 转 48k 再分发下游 ①②③④。`PcmCaptureService` 保持原生分发（Streamer 原始路径吃原生帧）。仅 native≠48k 时启用（48k 直通零成本）。
- 采样率来源：daemon Config `sample_rate`（原生）。若 = 48k，resampler 为 passthrough（不实例化 SpeexDSP）。
- 延迟：native↔48k 重采样延迟 <<1ms，计入 DenoiseProcessor `algorithmic_latency_samples()`。

- [ ] **Step 1: 写失败测试** - `noise_test.cpp`：
  - `resampler_48k_passthrough`：in_rate==out_rate==48000 -> 直通，输出 == 输入。
  - `resampler_441_to_48k_sine`：44.1k 正弦 -> 48k -> SNR > 60dB（与参考重采样对比）。
  - `resampler_48k_to_16k`（T2 复用验证）：48k -> 16k 精度。
  - `resampler_96k_to_48k_noise`：96k 含噪 -> 48k -> 频谱合理。
- [ ] **Step 2: 实现 Resampler** - resampler.hpp/.cpp（SpeexDSP RAII + 参数化 + passthrough 优化）。
- [ ] **Step 3: CMake + debian-packages** - find_package(SpeexDSP) + NOISE_SOURCES 加 resampler.cpp + debian-packages.sh 加 libspeexdsp-dev。
- [ ] **Step 4: AudioCapture 入口接入** - native≠48k 时 Resampler 转 48k。
- [ ] **Step 5: 构建 + 测试** - Run: `./noise-dev.sh build && ./daemon/build/noise-test -p` Expected: 全绿（含新 4 case）。
- [ ] **Step 6: 零回归** - Run: `./noise-dev.sh build --no-noise` Expected: 通过（WITH_NOISE=OFF 不引入 SpeexDSP）。
- [ ] **Step 7: 提交** - `feat(noise): Spec5 T1 入口重采样 - SpeexDSP native↔48k 解 48kHz 限制\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

---

### Task 2: DTLN + DeepFilterNet 插件 + ONNX 基建（3.2）

**Files:**
- Create: `daemon/noise/onnx_session.hpp/.cpp`, `daemon/noise/model-adapters/dtln/dtln_adapter.hpp/.cpp`, `daemon/noise/model-adapters/deepfilternet/deepfilternet_adapter.hpp/.cpp`, `daemon/noise/tests/download_models.sh`
- Modify: `daemon/noise/CMakeLists.txt`（find_package ONNXRuntime + NOISE_SOURCES + adapter 子目录）, `daemon/noise/denoise_processor.cpp`（kError 切 Passthrough）, `daemon/noise/denoise_plugin_factory.cpp`（注册 dtln/deepfilternet）, `daemon/config.hpp/.cpp` + `daemon/json.cpp`（onnx_model_dir 字段）, `daemon/main.cpp`（Ort::Env 生命周期）

**采用代码 / 关键实现要点**（D-S5.1/3/4/5 + denoise-plugin §3.2/§3.3/§9）：
- `onx_session.hpp`：`Ort::Env` 进程级单例（`OnnxEnv::instance()`，main 生命周期保证析构晚于 Session）。`CreateOnnxSession(path)` helper（`Ort::Session(env, path, opts)`）。封装 Ort memory info + 选项。
- `DtlnAdapter`（denoise-plugin §3.2）：双 ONNX 模型串联。`init`：加载 model_1.onnx + model_2.onnx（model_2 路径从 model_1 推导同目录 `_2`）；预分配两 LSTM 状态张量（`state1_`/`state2_`，按签名 AllocStateTensor）；native 16k，48k 输入经 T1 Resampler (48k,16k)。`process`：48k->16k -> 累积 buffer -> 每凑满 hop=128 取 frame=512 窗口 -> model_1（幅度谱掩蔽 + LSTM 状态）-> IFFT 时域块 -> model_2（时域增强 + LSTM 状态）-> 16k->48k 重采样输出。两状态张量跨帧喂回。overlap-add。
- `DeepFilterNetAdapter`（denoise-plugin §3.3）：三子图 ONNX。`init`：加载 enc/df_dec/erb_dec.onnx（同目录）；native 48k。`process`：每 hop=480 -> 算 ERB(32) + 复谱(2×96) 特征喂 enc -> embedding + c0 + 编码特征 + lsnr -> df_dec 算深度滤波系数 -> erb_dec 算 ERB 掩蔽 -> 应用回 STFT -> ISTFT 还原。`df_lookahead=2` 缓冲 2 未来 hop。postfilter 可选（PluginConfig params）。lsnr -> DenoiseResult.estimated_snr_db。**简化版**：先跳 postfilter 跑通端到端（R-S5.3）。
- **ONNX 失败降级**（D-S5.5）：process() 内所有 Run() try/catch。单帧失败 -> memcpy in->out + clamp + `result->status=kBypass` + `bypass_count_++`。连续 N=10 帧 kBypass -> `kError`。NaN/Inf 输出替换 0 + 直通。能量突增 >100× 直通。NoiseManager/DenoiseProcessor 收 kError -> switch_plugin("passthrough") + HTTP 告警（复用 Spec4 alert 引擎，新规则 `plugin_degraded`）。
- **WebRTC VAD 补缺**（D-S5.6）：DTLN/DFN 无 VAD，复用 Spec2 NoiseDetector WebRTC VAD（既有）。DenoiseResult.has_vad=false（DTLN/DFN），分析源选择不变。
- **RT 契约**：LSTM/STFT 缓冲构造时分配（运行时零堆分配）；Session 析构由 RcuPtr retire 延迟到 housekeeper；process() 不抛异常。
- **模型下载**：`download_models.sh` 下载 DTLN（model_1/2.onnx）+ DFN（enc/df_dec/erb_dec.onnx）tarball 到 `noise_models/`（CI 缓存，不进 git）。
- DenoisePluginRegistry 注册 `dtln`/`deepfilternet`（factory.cpp）。RnnoiseAdapter **不改**（已支持 model_path）。

- [ ] **Step 1: 写失败测试** - `noise_test.cpp`：
  - `dtln_denoises_speech`：合成含噪语音 -> DTLN -> 降噪量 > 8dB（需模型，skip if 模型未下载）。
  - `dfn_denoises_nonstation`：非平稳噪声（键盘/交通）-> DFN -> 降噪量 > 8dB。
  - `onnx_failure_degrades_to_bypass`：mock 注入 Run() 异常 -> kBypass + 直通，不抛异常。
  - `onnx_consecutive_failure_switches_passthrough`：连续 10 帧 kBypass -> kError -> 切 Passthrough。
  - `dtln_lstm_state_persists`：连续帧 -> LSTM 状态跨帧（首帧 vs 后续输出差异）。
  - `dfn_lookahead_buffers`：lookahead=2 缓冲验证（输出延迟对齐）。
- [ ] **Step 2: 实现 onx_session.hpp** - Ort::Env 单例 + CreateOnnxSession。
- [ ] **Step 3: 实现 DtlnAdapter** - 双模型串联 + LSTM 状态 + 48k↔16k 重采样 + overlap-add。
- [ ] **Step 4: 实现 DeepFilterNetAdapter** - 三子图编排 + STFT + lookahead（先简化版跳 postfilter）。
- [ ] **Step 5: 实现失败降级** - try/catch + kBypass/kError + NaN 清洗 + 切 Passthrough + 告警规则。
- [ ] **Step 6: Registry 注册 + CMake + 模型下载脚本** - factory 注册 dtln/deepfilternet + find_package(ONNXRuntime) + download_models.sh。
- [ ] **Step 7: Config onnx_model_dir + main.cpp Ort::Env** - 字段序列化 + Env 生命周期装配。
- [ ] **Step 8: 构建 + 测试** - Run: `./noise-dev.sh build && ./daemon/build/noise-test -p` Expected: 全绿（模型可用时；无模型时相关 case skip）。
- [ ] **Step 9: 零回归** - Run: `./noise-dev.sh build --no-noise` + objdump daemon 二进制零变化（WITH_NOISE=OFF 无 ONNX）。
- [ ] **Step 10: 提交** - `feat(noise): Spec5 T2 DTLN/DFN 插件 + ONNX Runtime 统一后端 + 失败降级\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

---

### Task 3: L3 ML 分类 VGGish（3.3）

**Files:**
- Create: `daemon/noise/ml_classifier.hpp/.cpp`
- Modify: `daemon/noise/noise_analyzer.hpp/.cpp`（L3 层 + l3_* 字段）, `daemon/noise/noise_template_db.hpp/.cpp`（feature_type + VGGish 嵌入存储）, `daemon/noise/noise_http.cpp`（feature_type + noise_type_source）, `daemon/config.hpp/.cpp` + `daemon/json.cpp`（ml_model_path 字段）, `daemon/main.cpp`（ml_classifier 装配）, `daemon/noise/CMakeLists.txt`（ml_classifier.cpp）

**采用代码 / 关键实现要点**（D-S5.8 + identification §4/§10）：
- `MlClassifier` 类（`ml_classifier.hpp`）：持 VGGish ONNX Session（复用 T2 onx_session）。`embed(const float* pcm, size_t n) -> std::array<float, 128>`（0.96s log-mel 输入 -> VGGish -> 128 维嵌入）。`classify(const float* pcm, const TemplateDB& templates) -> optional<L3Match>`（embed -> kNN/余弦相似度检索 feature_type=vggish 模板 -> 最佳匹配 {type, similarity}）。k=5 最近邻。
- `NoiseAnalyzer` L3 层：`analyze()` 在 L1（规则式）+ L2（Bark 模板）**均未识别**（primary_confidence < 阈值，默认 0.5）时调 `ml_classifier_->classify()`。L3 结果合并进 `NoiseAnalysisResult`（新增 `l3_match_type` + `l3_similarity` + `noise_type_source: "l1"|"l2"|"l3"`）。L3 仅在 L1+L2 未识别时触发（低频，~ms 级可接受）。
- `NoiseTemplateDB`：模板增 `feature_type` 字段（bark|vggish，默认 bark 向后兼容）。VGGish 模板存 128 维嵌入（录入时经 MlClassifier.embed 提取）。L2 匹配用 bark 模板，L3 用 vggish 模板。
- HTTP API：`/api/noise/template` POST 支持 `feature_type`（bark 录入走既有 Bark 提取，vggish 走 VGGish 嵌入）。`/api/noise/sensor/:id` 响应增 `noise_type_source`。
- Config：`ml_model_path`（VGGish 模型路径）。

- [ ] **Step 1: 写失败测试** - `noise_test.cpp`：
  - `vggish_embeds_known_noise`：白噪/键盘声 PCM -> embed -> 128 维向量合理（非零、有限）。
  - `ml_classify_matches_template`：录入 vggish 模板 -> classify 同类噪声 -> 匹配 + similarity > 阈值。
  - `l3_triggers_when_l1l2_unrecognized`：L1+L2 低置信度 -> 触发 L3 -> noise_type_source=l3。
  - `l3_skipped_when_l1_confident`：L1 高置信度 -> 不调 L3（性能：mock 计数 classify 调用次数）。
  - `template_feature_type_roundtrip`：bark/vggish 模板录入 + 检索往返 + 持久化。
- [ ] **Step 2: 实现 MlClassifier** - VGGish ONNX embed + kNN 检索。
- [ ] **Step 3: NoiseAnalyzer L3 层** - analyze L1+L2 未识别时调 classify + l3_* 字段 + noise_type_source。
- [ ] **Step 4: TemplateDB feature_type + VGGish 存储** - 模板 feature_type 字段 + vggish 嵌入存储 + 持久化。
- [ ] **Step 5: HTTP feature_type + noise_type_source** - /template POST feature_type + /sensor/:id 响应。
- [ ] **Step 6: Config ml_model_path + main.cpp 装配** - 字段序列化 + ml_classifier 注入 NoiseAnalyzer。
- [ ] **Step 7: 构建 + 测试** - Run: `./noise-dev.sh build && ./daemon/build/noise-test -p` Expected: 全绿（VGGish 模型可用时；无模型相关 case skip）。
- [ ] **Step 8: 零回归** - Run: `./noise-dev.sh build --no-noise` + objdump 零变化。
- [ ] **Step 9: 提交** - `feat(noise): Spec5 T3 L3 VGGish ML 分类 + kNN 检索\n\nCo-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

---

## Final Verification（所有 task 完成后）

- [ ] **全量构建** - Run: `./noise-dev.sh build` Expected: WITH_NOISE=ON 通过（含 ONNX + SpeexDSP）。
- [ ] **零回归** - Run: `./noise-dev.sh build --no-noise` + objdump 对比 Spec4 base（`b7dd4f9`）daemon 二进制 WITH_NOISE=OFF 零变化。
- [ ] **noise-test** - Run: `./daemon/build/noise-test -p` Expected: 全绿（Spec4 84 + Spec5 新增；模型相关 case 无模型时 skip）。
- [ ] **daemon-test** - Run: `./daemon/build/daemon-test -p` Expected: 既有 case 不回归（pre-existing set_ptp_config fatal 仍 out of scope）。
- [ ] **生产冒烟** - Run: `./noise-dev.sh run` + curl 验证：96k daemon.conf + 降噪正常、switch_plugin dtln/deepfilternet 可用、L3 分类（合成未知噪声）触发。
- [ ] **契约 additive 复核** - 对比 Spec4 §C：既有路由/字段语义零变化，新增项符合 §C 增量契约。
- [ ] **依赖合规** - debian-packages.sh + LICENSE_NOTICES.MD 更新（ONNX Runtime MIT、SpeexDSP BSD、VGGish Apache-2.0）。
- [ ] **final whole-branch review** - subagent-driven 末尾跑整分支 review（`b7dd4f9..HEAD`），重点：ONNX 线程安全 + RT 不阻塞 + 失败降级 + additive + WITH_NOISE=OFF 零回归。

## 执行顺序与依赖（spec5 §F）

T1（重采样基建，独立）-> T2（DTLN/DFN，复用 T1 resampler）-> T3（L3 VGGish，独立识别链，复用 T1 重采样后 48k PCM）。subagent-driven 串行，每 task implementer + reviewer + fix，末尾 final whole-branch review。

# Spec6 实现计划

## 概述

Spec6 = Phase 3 收尾，3 组 task 按序（T1 -> T2 -> T3），每组 implementer + reviewer + fix，末尾 final whole-branch review。设计依据 `docs/superpowers/specs/noise-spec6-design.md`（commit 560f6b4）。

Worktree: `.claude/worktrees/aes67-linux-daemon-noise-spec6`（feature/noise-spec6，base c853970 = master 含 Spec5 + openapi 补全）。

---

## T1: 3.5 历史持久化 + Spec5 字段暴露 + Minor + openapi 整理

### Files
- **Create**: `daemon/noise/noise_history.hpp`, `daemon/noise/noise_history.cpp`
- **Modify**:
  - `daemon/noise/noise_manager.hpp/.cpp`（注入 NoiseStore + 后台 flush 线程）
  - `daemon/noise/noise_metrics.hpp/.cpp`（l3_*/plugin_degraded/alert_level 字段 + append_snapshot_fields 序列化）
  - `daemon/noise/noise_http.cpp/.hpp`（历史查询/导出端点）
  - `daemon/noise/noise_template_db.cpp`（template_to_json_object 加 feature_type/vggish_embedding）
  - `daemon/config.hpp/.cpp` + `daemon/json.cpp`（noise_db_path/noise_history_retention_hours/noise_history_flush_interval_s）
  - `daemon/main.cpp`（NoiseStore 装配）
  - `daemon/noise/CMakeLists.txt`（find_package(SQLite3) + noise_history.cpp）
  - `debian-packages.sh`（libsqlite3-dev）
  - `daemon/noise/ml_classifier.hpp`（Minor: k=1 文档 + log1p 注释）
  - `daemon/noise/model-adapters/deepfilternet/deepfilternet_adapter.cpp`（Minor: enc index 0..6）
  - `docs/contracts/http/openapi.yaml`（整理：既有错误 + 历史端点 + Spec5 字段标注去除）

### Steps（TDD）
1. **写失败测试**（`noise_test.cpp`）：
   - `history_persists_metrics_to_sqlite`：喂 N period metrics -> SQLite metrics 表有记录
   - `history_persists_alerts_to_sqlite`：触发 alert -> SQLite alerts 表有记录
   - `history_query_by_time_range`：GET /history?from=&to= 返回范围内记录（无参默认 60s ring 兼容）
   - `history_export_json`：GET /history/export?format=json 返回 JSON 数组
   - `history_export_csv`：GET /history/export?format=csv 返回 CSV 下载
   - `history_retention_cleanup`：过期记录被后台清理
   - `l3_fields_in_metrics_response`：/metrics 响应含 l3_match_type/l3_similarity
   - `plugin_degraded_alert_level_in_metrics`：/metrics 含 plugin_degraded/alert_level
   - `feature_type_vggish_in_template_response`：/template 响应含 feature_type/vggish_embedding
2. **实现 NoiseStore**（SQLite + WAL + batch flush + retention）：per-sensor metrics_history 表 + 全局 alerts_history 表，(sensor_id, timestamp_ms) 索引，WAL 模式，控制线程定时 flush ring -> SQLite，定时清理过期。
3. **字段序列化**：NoiseMetricsSnapshot 加 l3_match_type/l3_similarity/plugin_degraded（从 NoiseAnalysisResult 拷入）+ append_snapshot_fields 输出；template_to_json_object 输出 feature_type/vggish_embedding。
4. **HTTP 历史端点**：扩展 GET /api/noise/sensor/{id}/history（?from=&to= 查 SQLite，无参 60s ring）+ GET /api/noise/sensor/{id}/history/export（?from=&to=&format=json|csv）。
5. **config + main 装配**：noise_db_path/noise_history_retention_hours(默认24)/noise_history_flush_interval_s(默认10) + main.cpp 注入 NoiseManager。
6. **CMake + debian-packages**：find_package(SQLite3) + noise_history.cpp + libsqlite3-dev。
7. **Minor 清理**：ml_classifier.hpp k=1 文档 + log1p 注释；deepfilternet_adapter.cpp enc index 0..6。
8. **openapi 整理**：SsePcmStream 实际格式（data:{JSON{pcm_base64}} 无 event/id）+ NoiseTemplate 删 phantom 加 wav_file + POST /template 返回 200 {id,label,status} + POST /template/{id}/test 返回格式 + DaemonConfig 删 phantom 加漏字段 + 删无引用 PluginInfo/DenoiseConfig/PluginParams + 历史端点 + Spec5 字段标注去除。
9. **构建 + 测试**：`./noise-dev.sh build && ./daemon/build/noise-test -p` 全绿。
10. **零回归**：`./noise-dev.sh build --no-noise` + objdump 0 SQLite/noise 符号。
11. **提交**：`feat(noise): Spec6 T1 历史持久化 + 字段暴露 + openapi 整理`

---

## T2: DFN fidelity + RefComparator

### Files
- **Modify**:
  - `daemon/noise/model-adapters/deepfilternet/deepfilternet_adapter.cpp`（deep-filter non-causal 重写）
  - `daemon/noise/ref_comparator.hpp/.cpp`（native rate 或 48k 路由）
  - `daemon/noise/noise_manager.cpp`（route_to_ref_comparators 用 48k chunk）
  - `daemon/noise/denoise_processor.hpp/.cpp`（algorithmic_latency_samples 含 resampler 延迟）
  - `daemon/pcm_capture_service.cpp`（set_latency_change_cb 消费者，播放延迟补偿）

### Steps（TDD）
1. **写失败测试**：
   - `dfn_deep_filter_non_causal_window`：输出帧 i 卷积 [i-2..i+2]（2 future frames）
   - `dfn_coef_mapping_oldest_first`：coef[0] 配最旧帧
   - `dfn_assign_df_alpha_blend`：spec_f = spec_f*alpha + spec*(1-alpha)
   - `ref_comparator_48k_route_native_not_48k`：native≠48k 时 comparator 收 48k 帧
   - `resampler_latency_in_algorithmic_latency`：algorithmic_latency_samples 含 resampler 延迟
2. **DFN deep-filter 重写**（对照 `DeepFilterNet/df/modules.py` DfOp/spec_pad/assign_df）：non-causal window + buffer future frames + coef mapping + alpha blend + 延迟输出对齐。
3. **RefComparator 48k 路由**：route_to_ref_comparators 用 resampled 48k chunk（或 comparator 传 native rate + 内部重采样）。
4. **resampler 延迟折入**：algorithmic_latency_samples 含 Resampler::output_latency()；set_latency_change_cb 消费者在 PcmCaptureService 做播放延迟补偿。
5. **删除延后注释**：deepfilternet_adapter.cpp 文件头 + 6b debt 注释；noise_manager.cpp RefComparator 注释；resampler.hpp output_latency 注释。
6. **构建 + 测试**：全绿（DFN 模型可用时；无模型 SKIP）。
7. **零回归**。
8. **提交**：`fix(noise): Spec6 T2 DFN non-causal 重写 + RefComparator 48k + resampler 延迟折入`

---

## T3: 降级线程模型 + 3.6 并行/RT refactor

### Files
- **Modify**:
  - `daemon/noise/noise_manager.hpp/.cpp`（switch_plugin 迁控制线程 + per-sink thread pool）
  - `daemon/noise/noise_session_manager_bridge.cpp`（on_pcm_frame 分发 per-sink 队列）
  - `daemon/noise/denoise_processor.hpp/.cpp`（控制线程 drain degraded_pending_）
  - `daemon/noise/fft.hpp`（Rfft/Irfft 返回改 span/输出参数）
  - `daemon/noise/model-adapters/dtln/dtln_adapter.cpp`（buf 预分配成员）
  - `daemon/noise/model-adapters/deepfilternet/deepfilternet_adapter.cpp`（buf 预分配成员）
  - `daemon/noise/noise_template_db.hpp/.cpp`（seqlock 替代 recursive_mutex）

### Steps（TDD）
1. **写失败测试**：
   - `switch_plugin_on_control_thread_not_capture`：降级切换在控制线程（capture 线程只置 flag）
   - `per_sink_parallel_processing`：多 sink on_frame 并行（线程独立）
   - `xrun_degradation`：ALSA xrun 时降级（跳过/直通）
   - `fft_rfft_no_heap_allocation`：Rfft/Irfft 不返 vector（span/输出参数）
   - `dtln_dfn_no_per_frame_heap`：process 无 per-call 堆分配（预分配成员）
   - `template_db_seqlock_read`：capture 读无锁（seqlock）
2. **降级线程模型迁控制线程**：degraded_pending_ atomic（RT 置）+ 控制线程 drain（switch_plugin + drain_retire）；on_period_end 不再直接 switch。
3. **per-sink 独立线程**：on_pcm_frame 分发到 per-sink 队列 + 每 sink 独立线程处理 on_frame（sink≤64）。
4. **xrun 降级**：ALSA xrun 检测 + 跳过 period 或直通。
5. **RT heap 预分配**：fft.hpp Rfft/Irfft 返回改 span/输出参数；DTLN contig/DFN buf 改预分配成员（std::array 或构造 reserve）。
6. **TemplateDB seqlock**：recursive_mutex 改 seqlock（HTTP 写少，capture 读多；write sequence + read retry）。
7. **构建 + 测试**：全绿。
8. **零回归**（objdump + TSan 可选）。
9. **提交**：`refactor(noise): Spec6 T3 降级线程模型迁控制线程 + per-sink thread + RT heap 预分配 + seqlock`

---

## Final Verification（所有 task 完成后）

- **全量构建**：`./noise-dev.sh build`（WITH_NOISE=ON，含 SQLite + ONNX + SpeexDSP）
- **零回归**：`./noise-dev.sh build --no-noise` + objdump 对比 Spec5 base（5e6040b）daemon 二进制 WITH_NOISE=OFF 零变化
- **noise-test**：`./daemon/build/noise-test -p` 全绿
- **daemon-test**：`./daemon/build/daemon-test -p`（set_ptp_config fatal 已修，不应回归）
- **openapi 验证**：`python3 -c "import yaml; yaml.safe_load(open('docs/contracts/http/openapi.yaml'))"`
- **final whole-branch review**：subagent-driven 末尾整分支 review（`5e6040b..HEAD`），重点：SQLite 线程安全 + per-sink thread 无竞争 + RT 零堆分配 + seqlock 正确 + additive + WITH_NOISE=OFF 零回归 + openapi 对齐实际

## 执行顺序与依赖

T1（历史+字段+openapi，独立）-> T2（DFN+RefComparator，复用 T1 字段）-> T3（线程模型+RT refactor，架构改动最大）。
subagent-driven 串行，每 task implementer + reviewer + fix，末尾 final whole-branch review。

## 约束（继承 Spec5）

- additive 不破坏 Spec5 §C / Spec4 §C
- RT 安全（per-sink thread 无竞争；seqlock 正确；heap 预分配零 per-frame 分配；switch_plugin 不在 RT）
- WITH_NOISE=OFF 零回归（SQLite/DFN/RT refactor 仅 WITH_NOISE 编译；objdump 0 符号）
- SQLite 线程安全（WAL + 单写线程）
- ONNX 线程安全（Env 单例 + per-sensor Session + 析构延迟）
- rm 禁令（tracked 删用 git rm；test temp 用 std::remove）
- 中文 commit + Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
- 只 commit 不 push

## out-of-scope

- GPU/Rust/crossfade（后续）
- 3.4 ALSA 回注（LKM 风险，独立 spec）
- 真实 DFN/VGGish 模型 fidelity 验证（需模型，CI skip）

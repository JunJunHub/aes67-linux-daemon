# Spec6 设计 - Phase 3 收尾

## 范围

统一纳入 3.5 历史持久化 + 3.6 并行/RT refactor + Spec5 延后项。Spec5（Phase 3 part 1）已完成合并 master（b7dd4f9..5e6040b），本 spec 是 Phase 3 收尾。

## Task 分解（3 组按序，每组 implementer + reviewer + fix）

### T1: 3.5 历史持久化 + Spec5 字段暴露 + Minor + openapi 整理

#### 3.5 历史持久化（SQLite，D-S6.1）

- **存储**：NoiseStore 通用 SQLite 仓储（`noise.sqlite`，按 config noise_db_path 放置；历史是首个用途，未来可扩展其他数据存储）
  - `metrics_history` 表：sensor_id, timestamp_ms, noise_level_dbfs, snr_db, spectral_flatness, noise_type_source, l3_match_type, l3_similarity, plugin_degraded, alert_level, ...（NoiseMetricsSnapshot 全字段）
  - `alerts_history` 表：sensor_id, timestamp_ms, level, rule, message, is_active（AlertEvent）
  - 索引：(sensor_id, timestamp_ms) 复合索引，时间范围查询高效
  - WAL 模式（并发读 + 单写线程）
- **后台 flush**：定时（每 N 秒，config noise_history_flush_interval_s 默认 10s）批量 insert。RT 路径只写内存 ring（现有 60s ring 保留），控制线程定时 flush ring → SQLite。不每 period 写（避免 RT I/O）。
- **保留时长**：config `noise_history_retention_hours`（默认 24h），后台定时清理过期记录（DELETE WHERE timestamp_ms < now - retention）。
- **HTTP 接口**（D-S6.2，扩展现有 + 导出）：
  - `GET /api/noise/sensor/{id}/history?from={ms}&to={ms}` -- 扩展现有 60s ring；无参默认 60s ring（兼容），有 from/to 查 SQLite（时间范围）
  - `GET /api/noise/sensor/{id}/history/export?from={ms}&to={ms}&format=json|csv` -- 导出历史（JSON 数组 / CSV 下载）
  - openapi 同步（新端点 + 查询参数 + 响应 schema）
- **依赖**：libsqlite3-dev（debian-packages.sh + LICENSE_NOTICES.MD，WITH_NOISE=OFF 不引入）

#### Spec5 字段暴露（D-S6.7）

- `l3_match_type`/`l3_similarity`：NoiseMetricsSnapshot 加字段 + `append_snapshot_fields` 序列化 + /metrics + /history 含
- `plugin_degraded`/`alert_level`：`append_snapshot_fields` 序列化 + /metrics + /history 含
- `feature_type`/`vggish_embedding`：`template_to_json_object` 输出 + /template 响应含
- openapi 去"未序列化"标注

#### Minor 清理

- kNN k=1 vs k=5 文档（ml_classifier.hpp:86 改"取最高者（k=1 最近邻）"）
- stale log1p 注释（ml_classifier.hpp:17 改"log(mel+0.01) 功率谱"）
- DFN enc comment off-by-one（deepfilternet_adapter.cpp:337 "index 0..6" 非 0..7）

#### openapi 整理（修既有错误，D-S6.8）

- SsePcmStream 改实际格式（`data:{JSON{pcm_base64 of S16 LE}}\n\n`，无 event/id，连接时 `: connected`）
- NoiseTemplate 删 phantom（reference_level_dbfs/created_at/source）+ 加 wav_file
- POST /template 返回 200 `{id,label,status}`（非 201 + NoiseTemplate）
- POST /template/{id}/test 返回 `{matched_template_id,similarity,requested_template_id}`
- DaemonConfig 删 phantom `noise_max_sensors` + 加 noise_status_file/noise_template_dir/fake_pcm_source
- 删无引用 PluginInfo/DenoiseConfig/PluginParams schema + "Denoise Plugin" tag

### T2: DFN fidelity + RefComparator

#### DFN deep-filter causality 重写（D-S6.6）

- 对照 `DeepFilterNet/df/modules.py` 的 `DfOp` + `spec_pad` + `assign_df`（真实 df 神经逻辑；lib.rs 是 bare STFT round-trip）
- non-causal window `[i-2..i+2]`（buffer 2 future frames，lookahead=2）
- coef-to-frame mapping：coef[0] 配最旧帧（o=0..4 pair with offset from oldest）
- assign_df alpha blend：`spec_f = spec_f*alpha + spec*(1-alpha)`（alpha from gain，非纯乘替换）
- 延迟输出 df_lookahead hops 对齐
- 删除 T2 review 标注的 DFN correctness debt（文件头 + 6b 代码注释）

#### RefComparator native≠48k + resampler 延迟折入

- RefComparator 路由改重采样后 48k 帧（route_to_ref_comparators 用 48k chunk 而非 native 帧）；或 comparator 构造传 native rate + 内部重采样
- resampler 延迟折入 `algorithmic_latency_samples`（延迟上报机制接线：set_latency_change_cb 消费者在 PcmCaptureService 做播放延迟补偿）
- 删除 T1 review 标注的延后项（noise_manager.cpp + resampler.hpp 注释）

### T3: 降级线程模型 + 3.6 并行/RT refactor

#### 降级线程模型迁控制线程 housekeeper（D-S6.5）

- switch_plugin 从 `on_period_end`（capture 线程）迁控制线程
- `degraded_pending_` atomic flag（RT 置）+ 控制线程 drain（执行 switch_plugin + drain_retire）
- 消除 capture 线程 Ort::Session teardown 风险（Spec5 T2 review Important #2）
- 删除 T2 review 标注的线程模型偏差注释，改为实际控制线程执行

#### 3.6 并行/RT refactor（D-S6.4 全做）

- **per-sink 独立线程**：on_frame 每 sink 独立线程（sink≤64，per-sink 线程简单；或线程池 N 线程处理 M sink）。on_pcm_frame 分发到 per-sink 队列 + 线程。
- **xrun 降级**：ALSA xrun 时降级处理（跳过 period 或直通）
- **RT heap 预分配**：
  - fft.hpp Rfft/Irfft 返回改 span/输出参数（不返 vector，消除 per-call 堆）
  - DTLN contig/DFN buf（input/buf/spec_m/spec_e/out_frame）改预分配成员（std::array 或构造时 reserve，运行时零分配）
- **mutex->seqlock**：TemplateDB recursive_mutex 改 seqlock（HTTP 写少，capture 读多）；metrics_mutex_ 保留（聚合需互斥）

## 决策汇总

- D-S6.1 通用 NoiseStore SQLite 仓储（noise.sqlite，历史表 metrics_history/alerts_history，未来可扩展其他数据存储；查询高效，时间范围索引，libsqlite3 依赖）
- D-S6.2 历史接口扩展现有 GET /history?from=&to= + GET /history/export
- D-S6.3 历史保留时长可配（noise_history_retention_hours 默认 24h）+ 后台定时 flush（noise_history_flush_interval_s 默认 10s）
- D-S6.4 RT refactor 全做（per-sink thread + heap 预分配 + seqlock + xrun）
- D-S6.5 降级线程模型迁控制线程（path A 严格化，消除 capture 线程 teardown）
- D-S6.6 DFN non-causal 重写（对照 modules.py）
- D-S6.7 Spec5 字段代码暴露（l3_*/plugin_degraded/feature_type 序列化）
- D-S6.8 openapi 整理（既有错误修复 + 历史端点 + Spec5 字段标注去除）

## 约束

- additive 不破坏 Spec5 §C / Spec4 §C（①②③④ 链序不变；passthrough 路径字节一致）
- RT 安全（per-sink thread 不引入竞争；seqlock 正确；heap 预分配零 per-frame 分配；switch_plugin 不在 RT）
- WITH_NOISE=OFF 零回归（SQLite/DFN/RT refactor 仅 WITH_NOISE 编译；objdump 0 符号）
- SQLite 线程安全（WAL 模式 + 单写线程 + 连接 per-thread 或串行化）
- ONNX 线程安全（继承 Spec5：Env 单例 + per-sensor Session + 析构延迟）

## 依赖

- libsqlite3-dev（SQLite，debian-packages.sh + LICENSE_NOTICES.MD）
- DeepFilterNet/df/modules.py（DFN 重写参考，已克隆 3rdparty 或参考仓库）

## 执行顺序

T1（历史+字段+Minor+openapi）-> T2（DFN+RefComparator）-> T3（线程模型+RT refactor）
末尾 final whole-branch review（b7dd4f9..HEAD 或 5e6040b..HEAD）。

## out-of-scope

- GPU/Rust/crossfade（留后续）
- 3.4 ALSA 回注（LKM 风险，独立 spec）
- 真实 DFN/VGGish 模型 fidelity 验证（需模型，CI skip）

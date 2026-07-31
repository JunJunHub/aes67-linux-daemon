# YAMNet 迁移实现计划

## 概述

将 L3 ML 分类层从 VGGish 嵌入 + kNN 模板匹配替换为 YAMNet 端到端多标签分类。YAMNet 直接输出 521 类 AudioSet 分数 + 1024 维嵌入，无需模板库，且能识别噪声声源（空调、风扇、交通等），与 L1 规则式的频域特征分类形成互补维度。

设计依据：`docs/superpowers/specs/noise-spec5-design.md`（L3 架构）、`docs/noise/architecture-design.md` §3.3。

分支：`feature/noise`（当前 worktree）。

## 背景

### 现状（VGGish + kNN）

- `MlClassifier` 输入 log-mel [1,96,64]，输出 128 维嵌入，再对 `NoiseTemplateDB` 中 `feature_type=vggish` 的模板做 kNN 余弦检索
- 需要手动录入模板（WAV -> embed -> 存 DB），模板覆盖面有限
- `classify()` 返回 `optional<L3Match{template_id, label, similarity}>`
- 分析器 `maybe_run_l3_()` 攒 0.96s @48k PCM，L1 置信度 < 0.5 时触发

### 目标（YAMNet 直接分类）

- `MlClassifier` 输入波形 [1,48000]（3s @16kHz），输出 scores [6,521] + embeddings [6,1024]
- 无需模板库，直接从 scores 取 top-N 噪声相关类别
- `classify()` 返回 `optional<MlResult{type_name, score, top_types}>`
- 分析器攒 3s @48k PCM（重采样到 16k = 48000 样本），触发条件不变

### 两个维度的互补

| 维度 | L1 规则式 | YAMNet |
|------|----------|--------|
| 回答 | 什么特征的噪声 | 什么东西在响 |
| 优势 | 50Hz/60Hz 精确区分、频谱平坦度 | 声源识别、多标签 |
| 输出 | NoiseType 枚举 | 类名字符串 + 分数 |

API 同时上报两个维度，不合并。

## 设计决策

### D1: NoiseType 枚举不扩展

L1 的 9 种类型（Clean/White/Pink/Hum50Hz/Hum60Hz/Impulse/Broadband/Digital/Unknown）是频域特征分类，与 YAMNet 的声源识别是不同维度。不把 YAMNet 的 521 类硬塞进枚举。

### D2: YAMNet 输出字段独立于 L3 现有字段

用新字段名 `ml_noise_type` / `ml_noise_score` / `ml_top_types` 替换 `l3_match_type` / `l3_similarity`。`noise_type_source` 仍用 "l1"/"l3" 标识来源层。

### D3: 噪声类别白名单过滤

YAMNet 521 类中大部分无关（动物、乐器、婴儿哭声等）。内置约 40 个噪声相关类别 index 白名单，只在白名单内取 top-N，避免输出 "Roaring cats" 之类。

### D4: 保留 NoiseTemplateDB

L2 Bark 模板匹配仍使用 `NoiseTemplateDB`，HTTP `/api/noise/templates/*` 路由保留。仅 L3 不再依赖模板库。`MlClassifier` 不再接收 `NoiseTemplateDB&` 参数。

### D5: PCM 环形缓冲扩大

从 0.96s @48k（46080 样本）扩大到 3s @48k（144000 样本），匹配 YAMNet 输入需求。L3 冷却周期从 0.96s（96 帧）调整为 3s（300 帧），避免在 3s 窗口未满时触发。

### D6: Speech 排除

降噪关闭时分析原始音频，YAMNet 可能高分 "Speech"。白名单排除 Speech(0) 及语音相关类别，确保只报告噪声类型。

## Tasks

### T1: MlClassifier 接口与实现重写

**Files:**
- **Rewrite**: `daemon/noise/ml_classifier.hpp`, `daemon/noise/ml_classifier.cpp`

**hpp 接口变更:**

```cpp
// 新返回结构（替换 L3Match）
struct MlTypeScore {
  std::string type_name;  // AudioSet 类名（如 "Air conditioning"）
  float score{0.0f};      // YAMNet 分数 [0, 1]
};

struct MlResult {
  std::string type_name;               // top-1 类名（空=未分类）
  float score{0.0f};                   // top-1 分数
  std::vector<MlTypeScore> top_types;  // top-3（白名单内，score > 0.1）
};
```

- 移除 `L3Match`、`kVggishEmbedDim`、`NoiseTemplateDB` 前向声明
- `classify()` 签名变更：移除 `NoiseTemplateDB&` 参数，返回 `optional<MlResult>`
- `embed()` 保留但维度从 128 -> 1024，输入改为 3s 波形（备用，当前不调用）
- `init()` 增加可选 class_map CSV 路径参数（与 model_path 同目录自动查找）

**cpp 实现变更:**

- 移除：mel filterbank、STFT、重采样器、`hz_to_mel`/`mel_to_hz`、VGGish 预处理常量
- 新增：YAMNet 常量（`kYamnetSampleRate=16000`、`kYamnetInputSamples=48000`、`kYamnetNumClasses=521`）
- 新增：噪声类别白名单（`kNoiseClassIndices[]`，约 40 个 index）
- 新增：class_map CSV 加载（`index -> display_name` 映射，`std::unordered_map<int, std::string>`）
- 新增：48k->16k 重采样（保留 `Resampler`，从 46080 -> 48000 改为 144000 -> 48000）
- `classify()` 实现：
  1. 重采样 3s @48k -> 48000 @16k
  2. ONNX Run：输入 `new_input` [1,48000]，取 `scores` [6,521]
  3. 6 帧分数取均值 -> [521]
  4. 白名单过滤 + 降序排序 -> top-3（score > 0.1）
  5. top-1 -> `MlResult`
- I/O 按 index 绑定（`new_input`=0, `scores`=2，与 T2 adapter 同手法）

### T2: NoiseAnalyzer 适配

**Files:**
- **Modify**: `daemon/noise/noise_analyzer.hpp`, `daemon/noise/noise_analyzer.cpp`

**hpp 变更:**
- `kVggishWindowSamples` 46080 -> `kYamnetWindowSamples` 144000（3s @48k）
- `kL3CooldownFrames` 96 -> 300（3s @10ms/帧）
- 移除 `template_db_` 成员、`set_template_db()` 方法、`NoiseTemplateDB` 前向声明
- `maybe_run_l3_()` 签名移除 `NoiseTemplateDB&`（classify 不再需要）
- `NoiseAnalysisResult` 字段重命名：`l3_match_type` -> `ml_noise_type`，`l3_similarity` -> `ml_noise_score`
- 新增 `NoiseAnalysisResult::ml_top_types`（`std::vector<MlTypeScore>`，最多 3 项）

**cpp 变更:**
- `maybe_run_l3_()` 中 `classify()` 调用移除 `template_db_` 参数
- PCM ring buffer 分配改为 `kYamnetWindowSamples`
- 结果填充：`ml_noise_type` = top-1 type_name，`ml_noise_score` = top-1 score，`ml_top_types` = top-3

### T3: NoiseMetrics 字段适配

**Files:**
- **Modify**: `daemon/noise/noise_metrics.hpp`, `daemon/noise/noise_metrics.cpp`

**hpp 变更:**
- `NoiseMetricsSnapshot` 字段重命名：`l3_match_type` -> `ml_noise_type`，`l3_similarity` -> `ml_noise_score`
- 新增 `NoiseMetricsSnapshot::ml_top_types`（序列化为 JSON 字符串或简化结构）

**cpp 变更:**
- `collect()` 中字段拷贝路径重命名

### T4: HTTP API 字段适配

**Files:**
- **Modify**: `daemon/noise/noise_http.cpp`, `daemon/noise/noise_http.hpp`

**变更:**
- JSON metrics 序列化：`l3_match_type` -> `ml_noise_type`，`l3_similarity` -> `ml_noise_score`，新增 `ml_top_types` 数组
- CSV 导出：列名同步重命名，`ml_top_types` 用分号分隔的 `type:score` 对
- `register_noise_template_routes()` 签名移除 `MlClassifier` 参数（模板路由不再需要 embed）
- 模板路由内部移除 `ml_classifier` 的使用（`add_template_from_wav` 不再调 embed）

### T5: NoiseManager + main.cpp 装配

**Files:**
- **Modify**: `daemon/noise/noise_manager.hpp`, `daemon/noise/noise_manager.cpp`, `daemon/main.cpp`

**变更:**
- `NoiseManager` 移除 `template_db_` 成员和 `set_template_db()` 方法（L3 不再需要）
- `add_sensor()` 中移除 `ctx.analyzer->set_template_db(template_db_)` 调用
- `main.cpp` 移除 `noise_template_db` 的创建和 `set_template_db` 调用
- `main.cpp` 中 `register_noise_template_routes()` 调用移除 `ml` 参数
- `main.cpp` 中 `ml_classifier->init()` 改为传入 YAMNet 模型路径（config `ml_model_path` 指向 `yamnet_3s.onnx`）
- 保留 `noise_template_db` 的创建和保存（L2 Bark 模板仍需要）

### T6: 测试适配

**Files:**
- **Modify**: `daemon/noise/tests/noise_test.cpp`

**变更:**
- `FakeMlClassifier` 子类：`embed()` 返回 `array<float, 1024>`，`classify()` 签名移除 `NoiseTemplateDB&`
- `spec5_find_vggish_model()` -> `spec5_find_yamnet_model()`，查找路径改为 `yamnet_3s.onnx`
- VGGish 嵌入测试 -> YAMNet 分类测试：
  - `vggish_embeds_known_noise` -> `yamnet_classifies_known_noise`：白噪声 -> scores 中 White noise 高分
  - `fake_classifier_l3_trigger`：FakeMlClassifier 返回 MlResult -> analyzer 填 `ml_noise_type`
  - 字段断言：`l3_match_type` -> `ml_noise_type`，`l3_similarity` -> `ml_noise_score`
  - CSV/JSON 字段名断言同步更新
- class_map CSV 路径查找（与模型同目录 `yamnet_class_map.csv`）

### T7: 模型下载脚本与配置

**Files:**
- **Modify**: `daemon/noise/tests/download_models.sh`

**变更:**
- `download_vggish()` -> `download_yamnet()`：下载 `yamnet_3s.onnx` + `yamnet_class_map.csv`
- 目录布局注释更新
- 移除 VGGish TF checkpoint 下载和 ONNX 转换步骤

## 验证

1. **编译**：`./noise-dev.sh build`（FAKE_DRIVER=ON, WITH_NOISE=ON, WITH_STREAMER=ON）
2. **单元测试**：`cd build && ./tests/noise-test -p`（noise 相关用例全通过）
3. **daemon-test**：`cd build && ./tests/daemon-test -p`（无回归）
4. **手动验证**：
   - 用白噪声 PCM 源跑 daemon，`GET /api/noise/metrics` 检查 `ml_noise_type` 字段
   - 用真实噪声样本（VoiceBank-DEMAND）验证 YAMNet 分类合理性
5. **--no-noise 回归**：`WITH_NOISE=OFF` 编译零影响

## 依赖

- YAMNet ONNX 模型：`/home/Share/GitHub/noise-model/yamnet/yamnet_3s.onnx`（15MB，已就绪）
- class_map CSV：`/home/Share/GitHub/noise-model/tensorflow-models/research/audioset/yamnet/yamnet_class_map.csv`（已就绪）
- ONNX Runtime：已集成（Spec5 T2）

## 不在范围

- 告警引擎变更（后续增量）
- L2 Bark 模板匹配变更
- WebUI 前端适配
- `NoiseTemplateDB` 删除（L2 仍使用）
- 噪声类别白名单的动态配置（硬编码足够）

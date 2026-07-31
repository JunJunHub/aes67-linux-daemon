# 噪声分析检测报告

> **版本**: v1.1
> **日期**: 2026-07-31
> **环境**: FAKE_DRIVER 模式，out-of-source 构建（WITH_NOISE=ON, WITH_STREAMER=ON）
> **模型**: YAMNet yamnet_3s.onnx（15MB，ONNX Runtime 1.27.0）
> **关联**: [架构设计](architecture-design.md) §3.3、[YAMNet 迁移计划](../superpowers/plans/yamnet-migration-plan.md)

---

## 1. 测试环境

| 项目 | 配置 |
|------|------|
| OS | Linux 7.0.0-28-generic |
| CPU | 16 核 |
| 内存 | 8.7 GB |
| 构建方式 | out-of-source（daemon/build/） |
| 编译选项 | FAKE_DRIVER=ON, WITH_NOISE=ON, WITH_STREAMER=ON, WITH_AVAHI=OFF, ENABLE_TESTS=ON |
| 模型目录 | `noise_models/`（worktree 内子目录布局） |
| 测试数据集 | ESC-50（2000 文件，22 类环境声）→ 64 文件转 48kHz mono，存 `noise-testset/concurrent64/audio/` |
| 测试脚本 | `noise-testset/concurrent64/run_verify.sh`（启停 daemon + 建 64 sink/sensor + 查询统计） |

### 模型目录布局

```
noise_models/
├── dtln/{model_1.onnx, model_2.onnx}
├── deepfilternet/{enc.onnx, df_dec.onnx, erb_dec.onnx}
└── yamnet/{yamnet_3s.onnx, yamnet_class_map.csv}
```

adapter 路径推导：`onnx_model_dir` 指向父目录，各 adapter 先找 `<dir>/<plugin_name>/` 子目录，再回退 `<dir>/` 平铺。

### 64 路并发测试配置

```
noise-testset/concurrent64/
├── audio/ch00.wav..ch63.wav   # 64 文件，48kHz mono s16，各 channel 相位错开
├── manifest.csv               # channel → ESC-50 文件 → category ground truth
├── config/daemon.conf         # streamer_channels=64, fake_pcm_source=audio 目录
├── prepare_audio.sh           # 从 ESC-50 选 64 文件 + ffmpeg 转换 + manifest
└── run_verify.sh              # 一键验证：start/setup/query/summarize/runall
```

测试链路贴近真实场景：64 个 sink 各配 `map:[N]` 绑定驱动 channel N，64 个 sensor
各关联 sink_id=N，`fake_pcm_source` 目录模式提供 64 通道 WAV（详见 §4.1）。

---

## 2. 三路降噪模型对比验证

### 2.1 白噪声测试

**音频源**: 合成白噪声 10s（48kHz mono，seed=42）

| 维度 | RNNoise | DTLN | DeepFilterNet |
|------|---------|------|---------------|
| **降噪量** | 48.8 dB | 36.0 dB | 12.6 dB |
| **L1 规则式** | white (0.47) | white (0.51) | white (0.35) |
| **L2 模板匹配** | 无模板 | 无模板 | 无模板 |
| **L3 YAMNet** | White noise (0.58) | White noise (0.53) | Static (0.29) |
| **L3 top-3** | White noise, Noise, Static | White noise, Noise, Static | Static, White noise, Pink noise |
| **告警** | critical | critical | critical |
| **plugin_degraded** | false | false | false |

### 2.2 含噪语音测试（VoiceBank-DEMAND）

**音频源**: VoiceBank-DEMAND `p232_023.wav`（含噪语音，9.8s）

| 维度 | RNNoise | DTLN | DeepFilterNet |
|------|---------|------|---------------|
| **降噪量** | 10.1 dB | 4.3 dB | 7.7 dB |
| **L1 规则式** | unknown (0.00) | unknown (0.00) | pink (0.22) |
| **L3 YAMNet** | Pour (0.07) | Pour (0.10) | Liquid (0.06) |
| **L3 top-3** | Pour, Liquid, Trickle | Pour, Trickle, Liquid | Liquid, Trickle, Pour |

**说明**:
- L1 分析噪声分量（original - denoised），SF≈0.01-0.03，不匹配规则式类型
- L3 分析原始音频，YAMNet 多标签分类识别出水声/液体声背景噪声
- Speech(0.75) 被白名单排除，Pour/Liquid/Trickle 是剩余高分噪声类别

---

## 3. L1/L2/L3 三层噪声分析并行架构验证

### 3.1 架构说明

```
analyze(frames, frame_size, detection, original_pcm, original_n)
│
├─ L3 PCM 累积 + 分类（独立于 VAD 和 L1）
│  ├─ 输入：原始音频（YAMNet 多标签分类需完整混合音频）
│  ├─ 3s 环形缓冲（144000 样本 @48k）
│  ├─ 每 3s 触发一次 YAMNet classify()
│  └─ 结果 sticky 持久化（每帧恢复上次 L3 命中）
│
├─ L1 频域分析（始终运行，不受 VAD 门控）
│  ├─ 输入：噪声分量（降噪开启）或原始 PCM（降噪关闭）
│  ├─ FFT + Goertzel + 时域脉冲检测
│  └─ 规则式分类 -> primary_type + confidence
│
├─ L2 Bark 模板匹配（每帧）
│  ├─ 输入：32 维 Bark 频带能量
│  └ NoiseTemplateDB 余弦相似度匹配
│
└─ return result（L1 + L2 + L3 + VAD 并行上报）
```

### 3.2 关键设计决策

| 决策 | 说明 |
|------|------|
| **VAD 不门控 L1** | 降噪开启时分析的是噪声分量（纯噪声），VAD 判的是原始音频，与噪声分量无关 |
| **L3 分析原始音频** | YAMNet 多标签分类能从"语音+噪声"混合音频中识别噪声类型，白名单过滤 Speech |
| **L3 独立于 L1** | L1 和 L3 是互补维度（频域特征 vs 声源识别），并行运行互不门控 |
| **L3 sticky 持久化** | L3 每 3s 触发一次（300 帧中 1 帧），结果保存并在每帧恢复，避免瞬态丢失 |
| **L2 每帧匹配** | 32 维余弦相似度 <0.1ms，开销极小 |

### 3.3 验证结果

| 场景 | L1 | L2 | L3 | 说明 |
|------|-----|-----|-----|------|
| 白噪声 | white ✅ | 无模板 | White noise ✅ | L1+L3 互相验证 |
| 风扇噪声 | pink | 无模板 | Noise | L1 高置信时 L3 仍运行 |
| 含噪语音 | unknown | 无模板 | Pour/Liquid | L3 从原始音频识别水声 |

---

## 4. 64 路满载并发验证(仅检测)

### 4.1 测试配置

| 项目 | 值 |
|------|-----|
| Sensor 数 | 64（系统上限，sink 0-63） |
| 降噪 | 关闭（`denoise_enabled=false`） |
| 音频源 | ESC-50 数据集 64 个环境声 WAV（22 类噪声，每类 3 个文件） |
| 音频源模式 | `fake_pcm_source` 目录模式（64 文件 → 64 通道），各 channel 相位错开 |
| Sink 绑定 | sink N 配 `map:[N]` 绑定驱动 channel N（贴近真实场景） |
| L3 模型 | YAMNet（共享单实例 + mutex，局部 Resampler） |
| 采样率 | 48kHz mono s16（ESC-50 44.1kHz 经 ffmpeg 转 48k） |

测试音频覆盖 ESC-50 类别：rain/sea_waves/crackling_fire/crickets/water_drops/wind/
pouring_water/thunderstorm/washing_machine/vacuum_cleaner/clock_alarm/clock_tick/
helicopter/chainsaw/siren/engine/train/airplane/fireworks/hand_saw/toilet_flush/
glass_breaking。

### 4.2 资源占用

| 指标 | 值 |
|------|-----|
| **CPU** | 547%（16 核，约 34%） |
| **内存 (RSS)** | 287 MB |
| **稳定性** | 无崩溃，3 轮 192 次查询全部返回 |

### 4.3 检测结果统计

| 指标 | 值 |
|------|-----|
| **不同 score 数** | **64/64（100% 独立）** |
| **L3 类型多样性** | 21-25 种（3 轮） |
| **L1 类型多样性** | 2-4 种（unknown/hum_50hz/hum_60hz/broadband/impulse） |
| **L3 非空率** | 58/64（91%，6 路低于 min_score 阈值） |

> L1 规则经 §4.7 守卫优化后更保守（unknown 占多数），这是诚实结论：
> L1 定位"信号形态分类"（White/Pink/Hum/Broadband），ESC-50 真实环境声
> 多为谐波/瞬态结构不属于这 6 种形态；声源识别交给 L3 YAMNet（21+ 类型）。

### 4.4 L3 类型分布（典型轮次）

| 类型 | 次数 | ESC-50 对应 | 命中合理性 |
|------|------|------------|-----------|
| Water（水声） | 11 | rain/sea_waves/pouring_water/water_drops | ✓ 合理 |
| Engine（引擎） | 9 | engine/train/chainsaw/vacuum_cleaner | ✓ 合理 |
| Alarm（警报） | 4 | clock_alarm/siren | ✓ 合理 |
| Rattle（咔嗒） | 3 | crickets/clock_tick | ✓ 合理 |
| Liquid（液体） | 3 | water_drops/toilet_flush | ✓ 合理 |
| Jet engine（喷气引擎） | 3 | helicopter/vacuum_cleaner | ✓ 合理 |
| Siren（警笛） | 2 | siren | ✓✓ 精确 |
| Wind（风声） | 2 | wind/thunderstorm | ✓ 合理 |
| Outside, rural/natural | 3 | fireworks/airplane | △ 近似 |
| 其他（Crackle/Motor vehicle 等） | 数个 | 各类 | ✓ 合理 |

> 64 路结果完全独立（64/64 不同 score），L3 覆盖 21+ 种类型，与 ESC-50
> ground truth 高度吻合。此前"全部 Water"的假象是 channel_map 解复用 bug
> 所致（详见 §4.6）。

### 4.5 逐路结果示例（L1 优化后）

```
SID | ESC-50 类别      | L1        | L3                      score
--------------------------------------------------------------------
  0 | rain             | unknown   | Water                   0.774
  9 | crickets         | unknown   | Rattle                  0.539
 12 | water_drops      | impulse   | Liquid                  0.243
 27 | vacuum_cleaner   | unknown   | Jet engine              0.353
 42 | siren            | unknown   | Siren                   0.622
 45 | engine           | unknown   | Motor vehicle (road)    0.081
 48 | train            | hum_50hz  | Motor vehicle (road)    0.076
 54 | fireworks        | unknown   | Thump, thud             0.100
 63 | glass_breaking   | unknown   | Clang                   0.064
```

> L1 优化前 crickets 误判 digital、vacuum_cleaner/engine 误判 pink；
> 优化后这些误判消除（unknown 是诚实结论，声源由 L3 识别）。L3 与 ESC-50
> ground truth 高度吻合（rain→Water, siren→Siren, vacuum_cleaner→Jet engine,
> crickets→Rattle, water_drops→Liquid 等）。

### 4.6 修复的并发验证阻断 bug

64 路验证暴露并修复了 5 个使 FAKE 测试与真实场景行为不一致的 bug：

1. **`fake_capture_loop` 不读 config 通道数**：硬用 `test_channels_`（默认 2），
   配 `streamer_channels=64` 仍只分发 2 通道。修复：优先用 config 值
   （`config_->get_streamer_channels()`），与真实 `capture_loop` 一致。
2. **`get_sink_channel_map` 宏守卫错误**：用 `#ifndef _USE_FAKE_DRIVER_` 跳过
   SessionManager 查询，但生产 FAKE daemon 也定义该宏 → channel_map 查询被
   跳过，所有 sink 兜底 channel 0 → 64 路听到同一音频。修复：改用
   `_HAS_SESSION_MANAGER_LINK_` 宏（仅生产 aes67-daemon 定义，noise-test 不
   定义），运行时 `session_manager_` 判空兜底。
3. **`streamer_channels` config 校验过严**：上限 16（上游 AAC streamer 限制），
   64 路配置被强制回退 8。修复：上限放宽到 64（RAVENNA 驱动支持 64 通道）。
4. **`kMaxChannels` 容量不足**：bridge 解复用缓冲按 `kMaxChannels=8` 分配，
   64 通道 period 超容量被整体丢弃。修复：`kMaxChannels=64`。
5. **MlClassifier 共享 Resampler 状态污染**：单实例 `downsample` Resampler
   被 64 路交叉调用，SpeexDSP 滤波状态残留导致分类失真。修复：`classify()`
   改用局部 Resampler（无状态残留），每次独立。

### 4.7 L1 规则式分类优化（64 路验证驱动）

64 路 ESC-50 验证暴露 L1 规则对真实环境声的误判，做 3 处守卫优化：

| 规则 | 优化前 | 优化后 | 效果 |
|------|--------|--------|------|
| **Pink** | ±3dB 容差，无 SF 守卫，无 R² | ±2dB + SF≥0.2 守卫 + R² 软降(/0.4) | 21→0 误判（rain/engine/siren 等低 SF 自然声不再误判） |
| **Digital** | 高频能量比>0.5 即报 | + 高频平坦度硬门槛(≥0.15) | 4→0 误判（crickets 谐波尖峰不再误判数字噪声） |
| **Broadband** | 与 Pink 重叠抢首位 | Pink 候选 conf>0.3 时跳过 Broadband | 粉噪正确归 Pink（合成粉噪 pink 0.923） |

**优化前后对照**（64 路 ESC-50，3 轮稳定）：

| L1 类型 | 优化前 | 优化后 |
|---------|--------|--------|
| unknown | 38 | 56-61 |
| pink | 21（误判） | 0 |
| digital | 4（误判） | 0 |
| hum/broadband/impulse | 1 | 3-5（合理偶发） |

**合成噪声验证**（规则仍正确识别）：白噪→white(0.61)，粉噪→pink(0.92)。

> 优化后 L1 更保守（unknown 增多），但这是正确的：L1 定位为"信号形态
> 分类"（White/Pink/Hum/Broadband），声源识别交给 L3 YAMNet（21+ 类型）。
> ESC-50 真实环境声多为谐波/瞬态结构，不属于 L1 的 6 种形态，unknown 是
> 诚实结论而非误判。

---

## 5. L3 YAMNet 性能基准

### 5.1 单次 classify() 耗时

**64 路满载并发实测**（16 核，198 次采样）：

| 步骤 | 平均耗时 | 说明 |
|------|----------|------|
| 重采样 48k -> 16k | ~1 ms | SpeexDSP，144000 -> 48000 样本（局部 Resampler） |
| ONNX Run | ~19.6 ms | YAMNet 前向推理（CPUExecutionProvider） |
| scores 均值 + 白名单过滤 | ~0.03 ms | 6×521 均值 + 48 类过滤 + 排序 |
| **总计** | **~21 ms**（中位 19.3ms，范围 17.4-42ms） | 198 次平均 |

> 单路无负载时 ONNX Run 仅 ~3.7ms（Python 实测），但 64 路满载并发时
> ONNX Runtime 内部多线程与 daemon 其他线程（L1/降噪/capture）争抢 CPU，
> ONNX Run 升至 ~19.6ms。这是并发负载下的真实耗时。

### 5.2 并发影响

- MlClassifier 是共享单实例（1 个 ONNX session，省内存）
- `classify()` 用 `classify_mutex_` 保护 ONNX Run（Ort::Session 非线程安全）
- **重采样改用局部 Resampler**（非成员）：此前成员 `downsample` 的 SpeexDSP
  滤波状态被 64 路交叉调用污染（sensor A 残留混入 B 输出），导致分类失真
  偏向 Water。局部构造无状态残留，3s 一次批处理构造开销可忽略（<<1ms）
- 64 路调 `classify()` 时 ONNX Run 串行（mutex），每路每 3s 触发一次（300 帧
  中 1 帧），实际同时触发概率低；最坏情况 64 路同时触发：64 × 21ms ≈ 1.3s
  串行等待，但 YAMNet 结果 sticky 持久化（3s 内复用上次结果），不影响实时性

### 5.3 L3 独立性

每次 `classify()` 完全独立，无前后依赖：
- 输入：环形缓冲中最新 3s PCM（144000 样本 @48k）
- 不依赖上一次结果
- 局部 Resampler 无跨调用状态（SpeexDSP 滤波器每次全新构造）
- 64 路并发验证：64/64 不同 score，21+ 种 L3 类型，确认独立性

---

## 6. 并发能力总结

| 模式 | 并发上限 | CPU 占用 | 说明 |
|------|---------|---------|------|
| **仅检测/分析**（无降噪） | **64 路**（系统硬上限） | ~547%（16核 34%） | VAD + FFT + L1 规则 + L2 模板 + L3 YAMNet(每3s) |
| **降噪开启**（RNNoise/DTLN） | 4-8 路（参考） | - | 加 ONNX 降噪推理 1-3ms/帧 |
| **降噪开启**（DeepFilterNet） | 2-4 路（参考） | - | 三子图推理 ~7ms/帧 |

> 无降噪模式下 64 路全部跑到系统硬上限（AES67 Sink 上限 64），CPU 仍有余量
> （16 核仅用 34%），可支持更高并发（受限于 64 路 Sink 上限）。

---

## 7. 噪声类别白名单

YAMNet 521 类中筛选 48 个噪声相关类别（排除语音/音乐/动物）：

| 类别 | AudioSet Index | 中文 |
|------|---------------|------|
| Humming | 32 | 嗡鸣 |
| Hiss | 79 | 嘶嘶声 |
| Roar | 105 | 轰鸣 |
| Buzz | 125 | 蜂鸣 |
| Rattle | 130 | 咔嗒声 |
| Wind | 277 | 风声 |
| Wind noise (microphone) | 279 | 麦克风风噪 |
| Water | 282 | 水声 |
| Stream | 286 | 溪流声 |
| Crackle | 293 | 爆裂声 |
| Motor vehicle (road) | 300 | 机动车 |
| Motorcycle | 320 | 摩托车 |
| Traffic noise, roadway noise | 321 | 交通噪声 |
| Train | 323 | 火车 |
| Aircraft engine | 330 | 飞机引擎 |
| Jet engine | 331 | 喷气引擎 |
| Engine | 337 | 引擎 |
| Light engine (high frequency) | 338 | 轻型引擎（高频） |
| Medium engine (mid frequency) | 342 | 中型引擎（中频） |
| Heavy engine (low frequency) | 343 | 重型引擎（低频） |
| Engine knocking | 344 | 引擎爆震 |
| Knock | 353 | 敲击 |
| Squeak | 355 | 吱吱声 |
| Vacuum cleaner | 371 | 吸尘器 |
| Alarm | 382 | 警报 |
| Siren | 390 | 警笛 |
| Buzzer | 392 | 蜂鸣器 |
| Mechanical fan | 406 | 机械风扇 |
| Air conditioning | 407 | 空调 |
| Burst, pop | 428 | 爆裂、砰声 |
| Crack | 434 | 断裂声 |
| Liquid | 438 | 液体声 |
| Pour | 443 | 倾倒声 |
| Trickle, dribble | 444 | 滴漏声 |
| Pump (liquid) | 448 | 泵（液体） |
| Thump, thud | 454 | 沉闷撞击 |
| Scrape | 469 | 刮擦声 |
| Clang | 478 | 哐当声 |
| Whir | 482 | 旋转嗡声 |
| Clicking | 485 | 咔哒声 |
| Rumble | 487 | 隆隆声 |
| Hum | 490 | 嗡嗡声 |
| Outside, rural or natural | 504 | 户外/自然环境声 |
| Noise | 507 | 噪声（通用） |
| Environmental noise | 508 | 环境噪声 |
| Static | 509 | 静电噪声 |
| Mains hum | 510 | 工频哼声 |
| White noise | 514 | 白噪声 |
| Pink noise | 515 | 粉红噪声 |

最低报告分数：0.05（score < 0.05 的类别不报告）

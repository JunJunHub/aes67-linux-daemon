# 噪声分析检测报告

> **版本**: v1.0
> **日期**: 2026-07-29
> **环境**: FAKE_DRIVER 模式，noise-dev.sh 构建（WITH_NOISE=ON, WITH_STREAMER=ON）
> **模型**: YAMNet yamnet_3s.onnx（15MB，ONNX Runtime 1.27.0）
> **关联**: [架构设计](architecture-design.md) §3.3、[YAMNet 迁移计划](../superpowers/plans/yamnet-migration-plan.md)

---

## 1. 测试环境

| 项目 | 配置 |
|------|------|
| OS | Linux 7.0.0-28-generic |
| CPU | 8 核 |
| 内存 | 8.7 GB |
| 构建方式 | out-of-source（daemon/build/） |
| 编译选项 | FAKE_DRIVER=ON, WITH_NOISE=ON, WITH_STREAMER=ON, WITH_AVAHI=OFF |
| 模型目录 | `/tmp/noise_models_proper/`（子目录布局） |

### 模型目录布局

```
noise_models/
├── dtln/{model_1.onnx, model_2.onnx}
├── deepfilternet/{enc.onnx, df_dec.onnx, erb_dec.onnx}
└── yamnet/{yamnet_3s.onnx, yamnet_class_map.csv}
```

adapter 路径推导：`onnx_model_dir` 指向父目录，各 adapter 先找 `<dir>/<plugin_name>/` 子目录，再回退 `<dir>/` 平铺。

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

## 3. L1/L2/L3 三层并行架构验证

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

## 4. 64 路满载并发验证

### 4.1 测试配置

| 项目 | 值 |
|------|-----|
| Sensor 数 | 64（系统上限，sink 0-63） |
| 降噪 | 关闭（`denoise_enabled=false`） |
| 音频源 | VoiceBank-DEMAND p232_023.wav（含噪语音，循环播放） |
| L3 模型 | YAMNet（共享单实例 + mutex） |
| 查询轮次 | 每路查询 3 次，间隔 2s |

### 4.2 资源占用

| 指标 | 值 |
|------|-----|
| **CPU** | 537%（8 核约 67%） |
| **内存 (RSS)** | 250 MB |
| **线程数** | 86（64 per-sink + 控制/HTTP/housekeeper） |
| **稳定性** | 无崩溃，192 次查询全部返回 |

### 4.3 检测结果统计

| 指标 | 值 |
|------|-----|
| **总查询** | 192 次（0 错误） |
| **L1 分类 (conf>0)** | 0/192（0%） |
| **L3 分类 (非空)** | **192/192（100%）** |

### 4.4 L3 类型分布

| 类型 | 次数 | 占比 | 说明 |
|------|------|------|------|
| Pour（倾倒声） | 111 | 58% | 水声/液体倾倒 |
| Trickle, dribble（滴漏声） | 81 | 42% | 水滴/细流 |

两类都是水声相关，符合 VoiceBank-DEMAND p232_023 的音频特征。不同 sensor 的 L3 类型差异来自 YAMNet 触发时刻不同（3s 窗口落在音频不同位置）。

### 4.5 逐路结果示例

```
SID | L1         conf  | L3                   score  | speech | SF      lvl     snr
----------------------------------------------------------------------------------------
  0 | unknown    0.000 | Pour                 0.0729 | True   | 0.0078  -18.8   22.1
  3 | unknown    0.000 | Pour                 0.1021 | True   | 0.0078  -18.8   22.1
 32 | unknown    0.000 | Trickle, dribble     0.0558 | True   | 0.0078  -18.8   22.1
 48 | unknown    0.000 | Trickle, dribble     0.0558 | True   | 0.0078  -18.8   22.1
 63 | unknown    0.000 | Pour                 0.1366 | True   | 0.0078  -18.8   22.1
```

---

## 5. L3 YAMNet 性能基准

### 5.1 单次 classify() 耗时

| 步骤 | 耗时 | 说明 |
|------|------|------|
| 重采样 48k -> 16k | ~1 ms | 144000 -> 48000 样本 |
| ONNX Run | ~17 ms | YAMNet 前向推理 |
| scores 均值 + 白名单过滤 | ~1 ms | 6×521 均值 + 48 类过滤 + 排序 |
| **总计** | **~19 ms** | 10 次平均 |

### 5.2 并发影响

- MlClassifier 是共享单实例（1 个 ONNX session，省内存）
- `classify()` 用 `classify_mutex_` 保护 Resampler（有内部状态）
- 64 路调 `classify()` 时串行通过 Resampler + ONNX Run
- 每路每 3s 触发一次（300 帧中 1 帧），实际竞争极少
- 最坏情况 64 路同时触发：64 × 19ms ≈ 1.2s 串行等待（实际不会同时）

### 5.3 L3 独立性

每次 `classify()` 完全独立，无前后依赖：
- 输入：环形缓冲中最新 3s PCM（144000 样本 @48k）
- 不依赖上一次结果
- 不维护跨调用状态（Resampler 内部滤波器系数不影响结果正确性）

---

## 6. 并发能力总结

| 模式 | 并发上限 | CPU 占用 | 说明 |
|------|---------|---------|------|
| **仅检测/分析**（无降噪） | **64 路**（系统硬上限） | ~537%（8核） | VAD + FFT + L1 规则 + L2 模板 + L3 YAMNet(每3s) |
| **降噪开启**（RNNoise/DTLN） | 4-8 路（参考） | - | 加 ONNX 降噪推理 1-3ms/帧 |
| **降噪开启**（DeepFilterNet） | 2-4 路（参考） | - | 三子图推理 ~7ms/帧 |

> 无降噪模式下 64 路全部跑到系统硬上限（AES67 Sink 上限 64），CPU 仍有余量。

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

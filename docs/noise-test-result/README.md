# Noise 模块测试报告

> 测试日期：2026-07-28
> 分支：feature/noise HEAD `8718563`
> 测试人：skylark + Claude Fable 5

## 1. 测试环境

| 项 | 值 |
|----|-----|
| CPU | Intel i7-14700, 16 逻辑核（8 socket × 2 core, 1 thread/core） |
| OS | Linux 7.0.0-28-generic |
| 编译 | FAKE_DRIVER=ON, WITH_NOISE=ON, WITH_STREAMER=ON, WITH_AVAHI=ON |
| 测试集 | VoiceBank-DEMAND 824 对 clean/noisy（48kHz mono, ~1-2s） |
| 噪声样本 | DEMAND DKITCHEN（48k/16k, 16ch） |
| 评测工具 | PESQ(wb) + STOI + SNR |
| Python venv | onnxruntime/scipy/soundfile/librosa/pesq/pystoi |
| 模型 | RNNoise(内嵌 C) + DTLN(2×ONNX) + DFN(3×ONNX) |

## 2. 测试方法

### 2.1 方法 A：直接测降噪模型（noise_bench 离线工具）

**目的**：绕过 daemon 网络层，纯算法评测降噪质量和性能。

**工具**：`daemon/noise/tools/noise_bench.cpp`（CMake target `noise-bench`）

**原理**：

```
WAV(int16) -> float[-1,1] -> adapter.process(480样本/帧) -> denoised float -> WAV(int16)
                                       ↓
                              计时 -> RTF = 处理时间 / 音频时长
```

**用法**：

```bash
# 单文件处理（写 denoised WAV）
./noise-bench --plugin <rnnoise|dtln|deepfilternet> \
              --input <noisy.wav> --output <denoised.wav> \
              [--model-dir <dir>] [--dry-wet <0-1>]

# 多线程并发测试（不写 output）
./noise-bench --plugin <name> --input <wav> \
              --model-dir <dir> --threads <N>
```

**模型路径**：
- RNNoise：无需 `--model-dir`（内嵌编译）
- DTLN：`--model-dir` 指向含 `model_1.onnx` + `model_2.onnx` 的目录
- DFN：`--model-dir` 指向含 `enc.onnx` + `df_dec.onnx` + `erb_dec.onnx` 的目录

**评测脚本**：`daemon/noise/tools/noise_bench_eval.py`

```bash
source .venv/bin/activate
python3 noise/tools/noise_bench_eval.py \
    --bench ./build/noise-bench \
    --dataset /path/to/VoiceBank-DEMAND \
    --model-dir ../noise_models \
    --num-files 30
```

脚本流程：
1. 从 VoiceBank-DEMAND 均匀采样 N 对 clean/noisy 文件
2. 用 noise-bench 分别跑 3 个插件处理 noisy 文件
3. 用互相关自动对齐 denoised 和 clean（补偿算法延迟）
4. 计算 PESQ(wb) / STOI / SNR
5. 输出对比表格 + JSON

**关键注意**：
- 输出长度对齐：adapter 有算法延迟，输出可能 ≠ 输入长度。noise_bench 截断/补零到输入长度
- 互相关对齐：`scipy.signal.correlate(clean, denoised)` 的 lag<0 表示 denoised 延迟 |lag|，应对齐 `denoised = denoised[-lag:]`（去前 |lag| 个），不是 `denoised[:lag]`
- PESQ 仅支持 8k/16k：48k 音频需 `scipy.signal.resample_poly(x, 1, 3)` 降采样到 16k

### 2.2 方法 B：fake 全链路测试（daemon + HTTP API）

**目的**：验证完整 daemon 流程（PCM 分发 → 降噪 → 噪声分析 → 告警）。

**配置**：生成临时 daemon.conf，设置 `fake_pcm_source` 指向 noisy WAV 文件：

```json
{
  "http_port": 9999,
  "interface_name": "lo",
  "streamer_enabled": true,
  "onnx_model_dir": "/path/to/merged_models",
  "fake_pcm_source": "/path/to/noisy.wav",
  ...
}
```

> **onnx_model_dir 注意**：DTLN 期望 `<dir>/model_1.onnx`，DFN 期望 `<dir>/enc.onnx`。所有模型文件需放在同一目录（可用符号链接合并子目录）。

**启动**：

```bash
./build/aes67-daemon -c /tmp/noise-e2e-test.conf -p 9999
```

PcmCaptureService 的 `fake_capture_loop` 会循环播放 `fake_pcm_source` 指定的 WAV 文件，逐 period 分发给 NoiseManager。

**HTTP API 操作**：

```bash
# 添加 sensor 0 + 配置降噪插件
curl -X PUT http://127.0.0.1:9999/api/noise/sensor/0 \
     -H "Content-Type: application/json" \
     -d '{"sink_id":0,"denoise_enabled":true,"denoise_plugin":"dtln","dry_wet":1.0}'

# 等待 10s 收敛后查询指标
curl http://127.0.0.1:9999/api/noise/sensor/0/metrics

# 查询所有 sensor 状态
curl http://127.0.0.1:9999/api/noise/sensors

# 切换插件
curl -X PUT http://127.0.0.1:9999/api/noise/sensor/0 \
     -H "Content-Type: application/json" \
     -d '{"sink_id":0,"denoise_enabled":true,"denoise_plugin":"rnnoise","dry_wet":1.0}'
```

**关键注意**：
- `noise_reduction_db` 是**瞬时值**（每帧更新），语音段 ≈0dB、噪声间隙段 70-90dB。单次查询可能命中极端帧，需多次采样观察分布
- `switch_plugin` 有 6 帧（2880 样本）静音过渡窗（`mute_remaining`），切换后前 6 帧 denoised=0
- FAKE_DRIVER 模式下 PTP 永远 "unlocked"，需手动 `on_ptp_locked()` 使 pipeline 运行（已修复）

### 2.3 两种方法对比

| 维度 | 方法 A（noise_bench） | 方法 B（daemon 全链路） |
|------|----------------------|----------------------|
| 测试范围 | 纯算法（adapter only） | 完整流程（PCM→降噪→分析→告警） |
| 评测精度 | 高（PESQ/STOI 对齐计算） | 低（瞬时 metrics，无 clean 参考） |
| 并发测试 | ✅ `--threads N` | 需多实例 |
| 噪声识别 | ❌ 不涉及 | ✅ NoiseDetector + L3 ML |
| 告警引擎 | ❌ 不涉及 | ✅ alert_level 升降级 |
| 环境依赖 | 仅编译 + Python venv | daemon 完整启动 |

## 3. 测试结果

### 3.1 降噪质量对比（30 对 VoiceBank-DEMAND，方法 A）

| Metric | Noisy | RNNoise | DTLN | DFN |
|--------|-------|---------|------|-----|
| PESQ | 2.114 ± 0.725 | 2.289 ± 0.646 | **2.576 ± 0.549** | 1.033 ± 0.011 ⚠️ |
| STOI | 0.930 ± 0.072 | 0.907 ± 0.074 | **0.929 ± 0.063** | 0.586 ± 0.044 ⚠️ |
| SNR(dB) | 8.527 ± 5.467 | 13.128 ± 2.632 | **16.548 ± 3.509** | 0.512 ± 0.129 ⚠️ |
| RTF | - | 0.705 ± 0.007 | **0.067 ± 0.002** | 0.566 ± 0.014 |

**结论**：
- **DTLN 质量最优**：PESQ +0.46，SNR +8.0dB，STOI 几乎无损
- **RNNoise 中规中矩**：PESQ +0.18，SNR +4.6dB，STOI 轻微下降
- **DFN 待修复**：wnorm_ 双重缩放修复后不再静音，但 ERB mask / non-causal deep-filter 仍有问题

### 3.2 并发性能（16 核 CPU，方法 A `--threads N`）

| 插件 | 1线程 | 2线程 | 4线程 | 8线程 | 12线程 | 16线程 | 最大并发 |
|------|-------|-------|-------|-------|--------|--------|---------|
| RNNoise | 1.4x | 2.8x | 5.2x | 9.1x | 11.6x | 13.4x | ~13 路 |
| DTLN | 14.8x | 23.4x | 40.3x | 64.8x | 77.4x | 76.4x | **~76 路** |
| DFN | 1.8x | 3.2x | 6.0x | 10.4x | 12.6x | 13.9x | ~14 路 |

> Throughput = 1/Concurrent RTF，表示相对 real-time 的倍数。
> 实际 daemon 运行时并发路数约为离线 RTF 推算的 60-70%（PCM 分发 + 噪声识别 + HTTP 开销）。

**结论**：
- **DTLN 并发最强**：16k 域处理 + 轻量模型，12 线程即饱和
- **RNNoise/DFN 类似**：16 线程 ~14 路饱和

### 3.3 daemon 完整流程验证（方法 B）

#### 三插件 daemon 运行 metrics（单次查询）

| Metric | RNNoise | DTLN | DFN |
|--------|---------|------|-----|
| noise_level_dbfs | -42.0 | -20.5 | -14.1 |
| noise_reduction_db | 84.0 * | -13.1 * | 21.3 |
| spectral_centroid_hz | 4562 | 4095 | 3491 |
| spectral_flatness | 0.026 | 0.023 | 0.011 |
| hum_strength_db | 8.4 | 0 | -0.8 |
| estimated_snr_db | 10.4 | 18.8 | 38.5 |
| alert_level | warning | critical | critical |

> \* `noise_reduction_db` 为瞬时值，单次查询命中噪声间隙段（RNNoise）或语音段（DTLN）导致极端值。多次采样确认正常波动：语音段 ≈0-2dB，噪声间隙段 70-90dB。

#### 功能验证状态

| 功能 | 状态 | 说明 |
|------|------|------|
| PCM 分发 | ✅ | fake_pcm_source 循环播放 → bridge S16→float → NoiseManager |
| 降噪处理 | ✅ | 三插件均正常运行，瞬时 noise_reduction 正常波动 |
| 频域分析 | ✅ | spectral_centroid / flatness / hum 均有合理值 |
| 告警引擎 | ✅ | warning / critical 升降级正常 |
| 噪声类型识别 | ⚠️ | 始终 unknown, confidence=0 |
| L3 ML 分类 | ❌ | ml_model_path 为空，VGGish 未加载 |

## 4. 发现并修复的生产 bug

### Bug 1：DTLN up_contig_ 缓冲溢出（`607e5a4`）

**现象**：DTLN 输出量 >> 输入量（132480 vs 83582 样本），输出包含重复旧数据导致失真。PESQ=1.074, SNR=-3.0dB。

**根因**：`up_->process()` 接收 `up_contig_.size()` 作为输入长度，但 `up_contig_` 只 resize 增长不缩小。当 `out_fifo16_` 从 256 缩回 128 时，`up_contig_.size()` 仍为 256，导致 up_ 处理了 128 新样本 + 128 旧样本。

**修复**：保存 `out_fifo16_.size()` 到 `in16_size`，用 `in16_size` 传给 `up_->process()`。

**效果**：PESQ 1.074 → **2.576**，SNR -3.0 → **16.5 dB**。

### Bug 2：DFN ISTFT 双重缩放（`e6ca4b1`）

**现象**：DFN 输出几乎静音（max=0.0003），降噪 71dB（把语音也消除）。lsnr=-6.98。

**根因**：`wnorm_=1/N` 在 STFT 缩小频谱 N 倍，`fft.hpp` 的 `Irfft` 含 1/N 归一化又缩小 N 倍，总计缩小 N²=921600 倍。原始 libdf 的 FFT/IFFT 不含归一化，`wnorm_=1/N` 单次缩放平衡。

**修复**：ISTFT 后 `time_block_[i] *= window_[i] * kFft`（乘 N 补偿 IFFT 的 1/N）。

**效果**：max=0.0003 → **0.296**，不再静音。PESQ 仍低（1.033），DFN 还有 ERB mask / non-causal deep-filter 的实现问题待修。

### Bug 3：FAKE_DRIVER ptp_locked_ 永远 false（`8718563`）

**现象**：daemon 中 noise pipeline 完全不运行。noise_level_dbfs=-120, spectral_centroid=0, noise_reduction=0。

**根因**：PcmCaptureService 创建时立即收到 PTP "unlocked"（fake driver）并启动 fake_capture_loop，但 `ptp_status_forward_cb_` 此时未设置（main.cpp 中 `set_ptp_status_forward_callback` 在 `PcmCaptureService::create` 之后），"locked" 未转发到 NoiseManager。`ptp_locked_` 保持 false → `on_frame` 首行 `if (!ptp_locked_) return;` 短路 → 整个 pipeline 死。

**修复**：`set_ptp_status_forward_callback` 后手动调 `noise_manager->on_ptp_locked()`（仅 `#ifdef _USE_FAKE_DRIVER_`）。

**效果**：noise_level -120 → **-20.5**, spectral_centroid 0 → **4095 Hz**。

## 5. 待解决问题

| # | 问题 | 优先级 | 方向 |
|---|------|--------|------|
| 1 | **DFN 降噪质量差**（PESQ 1.033） | 高 | ERB mask 值 0.27 可能偏低；non-causal deep-filter coefs 映射需对比 DeepFilterNet Python 参考实现 |
| 2 | 噪声类型识别 unknown | 中 | VoiceBank-DEMAND 噪声特征可能不在 NoiseDetector 分类范围；需调参或扩展分类 |
| 3 | L3 ML 分类未工作 | 中 | 需配置 `ml_model_path` 加载 VGGish 模型 |
| 4 | noise_reduction 瞬时值波动 | 低 | 可选改进：滑动平均或 VAD 门控 |

## 6. 综合结论

| 维度 | RNNoise | DTLN | DFN |
|------|---------|------|-----|
| 降噪质量 | 中（PESQ +0.18） | **优（PESQ +0.46）** | 差⚠️（待修） |
| 并发性能 | 13 路 | **76 路** | 14 路 |
| daemon 集成 | ✅ | ✅ | ✅ |
| 模型依赖 | 无（C 内嵌） | 2×ONNX | 3×ONNX |
| RTF | 0.705 | **0.067** | 0.566 |

**推荐**：
- **DTLN 综合最优**：质量最好 + 并发最强 + RTF 最低
- **RNNoise 适合无 ONNX 环境**：质量可接受，无外部依赖
- **DFN 待修复后潜力最大**：原始 DeepFilterNet 论文优于 DTLN，当前实现有 ERB mask / deep-filter 问题

## 7. 本次提交清单（feature/noise 分支）

| Commit | Type | 说明 |
|--------|------|------|
| `e751bb6` | refactor | E cosmetic：const_cast 消除 + save_nolock_ DRY + alert_level_to_string DRY |
| `f3f8666` | test | D 测试质量：mallinfo2 heap 检测 + CSV 数据验证 |
| `75a5be0` | feat | C D-S5.5：ONNX 失败 memcpy passthrough（dry_wet 降级替代 silence） |
| `607e5a4` | fix | DTLN up_contig_ 缓冲溢出修复 |
| `e6ca4b1` | fix | DFN ISTFT 双重缩放修复 |
| `8718563` | fix | FAKE_DRIVER ptp_locked_ 永远 false 修复 |

新增工具：
- `daemon/noise/tools/noise_bench.cpp`：降噪质量 + 并发性能基准工具
- `daemon/noise/tools/noise_bench_eval.py`：VoiceBank-DEMAND 批量 PESQ/STOI 评测脚本

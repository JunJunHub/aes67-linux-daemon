// daemon/noise/noise_analyzer.cpp
// L1 规则式噪声分类 + Bark 频带 + 逐帧 FrameFeatures 环形缓冲实现。
// 架构依据:docs/noise/architecture-design.md §3.3 L461-627。
//
// FFT 复用说明(arch §3.3.6 L597 "kiss_fft / pffft"):
// 本文件内嵌 512 点 radix-2 Cooley-Tukey FFT,与 Task 2 的 NoiseDetector
// 各自独立持有 FFT 实例(组件级隔离)。DRY 偏差记为 Minor,未来可提取
// 共享 daemon/noise/fft.hpp(不在本 task 范围内,避免重开 Task 2 review)。
#include "noise_analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <numeric>
#include <vector>

#include "ml_classifier.hpp"  // L3 ML 分类（YAMNet）

namespace noise {

namespace {
// FFT 点数(radix-2,480 样本零填充到 512)。
constexpr size_t kFftSize = 512;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kLogEps = 1e-20f;
constexpr uint32_t kDefaultSampleRate = 48000;
constexpr float kDefaultFrameSize = 480.0f;

// 3-tap 功率谱平滑窗口(与 Task 2 NoiseDetector 一致)。
constexpr size_t kSmoothRadius = 1;

// 1/3 倍频程 Bark 频带边缘(33 个边缘 -> 32 个频带,覆盖 20Hz-32kHz)。
// 采用 IEC 61260 标准中心频率作为边缘(简化:每个频带从前一中心到下一中心)。
// 这是 L2 模板匹配的特征向量(Task 6 消费)。
constexpr float kBandEdges[33] = {
    20.0f,    25.0f,    31.5f,    40.0f,    50.0f,   63.0f,   80.0f,
    100.0f,   125.0f,   160.0f,   200.0f,   250.0f,  315.0f,  400.0f,
    500.0f,   630.0f,   800.0f,   1000.0f,  1250.0f, 1600.0f, 2000.0f,
    2500.0f,  3150.0f,  4000.0f,  5000.0f,  6300.0f, 8000.0f, 10000.0f,
    12500.0f, 16000.0f, 20000.0f, 25000.0f, 32000.0f};

// 512 点 radix-2 Cooley-Tukey FFT(迭代 in-place)。
// 与 Task 2 NoiseDetector 的 FFT 实现一致(各组件独立持有)。
void fft_radix2(std::vector<std::complex<float>>& a) {
  const size_t n = a.size();
  // 位反转排列
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j)
      std::swap(a[i], a[j]);
  }
  // 蝶形运算
  for (size_t len = 2; len <= n; len <<= 1) {
    float ang = -2.0f * kPi / static_cast<float>(len);
    std::complex<float> wlen(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<float> w(1.0f, 0.0f);
      for (size_t j = 0; j < len / 2; ++j) {
        std::complex<float> u = a[i + j];
        std::complex<float> v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        w *= wlen;
      }
    }
  }
}

// 频谱平坦度(arch §3.2 L421-429,与 Task 2 NoiseDetector 一致)。
// SF = geo_mean(|X|²) / arith_mean(|X|²),使用正频 bins(k=1..N/2-1)。
// 3-tap 平滑降低单 bin 方差(白噪 SF 趋近 1)。
float compute_spectral_flatness(const std::vector<std::complex<float>>& X) {
  const size_t n_half = X.size() / 2;
  if (n_half < 2)
    return 0.0f;

  const size_t num_bins = n_half - 1;  // bins 1..n_half-1
  std::vector<float> power(num_bins);
  for (size_t k = 1; k < n_half; ++k) {
    power[k - 1] = std::norm(X[k]);
  }

  // 3-tap moving average(margin bins 用对称延拓)
  if (num_bins >= 3 && kSmoothRadius > 0) {
    std::vector<float> smoothed(num_bins);
    for (size_t k = 0; k < num_bins; ++k) {
      float sum = power[k];
      int cnt = 1;
      if (k > 0) {
        sum += power[k - 1];
        ++cnt;
      }
      if (k + 1 < num_bins) {
        sum += power[k + 1];
        ++cnt;
      }
      smoothed[k] = sum / static_cast<float>(cnt);
    }
    power.swap(smoothed);
  }

  float sum_log = 0.0f;
  float sum_power = 0.0f;
  for (size_t k = 0; k < num_bins; ++k) {
    sum_log += std::log(power[k] + kLogEps);
    sum_power += power[k];
  }
  if (num_bins == 0 || sum_power <= 0.0f)
    return 0.0f;

  float geo_mean = std::exp(sum_log / static_cast<float>(num_bins));
  float arith_mean = sum_power / static_cast<float>(num_bins);
  float sf = geo_mean / arith_mean;
  return std::clamp(sf, 0.0f, 1.0f);
}

// 在特定频率上计算 DFT 能量(Goertzel 等价,用于工频哼声检测)。
// 返回 |X(f)|² / N(归一化功率)。比 FFT bin 更精确(N=480 时 FFT 分辨率
// 93.75Hz,无法区分 50Hz 与 60Hz)。
float dft_power_at_freq(const float* x,
                        size_t n,
                        float freq,
                        float sample_rate) {
  std::complex<float> X(0.0f, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    float phase = -2.0f * kPi * freq * static_cast<float>(i) / sample_rate;
    X += std::complex<float>(x[i] * std::cos(phase), x[i] * std::sin(phase));
  }
  return std::norm(X) / static_cast<float>(n);
}

// 将 float clamp 到 [lo, hi]。
inline float clampf(float v, float lo, float hi) {
  return std::clamp(v, lo, hi);
}

// Spec3 Task 5：Bark 频带累加（DRY 提取，供 analyze() 与
// compute_bark_spectrum() 共享同一频带映射逻辑）。
// 将 FFT 正频功率谱 power（bins 1..fft_n/2-1，索引 0..num_bins-1）累加到
// 32 维 Bark 频带能量数组 out（accumulate 语义：不清零，调用方负责初始化）。
// 与 analyze() 原内联循环完全等价（同 kBandEdges + 同 bin 分配逻辑）。
void accumulate_bark_bands(const std::vector<float>& power,
                           size_t fft_n,
                           float sample_rate,
                           std::array<float, 32>& out) {
  const size_t n_half = fft_n / 2;
  const float bin_freq = sample_rate / static_cast<float>(fft_n);
  for (size_t k = 1; k < n_half; ++k) {
    float freq_hz = static_cast<float>(k) * bin_freq;
    for (size_t b = 0; b < 32; ++b) {
      if (freq_hz >= kBandEdges[b] && freq_hz < kBandEdges[b + 1]) {
        out[b] += power[k - 1];
        break;
      }
    }
  }
}
}  // namespace

// out-of-line 构造/析构。ml_classifier_ 的 shared_ptr 成员需完整类型析构
// （前向声明在头），定义在 .cpp（此处 include 全定义）。
NoiseAnalyzer::NoiseAnalyzer() = default;
NoiseAnalyzer::~NoiseAnalyzer() = default;

void NoiseAnalyzer::set_ml_classifier(std::shared_ptr<MlClassifier> ml) {
  ml_classifier_ = std::move(ml);
}

NoiseAnalysisResult NoiseAnalyzer::analyze(
    const float* frames,
    size_t frame_size,
    const NoiseDetectionResult& detection) {
  NoiseAnalysisResult result{};
  result.primary_type = NoiseType::Unknown;
  result.primary_confidence = 0.0f;
  result.is_mixed = false;
  result.noise_level_dbfs = -120.0f;
  result.spectral_centroid_hz = 0.0f;
  result.spectral_flatness = 0.0f;
  result.hum_strength_db = 0.0f;
  result.impulse_count = 0.0f;
  result.band_energy.fill(0.0f);
  // L3 字段：恢复上次 L3 命中结果（持久化，避免 1/300 帧瞬态被覆盖）。
  // maybe_run_l3_ 触发时更新 last_l3_*；未触发时恢复上次结果。
  if (l3_has_result_) {
    result.ml_noise_type = last_l3_type_;
    result.ml_noise_score = last_l3_score_;
    result.ml_top_types = last_l3_top_types_;
    result.noise_type_source = "l3";
  } else {
    result.ml_noise_type.clear();
    result.ml_noise_score = 0.0f;
    result.ml_top_types.clear();
    result.noise_type_source = "l1";
  }

  if (frame_size == 0 || frames == nullptr)
    return result;

  // L3 PCM 累积 + 分类：独立于 VAD 和 L1。
  // YAMNet 多标签分类能处理混合音频（语音+噪声），不受 VAD 门控。
  // L3 与 L1 是互补维度（频域特征 vs 声源识别），并行运行互不门控。
  maybe_run_l3_(result, frames, frame_size);

  // arch §3.3.1:分析 OriginalPCM(降噪关闭)时,需 VAD 过滤语音段,仅在
  // 非语音段做频谱分析。speech 帧不是噪声 -> 跳过 L1 分析。
  // L3 已在上面处理（PCM 累积不受 VAD 影响）。
  if (detection.is_speech) {
    return result;
  }

  const float sample_rate = static_cast<float>(kDefaultSampleRate);

  // ── RMS + 噪声级 (dBFS) ─────────────────────────────────────────────
  float sum_sq = 0.0f;
  for (size_t i = 0; i < frame_size; ++i) {
    sum_sq += frames[i] * frames[i];
  }
  float rms = std::sqrt(sum_sq / static_cast<float>(frame_size));
  if (rms > 1e-10f) {
    result.noise_level_dbfs = 20.0f * std::log10(rms);
  }

  // ── FFT(零填充到 kFftSize) ──────────────────────────────────────────
  std::vector<std::complex<float>> X(kFftSize, std::complex<float>(0.0f, 0.0f));
  size_t copy_len = std::min(frame_size, kFftSize);
  for (size_t i = 0; i < copy_len; ++i) {
    X[i] = std::complex<float>(frames[i], 0.0f);
  }
  fft_radix2(X);

  // 功率谱(正频 bins 1..N/2-1)
  const size_t n_half = kFftSize / 2;
  const size_t num_bins = n_half - 1;
  std::vector<float> power(num_bins);
  for (size_t k = 1; k < n_half; ++k) {
    power[k - 1] = std::norm(X[k]);
  }

  // ── 频谱平坦度(3-tap 平滑,与 Task 2 一致) ─────────────────────────
  float sf_recomputed = compute_spectral_flatness(X);
  // Resolution #2 + #8:analyze() 自己重算 FFT + SF,不依赖 detection.SF。
  // 测试中 NoiseDetectionResult det; 未初始化 spectral_flatness(C++ POD
  // 默认不零初始化),detection.SF 可能是栈垃圾。Resolution #2 明确:
  // "authoritative features come from the frame's own FFT" -- 直接用
  // sf_recomputed 作为所有 SF 规则(White/Broadband)输入和 result 字段。
  // (Resolution #8 允许 max(recomputed, detection.SF) 作为备选,但因
  //  det 可能未初始化,此处采用"just recompute correctly"备选。)
  result.spectral_flatness = sf_recomputed;

  // ── 频谱质心(arch §3.3.3:加权平均频率) ─────────────────────────────
  float sum_weighted_freq = 0.0f;
  float sum_mag = 0.0f;
  for (size_t k = 1; k < n_half; ++k) {
    float mag = std::abs(X[k]);
    sum_weighted_freq += static_cast<float>(k) * mag;
    sum_mag += mag;
  }
  if (sum_mag > 1e-10f) {
    float centroid_bin = sum_weighted_freq / sum_mag;
    result.spectral_centroid_hz = centroid_bin * (sample_rate / kFftSize);
  }

  // ── Bark 32 频带能量(arch §3.3.3,1/3 倍频程) ────────────────────────
  // Spec3 Task 5：频带映射提取为共享 accumulate_bark_bands（DRY），
  // 与 compute_bark_spectrum()（WAV 模板录入路径）共用同一逻辑。
  accumulate_bark_bands(power, kFftSize, sample_rate, result.band_energy);

  // ── 工频哼声检测(Goertzel 等价:直接 DFT at 50/60/100/120/150/180 Hz)
  // arch §3.3.4:50/100Hz 倍频峰值超周围 -> conf = clamp((peak_db - 10)/20,0,1)
  // ──────────────────────────────────────────────────────────────────────
  float e50 = dft_power_at_freq(frames, frame_size, 50.0f, sample_rate);
  float e60 = dft_power_at_freq(frames, frame_size, 60.0f, sample_rate);
  float e100 = dft_power_at_freq(frames, frame_size, 100.0f, sample_rate);
  float e120 = dft_power_at_freq(frames, frame_size, 120.0f, sample_rate);
  float e150 = dft_power_at_freq(frames, frame_size, 150.0f, sample_rate);
  float e180 = dft_power_at_freq(frames, frame_size, 180.0f, sample_rate);
  // 50Hz 谐波组 vs 60Hz 谐波组
  float peak_50hz = std::max({e50, e100, e150});
  float peak_60hz = std::max({e60, e120, e180});
  float hum_peak = std::max(peak_50hz, peak_60hz);
  // Resolution #5 extension:基频存在性守卫,防止非 hum 低频音(如 200Hz 纯音)
  // 被 Goertzel 误判为 hum。200Hz 纯音在 180Hz(60Hz 3 倍频)产生强频谱泄漏,
  // 但 60Hz 基频很弱;真实工频哼声的基频(50 或 60Hz)总是显著存在。
  // 要求 max(e50, e60) >= 0.3 * hum_peak,否则视为非 hum,清零 hum_peak。
  // 这是 fix #3 "real peak above real noise floor" 原则的延伸:
  // 不仅 floor 要真实,peak 也要真实(基频存在),而非谐波泄漏冒充。
  if (hum_peak > 1e-12f) {
    float fundamental = std::max(e50, e60);
    if (fundamental < 0.3f * hum_peak) {
      hum_peak = 0.0f;
    }
  }
  // 周围频带中位数(FFT 功率谱 bins 4-30,即 375-2812 Hz)
  std::vector<float> surrounding;
  for (size_t k = 4; k <= 30 && k < num_bins; ++k) {
    surrounding.push_back(power[k - 1]);
  }
  float surrounding_median = 0.0f;
  if (!surrounding.empty()) {
    std::sort(surrounding.begin(), surrounding.end());
    size_t mid = surrounding.size() / 2;
    surrounding_median = surrounding[mid];
  }
  float peak_db = 0.0f;
  // Resolution #5:仅在 surrounding_median 有意义(> 1e-12)时计算 peak_db。
  // 原 60dB 兜底分支假设"峰值显著但周围近 0 -> 极强 hum",但近静音时
  // peak/floor 都是微小数值(如频谱泄漏),并非真实 hum。
  // 默认 peak_db=0 -> hum_conf=clamp((0-10)/20,0,1)=0,不触发 hum 候选。
  if (hum_peak > 1e-12f && surrounding_median > 1e-12f) {
    peak_db = 10.0f * std::log10(hum_peak / surrounding_median);
  }
  result.hum_strength_db = peak_db;

  // ── 脉冲检测(时域,arch §3.3.4:短时能量突变 > 6σ) ─────────────────
  // mean μ + stddev σ,max deviation in σ units,conf = clamp((σ-6)/6,0,1)
  float mean = 0.0f;
  for (size_t i = 0; i < frame_size; ++i) {
    mean += frames[i];
  }
  mean /= static_cast<float>(frame_size);
  float var = 0.0f;
  for (size_t i = 0; i < frame_size; ++i) {
    float d = frames[i] - mean;
    var += d * d;
  }
  var /= static_cast<float>(frame_size);
  float sigma = std::sqrt(var);
  float max_dev = 0.0f;
  size_t impulse_samples = 0;
  if (sigma > 1e-10f) {
    for (size_t i = 0; i < frame_size; ++i) {
      float dev = std::abs(frames[i] - mean) / sigma;
      if (dev > max_dev)
        max_dev = dev;
      if (dev > 6.0f)
        ++impulse_samples;
    }
  }
  // impulse_count:每秒脉冲样本数(frame_duration = frame_size/sample_rate 秒)
  float frame_duration_sec = static_cast<float>(frame_size) / sample_rate;
  result.impulse_count =
      frame_duration_sec > 0.0f
          ? static_cast<float>(impulse_samples) / frame_duration_sec
          : 0.0f;

  // ── L1 规则式分类(6 条规则,各输出连续置信度) ───────────────────────
  auto candidates = classify_rule_based(power, kFftSize, sample_rate, frames,
                                        frame_size, sf_recomputed);

  // 额外填充 Hum50Hz/60Hz 候选(Goertzel 精确数据,classify_rule_based 不再添加)
  float hum_conf = clampf((peak_db - 10.0f) / 20.0f, 0.0f, 1.0f);
  if (hum_conf > 0.1f) {
    NoiseType hum_type =
        (peak_50hz >= peak_60hz) ? NoiseType::Hum50Hz : NoiseType::Hum60Hz;
    candidates.push_back({hum_type, hum_conf});
  }

  // 额外填充 Impulse 候选(需要时域数据)
  float impulse_conf = clampf((max_dev - 6.0f) / 6.0f, 0.0f, 1.0f);
  if (impulse_conf > 0.1f) {
    bool found = false;
    for (auto& c : candidates) {
      if (c.type == NoiseType::Impulse) {
        c.confidence = impulse_conf;
        found = true;
        break;
      }
    }
    if (!found) {
      candidates.push_back({NoiseType::Impulse, impulse_conf});
    }
  }

  // 按置信度降序排序,取 top-3(conf > 0.1)
  std::sort(candidates.begin(), candidates.end(),
            [](const NoiseTypeCandidate& a, const NoiseTypeCandidate& b) {
              return a.confidence > b.confidence;
            });
  // 去重同类型(保留最高置信度)
  std::vector<NoiseTypeCandidate> deduped;
  for (const auto& c : candidates) {
    bool dup = false;
    for (const auto& d : deduped) {
      if (d.type == c.type) {
        dup = true;
        break;
      }
    }
    if (!dup && c.confidence > 0.1f) {
      deduped.push_back(c);
    }
  }
  if (deduped.size() > 3) {
    deduped.resize(3);
  }
  result.candidates = deduped;

  // 主类型 + 混合判定(arch §3.3.4 L530)
  if (!result.candidates.empty()) {
    result.primary_type = result.candidates[0].type;
    result.primary_confidence = result.candidates[0].confidence;
    // 混合:candidates.size() >= 2 && candidates[1].confidence > 0.3
    result.is_mixed =
        result.candidates.size() >= 2 && result.candidates[1].confidence > 0.3f;
  }

  return result;
}

// L3 ML 分类层（YAMNet 端到端分类，独立于 L1）。
// 每帧追加 analysis PCM 到 3s 环形缓冲；当缓冲满 + 节流冷却到期时，
// 取最新一窗 PCM 调 MlClassifier::classify 做直接分类。
// 命中 -> noise_type_source="l3" + ml_noise_type/ml_noise_score/ml_top_types。
//
// L3 与 L1 是互补维度（频域特征 vs 声源识别），并行运行：
// - L3 不受 L1 置信度门控（L1 高置信时 L3 仍运行，提供声源信息）
// - L3 不受 VAD 门控（YAMNet 多标签能处理混合音频）
// 触发频率：节流 kL3CooldownFrames(=3s) 一次，避免每帧跑 ONNX。
void NoiseAnalyzer::maybe_run_l3_(NoiseAnalysisResult& result,
                                  const float* frames,
                                  size_t frame_size) {
  if (!ml_classifier_)
    return;
  if (frames == nullptr || frame_size == 0)
    return;

  // 追加到 PCM 环形缓冲（惰性分配）。
  if (pcm_ring_.empty())
    pcm_ring_.assign(kYamnetWindowSamples, 0.0f);
  for (size_t i = 0; i < frame_size; ++i) {
    pcm_ring_[pcm_ring_head_] = frames[i];
    pcm_ring_head_ = (pcm_ring_head_ + 1) % kYamnetWindowSamples;
    if (pcm_ring_count_ < kYamnetWindowSamples)
      ++pcm_ring_count_;
  }

  // 节流冷却递减。
  if (l3_cooldown_ > 0)
    --l3_cooldown_;

  // 触发条件：缓冲满 + L1 未识别 + 冷却到期。
  if (pcm_ring_count_ < kYamnetWindowSamples)
    return;  // 不足 3s，无法分类
  if (l3_cooldown_ > 0)
    return;  // 节流中

  // 取最新一窗（环形缓冲按写入顺序展开为连续 3s）。
  std::vector<float> window(kYamnetWindowSamples);
  // head 指向下一个写入位置 = 最旧样本位置（缓冲满时）。
  for (size_t i = 0; i < kYamnetWindowSamples; ++i)
    window[i] = pcm_ring_[(pcm_ring_head_ + i) % kYamnetWindowSamples];

  auto m = ml_classifier_->classify(window.data(), kYamnetWindowSamples);
  l3_cooldown_ = kL3CooldownFrames;  // 触发后冷却（无论命中）
  if (m.has_value()) {
    // 持久化 L3 结果：后续帧恢复此结果，直到下次 L3 触发更新。
    last_l3_type_ = m->type_name;
    last_l3_score_ = m->score;
    last_l3_top_types_ = m->top_types;
    l3_has_result_ = true;
    result.noise_type_source = "l3";
    result.ml_noise_type = m->type_name;
    result.ml_noise_score = m->score;
    result.ml_top_types = std::move(m->top_types);
  }
  // 未命中：保持上次 L3 结果（如有），L3 已尽力。
}

std::vector<NoiseTypeCandidate> NoiseAnalyzer::classify_rule_based(
    const std::vector<float>& power_spectrum,
    size_t fft_n,
    float sample_rate,
    const float* /*frames*/,
    size_t /*frame_size*/,
    float spectral_flatness) {
  std::vector<NoiseTypeCandidate> candidates;

  const size_t n_half = fft_n / 2;
  const size_t num_bins = n_half - 1;
  if (num_bins == 0)
    return candidates;

  const float bin_freq = sample_rate / fft_n;

  // ── 规则 1: 白噪声(arch §3.3.4 L523)
  // SF > 0.7,conf = clamp((SF - 0.7) / 0.3, 0, 1)
  if (spectral_flatness > 0.7f) {
    float conf = clampf((spectral_flatness - 0.7f) / 0.3f, 0.0f, 1.0f);
    if (conf > 0.1f)
      candidates.push_back({NoiseType::White, conf});
  }

  // ── 规则 2: 粉红噪声(arch §3.3.4 L524)
  // 频谱斜率 ≈ -3dB/oct,conf = 1 - |slope + 3| / 1.5
  // 计算:在 log-freq vs power_db 空间做线性回归
  {
    std::vector<float> log_freqs;
    std::vector<float> power_db;
    for (size_t k = 1; k < n_half; ++k) {
      float freq_hz = static_cast<float>(k) * bin_freq;
      if (freq_hz < 20.0f || freq_hz > 16000.0f)
        continue;
      if (power_spectrum[k - 1] < 1e-12f)
        continue;
      log_freqs.push_back(std::log2(freq_hz));
      power_db.push_back(10.0f * std::log10(power_spectrum[k - 1]));
    }
    if (log_freqs.size() >= 10) {
      // 线性回归:power_db = a * log_freq + b
      float n = static_cast<float>(log_freqs.size());
      float sum_x = std::accumulate(log_freqs.begin(), log_freqs.end(), 0.0f);
      float sum_y = std::accumulate(power_db.begin(), power_db.end(), 0.0f);
      float sum_xy = 0.0f, sum_xx = 0.0f;
      for (size_t i = 0; i < log_freqs.size(); ++i) {
        sum_xy += log_freqs[i] * power_db[i];
        sum_xx += log_freqs[i] * log_freqs[i];
      }
      float denom = n * sum_xx - sum_x * sum_x;
      if (std::abs(denom) > 1e-10f) {
        float slope = (n * sum_xy - sum_x * sum_y) / denom;
        // slope 单位:dB/oct(log2 频率)
        float conf = 1.0f - std::abs(slope + 3.0f) / 1.5f;
        conf = clampf(conf, 0.0f, 1.0f);
        if (conf > 0.1f)
          candidates.push_back({NoiseType::Pink, conf});
      }
    }
  }

  // ── 规则 3: 工频哼声(Goertzel,在 analyze() 中计算)
  // Resolution #2(Important #2):classify_rule_based 不再添加粗略 hum 候选。
  // 原 FFT bin 1-3(93.75-281.25Hz)粗估会产生误判 -- 200Hz 纯音等非 hum
  // 低频信号被误归为 Hum50Hz 且无法被 Goertzel 路径覆盖替换。
  // hum 分类完全交由 analyze() 中的 Goertzel 精确检测(直接 DFT at
  // 50/60/100/120/150/180 Hz,能正确区分 50 vs 60Hz)。

  // ── 规则 4: 脉冲(时域,在 analyze() 中计算,此处不重复)
  // analyze() 在调用 classify_rule_based 后追加 Impulse 候选。

  // ── 规则 5: 宽带噪声(arch §3.3.4 L527)
  // SF 0.3~0.7,conf = 1 - |SF - 0.5| / 0.2
  if (spectral_flatness >= 0.3f && spectral_flatness <= 0.7f) {
    float conf = 1.0f - std::abs(spectral_flatness - 0.5f) / 0.2f;
    conf = clampf(conf, 0.0f, 1.0f);
    if (conf > 0.1f)
      candidates.push_back({NoiseType::Broadband, conf});
  }

  // ── 规则 6: 数字噪声(arch §3.3.4 L528)
  // 高频(>8kHz)能量比"异常高",conf = clamp((hf_ratio - 0.5) / 0.3, 0, 1)
  // Guard:arch 说"异常高" -- 平坦频谱(白噪)的 hf_ratio 自然 ≈0.67
  // (因为 8-24kHz 占 0-24kHz 的 2/3),这不是"异常"。仅在频谱非平坦
  // (SF < 0.6)时启用,避免白噪误判为 Digital。flat 噪声由 White 规则捕获。
  if (spectral_flatness < 0.6f) {
    float hf_energy = 0.0f;
    float total_energy = 0.0f;
    for (size_t k = 1; k < n_half; ++k) {
      float freq_hz = static_cast<float>(k) * bin_freq;
      total_energy += power_spectrum[k - 1];
      if (freq_hz > 8000.0f) {
        hf_energy += power_spectrum[k - 1];
      }
    }
    if (total_energy > 1e-12f) {
      float hf_ratio = hf_energy / total_energy;
      float conf = clampf((hf_ratio - 0.5f) / 0.3f, 0.0f, 1.0f);
      if (conf > 0.1f)
        candidates.push_back({NoiseType::Digital, conf});
    }
  }

  return candidates;
}

// Spec3 Task 5：一次性 Bark 频谱提取（arch §7.7 add_template 流程步骤 3）。
// 从完整 PCM（WAV 文件内容）提取 32 维 Bark 频带能量，供模板录入使用。
// 与 NoiseAnalyzer::analyze() 共享 accumulate_bark_bands（DRY：同一频带映射 +
// 同一 FFT 实现）。
//
// 实现：按 kFrameSize(480) 样本分帧、每帧零填充到 kFftSize(512) 做 FFT，
// 调 accumulate_bark_bands 累加到 32 维数组，最后跨帧取平均。这与
// NoiseAnalyzer::aggregate_window() 的 bark_energy 均值聚合语义一致
// （arch §3.3.7 L625），但无环形缓冲状态 -- 适合一次性 WAV 处理。
//
// Phase 1 限定：sample_rate 必须 == 48000（arch §11 风险1）。其他采样率
// 返回全零数组（调用方应拒绝非 48kHz WAV，此处为防御性兜底）。
std::array<float, 32> compute_bark_spectrum(const float* pcm,
                                            size_t n,
                                            uint32_t sample_rate) {
  std::array<float, 32> bark{};
  bark.fill(0.0f);
  if (pcm == nullptr || n == 0 || sample_rate != kDefaultSampleRate)
    return bark;

  // 按 kDefaultFrameSize(480) 样本分帧（与 analyze() 的帧大小一致）。
  // 每帧零填充到 kFftSize(512) 做 FFT，累加 Bark 频带能量。
  size_t frame_count = 0;
  for (size_t pos = 0; pos + kDefaultFrameSize <= n; pos += kDefaultFrameSize) {
    std::vector<std::complex<float>> X(kFftSize,
                                       std::complex<float>(0.0f, 0.0f));
    for (size_t i = 0; i < kDefaultFrameSize; ++i) {
      X[i] = std::complex<float>(pcm[pos + i], 0.0f);
    }
    fft_radix2(X);

    const size_t n_half = kFftSize / 2;
    const size_t num_bins = n_half - 1;
    std::vector<float> power(num_bins);
    for (size_t k = 1; k < n_half; ++k) {
      power[k - 1] = std::norm(X[k]);
    }
    accumulate_bark_bands(power, kFftSize, static_cast<float>(sample_rate),
                          bark);
    ++frame_count;
  }

  // 跨帧平均（与 aggregate_window() 的 bark 均值聚合一致）。
  if (frame_count > 0) {
    float fc = static_cast<float>(frame_count);
    for (size_t b = 0; b < 32; ++b) {
      bark[b] /= fc;
    }
  }
  return bark;
}

}  // namespace noise

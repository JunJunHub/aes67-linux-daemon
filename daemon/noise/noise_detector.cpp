// daemon/noise/noise_detector.cpp
// 噪声检测实现:VAD + FFT + SF + SNR 估算。
// 架构依据:docs/noise/architecture-design.md §3.2 L388-459。
#include "noise_detector.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace noise {

namespace {
// FFT 点数(radix-2,480 样本零填充到 512)。
constexpr size_t kFftSize = 512;
constexpr float kPi = 3.14159265358979323846f;
// log 域防 0(避免 log(0) -> -inf)。
constexpr float kLogEps = 1e-20f;
// 噪声判定阈值(arch §3.2 L429 + L435)。
constexpr float kSfNoisyThreshold = 0.6f;
constexpr float kSnrNoisyThresholdDb = 20.0f;
constexpr float kMaxSnrDb = 96.0f;
// 功率谱平滑窗口(3-tap moving average)。
// 目的:降低白噪 FFT bin 间方差(单 bin |X|² 为指数分布,方差 = 均值²),
// 使 SF 估计更稳定趋近理论值 1.0;对谐波信号(speech_like)几乎无影响,
// 因为远端 bin 已接近 0,平滑不改变其量级。
constexpr size_t kSmoothRadius = 1;  // 3-tap = 1 left + center + 1 right

// 512 点 radix-2 Cooley-Tukey FFT(迭代 in-place)。
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

// 频谱平坦度(arch §3.2 L421-429)。
// SF = geo_mean(|X|²) / arith_mean(|X|²),使用正频 bins(k=1..N/2-1)。
// 平滑:3-tap moving average 降低单 bin 方差(白噪 SF 趋近 1)。
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
}  // namespace

NoiseDetectionResult NoiseDetector::process_frame(const float* frames,
                                                  size_t frame_size) {
  NoiseDetectionResult result{false, 0.0f, 0.0f, 0.0f, false};
  if (frame_size == 0)
    return result;

  // 信号能量
  float signal_energy = 0.0f;
  for (size_t i = 0; i < frame_size; ++i) {
    signal_energy += frames[i] * frames[i];
  }

  // VAD(arch §3.2 L410-419,plan deviation:SimpleEnergyVad)
  result.is_speech = vad_.process(frames, frame_size, 48000);

  // 噪声底(arch §3.2 L431-435,最小统计法:非语音帧 track min energy)
  if (!result.is_speech) {
    if (frame_count_ == 0 || signal_energy < noise_floor_energy_) {
      noise_floor_energy_ = signal_energy;
    }
  }
  ++frame_count_;

  // FFT(零填充到 kFftSize)
  std::vector<std::complex<float>> X(kFftSize, std::complex<float>(0.0f, 0.0f));
  size_t copy_len = std::min(frame_size, kFftSize);
  for (size_t i = 0; i < copy_len; ++i) {
    X[i] = std::complex<float>(frames[i], 0.0f);
  }
  fft_radix2(X);

  // 频谱平坦度
  result.spectral_flatness = compute_spectral_flatness(X);

  // SNR 估算(arch §3.2 L434):10·log10(signal / noise_floor)
  float noise_floor = noise_floor_energy_ > 0.0f ? noise_floor_energy_ : 1e-10f;
  float snr = 10.0f * std::log10(signal_energy / noise_floor);
  result.estimated_snr_db = std::clamp(snr, 0.0f, kMaxSnrDb);

  // is_noisy:SF > 0.6(频谱平坦)或 SNR < 20dB(噪声显著)
  result.is_noisy = (result.spectral_flatness > kSfNoisyThreshold) ||
                    (result.estimated_snr_db < kSnrNoisyThresholdDb);

  // 置信度:SF 距阈值越远置信度越高(监测角色,粗略启发式)
  float sf_dist = std::abs(result.spectral_flatness - kSfNoisyThreshold);
  result.confidence = std::clamp(sf_dist / kSfNoisyThreshold, 0.0f, 1.0f);

  return result;
}

void NoiseDetector::set_sensitivity(float level) {
  sensitivity_ = std::clamp(level, 0.0f, 1.0f);
}

void NoiseDetector::reset() {
  vad_.reset();
  sensitivity_ = 0.5f;
  noise_floor_energy_ = 0.0f;
  frame_count_ = 0;
}

}  // namespace noise

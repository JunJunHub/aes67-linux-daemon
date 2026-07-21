// daemon/noise/noise_metrics.cpp
// ④NoiseMetrics 实现。架构依据：docs/noise/architecture-design.md §3.6。
#include "noise_metrics.hpp"

#include <algorithm>
#include <cmath>

namespace noise {

// IEEE 754 float <-> uint32_t 位转换（与 IDenoisePlugin set_dry_wet
// 同一做法）。
static inline uint32_t float_to_bits(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(float));
  return bits;
}
static inline float bits_to_float(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof(float));
  return f;
}

// 安全 dBFS：rms <= 0 返回 -100dBFS（静音下限，不产生 -inf）。
static inline float rms_to_dbfs(float rms) {
  if (rms <= 0.0f)
    return -100.0f;
  return 20.0f * std::log10(rms);
}

NoiseMetrics::NoiseMetrics() {
  // denoise_dry_wet_bits_ 默认 1.0f（IEEE 754 位模式）。
  denoise_dry_wet_bits_.store(float_to_bits(1.0f), std::memory_order_relaxed);
}

void NoiseMetrics::set_denoise_state(bool enabled, float dry_wet) {
  denoise_enabled_.store(enabled, std::memory_order_relaxed);
  denoise_dry_wet_bits_.store(float_to_bits(dry_wet),
                              std::memory_order_relaxed);
}

void NoiseMetrics::collect(const DenoiseResult& denoise,
                           const NoiseDetectionResult& detection,
                           const NoiseAnalysisResult& analysis,
                           float input_rms,
                           float denoised_rms) {
  // ② 检测结果
  latest_.is_noisy = detection.is_noisy;
  latest_.noise_confidence = detection.confidence;
  latest_.estimated_snr_db = detection.estimated_snr_db;

  // ③ 分析结果
  latest_.noise_type = analysis.primary_type;
  latest_.noise_type_confidence = analysis.primary_confidence;
  latest_.is_mixed = analysis.is_mixed;
  // candidates：vector -> 定长 array（最多 3，避免 per-call 堆分配）。
  size_t n = std::min(analysis.candidates.size(), kMaxNoiseCandidates);
  for (size_t i = 0; i < n; ++i) {
    latest_.noise_candidates[i].type = analysis.candidates[i].type;
    latest_.noise_candidates[i].confidence = analysis.candidates[i].confidence;
  }
  latest_.noise_candidates_count = n;
  latest_.noise_level_dbfs = analysis.noise_level_dbfs;
  latest_.spectral_centroid_hz = analysis.spectral_centroid_hz;
  latest_.spectral_flatness = analysis.spectral_flatness;
  latest_.hum_strength_db = analysis.hum_strength_db;

  // ① 降噪效果
  // atomic 读 denoise_enabled / dry_wet（控制线程 set_denoise_state 写）。
  latest_.denoise_enabled = denoise_enabled_.load(std::memory_order_relaxed);
  latest_.denoise_dry_wet =
      bits_to_float(denoise_dry_wet_bits_.load(std::memory_order_relaxed));
  latest_.input_level_dbfs = rms_to_dbfs(input_rms);
  latest_.output_level_dbfs = rms_to_dbfs(denoised_rms);
  // noise_reduction_db = 20·log10(input_rms / denoised_rms)。
  // Guard divide-by-zero：denoised_rms <= 0 时设为 0（不产生 inf/nan，
  // brief D-S3.9 要求）。
  if (input_rms > 0.0f && denoised_rms > 0.0f) {
    latest_.noise_reduction_db = 20.0f * std::log10(input_rms / denoised_rms);
  } else {
    latest_.noise_reduction_db = 0.0f;
  }

  // 告警判定（D-S3.4）
  latest_.is_alerting =
      latest_.noise_level_dbfs > latest_.alert_threshold_dbfs ||
      latest_.hum_strength_db > latest_.hum_alert_threshold_db;

  // timestamp：用帧计数作为相对时间戳（非墙钟，足够 /history 序列化用）。
  latest_.timestamp_ms = frame_counter_;

  // 60s history ring：每 kHistorySampleIntervalFrames 帧采样一次。
  ++frame_counter_;
  if (frame_counter_ % history_sample_interval_ == 0) {
    history_.push_back(latest_);
    if (history_.size() > kMaxHistorySize)
      history_.pop_front();
  }
}

}  // namespace noise

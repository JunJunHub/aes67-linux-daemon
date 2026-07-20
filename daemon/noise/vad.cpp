// daemon/noise/vad.cpp
// SimpleEnergyVad 实现:RMS + ZCR + 最小统计法 noise floor。
// 架构依据:docs/noise/architecture-design.md §3.2 L410-419(VAD 层)。
#include "vad.hpp"

#include <algorithm>
#include <cmath>

namespace noise {

namespace {
// 学习帧数:前 N 帧强制判非语音,用于初始化 noise floor。
constexpr size_t kLearningFrames = 10;
// 绝对噪声底(RMS):低于此值判为静音,避免 noise_floor=0 时任意微小信号都过阈值。
// 选 1e-4:静音帧(RMS≈0)远低于此,正常语音 RMS > 0.05 轻松超过。
constexpr float kAbsoluteFloor = 1e-4f;
// 语音 RMS 阈值因子:RMS > noise_floor × 此值才视为语音候选。
constexpr float kSpeechRmsFactor = 4.0f;
// ZCR 上界:语音为周期信号(基频 + 谐波),过零率显著低于白噪(ZCR≈0.5)。
// 选 0.45:排除白噪,容纳合成 speech_like(ZCR≈0.006)与真实语音(ZCR 0.1-0.35)。
// 注:brief 建议 0.1-0.35 是真实语音的典型范围;此处放宽下界以容纳合成谐波信号。
constexpr float kZcrUpperBound = 0.45f;
}  // namespace

bool SimpleEnergyVad::process(const float* frames,
                              size_t frame_size,
                              uint32_t /*sample_rate*/) {
  if (frame_size == 0)
    return false;

  // RMS = sqrt(mean(x²))
  float sum_sq = 0.0f;
  for (size_t i = 0; i < frame_size; ++i) {
    sum_sq += frames[i] * frames[i];
  }
  float rms = std::sqrt(sum_sq / static_cast<float>(frame_size));

  // ZCR = 相邻样本符号翻转的比例
  size_t crossings = 0;
  for (size_t i = 1; i < frame_size; ++i) {
    if ((frames[i - 1] >= 0.0f) != (frames[i] >= 0.0f)) {
      ++crossings;
    }
  }
  float zcr = static_cast<float>(crossings) / static_cast<float>(frame_size);

  bool is_speech;
  if (frame_count_ < kLearningFrames) {
    // 学习阶段:强制判非语音,用于建立初始 noise floor。
    is_speech = false;
  } else {
    // 阈值 = max(noise_floor × factor, absolute_floor)
    // 绝对底防止 noise_floor=0(全静音学习)时阈值退化为 0、任意信号都过。
    float threshold =
        std::max(noise_floor_rms_ * kSpeechRmsFactor, kAbsoluteFloor);
    is_speech = (rms > threshold) && (zcr < kZcrUpperBound);
  }

  // 非语音帧更新 noise floor(最小统计法:track min RMS)。
  if (!is_speech) {
    if (frame_count_ == 0 || rms < noise_floor_rms_) {
      noise_floor_rms_ = rms;
    }
  }
  ++frame_count_;
  return is_speech;
}

}  // namespace noise

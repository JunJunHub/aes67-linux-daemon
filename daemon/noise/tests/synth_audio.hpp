// daemon/noise/tests/synth_audio.hpp
// 合成帧生成器,所有 noise 后续 task 测试共用。
// 与 task-2-brief 的差异(由 task brief 显式授权):
//  - white_noise: 使用传入 seed 而非硬编码 12345,允许不同测试用例生成不同噪声。
//  - 使用局部 constexpr kPi 替代 M_PI,避免依赖 _DEFAULT_SOURCE/_GNU_SOURCE 宏。
#ifndef NOISE_TEST_SYNTH_AUDIO_HPP_
#define NOISE_TEST_SYNTH_AUDIO_HPP_

#include <cmath>
#include <cstdint>
#include <cstddef>

namespace synth {

constexpr size_t kFrameSize = 480;
constexpr uint32_t kSampleRate = 48000;
constexpr double kPi = 3.14159265358979323846;

// 白噪(频谱平坦,SF 趋近 1)
// seed=0 时回退到 12345 保持与 brief 兼容;非零 seed 用于生成不同噪声序列。
inline void white_noise(float* out, size_t n, uint32_t seed) {
  uint32_t s = seed ? seed : 12345u;
  for (size_t i = 0; i < n; ++i) {
    s = s * 1103515245u + 12345u;
    out[i] = (static_cast<float>(s >> 16) / 65535.0f - 0.5f) * 0.2f;
  }
}

// 50Hz 哼声(50Hz + 倍频)
inline void hum_50hz(float* out, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    float t = static_cast<float>(i) / kSampleRate;
    out[i] =
        0.3f * std::sin(2 * kPi * 50 * t) + 0.1f * std::sin(2 * kPi * 100 * t);
  }
}

// 脉冲(短时能量突变)
inline void impulse(float* out, size_t n) {
  for (size_t i = 0; i < n; ++i)
    out[i] = 0;
  for (size_t i = 0; i < 5; ++i)
    out[i] = 0.9f;  // 起始尖峰
}

// 语音(基频 150Hz + 谐波,模拟有结构频谱)
inline void speech_like(float* out, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    float t = static_cast<float>(i) / kSampleRate;
    out[i] = 0.2f *
             (std::sin(2 * kPi * 150 * t) + 0.5f * std::sin(2 * kPi * 300 * t) +
              0.25f * std::sin(2 * kPi * 450 * t));
  }
}

inline void silence(float* out, size_t n) {
  for (size_t i = 0; i < n; ++i)
    out[i] = 0;
}

}  // namespace synth

#endif  // NOISE_TEST_SYNTH_AUDIO_HPP_

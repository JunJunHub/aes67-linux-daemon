// daemon/noise/noise_detector.hpp
// 噪声检测(监测角色,非门控)。
// 架构依据:docs/noise/architecture-design.md §3.2 L388-459。
// Spec2 1.5:NoiseDetectionResult struct + NoiseDetector 类签名逐字采用 §3.2
// L441-459;WebRTC VAD 替换为 IVad/SimpleEnergyVad(plan deviation)。
#ifndef NOISE_NOISE_DETECTOR_HPP_
#define NOISE_NOISE_DETECTOR_HPP_

#include <cstddef>

#include "vad.hpp"

namespace noise {

// 噪声检测结果(arch §3.2 L441-447)。
struct NoiseDetectionResult {
  bool is_noisy;            // 当前帧是否含噪声
  float confidence;         // 置信度 [0, 1]
  float spectral_flatness;  // 频谱平坦度
  float estimated_snr_db;   // 估算 SNR (dB)
  bool is_speech;           // VAD 结果
};

// 噪声检测器:实时判断当前帧是否含噪声,输出布尔结果 + 置信度。
// 监测角色,不门控 DenoiseProcessor(arch §3.2 L390-392 论证)。
class NoiseDetector {
 public:
  NoiseDetectionResult process_frame(const float* frames, size_t frame_size);
  void set_sensitivity(float level);  // 检测灵敏度 [0, 1]
  void reset();

 private:
  SimpleEnergyVad vad_;  // arch §3.2 L449 原 WebRTC VAD,plan deviation
  float sensitivity_{0.5f};
  // 噪声底能量(用于 SNR 估算)。最小统计法在非语音帧更新。
  float noise_floor_energy_{0.0f};
  size_t frame_count_{0};
};

}  // namespace noise

#endif  // NOISE_NOISE_DETECTOR_HPP_

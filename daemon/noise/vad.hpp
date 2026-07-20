// daemon/noise/vad.hpp
// VAD 接口与零依赖实现(plan deviation:WebRTC VAD -> IVad/SimpleEnergyVad)。
// 架构依据:docs/noise/architecture-design.md §3.2 L410-419(原计划 WebRTC VAD;
// 本 task 改为 SimpleEnergyVad,Task 4 引入 RNNoise 后其 VAD 概率为降噪开启时的
// 主 VAD 来源,本接口的 IVad 可 drop-in 适配)。
#ifndef NOISE_VAD_HPP_
#define NOISE_VAD_HPP_

#include <cstddef>
#include <cstdint>

namespace noise {

// VAD 接口(决策3 deviation:SimpleEnergyVad 实现,WebrtcVadAdapter 可 drop-in)。
class IVad {
 public:
  virtual ~IVad() = default;
  // 返回 true=语音,false=非语音(噪声候选)。
  virtual bool process(const float* frames,
                       size_t frame_size,
                       uint32_t sample_rate) = 0;
  virtual void reset() = 0;
};

// 能量 + 过零率 VAD(零外部依赖)。
// 实现:RMS > noise_floor × kSpeechRmsFactor 且 ZCR 在语音范围判为语音;
// 非语音帧以最小统计法更新 noise_floor;前 kLearningFrames 帧强制学习。
class SimpleEnergyVad : public IVad {
 public:
  bool process(const float* frames,
               size_t frame_size,
               uint32_t sample_rate) override;
  void reset() override {
    noise_floor_rms_ = 0.0f;
    frame_count_ = 0;
  }

 private:
  float noise_floor_rms_{0.0f};
  size_t frame_count_{0};
};

}  // namespace noise

#endif  // NOISE_VAD_HPP_

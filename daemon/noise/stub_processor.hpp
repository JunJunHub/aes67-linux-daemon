// daemon/noise/stub_processor.hpp
// 1.4b stub：直通帧，不处理。Task 7 替换为真实 DenoiseProcessor（①②③）。
#ifndef NOISE_STUB_PROCESSOR_HPP_
#define NOISE_STUB_PROCESSOR_HPP_

#include <cstddef>
#include <cstdint>

class StubProcessor {
 public:
  void on_period_begin() {}
  void on_period_end() {}
  // 1.4b stub：直通帧，不处理。Task 7 替换为真实 ①②③。
  void process(uint8_t sink_id, const float* frames, size_t frame_size) {
    (void)sink_id;
    (void)frames;
    (void)frame_size;
  }
};

#endif  // NOISE_STUB_PROCESSOR_HPP_

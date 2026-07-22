// daemon/noise/ref_comparator.hpp
// Spec4 T5：RefComparator - 主备链路参考音比对。
// 架构依据：docs/noise/architecture-design.md §3.5（L671-712）+ §6.3.2
// （L1497-1513）+ §11 风险 9/12/20。
//
// 职责：对两路 Sink 的音频帧做参考音比对式噪声估计。
//   - 参考音（ref）：主链路或本地 Source 的干净信号
//   - 比对音（cmp）：备链路或远端 Sink 的传输后信号
//   - 算法：时域互相关延时搜索 + NLMS 自适应滤波（残差 = 加性噪声 dB）
//
// 线程模型（D-S4.3，风险 9/12）：
//   - write_ref/write_cmp：capture 线程调用（on_frame 末尾），仅 memcpy 进
//     环形缓冲，快速返回，绝不做 MFCC/NLMS 重计算。
//   - try_process()：comparison 线程调用（SCHED_OTHER，~每 2s 一次），
//     从双路 ring 读取数据 -> 互相关延时搜索 -> NLMS -> 残差 dB -> 输出结果。
//   - Ring buffer 写入用 mutex 保护（持有 ~0.5μs，仅 memcpy + 索引更新，
//     与 NoiseMetrics::collect 持 metrics_mutex_ 同模式，不影响 RT 预算）。
//
// 算法简化（R-S4.1）：先以简化版跑通端到端：
//   - 延时：时域互相关搜索（样本级精度 ≤1ms，无需 MFCC）
//   - 信道估计：固定阶数 NLMS（256 taps，步长经验值）
//   - 残差 = 比对音 - 滤波器估计 -> RMS -> dB = 加性噪声
//   - 相似度 = 互相关峰值的归一化系数（cosine-like，[0, 1]）
//   全 MFCC + GCC-PHAT 可后续迭代精度，当前简化版已满足 ≤1ms 延时精度
//   + 合理噪声估计的验收标准。
#ifndef NOISE_REF_COMPARATOR_HPP_
#define NOISE_REF_COMPARATOR_HPP_

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace noise {

// 比对结果（arch §3.5 L700-706）。
struct RefCompareResult {
  float delay_ms{0.0f};            // 两路信号延时差 (ms)
  float similarity{0.0f};          // 相似度 [0, 1]
  float noise_db{-100.0f};         // 加性噪声估计 (dB)
  float channel_distortion{0.0f};  // 信道线性失真度 [0, 1]
};

// RefComparator - per-comparator 实例，由 NoiseManager 在配置参考比对时创建。
// 一个 comparator 绑定一对 (ref_sink, cmp_sink)，维护双路环形缓冲 + 对齐状态。
class RefComparator {
 public:
  RefComparator(uint8_t ref_sink_id,
                uint8_t cmp_sink_id,
                uint32_t sample_rate = 48000);

  // ── capture 线程调用（on_frame 末尾）──
  // 将帧 memcpy 进对应路的环形缓冲。快速，非阻塞（mutex 持有 ~0.5μs）。
  void write_ref(const float* frames, size_t n);
  void write_cmp(const float* frames, size_t n);

  // ── comparison 线程调用（~每 2s 一次）──
  // 检查两路都有足够数据且未 stale -> 互相关延时搜索 + NLMS -> 残差 dB。
  // 返回 nullopt 条件：
  //   - 任一路数据不足（< kMinProcessSamples）
  //   - 任一路 stale（超过 kStaleThreshold 无新数据 -> delay_anomaly）
  //   - 对齐尚未建立（首次或暂停恢复后需两路都活跃）
  std::optional<RefCompareResult> try_process();

  // ── 访问器 ──
  uint8_t ref_sink_id() const { return ref_sink_id_; }
  uint8_t cmp_sink_id() const { return cmp_sink_id_; }
  bool delay_anomaly() const {
    return delay_anomaly_.load(std::memory_order_relaxed);
  }
  void clear_delay_anomaly_for_test() { delay_anomaly_.store(false); }

  // 测试钩子：溢出计数（各路独立）。
  size_t ref_overflow_count() const;
  size_t cmp_overflow_count() const;

  // 测试钩子：设置环形缓冲容量（默认 kDefaultRingCapacity）。
  void set_ring_capacity_for_test(size_t capacity);

  // 测试钩子：设置 stale 阈值（默认 500ms）。
  void set_stale_threshold_for_test(std::chrono::milliseconds threshold);

  // 常量：默认环形缓冲容量（2×period ~128ms @48k，风险 12）。
  static constexpr size_t kDefaultRingCapacity = 6144;
  // 最少处理样本数（NLMS 256 taps 需足够收敛数据）。
  static constexpr size_t kMinProcessSamples = 1024;
  // stale 阈值：超过此时间无新数据 -> delay_anomaly（风险 20）。
  static constexpr auto kStaleThreshold = std::chrono::milliseconds(500);
  // 延时差异常阈值（>10ms -> delay_anomaly，风险 12）。
  static constexpr float kDelayAnomalyMs = 10.0f;

 private:
  // 环形缓冲：单生产者（capture 线程）单消费者（comparison 线程）。
  // mutex 保护：写入（capture 线程，~0.5μs memcpy）vs 读取（comparison 线程，
  // ~6μs copy out）。与 NoiseMetrics::collect 同模式。
  struct RingBuffer {
    std::vector<float> buf;
    size_t capacity{0};
    size_t write_pos{0};
    size_t count{0};
    size_t overflow_count{0};
    std::chrono::steady_clock::time_point last_write;
    size_t total_written{0};
    mutable std::mutex mutex;

    explicit RingBuffer(size_t cap);
    void write(const float* src, size_t n);
    // 线性化拷贝当前缓冲内容到 out（不消费）。返回拷贝的样本数。
    size_t read(std::vector<float>& out) const;
    bool is_stale(std::chrono::milliseconds threshold) const;
    size_t get_overflow_count() const;
    size_t get_total_written() const;
    // 重建缓冲为新容量（测试用，无并发安全考虑）。
    void resize_for_test(size_t new_cap);
  };

  uint8_t ref_sink_id_;
  uint8_t cmp_sink_id_;
  uint32_t sample_rate_;
  RingBuffer ref_buf_;
  RingBuffer cmp_buf_;

  std::atomic<bool> delay_anomaly_{false};
  // 对齐状态：首次或暂停恢复后需两路都活跃才建立对齐（风险 20）。
  bool aligned_{false};
  // 上次成功处理的延时（用于漂移检测）。
  float last_delay_ms_{0.0f};

  // stale 阈值（可测试覆盖）。
  std::chrono::milliseconds stale_threshold_{500};

  // ── 算法 ──
  // 时域互相关延时搜索。返回延时样本数（cmp 相对 ref 的偏移）+ 峰值归一化
  // 相似度。search_range = 搜索范围（±样本数）。
  int estimate_delay(const std::vector<float>& ref,
                     const std::vector<float>& cmp,
                     size_t search_range,
                     float& similarity) const;

  // NLMS 自适应滤波：以 input 估计 desired，返回残差 RMS。
  // filter_coeffs 为滤波器系数（in/out，跨调用保持状态以连续收敛）。
  float run_nlms(const std::vector<float>& input,
                 const std::vector<float>& desired,
                 std::vector<float>& filter_coeffs,
                 float& residual_rms) const;

  // NLMS 滤波器系数（256 taps，持续跨 try_process 调用以累积收敛）。
  // 重置条件：暂停恢复后重对齐（aligned_=false -> true 时 reset）。
  std::vector<float> nlms_coeffs_;
  static constexpr size_t kNlmsOrder = 256;
  static constexpr float kNlmsStepSize = 0.01f;
  static constexpr float kNlmsRegEps = 1e-6f;
};

}  // namespace noise

#endif  // NOISE_REF_COMPARATOR_HPP_

// daemon/noise/ref_comparator.cpp
// Spec4 T5：RefComparator 实现。架构依据：docs/noise/architecture-design.md
// §3.5（L671-712）+ §6.3.2（L1497-1513）+ §11 风险 9/12/20。
//
// 算法简化决策（R-S4.1，参考 `噪声比对监测实现说明.docx` 既有参数）：
//   - 延时搜索：时域互相关（非 GCC-PHAT/MFCC）。样本级精度足够 ≤1ms 验收，
//     实现量小且非确定性低（NLMS 收敛是主要成本，已在 comparison 线程隔离）。
//   - 信道估计：固定阶数 NLMS（256 taps），步长 0.01，跨 try_process 调用
//     持续收敛。暂停恢复后 reset（风险 20）。
//   - 残差 = cmp - NLMS(ref) -> 滤波器估计的加性噪声 -> RMS -> dB。
//   - 相似度 = 互相关峰值归一化（标准 cosine 相似度变体）。
//
// FFT 选型决策：本文件不复用 NoiseAnalyzer 内嵌 FFT（其 fft_radix2 是
// anonymous namespace static，不导出）。简化版延时搜索用纯时域互相关，
// 无需 FFT。若后续精度升级到 GCC-PHAT/MFCC，可提取共享 fft.hpp
// （与 noise_analyzer.cpp 内嵌 FFT 的 DRY 偏差已在 noise_analyzer.cpp
// 注释记为 Minor，本文件同一偏差）。
#include "ref_comparator.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace noise {

// ── RingBuffer ──────────────────────────────────────────────────────────────

RefComparator::RingBuffer::RingBuffer(size_t cap)
    : buf(cap, 0.0f),
      capacity(cap),
      last_write(std::chrono::steady_clock::now()) {}

void RefComparator::RingBuffer::write(const float* src, size_t n) {
  std::lock_guard<std::mutex> lock(mutex);
  if (capacity == 0) {
    return;
  }
  // 溢出 drop oldest：若 n > capacity，只保留最后 capacity 个样本，
  // 被丢弃的 (n - capacity) 个样本计入 overflow_count。
  if (n > capacity) {
    overflow_count += (n - capacity);
    src += (n - capacity);
    n = capacity;
  }
  // 若 count + n > capacity，丢弃前面溢出的（drop oldest）。
  size_t overflow = 0;
  if (count + n > capacity) {
    overflow = (count + n) - capacity;
    // 推进 write_pos 越过将被覆盖的数据（逻辑等价于 drop oldest）。
    write_pos = (write_pos + overflow) % capacity;
    count -= overflow;
    overflow_count += overflow;
  }
  // 环形拷贝：可能跨 buf 末端，分两段。
  size_t first_chunk = std::min(n, capacity - write_pos);
  if (first_chunk > 0) {
    std::memcpy(&buf[write_pos], src, first_chunk * sizeof(float));
  }
  size_t second_chunk = n - first_chunk;
  if (second_chunk > 0) {
    std::memcpy(&buf[0], src + first_chunk, second_chunk * sizeof(float));
  }
  write_pos = (write_pos + n) % capacity;
  count += n;
  total_written += n;
  last_write = std::chrono::steady_clock::now();
}

size_t RefComparator::RingBuffer::read(std::vector<float>& out) const {
  std::lock_guard<std::mutex> lock(mutex);
  if (count == 0) {
    out.clear();
    return 0;
  }
  out.resize(count);
  // 线性化拷贝：从 (write_pos - count) % capacity 开始，按时间顺序输出。
  size_t start = (write_pos + capacity - count) % capacity;
  size_t first_chunk = std::min(count, capacity - start);
  if (first_chunk > 0) {
    std::memcpy(out.data(), &buf[start], first_chunk * sizeof(float));
  }
  size_t second_chunk = count - first_chunk;
  if (second_chunk > 0) {
    std::memcpy(out.data() + first_chunk, &buf[0],
                second_chunk * sizeof(float));
  }
  return count;
}

bool RefComparator::RingBuffer::is_stale(
    std::chrono::milliseconds threshold) const {
  std::lock_guard<std::mutex> lock(mutex);
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - last_write);
  return elapsed > threshold;
}

size_t RefComparator::RingBuffer::get_overflow_count() const {
  std::lock_guard<std::mutex> lock(mutex);
  return overflow_count;
}

size_t RefComparator::RingBuffer::get_total_written() const {
  std::lock_guard<std::mutex> lock(mutex);
  return total_written;
}

void RefComparator::RingBuffer::resize_for_test(size_t new_cap) {
  std::lock_guard<std::mutex> lock(mutex);
  buf.assign(new_cap, 0.0f);
  capacity = new_cap;
  write_pos = 0;
  count = 0;
  overflow_count = 0;
  total_written = 0;
  last_write = std::chrono::steady_clock::now();
}

// ── RefComparator ───────────────────────────────────────────────────────────

RefComparator::RefComparator(uint8_t ref_sink_id,
                             uint8_t cmp_sink_id,
                             uint32_t sample_rate)
    : ref_sink_id_(ref_sink_id),
      cmp_sink_id_(cmp_sink_id),
      sample_rate_(sample_rate),
      ref_buf_(kDefaultRingCapacity),
      cmp_buf_(kDefaultRingCapacity),
      nlms_coeffs_(kNlmsOrder, 0.0f) {}

void RefComparator::write_ref(const float* frames, size_t n) {
  ref_buf_.write(frames, n);
}

void RefComparator::write_cmp(const float* frames, size_t n) {
  cmp_buf_.write(frames, n);
}

std::optional<RefCompareResult> RefComparator::try_process() {
  // 前置：PTP 解锁期间不应调用（由 NoiseManager 控制 comparison_running_），
  // 但防御性检查：任一路 stale -> delay_anomaly + 拒绝处理（风险 20）。
  bool ref_stale = ref_buf_.is_stale(stale_threshold_);
  bool cmp_stale = cmp_buf_.is_stale(stale_threshold_);
  if (ref_stale || cmp_stale) {
    // 一路暂停/超时 -> 重置对齐状态 + 标记 anomaly（风险 20）。
    if (aligned_) {
      aligned_ = false;
      delay_anomaly_.store(true, std::memory_order_relaxed);
      // 重置 NLMS 系数，要求两路都活跃后重做完整对齐。
      std::fill(nlms_coeffs_.begin(), nlms_coeffs_.end(), 0.0f);
    }
    return std::nullopt;
  }

  // 读取两路当前缓冲内容。
  std::vector<float> ref, cmp;
  size_t ref_n = ref_buf_.read(ref);
  size_t cmp_n = cmp_buf_.read(cmp);
  if (ref_n < kMinProcessSamples || cmp_n < kMinProcessSamples) {
    return std::nullopt;
  }

  // 对齐建立门：首次或暂停恢复后，要求两路都有充足数据才建立对齐。
  if (!aligned_) {
    aligned_ = true;
    delay_anomaly_.store(false, std::memory_order_relaxed);
    last_delay_ms_ = 0.0f;
  }

  // 取两路较短长度做对齐处理（避免越界）。
  size_t n = std::min(ref_n, cmp_n);

  // 1. 时域互相关延时搜索。搜索范围 ±kDefaultRingCapacity/4 样本
  //    （足够覆盖 1 个 ALSA period 的延时差，约 ±32ms）。
  float similarity = 0.0f;
  size_t search_range = std::min<size_t>(n / 4, 1536);  // ±~32ms @48k
  int delay_samples = estimate_delay(ref, cmp, search_range, similarity);
  float delay_ms = static_cast<float>(delay_samples) * 1000.0f /
                   static_cast<float>(sample_rate_);

  // 2. 延时异常检测（风险 12）：延时差 >10ms -> delay_anomaly（告警不丢数据）。
  if (std::abs(delay_ms - last_delay_ms_) > kDelayAnomalyMs) {
    delay_anomaly_.store(true, std::memory_order_relaxed);
  }
  last_delay_ms_ = delay_ms;

  // 3. NLMS 自适应滤波：以 ref 估计 cmp，残差 = 加性噪声。
  //    对齐后截取（cmp 前移/后移 delay_samples），使两路同相。
  //    delay_samples > 0（cmp 落后 ref）：从 ref[delay] 起，cmp 从 0 起。
  //    delay_samples < 0（cmp 领先 ref）：ref 从 0 起，cmp 从 [-delay] 起。
  std::vector<float> ref_aligned, cmp_aligned;
  int offset = delay_samples;
  // 对齐后可用样本数 = n - |offset|。
  size_t abs_offset = static_cast<size_t>(std::abs(offset));
  size_t usable = (abs_offset < n) ? (n - abs_offset) : 0;
  if (usable < kNlmsOrder + 64) {
    // 对齐后可用样本太少，跳过 NLMS（避免欠定）。
    RefCompareResult r;
    r.delay_ms = delay_ms;
    r.similarity = similarity;
    r.noise_db = -100.0f;
    r.channel_distortion = 0.0f;
    return r;
  }
  ref_aligned.resize(usable);
  cmp_aligned.resize(usable);
  if (offset >= 0) {
    // cmp 落后 ref：ref[delay:] 对齐 cmp[0:]。
    std::memcpy(ref_aligned.data(), ref.data() + abs_offset,
                usable * sizeof(float));
    std::memcpy(cmp_aligned.data(), cmp.data(), usable * sizeof(float));
  } else {
    // cmp 领先 ref：ref[0:] 对齐 cmp[-delay:]。
    std::memcpy(ref_aligned.data(), ref.data(), usable * sizeof(float));
    std::memcpy(cmp_aligned.data(), cmp.data() + abs_offset,
                usable * sizeof(float));
  }

  float residual_rms = 0.0f;
  float filter_energy =
      run_nlms(ref_aligned, cmp_aligned, nlms_coeffs_, residual_rms);

  // 4. 残差 -> dB（加性噪声估计）。
  float noise_db = -100.0f;
  if (residual_rms > 1e-10f) {
    noise_db = 20.0f * std::log10(residual_rms);
  }

  // 5. 信道线性失真度：滤波器系数能量 / 输入能量（简化估计）。
  //    失真低 = 滤波器接近单位冲激（主路径无失真）。
  float channel_distortion = std::clamp(filter_energy, 0.0f, 1.0f);

  RefCompareResult result;
  result.delay_ms = delay_ms;
  result.similarity = similarity;
  result.noise_db = noise_db;
  result.channel_distortion = channel_distortion;
  return result;
}

int RefComparator::estimate_delay(const std::vector<float>& ref,
                                  const std::vector<float>& cmp,
                                  size_t search_range,
                                  float& similarity) const {
  // 时域互相关：对每个候选延时 lag，计算 sum(ref[i+lag] * cmp[i])。
  // 返回使归一化互相关最大的 lag（cmp 相对 ref 的延时样本数）。
  // 正 lag 表示 cmp 落后 ref（需前移 cmp 对齐），负 lag 反之。
  // 约定：cmp[i] = ref[i - delay] -> best lag = -delay ->
  //       delay_samples = -lag（正数 = cmp 落后 ref）。
  size_t n = std::min(ref.size(), cmp.size());
  if (n < 2 * search_range) {
    similarity = 0.0f;
    return 0;
  }

  // 中心对齐窗口：在 [n/2 - search_range, n/2 + search_range] 范围搜索。
  size_t center = n / 2;
  size_t win = std::min<size_t>(search_range, center);

  float best_corr = -2.0f;
  int best_lag = 0;
  // 限制窗口长度避免 O(n * search_range) 过大。用较大窗口（~4096）以获得
  // 足够的频率分辨率区分周期信号的真实延时峰与谐波峰。对宽带信号（白噪）
  // 则互相关峰尖锐，小窗口即可。生产环境 ring buffer ~6144 样本时
  // corr_win 受 n-2*win 约束。
  size_t corr_win = std::min<size_t>(4096, n - 2 * win);

  for (size_t lag_idx = 0; lag_idx <= 2 * win; ++lag_idx) {
    int lag = static_cast<int>(lag_idx) - static_cast<int>(win);
    // ref_start = center + lag：若 lag > 0，ref 前移（对应 cmp 落后 ref）。
    // 若 lag < 0，ref 后移（对应 cmp 领先 ref）。
    size_t ref_start = center + lag;
    size_t cmp_start = center;
    if (ref_start + corr_win > n || cmp_start + corr_win > n) {
      continue;
    }
    float corr = 0.0f;
    float ref_energy = 0.0f;
    float cmp_energy = 0.0f;
    for (size_t i = 0; i < corr_win; ++i) {
      float r = ref[ref_start + i];
      float c = cmp[cmp_start + i];
      corr += r * c;
      ref_energy += r * r;
      cmp_energy += c * c;
    }
    // 归一化互相关（cosine 相似度变体）。
    float norm = std::sqrt(ref_energy * cmp_energy);
    float normalized = (norm > 1e-10f) ? corr / norm : 0.0f;
    if (normalized > best_corr) {
      best_corr = normalized;
      best_lag = lag;
    }
  }

  similarity = std::clamp(best_corr, 0.0f, 1.0f);
  // 返回 delay_samples = -best_lag（正数 = cmp 落后 ref）。
  return -best_lag;
}

float RefComparator::run_nlms(const std::vector<float>& input,
                              const std::vector<float>& desired,
                              std::vector<float>& filter_coeffs,
                              float& residual_rms) const {
  // NLMS 自适应滤波：以 input 估计 desired。
  //   y[n] = sum_{k=0}^{N-1} w[k] * x[n-k]
  //   e[n] = d[n] - y[n]
  //   w[k] += mu * e[n] * x[n-k] / (||x||^2 + eps)
  // 残差 e = desired - 估计 = 加性噪声分量。
  size_t n = std::min(input.size(), desired.size());
  if (n <= kNlmsOrder) {
    residual_rms = 0.0f;
    return 0.0f;
  }

  double sum_sq_residual = 0.0f;
  double sum_sq_filter = 0.0f;
  size_t num_samples = 0;

  for (size_t i = kNlmsOrder; i < n; ++i) {
    // 计算 y = w · x[i-N..i]
    float y = 0.0f;
    float x_energy = 0.0f;
    for (size_t k = 0; k < kNlmsOrder; ++k) {
      float x = input[i - k];
      y += filter_coeffs[k] * x;
      x_energy += x * x;
    }
    float e = desired[i] - y;
    // 系数更新（NLMS 归一化步长）。
    float denom = x_energy + kNlmsRegEps;
    float scale = kNlmsStepSize * e / denom;
    for (size_t k = 0; k < kNlmsOrder; ++k) {
      filter_coeffs[k] += scale * input[i - k];
    }
    // 累计残差统计。
    sum_sq_residual += static_cast<double>(e) * e;
    ++num_samples;
  }

  // 残差 RMS = sqrt(mean(e^2))。
  if (num_samples > 0) {
    residual_rms = static_cast<float>(
        std::sqrt(sum_sq_residual / static_cast<double>(num_samples)));
  } else {
    residual_rms = 0.0f;
  }

  // 滤波器能量（用于信道失真度估计）。
  for (size_t k = 0; k < kNlmsOrder; ++k) {
    sum_sq_filter += static_cast<double>(filter_coeffs[k]) * filter_coeffs[k];
  }
  // 归一化到 [0, 1]：理想信道（单位冲激）能量 = 1（w[0]=1，其余=0）。
  float filter_energy =
      static_cast<float>(std::sqrt(sum_sq_filter) / kNlmsOrder);
  return filter_energy;
}

// ── 访问器 / 测试钩子 ──────────────────────────────────────────────────────

size_t RefComparator::ref_overflow_count() const {
  return ref_buf_.get_overflow_count();
}

size_t RefComparator::cmp_overflow_count() const {
  return cmp_buf_.get_overflow_count();
}

void RefComparator::set_ring_capacity_for_test(size_t capacity) {
  // 重建 ring buffer 内容（仅测试用，无并发安全考虑）。
  // RingBuffer 含 std::mutex（不可拷贝/移动），用 resize_for_test 重建。
  ref_buf_.resize_for_test(capacity);
  cmp_buf_.resize_for_test(capacity);
}

void RefComparator::set_stale_threshold_for_test(
    std::chrono::milliseconds threshold) {
  stale_threshold_ = threshold;
}

}  // namespace noise

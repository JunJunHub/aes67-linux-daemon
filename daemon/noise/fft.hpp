// daemon/noise/fft.hpp
// 轻量自包含 FFT，供 DTLN（512 点，radix-2）与 DeepFilterNet（960 点，
// 非二次幂，Bluestein）adapter 复用。不依赖外部 FFT 库（kiss_fft 仅
// rnnoise 内部，未作独立 target 暴露；自实现避免引入耦合）。
//
// 实现要点：
//   - radix-2 迭代 Cooley-Tukey（bit-reversal + 蝶形），O(N log N)，仅
//     支持 N 为 2 的幂。
//   - Bluestein 算法：对任意 N，用 chirp-z 变换归约为 2 的幂的循环卷积，
//     再调 radix-2。N=960=2^6×3×5 用此路径。
//   - rfft/irfft 语义对齐 numpy.fft：rfft 返回前 N/2+1 个频点（正频率），
//     正变换不归一化；irfft 逆变换乘 1/N。与 DTLN/DFN 参考实现一致。
//
// 数值约定（与 numpy.fft 对齐，确保 adapter 输出与 Python 参考一致）：
//   DFT   : X[k] = Σ_{n=0}^{N-1} x[n] · e^{-i 2π k n / N}
//   IDFT  : x[n] = (1/N) Σ_{k=0}^{N-1} X[k] · e^{+i 2π k n / N}
#ifndef NOISE_FFT_HPP_
#define NOISE_FFT_HPP_

#include <complex>
#include <cstddef>
#include <cmath>
#include <vector>

namespace noise {
namespace fft {

using Complex = std::complex<float>;
constexpr float kPi = 3.14159265358979323846f;

// 返回 v 是否为 2 的幂。
inline bool IsPow2(size_t v) { return v > 0 && (v & (v - 1)) == 0; }

// in-place radix-2 迭代 FFT。sign=-1 正变换，sign=+1 逆变换（含 1/N 归一化）。
// 仅支持 n 为 2 的幂。蝶形方向：标准 Cooley-Tukey 时间抽取。
inline void FftRadix2(std::vector<Complex>& a, int sign) {
  const size_t n = a.size();
  // bit-reversal 重排
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j)
      std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const float ang = sign * (-2.0f * kPi) / static_cast<float>(len);
    const Complex wlen(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      Complex w(1.0f, 0.0f);
      for (size_t j = 0; j < len / 2; ++j) {
        const Complex u = a[i + j];
        const Complex v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        w *= wlen;
      }
    }
  }
  if (sign > 0) {
    const float inv = 1.0f / static_cast<float>(n);
    for (auto& x : a)
      x *= inv;
  }
}

// Bluestein 算法：对任意 N 的 FFT。用 chirp-z 归约为长度 M >= 2N-1 的
// 循环卷积（M 取 >= 2N-1 的最小 2 的幂），再调 radix-2。
// sign=-1 正变换（不归一化），sign=+1 逆变换（1/N 归一化）。
inline void FftBluestein(std::vector<Complex>& a, int sign) {
  const size_t n = a.size();
  if (n <= 1)
    return;
  // 卷积长度 M >= 2N-1，取最小 2 的幂。
  size_t m = 1;
  while (m < 2 * n - 1)
    m <<= 1;

  // chirp 因子 w[k] = exp(sign * -i π k² / N)（bluestein 标准定义）。
  // a 与 b 的卷积 = ifft(fft(a)·fft(b))，其中 b 为 w 的共轭卷积核。
  std::vector<Complex> fa(m, Complex(0, 0));
  std::vector<Complex> fb(m, Complex(0, 0));
  for (size_t k = 0; k < n; ++k) {
    // angle = sign * -π k² / N  （平方模 n 防 overflow）
    const float k2 = static_cast<float>((k * k) % (2 * n));
    const float ang = sign * (-kPi * k2) / static_cast<float>(n);
    const Complex w(std::cos(ang), std::sin(ang));
    fa[k] = a[k] * w;
    fb[k] = w;  // w[k]
    if (k != 0)
      fb[m - k] = w;  // 共轭对称延拓（w[-k]=w[k] 因 k² 对称）
  }
  FftRadix2(fa, -1);
  FftRadix2(fb, -1);
  for (size_t i = 0; i < m; ++i)
    fa[i] *= fb[i];
  FftRadix2(fa, +1);  // 逆变换（含 1/M 归一化）

  // 提取前 N 点并乘 w[k] 完成变换。
  for (size_t k = 0; k < n; ++k) {
    const float k2 = static_cast<float>((k * k) % (2 * n));
    const float ang = sign * (-kPi * k2) / static_cast<float>(n);
    const Complex w(std::cos(ang), std::sin(ang));
    a[k] = fa[k] * w;
  }
  if (sign > 0) {
    const float inv = 1.0f / static_cast<float>(n);
    for (auto& x : a)
      x *= inv;
  }
}

// 通用复 FFT（任意 N）：二次幂走 radix-2，否则 Bluestein。
// sign=-1 正变换（不归一化，对齐 numpy.fft.fft），sign=+1 逆变换（1/N）。
inline void Fft(std::vector<Complex>& a, int sign) {
  if (a.size() <= 1)
    return;
  if (IsPow2(a.size()))
    FftRadix2(a, sign);
  else
    FftBluestein(a, sign);
}

// rfft：实序列 N 点正变换 -> 前 N/2+1 个复频点（对齐 numpy.fft.rfft）。
// 输入 real 长度 N（任意），输出复 vector 长度 N/2+1。
inline std::vector<Complex> Rfft(const float* real, size_t n) {
  std::vector<Complex> a(n);
  for (size_t i = 0; i < n; ++i)
    a[i] = Complex(real[i], 0.0f);
  Fft(a, -1);
  a.resize(n / 2 + 1);
  return a;
}

// irfft：前 nbins 个复频点 -> N 点实序列（对齐 numpy.fft.irfft）。
// n_out 为目标时域长度（必须 >= 2*(nbins-1)；通常 n_out=2*(nbins-1)）。
// 重建 Hermitian 对称谱后逆变换取实部。
inline std::vector<float> Irfft(const Complex* spec, size_t nbins, size_t n_out) {
  std::vector<Complex> full(n_out, Complex(0, 0));
  for (size_t k = 0; k < nbins && k < n_out; ++k)
    full[k] = spec[k];
  // Hermitian 对称：full[N-k] = conj(full[k])，k=1..N/2-1。
  for (size_t k = 1; k < n_out - k; ++k) {
    if (n_out - k < n_out)
      full[n_out - k] = std::conj(full[k]);
  }
  Fft(full, +1);  // 逆变换含 1/N 归一化
  std::vector<float> out(n_out);
  for (size_t i = 0; i < n_out; ++i)
    out[i] = full[i].real();
  return out;
}

}  // namespace fft
}  // namespace noise

#endif  // NOISE_FFT_HPP_

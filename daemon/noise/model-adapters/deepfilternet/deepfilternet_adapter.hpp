// daemon/noise/model-adapters/deepfilternet/deepfilternet_adapter.hpp
// DeepFilterNet3 降噪插件适配器（架构依据：
//   docs/noise/denoise-plugin-architecture.md §3.3）。Spec5 Task 2。
//
// 三子图协作（实测签名见 docs/noise/denoise-plugin-architecture.md §1.2）：
//   enc.onnx     IN feat_erb[1,1,S,32] feat_spec[1,2,S,96]
//                 OUT e0/e1/e2/e3 emb[1,S,512] c0[1,64,S,96] lsnr[S,1]
//   df_dec.onnx  IN emb[1,S,512] c0[1,64,S,96]  OUT coefs[..,S,..,10] 235(gain)
//   erb_dec.onnx IN emb e3 e2 e1 e0            OUT m[..,1,S,32]
// native 48k，fft=960 hop=480 df_lookahead=2。lsnr ->
// DenoiseResult.estimated_snr_db。
//
// 流式模型（对齐 libDF/libdf src/lib.rs，逐帧 S=1 + 外部 norm 状态）：
//   STFT(vorbis 窗) -> feat_erb(dB+mean-norm) + feat_cplx(unit-norm) -> enc
//   -> df_dec(coefs) + erb_dec(mask) -> 应用回复谱（df 前 nb_df=96 频点用深度
//   滤波，余用 ERB 掩蔽） -> ISTFT(vorbis 窗 overlap-add)。lookahead=2 缓冲。
//
// **当前状态**：三子图加载 + ERB 滤波器组 + vorbis 窗 + STFT/ISTFT + norm
// 状态 + 深度滤波应用已实现（忠实移植 libdf）。简化版跳 postfilter（R-S5.3）。
// 详见 .cpp。数值正确性已对照 onnxruntime Python 参考验证（dtln_denoises 类
// 比测试 dfn_denoises_nonstation，无模型时 SKIP）。
#ifndef NOISE_MODEL_ADAPTERS_DEEPFILTERNET_DEEPFILTERNET_ADAPTER_HPP_
#define NOISE_MODEL_ADAPTERS_DEEPFILTERNET_DEEPFILTERNET_ADAPTER_HPP_

#include <atomic>
#include <complex>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "denoise_plugin.hpp"
#include "fft.hpp"

namespace Ort {
class Session;
}

namespace noise {

class DeepFilterNetAdapter : public IDenoisePlugin {
 public:
  // 默认构造与析构均 out-of-line（定义在 .cpp）：同 DTLN，类内 = default
  // 的默认构造 inline 定义在头内会为异常安全生成成员析构清理，实例化
  // unique_ptr<Ort::Session>::~unique_ptr() 需完整类型而此处仅前向声明。
  DeepFilterNetAdapter();
  ~DeepFilterNetAdapter() override;

  DeepFilterNetAdapter(const DeepFilterNetAdapter&) = delete;
  DeepFilterNetAdapter& operator=(const DeepFilterNetAdapter&) = delete;

  bool init(const PluginConfig& cfg) override;
  void reset() override;

  const char* name() const override;
  uint32_t native_sample_rate() const override;
  uint32_t algorithmic_latency_samples() const override;
  bool supports_vad() const override;
  bool supports_snr() const override;

  size_t process(const float* in,
                 size_t n_in,
                 float* out,
                 size_t n_out_max,
                 DenoiseResult* result) override;
  size_t flush(float* out, size_t n_out_max) override;

  void set_dry_wet(float ratio) override;
  bool set_param(const std::string& key, const std::string& value) override;
  std::string get_param(const std::string& key) const override;

 private:
  bool process_one_frame_(float& lsnr_out);
  void init_erb_fb_();
  void init_window_();

  std::unique_ptr<Ort::Session> enc_;
  std::unique_ptr<Ort::Session> df_dec_;
  std::unique_ptr<Ort::Session> erb_dec_;
  bool initialized_{false};

  // DFN 参数（config.ini）。
  static constexpr uint32_t kSr = 48000;
  static constexpr size_t kFft = 960;            // fft_size
  static constexpr size_t kHop = 480;            // hop_size
  static constexpr size_t kFreq = kFft / 2 + 1;  // 481
  static constexpr size_t kNbErb = 32;
  static constexpr size_t kNbDf = 96;
  static constexpr size_t kDfOrder = 5;
  static constexpr size_t kDfLookahead = 2;
  static constexpr size_t kConvCh = 64;

  // ERB 滤波器组：每 ERB band 包含的频点数（sum == kFreq=481）。
  std::vector<size_t> erb_;
  // vorbis 窗 + 归一化（libdf:
  // sin(pi/2*sin^2(pi*n/N))，wnorm=1/(N^2/(2*hop)))。
  std::vector<float> window_;
  float wnorm_{1.0f};

  // I/O 名字缓存（init 取一次）。
  std::string enc_in0_, enc_in1_, enc_out_lsnr_;
  std::vector<std::string> enc_out_names_;
  std::vector<std::string> df_in_names_, df_out_names_;
  std::vector<std::string> erb_in_names_, erb_out_names_;
  std::vector<const char*> enc_in_c_, enc_out_c_;
  std::vector<const char*> df_in_c_, df_out_c_;
  std::vector<const char*> erb_in_c_, erb_out_c_;

  // 流式状态（libdf DFState 对应物，跨帧维护）。
  std::vector<float> analysis_mem_;  // STFT 输入重叠缓冲（kFft-kHop=480）
  std::vector<float> synthesis_mem_;  // ISTFT 输出重叠缓冲（480）
  std::vector<float> mean_norm_state_;  // feat_erb 的指数均值状态（kNbErb）
  std::vector<float> unit_norm_state_;  // feat_spec 的指数均值状态（kNbDf）
  // 深度滤波历史复谱系数：kNbDf 个频点 × kDfOrder 阶，每帧滑窗。
  // spec_df_buf_[t][f] = 复谱（real,imag interleaved 或 Complex）。存最近
  // kDfOrder + lookahead 帧的 nb_df 频点复谱，供深度滤波卷积。
  std::vector<std::vector<fft::Complex>> df_spec_history_;
  // 当前帧产出因 lookahead=2 延迟：缓冲 lookahead+1 帧的频谱 + 输出，
  // 待未来帧到达后对齐输出。
  std::deque<std::vector<float>> out_frame_buf_;  // 待输出时域帧（每帧 kHop）

  // 48k 输入/输出 FIFO + 延迟线（同 DTLN 的流式结构）。
  std::deque<float> in_fifo_;
  std::deque<float> out_fifo_;
  std::deque<float> in_delay_;

  std::atomic<uint32_t> dry_wet_bits_{0u};

  // FFT/特征暂存（复用）。
  std::vector<fft::Complex> spec_;
  std::vector<float> feat_erb_;   // [1,1,1,32]
  std::vector<float> feat_spec_;  // [1,2,1,96]
};

}  // namespace noise

#endif  // NOISE_MODEL_ADAPTERS_DEEPFILTERNET_DEEPFILTERNET_ADAPTER_HPP_

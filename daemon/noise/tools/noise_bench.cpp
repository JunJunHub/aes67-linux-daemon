// daemon/noise/tools/noise_bench.cpp
// 降噪质量 + 并发性能基准工具。
//
// 用法：
//   ./noise_bench --plugin <rnnoise|dtln|deepfilternet> \
//                 --input <wav> [--output <wav>] \
//                 [--model-dir <dir>] [--threads <N>] [--dry-wet <0-1>]
//
// 单线程（--threads 1，默认）：处理 input -> output，输出 RTF。
// 多线程（--threads N）：N 线程并发处理同一 input（不写 output），
//   输出总 RTF + 推算最大并发路数。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "denoise_plugin_factory.hpp"
#include "denoise_plugin.hpp"

// ── WAV 读写（简单 RIFF/PCM16 mono，无外部依赖）──────────────────────────

struct WavData {
  uint32_t sample_rate{0};
  uint16_t channels{0};
  std::vector<float> samples;  // interleaved float [-1, 1]
};

static bool read_wav(const std::string& path, WavData& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return false;
  char riff[4], wave[4];
  uint32_t file_size;
  f.read(riff, 4);
  f.read(reinterpret_cast<char*>(&file_size), 4);
  f.read(wave, 4);
  if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0)
    return false;

  uint16_t audio_format = 0, channels = 0, bits = 0;
  uint32_t sample_rate = 0, data_size = 0;
  bool have_fmt = false, have_data = false;

  while (f && (!have_fmt || !have_data)) {
    char chunk_id[4];
    uint32_t chunk_size;
    f.read(chunk_id, 4);
    f.read(reinterpret_cast<char*>(&chunk_size), 4);
    if (!f)
      break;
    if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
      f.read(reinterpret_cast<char*>(&audio_format), 2);
      f.read(reinterpret_cast<char*>(&channels), 2);
      f.read(reinterpret_cast<char*>(&sample_rate), 4);
      f.seekg(4 + 2, std::ios::cur);  // byte_rate + block_align
      f.read(reinterpret_cast<char*>(&bits), 2);
      if (chunk_size > 16)
        f.seekg(chunk_size - 16, std::ios::cur);
      have_fmt = true;
    } else if (std::memcmp(chunk_id, "data", 4) == 0) {
      data_size = chunk_size;
      have_data = true;
      break;
    } else {
      f.seekg(chunk_size, std::ios::cur);
    }
  }

  if (!have_fmt || !have_data || audio_format != 1 || bits != 16)
    return false;

  std::vector<int16_t> pcm(data_size / 2);
  f.read(reinterpret_cast<char*>(pcm.data()), data_size);

  out.sample_rate = sample_rate;
  out.channels = channels;
  out.samples.resize(pcm.size());
  for (size_t i = 0; i < pcm.size(); ++i)
    out.samples[i] = static_cast<float>(pcm[i]) / 32768.0f;
  return true;
}

static bool write_wav(const std::string& path, const WavData& wav) {
  std::ofstream f(path, std::ios::binary);
  if (!f)
    return false;
  std::vector<int16_t> pcm(wav.samples.size());
  for (size_t i = 0; i < wav.samples.size(); ++i) {
    float s = std::max(-1.0f, std::min(1.0f, wav.samples[i]));
    pcm[i] = static_cast<int16_t>(s * 32767.0f);
  }
  uint32_t data_size = pcm.size() * 2;
  uint16_t channels = wav.channels ? wav.channels : 1;
  uint16_t bits = 16;
  uint32_t byte_rate = wav.sample_rate * channels * bits / 8;
  uint16_t block_align = channels * bits / 8;
  f.write("RIFF", 4);
  uint32_t file_size = 36 + data_size;
  f.write(reinterpret_cast<char*>(&file_size), 4);
  f.write("WAVE", 4);
  f.write("fmt ", 4);
  uint32_t fmt_size = 16;
  f.write(reinterpret_cast<char*>(&fmt_size), 4);
  uint16_t audio_format = 1;
  f.write(reinterpret_cast<char*>(&audio_format), 2);
  f.write(reinterpret_cast<char*>(&channels), 2);
  f.write(reinterpret_cast<const char*>(&wav.sample_rate), 4);
  f.write(reinterpret_cast<char*>(&byte_rate), 4);
  f.write(reinterpret_cast<char*>(&block_align), 2);
  f.write(reinterpret_cast<char*>(&bits), 2);
  f.write("data", 4);
  f.write(reinterpret_cast<char*>(&data_size), 4);
  f.write(reinterpret_cast<const char*>(pcm.data()), data_size);
  return f.good();
}

// ── 降噪处理 ─────────────────────────────────────────────────────────────

static constexpr size_t kFrameSize = 480;  // 匹配 daemon noise frame size

struct BenchResult {
  double duration_sec{0};
  double audio_sec{0};
  double rtf{0};
  size_t total_samples{0};
  bool ok{false};
};

static BenchResult process_single(noise::IDenoisePlugin* plugin,
                                  const float* in,
                                  size_t n_samples,
                                  std::vector<float>* out_buf = nullptr) {
  BenchResult r;
  r.audio_sec = static_cast<double>(n_samples) / 48000.0;
  r.total_samples = n_samples;

  auto t0 = std::chrono::steady_clock::now();

  std::vector<float> out_buf_local;
  auto& out = out_buf ? *out_buf : out_buf_local;
  out.clear();
  out.reserve(n_samples + plugin->algorithmic_latency_samples() + kFrameSize);

  noise::DenoiseResult dr;
  std::vector<float> frame_out(kFrameSize +
                               plugin->algorithmic_latency_samples());

  size_t pos = 0;
  int frame_idx = 0;
  while (pos < n_samples) {
    size_t n_in = std::min(kFrameSize, n_samples - pos);
    size_t n_out = plugin->process(in + pos, n_in, frame_out.data(),
                                   frame_out.size(), &dr);
    if (getenv("BENCH_DEBUG") && frame_idx < 15)
      std::fprintf(stderr, "  frame %d: n_in=%zu n_out=%zu status=%d\n",
                   frame_idx, n_in, n_out, static_cast<int>(dr.status));
    out.insert(out.end(), frame_out.data(), frame_out.data() + n_out);
    pos += n_in;
    ++frame_idx;
  }
  // flush 残余
  size_t n_flush = plugin->flush(frame_out.data(), frame_out.size());
  out.insert(out.end(), frame_out.data(), frame_out.data() + n_flush);

  // 长度对齐：截断或补零到输入长度（PESQ/STOI 要求 clean/denoised 等长）。
  if (out.size() > n_samples)
    out.resize(n_samples);
  else if (out.size() < n_samples)
    out.resize(n_samples, 0.0f);

  auto t1 = std::chrono::steady_clock::now();
  r.duration_sec = std::chrono::duration<double>(t1 - t0).count();
  r.rtf = r.audio_sec > 0 ? r.duration_sec / r.audio_sec : 0;
  r.ok = true;
  return r;
}

// ── 命令行 ───────────────────────────────────────────────────────────────

struct Args {
  std::string plugin = "rnnoise";
  std::string input;
  std::string output;
  std::string model_dir = "./noise_models";
  int threads = 1;
  float dry_wet = 1.0f;
};

static Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&]() -> std::string {
      return (i + 1 < argc) ? argv[++i] : "";
    };
    if (arg == "--plugin")
      a.plugin = next();
    else if (arg == "--input")
      a.input = next();
    else if (arg == "--output")
      a.output = next();
    else if (arg == "--model-dir")
      a.model_dir = next();
    else if (arg == "--threads")
      a.threads = std::stoi(next());
    else if (arg == "--dry-wet")
      a.dry_wet = std::stof(next());
    else {
      std::fprintf(stderr, "Unknown arg: %s\n", arg.c_str());
      std::exit(1);
    }
  }
  if (a.input.empty()) {
    std::fprintf(stderr,
                 "Usage: %s --plugin <rnnoise|dtln|deepfilternet> "
                 "--input <wav> [--output <wav>] [--model-dir <dir>] "
                 "[--threads <N>] [--dry-wet <0-1>]\n",
                 argv[0]);
    std::exit(1);
  }
  return a;
}

int main(int argc, char** argv) {
  Args args = parse_args(argc, argv);

  // 读取输入 WAV
  WavData wav;
  if (!read_wav(args.input, wav)) {
    std::fprintf(stderr, "Failed to read WAV: %s\n", args.input.c_str());
    return 1;
  }
  if (wav.channels != 1) {
    std::fprintf(stderr, "Only mono WAV supported (got %u channels)\n",
                 wav.channels);
    return 1;
  }
  std::printf("Input: %s  sr=%u  samples=%zu  dur=%.2fs\n", args.input.c_str(),
              wav.sample_rate, wav.samples.size(),
              static_cast<double>(wav.samples.size()) / wav.sample_rate);

  if (args.threads == 1) {
    // ── 单线程：处理 + 写 output ──
    auto plugin = noise::DenoisePluginRegistry::instance().create(args.plugin);
    if (!plugin) {
      std::fprintf(stderr, "Unknown plugin: %s\n", args.plugin.c_str());
      return 1;
    }
    noise::PluginConfig cfg;
    cfg.sample_rate_in = wav.sample_rate;
    cfg.dry_wet = args.dry_wet;
    cfg.onnx_model_dir = args.model_dir;
    if (!plugin->init(cfg)) {
      std::fprintf(stderr, "Plugin init failed: %s\n", args.plugin.c_str());
      return 1;
    }
    plugin->set_dry_wet(args.dry_wet);
    std::printf("Plugin: %s  latency=%u samples\n", plugin->name(),
                plugin->algorithmic_latency_samples());

    std::vector<float> out;
    BenchResult r = process_single(plugin.get(), wav.samples.data(),
                                   wav.samples.size(), &out);
    if (!r.ok) {
      std::fprintf(stderr, "Processing failed\n");
      return 1;
    }

    // 写 output
    if (!args.output.empty()) {
      WavData out_wav;
      out_wav.sample_rate = wav.sample_rate;
      out_wav.channels = 1;
      out_wav.samples = out;
      if (!write_wav(args.output, out_wav)) {
        std::fprintf(stderr, "Failed to write WAV: %s\n", args.output.c_str());
        return 1;
      }
      std::printf("Output: %s  samples=%zu\n", args.output.c_str(), out.size());
    }

    std::printf("Duration: %.3fs  Audio: %.3fs  RTF: %.4f\n", r.duration_sec,
                r.audio_sec, r.rtf);
    std::printf("Throughput: %.1fx real-time\n", r.rtf > 0 ? 1.0 / r.rtf : 0);
  } else {
    // ── 多线程并发测试 ──
    int nproc = std::thread::hardware_concurrency();
    std::printf("Threads: %d  CPU cores: %d\n", args.threads, nproc);
    std::printf("Plugin: %s\n", args.plugin.c_str());

    std::atomic<int> init_ok{0};
    std::atomic<int> init_fail{0};
    std::vector<std::thread> threads;
    std::vector<BenchResult> results(args.threads);
    std::vector<std::unique_ptr<noise::IDenoisePlugin>> plugins(args.threads);

    auto t0 = std::chrono::steady_clock::now();

    for (int t = 0; t < args.threads; ++t) {
      threads.emplace_back([&, t]() {
        auto plugin =
            noise::DenoisePluginRegistry::instance().create(args.plugin);
        if (!plugin) {
          init_fail++;
          return;
        }
        noise::PluginConfig cfg;
        cfg.sample_rate_in = wav.sample_rate;
        cfg.dry_wet = args.dry_wet;
        cfg.onnx_model_dir = args.model_dir;
        if (!plugin->init(cfg)) {
          init_fail++;
          return;
        }
        plugin->set_dry_wet(args.dry_wet);
        plugins[t] = std::move(plugin);
        init_ok++;

        results[t] = process_single(plugins[t].get(), wav.samples.data(),
                                    wav.samples.size());
      });
    }

    for (auto& th : threads)
      th.join();

    auto t1 = std::chrono::steady_clock::now();
    double wall = std::chrono::duration<double>(t1 - t0).count();

    if (init_fail > 0) {
      std::fprintf(stderr, "Init failures: %d\n", init_fail.load());
      return 1;
    }

    double total_audio = 0, max_duration = 0;
    for (int t = 0; t < args.threads; ++t) {
      total_audio += results[t].audio_sec;
      max_duration = std::max(max_duration, results[t].duration_sec);
    }

    double rtf_wall = wall / total_audio;  // 并发 RTF
    double max_concurrency = rtf_wall > 0 ? 1.0 / rtf_wall : 0;

    std::printf("\n--- Concurrency Results ---\n");
    std::printf("Wall time:      %.3fs\n", wall);
    std::printf("Total audio:    %.3fs (%d x %.2fs)\n", total_audio,
                args.threads, wav.samples.size() / 48000.0);
    std::printf("Per-thread avg: %.3fs\n",
                max_duration);  // 最慢线程的耗时
    std::printf("Concurrent RTF: %.4f (wall/total_audio)\n", rtf_wall);
    std::printf("Max concurrency: ~%.0f channels on %d cores\n",
                max_concurrency, nproc);
    std::printf("Throughput:     %.1fx real-time\n",
                rtf_wall > 0 ? 1.0 / rtf_wall : 0);
  }

  return 0;
}

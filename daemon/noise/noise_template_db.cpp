// daemon/noise/noise_template_db.cpp
// L2 模板匹配实现:余弦相似度 + 0.75 阈值。
// 架构依据:docs/noise/architecture-design.md §3.3.5 L534-542。
// Spec3 Task 4:load(dir)/save(dir) 持久化（arch §7.5）。
#include "noise_template_db.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include <boost/foreach.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "noise_analyzer.hpp"  // compute_bark_spectrum (Spec3 Task 5)
#include "noise_status.hpp"    // write_atomic

namespace noise {

namespace {
// 计算两个 32 维向量的余弦相似度 dot(a,b) / (|a| * |b|)。
// 零范数守卫:|a|==0 或 |b|==0 时返回 0(避免 div-by-zero)。
float cosine_similarity(const std::array<float, 32>& a,
                        const std::array<float, 32>& b) {
  float dot = 0.0f;
  float norm_a = 0.0f;
  float norm_b = 0.0f;
  for (size_t i = 0; i < 32; ++i) {
    dot += a[i] * b[i];
    norm_a += a[i] * a[i];
    norm_b += b[i] * b[i];
  }
  norm_a = std::sqrt(norm_a);
  norm_b = std::sqrt(norm_b);
  if (norm_a == 0.0f || norm_b == 0.0f) {
    return 0.0f;
  }
  return dot / (norm_a * norm_b);
}
}  // namespace

uint32_t NoiseTemplateDB::add_template(
    const std::string& name,
    const std::array<float, 32>& bark_features) {
  return add_template(name, bark_features, "", "");
}

uint32_t NoiseTemplateDB::add_template(
    const std::string& name,
    const std::array<float, 32>& bark_features,
    const std::string& description,
    const std::string& wav_file) {
  Template t;
  t.template_id = next_id_++;
  t.name = name;
  t.bark_features = bark_features;
  t.description = description;
  t.wav_file = wav_file;
  templates_.push_back(std::move(t));
  return templates_.back().template_id;
}

std::pair<uint32_t, float> NoiseTemplateDB::match(
    const std::array<float, 32>& bark_spectrum) {
  if (templates_.empty()) {
    return {0, 0.0f};
  }
  uint32_t best_id = 0;
  float best_sim = 0.0f;
  for (const auto& t : templates_) {
    float sim = cosine_similarity(bark_spectrum, t.bark_features);
    if (sim > best_sim) {
      best_sim = sim;
      best_id = t.template_id;
    }
  }
  // 阈值守卫:最高相似度必须 > 0.75 才判为匹配,否则归 Unknown 返回 (0, 0)。
  if (best_sim > kMatchThreshold) {
    return {best_id, best_sim};
  }
  return {0, 0.0f};
}

bool NoiseTemplateDB::remove_template(uint32_t template_id) {
  auto it = std::find_if(templates_.begin(), templates_.end(),
                         [template_id](const Template& t) {
                           return t.template_id == template_id;
                         });
  if (it == templates_.end()) {
    return false;
  }
  templates_.erase(it);
  return true;
}

std::vector<std::pair<uint32_t, std::string>> NoiseTemplateDB::list_templates()
    const {
  std::vector<std::pair<uint32_t, std::string>> result;
  result.reserve(templates_.size());
  for (const auto& t : templates_) {
    result.emplace_back(t.template_id, t.name);
  }
  return result;
}

const Template* NoiseTemplateDB::get_template(uint32_t template_id) const {
  for (const auto& t : templates_) {
    if (t.template_id == template_id)
      return &t;
  }
  return nullptr;
}

bool NoiseTemplateDB::update_template(uint32_t template_id,
                                      const std::string& new_label,
                                      const std::string& new_description) {
  for (auto& t : templates_) {
    if (t.template_id == template_id) {
      // label/description 仅在非空时覆盖（PUT {"label":"x"} 不清空
      // description， PUT {"description":"y"} 不清空
      // label）。空字符串视为"不更新该字段"， 与 label 的条件式处理一致（review
      // Minor #4）。
      if (!new_label.empty())
        t.name = new_label;
      if (!new_description.empty())
        t.description = new_description;
      return true;
    }
  }
  return false;
}

// ── Spec3 Task 4 持久化实现（arch §7.5）─────────────────────────────────
// JSON 输出 = 手工拼接（与 daemon/json.cpp + noise_http.cpp 同一模式）：
// 数字/bool 不加引号，字符串加引号 + escape_json（共享自 noise_status.hpp，
// review Minor #5 去重）。
// 不用 boost::property_tree::write_json（会将所有值引号化，违反约定）。
// 输入用 boost::property_tree::ptree + read_json（daemon 既有模式）。

bool NoiseTemplateDB::save(const std::string& dir) const {
  if (dir.empty())
    return false;
  // 序列化 templates_ 为 JSON（arch §7.5 格式）：
  // { "templates": [ { "id": 1, "label": "test", "bark_spectrum": [...],
  //                    "description": "...", "wav_file": "..." } ] }
  // Spec3 Task 5：新增 description + wav_file 字段（arch §7.5）。
  std::ostringstream ss;
  ss << "{\n  \"templates\": [";
  for (size_t i = 0; i < templates_.size(); ++i) {
    const auto& t = templates_[i];
    if (i > 0)
      ss << ",";
    ss << "\n    {" << "\n      \"id\": " << t.template_id
       << ",\n      \"label\": \"" << escape_json(t.name) << "\""
       << ",\n      \"bark_spectrum\": [";
    for (size_t j = 0; j < t.bark_features.size(); ++j) {
      if (j > 0)
        ss << ", ";
      // 默认精度 6 位（float 精度足够往返）。
      ss << t.bark_features[j];
    }
    ss << "],\n      \"description\": \"" << escape_json(t.description) << "\""
       << ",\n      \"wav_file\": \"" << escape_json(t.wav_file) << "\""
       << "\n    }";
  }
  ss << "\n  ]\n}\n";
  std::string path = dir + "/templates.json";
  return write_atomic(path, ss.str());
}

bool NoiseTemplateDB::load(const std::string& dir) {
  if (dir.empty())
    return false;
  dir_ = dir;
  std::string path = dir + "/templates.json";
  if (!std::filesystem::exists(path))
    return false;  // 首次启动，无文件
  try {
    boost::property_tree::ptree pt;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
      std::cerr << "NoiseTemplateDB::load: cannot open " << path << std::endl;
      return false;
    }
    boost::property_tree::read_json(in, pt);
    templates_.clear();
    next_id_ = 1;
    BOOST_FOREACH (const boost::property_tree::ptree::value_type& v,
                   pt.get_child("templates")) {
      Template t;
      t.template_id = v.second.get<uint32_t>("id");
      t.name = v.second.get<std::string>("label");
      // Spec3 Task 5：description + wav_file 可缺省（向后兼容 T4 格式）。
      t.description = v.second.get<std::string>("description", "");
      t.wav_file = v.second.get<std::string>("wav_file", "");
      std::array<float, 32> feat{};
      size_t idx = 0;
      BOOST_FOREACH (const boost::property_tree::ptree::value_type& f,
                     v.second.get_child("bark_spectrum")) {
        if (idx < 32)
          feat[idx++] = f.second.get_value<float>();
      }
      t.bark_features = feat;
      templates_.push_back(std::move(t));
      if (t.template_id >= next_id_)
        next_id_ = t.template_id + 1;
    }
    return true;
  } catch (const boost::property_tree::json_parser::json_parser_error& je) {
    std::cerr << "NoiseTemplateDB::load: JSON parse error at line " << je.line()
              << ": " << je.message() << std::endl;
    return false;
  } catch (const std::exception& e) {
    std::cerr << "NoiseTemplateDB::load: error: " << e.what() << std::endl;
    return false;
  }
}

// ── Spec3 Task 5：WAV 录入（arch §7.7）─────────────────────────────────
// 最小 RIFF/fmt/data chunk 解析器（PCM 16-bit）。
// 仅支持 48kHz PCM-16 单声道（Phase 1 限定，arch §11 风险1）。
// 成功：out_samples 填充 float PCM（int16 /32768，匹配 RNNoise scaling），
//   out_sample_rate = 48000。失败：返回 false。
namespace {
// 从字节数组读取 little-endian uint16/uint32（RIFF 全小端）。
uint16_t rd_u16le(const unsigned char* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t rd_u32le(const unsigned char* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
}  // namespace

// 解析 WAV 二进制：验证 RIFF/WAVE 标识 + 找 fmt + data chunk。
// Phase 1 限定：仅 PCM(PCM_code=1) + 16-bit + 单声道 + 48kHz。
// Spec3 Task 5：公开为 noise::parse_wav_pcm16_48k_mono（供 HTTP /test 路径
// 复用，DRY）。
bool parse_wav_pcm16_48k_mono(const std::string& wav_bytes,
                              std::vector<float>& out_samples,
                              uint32_t& out_sample_rate) {
  const size_t min_hdr = 44;  // RIFF(12) + fmt(24) + data(8) 最小
  if (wav_bytes.size() < min_hdr)
    return false;
  const auto* p = reinterpret_cast<const unsigned char*>(wav_bytes.data());
  // RIFF header
  if (std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0)
    return false;
  // 遍历 chunks（从 offset 12 开始）
  size_t pos = 12;
  uint16_t audio_format = 0;
  uint16_t num_channels = 0;
  uint32_t sample_rate = 0;
  uint16_t bits_per_sample = 0;
  const unsigned char* data_ptr = nullptr;
  uint32_t data_size = 0;
  while (pos + 8 <= wav_bytes.size()) {
    const unsigned char* ch = p + pos;
    uint32_t chunk_size = rd_u32le(ch + 4);
    if (std::memcmp(ch, "fmt ", 4) == 0) {
      if (chunk_size < 16 || pos + 8 + 16 > wav_bytes.size())
        return false;
      audio_format = rd_u16le(ch + 8);
      num_channels = rd_u16le(ch + 10);
      sample_rate = rd_u32le(ch + 12);
      bits_per_sample = rd_u16le(ch + 22);
    } else if (std::memcmp(ch, "data", 4) == 0) {
      data_ptr = ch + 8;
      data_size = chunk_size;
      break;  // data chunk 后通常无更多有效 chunk
    }
    // chunk 对齐到偶数字节
    pos += 8 + chunk_size + (chunk_size & 1u);
  }
  if (audio_format != 1)  // PCM = 1
    return false;
  if (num_channels != 1)  // Phase 1: mono only
    return false;
  if (bits_per_sample != 16)
    return false;
  if (sample_rate != 48000)  // Phase 1: 48kHz only (arch §11 风险1)
    return false;
  if (data_ptr == nullptr || data_size == 0)
    return false;
  // 限制 data_size 不超过剩余字节
  size_t remaining = wav_bytes.size() - (data_ptr - p);
  if (data_size > remaining)
    data_size = static_cast<uint32_t>(remaining);
  size_t num_samples = data_size / 2;  // 16-bit = 2 bytes/sample
  out_samples.resize(num_samples);
  for (size_t i = 0; i < num_samples; ++i) {
    int16_t s = static_cast<int16_t>(rd_u16le(data_ptr + i * 2));
    out_samples[i] = static_cast<float>(s) / 32768.0f;
  }
  out_sample_rate = sample_rate;
  return true;
}

uint32_t NoiseTemplateDB::add_template_from_wav(const std::string& label,
                                                const std::string& description,
                                                const std::string& wav_bytes) {
  if (dir_.empty())
    return 0;
  // 1. 解析 WAV -> float PCM
  std::vector<float> samples;
  uint32_t sample_rate = 0;
  if (!parse_wav_pcm16_48k_mono(wav_bytes, samples, sample_rate))
    return 0;
  // 2. 提取 32 维 Bark（复用 NoiseAnalyzer 的 compute_bark_spectrum，DRY）
  auto bark =
      compute_bark_spectrum(samples.data(), samples.size(), sample_rate);
  // 3. 先 add_template 占位获取 id（wav_file 后填）
  uint32_t id = add_template(label, bark, description, "");
  if (id == 0)
    return 0;
  // 4. 写 WAV 文件到 dir_/template-<id>.wav
  std::string wav_name = "template-" + std::to_string(id) + ".wav";
  std::string wav_path = dir_ + "/" + wav_name;
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  std::ofstream out(wav_path, std::ios::binary);
  if (!out.is_open()) {
    // 写文件失败：回滚内存中的 add_template
    remove_template(id);
    return 0;
  }
  out.write(wav_bytes.data(), static_cast<std::streamsize>(wav_bytes.size()));
  out.close();
  // 检查写入是否成功（review Minor #5）：disk-full / mid-write 失败会留下
  // 截断的 WAV 文件，GET /:id/wav 会服务坏文件。失败时回滚（与 open 失败
  // 同一路径）并删除已写的残片。
  if (!out.good()) {
    std::error_code rm_ec;
    std::filesystem::remove(wav_path, rm_ec);
    remove_template(id);
    return 0;
  }
  // 5. 更新 wav_file 字段 + save
  if (auto* t = const_cast<Template*>(get_template(id))) {
    t->wav_file = wav_name;
  }
  if (!save(dir_)) {
    // save 失败不回滚（内存状态正确，仅持久化失败；调用方应告警）
    std::cerr << "NoiseTemplateDB::add_template_from_wav: save failed"
              << std::endl;
  }
  return id;
}

}  // namespace noise

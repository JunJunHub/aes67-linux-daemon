// daemon/noise/noise_template_db.cpp
// L2 模板匹配实现:余弦相似度 + 0.75 阈值。
// 架构依据:docs/noise/architecture-design.md §3.3.5 L534-542。
// Spec3 Task 4:load(dir)/save(dir) 持久化（arch §7.5）。
#include "noise_template_db.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include <boost/foreach.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "noise_status.hpp"  // write_atomic

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
  Template t;
  t.template_id = next_id_++;
  t.name = name;
  t.bark_features = bark_features;
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

// ── Spec3 Task 4 持久化实现（arch §7.5）─────────────────────────────────
// JSON 输出 = 手工拼接（与 daemon/json.cpp + noise_http.cpp 同一模式）：
// 数字/bool 不加引号，字符串加引号 + escape_json。
// 不用 boost::property_tree::write_json（会将所有值引号化，违反约定）。
// 输入用 boost::property_tree::ptree + read_json（daemon 既有模式）。
namespace {

// JSON 字符串转义（daemon/json.cpp L42-78 复制，避免跨模块依赖）。
std::string escape_json(const std::string& s) {
  std::ostringstream ss;
  for (auto c = s.cbegin(); c != s.cend(); c++) {
    switch (*c) {
      case '"':
        ss << "\\\"";
        break;
      case '\\':
        ss << "\\\\";
        break;
      case '\b':
        ss << "\\b";
        break;
      case '\f':
        ss << "\\f";
        break;
      case '\n':
        ss << "\\n";
        break;
      case '\r':
        ss << "\\r";
        break;
      case '\t':
        ss << "\\t";
        break;
      default:
        if ('\x00' <= *c && *c <= '\x1f') {
          ss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
             << static_cast<int>(*c);
        } else {
          ss << *c;
        }
    }
  }
  return ss.str();
}

}  // namespace

bool NoiseTemplateDB::save(const std::string& dir) const {
  if (dir.empty())
    return false;
  // 序列化 templates_ 为 JSON（arch §7.5 格式）：
  // { "templates": [ { "id": 1, "label": "test", "bark_spectrum": [...] } ] }
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
    ss << "]\n    }";
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

}  // namespace noise

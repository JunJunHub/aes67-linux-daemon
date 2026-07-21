// daemon/noise/noise_template_db.cpp
// L2 模板匹配实现:余弦相似度 + 0.75 阈值。
// 架构依据:docs/noise/architecture-design.md §3.3.5 L534-542。
#include "noise_template_db.hpp"

#include <algorithm>
#include <cmath>

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

}  // namespace noise

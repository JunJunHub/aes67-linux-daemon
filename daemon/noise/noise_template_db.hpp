// daemon/noise/noise_template_db.hpp
// L2 模板匹配:内存中的 Bark 32 频带噪声模板库 + 余弦相似度匹配。
// 架构依据:docs/noise/architecture-design.md §3.3.5 L534-542。
// Spec2 1.8:Template struct + NoiseTemplateDB 类(内存 store,无
// HTTP/磁盘持久化)。
//
// 决策 1(仅内存 store):本文件不涉及 HTTP API 与磁盘持久化,那是 Spec3 的职责。
// 线程安全:无 locking - Spec3 的 HTTP 层暴露 DB 时再加 mutex 同步访问。
#ifndef NOISE_NOISE_TEMPLATE_DB_HPP_
#define NOISE_NOISE_TEMPLATE_DB_HPP_

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace noise {

// 噪声模板:L2 模板匹配的最小单元。
// bark_features 为归一化前的 32 维 Bark 频带能量(由调用方决定是否归一化,
// 余弦相似度本身对放缩不敏感)。
struct Template {
  uint32_t template_id{0};
  std::string name;
  std::array<float, 32> bark_features{};
};

// L2 模板匹配库(arch §3.3.5)。
// - add_template:分配递增 template_id(从 1 起),存入 vector,返回 id。
// - match:逐一计算余弦相似度,返回最高(>0.75)的 (template_id, similarity);
//   无匹配返回 (0, 0.0f)。
// - remove_template:按 id 删除(不重排 id;next_id_ 持续递增)。
// - list_templates:返回当前所有 (id, name) 对。
class NoiseTemplateDB {
 public:
  // 添加模板,返回分配的 template_id(从 1 起,单调递增)。
  uint32_t add_template(const std::string& name,
                        const std::array<float, 32>& bark_features);

  // 匹配给定 Bark 频谱,返回 (template_id, similarity)。
  // 最高相似度 > 0.75 才视为匹配;否则返回 (0, 0.0f)。
  // 空库直接返回 (0, 0.0f)。零范数守卫:|a|==0 或 |b|==0 时该对相似度记 0。
  std::pair<uint32_t, float> match(const std::array<float, 32>& bark_spectrum);

  // 按 id 删除模板。找到并删除返回 true,未找到返回 false。
  // 不重新压缩 id;next_id_ 持续递增。
  bool remove_template(uint32_t template_id);

  // 返回所有当前模板的 (id, name) 对。
  std::vector<std::pair<uint32_t, std::string>> list_templates() const;

 private:
  std::vector<Template> templates_;
  uint32_t next_id_{1};  // 从 1 起,确保 id > 0

  // 匹配阈值(arch §3.3.5 L540):> 0.75 判为该模板的噪声类型。
  static constexpr float kMatchThreshold = 0.75f;
};

}  // namespace noise

#endif  // NOISE_NOISE_TEMPLATE_DB_HPP_

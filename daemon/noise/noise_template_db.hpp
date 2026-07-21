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
// Spec3 Task 4:load(dir)/save(dir) 持久化到 dir/templates.json(arch §7.5)。
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

  // ── Spec3 Task 4 持久化（arch §7.5）──
  // load(dir)：从 dir/templates.json 读取模板列表，重建内存索引。
  //   dir 空字符串 -> 返回 false。文件不存在 -> 返回
  //   false（非错误，首次启动）。 JSON 解析失败 -> 返回 false 并 stderr 告警。
  bool load(const std::string& dir);
  // save(dir)：序列化 templates_ 为 dir/templates.json via write_atomic。
  //   dir 空字符串 -> 返回 false（no-op）。父目录不存在 -> 自动创建。
  bool save(const std::string& dir) const;
  // 测试钩子（spec §D）：设置内部 dir_ 供 save-on-change 使用。
  // 生产环境由 NoiseManager::load_status 设置。
  void set_dir_for_test(const std::string& dir) { dir_ = dir; }
  const std::string& get_dir_for_test() const { return dir_; }

 private:
  std::vector<Template> templates_;
  uint32_t next_id_{1};  // 从 1 起,确保 id > 0
  // Spec3 Task 4：持久化目录（arch §7.5）。
  // 由 load(dir) / set_dir_for_test 设置，save(dir) 可覆盖参数。
  std::string dir_;

  // 匹配阈值(arch §3.3.5 L540):> 0.75 判为该模板的噪声类型。
  static constexpr float kMatchThreshold = 0.75f;
};

}  // namespace noise

#endif  // NOISE_NOISE_TEMPLATE_DB_HPP_

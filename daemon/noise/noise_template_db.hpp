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

// Spec3 Task 5：最小 RIFF/fmt/data WAV 解析器（PCM 16-bit mono 48kHz）。
// 供 add_template_from_wav + HTTP /template/:id/test 路径复用（DRY）。
// 成功：out_samples 填充 float PCM（int16 /32768），out_sample_rate = 48000。
// 失败（非 PCM / 非 16-bit / 非 mono / 非 48kHz / 格式错误）：返回 false。
// Phase 1 限定（arch §11 风险1）。
bool parse_wav_pcm16_48k_mono(const std::string& wav_bytes,
                              std::vector<float>& out_samples,
                              uint32_t& out_sample_rate);

// 噪声模板:L2 模板匹配的最小单元。
// bark_features 为归一化前的 32 维 Bark 频带能量(由调用方决定是否归一化,
// 余弦相似度本身对放缩不敏感)。
// Spec3 Task 5:新增 description + wav_file 字段（arch §7.5）。
//   wav_file 为相对于 template_dir 的文件名（如 "template-1.wav"），
//   空字符串表示该模板无原始 WAV（如 JSON 导入的模板）。
// Spec4 T1（D-S4.8）:新增 wav_available 字段（additive，向后兼容）。
//   load 时检查 wav_file 对应文件是否存在，缺失则置 false（arch §11 风险15）。
//   bark_spectrum 特征向量保留，L2 匹配仍可用。save 序列化该字段，
//   load 时缺省 = true（向后兼容旧 templates.json）。
struct Template {
  uint32_t template_id{0};
  std::string name;
  std::array<float, 32> bark_features{};
  std::string description;   // Spec3 Task 5（arch §7.5）
  std::string wav_file;      // Spec3 Task 5：相对 template_dir 的文件名
  bool wav_available{true};  // Spec4 T1（D-S4.8）：WAV 文件是否存在
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

  // Spec3 Task 5：带 description/wav_file 的添加（arch §7.5）。
  // wav_file 为相对 template_dir 的文件名；空字符串表示无 WAV（JSON 导入）。
  uint32_t add_template(const std::string& name,
                        const std::array<float, 32>& bark_features,
                        const std::string& description,
                        const std::string& wav_file);

  // 匹配给定 Bark 频谱,返回 (template_id, similarity)。
  // 最高相似度 > 0.75 才视为匹配;否则返回 (0, 0.0f)。
  // 空库直接返回 (0, 0.0f)。零范数守卫:|a|==0 或 |b|==0 时该对相似度记 0。
  std::pair<uint32_t, float> match(const std::array<float, 32>& bark_spectrum);

  // 按 id 删除模板。找到并删除返回 true,未找到返回 false。
  // 不重新压缩 id;next_id_ 持续递增。
  bool remove_template(uint32_t template_id);

  // 返回所有当前模板的 (id, name) 对。
  std::vector<std::pair<uint32_t, std::string>> list_templates() const;

  // Spec3 Task 5：按 id 查找模板详情。未找到返回 nullptr。
  const Template* get_template(uint32_t template_id) const;

  // Spec4 T1（D-S4.7）：返回模板 WAV 文件的完整路径。
  //   wav_available=false 或 wav_file 为空 -> 返回空串（无 WAV 可用）。
  //   否则返回 dir_ + "/" + wav_file。
  //   未找到模板 -> 返回空串。
  std::string get_wav_path(uint32_t template_id) const;

  // Spec3 Task 5：更新 label/description。未找到返回 false。
  bool update_template(uint32_t template_id,
                       const std::string& new_label,
                       const std::string& new_description);

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

  // ── Spec3 Task 5：WAV 录入（arch §7.7）──
  // add_template_from_wav：读 WAV header（RIFF/fmt/data，PCM 16-bit）->
  //   提取 32 维 Bark（复用 NoiseAnalyzer 的 compute_bark_spectrum）->
  //   写 WAV 到 dir/template-<id>.wav -> add_template -> save(dir)。
  //   返回分配的 template_id（>0）；失败返回 0。
  //   Phase 1 限定（arch §11 风险1）：仅支持 48kHz PCM 16-bit 单声道 WAV。
  //   非 48kHz / 非 PCM-16 / mono 检查失败 -> 返回 0（HTTP 层映射为 400）。
  //   dir 空字符串 -> 返回 0（无持久化目录）。
  //   wav_bytes 为完整 WAV 文件二进制（含 RIFF header）。
  uint32_t add_template_from_wav(const std::string& label,
                                 const std::string& description,
                                 const std::string& wav_bytes);

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

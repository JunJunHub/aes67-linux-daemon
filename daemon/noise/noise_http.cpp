// daemon/noise/noise_http.cpp
// Spec3 Task 3：HTTP sensor API
// 实现。架构依据：docs/noise/architecture-design.md §5.1（端点）+
// §5.4（响应字段）。
//
// JSON 序列化模式（照搬 daemon/json.cpp）：
// - 输出：std::stringstream 手工拼接 + escape_json() 转义。数字/bool 不加
//   引号（arch §5.4 示例 `"noise_level_dbfs": -35.0`、`"is_alerting": true`）。
// - 输入（PUT body 解析）：boost::property_tree::ptree + read_json（同
//   daemon/json.cpp 的 json_to_config_ / json_to_source / json_to_sink）。
// - 不用 boost::property_tree::write_json 做输出（该函数将所有值序列化为
//   字符串 `"true"`/`"42"`，违反 arch §5.4 的裸类型约定 + 与 daemon API
//   不一致）。
#include "noise_http.hpp"

#include <boost/foreach.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace noise {

// NoiseType 枚举 -> 小写蛇形字符串（arch §5.4 约定）。
// C++ enum CamelCase 与 JSON 字段名解耦：序列化层负责映射，enum 名不进 JSON。
std::string noise_type_to_string(NoiseType type) {
  switch (type) {
    case NoiseType::Clean:
      return "clean";
    case NoiseType::White:
      return "white";
    case NoiseType::Pink:
      return "pink";
    case NoiseType::Hum50Hz:
      return "hum_50hz";
    case NoiseType::Hum60Hz:
      return "hum_60hz";
    case NoiseType::Impulse:
      return "impulse";
    case NoiseType::Broadband:
      return "broadband";
    case NoiseType::Digital:
      return "digital";
    case NoiseType::Unknown:
    default:
      return "unknown";
  }
}

// ── JSON 输出 helper（照搬 daemon/json.cpp L44-78 escape_json）────────────
// 手工拼接 JSON：数字/bool 不加引号，字符串加引号 + 转义。与 daemon 的
// config_to_json / source_to_json 同一模式。
namespace {

// JSON 字符串转义（daemon/json.cpp L44-78 复制，避免跨模块依赖）。
// 处理 " \\ \b \f \n \r \t + 控制字符 \u00XX。
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

// bool -> JSON 字面量（不加引号）。不用 std::boolalpha 避免影响 stream
// 状态（后续浮点输出会变）。
inline const char* bool_str(bool b) {
  return b ? "true" : "false";
}

// 单个 NoiseMetricsSnapshot 字段追加到 stream（不含外层 {}）。
// indent 是每行前缀（如 "  " 或 "    "），用于嵌套场景的对齐。
// include_denoise_enabled：是否输出 denoise_enabled 字段。
//   true: metrics 独立视图（GET /metrics），输出 denoise_enabled（stale
//   值，仅 collect() 时刷新 - 但这是 /metrics 的语义：返回最新 metrics 快照）。
//   false: sensor 合并视图（GET /sensor/:id），denoise_enabled 由 sensor
//   元信息提供（权威值，Spec3 Task3 review Important #1），不重复输出。
// 字段名严格按 arch §5.4 响应示例。
void append_snapshot_fields(std::stringstream& ss,
                            const NoiseMetricsSnapshot& s,
                            const char* indent,
                            bool include_denoise_enabled) {
  ss << indent << "\"noise_level_dbfs\": " << s.noise_level_dbfs << ",\n"
     << indent << "\"noise_type\": \""
     << escape_json(noise_type_to_string(s.noise_type)) << "\",\n"
     << indent << "\"noise_type_confidence\": " << s.noise_type_confidence
     << ",\n"
     << indent << "\"is_mixed\": " << bool_str(s.is_mixed) << ",\n"
     << indent << "\"estimated_snr_db\": " << s.estimated_snr_db;
  if (include_denoise_enabled) {
    ss << ",\n"
       << indent << "\"denoise_enabled\": " << bool_str(s.denoise_enabled);
  }
  ss << ",\n"
     << indent << "\"denoise_dry_wet\": " << s.denoise_dry_wet << ",\n"
     << indent << "\"noise_reduction_db\": " << s.noise_reduction_db << ",\n"
     << indent << "\"alert_threshold_dbfs\": " << s.alert_threshold_dbfs
     << ",\n"
     << indent << "\"is_alerting\": " << bool_str(s.is_alerting) << ",\n"
     << indent << "\"spectral_centroid_hz\": " << s.spectral_centroid_hz
     << ",\n"
     << indent << "\"spectral_flatness\": " << s.spectral_flatness << ",\n"
     << indent << "\"hum_strength_db\": " << s.hum_strength_db;
}

// noise_candidates 数组追加（arch §5.4 混合噪声示例）。
// 定长 array + count 截断（避免 RT 路径堆分配，见 noise_metrics.hpp）。
void append_candidates_array(std::stringstream& ss,
                             const NoiseMetricsSnapshot& s,
                             const char* indent) {
  ss << indent << "\"noise_candidates\": [";
  for (size_t i = 0; i < s.noise_candidates_count; ++i) {
    if (i > 0)
      ss << ", ";
    ss << "{\"type\": \""
       << escape_json(noise_type_to_string(s.noise_candidates[i].type))
       << "\", \"confidence\": " << s.noise_candidates[i].confidence << "}";
  }
  ss << "]";
}

// 解析 URL path 中的 :id 为 uint8_t。超范围（>255 或负）返回 false 并设置
// 400 响应。防止 static_cast<uint8_t> 截断大整数（如 id=256 -> 0 误命中
// sensor 0）。
bool parse_sensor_id(const httplib::Request& req,
                     httplib::Response& res,
                     uint8_t& out) {
  try {
    int v = std::stoi(req.matches[1]);
    if (v < 0 || v > 255) {
      res.status = 400;
      res.set_content("sensor id out of range (0-255)", "text/plain");
      return false;
    }
    out = static_cast<uint8_t>(v);
    return true;
  } catch (...) {
    res.status = 400;
    res.set_content("invalid id", "text/plain");
    return false;
  }
}
}  // namespace

// ── JSON 输出函数（手工拼接，对齐 daemon/json.cpp 模式）──────────────────

std::string metrics_to_json(const NoiseMetricsSnapshot& snapshot) {
  // /metrics 独立视图：输出全部 metrics 字段（含 denoise_enabled，返回最新
  // metrics 快照，值由 collect() 时刷新）。
  std::stringstream ss;
  ss << "{\n";
  append_snapshot_fields(ss, snapshot, "  ", /*include_denoise_enabled=*/true);
  ss << ",\n";
  append_candidates_array(ss, snapshot, "  ");
  ss << "\n}\n";
  return ss.str();
}

std::string sensor_to_json(const SensorInfo& info) {
  // /sensor/:id 合并视图：sensor 元信息（id/sink_id/enabled/denoise_enabled
  // - 权威值来自 SensorContext）+ metrics 字段（不含 denoise_enabled，避免
  // 与 sensor 元信息的权威值冲突 - Spec3 Task3 review Important #1）。
  std::stringstream ss;
  ss << "{\n  \"id\": " << static_cast<unsigned>(info.id)
     << ",\n  \"sink_id\": " << static_cast<unsigned>(info.sink_id)
     << ",\n  \"enabled\": " << bool_str(info.enabled)
     << ",\n  \"denoise_enabled\": " << bool_str(info.denoise_enabled) << ",\n";
  append_snapshot_fields(ss, info.metrics, "  ",
                         /*include_denoise_enabled=*/false);
  ss << ",\n";
  append_candidates_array(ss, info.metrics, "  ");
  ss << "\n}\n";
  return ss.str();
}

std::string sensors_to_json(
    const std::vector<std::pair<uint8_t, SensorInfo>>& sensors) {
  std::stringstream ss;
  ss << "{\n  \"sensors\": [";
  size_t i = 0;
  for (const auto& [id, info] : sensors) {
    if (i++ > 0)
      ss << ", ";
    // 复用 sensor_to_json 输出单个 sensor 对象。
    ss << sensor_to_json(info);
  }
  ss << "  ]\n}\n";
  return ss.str();
}

std::string history_to_json(const std::vector<NoiseMetricsSnapshot>& history) {
  std::stringstream ss;
  ss << "{\n  \"history\": [";
  size_t i = 0;
  for (const auto& snap : history) {
    if (i++ > 0)
      ss << ", ";
    ss << metrics_to_json(snap);
  }
  ss << "  ]\n}\n";
  return ss.str();
}

// ── 路由注册 ────────────────────────────────────────────────────────────
// 照搬 daemon/http_server.cpp 模式：std::regex ([0-9]+) + req.matches[1]。
// 错误处理：sensor 不存在 -> 404 + text/plain；JSON 解析失败 -> 400 +
// text/plain。 成功：200 + application/json（GET）或 200 + text/plain 空
// body（PUT/DELETE）。
//
// 注：cpp-httplib 的 regex_match 要求完整匹配，/api/noise/sensor/0/metrics
// 不会命中 /api/noise/sensor/([0-9]+)（后者只匹配不含 /metrics 后缀的路径）。
// 顺序无依赖，但更具体的路由先注册以防误匹配。
void register_noise_sensor_routes(httplib::Server& svr, NoiseManager& mgr) {
  using httplib::Request;
  using httplib::Response;

  // GET /api/noise/sensors - 列出所有 sensors（arch §5.1）。
  svr.Get("/api/noise/sensors", [&mgr](const Request& /*req*/, Response& res) {
    auto sensors = mgr.list_sensor_infos();
    res.set_content(sensors_to_json(sensors), "application/json");
  });

  // GET /api/noise/sensor/([0-9]+)/metrics - 最新 metrics 快照。
  // Spec3 Task3 review Minor #4：用 get_metrics_snapshot 而非 get_sensor_info，
  // 聚焦 accessor - 不读 sensor 配置字段（sink_id/enabled 等），仅 metrics。
  svr.Get("/api/noise/sensor/([0-9]+)/metrics",
          [&mgr](const Request& req, Response& res) {
            uint8_t id;
            if (!parse_sensor_id(req, res, id))
              return;
            NoiseMetricsSnapshot snapshot;
            if (!mgr.get_metrics_snapshot(id, snapshot)) {
              res.status = 404;
              res.set_content("sensor not found", "text/plain");
              return;
            }
            res.set_content(metrics_to_json(snapshot), "application/json");
          });

  // GET /api/noise/sensor/([0-9]+)/history - 60s history ring。
  // 注：?duration & ?interval 查询参数留给 Phase 2（D-S3.5 内存 60s 固定）。
  svr.Get("/api/noise/sensor/([0-9]+)/history",
          [&mgr](const Request& req, Response& res) {
            uint8_t id;
            if (!parse_sensor_id(req, res, id))
              return;
            SensorInfo info;
            if (!mgr.get_sensor_info(id, info)) {
              res.status = 404;
              res.set_content("sensor not found", "text/plain");
              return;
            }
            auto hist = mgr.get_history_snapshot(id);
            res.set_content(history_to_json(hist), "application/json");
          });

  // GET /api/noise/sensor/([0-9]+) - sensor 信息 + 最新 metrics 快照（§5.4
  // 示例）。
  svr.Get("/api/noise/sensor/([0-9]+)",
          [&mgr](const Request& req, Response& res) {
            uint8_t id;
            if (!parse_sensor_id(req, res, id))
              return;
            SensorInfo info;
            if (!mgr.get_sensor_info(id, info)) {
              res.status = 404;
              res.set_content("sensor not found", "text/plain");
              return;
            }
            res.set_content(sensor_to_json(info), "application/json");
          });

  // PUT /api/noise/sensor/([0-9]+) - 创建/更新 sensor。
  // Body 字段：sink_id, denoise_enabled, denoise_plugin (-> plugin_name),
  //   denoise_dry_wet, sensitivity, enabled（可选）。
  // 注：NoiseManager::add_sensor 是 upsert（COW 表里若有则覆盖
  // SensorContext），
  //   故 PUT 即创建即更新。enabled 字段单独走 enable_sensor 路由（PUT
  //   后调一次）。
  // Spec3 Task3 review Minor #5：add_sensor 恒返回 true（COW + make_shared
  // 成功或抛异常被外层 catch 捕获为 400），故移除 dead 500 路径。
  // 输入解析用 boost::property_tree（照搬 daemon/json.cpp json_to_source
  // 模式）。
  svr.Put("/api/noise/sensor/([0-9]+)", [&mgr](const Request& req,
                                               Response& res) {
    uint8_t id;
    if (!parse_sensor_id(req, res, id))
      return;
    try {
      boost::property_tree::ptree pt;
      std::stringstream ss(req.body);
      boost::property_tree::read_json(ss, pt);
      NoiseSensorConfig cfg;
      uint8_t sink_id = id;
      // sink_id 可缺省（默认与 sensor_id 相同，简化单 sensor 场景）。
      auto sid_opt = pt.get_optional<uint8_t>("sink_id");
      if (sid_opt)
        sink_id = *sid_opt;
      cfg.denoise_enabled = pt.get<bool>("denoise_enabled", false);
      // denoise_plugin 字段名（HTTP 约定）-> plugin_name（C++ 字段名）。
      cfg.plugin_name = pt.get<std::string>("denoise_plugin", "passthrough");
      cfg.dry_wet = pt.get<float>("denoise_dry_wet", 1.0f);
      cfg.sensitivity = pt.get<float>("sensitivity", 1.0f);
      mgr.add_sensor(id, sink_id, cfg);
      // enabled 可选字段：PUT 时若提供则应用 enable_sensor。
      auto en_opt = pt.get_optional<bool>("enabled");
      if (en_opt)
        mgr.enable_sensor(id, *en_opt);
      res.set_content("", "text/plain");
    } catch (const std::exception& e) {
      res.status = 400;
      res.set_content(std::string("invalid JSON: ") + e.what(), "text/plain");
    }
  });

  // DELETE /api/noise/sensor/([0-9]+) - 删除 sensor。
  svr.Delete("/api/noise/sensor/([0-9]+)",
             [&mgr](const Request& req, Response& res) {
               uint8_t id;
               if (!parse_sensor_id(req, res, id))
                 return;
               if (!mgr.remove_sensor(id)) {
                 res.status = 404;
                 res.set_content("sensor not found", "text/plain");
                 return;
               }
               res.set_content("", "text/plain");
             });
}

// ── Spec3 Task 5：噪声模板 HTTP API（arch §5.3）────────────────────────
// 9 端点：CRUD + WAV 上传 + 匹配测试 + WAV 回听 + 导入导出。
// JSON 输出手写拼接（reuse escape_json），输入用 boost::property_tree。
// Phase 1 限定（arch §11 风险1）：WAV 须 48kHz PCM-16 mono，否则 400。
namespace {

// 单个模板序列化为 JSON 对象（不含外层 {}，由调用方决定缩进）。
// 字段：id, label, description, bark_spectrum, wav_file（arch §7.5）。
std::string template_to_json_object(const Template& t, const char* indent) {
  std::stringstream ss;
  ss << indent << "\"id\": " << t.template_id << ",\n"
     << indent << "\"label\": \"" << escape_json(t.name) << "\"" << ",\n"
     << indent << "\"description\": \"" << escape_json(t.description) << "\""
     << ",\n"
     << indent << "\"bark_spectrum\": [";
  for (size_t j = 0; j < t.bark_features.size(); ++j) {
    if (j > 0)
      ss << ", ";
    ss << t.bark_features[j];
  }
  ss << "],\n"
     << indent << "\"wav_file\": \"" << escape_json(t.wav_file) << "\"";
  return ss.str();
}

// 解析 URL path 中的 :id 为 uint32_t（模板 id 从 1 起，无 255 上限）。
bool parse_template_id(const httplib::Request& req,
                       httplib::Response& res,
                       uint32_t& out) {
  try {
    int v = std::stoi(req.matches[1]);
    if (v <= 0) {
      res.status = 400;
      res.set_content("template id must be positive", "text/plain");
      return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
  } catch (...) {
    res.status = 400;
    res.set_content("invalid id", "text/plain");
    return false;
  }
}
}  // namespace

void register_noise_template_routes(httplib::Server& svr,
                                    NoiseManager& /*mgr*/,
                                    NoiseTemplateDB& template_db) {
  using httplib::Request;
  using httplib::Response;

  // GET /api/noise/templates - 列出所有模板（arch §5.3）。
  svr.Get("/api/noise/templates",
          [&template_db](const Request& /*req*/, Response& res) {
            auto list = template_db.list_templates();
            std::stringstream ss;
            ss << "{\n  \"templates\": [";
            for (size_t i = 0; i < list.size(); ++i) {
              if (i > 0)
                ss << ",";
              ss << "\n    {" << "\n      \"id\": " << list[i].first
                 << ",\n      \"label\": \"" << escape_json(list[i].second)
                 << "\"\n    }";
            }
            ss << "\n  ]\n}\n";
            res.set_content(ss.str(), "application/json");
          });

  // GET /api/noise/templates/export - 导出完整模板库（含 bark_spectrum）。
  svr.Get("/api/noise/templates/export", [&template_db](const Request& /*req*/,
                                                        Response& res) {
    auto list = template_db.list_templates();
    std::stringstream ss;
    ss << "{\n  \"templates\": [";
    bool first = true;
    for (const auto& [id, name] : list) {
      const Template* t = template_db.get_template(id);
      if (t == nullptr)
        continue;
      if (!first)
        ss << ",";
      first = false;
      ss << "\n    {\n" << template_to_json_object(*t, "      ") << "\n    }";
    }
    ss << "\n  ]\n}\n";
    res.set_content(ss.str(), "application/json");
  });

  // POST /api/noise/templates/import - 导入模板库（JSON body，按 label 去重）。
  // 输入格式同 export：{ "templates": [ {label, bark_spectrum, description?,
  // ...} ] } 已存在的 label 跳过（去重）；不导入 wav_file（导入的模板无原始
  // WAV）。
  svr.Post("/api/noise/templates/import", [&template_db](const Request& req,
                                                         Response& res) {
    try {
      boost::property_tree::ptree pt;
      std::stringstream ss(req.body);
      boost::property_tree::read_json(ss, pt);
      int imported = 0;
      int skipped = 0;
      // 收集现有 label 用于去重
      auto existing = template_db.list_templates();
      std::set<std::string> existing_labels;
      for (const auto& [eid, ename] : existing)
        existing_labels.insert(ename);
      BOOST_FOREACH (const boost::property_tree::ptree::value_type& v,
                     pt.get_child("templates")) {
        std::string label = v.second.get<std::string>("label");
        if (existing_labels.count(label) > 0) {
          ++skipped;
          continue;
        }
        std::array<float, 32> feat{};
        size_t idx = 0;
        BOOST_FOREACH (const boost::property_tree::ptree::value_type& f,
                       v.second.get_child("bark_spectrum")) {
          if (idx < 32)
            feat[idx++] = f.second.get_value<float>();
        }
        std::string desc = v.second.get<std::string>("description", "");
        template_db.add_template(label, feat, desc, "");
        existing_labels.insert(label);
        ++imported;
      }
      std::stringstream out;
      out << "{\n  \"imported\": " << imported
          << ",\n  \"skipped\": " << skipped << "\n}\n";
      res.set_content(out.str(), "application/json");
    } catch (const std::exception& e) {
      res.status = 400;
      res.set_content(std::string("invalid JSON: ") + e.what(), "text/plain");
    }
  });

  // GET /api/noise/template/([0-9]+)/wav - 获取模板原始 WAV 二进制。
  svr.Get("/api/noise/template/([0-9]+)/wav", [&template_db](const Request& req,
                                                             Response& res) {
    uint32_t id;
    if (!parse_template_id(req, res, id))
      return;
    const Template* t = template_db.get_template(id);
    if (t == nullptr) {
      res.status = 404;
      res.set_content("template not found", "text/plain");
      return;
    }
    if (t->wav_file.empty()) {
      res.status = 404;
      res.set_content("template has no WAV file", "text/plain");
      return;
    }
    std::string wav_path = template_db.get_dir_for_test() + "/" + t->wav_file;
    std::ifstream in(wav_path, std::ios::binary);
    if (!in.is_open()) {
      res.status = 404;
      res.set_content("WAV file missing on disk", "text/plain");
      return;
    }
    std::string content((std::istreambuf_iterator<char>(in)), {});
    res.set_content(content, "audio/wav");
  });

  // GET /api/noise/template/([0-9]+)/test - 占位（POST 才是匹配测试）。
  // cpp-httplib 区分 GET/POST 同路径，此处不注册 GET /test。

  // POST /api/noise/template/([0-9]+)/test - 上传 WAV 测试匹配。
  // 返回 { "matched_template_id": N, "similarity": 0.xx }
  svr.Post("/api/noise/template/([0-9]+)/test",
           [&template_db](const Request& req, Response& res) {
             uint32_t id;
             if (!parse_template_id(req, res, id))
               return;
             if (template_db.get_template(id) == nullptr) {
               res.status = 404;
               res.set_content("template not found", "text/plain");
               return;
             }
             if (!req.has_file("wav")) {
               res.status = 400;
               res.set_content("missing wav file field", "text/plain");
               return;
             }
             auto wav = req.get_file_value("wav");
             // 复用 parse_wav_pcm16_48k_mono + compute_bark_spectrum（DRY：
             // 与 add_template_from_wav 共用同一 WAV 解析 + Bark 提取路径）。
             std::vector<float> samples;
             uint32_t sample_rate = 0;
             if (!parse_wav_pcm16_48k_mono(wav.content, samples, sample_rate)) {
               res.status = 400;
               res.set_content("WAV must be 48kHz PCM-16 mono (Phase 1 limit)",
                               "text/plain");
               return;
             }
             auto bark = compute_bark_spectrum(samples.data(), samples.size(),
                                               sample_rate);
             std::pair<uint32_t, float> m = template_db.match(bark);
             // 显式格式化 similarity 防止 NaN/默认精度问题（review Minor #2）
             char sim_buf[32];
             std::snprintf(sim_buf, sizeof(sim_buf), "%.6f", m.second);
             std::stringstream ss;
             ss << "{\n  \"matched_template_id\": " << m.first
                << ",\n  \"similarity\": " << sim_buf
                << ",\n  \"requested_template_id\": " << id << "\n}\n";
             res.set_content(ss.str(), "application/json");
           });

  // GET /api/noise/template/([0-9]+) - 模板详情（含 bark_spectrum）。
  svr.Get("/api/noise/template/([0-9]+)",
          [&template_db](const Request& req, Response& res) {
            uint32_t id;
            if (!parse_template_id(req, res, id))
              return;
            const Template* t = template_db.get_template(id);
            if (t == nullptr) {
              res.status = 404;
              res.set_content("template not found", "text/plain");
              return;
            }
            std::stringstream ss;
            ss << "{\n  " << template_to_json_object(*t, "  ") << "\n}\n";
            res.set_content(ss.str(), "application/json");
          });

  // DELETE /api/noise/template/([0-9]+) - 删除模板 + WAV 文件。
  svr.Delete("/api/noise/template/([0-9]+)", [&template_db](const Request& req,
                                                            Response& res) {
    uint32_t id;
    if (!parse_template_id(req, res, id))
      return;
    const Template* t = template_db.get_template(id);
    if (t == nullptr) {
      res.status = 404;
      res.set_content("template not found", "text/plain");
      return;
    }
    // 先删 WAV 文件（若存在）
    if (!t->wav_file.empty()) {
      std::string wav_path = template_db.get_dir_for_test() + "/" + t->wav_file;
      std::error_code ec;
      std::filesystem::remove(wav_path, ec);
      // 忽略删除失败（文件可能已不存在），仍删内存记录
    }
    if (!template_db.remove_template(id)) {
      res.status = 404;
      res.set_content("template not found", "text/plain");
      return;
    }
    // 持久化更新
    template_db.save(template_db.get_dir_for_test());
    res.set_content("", "text/plain");
  });

  // PUT /api/noise/template/([0-9]+) - 更新 label/description。
  // Body: { "label": "...", "description": "..." }
  svr.Put("/api/noise/template/([0-9]+)", [&template_db](const Request& req,
                                                         Response& res) {
    uint32_t id;
    if (!parse_template_id(req, res, id))
      return;
    try {
      boost::property_tree::ptree pt;
      std::stringstream ss(req.body);
      boost::property_tree::read_json(ss, pt);
      std::string label = pt.get<std::string>("label", "");
      std::string desc = pt.get<std::string>("description", "");
      if (!template_db.update_template(id, label, desc)) {
        res.status = 404;
        res.set_content("template not found", "text/plain");
        return;
      }
      template_db.save(template_db.get_dir_for_test());
      res.set_content("", "text/plain");
    } catch (const std::exception& e) {
      res.status = 400;
      res.set_content(std::string("invalid JSON: ") + e.what(), "text/plain");
    }
  });

  // POST /api/noise/template - 录入新模板（multipart: wav + label +
  // description）。 arch §7.7: HTTP 接收 WAV -> 提取 32 维 Bark -> 存
  // templates.json + WAV 文件。
  svr.Post(
      "/api/noise/template", [&template_db](const Request& req, Response& res) {
        if (!req.has_file("wav")) {
          res.status = 400;
          res.set_content("missing wav file field", "text/plain");
          return;
        }
        auto wav = req.get_file_value("wav");
        std::string label;
        std::string description;
        if (req.has_file("label"))
          label = req.get_file_value("label").content;
        if (req.has_file("description"))
          description = req.get_file_value("description").content;
        if (label.empty()) {
          res.status = 400;
          res.set_content("missing label field", "text/plain");
          return;
        }
        uint32_t id =
            template_db.add_template_from_wav(label, description, wav.content);
        if (id == 0) {
          res.status = 400;
          res.set_content("WAV ingestion failed (must be 48kHz PCM-16 mono)",
                          "text/plain");
          return;
        }
        std::stringstream ss;
        ss << "{\n  \"id\": " << id << ",\n  \"label\": \""
           << escape_json(label) << "\"" << ",\n  \"status\": \"created\"\n}\n";
        res.set_content(ss.str(), "application/json");
      });
}

// register_noise_routes：聚合 sensor + template + SSE 路由（arch §5.1 +
// §5.3 + Spec4 §5.1 SSE）。T6 main.cpp 装配时调用一次，把全部 /api/noise/*
// 路由注册到同一 httplib::Server。 顺序无关（路由按 method+path
// 匹配，无重叠）。
void register_noise_routes(httplib::Server& svr,
                           NoiseManager& mgr,
                           NoiseTemplateDB& template_db) {
  register_noise_sensor_routes(svr, mgr);
  register_noise_template_routes(svr, mgr, template_db);
  // Spec4 Task 3：SSE 路由（4 端点）。
  register_noise_sse_routes(svr, mgr);
}

// ── Spec4 Task 3：SSE 路由（arch §5.1 + D-S4.5）──────────────────────
// 4 SSE 端点使用 cpp-httplib set_chunked_content_provider。
//
// SSE handler 线程模型（D-S4.1）：
// - handler 线程（cpp-httplib 工作线程）调 subscribe() 取队列 shared_ptr。
// - chunked provider loop：try_drain 队列 -> sink.write SSE 帧。
// - 无事件时 sleep 20ms 避免 busy-loop（不调 sink.write -> data_available
//   保持 true，loop 继续；sleep 让出 CPU）。
// - 客户端断连 -> sink.is_writable()=false -> sink.done() -> provider 返回
//   true（正常退出 loop）-> releaser 调 unsubscribe。
// - releaser 在 Response 析构时调用（cpp-httplib 工作线程清理时）。
//
// RT 非阻塞保证（风险 9）：
// - capture 线程 on_period_end push 到 broadcaster（try_lock + drop-oldest）。
// - SSE handler 线程 drain + sink.write（socket I/O 仅在 handler 线程）。
// - 慢消费者（handler sleep 长）-> 队列满 -> drop oldest（dropped_count
// 递增），
//   capture 线程不阻塞。
namespace {

// SSE handler drain loop：通用 drain + write 逻辑，供 4 路由复用。
// broadcaster：事件源（shared_ptr，延长生命周期至 handler 退出）。
// res：设置 chunked provider 的 Response。
// capacity：订阅队列容量（metrics/alerts 用 64，PCM 用 16 - 每帧较大）。
void setup_sse_route(httplib::Response& res,
                     std::shared_ptr<SseBroadcaster> broadcaster,
                     size_t capacity = 64) {
  if (!broadcaster) {
    res.status = 404;
    res.set_content("sensor not found or denoise disabled", "text/plain");
    return;
  }
  auto handle = broadcaster->subscribe(capacity);
  // 捕获 queue（shared_ptr 可拷贝）+ id，不捕获 handle（move-only）。
  auto queue = handle.queue;
  uint64_t sub_id = handle.id;
  // broadcaster shared_ptr 捕获到 releaser：sensor 被 remove 后 broadcaster
  // 仍存活（shared_ptr 延长生命周期），releaser 安全调 unsubscribe。
  res.set_chunked_content_provider(
      "text/event-stream",
      [queue](size_t offset, httplib::DataSink& sink) -> bool {
        // 首次调用时发送 SSE 注释行（": keepalive\n\n"）建立连接。
        // SSE 注释以 ':' 开头，客户端忽略。这确保 chunked 响应立即发送
        // 至少一个 chunk，客户端确认连接建立。
        if (offset == 0) {
          const char* hello = ": connected\n\n";
          if (!sink.write(hello, 12))
            return false;
        }
        std::vector<std::string> events;
        if (queue->try_drain(events)) {
          for (const auto& e : events) {
            if (!sink.write(e.data(), e.size()))
              return false;  // write 失败
          }
        }
        if (!sink.is_writable()) {
          sink.done();
          return true;
        }
        // 无事件时短暂 sleep 避免 busy-loop
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return true;
      },
      [broadcaster, sub_id](bool /*success*/) {
        broadcaster->unsubscribe(sub_id);
      });
}

}  // namespace

void register_noise_sse_routes(httplib::Server& svr, NoiseManager& mgr) {
  using httplib::Request;
  using httplib::Response;

  // GET /api/noise/sensor/([0-9]+)/metrics/sse - 指标快照 SSE（~1s/event）。
  // 返回 text/event-stream，每 period 推送一个 metrics 快照事件。
  svr.Get("/api/noise/sensor/([0-9]+)/metrics/sse",
          [&mgr](const Request& req, Response& res) {
            uint8_t id;
            if (!parse_sensor_id(req, res, id))
              return;
            auto bc = mgr.get_metrics_broadcaster(id);
            setup_sse_route(res, bc, /*capacity=*/64);
          });

  // GET /api/noise/sensor/([0-9]+)/denoised - PCM base64 SSE（denoised）。
  // 每 period 推送一个 PCM chunk（base64 编码的 S16 LE）。
  // denoise 关 -> broadcaster 不存在 -> 404（与 /api/streamer/.../denoised
  // 语义一致）。
  svr.Get("/api/noise/sensor/([0-9]+)/denoised",
          [&mgr](const Request& req, Response& res) {
            uint8_t id;
            if (!parse_sensor_id(req, res, id))
              return;
            auto bc = mgr.get_pcm_broadcaster(id, /*denoised=*/true);
            // PCM 帧较大，队列容量小一些（16 = ~64ms 缓冲 @4ms/period）
            setup_sse_route(res, bc, /*capacity=*/16);
          });

  // GET /api/noise/sensor/([0-9]+)/noise - PCM base64 SSE（noise）。
  svr.Get("/api/noise/sensor/([0-9]+)/noise",
          [&mgr](const Request& req, Response& res) {
            uint8_t id;
            if (!parse_sensor_id(req, res, id))
              return;
            auto bc = mgr.get_pcm_broadcaster(id, /*denoised=*/false);
            setup_sse_route(res, bc, /*capacity=*/16);
          });

  // GET /api/noise/alerts/sse - 告警事件 SSE（T4 push）。
  // 全局 broadcaster（非 per-sensor），T4 告警引擎 push 事件。
  svr.Get("/api/noise/alerts/sse",
          [&mgr](const Request& /*req*/, Response& res) {
            auto bc = mgr.get_alert_broadcaster();
            setup_sse_route(res, bc, /*capacity=*/64);
          });

  // Spec4 T4：GET /api/noise/alerts - 查询告警历史 ring（所有 sensor）。
  // 返回 JSON 数组（arch §C 告警事件 JSON 格式）：
  //   { "alerts": [ {sensor_id, level, rule, message, raised_at_ms,
  //   is_active} ] }
  svr.Get("/api/noise/alerts", [&mgr](const Request& /*req*/, Response& res) {
    auto entries = mgr.get_alert_history();
    std::stringstream ss;
    ss << "{\n  \"alerts\": [";
    bool first = true;
    for (const auto& e : entries) {
      if (!first)
        ss << ",";
      first = false;
      const char* level_str = "none";
      switch (e.event.level) {
        case AlertLevel::Info:
          level_str = "info";
          break;
        case AlertLevel::Warning:
          level_str = "warning";
          break;
        case AlertLevel::Critical:
          level_str = "critical";
          break;
        case AlertLevel::None:
        default:
          level_str = "none";
          break;
      }
      ss << "\n    {"
         << "\n      \"sensor_id\": " << static_cast<unsigned>(e.sensor_id)
         << ",\n      \"level\": \"" << level_str << "\""
         << ",\n      \"rule\": \"" << escape_json(e.event.rule) << "\""
         << ",\n      \"message\": \"" << escape_json(e.event.message) << "\""
         << ",\n      \"raised_at_ms\": " << e.event.raised_at_ms
         << ",\n      \"is_active\": " << (e.event.is_active ? "true" : "false")
         << "\n    }";
    }
    ss << "\n  ]\n}\n";
    res.set_content(ss.str(), "application/json");
  });
}

}  // namespace noise

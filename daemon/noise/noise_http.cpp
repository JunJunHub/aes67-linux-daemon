// daemon/noise/noise_http.cpp
// Spec3 Task 3：HTTP sensor API
// 实现。架构依据：docs/noise/architecture-design.md §5.1（端点）+
// §5.4（响应字段）。
#include "noise_http.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <cstdint>
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

// ── ptree 构造 helper ───────────────────────────────────────────────────
// boost::property_tree 风格（与 daemon/json.cpp config_to_json 同一做法）。
// 不直接手拼字符串，避免 escape/格式问题。write_json(ss, pt, false)
// pretty=false 输出紧凑 JSON（默认 true 会带换行+缩进）。
namespace {
namespace pt = boost::property_tree;

// 单个 NoiseMetricsSnapshot -> ptree（不含外层 wrapper）。
// 字段名严格按 arch §5.4 响应示例：noise_level_dbfs / noise_type /
// noise_type_confidence / is_mixed / estimated_snr_db / denoise_enabled /
// denoise_dry_wet / noise_reduction_db / alert_threshold_dbfs / is_alerting
// / spectral_centroid_hz / spectral_flatness / hum_strength_db。
pt::ptree snapshot_to_ptree(const NoiseMetricsSnapshot& s) {
  pt::ptree node;
  node.put("noise_level_dbfs", s.noise_level_dbfs);
  node.put("noise_type", noise_type_to_string(s.noise_type));
  node.put("noise_type_confidence", s.noise_type_confidence);
  node.put("is_mixed", s.is_mixed);
  node.put("estimated_snr_db", s.estimated_snr_db);
  node.put("denoise_enabled", s.denoise_enabled);
  node.put("denoise_dry_wet", s.denoise_dry_wet);
  node.put("noise_reduction_db", s.noise_reduction_db);
  node.put("alert_threshold_dbfs", s.alert_threshold_dbfs);
  node.put("is_alerting", s.is_alerting);
  node.put("spectral_centroid_hz", s.spectral_centroid_hz);
  node.put("spectral_flatness", s.spectral_flatness);
  node.put("hum_strength_db", s.hum_strength_db);
  // noise_candidates 数组（arch §5.4 混合噪声示例）。
  // NoiseMetricsSnapshot.noise_candidates 是定长 array + count（避免 RT 路径
  // 堆分配）。序列化时按 count 截断。
  pt::ptree cand_arr;
  for (size_t i = 0; i < s.noise_candidates_count; ++i) {
    pt::ptree elem;
    elem.put("type", noise_type_to_string(s.noise_candidates[i].type));
    elem.put("confidence", s.noise_candidates[i].confidence);
    cand_arr.push_back(std::make_pair("", elem));
  }
  node.put_child("noise_candidates", cand_arr);
  return node;
}

// SensorInfo -> ptree：sensor 元信息（id/sink_id/enabled/denoise_enabled）
// + metrics 快照字段（合并到同一层级，照搬 arch §5.4 GET /sensor/:id 响应）。
pt::ptree sensor_info_to_ptree(const SensorInfo& info) {
  pt::ptree node;
  node.put("id", static_cast<unsigned>(info.id));
  node.put("sink_id", static_cast<unsigned>(info.sink_id));
  node.put("enabled", info.enabled);
  node.put("denoise_enabled", info.denoise_enabled);
  // 合并 metrics 字段到同一层级。
  auto metrics_pt = snapshot_to_ptree(info.metrics);
  for (auto& [k, v] : metrics_pt) {
    node.put_child(k, v);
  }
  return node;
}

// ptree -> 紧凑 JSON 字符串。
std::string ptree_to_json(const pt::ptree& root) {
  std::stringstream ss;
  pt::write_json(ss, root, false);
  return ss.str();
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

std::string metrics_to_json(const NoiseMetricsSnapshot& snapshot) {
  return ptree_to_json(snapshot_to_ptree(snapshot));
}

std::string sensor_to_json(const SensorInfo& info) {
  return ptree_to_json(sensor_info_to_ptree(info));
}

std::string sensors_to_json(
    const std::vector<std::pair<uint8_t, SensorInfo>>& sensors) {
  pt::ptree root;
  pt::ptree arr;
  for (const auto& [id, info] : sensors) {
    arr.push_back(std::make_pair("", sensor_info_to_ptree(info)));
  }
  root.put_child("sensors", arr);
  return ptree_to_json(root);
}

std::string history_to_json(const std::vector<NoiseMetricsSnapshot>& history) {
  pt::ptree root;
  pt::ptree arr;
  for (const auto& snap : history) {
    arr.push_back(std::make_pair("", snapshot_to_ptree(snap)));
  }
  root.put_child("history", arr);
  return ptree_to_json(root);
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
  svr.Get("/api/noise/sensor/([0-9]+)/metrics",
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
            res.set_content(metrics_to_json(info.metrics), "application/json");
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
  svr.Put("/api/noise/sensor/([0-9]+)", [&mgr](const Request& req,
                                               Response& res) {
    uint8_t id;
    if (!parse_sensor_id(req, res, id))
      return;
    try {
      pt::ptree pt;
      std::stringstream ss(req.body);
      pt::read_json(ss, pt);
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
      if (!mgr.add_sensor(id, sink_id, cfg)) {
        res.status = 500;
        res.set_content("failed to add sensor", "text/plain");
        return;
      }
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

}  // namespace noise

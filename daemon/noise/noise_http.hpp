// daemon/noise/noise_http.hpp
// Spec3 Task 3：HTTP REST API for noise sensors。
// 架构依据：docs/noise/architecture-design.md §5.1（端点列表）+ §5.4
// （JSON 响应字段 + NoiseType 小写蛇形映射）。
//
// 路由模式照搬 daemon/http_server.cpp：
//   - svr.Get/Post/Put/Delete("/api/...", [capture](req,res){...})。
//   - regex ([0-9]+) + req.matches[1] -> std::stoi -> uint8_t sensor_id。
//   - 错误：HTTP 404/400 + text/plain（sensor 不存在/JSON 解析失败）。
//   - 成功：HTTP 200 + application/json（GET）或 200 + 空 body（PUT/DELETE）。
//
// 线程模型：register_noise_sensor_routes 捕获 NoiseManager& 引用。NoiseManager
// 内部用 RcuPtr<const SensorTable> 保护 sensor 表读路径；metrics 读路径由
// NoiseMetrics::get_snapshot() / get_history() 持 metrics_mutex_ 与 RT 写
// (collect) 互斥（见 noise_metrics.hpp 注释）。
//
// 集成：本文件仅在 WITH_NOISE=ON 时编译（noise lib 内）。由 T6 的
// #ifdef _USE_NOISE_ 块调用 register_noise_sensor_routes 注册到 daemon
// http_server。--no-noise (WITH_NOISE=OFF) 构建不包含本文件，零回归。
#ifndef NOISE_NOISE_HTTP_HPP_
#define NOISE_NOISE_HTTP_HPP_

#include <httplib.h>

#include <string>
#include <utility>
#include <vector>

#include "noise_analyzer.hpp"     // NoiseType
#include "noise_manager.hpp"      // NoiseManager, SensorInfo
#include "noise_metrics.hpp"      // NoiseMetricsSnapshot
#include "noise_template_db.hpp"  // NoiseTemplateDB (Spec3 Task 5)

namespace noise {

// NoiseType 枚举序列化：C++ CamelCase -> JSON 小写蛇形（arch §5.4）。
//   Clean -> "clean", Hum50Hz -> "hum_50hz", Hum60Hz -> "hum_60hz", 等。
std::string noise_type_to_string(NoiseType type);

// JSON 序列化（boost::property_tree，arch §5.4 字段名）。
// sensor_to_json：单个 sensor 信息 + 最新 metrics 快照（GET /sensor/:id）。
// sensors_to_json：sensor 列表（GET /sensors）。
// metrics_to_json：纯 metrics 快照（GET /sensor/:id/metrics）。
// history_to_json：metrics 历史数组（GET /sensor/:id/history）。
std::string sensor_to_json(const SensorInfo& info);
std::string sensors_to_json(
    const std::vector<std::pair<uint8_t, SensorInfo>>& sensors);
std::string metrics_to_json(const NoiseMetricsSnapshot& snapshot);
std::string history_to_json(const std::vector<NoiseMetricsSnapshot>& history);

// 注册噪声传感器 HTTP 路由到 svr。
// 端点（arch §5.1，未含 SSE 流：denoised/noise 留给 Phase 2）：
//   GET    /api/noise/sensors
//   GET    /api/noise/sensor/([0-9]+)
//   PUT    /api/noise/sensor/([0-9]+)
//   DELETE /api/noise/sensor/([0-9]+)
//   GET    /api/noise/sensor/([0-9]+)/metrics
//   GET    /api/noise/sensor/([0-9]+)/history
//
// PUT body 字段（JSON）：sink_id, denoise_enabled, denoise_plugin
// （映射到 NoiseSensorConfig::plugin_name）, denoise_dry_wet, sensitivity,
// enabled（可选，PUT 后调 enable_sensor 应用）。
void register_noise_sensor_routes(httplib::Server& svr, NoiseManager& mgr);

// Spec3 Task 5：注册噪声模板 HTTP 路由到 svr（arch §5.3，9 端点）。
// template_db 由调用方（T6 wiring）独立持有并传入；NoiseManager 不拥有它
// （review Important #1：DB 作为独立参数）。
//
// 端点（arch §5.3）：
//   GET    /api/noise/templates              - 列出所有模板
//   POST   /api/noise/template               - 录入新模板（multipart:
//   wav+label） GET    /api/noise/template/([0-9]+)      - 模板详情（含
//   bark_spectrum） DELETE /api/noise/template/([0-9]+)      - 删除模板（+ WAV
//   文件） PUT    /api/noise/template/([0-9]+)      - 更新 label/description
//   POST   /api/noise/template/([0-9]+)/test - 上传 WAV 测试匹配
//   GET    /api/noise/template/([0-9]+)/wav  - 获取模板原始 WAV
//   GET    /api/noise/templates/export       - 导出模板库 (JSON)
//   POST   /api/noise/templates/import       - 导入模板库 (JSON)
//
// Phase 1 限定（arch §11 风险1）：WAV 须 48kHz PCM-16 mono，否则 400。
// JSON 输出 = 手工拼接（reuse escape_json），输入用 boost::property_tree。
void register_noise_template_routes(httplib::Server& svr,
                                    NoiseManager& mgr,
                                    NoiseTemplateDB& template_db);

// Spec3 Task 6：聚合注册 sensor + template 路由（main.cpp 单点调用）。
// 等价于依次调用 register_noise_sensor_routes +
// register_noise_template_routes。 模板 DB 由调用方持有并传入（review Important
// #1：DB 独立参数）。
void register_noise_routes(httplib::Server& svr,
                           NoiseManager& mgr,
                           NoiseTemplateDB& template_db);

}  // namespace noise

#endif  // NOISE_NOISE_HTTP_HPP_

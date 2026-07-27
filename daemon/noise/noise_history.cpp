// daemon/noise/noise_history.cpp
// Spec6 T1（D-S6.1）：NoiseStore SQLite 仓储实现。
// 架构依据：docs/superpowers/specs/noise-spec6-design.md §3.5。
#include "noise_history.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#include <sqlite3.h>

namespace noise {

namespace {

// 墙钟毫秒（Unix epoch）。housekeeper push 时 + retention cleanup 用。
uint64_t now_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

// noise_candidates 序列化为紧凑字符串 "type:conf|type:conf|..."（最多 3 项）。
// 不用 JSON 避免解析依赖；格式可逆且无歧义（type 为 int，conf 为 float）。
std::string candidates_to_str(const NoiseMetricsSnapshot& s) {
  std::ostringstream ss;
  for (size_t i = 0; i < s.noise_candidates_count && i < kMaxNoiseCandidates;
       ++i) {
    if (i > 0)
      ss << "|";
    ss << static_cast<int>(s.noise_candidates[i].type) << ":"
       << s.noise_candidates[i].confidence;
  }
  return ss.str();
}

// 反序列化 candidates。格式错误时返回 count=0（容错）。
void str_to_candidates(const std::string& str, NoiseMetricsSnapshot& s) {
  s.noise_candidates_count = 0;
  if (str.empty())
    return;
  size_t pos = 0;
  while (pos < str.size() && s.noise_candidates_count < kMaxNoiseCandidates) {
    size_t bar = str.find('|', pos);
    std::string token = (bar == std::string::npos) ? str.substr(pos)
                                                   : str.substr(pos, bar - pos);
    size_t colon = token.find(':');
    if (colon != std::string::npos) {
      try {
        int t = std::stoi(token.substr(0, colon));
        float c = std::stof(token.substr(colon + 1));
        s.noise_candidates[s.noise_candidates_count].type =
            static_cast<NoiseType>(t);
        s.noise_candidates[s.noise_candidates_count].confidence = c;
        ++s.noise_candidates_count;
      } catch (...) {
        // 容错：跳过坏 token
      }
    }
    if (bar == std::string::npos)
      break;
    pos = bar + 1;
  }
}

// helper：执行无结果 SQL（DDL/PRAGMA/INSERT/DELETE）。失败记 stderr。
bool exec_simple(sqlite3* db, const char* sql) {
  char* err = nullptr;
  int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    std::cerr << "NoiseStore: sqlite_exec failed: " << (err ? err : "(null)")
              << " (sql: " << sql << ")\n";
    sqlite3_free(err);
    return false;
  }
  return true;
}

// metrics_history 建表 SQL。列 = NoiseMetricsSnapshot 全字段
// （noise_type / alert_level 存 int，noise_candidates 存紧凑字符串 blob）。
// 设计依据 spec6-design §3.5 metrics_history 表字段清单。
constexpr const char* kCreateMetricsSql =
    "CREATE TABLE IF NOT EXISTS metrics_history ("
    "sensor_id INTEGER NOT NULL,"
    "timestamp_ms INTEGER NOT NULL,"
    "is_noisy INTEGER,"
    "noise_confidence REAL,"
    "estimated_snr_db REAL,"
    "noise_type INTEGER,"
    "noise_type_confidence REAL,"
    "is_mixed INTEGER,"
    "noise_type_source TEXT,"
    "noise_candidates TEXT,"
    "noise_candidates_count INTEGER,"
    "noise_level_dbfs REAL,"
    "spectral_centroid_hz REAL,"
    "spectral_flatness REAL,"
    "hum_strength_db REAL,"
    "denoise_enabled INTEGER,"
    "denoise_dry_wet REAL,"
    "input_level_dbfs REAL,"
    "output_level_dbfs REAL,"
    "noise_reduction_db REAL,"
    "ref_similarity REAL,"
    "ref_noise_db REAL,"
    "ref_delay_ms REAL,"
    "alert_threshold_dbfs REAL,"
    "hum_alert_threshold_db REAL,"
    "snr_alert_threshold_db REAL,"
    "ref_similarity_threshold REAL,"
    "alert_debounce_periods INTEGER,"
    "is_alerting INTEGER,"
    "alert_level INTEGER,"
    "plugin_degraded INTEGER,"
    "l3_match_type TEXT,"
    "l3_similarity REAL,"
    "PRIMARY KEY (sensor_id, timestamp_ms))";

constexpr const char* kCreateAlertsSql =
    "CREATE TABLE IF NOT EXISTS alerts_history ("
    "sensor_id INTEGER NOT NULL,"
    "timestamp_ms INTEGER NOT NULL,"
    "level INTEGER,"
    "rule TEXT,"
    "message TEXT,"
    "is_active INTEGER)";

}  // namespace

NoiseStore::NoiseStore(const std::string& db_path,
                       uint32_t retention_hours,
                       uint32_t flush_interval_s)
    : retention_hours_(retention_hours), flush_interval_s_(flush_interval_s) {
  if (db_path.empty())
    return;
  int rc = sqlite3_open(db_path.c_str(), &db_);
  if (rc != SQLITE_OK) {
    std::cerr << "NoiseStore: sqlite3_open failed: "
              << (db_ ? sqlite3_errmsg(db_) : "(null)") << " (path: " << db_path
              << ")\n";
    sqlite3_close(db_);
    db_ = nullptr;
    return;
  }
  init_schema();
}

NoiseStore::~NoiseStore() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

void NoiseStore::init_schema() {
  if (!db_)
    return;
  // WAL 模式（D-S6.1）：并发读 + 单写不互斥。
  exec_simple(db_, "PRAGMA journal_mode=WAL;");
  // 同步 NORMAL：WAL 下足够的持久性 + 更高吞吐（housekeeper 批量写）。
  exec_simple(db_, "PRAGMA synchronous=NORMAL;");
  exec_simple(db_, kCreateMetricsSql);
  exec_simple(db_, kCreateAlertsSql);
  // 复合索引：(sensor_id, timestamp_ms) 支持按 sensor 时间范围查询。
  exec_simple(db_,
              "CREATE INDEX IF NOT EXISTS idx_metrics_sensor_time "
              "ON metrics_history(sensor_id, timestamp_ms);");
  // 全局时间索引：alerts_history 跨 sensor 时间范围查询。
  exec_simple(db_,
              "CREATE INDEX IF NOT EXISTS idx_alerts_time "
              "ON alerts_history(timestamp_ms);");
}

// ── 批量插入 ───────────────────────────────────────────────────────────
// 用 BEGIN/COMMIT 事务 + prepared statement 复用，N 行一次提交。
void NoiseStore::insert_metrics(
    const std::vector<MetricsHistoryRecord>& records) {
  if (!db_ || records.empty())
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT OR REPLACE INTO metrics_history "
      "(sensor_id,timestamp_ms,is_noisy,noise_confidence,estimated_snr_db,"
      "noise_type,noise_type_confidence,is_mixed,noise_type_source,"
      "noise_candidates,noise_candidates_count,noise_level_dbfs,"
      "spectral_centroid_hz,spectral_flatness,hum_strength_db,denoise_enabled,"
      "denoise_dry_wet,input_level_dbfs,output_level_dbfs,noise_reduction_db,"
      "ref_similarity,ref_noise_db,ref_delay_ms,alert_threshold_dbfs,"
      "hum_alert_threshold_db,snr_alert_threshold_db,"
      "ref_similarity_threshold,alert_debounce_periods,is_alerting,"
      "alert_level,plugin_degraded,l3_match_type,l3_similarity) "
      "VALUES "
      "(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    std::cerr << "NoiseStore: prepare metrics insert failed: "
              << sqlite3_errmsg(db_) << "\n";
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return;
  }
  for (const auto& r : records) {
    const NoiseMetricsSnapshot& s = r.snapshot;
    sqlite3_bind_int(stmt, 1, r.sensor_id);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(r.timestamp_ms));
    sqlite3_bind_int(stmt, 3, s.is_noisy ? 1 : 0);
    sqlite3_bind_double(stmt, 4, s.noise_confidence);
    sqlite3_bind_double(stmt, 5, s.estimated_snr_db);
    sqlite3_bind_int(stmt, 6, static_cast<int>(s.noise_type));
    sqlite3_bind_double(stmt, 7, s.noise_type_confidence);
    sqlite3_bind_int(stmt, 8, s.is_mixed ? 1 : 0);
    sqlite3_bind_text(stmt, 9, s.noise_type_source.c_str(), -1,
                      SQLITE_TRANSIENT);
    std::string cand = candidates_to_str(s);
    sqlite3_bind_text(stmt, 10, cand.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 11, static_cast<int>(s.noise_candidates_count));
    sqlite3_bind_double(stmt, 12, s.noise_level_dbfs);
    sqlite3_bind_double(stmt, 13, s.spectral_centroid_hz);
    sqlite3_bind_double(stmt, 14, s.spectral_flatness);
    sqlite3_bind_double(stmt, 15, s.hum_strength_db);
    sqlite3_bind_int(stmt, 16, s.denoise_enabled ? 1 : 0);
    sqlite3_bind_double(stmt, 17, s.denoise_dry_wet);
    sqlite3_bind_double(stmt, 18, s.input_level_dbfs);
    sqlite3_bind_double(stmt, 19, s.output_level_dbfs);
    sqlite3_bind_double(stmt, 20, s.noise_reduction_db);
    sqlite3_bind_double(stmt, 21, s.ref_similarity);
    sqlite3_bind_double(stmt, 22, s.ref_noise_db);
    sqlite3_bind_double(stmt, 23, s.ref_delay_ms);
    sqlite3_bind_double(stmt, 24, s.alert_threshold_dbfs);
    sqlite3_bind_double(stmt, 25, s.hum_alert_threshold_db);
    sqlite3_bind_double(stmt, 26, s.snr_alert_threshold_db);
    sqlite3_bind_double(stmt, 27, s.ref_similarity_threshold);
    sqlite3_bind_int(stmt, 28, static_cast<int>(s.alert_debounce_periods));
    sqlite3_bind_int(stmt, 29, s.is_alerting ? 1 : 0);
    sqlite3_bind_int(stmt, 30, static_cast<int>(s.alert_level));
    sqlite3_bind_int(stmt, 31, s.plugin_degraded ? 1 : 0);
    sqlite3_bind_text(stmt, 32, s.l3_match_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 33, s.l3_similarity);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      std::cerr << "NoiseStore: metrics insert step failed: "
                << sqlite3_errmsg(db_) << "\n";
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
  }
  sqlite3_finalize(stmt);
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
}

void NoiseStore::insert_alerts(const std::vector<AlertHistoryRecord>& records) {
  if (!db_ || records.empty())
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO alerts_history "
      "(sensor_id,timestamp_ms,level,rule,message,is_active) "
      "VALUES (?,?,?,?,?,?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    std::cerr << "NoiseStore: prepare alerts insert failed: "
              << sqlite3_errmsg(db_) << "\n";
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return;
  }
  for (const auto& r : records) {
    sqlite3_bind_int(stmt, 1, r.sensor_id);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(r.timestamp_ms));
    sqlite3_bind_int(stmt, 3, static_cast<int>(r.event.level));
    sqlite3_bind_text(stmt, 4, r.event.rule.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, r.event.message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, r.event.is_active ? 1 : 0);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      std::cerr << "NoiseStore: alerts insert step failed: "
                << sqlite3_errmsg(db_) << "\n";
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
  }
  sqlite3_finalize(stmt);
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
}

// ── 时间范围查询 ───────────────────────────────────────────────────────
std::vector<MetricsHistoryRecord> NoiseStore::query_metrics(
    uint8_t sensor_id,
    uint64_t from_ms,
    uint64_t to_ms) const {
  std::vector<MetricsHistoryRecord> out;
  if (!db_ || from_ms > to_ms)
    return out;
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT timestamp_ms,is_noisy,noise_confidence,estimated_snr_db,"
      "noise_type,noise_type_confidence,is_mixed,noise_type_source,"
      "noise_candidates,noise_candidates_count,noise_level_dbfs,"
      "spectral_centroid_hz,spectral_flatness,hum_strength_db,denoise_enabled,"
      "denoise_dry_wet,input_level_dbfs,output_level_dbfs,noise_reduction_db,"
      "ref_similarity,ref_noise_db,ref_delay_ms,alert_threshold_dbfs,"
      "hum_alert_threshold_db,snr_alert_threshold_db,"
      "ref_similarity_threshold,alert_debounce_periods,is_alerting,"
      "alert_level,plugin_degraded,l3_match_type,l3_similarity "
      "FROM metrics_history WHERE sensor_id=? AND timestamp_ms>=? AND "
      "timestamp_ms<=? ORDER BY timestamp_ms ASC";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    std::cerr << "NoiseStore: prepare metrics query failed: "
              << sqlite3_errmsg(db_) << "\n";
    return out;
  }
  sqlite3_bind_int(stmt, 1, sensor_id);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(from_ms));
  sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(to_ms));
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    MetricsHistoryRecord r;
    r.sensor_id = sensor_id;
    r.timestamp_ms = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    NoiseMetricsSnapshot& s = r.snapshot;
    s.is_noisy = sqlite3_column_int(stmt, 1) != 0;
    s.noise_confidence = static_cast<float>(sqlite3_column_double(stmt, 2));
    s.estimated_snr_db = static_cast<float>(sqlite3_column_double(stmt, 3));
    s.noise_type = static_cast<NoiseType>(sqlite3_column_int(stmt, 4));
    s.noise_type_confidence =
        static_cast<float>(sqlite3_column_double(stmt, 5));
    s.is_mixed = sqlite3_column_int(stmt, 6) != 0;
    if (const unsigned char* t = sqlite3_column_text(stmt, 7))
      s.noise_type_source = reinterpret_cast<const char*>(t);
    if (const unsigned char* t = sqlite3_column_text(stmt, 8))
      str_to_candidates(reinterpret_cast<const char*>(t), s);
    s.noise_candidates_count = static_cast<size_t>(sqlite3_column_int(stmt, 9));
    // guard count vs parsed
    if (s.noise_candidates_count > kMaxNoiseCandidates)
      s.noise_candidates_count = kMaxNoiseCandidates;
    s.noise_level_dbfs = static_cast<float>(sqlite3_column_double(stmt, 10));
    s.spectral_centroid_hz =
        static_cast<float>(sqlite3_column_double(stmt, 11));
    s.spectral_flatness = static_cast<float>(sqlite3_column_double(stmt, 12));
    s.hum_strength_db = static_cast<float>(sqlite3_column_double(stmt, 13));
    s.denoise_enabled = sqlite3_column_int(stmt, 14) != 0;
    s.denoise_dry_wet = static_cast<float>(sqlite3_column_double(stmt, 15));
    s.input_level_dbfs = static_cast<float>(sqlite3_column_double(stmt, 16));
    s.output_level_dbfs = static_cast<float>(sqlite3_column_double(stmt, 17));
    s.noise_reduction_db = static_cast<float>(sqlite3_column_double(stmt, 18));
    s.ref_similarity = static_cast<float>(sqlite3_column_double(stmt, 19));
    s.ref_noise_db = static_cast<float>(sqlite3_column_double(stmt, 20));
    s.ref_delay_ms = static_cast<float>(sqlite3_column_double(stmt, 21));
    s.alert_threshold_dbfs =
        static_cast<float>(sqlite3_column_double(stmt, 22));
    s.hum_alert_threshold_db =
        static_cast<float>(sqlite3_column_double(stmt, 23));
    s.snr_alert_threshold_db =
        static_cast<float>(sqlite3_column_double(stmt, 24));
    s.ref_similarity_threshold =
        static_cast<float>(sqlite3_column_double(stmt, 25));
    s.alert_debounce_periods =
        static_cast<uint32_t>(sqlite3_column_int(stmt, 26));
    s.is_alerting = sqlite3_column_int(stmt, 27) != 0;
    s.alert_level = static_cast<AlertLevel>(sqlite3_column_int(stmt, 28));
    s.plugin_degraded = sqlite3_column_int(stmt, 29) != 0;
    if (const unsigned char* t = sqlite3_column_text(stmt, 30))
      s.l3_match_type = reinterpret_cast<const char*>(t);
    s.l3_similarity = static_cast<float>(sqlite3_column_double(stmt, 31));
    // timestamp_ms 字段（快照内的相对帧计数）设为墙钟，与 DB 列一致，
    // 供 /history JSON 输出时间戳。
    s.timestamp_ms = r.timestamp_ms;
    out.push_back(std::move(r));
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<AlertHistoryRecord> NoiseStore::query_alerts(uint64_t from_ms,
                                                         uint64_t to_ms) const {
  std::vector<AlertHistoryRecord> out;
  if (!db_ || from_ms > to_ms)
    return out;
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT sensor_id,timestamp_ms,level,rule,message,is_active "
      "FROM alerts_history WHERE timestamp_ms>=? AND timestamp_ms<=? "
      "ORDER BY timestamp_ms ASC";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    std::cerr << "NoiseStore: prepare alerts query failed: "
              << sqlite3_errmsg(db_) << "\n";
    return out;
  }
  sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(from_ms));
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(to_ms));
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    AlertHistoryRecord r;
    r.sensor_id = static_cast<uint8_t>(sqlite3_column_int(stmt, 0));
    r.timestamp_ms = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
    r.event.level = static_cast<AlertLevel>(sqlite3_column_int(stmt, 2));
    if (const unsigned char* t = sqlite3_column_text(stmt, 3))
      r.event.rule = reinterpret_cast<const char*>(t);
    if (const unsigned char* t = sqlite3_column_text(stmt, 4))
      r.event.message = reinterpret_cast<const char*>(t);
    r.event.is_active = sqlite3_column_int(stmt, 5) != 0;
    r.event.sensor_id = r.sensor_id;
    r.event.raised_at_ms = r.timestamp_ms;
    out.push_back(std::move(r));
  }
  sqlite3_finalize(stmt);
  return out;
}

// ── 过期保留清理 ───────────────────────────────────────────────────────
size_t NoiseStore::run_retention_cleanup() {
  if (!db_)
    return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t cutoff =
      now_ms() - static_cast<uint64_t>(retention_hours_) * 3600ULL * 1000ULL;
  sqlite3_stmt* stmt = nullptr;
  size_t deleted = 0;
  // 分别清理两表。
  if (sqlite3_prepare_v2(db_,
                         "DELETE FROM metrics_history WHERE timestamp_ms<?", -1,
                         &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(cutoff));
    if (sqlite3_step(stmt) == SQLITE_DONE)
      deleted += static_cast<size_t>(sqlite3_changes(db_));
    sqlite3_finalize(stmt);
  }
  stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM alerts_history WHERE timestamp_ms<?",
                         -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(cutoff));
    if (sqlite3_step(stmt) == SQLITE_DONE)
      deleted += static_cast<size_t>(sqlite3_changes(db_));
    sqlite3_finalize(stmt);
  }
  return deleted;
}

size_t NoiseStore::count_metrics() const {
  if (!db_)
    return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt* stmt = nullptr;
  size_t n = 0;
  if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM metrics_history", -1, &stmt,
                         nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW)
      n = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    sqlite3_finalize(stmt);
  }
  return n;
}

size_t NoiseStore::count_alerts() const {
  if (!db_)
    return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt* stmt = nullptr;
  size_t n = 0;
  if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM alerts_history", -1, &stmt,
                         nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW)
      n = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    sqlite3_finalize(stmt);
  }
  return n;
}

}  // namespace noise

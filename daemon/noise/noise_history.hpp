// daemon/noise/noise_history.hpp
// Spec6 T1（D-S6.1）：NoiseStore 通用 SQLite 仓储。
// 架构依据：docs/superpowers/specs/noise-spec6-design.md §3.5（历史持久化）。
//
// 职责：把 per-sensor 指标快照 + 全局告警事件持久化到 SQLite（noise.sqlite），
// 支持时间范围查询 + 导出 + 过期保留清理。历史是首个用途，数据库名为通用名
// （noise.sqlite，未来可扩展其他数据存储）。
//
// 线程模型（约束：SQLite 线程安全 = WAL + 单写线程）：
//   - 写入（insert_metrics / insert_alerts / run_retention_cleanup）仅在
//     NoiseManager 的 housekeeper 控制线程调用（capture 线程只写内存 pending
//     队列，不接触 SQLite，避免 RT I/O）。
//   - 读取（query_metrics / query_alerts）由 HTTP 控制线程调用。
//   - 单 sqlite3* 连接 + 内部 mutex 串行化所有访问（防御性，与
//   SQLITE_THREADSAFE
//     串行模式叠加）。WAL 模式开启（D-S6.1，并发读 + 单写不互斥的
//     forward-compat）。
//
// 时间戳：所有 timestamp_ms 为墙钟毫秒（system_clock，Unix epoch），由调用方
// （housekeeper 在 drain pending 时）写入。时间范围查询 / 过期保留均基于墙钟。
#ifndef NOISE_NOISE_HISTORY_HPP_
#define NOISE_NOISE_HISTORY_HPP_

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "noise_metrics.hpp"  // NoiseMetricsSnapshot, AlertEvent

struct sqlite3;

namespace noise {

// 持久化记录：传感器 id + 墙钟时间戳 + 快照。housekeeper drain pending 后
// 批量插入；HTTP 历史查询/导出读取后还原为 NoiseMetricsSnapshot。
struct MetricsHistoryRecord {
  uint8_t sensor_id{0};
  uint64_t timestamp_ms{0};  // 墙钟毫秒
  NoiseMetricsSnapshot snapshot{};
};

// 持久化告警记录：sensor_id + 墙钟时间戳 + AlertEvent。告警引擎 raise/clear
// 时 push 到 pending，housekeeper 批量插入。
struct AlertHistoryRecord {
  uint8_t sensor_id{0};
  uint64_t timestamp_ms{0};  // 墙钟毫秒
  AlertEvent event{};
};

// NoiseStore：噪声历史 SQLite 仓储（D-S6.1）。
// 表：
//   metrics_history(sensor_id, timestamp_ms, <NoiseMetricsSnapshot 全字段>)
//     - 复合索引 (sensor_id, timestamp_ms) 支持时间范围查询
//   alerts_history(sensor_id, timestamp_ms, level, rule, message, is_active)
//     - 索引 (timestamp_ms) 支持全局时间范围查询
// WAL 模式 + 单写线程（housekeeper）。
class NoiseStore {
 public:
  // db_path 空字符串 -> 不打开数据库（所有写/读 no-op，返回空/0）。
  //   用于 WITH_NOISE=ON 但未配置 noise_db_path 的降级场景。
  // retention_hours：历史保留时长（默认 24h），0 = 立即过期（测试用）。
  // flush_interval_s：housekeeper flush 间隔（仅文档化，实际节流由
  //   NoiseManager 的 housekeeper 线程控制，本类不持有线程）。
  NoiseStore(const std::string& db_path,
             uint32_t retention_hours = 24,
             uint32_t flush_interval_s = 10);
  ~NoiseStore();

  // 不可拷贝（持有 sqlite3* 连接 + mutex）。
  NoiseStore(const NoiseStore&) = delete;
  NoiseStore& operator=(const NoiseStore&) = delete;

  // 批量插入（housekeeper 控制线程调用）。空 vector no-op。失败记日志不抛。
  void insert_metrics(const std::vector<MetricsHistoryRecord>& records);
  void insert_alerts(const std::vector<AlertHistoryRecord>& records);

  // 时间范围查询（HTTP 控制线程调用，wall-clock ms）。
  // query_metrics：指定 sensor_id 的指标历史，按 timestamp_ms 升序。
  // query_alerts：所有 sensor 的告警历史，按 timestamp_ms 升序。
  // from > to 或无记录 -> 返回空 vector。
  std::vector<MetricsHistoryRecord> query_metrics(uint8_t sensor_id,
                                                  uint64_t from_ms,
                                                  uint64_t to_ms) const;
  std::vector<AlertHistoryRecord> query_alerts(uint64_t from_ms,
                                               uint64_t to_ms) const;

  // 过期保留清理：DELETE WHERE timestamp_ms < now - retention_hours。
  // 返回删除行数。housekeeper 定时调用。retention_hours=0 删除所有历史记录。
  size_t run_retention_cleanup();

  // 测试钩子：返回表行数（持锁 count）。
  size_t count_metrics() const;
  size_t count_alerts() const;

  // 测试钩子：数据库是否成功打开（db_path 非空且 sqlite3_open 成功）。
  bool is_open() const { return db_ != nullptr; }
  // flush 间隔（构造时传入，供 NoiseManager housekeeper 同步节流）。
  uint32_t get_flush_interval_s() const { return flush_interval_s_; }

 private:
  // 建表 + 索引 + PRAGMA（WAL）。构造时调用。
  void init_schema();

  sqlite3* db_{nullptr};
  uint32_t retention_hours_{24};
  uint32_t flush_interval_s_{10};
  // 串行化所有 sqlite3 调用（写 + 读）。防御性：即使系统 sqlite3 未编译
  // serialized 模式也安全。housekeeper 写 + HTTP 读互斥，但二者均低频
  // （写 ~10s 一次，读 UI 手动轮询），mutex 非 contends。
  mutable std::mutex mutex_;
};

}  // namespace noise

#endif  // NOISE_NOISE_HISTORY_HPP_

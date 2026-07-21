// daemon/noise/noise_status.hpp
// Spec3 Task 4：数据持久化基建。
// 架构依据：docs/noise/architecture-design.md §7.1（原子写设计原则）。
//
// write_atomic：写临时文件 + std::filesystem::rename 原子替换。
// 与 daemon 的 status.json 直接覆写不同——噪声模块数据对崩溃恢复更敏感
// （长期运行无人值守监测场景，arch §7.1），故采用 tmp+rename 保证不留半写。
//
// JSON 输出模式（arch §7.4/§7.5 + §5.4 约定）：
// - 输出：std::stringstream 手工拼接 + escape_json() 转义。数字/bool 不加
//   引号（与 daemon/json.cpp + noise_http.cpp 同一模式）。
// - 输入：boost::property_tree::ptree + read_json（daemon 既有模式）。
// - 不用 boost::property_tree::write_json 输出（会将所有值引号化，违反约定）。
#ifndef NOISE_NOISE_STATUS_HPP_
#define NOISE_NOISE_STATUS_HPP_

#include <string>

namespace noise {

// 原子写入文件：先写 path+".tmp"，再 std::filesystem::rename 替换 path。
// 若父目录不存在，自动创建（std::filesystem::create_directories）。
// 返回 true 表示写入成功，false 表示 I/O 错误（errno 记录在 stderr）。
bool write_atomic(const std::string& path, const std::string& content);

}  // namespace noise

#endif  // NOISE_NOISE_STATUS_HPP_

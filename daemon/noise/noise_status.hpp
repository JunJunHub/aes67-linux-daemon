// daemon/noise/noise_status.hpp
// Spec3 Task 4：数据持久化基建。
// 架构依据：docs/noise/architecture-design.md §7.1（原子写设计原则）。
//
// write_atomic：写临时文件 + std::filesystem::rename 原子替换。
// 与 daemon 的 status.json 直接覆写不同--噪声模块数据对崩溃恢复更敏感
// （长期运行无人值守监测场景，arch §7.1），故采用 tmp+rename 保证不留半写。
//
// JSON 输出模式（arch §7.4/§7.5 + §5.4 约定）：
// - 输出：std::stringstream 手工拼接 + escape_json() 转义。数字/bool 不加
//   引号（与 daemon/json.cpp + noise_http.cpp 同一模式）。
// - 输入：boost::property_tree::ptree + read_json（daemon 既有模式）。
// - 不用 boost::property_tree::write_json 输出（会将所有值引号化，违反约定）。
//
// escape_json：Task4 review Minor #5 去重 - 从 noise_manager.cpp /
// noise_template_db.cpp 的 anonymous namespace 副本提取为共享 inline 函数。
// 与 daemon/json.cpp L42-78 同一实现（不跨模块依赖 daemon 的 static 函数）。
#ifndef NOISE_NOISE_STATUS_HPP_
#define NOISE_NOISE_STATUS_HPP_

#include <iomanip>
#include <sstream>
#include <string>

namespace noise {

// 原子写入文件：先写 path+".tmp"，再 std::filesystem::rename 替换 path。
// 若父目录不存在，自动创建（std::filesystem::create_directories）。
// 返回 true 表示写入成功，false 表示 I/O 错误（errno 记录在 stderr）。
bool write_atomic(const std::string& path, const std::string& content);

// JSON 字符串转义（daemon/json.cpp L42-78 复制，供 noise 模块持久化
// 序列化共享）。处理 " \\ \b \f \n \r \t + 控制字符 \u00XX。
// 内联定义避免跨编译单元重复拷贝（review Minor #5 去重）。
inline std::string escape_json(const std::string& s) {
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

}  // namespace noise

#endif  // NOISE_NOISE_STATUS_HPP_

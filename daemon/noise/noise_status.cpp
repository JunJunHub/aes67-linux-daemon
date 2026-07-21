// daemon/noise/noise_status.cpp
// Spec3 Task 4：write_atomic 实现。架构依据：arch §7.1。
#include "noise_status.hpp"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>

namespace noise {

bool write_atomic(const std::string& path, const std::string& content) {
  if (path.empty())
    return false;
  // 父目录自动创建（arch §7.3 注：noise_template_dir 指定目录若不存在，
  // 首次保存时自动创建）。
  std::filesystem::path p(path);
  std::filesystem::path parent = p.parent_path();
  if (!parent.empty() && !std::filesystem::exists(parent)) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      std::cerr << "write_atomic: create_directories failed for " << parent
                << ": " << ec.message() << std::endl;
      return false;
    }
  }
  // 1) 写临时文件 path + ".tmp"
  std::string tmp_path = path + ".tmp";
  std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    std::cerr << "write_atomic: cannot open " << tmp_path
              << " for writing (errno=" << errno << ")" << std::endl;
    return false;
  }
  out << content;
  out.flush();
  if (!out.good()) {
    std::cerr << "write_atomic: write error to " << tmp_path << std::endl;
    out.close();
    // 清理半写 tmp 文件（best-effort）
    std::error_code ec;
    std::filesystem::remove(tmp_path, ec);
    return false;
  }
  out.close();
  // 2) 原子 rename：同文件系统上 std::filesystem::rename 是原子操作。
  std::error_code ec;
  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    std::cerr << "write_atomic: rename " << tmp_path << " -> " << path
              << " failed: " << ec.message() << std::endl;
    std::filesystem::remove(tmp_path, ec);
    return false;
  }
  return true;
}

}  // namespace noise

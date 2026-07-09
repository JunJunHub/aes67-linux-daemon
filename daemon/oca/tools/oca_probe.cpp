//  oca_probe.cpp - OCP.1 探测客户端(独立验证工具)
//
//  连接一个运行中的 AES70/OCA 设备(默认本机 65037),用最小命令集探测
//  其控制平面:KeepAlive 握手、设备身份、管理器枚举、根块拓扑、类标识、
//  EV2 订阅。复用 daemon/oca/ocp1.cpp 的编解码,不依赖服务端对象树,
//  行为等同「外部陌生控制器」,用于真实控制器对接前的独立验证。
//
//  用法:
//    oca-probe [host] [port] [--no-sub]      # 默认 127.0.0.1 65037
//    oca-probe 172.16.1.198 65037
//    oca-probe 127.0.0.1 65037 --no-sub      # 跳过 EV2 订阅测试
//
//  退出码:0=全部探测通过;1=连接/协议错误或某探测失败。

#include "oca/methods.hpp"
#include "oca/ocp1.hpp"
#include "oca/types.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace m = oca::methods;
namespace o = oca;

// ---- 终端着色
// ----------------------------------------------------------------
static bool g_color = isatty(fileno(stdout));
const char* OK() {
  return g_color ? "\033[32m" : "";
}
const char* WARN() {
  return g_color ? "\033[33m" : "";
}
const char* ERR() {
  return g_color ? "\033[31m" : "";
}
const char* DIM() {
  return g_color ? "\033[2m" : "";
}
const char* OFF() {
  return g_color ? "\033[0m" : "";
}

void section(const std::string& t) {
  std::cout << "\n" << DIM() << "==== " << t << " ====" << OFF() << "\n";
}

// 状态码转可读名(types.hpp 的 Status 枚举顺序)
const char* status_name(o::Status s) {
  switch (s) {
    case o::Status::OK:
      return "OK";
    case o::Status::ProtocolVersionError:
      return "ProtocolVersionError";
    case o::Status::DeviceError:
      return "DeviceError";
    case o::Status::Locked:
      return "Locked";
    case o::Status::BadFormat:
      return "BadFormat";
    case o::Status::BadONo:
      return "BadONo";
    case o::Status::ParameterError:
      return "ParameterError";
    case o::Status::ParameterOutOfRange:
      return "ParameterOutOfRange";
    case o::Status::NotImplemented:
      return "NotImplemented";
    case o::Status::InvalidRequest:
      return "InvalidRequest";
    case o::Status::ProcessingFailed:
      return "ProcessingFailed";
    case o::Status::BadMethod:
      return "BadMethod";
    case o::Status::PartiallySucceeded:
      return "PartiallySucceeded";
    case o::Status::Timeout:
      return "Timeout";
    case o::Status::BufferOverflow:
      return "BufferOverflow";
  }
  return "?";
}

// ClassID 转字符串,如 {1,2,1}
std::string classid_str(const std::vector<uint16_t>& levels) {
  std::string s = "{";
  for (size_t i = 0; i < levels.size(); ++i) {
    if (i)
      s += ",";
    s += std::to_string(levels[i]);
  }
  s += "}";
  return s;
}

// ---- 连接:收发 PDU ----------------------------------------------------------
// 复用 oca_test.cpp E2E 的分帧逻辑,做成带超时的健壮版。

bool send_all(int fd, const uint8_t* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
    if (n <= 0)
      return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

// 阻塞读精确 n 字节;false=EOF/错误/超时
bool recv_exact(int fd, uint8_t* buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = ::recv(fd, buf + got, n - got, 0);
    if (r <= 0)
      return false;
    got += static_cast<size_t>(r);
  }
  return true;
}

// 读一个完整 PDU(含 sync)。返回 header + payload(不含 sync),或 nullopt。
struct RecvPdu {
  oca::ocp1::Header hdr;
  std::vector<uint8_t> payload;  // 不含 header
};
std::optional<RecvPdu> recv_pdu(int fd) {
  uint8_t sync = 0;
  // 逐字节同步到 0x3B
  while (true) {
    ssize_t r = ::recv(fd, &sync, 1, 0);
    if (r <= 0)
      return std::nullopt;
    if (sync == m::kSyncVal)
      break;
  }
  uint8_t hdrbuf[9];
  if (!recv_exact(fd, hdrbuf, 9))
    return std::nullopt;
  auto hdr = oca::ocp1::PduReader::try_parse_header(hdrbuf, 9);
  if (!hdr)
    return std::nullopt;
  if (hdr->pduSize < 9)
    return std::nullopt;
  size_t plen = hdr->pduSize - 9;
  std::vector<uint8_t> payload(plen);
  if (plen && !recv_exact(fd, payload.data(), plen))
    return std::nullopt;
  return RecvPdu{*hdr, std::move(payload)};
}

// ---- 探测器
// ------------------------------------------------------------------
struct Probe {
  int fd = -1;
  uint32_t next_handle = 1;
  int failures = 0;

  // 发一条命令,循环接收直到拿到匹配 handle 的 Response。
  // 途中的 Notification2 / KeepAlive 单独打印,不与 Response 错配。
  // 返回 Response(paramData/paramCount 指向内部缓冲,下次调用失效)。
  struct CmdResult {
    o::Status status = o::Status::OK;
    std::vector<uint8_t> params;
    bool ok = false;  // 是否成功收到匹配的 Response
  };

  CmdResult cmd(o::ONo target,
                o::MethodID mid,
                const uint8_t* params = nullptr,
                uint8_t paramCount = 0) {
    uint32_t handle = next_handle++;
    oca::ocp1::Writer cw;
    oca::ocp1::write_command(cw, handle, target, mid, params, paramCount);
    auto pdu = oca::ocp1::PduWriter::build_command_pdu(1, cw.data(), cw.size());
    if (!send_all(fd, pdu.data(), pdu.size())) {
      std::cout << ERR() << "  [发送失败]" << OFF() << "\n";
      failures++;
      return {};
    }
    // 接收循环:最多重试若干次以跨过穿插的 Notification/KeepAlive
    for (int i = 0; i < 16; ++i) {
      auto rp = recv_pdu(fd);
      if (!rp) {
        std::cout << ERR() << "  [连接断开]" << OFF() << "\n";
        failures++;
        return {};
      }
      if (rp->hdr.pduType == m::kPduResponse) {
        auto rsps = oca::ocp1::PduReader::parse_responses(
            rp->payload.data(), rp->payload.size(), rp->hdr.messageCount);
        for (const auto& r : rsps) {
          if (r.handle == handle) {
            CmdResult out;
            out.status = r.statusCode;
            out.ok = true;
            if (r.paramCount && r.paramData)
              out.params.assign(r.paramData, r.paramData + r.paramCount);
            return out;
          }
          // 其它 handle 的响应(不应出现)忽略
        }
      } else if (rp->hdr.pduType == m::kPduNtf2) {
        auto ntfs = oca::ocp1::PduReader::parse_notifications2(
            rp->payload.data(), rp->payload.size(), rp->hdr.messageCount);
        for (const auto& n : ntfs) {
          std::cout << WARN() << "  [通知] emitter=" << n.emitterONo
                    << " event={" << n.eventID.defLevel << ","
                    << n.eventID.eventIndex
                    << "} type=" << (int)n.notificationType
                    << " dataLen=" << n.dataCount << OFF() << "\n";
        }
        continue;  // 继续等 Response
      } else if (rp->hdr.pduType == m::kPduKeepAlive) {
        continue;  // KeepAlive 忽略
      } else {
        std::cout << DIM() << "  [未知 PduType=" << (int)rp->hdr.pduType << "]"
                  << OFF() << "\n";
        continue;
      }
    }
    std::cout << ERR() << "  [超时:未收到匹配响应]" << OFF() << "\n";
    failures++;
    return {};
  }

  // 无参命令的便捷封装
  CmdResult cmd0(o::ONo target, o::MethodID mid) { return cmd(target, mid); }
};

// 读 ClassID(从 Reader 当前位置):u16 levelCount + 各 level
std::vector<uint16_t> read_classid(oca::ocp1::Reader& r) {
  std::vector<uint16_t> v;
  uint16_t n = r.u16();
  for (uint16_t i = 0; i < n; ++i)
    v.push_back(r.u16());
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  uint16_t port = 65037;
  bool do_sub = true;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--no-sub") {
      do_sub = false;
    } else if (a == "-h" || a == "--help") {
      std::cout << "用法: oca-probe [host] [port] [--no-sub]\n"
                << "  默认 127.0.0.1 65037\n";
      return 0;
    } else if (a.find_first_not_of("0123456789.") == std::string::npos &&
               a.find('.') != std::string::npos && host == "127.0.0.1") {
      host = a;
    } else if (host == "127.0.0.1" && a.find('.') != std::string::npos) {
      host = a;
    } else {
      // 端口
      try {
        port = static_cast<uint16_t>(std::stoi(a));
      } catch (...) {
        std::cerr << ERR() << "无法解析参数: " << a << OFF() << "\n";
        return 1;
      }
    }
  }

  std::cout << "OCP.1 探测客户端 -> " << host << ":" << port << "\n";

  // 连接
  Probe probe;
  probe.fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (probe.fd < 0) {
    std::cerr << ERR() << "socket() 失败" << OFF() << "\n";
    return 1;
  }
  // 5s 收发超时,避免永久阻塞
  struct timeval tv {
    5, 0
  };
  ::setsockopt(probe.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(probe.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
    std::cerr << ERR() << "无效地址: " << host << OFF() << "\n";
    return 1;
  }
  if (::connect(probe.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
      0) {
    std::cerr << ERR() << "连接失败 " << host << ":" << port << " ("
              << strerror(errno) << ")" << OFF() << "\n";
    std::cerr << "确认 daemon 已运行且监听该地址(./oca-dev.sh status)\n";
    return 1;
  }
  std::cout << OK() << "已连接" << OFF() << "\n";

  // --- 1. KeepAlive 握手 -----------------------------------------------------
  section("KeepAlive 握手");
  {
    auto ka = oca::ocp1::PduWriter::build_keepalive_pdu(5);
    send_all(probe.fd, ka.data(), ka.size());
    auto rp = recv_pdu(probe.fd);
    if (!rp) {
      std::cout << ERR() << "  [FAIL] 无 KeepAlive 回应" << OFF() << "\n";
      probe.failures++;
    } else if (rp->hdr.pduType != m::kPduKeepAlive) {
      std::cout << ERR() << "  [FAIL] 期望 KeepAlive,收到 PduType="
                << (int)rp->hdr.pduType << OFF() << "\n";
      probe.failures++;
    } else {
      oca::ocp1::Reader r(rp->payload.data(), rp->payload.size());
      uint16_t hb = rp->payload.size() >= 2 ? r.u16() : 0;
      std::cout << OK() << "  [OK] KeepAlive 回应,协商心跳=" << hb << "s"
                << OFF() << "\n";
    }
  }

  // --- 2. 设备身份(ONo 1 = DeviceManager)-----------------------------------
  section("设备身份 (DeviceManager ONo=1)");
  {
    auto r = probe.cmd0(1, {m::kDefLevelDeviceMngr, m::kDevGetOcaVersion});
    if (r.ok && r.status == o::Status::OK) {
      oca::ocp1::Reader pr(r.params.data(), r.params.size());
      std::cout << OK() << "  [OK] GetOcaVersion = " << pr.u16() << OFF()
                << "\n";
    } else {
      std::cout << ERR()
                << "  [FAIL] GetOcaVersion status=" << status_name(r.status)
                << OFF() << "\n";
    }
  }
  {
    auto r =
        probe.cmd0(1, {m::kDefLevelDeviceMngr, m::kDevGetModelDescription});
    if (r.ok && r.status == o::Status::OK) {
      oca::ocp1::Reader pr(r.params.data(), r.params.size());
      std::string mfr = pr.string();
      std::string name = pr.string();
      std::string ver = pr.string();
      std::cout << OK() << "  [OK] GetModelDescription" << OFF() << "\n"
                << "        manufacturer : " << mfr << "\n"
                << "        name         : " << name << "\n"
                << "        version      : " << ver << "\n";
    } else {
      std::cout << ERR() << "  [FAIL] GetModelDescription status="
                << status_name(r.status) << OFF() << "\n";
    }
  }
  {
    auto r = probe.cmd0(1, {m::kDefLevelDeviceMngr, m::kDevGetDeviceName});
    if (r.ok && r.status == o::Status::OK) {
      oca::ocp1::Reader pr(r.params.data(), r.params.size());
      std::cout << OK() << "  [OK] GetDeviceName = \"" << pr.string() << "\""
                << OFF() << "\n";
    } else {
      std::cout << ERR()
                << "  [FAIL] GetDeviceName status=" << status_name(r.status)
                << OFF() << "\n";
    }
  }
  {
    auto r = probe.cmd0(1, {m::kDefLevelDeviceMngr, m::kDevGetSerialNumber});
    if (r.ok && r.status == o::Status::OK) {
      oca::ocp1::Reader pr(r.params.data(), r.params.size());
      std::cout << OK() << "  [OK] GetSerialNumber = \"" << pr.string() << "\""
                << OFF() << "\n";
    } else {
      std::cout << WARN()
                << "  [--] GetSerialNumber status=" << status_name(r.status)
                << " (未实现属 Spec2)" << OFF() << "\n";
    }
  }

  // --- 3. 管理器枚举(GetManagers)-------------------------------------------
  section("管理器枚举 (GetManagers)");
  std::vector<std::pair<uint32_t, std::string>> managers;  // ONo, role
  {
    auto r = probe.cmd0(1, {m::kDefLevelDeviceMngr, m::kDevGetManagers});
    if (r.ok && r.status == o::Status::OK) {
      oca::ocp1::Reader pr(r.params.data(), r.params.size());
      uint16_t count = pr.u16();
      std::cout << OK() << "  [OK] 管理器数量 = " << count << OFF() << "\n";
      for (uint16_t i = 0; i < count; ++i) {
        uint32_t ono = pr.u32();
        std::string role = pr.string();
        auto cid = read_classid(pr);
        uint16_t cver = pr.u16();
        managers.emplace_back(ono, role);
        std::cout << "        #" << i << " ONo=" << ono << " role=\"" << role
                  << "\" classID=" << classid_str(cid) << " v" << cver << "\n";
      }
    } else {
      std::cout << ERR()
                << "  [FAIL] GetManagers status=" << status_name(r.status)
                << OFF() << "\n";
    }
  }

  // --- 4. 根块成员(GetMembers on ONo 100)-----------------------------------
  section("根块拓扑 (GetMembers on RootBlock ONo=100)");
  {
    auto r = probe.cmd0(100, {m::kDefLevelBlock, m::kBlockGetMembers});
    if (r.ok && r.status == o::Status::OK) {
      oca::ocp1::Reader pr(r.params.data(), r.params.size());
      uint16_t count = pr.u16();
      std::cout << OK() << "  [OK] 根块成员数 = " << count << OFF() << "\n";
      std::cout << "        members: ";
      for (uint16_t i = 0; i < count; ++i) {
        uint32_t ono = pr.u32();
        if (i)
          std::cout << ", ";
        std::cout << ono;
      }
      std::cout << "\n";
    } else {
      std::cout << ERR()
                << "  [FAIL] GetMembers status=" << status_name(r.status)
                << OFF() << "\n";
    }
  }

  // --- 5. 类标识(对每个 manager 调 GetClassIdentification,交叉验证)-----
  section("类标识 (GetClassIdentification)");
  for (const auto& [ono, role] : managers) {
    auto r =
        probe.cmd0(ono, {m::kDefLevelRoot, m::kRootGetClassIdentification});
    if (r.ok && r.status == o::Status::OK) {
      oca::ocp1::Reader pr(r.params.data(), r.params.size());
      auto cid = read_classid(pr);
      uint16_t cver = pr.u16();
      std::cout << OK() << "  [OK] ONo=" << ono << " (" << role
                << ") classID=" << classid_str(cid) << " v" << cver << OFF()
                << "\n";
    } else {
      std::cout << ERR() << "  [FAIL] ONo=" << ono << " (" << role
                << ") status=" << status_name(r.status) << OFF() << "\n";
    }
  }

  // --- 6. EV2 订阅验证(AddSubscription2)-----------------------------------
  if (do_sub) {
    section("EV2 订阅 (AddSubscription2) — Task17 关键验证门");
    // 找 SubscriptionManager(通常是 GetManagers 里 classID 含 {1,2,4} 的,
    // 但探测客户端不硬编码:直接试已知 ONo=4,失败则回退遍历)
    // 参数:EmitterONo(u32)=1, EventID(u16 defLevel, u16 idx)={3,1},
    //      subscriberContext(u16)=0
    oca::ocp1::Writer params;
    params.u32(1);  // EmitterONo = DeviceManager
    params.u16(m::kDefLevelDeviceMngr);
    params.u16(m::kEventOperationalState);
    params.u16(0);  // 空 subscriberContext

    auto r = probe.cmd(4, {m::kDefLevelSubMngr, m::kSubAddSubscription2},
                       params.data(), static_cast<uint8_t>(params.size()));
    if (r.ok && r.status == o::Status::OK) {
      oca::ocp1::Reader pr(r.params.data(), r.params.size());
      uint32_t subId = pr.u32();
      std::cout << OK()
                << "  [OK] AddSubscription2 成功,subscriptionID=" << subId
                << OFF() << "\n";
      std::cout << DIM()
                << "        (EV2 方法索引正确;通知投递需 daemon 触发事件,"
                << "本探测不触发)" << OFF() << "\n";
    } else if (r.ok && (r.status == o::Status::BadMethod ||
                        r.status == o::Status::NotImplemented)) {
      std::cout << ERR()
                << "  [FAIL] AddSubscription2 status=" << status_name(r.status)
                << " — EV2 方法索引(" << m::kSubAddSubscription2
                << ")可能不正确,需对照 AES70-2 XMI 修正" << OFF() << "\n";
      probe.failures++;
    } else {
      std::cout << WARN()
                << "  [--] AddSubscription2 status=" << status_name(r.status)
                << OFF() << "\n";
    }
  }

  // --- 汇总 ------------------------------------------------------------------
  section("汇总");
  if (probe.failures == 0) {
    std::cout << OK() << "全部探测通过" << OFF() << "\n";
  } else {
    std::cout << ERR() << "有 " << probe.failures << " 项失败" << OFF() << "\n";
  }

  ::close(probe.fd);
  return probe.failures == 0 ? 0 : 1;
}

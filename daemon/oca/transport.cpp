//  transport.cpp - TCP 传输实现

#include "oca/transport.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <utility>

#include "oca/classes/subscription_manager.hpp"
#include "oca/methods.hpp"
#include "oca/ocp1.hpp"
#include "oca/session.hpp"

namespace oca {

Transport::Transport(ObjectRegistry* registry, OcaSubscriptionManager* sub_mgr)
    : registry_(registry), sub_mgr_(sub_mgr) {}

Transport::~Transport() {
  stop();
}

bool Transport::start(uint16_t port) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0)
    return false;
  int yes = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
      0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (::listen(listen_fd_, 8) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  // 取实际端口
  socklen_t l = sizeof(addr);
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &l);
  port_ = ntohs(addr.sin_port);
  running_ = true;
  accept_thread_ = std::thread(&Transport::accept_loop, this);
  return true;
}

void Transport::stop() {
  if (!running_.exchange(false))
    return;
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (accept_thread_.joinable())
    accept_thread_.join();
  std::lock_guard<std::mutex> lk(conns_mutex_);
  for (auto& t : conn_threads_) {
    if (t.joinable())
      t.join();
  }
  conn_threads_.clear();
}

void Transport::accept_loop() {
  while (running_) {
    int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (!running_)
        break;
      continue;
    }
    // 1s 接收超时,便于排空通知队列与检测心跳
    struct timeval tv {
      1, 0
    };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ONo sid = next_session_id_.fetch_add(1);
    std::thread t(&Transport::conn_loop, this, fd, sid);
    std::lock_guard<std::mutex> lk(conns_mutex_);
    conn_threads_.push_back(std::move(t));
    // 清理已结束的线程
    for (auto it = conn_threads_.begin(); it != conn_threads_.end();) {
      // 无法非阻塞 join,保留线程;stop() 时统一 join。这里仅去重已 detach
      // 的(无)。
      ++it;
    }
  }
}

bool Transport::send_all(int fd, const uint8_t* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
    if (n <= 0)
      return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

// 阻塞读取精确 n 字节;false = EOF/错误/超时中途
static bool recv_exact(int fd, uint8_t* buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = ::recv(fd, buf + got, n - got, 0);
    if (r <= 0)
      return false;  // EOF/错误/超时
    got += static_cast<size_t>(r);
  }
  return true;
}

static uint64_t now_sec() {
  return static_cast<uint64_t>(std::time(nullptr));
}

void Transport::conn_loop(int fd, ONo session_id) {
  Session sess(session_id);
  sess.set_registry(registry_);
  std::mutex write_mutex;

  auto send_pdu = [&](const std::vector<uint8_t>& pdu) {
    std::lock_guard<std::mutex> lk(write_mutex);
    send_all(fd, pdu.data(), pdu.size());
  };

  while (running_) {
    // 1. 读 sync 字节
    uint8_t b = 0;
    ssize_t r = ::recv(fd, &b, 1, 0);
    if (r <= 0) {
      if (r < 0 && errno == EAGAIN) {
        // 空闲超时:排空通知队列、检测心跳
        std::vector<uint8_t> pdu;
        while (sess.take_notification(pdu))
          send_pdu(pdu);
        if (sess.expired(now_sec()))
          break;
        continue;
      }
      break;  // EOF/错误
    }
    if (b != methods::kSyncVal)
      continue;  // 重新同步

    // 2. 读 9 字节头
    uint8_t hdrbuf[9];
    if (!recv_exact(fd, hdrbuf, 9))
      break;
    auto hdr = ocp1::PduReader::try_parse_header(hdrbuf, 9);
    if (!hdr || hdr->protocolVersion != methods::kProtocolVersion)
      break;
    if (hdr->pduSize < 9)
      break;

    // 3. 读 payload(pduSize - 9 字节)
    size_t payloadLen = hdr->pduSize - 9;
    std::vector<uint8_t> payload(payloadLen);
    if (payloadLen && !recv_exact(fd, payload.data(), payloadLen))
      break;

    sess.touch(now_sec());

    // 4. 按 PduType 分派
    if (hdr->pduType == methods::kPduKeepAlive) {
      uint16_t hb = 15;
      if (payloadLen >= 2) {
        ocp1::Reader hr(payload.data(), payloadLen);
        hb = hr.u16();
      }
      sess.set_heartbeat(hb);
      send_pdu(ocp1::PduWriter::build_keepalive_pdu(hb));  // 回应 KeepAlive
    } else if (hdr->pduType == methods::kPduCommand ||
               hdr->pduType == methods::kPduCommandRrq) {
      auto cmds = ocp1::PduReader::parse_commands(payload.data(), payloadLen,
                                                  hdr->messageCount);
      ocp1::Writer rspAcc;
      for (const auto& c : cmds) {
        ocp1::Writer params;
        Status st;
        Object* obj = registry_->find(c.targetONo);
        if (!obj) {
          st = Status::BadONo;
        } else {
          ocp1::Reader pr(c.paramData, c.paramCount);
          st = obj->exec(c.methodID, pr, params, sess);
        }
        ocp1::write_response(rspAcc, c.handle, st, params.data(),
                             static_cast<uint8_t>(params.size()));
      }
      send_pdu(ocp1::PduWriter::build_response_pdu(
          static_cast<uint16_t>(cmds.size()), rspAcc.data(), rspAcc.size()));
    }
    // 其他 PduType(Response/Ntf)在设备侧忽略

    // 5. 排空通知队列
    std::vector<uint8_t> pdu;
    while (sess.take_notification(pdu))
      send_pdu(pdu);
  }

  if (sub_mgr_)
    sub_mgr_->remove_session(&sess);
  ::close(fd);
}

}  // namespace oca

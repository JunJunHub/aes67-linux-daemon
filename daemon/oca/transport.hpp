//  transport.hpp - OCP.1 TCP 传输层

#ifndef OCA_TRANSPORT_HPP_
#define OCA_TRANSPORT_HPP_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "oca/object.hpp"

namespace oca {

class OcaSubscriptionManager;

class Transport {
 public:
  Transport(ObjectRegistry* registry,
            OcaSubscriptionManager* sub_mgr = nullptr);
  ~Transport();
  bool start(uint16_t port);  // 0 = 自动分配
  void stop();
  uint16_t port() const { return port_; }

 private:
  void accept_loop();
  void conn_loop(int fd, ONo session_id);
  static bool send_all(int fd, const uint8_t* data, size_t len);

  ObjectRegistry* registry_;
  OcaSubscriptionManager* sub_mgr_;
  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::atomic<bool> running_{false};
  std::thread accept_thread_;
  std::atomic<ONo> next_session_id_{1};
  std::mutex conns_mutex_;
  std::vector<std::thread> conn_threads_;
};

}  // namespace oca

#endif  // OCA_TRANSPORT_HPP_

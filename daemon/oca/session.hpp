//  session.hpp - OCP.1 控制会话(每 TCP 连接一个)

#ifndef OCA_SESSION_HPP_
#define OCA_SESSION_HPP_

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "oca/object.hpp"
#include "oca/types.hpp"

namespace oca {

struct Subscription2 {
  ONo emitterONo = 0;
  EventID eventID{};
  OcaBlob subscriberContext;
};

class Session {
 public:
  explicit Session(ONo id) : id_(id) {}

  ONo session_id() const { return id_; }

  void set_registry(ObjectRegistry* r) { registry_ = r; }
  ObjectRegistry* registry() const { return registry_; }

  // 订阅(EV2)
  void add_subscription(const Subscription2& sub);
  void remove_subscription(ONo emitter, EventID event);
  bool has_subscription(ONo emitter, EventID event) const;
  std::vector<Subscription2> subscriptions() const;

  // 写队列(线程安全)
  void enqueue_notification(std::vector<uint8_t> pdu);
  bool take_notification(std::vector<uint8_t>& out);

  // KeepAlive / 心跳
  void set_heartbeat(uint16_t sec) { heartbeat_sec_ = sec; }
  uint16_t heartbeat() const { return heartbeat_sec_; }
  void touch(uint64_t now_sec) { last_seen_sec_ = now_sec; }
  bool expired(uint64_t now_sec) const {
    return now_sec > last_seen_sec_ &&
           (now_sec - last_seen_sec_) > 3u * heartbeat_sec_;
  }

 private:
  ONo id_;
  ObjectRegistry* registry_ = nullptr;
  mutable std::mutex mutex_;
  std::vector<Subscription2> subs_;
  std::deque<std::vector<uint8_t>> write_queue_;
  uint16_t heartbeat_sec_ = 15;
  uint64_t last_seen_sec_ = 0;
};

}  // namespace oca

#endif  // OCA_SESSION_HPP_

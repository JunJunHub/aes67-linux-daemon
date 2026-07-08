//  session.cpp - Session 实现

#include "oca/session.hpp"

#include <algorithm>

namespace oca {

void Session::add_subscription(const Subscription2& sub) {
  std::lock_guard<std::mutex> lk(mutex_);
  // 去重:同一 emitter+event 只存一份
  for (const auto& s : subs_) {
    if (s.emitterONo == sub.emitterONo &&
        s.eventID.defLevel == sub.eventID.defLevel &&
        s.eventID.eventIndex == sub.eventID.eventIndex) {
      return;
    }
  }
  subs_.push_back(sub);
}

void Session::remove_subscription(ONo emitter, EventID event) {
  std::lock_guard<std::mutex> lk(mutex_);
  subs_.erase(std::remove_if(subs_.begin(), subs_.end(),
      [&](const Subscription2& s) {
        return s.emitterONo == emitter &&
               s.eventID.defLevel == event.defLevel &&
               s.eventID.eventIndex == event.eventIndex;
      }), subs_.end());
}

bool Session::has_subscription(ONo emitter, EventID event) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& s : subs_) {
    if (s.emitterONo == emitter &&
        s.eventID.defLevel == event.defLevel &&
        s.eventID.eventIndex == event.eventIndex) {
      return true;
    }
  }
  return false;
}

std::vector<Subscription2> Session::subscriptions() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return subs_;
}

void Session::enqueue_notification(std::vector<uint8_t> pdu) {
  std::lock_guard<std::mutex> lk(mutex_);
  write_queue_.push_back(std::move(pdu));
}

bool Session::take_notification(std::vector<uint8_t>& out) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (write_queue_.empty()) return false;
  out = std::move(write_queue_.front());
  write_queue_.pop_front();
  return true;
}

}  // namespace oca

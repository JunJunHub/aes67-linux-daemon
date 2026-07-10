//  classes/subscription_manager.cpp

#include "oca/classes/subscription_manager.hpp"

#include <algorithm>

#include "oca/methods.hpp"
#include "oca/ocp1.hpp"

namespace oca {

namespace {
const ClassIdentification kSubscriptionManagerClassId = {{{1, 2, 4}}, 2};
}  // namespace

const ClassIdentification& OcaSubscriptionManager::class_id() const {
  return kSubscriptionManagerClassId;
}

ExecResult OcaSubscriptionManager::exec(MethodID m,
                                        ocp1::Reader& req,
                                        ocp1::Writer& rsp,
                                        Session& sess) {
  if (m.defLevel == methods::kDefLevelSubMngr) {
    switch (m.methodIndex) {
      case methods::kSubAddSubscription2:
        return AddSubscription2(req, rsp, sess);
      case methods::kSubRemoveSubscription2:
        return RemoveSubscription2(req, rsp, sess);
      default:
        return {Status::BadMethod, 0};  // PropertyChange 变体 Spec1 不实现
    }
  }
  return OcaManager::exec(m, req, rsp, sess);
}

ExecResult OcaSubscriptionManager::AddSubscription2(ocp1::Reader& req,
                                                    ocp1::Writer& rsp,
                                                    Session& sess) {
  ONo emitter = req.u32();
  EventID eid{req.u16(), req.u16()};
  OcaBlob ctx = req.blob();  // subscriberContext(未使用,仅消费)

  uint32_t id = next_id_.fetch_add(1);
  {
    std::lock_guard<std::mutex> lk(mutex_);
    entries_.push_back({id, &sess, emitter, eid});
  }
  sess.add_subscription({emitter, eid, std::move(ctx)});
  rsp.u32(id);             // 返回 subscriptionID
  return {Status::OK, 1};  // subscriptionID = 1 个参数
}

ExecResult OcaSubscriptionManager::RemoveSubscription2(ocp1::Reader& req,
                                                       ocp1::Writer& rsp,
                                                       Session& sess) {
  uint32_t id = req.u32();
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (it->id == id) {
      sess.remove_subscription(it->emitterONo, it->eventID);
      entries_.erase(it);
      return {Status::OK, 0};  // 无返回参数
    }
  }
  return {Status::BadONo, 0};  // 未知 subscriptionID
}

void OcaSubscriptionManager::trigger_event(ONo emitterONo,
                                           EventID eventID,
                                           const uint8_t* data,
                                           uint16_t dataCount) {
  std::vector<std::pair<Session*, Entry>> targets;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& e : entries_) {
      if (e.emitterONo == emitterONo &&
          e.eventID.defLevel == eventID.defLevel &&
          e.eventID.eventIndex == eventID.eventIndex) {
        targets.emplace_back(e.sess, e);
      }
    }
  }
  for (auto& [sp, e] : targets) {
    oca::ocp1::Writer nw;
    oca::ocp1::write_notification2(nw, emitterONo, eventID, 0 /*Event*/, data,
                                   dataCount);
    auto pdu =
        oca::ocp1::PduWriter::build_notification2_pdu(1, nw.data(), nw.size());
    sp->enqueue_notification(std::move(pdu));
  }
}

void OcaSubscriptionManager::remove_session(Session* sess) {
  std::lock_guard<std::mutex> lk(mutex_);
  entries_.erase(
      std::remove_if(entries_.begin(), entries_.end(),
                     [sess](const Entry& e) { return e.sess == sess; }),
      entries_.end());
}

}  // namespace oca

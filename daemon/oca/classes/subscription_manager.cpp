//  classes/subscription_manager.cpp

#include "oca/classes/subscription_manager.hpp"

#include <algorithm>

#include "oca/methods.hpp"
#include "oca/ocp1.hpp"

namespace oca {

namespace {
const ClassIdentification kSubscriptionManagerClassId = {{{1, 3, 4}}, 2};
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
        return {Status::NotImplemented,
                0};  // PropertyChange 变体/未实现方法 -> NotImplemented
    }
  }
  return OcaManager::exec(m, req, rsp, sess);
}

ExecResult OcaSubscriptionManager::AddSubscription2(ocp1::Reader& req,
                                                    ocp1::Writer& rsp,
                                                    Session& sess) {
  // sphinx 2024 §C.1: OcaEvent{EmitterONo + EventID}
  //                    + NotificationDeliveryMode + NetworkAddress
  ONo emitter = req.u32();
  EventID eid{req.u16(), req.u16()};
  (void)req.u8();    // NotificationDeliveryMode(Normal=1,同会话回送)
  (void)req.blob();  // NetworkAddress(OcaBlob 编码,仅消费)

  uint32_t id = next_id_.fetch_add(1);
  {
    std::lock_guard<std::mutex> lk(mutex_);
    entries_.push_back({id, &sess, emitter, eid});
  }
  sess.add_subscription({emitter, eid});  // EV2 无 subscriberContext
  rsp.u32(id);                            // 返回 subscriptionID
  return {Status::OK, 1};                 // subscriptionID = 1 个参数
}

ExecResult OcaSubscriptionManager::RemoveSubscription2(ocp1::Reader& req,
                                                       ocp1::Writer& rsp,
                                                       Session& sess) {
  // sphinx 2024 §C.1: OcaEvent + NotificationDeliveryMode + NetworkAddress
  // 语义 = 移除所有匹配 event 的订阅(非按 subscriptionID);幂等
  ONo emitter = req.u32();
  EventID eid{req.u16(), req.u16()};
  (void)req.u8();    // NotificationDeliveryMode(仅消费)
  (void)req.blob();  // NetworkAddress(仅消费)

  std::lock_guard<std::mutex> lk(mutex_);
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const Entry& e) {
                                  return e.sess == &sess &&
                                         e.emitterONo == emitter &&
                                         e.eventID.defLevel == eid.defLevel &&
                                         e.eventID.eventIndex == eid.eventIndex;
                                }),
                 entries_.end());
  sess.remove_subscription(emitter, eid);
  return {Status::OK, 0};  // 无返回参数(幂等:无匹配也 OK)
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

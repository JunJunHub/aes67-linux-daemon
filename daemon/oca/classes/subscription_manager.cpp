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
      case methods::kSubAddSubscription:
        return AddSubscription(req, rsp, sess);
      case methods::kSubRemoveSubscription:
        return RemoveSubscription(req, rsp, sess);
      case methods::kSubAddPropertyChangeSubscription:
        return AddPropertyChangeSubscription(req, rsp, sess);
      case methods::kSubRemovePropertyChangeSubscription:
        return RemovePropertyChangeSubscription(req, rsp, sess);
      case methods::kSubAddSubscription2:
        return AddSubscription2(req, rsp, sess);
      case methods::kSubRemoveSubscription2:
        return RemoveSubscription2(req, rsp, sess);
      case methods::kSubAddPropertyChangeSubscription2:
        return AddPropertyChangeSubscription2(req, rsp, sess);
      case methods::kSubRemovePropertyChangeSubscription2:
        return RemovePropertyChangeSubscription2(req, rsp, sess);
      default:
        return {Status::NotImplemented, 0};  // 未实现方法 -> NotImplemented
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

ExecResult OcaSubscriptionManager::AddSubscription(ocp1::Reader& req,
                                                   ocp1::Writer& rsp,
                                                   Session& sess) {
  // EV1 §3.1(OCAMicro,deprecated v3):
  //   OcaEvent{EmitterONo+EventID} + OcaMethod{ONo+MethodID}(subscriber,忽略)
  //   + OcaBlob(ctx,忽略) + NotificationDeliveryMode(u8,忽略)
  //   + NetworkAddress(blob,忽略)
  // 响应 0 参(无 subscriptionID,区别于 EV2 AddSubscription2 的 1 参)
  ONo emitter = req.u32();
  EventID eid{req.u16(), req.u16()};
  (void)req.u32();   // subscriber ONo(忽略)
  (void)req.u16();   // subscriber MethodID defLevel(忽略)
  (void)req.u16();   // subscriber MethodID methodIndex(忽略)
  (void)req.blob();  // context(忽略)
  (void)req.u8();    // NotificationDeliveryMode(忽略)
  (void)req.blob();  // NetworkAddress(忽略)

  uint32_t id = next_id_.fetch_add(1);
  {
    std::lock_guard<std::mutex> lk(mutex_);
    entries_.push_back({id, &sess, emitter, eid});
  }
  sess.add_subscription({emitter, eid});
  return {Status::OK, 0};  // EV1:无返回参数
}

ExecResult OcaSubscriptionManager::RemoveSubscription(ocp1::Reader& req,
                                                      ocp1::Writer& rsp,
                                                      Session& sess) {
  // EV1 §3.2: OcaEvent{EmitterONo+EventID} + OcaMethod(subscriber,忽略)
  ONo emitter = req.u32();
  EventID eid{req.u16(), req.u16()};
  (void)req.u32();  // subscriber ONo(忽略)
  (void)req.u16();  // subscriber MethodID defLevel(忽略)
  (void)req.u16();  // subscriber MethodID methodIndex(忽略)

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
  return {Status::OK, 0};  // 幂等:无匹配也 OK
}

ExecResult OcaSubscriptionManager::AddPropertyChangeSubscription(
    ocp1::Reader& req,
    ocp1::Writer& rsp,
    Session& sess) {
  // EV1 §3.5: EmitterONo + PropertyID{u16,u16}(忽略) + OcaMethod(忽略)
  //   + OcaBlob(ctx,忽略) + NotificationDeliveryMode(u8,忽略)
  //   + NetworkAddress(blob,忽略)
  // 订阅 PropertyChanged 事件(OcaRoot event,defLevel 1 / eventIndex 1)
  ONo emitter = req.u32();
  (void)req.u16();   // PropertyID defLevel(忽略)
  (void)req.u16();   // PropertyID propertyIndex(忽略)
  (void)req.u32();   // subscriber ONo(忽略)
  (void)req.u16();   // subscriber MethodID defLevel(忽略)
  (void)req.u16();   // subscriber MethodID methodIndex(忽略)
  (void)req.blob();  // context(忽略)
  (void)req.u8();    // NotificationDeliveryMode(忽略)
  (void)req.blob();  // NetworkAddress(忽略)

  EventID pce{methods::kDefLevelRoot, methods::kEventPropertyChanged};
  uint32_t id = next_id_.fetch_add(1);
  {
    std::lock_guard<std::mutex> lk(mutex_);
    entries_.push_back({id, &sess, emitter, pce});
  }
  sess.add_subscription({emitter, pce});
  return {Status::OK, 0};
}

ExecResult OcaSubscriptionManager::RemovePropertyChangeSubscription(
    ocp1::Reader& req,
    ocp1::Writer& rsp,
    Session& sess) {
  // EV1 §3.6: EmitterONo + PropertyID(忽略) + OcaMethod(忽略)
  ONo emitter = req.u32();
  (void)req.u16();  // PropertyID defLevel(忽略)
  (void)req.u16();  // PropertyID propertyIndex(忽略)
  (void)req.u32();  // subscriber ONo(忽略)
  (void)req.u16();  // subscriber MethodID defLevel(忽略)
  (void)req.u16();  // subscriber MethodID methodIndex(忽略)

  EventID pce{methods::kDefLevelRoot, methods::kEventPropertyChanged};
  std::lock_guard<std::mutex> lk(mutex_);
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const Entry& e) {
                                  return e.sess == &sess &&
                                         e.emitterONo == emitter &&
                                         e.eventID.defLevel == pce.defLevel &&
                                         e.eventID.eventIndex == pce.eventIndex;
                                }),
                 entries_.end());
  sess.remove_subscription(emitter, pce);
  return {Status::OK, 0};
}

ExecResult OcaSubscriptionManager::AddPropertyChangeSubscription2(
    ocp1::Reader& req,
    ocp1::Writer& rsp,
    Session& sess) {
  // EV2 §3.10(sphinx 2024): EmitterONo + PropertyID(忽略)
  //   + NotificationDeliveryMode(u8,忽略) + NetworkAddress(blob,忽略)
  // 响应 0 参(sphinx:仅返回 OcaStatus,无 subscriptionID)
  ONo emitter = req.u32();
  (void)req.u16();   // PropertyID defLevel(忽略)
  (void)req.u16();   // PropertyID propertyIndex(忽略)
  (void)req.u8();    // NotificationDeliveryMode(忽略)
  (void)req.blob();  // NetworkAddress(忽略)

  EventID pce{methods::kDefLevelRoot, methods::kEventPropertyChanged};
  uint32_t id = next_id_.fetch_add(1);
  {
    std::lock_guard<std::mutex> lk(mutex_);
    entries_.push_back({id, &sess, emitter, pce});
  }
  sess.add_subscription({emitter, pce});
  return {Status::OK, 0};
}

ExecResult OcaSubscriptionManager::RemovePropertyChangeSubscription2(
    ocp1::Reader& req,
    ocp1::Writer& rsp,
    Session& sess) {
  // EV2 §3.11(sphinx 2024): EmitterONo + PropertyID(忽略)
  //   + NotificationDeliveryMode(u8,忽略) + NetworkAddress(blob,忽略)
  ONo emitter = req.u32();
  (void)req.u16();   // PropertyID defLevel(忽略)
  (void)req.u16();   // PropertyID propertyIndex(忽略)
  (void)req.u8();    // NotificationDeliveryMode(忽略)
  (void)req.blob();  // NetworkAddress(忽略)

  EventID pce{methods::kDefLevelRoot, methods::kEventPropertyChanged};
  std::lock_guard<std::mutex> lk(mutex_);
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const Entry& e) {
                                  return e.sess == &sess &&
                                         e.emitterONo == emitter &&
                                         e.eventID.defLevel == pce.defLevel &&
                                         e.eventID.eventIndex == pce.eventIndex;
                                }),
                 entries_.end());
  sess.remove_subscription(emitter, pce);
  return {Status::OK, 0};
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

//  classes/subscription_manager.hpp - EV2 订阅管理器

#ifndef OCA_CLASSES_SUBSCRIPTION_MANAGER_HPP_
#define OCA_CLASSES_SUBSCRIPTION_MANAGER_HPP_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "oca/classes/root.hpp"  // OcaManager
#include "oca/session.hpp"

namespace oca {

class OcaSubscriptionManager : public OcaManager {
 public:
  explicit OcaSubscriptionManager(ONo ono) : OcaManager(ono) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 2; }
  std::string role() const override { return "SubscriptionManager"; }
  Status exec(MethodID m,
              ocp1::Reader& req,
              ocp1::Writer& rsp,
              Session& sess) override;

  // 触发事件:遍历订阅者,编码 Notification2 PDU 投递到各会话写队列
  void trigger_event(ONo emitterONo,
                     EventID eventID,
                     const uint8_t* data,
                     uint16_t dataCount);
  // 连接断开时清理该会话的所有订阅
  void remove_session(Session* sess);

 private:
  Status AddSubscription2(ocp1::Reader& req, ocp1::Writer& rsp, Session& sess);
  Status RemoveSubscription2(ocp1::Reader& req,
                             ocp1::Writer& rsp,
                             Session& sess);

  struct Entry {
    uint32_t id;
    Session* sess;
    ONo emitterONo;
    EventID eventID;
  };
  std::mutex mutex_;
  std::vector<Entry> entries_;
  std::atomic<uint32_t> next_id_{1};
};

}  // namespace oca

#endif  // OCA_CLASSES_SUBSCRIPTION_MANAGER_HPP_

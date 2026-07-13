//  classes/network_manager.hpp - OcaNetworkManager

#ifndef OCA_CLASSES_NETWORK_MANAGER_HPP_
#define OCA_CLASSES_NETWORK_MANAGER_HPP_

#include "oca/classes/root.hpp"  // OcaManager

namespace oca {

class OcaAudioBridge;  // 前向声明

class OcaNetworkManager : public OcaManager {
 public:
  explicit OcaNetworkManager(ONo ono) : OcaManager(ono) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 2; }
  std::string role() const override { return "NetworkManager"; }
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;

  // Spec5:注入 bridge(ONo 列表无需 bridge,但预留扩展)
  void set_bridge(OcaAudioBridge* bridge) { bridge_ = bridge; }

 private:
  OcaAudioBridge* bridge_ = nullptr;
};

}  // namespace oca

#endif  // OCA_CLASSES_NETWORK_MANAGER_HPP_

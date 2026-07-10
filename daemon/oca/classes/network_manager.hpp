//  classes/network_manager.hpp - OcaNetworkManager

#ifndef OCA_CLASSES_NETWORK_MANAGER_HPP_
#define OCA_CLASSES_NETWORK_MANAGER_HPP_

#include "oca/classes/root.hpp"  // OcaManager

namespace oca {

class OcaNetworkManager : public OcaManager {
 public:
  explicit OcaNetworkManager(ONo ono) : OcaManager(ono) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 3; }
  std::string role() const override { return "NetworkManager"; }
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;
};

}  // namespace oca

#endif  // OCA_CLASSES_NETWORK_MANAGER_HPP_

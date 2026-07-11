//  classes/control_network.hpp - OcaControlNetwork {1,4,1} v1
//
//  AES70-2018 合规最小强制实例。AvailableSince AES70-2018;无 DeviceType 门。
//  唯一强制方法 GetControlProtocol(1)。DefLevel == classID.fieldCount == 3。

#ifndef OCA_CLASSES_CONTROL_NETWORK_HPP_
#define OCA_CLASSES_CONTROL_NETWORK_HPP_

#include "oca/classes/root.hpp"

namespace oca {

class OcaControlNetwork : public OcaRoot {
 public:
  explicit OcaControlNetwork(ONo ono) : OcaRoot(ono) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 1; }
  std::string role() const override { return "Control Network"; }
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;
};

}  // namespace oca

#endif  // OCA_CLASSES_CONTROL_NETWORK_HPP_
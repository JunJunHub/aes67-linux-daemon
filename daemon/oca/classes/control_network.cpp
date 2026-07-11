//  classes/control_network.cpp - OcaControlNetwork {1,4,1} v1 实现

#include "oca/classes/control_network.hpp"

#include "oca/methods.hpp"

namespace oca {

namespace {
// OcaControlNetwork {1,4,1} v1。AvailableSince AES70-2018;无 DeviceType 门。
const ClassIdentification kControlNetworkClassId = {{{1, 4, 1}}, 1};
}  // namespace

const ClassIdentification& OcaControlNetwork::class_id() const {
  return kControlNetworkClassId;
}

ExecResult OcaControlNetwork::exec(MethodID m,
                                   ocp1::Reader& req,
                                   ocp1::Writer& rsp,
                                   Session& sess) {
  if (m.defLevel == methods::kDefLevelBlock) {  // == classID.fieldCount == 3
    switch (m.methodIndex) {
      case methods::kCtrlNetGetControlProtocol:
        // OcaNetworkControlProtocol(u8):OCP.1=1
        rsp.u8(1);
        return {Status::OK, 1};
      case methods::kAppNetGetServiceID:
        // OcaControlNetwork{1,4,1} 前缀匹配 OcaApplicationNetwork{1,4},工具对
        // 本实例也测 GetServiceID(Mandatory=true)。返空 OcaString(码点计数 0)。
        rsp.string("");
        return {Status::OK, 1};
      case methods::kAppNetGetSystemInterfaces:
        // 同上,返空 Ocp1List<OcaNetworkSystemInterfaceID>(u16 count=0)。
        rsp.u16(0);
        return {Status::OK, 1};
      default:
        return {Status::NotImplemented, 0};
    }
  }
  return OcaRoot::exec(m, req, rsp, sess);  // DefLevel 1 -> OcaRoot
}

}  // namespace oca
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
  // OcaControlNetwork{1,4,1} 前缀匹配 OcaApplicationNetwork{1,4}(classID
  // fieldCount =2, defLevel=2)。工具按基类测时 methodID.defLevel=2,故 AppNet
  // 强制方法 (GetServiceID=4/GetSystemInterfaces=6)在 defLevel 2 分派。
  if (m.defLevel ==
      methods::kDefLevelManager) {  // == OcaApplicationNetwork defLevel 2
    switch (m.methodIndex) {
      case methods::kAppNetGetServiceID:
        rsp.string("");  // 空 OcaString
        return {Status::OK, 1};
      case methods::kAppNetGetSystemInterfaces:
        rsp.u16(0);  // 空 Ocp1List
        return {Status::OK, 1};
      default:
        return {Status::NotImplemented, 0};
    }
  }
  if (m.defLevel == methods::kDefLevelBlock) {  // == classID.fieldCount == 3
    switch (m.methodIndex) {
      case methods::kCtrlNetGetControlProtocol:
        // OcaNetworkControlProtocol(u8):OCP.1=1
        rsp.u8(1);
        return {Status::OK, 1};
      default:
        return {Status::NotImplemented, 0};
    }
  }
  return OcaRoot::exec(m, req, rsp, sess);  // DefLevel 1 -> OcaRoot
}

}  // namespace oca
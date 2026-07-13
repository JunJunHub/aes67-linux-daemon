//  classes/control_network.cpp - OcaControlNetwork {1,4,1} v1 实现
//  继承链: OcaControlNetwork -> OcaApplicationNetwork -> OcaRoot

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
  // OcaControlNetwork{1,4,1} defLevel=3 自身方法。
  // OcaApplicationNetwork{1,4} defLevel=2 方法由父类
  // OcaApplicationNetwork::exec
  // 自动处理(GetLabel/SetLabel/GetOwner/GetServiceID/GetSystemInterfaces/GetPath)。
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
  return OcaApplicationNetwork::exec(m, req, rsp, sess);
}

}  // namespace oca
//  classes/network.cpp - OcaNetwork {1,2,1} v1 实现

#include "oca/classes/network.hpp"

#include "oca/methods.hpp"

namespace oca {

namespace {
// OcaNetwork {1,2,1} v1。DeprecatedSince AES70-2018 / 2023 弃用;
// 本实例仅为 AES70-2018 合规工具的最小强制实例(见头注释与 Spec3 计划)。
const ClassIdentification kNetworkClassId = {{{1, 2, 1}}, 1};
}  // namespace

const ClassIdentification& OcaNetwork::class_id() const {
  return kNetworkClassId;
}

ExecResult OcaNetwork::exec(MethodID m,
                            ocp1::Reader& req,
                            ocp1::Writer& rsp,
                            Session& sess) {
  if (m.defLevel == methods::kDefLevelBlock) {  // == classID.fieldCount == 3
    switch (m.methodIndex) {
      case methods::kNet2GetLinkType:
        // OcaNetworkLinkType(u8):EthernetWired=1
        rsp.u8(1);
        return {Status::OK, 1};
      case methods::kNet2GetIDAdvertised:
        // OcaNetworkNodeID = OcaBlob:空 blob(u16 len=0)
        rsp.blob(nullptr, 0);
        return {Status::OK, 1};
      case methods::kNet2GetControlProtocol:
        // OcaNetworkControlProtocol(u8):OCP.1=1
        rsp.u8(1);
        return {Status::OK, 1};
      case methods::kNet2GetMediaProtocol:
        // OcaNetworkMediaProtocol(u8):None=0
        rsp.u8(0);
        return {Status::OK, 1};
      case methods::kNet2GetSystemInterfaces:
        // Ocp1List<OcaNetworkSystemInterfaceID>:空列表(u16 count=0)
        rsp.u16(0);
        return {Status::OK, 1};
      default:
        return {Status::NotImplemented, 0};
    }
  }
  return OcaRoot::exec(m, req, rsp, sess);  // DefLevel 1 -> OcaRoot
}

}  // namespace oca
//  classes/network.cpp - OcaNetwork {1,2,1} v1 实现

#include "oca/classes/network.hpp"

#include "oca/methods.hpp"
#include "oca/oca_audio_bridge.hpp"

namespace oca {

namespace {
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
      case methods::kNet2GetIDAdvertised: {
        // OcaNetworkNodeID = OcaBlob。返回 node_id(设备唯一标识),
        // 非 ip_addr(IP 由 GetSystemInterfaces 提供)。
        if (bridge_) {
          std::string node_id = bridge_->get_device_id();
          rsp.blob(reinterpret_cast<const uint8_t*>(node_id.data()),
                   node_id.size());
        } else {
          rsp.blob(nullptr, 0);
        }
        return {Status::OK, 1};
      }
      case methods::kNet2GetControlProtocol:
        // OcaNetworkControlProtocol(u8):OCP.1=1
        rsp.u8(1);
        return {Status::OK, 1};
      case methods::kNet2GetMediaProtocol:
        // Spec5:返回 AES67(3) 而非 None(0)
        rsp.u8(3);  // AES67
        return {Status::OK, 1};
      case methods::kNet2GetSystemInterfaces: {
        // Spec5:bridge 非空时返回真实接口信息
        // Ocp1List<OcaNetworkSystemInterfaceDescriptor>
        if (bridge_) {
          rsp.u16(1);  // 1 个接口
          // OcaNetworkSystemInterfaceDescriptor:
          // string interfaceName + string ipAddress + string macAddress
          rsp.string(bridge_->get_interface_name());
          rsp.string(bridge_->get_ip_addr());
          rsp.string(bridge_->get_mac_addr());
        } else {
          rsp.u16(0);
        }
        return {Status::OK, 1};
      }
      case methods::kNet2Shutdown:
        return {Status::OK, 0};
      default:
        return {Status::NotImplemented, 0};
    }
  }
  return OcaAgent::exec(m, req, rsp, sess);  // DefLevel 1/2 -> OcaAgent
}

}  // namespace oca

//  classes/network_manager.cpp

#include "oca/classes/network_manager.hpp"

#include "oca/methods.hpp"

namespace oca {

namespace {
const ClassIdentification kNetworkManagerClassId = {{{1, 3, 6}}, 2};
}  // namespace

const ClassIdentification& OcaNetworkManager::class_id() const {
  return kNetworkManagerClassId;
}

ExecResult OcaNetworkManager::exec(MethodID m,
                                   ocp1::Reader& req,
                                   ocp1::Writer& rsp,
                                   Session& sess) {
  if (m.defLevel == methods::kDefLevelNetworkMngr) {
    switch (m.methodIndex) {
      case methods::kNetGetNetworks:
        // Spec5:返回 [4097, 4098, 8192]
        rsp.u16(3);
        rsp.u32(4097);  // OcaNetwork
        rsp.u32(4098);  // OcaControlNetwork
        rsp.u32(8192);  // OcaMediaTransportNetworkAES67
        return {Status::OK, 1};
      case methods::kNetGetStreamNetworks:
        // Spec5:返回 [8192]
        rsp.u16(1);
        rsp.u32(8192);
        return {Status::OK, 1};
      case methods::kNetGetControlNetworks:
        // Spec5:返回 [4098]
        rsp.u16(1);
        rsp.u32(4098);
        return {Status::OK, 1};
      case methods::kNetGetMediaTransportNetworks:
        // Spec5:返回 [8192]
        rsp.u16(1);
        rsp.u32(8192);
        return {Status::OK, 1};
      default:
        return {Status::NotImplemented, 0};
    }
  }
  return OcaManager::exec(m, req, rsp, sess);  // DefLevel 1 -> OcaRoot
}

}  // namespace oca

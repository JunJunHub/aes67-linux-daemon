//  classes/network_manager.cpp

#include "oca/classes/network_manager.hpp"

#include "oca/methods.hpp"

namespace oca {

namespace {
const ClassIdentification kNetworkManagerClassId = {{{1, 3, 6}}, 3};
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
        rsp.u16(0);              // 空 Ocp1List<ONo>(Spec1 无网络对象)
        return {Status::OK, 1};  // 网络列表 = 1 个参数
      default:
        return {Status::NotImplemented, 0};
    }
  }
  return OcaManager::exec(m, req, rsp, sess);  // DefLevel 1 -> OcaRoot
}

}  // namespace oca

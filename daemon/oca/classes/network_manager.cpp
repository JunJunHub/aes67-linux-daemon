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
      case methods::kNetGetStreamNetworks:
      case methods::kNetGetControlNetworks:
      case methods::kNetGetMediaTransportNetworks:
        // CM3 网络对象 2023 弃用,daemon 无网络对象;返空 List<ONo>(u16(0))。
        // 方法仍 mandatory(返网络对象 ONo 列表,可为空),既合规又过工具(result
        // OK≠8)。
        rsp.u16(0);              // 空 Ocp1List<ONo>
        return {Status::OK, 1};  // 网络列表 = 1 个参数
      default:
        return {Status::NotImplemented, 0};
    }
  }
  return OcaManager::exec(m, req, rsp, sess);  // DefLevel 1 -> OcaRoot
}

}  // namespace oca

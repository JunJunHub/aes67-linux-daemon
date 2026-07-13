//  classes/application_network.cpp - OcaApplicationNetwork {1,4} v1 实现

#include "oca/classes/application_network.hpp"

#include "oca/methods.hpp"

namespace oca {

ExecResult OcaApplicationNetwork::exec(MethodID m,
                                       ocp1::Reader& req,
                                       ocp1::Writer& rsp,
                                       Session& sess) {
  if (m.defLevel == methods::kDefLevelManager) {  // == classID.fieldCount == 2
    return handle_appnet(m.methodIndex, req, rsp);
  }
  return OcaRoot::exec(m, req, rsp, sess);  // 委托 DefLevel 1 -> OcaRoot
}

ExecResult OcaApplicationNetwork::handle_appnet(uint16_t idx,
                                                ocp1::Reader& req,
                                                ocp1::Writer& rsp) {
  switch (idx) {
    case methods::kAppNetGetLabel:
      // OcaString:返回 role() 作为标签
      rsp.string(role());
      return {Status::OK, 1};
    case methods::kAppNetSetLabel:
      // 读可选 OcaString,no-op。
      if (req.remaining() >= 2)
        (void)req.string();
      return {Status::OK, 0};
    case methods::kAppNetGetOwner:
      // ONo:含有块的对象号。
      rsp.u32(owner_ono_);
      return {Status::OK, 1};
    case methods::kAppNetGetServiceID:
      // OcaApplicationNetworkServiceID = OcaString:空字符串
      rsp.string("");
      return {Status::OK, 1};
    case methods::kAppNetGetSystemInterfaces:
      // Ocp1List<OcaNetworkSystemInterfaceDescriptor>:空列表(u16 count=0)
      rsp.u16(0);
      return {Status::OK, 1};
    case methods::kAppNetGetPath:
      // OcaPath 结构复杂,YAGNI。
      return {Status::NotImplemented, 0};
    default:
      return {Status::NotImplemented, 0};
  }
}

}  // namespace oca

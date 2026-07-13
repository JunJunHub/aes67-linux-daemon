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
      // Spec4:已 SetLabel 则返 label_,否则回退 role()
      rsp.string(label_.empty() ? role() : label_);
      return {Status::OK, 1};
    case methods::kAppNetSetLabel: {
      // Spec4:真存储 label_ + 触发 PropertyChanged。空体探测时 no-op。
      if (req.remaining() < 2)
        return {Status::OK, 0};
      std::string v = req.string();
      label_ = v;
      oca::ocp1::Writer vw;
      vw.string(v);
      emit_property_changed(methods::kDefLevelManager /*AppNet 引入级*/,
                            methods::kPropLabel, vw.data(),
                            static_cast<uint16_t>(vw.size()));
      return {Status::OK, 0};
    }
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

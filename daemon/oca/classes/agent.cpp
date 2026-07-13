//  classes/agent.cpp - OcaAgent {1,2} v2 实现

#include "oca/classes/agent.hpp"

#include "oca/methods.hpp"

namespace oca {

ExecResult OcaAgent::exec(MethodID m,
                          ocp1::Reader& req,
                          ocp1::Writer& rsp,
                          Session& sess) {
  if (m.defLevel == methods::kDefLevelManager) {  // == classID.fieldCount == 2
    return handle_agent(m.methodIndex, req, rsp);
  }
  return OcaRoot::exec(m, req, rsp, sess);  // 委托 DefLevel 1 -> OcaRoot
}

ExecResult OcaAgent::handle_agent(uint16_t idx,
                                  ocp1::Reader& req,
                                  ocp1::Writer& rsp) {
  switch (idx) {
    case methods::kAgentGetLabel:
      // Spec4:已 SetLabel 则返 label_,否则回退 role()
      rsp.string(label_.empty() ? role() : label_);
      return {Status::OK, 1};
    case methods::kAgentSetLabel: {
      // Spec4:真存储 label_ + 触发 PropertyChanged。空体探测(无完整
      // OcaString)时 no-op 返回 OK,不破坏 Spec1 回归。
      if (req.remaining() < 2)  // Ocp1List 最少长度 2 字节
        return {Status::OK, 0};
      std::string v = req.string();
      label_ = v;
      oca::ocp1::Writer vw;
      vw.string(v);
      emit_property_changed(methods::kDefLevelManager /*Agent 引入级*/,
                            methods::kPropLabel, vw.data(),
                            static_cast<uint16_t>(vw.size()));
      return {Status::OK, 0};
    }
    case methods::kAgentGetOwner:
      // ONo:含有块的对象号。Network/CtrlNet=100(根块),根块自身=0。
      rsp.u32(owner_ono_);
      return {Status::OK, 1};
    case methods::kAgentGetPath:
      // OcaPath 结构复杂(OcaPathStep 列表),YAGNI。
      return {Status::NotImplemented, 0};
    default:
      return {Status::NotImplemented, 0};
  }
}

}  // namespace oca

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
      // OcaString:返回 role() 作为标签
      rsp.string(role());
      return {Status::OK, 1};
    case methods::kAgentSetLabel:
      // 读可选 OcaString(remaining>=2 时跳过),no-op。
      if (req.remaining() >= 2)
        (void)req.string();
      return {Status::OK, 0};
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

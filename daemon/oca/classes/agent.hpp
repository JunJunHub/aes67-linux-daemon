//  classes/agent.hpp - OcaAgent {1,2} v2
//
//  AES70 抽象基类,定义 Agent 语义(Label/Owner/Path)。
//  OcaNetwork{1,2,1} 继承 OcaAgent{1,2}。DefLevel == classID.fieldCount == 2。
//  本类方法(defLevel 2)在本类处理;OcaRoot 通用方法(defLevel 1)经
//  exec 委托链回 OcaRoot::handle_root。

#ifndef OCA_CLASSES_AGENT_HPP_
#define OCA_CLASSES_AGENT_HPP_

#include "oca/classes/root.hpp"

namespace oca {

// OcaAgent {1,2} v2:Agent 抽象基类
// OcaNetwork{1,2,1} 继承此类。OcaAgent 本身不直接实例化(与 OcaWorker 同模式)。
class OcaAgent : public OcaRoot {
 public:
  explicit OcaAgent(ONo ono, ONo owner_ono = 0)
      : OcaRoot(ono), owner_ono_(owner_ono) {}
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;

 protected:
  // OcaAgent DefLevel-2 方法:GetLabel/SetLabel/GetOwner/GetPath。
  ExecResult handle_agent(uint16_t methodIndex,
                          ocp1::Reader& req,
                          ocp1::Writer& rsp);
  ONo owner_ono_;
};

}  // namespace oca

#endif  // OCA_CLASSES_AGENT_HPP_

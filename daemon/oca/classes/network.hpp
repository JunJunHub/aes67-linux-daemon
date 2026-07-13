//  classes/network.hpp - OcaNetwork {1,2,1} v1
//
//  AES70-2018 合规最小强制实例。DeprecatedSince AES70-2018;2023 立场:
//  本类已弃用,本实例仅为兼容 AES70-2018 合规工具(Aes70CompliancyTestTool)
//  的最小强制实例,不回上游(fork 专有)。详见 Spec3 计划与设计文档。
//
//  Spec5:bridge 注入后 GetSystemInterfaces/GetMediaProtocol/GetIDAdvertised
//  返回真实数据。
//
//  DefLevel == ClassID.fieldCount == 3。本类方法(defLevel 3)在本类处理;
//  OcaRoot 通用方法(GetClassIdentification/GetRole 等,defLevel 1)经
//  exec 委托链回 OcaRoot::handle_root。

#ifndef OCA_CLASSES_NETWORK_HPP_
#define OCA_CLASSES_NETWORK_HPP_

#include "oca/classes/agent.hpp"

namespace oca {

class OcaAudioBridge;  // 前向声明

// OcaNetwork {1,2,1} v1:DeprecatedSince AES70-2018 / 2023 弃用
class OcaNetwork : public OcaAgent {
 public:
  explicit OcaNetwork(ONo ono, ONo owner_ono = 0) : OcaAgent(ono, owner_ono) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 1; }
  std::string role() const override { return "Network"; }
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;

  // Spec5:注入 bridge 以读取真实网络数据
  void set_bridge(OcaAudioBridge* bridge) { bridge_ = bridge; }

 private:
  OcaAudioBridge* bridge_ = nullptr;
};

}  // namespace oca

#endif  // OCA_CLASSES_NETWORK_HPP_

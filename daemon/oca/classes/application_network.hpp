//  classes/application_network.hpp - OcaApplicationNetwork {1,4} v1
//
//  AES70-2018 应用网络抽象基类。OcaControlNetwork{1,4,1} 继承此类。
//  DefLevel == classID.fieldCount == 2。本类方法(defLevel 2)在本类处理;
//  OcaRoot 通用方法(defLevel 1)经 exec 委托链回 OcaRoot::handle_root。

#ifndef OCA_CLASSES_APPLICATION_NETWORK_HPP_
#define OCA_CLASSES_APPLICATION_NETWORK_HPP_

#include "oca/classes/root.hpp"

namespace oca {

// OcaApplicationNetwork {1,4} v1:应用网络抽象基类
// AvailableSince AES70-2018。OcaControlNetwork{1,4,1} 继承此类。
// GetServiceID/GetSystemInterfaces 从 OcaControlNetwork 内联分派移入此类。
class OcaApplicationNetwork : public OcaRoot {
 public:
  explicit OcaApplicationNetwork(ONo ono, ONo owner_ono = 0)
      : OcaRoot(ono), owner_ono_(owner_ono) {}
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;

 protected:
  // OcaApplicationNetwork DefLevel-2 方法。
  ExecResult handle_appnet(uint16_t methodIndex,
                           ocp1::Reader& req,
                           ocp1::Writer& rsp);
  ONo owner_ono_;
  std::string label_;  // Spec4:Label 真存储(空则 GetLabel 回退 role())
};

}  // namespace oca

#endif  // OCA_CLASSES_APPLICATION_NETWORK_HPP_

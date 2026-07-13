//  classes/media_transport_network_aes67.hpp
//  - OcaMediaTransportNetworkAES67 {1,4,2,0xFFFF,0xFA,0x2EE9,1} v1
//
//  AES67 媒体传输网络子类。ONo=8192。
//  ClassID 7 字段,defLevel=7。Fitcan 控制器按此 ClassID 搜索。
//  覆盖基类纯虚方法:协议=AES67,容量=64,CRUD 调 bridge。
//  AES67 defLevel-7 方法全部 NOT_IMPLEMENTED。
//  Fitcan 私有:DeleteAllConnectors(1000),UpdateRouteTableCommand(0x8000)。

#ifndef OCA_CLASSES_MEDIA_TRANSPORT_NETWORK_AES67_HPP_
#define OCA_CLASSES_MEDIA_TRANSPORT_NETWORK_AES67_HPP_

#include "oca/classes/media_transport_network.hpp"

namespace oca {

class OcaMediaTransportNetworkAES67 : public OcaMediaTransportNetwork {
 public:
  explicit OcaMediaTransportNetworkAES67(ONo ono, ONo owner_ono = 0)
      : OcaMediaTransportNetwork(ono, owner_ono) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 1; }
  std::string role() const override { return "MediaTransportNetworkAES67"; }
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;

 protected:
  // 基类纯虚实现
  uint8_t get_media_protocol() const override {
    return methods::kMediaProtocolAes67;
  }
  uint16_t get_max_source_connectors() const override { return 64; }
  uint16_t get_max_sink_connectors() const override { return 64; }
  uint16_t get_max_pins_per_connector() const override { return 64; }
  uint16_t get_max_ports_per_pin() const override { return 1; }

  ExecResult add_source_connector_impl(ocp1::Reader& req,
                                       ocp1::Writer& rsp) override;
  ExecResult add_sink_connector_impl(ocp1::Reader& req,
                                     ocp1::Writer& rsp) override;
  ExecResult delete_connector_impl(uint32_t connector_id,
                                   ocp1::Writer& rsp) override;

 private:
  // AES67 defLevel-7 方法
  ExecResult handle_mtn_aes67(uint16_t methodIndex,
                              ocp1::Reader& req,
                              ocp1::Writer& rsp);

  // source/sink ID 分配(0..63)
  uint8_t next_source_id_ = 0;
  uint8_t next_sink_id_ = 0;
};

}  // namespace oca

#endif  // OCA_CLASSES_MEDIA_TRANSPORT_NETWORK_AES67_HPP_

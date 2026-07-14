//  classes/media_transport_network.hpp - OcaMediaTransportNetwork {1,4,2} v1
//
//  AES70-2018 媒体传输网络抽象基类。OcaMediaTransportNetworkAES67 继承此类。
//  DefLevel == classID.fieldCount == 3。本类方法(defLevel 3)在本类处理;
//  OcaApplicationNetwork 方法(defLevel 2)经 exec 委托链回 AppNet。

#ifndef OCA_CLASSES_MEDIA_TRANSPORT_NETWORK_HPP_
#define OCA_CLASSES_MEDIA_TRANSPORT_NETWORK_HPP_

#include <map>
#include <vector>

#include "oca/oca_audio_bridge.hpp"
#include "oca/classes/application_network.hpp"

namespace oca {

// OcaMediaTransportNetwork {1,4,2} v1
// connector 内部存储:Source/Sink 各按 connectorID 索引。
// 两套 ID 命名空间分离:connector_id(OCA 线缆侧,u16)与 daemon_id
// (daemon 流 id,0..63,经 bridge CRUD)。避免删除/查状态时映射错位。
struct SourceConnector {
  uint16_t connector_id;  // OcaMediaConnectorID = u16(OCAMicro)
  uint8_t daemon_id = 0;  // daemon Source id(0..63),传 bridge
  std::string name;
  std::string codec;         // "L16","L24","AM824"
  std::vector<uint8_t> map;  // pin → port
  std::string dest_addr;     // 组播/单播 IP
  uint16_t dest_port = 0;
  uint8_t ttl = 0;
  uint8_t payload_type = 0;
  uint32_t ptime = 0;  // samples per packet
  uint32_t session_id = 0;
  uint32_t session_version = 0;
  bool enabled = false;
};

struct SinkConnector {
  uint16_t connector_id;  // OcaMediaConnectorID = u16(OCAMicro)
  uint8_t daemon_id = 0;  // daemon Sink id(0..63),传 bridge
  std::string name;
  std::string codec;
  std::vector<uint8_t> map;
  uint32_t delay = 0;
  std::string source_url;
  std::string sdp;
  bool use_sdp = false;
  std::string dest_addr;
  uint16_t dest_port = 0;
  uint8_t ttl = 0;
  uint8_t payload_type = 0;
  uint32_t ptime = 0;
  uint32_t session_id = 0;
  uint32_t session_version = 0;
};

class OcaMediaTransportNetwork : public OcaApplicationNetwork {
 public:
  explicit OcaMediaTransportNetwork(ONo ono, ONo owner_ono = 0)
      : OcaApplicationNetwork(ono, owner_ono) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 1; }
  std::string role() const override { return "MediaTransportNetwork"; }
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;

  void set_bridge(OcaAudioBridge* bridge) { bridge_ = bridge; }

 protected:
  // 纯虚:子类提供协议/容量
  virtual uint8_t get_media_protocol() const = 0;
  virtual uint16_t get_max_source_connectors() const = 0;
  virtual uint16_t get_max_sink_connectors() const = 0;
  virtual uint16_t get_max_pins_per_connector() const = 0;
  virtual uint16_t get_max_ports_per_pin() const = 0;
  // 纯虚:子类实现 connector CRUD(调 bridge)
  virtual ExecResult add_source_connector_impl(ocp1::Reader& req,
                                               ocp1::Writer& rsp) = 0;
  virtual ExecResult add_sink_connector_impl(ocp1::Reader& req,
                                             ocp1::Writer& rsp) = 0;
  virtual ExecResult delete_connector_impl(uint16_t connector_id,
                                           ocp1::Writer& rsp) = 0;

  // DefLevel 3 方法分派
  ExecResult handle_mtn(uint16_t methodIndex,
                        ocp1::Reader& req,
                        ocp1::Writer& rsp);

  // connector 序列化(供子类和本类使用)
  void write_source_connector(const SourceConnector& sc, ocp1::Writer& rsp);
  void write_sink_connector(const SinkConnector& sc, ocp1::Writer& rsp);
  void write_connector_status(uint16_t connector_id,
                              uint8_t state,
                              ocp1::Writer& rsp);

  // connector 列表(子类 CRUD 时操作),按 OcaMediaConnectorID(u16)索引
  std::map<uint16_t, SourceConnector> sources_;
  std::map<uint16_t, SinkConnector> sinks_;
  OcaAudioBridge* bridge_ = nullptr;
};

}  // namespace oca

#endif  // OCA_CLASSES_MEDIA_TRANSPORT_NETWORK_HPP_

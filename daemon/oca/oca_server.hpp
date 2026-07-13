//  oca_server.hpp - OCA 门面

#ifndef OCA_OCA_SERVER_HPP_
#define OCA_OCA_SERVER_HPP_

#include <cstdint>
#include <memory>
#include <string>

#include "oca/object.hpp"

namespace oca {

class Transport;
class OcaSubscriptionManager;
class OcaAudioBridge;
#ifdef _USE_AVAHI_
class MdnsPublisher;
#endif

// 由 main.cpp 从 Config 填充(解耦 OcaServer 与 Config)
struct OcaServerConfig {
  uint16_t port = 65037;
  std::string device_name;  // 空 -> node_id
  std::string manufacturer = "AES67-Linux-Daemon";
  std::string model;          // 空 -> daemon_version
  std::string serial_number;  // 空 -> node_id
  std::string node_id;        // device_name/serial 的回退
  std::string daemon_version;
  bool mdns_enabled = false;
  // Spec5:mDNS TXT + OcaNetwork 现实化
  std::string ip_addr;
  std::string ip_addr_sec;
  std::string mac_addr;
  uint32_t channels = 0;
};

class OcaServer {
 public:
  explicit OcaServer(const OcaServerConfig& cfg,
                     OcaAudioBridge* bridge = nullptr);
  ~OcaServer();
  bool start();  // 在 cfg.port 监听(0=自动)
  void stop();
  uint16_t port() const;
  OcaSubscriptionManager* subscription_manager();

 private:
  OcaServerConfig cfg_;
  OcaAudioBridge* bridge_;
  ObjectRegistry registry_;
  OcaSubscriptionManager* sub_mgr_ = nullptr;  // 归 registry_ 所有
  std::unique_ptr<Transport> transport_;
#ifdef _USE_AVAHI_
  std::unique_ptr<MdnsPublisher> mdns_;
#endif
};

}  // namespace oca

#endif  // OCA_OCA_SERVER_HPP_

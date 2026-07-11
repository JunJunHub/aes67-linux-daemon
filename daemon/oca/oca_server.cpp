//  oca_server.cpp

#include "oca/oca_server.hpp"

#include "oca/classes/control_network.hpp"
#include "oca/classes/device_manager.hpp"
#include "oca/classes/network.hpp"
#include "oca/classes/network_manager.hpp"
#include "oca/classes/root.hpp"
#include "oca/classes/subscription_manager.hpp"
#include "oca/transport.hpp"

#ifdef _USE_AVAHI_
#include "oca/mdns_publisher.hpp"
#endif

namespace oca {

OcaServer::OcaServer(const OcaServerConfig& cfg) : cfg_(cfg) {
  OcaDeviceIdentity id;
  id.manufacturer =
      cfg_.manufacturer.empty() ? "AES67-Linux-Daemon" : cfg_.manufacturer;
  id.model_name = cfg_.model.empty() ? cfg_.daemon_version : cfg_.model;
  id.model_version = cfg_.daemon_version;
  id.serial_number =
      cfg_.serial_number.empty() ? cfg_.node_id : cfg_.serial_number;
  id.device_name = cfg_.device_name.empty() ? cfg_.node_id : cfg_.device_name;

  auto* dm = new OcaDeviceManager(1, id);
  auto* nm = new OcaNetworkManager(6);
  auto* sm = new OcaSubscriptionManager(4);
  auto* root = new OcaBlock(100);
  // CM3 网络对象(Spec3):为 AES70-2018 合规工具最小强制实例。
  // OcaNetwork{1,2,1} DeprecatedSince 2018 / 2023 弃用;OcaControlNetwork{1,4.1}
  // AvailableSince 2018。ONo >=4096(BlockMember 区段),加入根块 GetMembers/
  // GetMembersRecursive 成员列表。详见 Spec3 计划与设计文档(2023 弃用立场)。
  auto* net2 = new OcaNetwork(4097);
  auto* ctrl_net = new OcaControlNetwork(4098);
  sub_mgr_ = sm;
  registry_.register_object(std::unique_ptr<Object>(dm));
  registry_.register_object(std::unique_ptr<Object>(nm));
  registry_.register_object(std::unique_ptr<Object>(sm));
  registry_.register_object(std::unique_ptr<Object>(root));
  registry_.register_object(std::unique_ptr<Object>(net2));
  registry_.register_object(std::unique_ptr<Object>(ctrl_net));

  transport_ = std::make_unique<Transport>(&registry_, sub_mgr_);
}

OcaServer::~OcaServer() {
  stop();
}

bool OcaServer::start() {
  if (!transport_ || !transport_->start(cfg_.port))
    return false;
#ifdef _USE_AVAHI_
  if (cfg_.mdns_enabled) {
    mdns_ = std::make_unique<MdnsPublisher>(
        cfg_.device_name.empty() ? cfg_.node_id : cfg_.device_name,
        transport_->port());
    mdns_->start();
  }
#endif
  return true;
}

void OcaServer::stop() {
#ifdef _USE_AVAHI_
  if (mdns_)
    mdns_->stop();
  mdns_.reset();
#endif
  if (transport_)
    transport_->stop();
}

uint16_t OcaServer::port() const {
  return transport_ ? transport_->port() : 0;
}

OcaSubscriptionManager* OcaServer::subscription_manager() {
  return sub_mgr_;
}

}  // namespace oca

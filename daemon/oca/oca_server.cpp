//  oca_server.cpp

#include "oca/oca_server.hpp"

#include "oca/classes/device_manager.hpp"
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
  auto* nm = new OcaNetworkManager(2);
  auto* sm = new OcaSubscriptionManager(4);
  auto* root = new OcaBlock(100);
  sub_mgr_ = sm;
  registry_.register_object(std::unique_ptr<Object>(dm));
  registry_.register_object(std::unique_ptr<Object>(nm));
  registry_.register_object(std::unique_ptr<Object>(sm));
  registry_.register_object(std::unique_ptr<Object>(root));

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

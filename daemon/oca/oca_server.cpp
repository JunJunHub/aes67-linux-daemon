//  oca_server.cpp

#include "oca/oca_server.hpp"

#include "oca/classes/control_network.hpp"
#include "oca/classes/device_manager.hpp"
#include "oca/classes/media_clock3.hpp"
#include "oca/classes/media_clock_manager.hpp"
#include "oca/classes/media_transport_network_aes67.hpp"
#include "oca/classes/network.hpp"
#include "oca/classes/network_manager.hpp"
#include "oca/classes/root.hpp"
#include "oca/classes/subscription_manager.hpp"
#include "oca/oca_audio_bridge.hpp"
#include "oca/transport.hpp"

#ifdef _USE_AVAHI_
#include "oca/mdns_publisher.hpp"
#endif

namespace oca {

OcaServer::OcaServer(const OcaServerConfig& cfg, OcaAudioBridge* bridge)
    : cfg_(cfg), bridge_(bridge) {
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
  auto* net2 = new OcaNetwork(4097, 100);
  auto* ctrl_net = new OcaControlNetwork(4098, 100);
  sub_mgr_ = sm;
  registry_.register_object(std::unique_ptr<Object>(dm));
  registry_.register_object(std::unique_ptr<Object>(nm));
  registry_.register_object(std::unique_ptr<Object>(sm));
  registry_.register_object(std::unique_ptr<Object>(root));
  registry_.register_object(std::unique_ptr<Object>(net2));
  registry_.register_object(std::unique_ptr<Object>(ctrl_net));

  // Spec5:媒体类对象
  auto* mcm = new OcaMediaClockManager(7);
  auto* mtn =
      new OcaMediaTransportNetworkAES67(8192, 100);  // owner = Root Block
  auto* mc3 = new OcaMediaClock3(8193, 100);
  registry_.register_object(std::unique_ptr<Object>(mcm));
  registry_.register_object(std::unique_ptr<Object>(mtn));
  registry_.register_object(std::unique_ptr<Object>(mc3));

  // 注入 bridge 到需要运行时数据的对象
  if (bridge_) {
    mtn->set_bridge(bridge_);
    mc3->set_bridge(bridge_);
    net2->set_bridge(bridge_);  // Spec7:OcaNetwork 现实化
    nm->set_bridge(bridge_);    // Spec7:OcaNetworkManager 真实列表

    // 注册 bridge observer:PTP 状态变化 → OcaMediaClock3 自发射
    bridge_->set_ptp_observer([mc3](const OcaAudioBridge::PtpStatus& st) {
      mc3->on_ptp_status_changed(st);
    });
  }

  // Spec4:为所有已注册对象注入事件总线
  for (auto* obj : registry_.objects_in_range(1, 9999))
    static_cast<OcaRoot*>(obj)->set_event_emitter(sub_mgr_);

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
    MdnsTxtRecords txt;
    txt.ip_addr = cfg_.ip_addr;
    txt.ip_addr_sec = cfg_.ip_addr_sec;
    txt.mac_addr = cfg_.mac_addr;
    txt.mac_addr_sec = cfg_.mac_addr_sec;
    txt.firmware = cfg_.daemon_version;
    mdns_ = std::make_unique<MdnsPublisher>(
        cfg_.device_name.empty() ? cfg_.node_id : cfg_.device_name,
        transport_->port(), txt);
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

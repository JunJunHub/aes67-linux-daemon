//  mdns_publisher.hpp - Avahi _oca._tcp 发布器(仅 WITH_AVAHI)

#ifndef OCA_MDNS_PUBLISHER_HPP_
#define OCA_MDNS_PUBLISHER_HPP_

#ifdef _USE_AVAHI_

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/thread-watch.h>

#include <cstdint>
#include <string>

namespace oca {

// Spec5:mDNS TXT 记录扩展。key 名为 Fitcan 控制器私有约定(见 .cpp),
// 非 AES70 标准风格。
struct MdnsTxtRecords {
  std::string ip_addr;
  std::string ip_addr_sec;
  std::string mac_addr;
  std::string firmware;
};

class MdnsPublisher {
 public:
  MdnsPublisher(std::string name, uint16_t port, MdnsTxtRecords txt = {});
  ~MdnsPublisher();
  bool start();
  void stop();

 private:
  static void client_cb(struct AvahiClient* c,
                        AvahiClientState state,
                        void* userdata);
  static void group_cb(struct AvahiEntryGroup* g,
                       AvahiEntryGroupState state,
                       void* userdata);
  void create_service(struct AvahiClient* c);

  std::string name_;
  uint16_t port_;
  MdnsTxtRecords txt_;
  struct AvahiThreadedPoll* poll_ = nullptr;
  struct AvahiClient* client_ = nullptr;
  struct AvahiEntryGroup* group_ = nullptr;
};

}  // namespace oca

#endif  // _USE_AVAHI_
#endif  // OCA_MDNS_PUBLISHER_HPP_

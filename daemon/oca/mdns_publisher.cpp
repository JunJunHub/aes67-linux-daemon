//  mdns_publisher.cpp

#ifdef _USE_AVAHI_

#include "oca/mdns_publisher.hpp"

#include <avahi-common/alternative.h>
#include <avahi-common/malloc.h>

namespace oca {

MdnsPublisher::MdnsPublisher(std::string name,
                             uint16_t port,
                             MdnsTxtRecords txt)
    : name_(std::move(name)), port_(port), txt_(std::move(txt)) {}

MdnsPublisher::~MdnsPublisher() {
  stop();
}

bool MdnsPublisher::start() {
  if (poll_)
    return true;
  poll_ = avahi_threaded_poll_new();
  if (!poll_)
    return false;
  int err;
  client_ =
      avahi_client_new(avahi_threaded_poll_get(poll_), AVAHI_CLIENT_NO_FAIL,
                       &MdnsPublisher::client_cb, this, &err);
  if (!client_) {
    avahi_threaded_poll_free(poll_);
    poll_ = nullptr;
    return false;
  }
  avahi_threaded_poll_start(poll_);
  return true;
}

void MdnsPublisher::stop() {
  if (!poll_)
    return;
  avahi_threaded_poll_stop(poll_);
  if (group_) {
    avahi_entry_group_free(group_);
    group_ = nullptr;
  }
  if (client_) {
    avahi_client_free(client_);
    client_ = nullptr;
  }
  avahi_threaded_poll_free(poll_);
  poll_ = nullptr;
}

void MdnsPublisher::create_service(struct AvahiClient* c) {
  if (!group_) {
    group_ = avahi_entry_group_new(c, &MdnsPublisher::group_cb, this);
    if (!group_)
      return;
  }
  if (!avahi_entry_group_is_empty(group_))
    return;

  // Spec5:TXT 记录含设备元数据(Fitcan 控制器期望)。
  // 用 avahi_string_list 逐条追加非空字段:varargs 版 add_service 以 nullptr
  // 结尾, 任一中段字段为空即截断后续(mac/device_id/channels/firmware
  // 会被丢弃)。 strlst 版无此缺陷。
  AvahiStringList* txt = nullptr;
  txt = avahi_string_list_add_pair(txt, "txtvers", "1");
  txt = avahi_string_list_add_pair(txt, "protovers", "1");
  if (!txt_.ip_addr.empty())
    txt = avahi_string_list_add_pair(txt, "ip_addr", txt_.ip_addr.c_str());
  if (!txt_.ip_addr_sec.empty())
    txt = avahi_string_list_add_pair(txt, "ip_addr_sec",
                                     txt_.ip_addr_sec.c_str());
  if (!txt_.mac_addr.empty())
    txt = avahi_string_list_add_pair(txt, "mac_addr", txt_.mac_addr.c_str());
  if (!txt_.device_id.empty())
    txt = avahi_string_list_add_pair(txt, "device_id", txt_.device_id.c_str());
  if (txt_.channels > 0) {
    const std::string ch = std::to_string(txt_.channels);
    txt = avahi_string_list_add_pair(txt, "channels", ch.c_str());
  }
  if (!txt_.firmware.empty())
    txt = avahi_string_list_add_pair(txt, "firmware", txt_.firmware.c_str());

  int r = avahi_entry_group_add_service_strlst(
      group_, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
      static_cast<AvahiPublishFlags>(0), name_.c_str(), "_oca._tcp", nullptr,
      nullptr, port_, txt);
  avahi_string_list_free(txt);
  if (r < 0) {
    if (r == AVAHI_ERR_COLLISION) {
      // 名称冲突:换一个名(AVAHI_CLIENT_S_COLLISION 重连时处理)
    }
    return;
  }
  avahi_entry_group_commit(group_);
}

void MdnsPublisher::group_cb(struct AvahiEntryGroup* g,
                             AvahiEntryGroupState state,
                             void* userdata) {
  auto* self = static_cast<MdnsPublisher*>(userdata);
  if (state == AVAHI_ENTRY_GROUP_COLLISION) {
    char* alt = avahi_alternative_service_name(self->name_.c_str());
    self->name_ = alt;
    avahi_free(alt);
    avahi_entry_group_reset(g);
    self->create_service(avahi_entry_group_get_client(g));
  }
}

void MdnsPublisher::client_cb(struct AvahiClient* c,
                              AvahiClientState state,
                              void* userdata) {
  auto* self = static_cast<MdnsPublisher*>(userdata);
  switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
      self->create_service(c);
      break;
    case AVAHI_CLIENT_S_COLLISION:
    case AVAHI_CLIENT_FAILURE:
      self->group_ = nullptr;
      break;
    default:
      break;
  }
}

}  // namespace oca

#endif  // _USE_AVAHI_

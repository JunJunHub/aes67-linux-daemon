//  mdns_publisher.cpp

#ifdef _USE_AVAHI_

#include "oca/mdns_publisher.hpp"

#include <avahi-common/alternative.h>
#include <avahi-common/malloc.h>

namespace oca {

MdnsPublisher::MdnsPublisher(std::string name, uint16_t port)
    : name_(std::move(name)), port_(port) {}

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

  // TXT: txtvers=1, protovers=1
  int r = avahi_entry_group_add_service(
      group_, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
      static_cast<AvahiPublishFlags>(0), name_.c_str(), "_oca._tcp", nullptr,
      nullptr, port_, "txtvers=1", "protovers=1", nullptr);
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
    // 名称冲突:取替代名重发
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
      self->create_service(c);  // 服务器就绪,发布
      break;
    case AVAHI_CLIENT_S_COLLISION:
    case AVAHI_CLIENT_FAILURE:
      self->group_ = nullptr;  // 重建
      break;
    default:
      break;
  }
}

}  // namespace oca

#endif  // _USE_AVAHI_

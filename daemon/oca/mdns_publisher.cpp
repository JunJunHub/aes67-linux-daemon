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

  // Spec5:TXT 记录含设备元数据。key 名为 Fitcan 控制器私有约定(权威源
  // OcaLiteLib AES70Browser_OnResolveServiceName.cpp::GetInfoFromTXT_Record),
  // 非 AES70 标准风格。Fitcan 解析的 key 与标准不同名,且存在硬过滤:
  // AES70Manager_ThreadOnBrowsingDevice.cpp:147 IP_P==0 && IP_S==0 即丢弃。
  // 故单接口设备必须发 IP_P(真实 IP,与 A 记录一致,否则故障连接检查失败)。
  // 用 avahi_string_list 逐条追加:varargs 版 add_service 以 nullptr 结尾,
  // 任一中段字段为空即截断后续。strlst 版无此缺陷。
  AvahiStringList* txt = nullptr;
  txt = avahi_string_list_add_pair(txt, "txtvers", "1");
  txt = avahi_string_list_add_pair(txt, "protovers", "1");
  // Fitcan 私有 key:主接口(IP_P/MAC_P)为必需/建议,备接口(IP_S/MAC_S)
  // 单接口可省(硬过滤是 &&,IP_P 有值即过)。
  if (!txt_.ip_addr.empty())
    txt = avahi_string_list_add_pair(txt, "IP_P", txt_.ip_addr.c_str());
  if (!txt_.ip_addr_sec.empty())
    txt = avahi_string_list_add_pair(txt, "IP_S", txt_.ip_addr_sec.c_str());
  if (!txt_.mac_addr.empty())
    txt = avahi_string_list_add_pair(txt, "MAC_P", txt_.mac_addr.c_str());
  // ServicePortNo:Fitcan 仅作信息字段(控制连接实际用 SRV 记录端口);
  // 与 port_ 一致以保持可读性。
  txt = avahi_string_list_add_pair(txt, "ServicePortNo",
                                   std::to_string(port_).c_str());
  // DeviceID:Fitcan 按 atol 十进制数字解析(非字符串)。daemon 的 node_id
  // 是字符串,发数字 0(仅用于设备查找,不参与过滤/连接;设备名已在 SRV
  // service instance name 中)。
  txt = avahi_string_list_add_pair(txt, "DeviceID", "0");
  // SubChNum:每路流的子通道数(1=mono,2=stereo),非总通道数。默认 1。
  txt = avahi_string_list_add_pair(txt, "SubChNum", "1");
  if (!txt_.firmware.empty())
    txt = avahi_string_list_add_pair(txt, "FirmwareVersion",
                                     txt_.firmware.c_str());

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

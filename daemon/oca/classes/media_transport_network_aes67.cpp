//  classes/media_transport_network_aes67.cpp
//  - OcaMediaTransportNetworkAES67 {1,4,2,0xFFFF,0xFA,0x2EE9,1} v1 实现

#include "oca/classes/media_transport_network_aes67.hpp"

#include "oca/methods.hpp"

namespace oca {

namespace {
// ClassID {1,4,2,0xFFFF,0xFA,0x2EE9,1} — 7 字段
// 0xFFFF = proprietary marker, 0x00FA = OCA Alliance manufacturer ID 高字节,
// 0x2EE9 = manufacturer ID 低字节, 1 = 子类编号
const ClassIdentification kMtnAes67ClassId = {
    {{1, 4, 2, 0xFFFF, 0x00FA, 0x2EE9, 1}},
    1};
}  // namespace

const ClassIdentification& OcaMediaTransportNetworkAES67::class_id() const {
  return kMtnAes67ClassId;
}

ExecResult OcaMediaTransportNetworkAES67::exec(MethodID m,
                                               ocp1::Reader& req,
                                               ocp1::Writer& rsp,
                                               Session& sess) {
  // AES67 defLevel=7 方法优先
  if (m.defLevel == methods::kDefLevelMtnAes67) {
    return handle_mtn_aes67(m.methodIndex, req, rsp);
  }
  // 基类 defLevel=3 方法
  return OcaMediaTransportNetwork::exec(m, req, rsp, sess);
}

// ── 基类纯虚实现 ──

ExecResult OcaMediaTransportNetworkAES67::add_source_connector_impl(
    ocp1::Reader& req,
    ocp1::Writer& rsp) {
  // AddSourceConnector 参数:
  //   param1: OcaMediaConnectorState (u8): 0=STOPPED,1=SETTING_UP,2=RUNNING,
  //           3=PAUSED,4=FAULT
  //   param2: OcaMediaSourceConnector (复合结构)
  if (req.remaining() < 1)
    return {Status::BadFormat, 0};
  uint8_t state = req.u8();

  // 解析 OcaMediaSourceConnector(OCAMicro 格式):
  // u16 connectorIDInternal
  // string idExternal
  // OcaMediaConnection: u8 secure + blob streamParams + u8 streamCastMode
  // OcaMediaCoding: u16 codingSchemeID + string codecParams + u32 clockONo
  // u16 pinCount
  // OcaLiteMap<u16,PortID>: u16 count + [u16 key, u32 ownerONo, u16 portID]*
  // float32 alignmentLevel
  if (req.remaining() < 2)
    return {Status::BadFormat, 0};
  uint16_t int_id = req.u16();
  std::string name = req.string();

  // OcaMediaConnection
  if (req.remaining() < 1)
    return {Status::BadFormat, 0};
  (void)req.u8();               // secure
  auto conn_blob = req.blob();  // streamParameters
  if (req.remaining() < 1)
    return {Status::BadFormat, 0};
  uint8_t cast_mode = req.u8();  // streamCastMode

  // OcaMediaCoding
  if (req.remaining() < 2)
    return {Status::BadFormat, 0};
  uint16_t coding_scheme = req.u16();
  std::string codec = req.string();
  if (req.remaining() < 4)
    return {Status::BadFormat, 0};
  uint32_t clock_ono = req.u32();

  // pinCount + pinMap
  if (req.remaining() < 2)
    return {Status::BadFormat, 0};
  uint16_t pin_count = req.u16();
  std::vector<uint8_t> chan_map;
  for (uint16_t i = 0; i < pin_count; ++i) {
    if (req.remaining() < 8)
      return {Status::BadFormat, 0};
    (void)req.u16();  // key = pin index
    (void)req.u32();  // ownerONo
    chan_map.push_back(static_cast<uint8_t>(req.u16() & 0xFF));  // portID
  }
  // alignmentLevel
  if (req.remaining() >= 4)
    (void)req.f32();

  // 分配 source ID
  uint8_t src_id = next_source_id_++;
  if (src_id > 63)
    return {Status::ParameterOutOfRange, 0};

  // 构造 SourceConnector
  SourceConnector sc;
  sc.connector_id = int_id ? int_id : (0x0001 + src_id);
  sc.name = name.empty() ? ("Source " + std::to_string(src_id)) : name;
  sc.codec = codec.empty() ? "L24" : codec;
  sc.map = chan_map.empty() ? std::vector<uint8_t>{0} : chan_map;
  sc.enabled = (state == 2 /*RUNNING*/);

  // 通过 bridge 添加 source
  if (bridge_) {
    OcaAudioBridge::SourceInfo si;
    si.id = src_id;
    si.enabled = sc.enabled;
    si.name = sc.name;
    si.codec = sc.codec;
    si.map = sc.map;
    si.max_samples_per_packet = 48;
    si.ttl = 15;
    si.payload_type = 98;
    si.dscp = 34;
    si.refclk_ptp_traceable = true;
    if (!bridge_->add_source(si))
      return {Status::ProcessingFailed, 0};
  }

  sources_[sc.connector_id] = sc;

  // 返回更新后的 connector(含分配的 ID)
  write_source_connector(sc, rsp);
  return {Status::OK, 1};
}

ExecResult OcaMediaTransportNetworkAES67::add_sink_connector_impl(
    ocp1::Reader& req,
    ocp1::Writer& rsp) {
  // AddSinkConnector 参数:
  //   param1: OcaMediaConnectorState (u8)
  //   param2: OcaMediaSinkConnector (复合结构,比 Source 多 alignmentGain)
  if (req.remaining() < 1)
    return {Status::BadFormat, 0};
  uint8_t state = req.u8();

  // 解析 OcaMediaSinkConnector
  if (req.remaining() < 2)
    return {Status::BadFormat, 0};
  uint16_t int_id = req.u16();
  std::string name = req.string();

  // OcaMediaConnection
  if (req.remaining() < 1)
    return {Status::BadFormat, 0};
  (void)req.u8();  // secure
  auto conn_blob = req.blob();
  if (req.remaining() < 1)
    return {Status::BadFormat, 0};
  uint8_t cast_mode = req.u8();

  // OcaMediaCoding
  if (req.remaining() < 2)
    return {Status::BadFormat, 0};
  uint16_t coding_scheme = req.u16();
  std::string codec = req.string();
  if (req.remaining() < 4)
    return {Status::BadFormat, 0};
  uint32_t clock_ono = req.u32();

  // pinCount + pinMap
  if (req.remaining() < 2)
    return {Status::BadFormat, 0};
  uint16_t pin_count = req.u16();
  std::vector<uint8_t> chan_map;
  for (uint16_t i = 0; i < pin_count; ++i) {
    if (req.remaining() < 8)
      return {Status::BadFormat, 0};
    (void)req.u16();
    (void)req.u32();
    chan_map.push_back(static_cast<uint8_t>(req.u16() & 0xFF));
  }
  // alignmentLevel + alignmentGain(Sink 独有)
  if (req.remaining() >= 4)
    (void)req.f32();
  if (req.remaining() >= 4)
    (void)req.f32();

  uint8_t snk_id = next_sink_id_++;
  if (snk_id > 63)
    return {Status::ParameterOutOfRange, 0};

  SinkConnector sc;
  sc.connector_id = int_id ? int_id : (0x0101 + snk_id);
  sc.name = name.empty() ? ("Sink " + std::to_string(snk_id)) : name;
  sc.codec = codec.empty() ? "L24" : codec;
  sc.map = chan_map.empty() ? std::vector<uint8_t>{0} : chan_map;
  sc.use_sdp = false;

  if (bridge_) {
    OcaAudioBridge::SinkInfo si;
    si.id = snk_id;
    si.name = sc.name;
    si.delay = 576;
    si.source_url = "";
    si.sdp = "";
    si.use_sdp = false;
    si.ignore_refclk_gmid = false;
    si.map = sc.map;
    if (!bridge_->add_sink(si))
      return {Status::ProcessingFailed, 0};
  }

  sinks_[sc.connector_id] = sc;

  write_sink_connector(sc, rsp);
  return {Status::OK, 1};
}

ExecResult OcaMediaTransportNetworkAES67::delete_connector_impl(
    uint32_t connector_id,
    ocp1::Writer& rsp) {
  auto sit = sources_.find(connector_id);
  if (sit != sources_.end()) {
    if (bridge_)
      bridge_->remove_source(static_cast<uint8_t>(connector_id & 0xFF));
    sources_.erase(sit);
    return {Status::OK, 0};
  }
  auto kit = sinks_.find(connector_id);
  if (kit != sinks_.end()) {
    if (bridge_)
      bridge_->remove_sink(static_cast<uint8_t>(connector_id & 0xFF));
    sinks_.erase(kit);
    return {Status::OK, 0};
  }
  return {Status::ParameterError, 0};
}

// ── AES67 defLevel-7 方法 ──

ExecResult OcaMediaTransportNetworkAES67::handle_mtn_aes67(uint16_t idx,
                                                           ocp1::Reader& req,
                                                           ocp1::Writer& rsp) {
  switch (idx) {
    case methods::kMtnAes67GetSendPacketTimes:
    case methods::kMtnAes67GetReceivePacketTimes:
    case methods::kMtnAes67GetMinReceiveBufferCapacity:
    case methods::kMtnAes67GetMaxReceiveBufferCapacity:
    case methods::kMtnAes67GetTransmissionTimeVariation:
    case methods::kMtnAes67GetSupportedDiscoverySystems:
      return {Status::NotImplemented, 0};

    case methods::kMtnAes67DeleteAllConnectors: {
      // Fitcan 私有:DeleteAllConnectors(u16 type)
      // 1=RX sinks, 2=TX sources, 3=both
      uint16_t type = 0;
      if (req.remaining() >= 2)
        type = req.u16();
      if (type == 2 || type == 3) {
        for (auto& [id, sc] : sources_) {
          if (bridge_)
            bridge_->remove_source(static_cast<uint8_t>(id & 0xFF));
        }
        sources_.clear();
      }
      if (type == 1 || type == 3) {
        for (auto& [id, sc] : sinks_) {
          if (bridge_)
            bridge_->remove_sink(static_cast<uint8_t>(id & 0xFF));
        }
        sinks_.clear();
      }
      return {Status::OK, 0};
    }

    case methods::kMtnAes67UpdateRouteTableCommand:
      return {Status::NotImplemented, 0};

    default:
      return {Status::NotImplemented, 0};
  }
}

}  // namespace oca

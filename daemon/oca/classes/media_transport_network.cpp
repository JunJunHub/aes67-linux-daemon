//  classes/media_transport_network.cpp - OcaMediaTransportNetwork {1,4,2} v1
//  实现

#include "oca/classes/media_transport_network.hpp"

#include "oca/methods.hpp"

namespace oca {

namespace {
const ClassIdentification kMtnClassId = {{{1, 4, 2}}, 1};
}  // namespace

const ClassIdentification& OcaMediaTransportNetwork::class_id() const {
  return kMtnClassId;
}

ExecResult OcaMediaTransportNetwork::exec(MethodID m,
                                          ocp1::Reader& req,
                                          ocp1::Writer& rsp,
                                          Session& sess) {
  if (m.defLevel == methods::kDefLevelMtn) {
    return handle_mtn(m.methodIndex, req, rsp);
  }
  return OcaApplicationNetwork::exec(m, req, rsp, sess);
}

ExecResult OcaMediaTransportNetwork::handle_mtn(uint16_t idx,
                                                ocp1::Reader& req,
                                                ocp1::Writer& rsp) {
  switch (idx) {
    case methods::kMtnGetMediaProtocol:
      rsp.u8(get_media_protocol());
      return {Status::OK, 1};

    case methods::kMtnGetPorts: {
      // Ocp1List<OcaPort> = u16 count + [OcaPort: u16 id + u8 mode + u16
      // index]* 从 bridge 获取 I/O 端口数。OcaPortMode: INPUT=1,
      // OUTPUT=2(OCAMicro)。
      uint32_t inputs = bridge_ ? bridge_->get_input_channels() : 0;
      uint32_t outputs = bridge_ ? bridge_->get_output_channels() : 0;
      uint16_t total = static_cast<uint16_t>(inputs + outputs);
      rsp.u16(total);
      for (uint16_t i = 0; i < static_cast<uint16_t>(inputs); ++i) {
        rsp.u16(i);      // id
        rsp.u8(1);       // mode = INPUT
        rsp.u16(i + 1);  // index(从 1 起)
      }
      for (uint16_t i = 0; i < static_cast<uint16_t>(outputs); ++i) {
        rsp.u16(static_cast<uint16_t>(inputs) + i);  // id
        rsp.u8(2);                                   // mode = OUTPUT
        rsp.u16(i + 1);                              // index(从 1 起)
      }
      return {Status::OK, 1};
    }

    case methods::kMtnGetPortName:
      return {Status::NotImplemented, 0};

    case methods::kMtnSetPortName:
      return {Status::NotImplemented, 0};

    case methods::kMtnGetMaxSourceConnectors:
      rsp.u16(get_max_source_connectors());
      return {Status::OK, 1};

    case methods::kMtnGetMaxSinkConnectors:
      rsp.u16(get_max_sink_connectors());
      return {Status::OK, 1};

    case methods::kMtnGetMaxPinsPerConnector:
      rsp.u16(get_max_pins_per_connector());
      return {Status::OK, 1};

    case methods::kMtnGetMaxPortsPerPin:
      rsp.u16(get_max_ports_per_pin());
      return {Status::OK, 1};

    case methods::kMtnGetSourceConnectors: {
      // Ocp1List<OcaMediaSourceConnector>
      rsp.u16(static_cast<uint16_t>(sources_.size()));
      for (const auto& [id, sc] : sources_)
        write_source_connector(sc, rsp);
      return {Status::OK, 1};
    }

    case methods::kMtnGetSourceConnector: {
      // OcaMediaConnectorID = u16(OCAMicro OcaLiteMediaConnectorID)
      if (req.remaining() < 2)
        return {Status::BadFormat, 0};
      uint16_t cid = req.u16();
      auto it = sources_.find(cid);
      if (it == sources_.end())
        return {Status::ParameterError, 0};
      write_source_connector(it->second, rsp);
      return {Status::OK, 1};
    }

    case methods::kMtnGetSinkConnectors: {
      // Ocp1List<OcaMediaSinkConnector>
      rsp.u16(static_cast<uint16_t>(sinks_.size()));
      for (const auto& [id, sc] : sinks_)
        write_sink_connector(sc, rsp);
      return {Status::OK, 1};
    }

    case methods::kMtnGetSinkConnector: {
      // OcaMediaConnectorID = u16
      if (req.remaining() < 2)
        return {Status::BadFormat, 0};
      uint16_t cid = req.u16();
      auto it = sinks_.find(cid);
      if (it == sinks_.end())
        return {Status::ParameterError, 0};
      write_sink_connector(it->second, rsp);
      return {Status::OK, 1};
    }

    case methods::kMtnGetConnectorsStatuses: {
      // Ocp1List<OcaMediaConnectorStatus>: {u16 id, u8 state, u16 errorCode}*
      // state = OcaMediaConnectorState: STOPPED=0,SETTING_UP=1,RUNNING=2,
      // PAUSED=3,FAULT=4(OCAMicro)。errorCode=0(无故障)。
      uint16_t total = static_cast<uint16_t>(sources_.size() + sinks_.size());
      rsp.u16(total);
      for (const auto& [id, sc] : sources_) {
        uint8_t state = sc.enabled ? 2 /*RUNNING*/ : 0 /*STOPPED*/;
        write_connector_status(id, state, rsp);
      }
      for (const auto& [id, sc] : sinks_) {
        uint8_t state = 0; /*STOPPED*/
        if (bridge_) {
          auto ss = bridge_->get_sink_status(sc.daemon_id);
          state = ss.receiving ? 2 /*RUNNING*/ : (ss.muted ? 3 /*PAUSED*/ : 0);
        }
        write_connector_status(id, state, rsp);
      }
      return {Status::OK, 1};
    }

    case methods::kMtnGetConnectorStatus: {
      // OcaMediaConnectorID = u16
      if (req.remaining() < 2)
        return {Status::BadFormat, 0};
      uint16_t cid = req.u16();
      auto sit = sources_.find(cid);
      if (sit != sources_.end()) {
        uint8_t state = sit->second.enabled ? 2 /*RUNNING*/ : 0 /*STOPPED*/;
        write_connector_status(cid, state, rsp);
        return {Status::OK, 1};
      }
      auto kit = sinks_.find(cid);
      if (kit != sinks_.end()) {
        uint8_t state = 0;
        if (bridge_) {
          auto ss = bridge_->get_sink_status(kit->second.daemon_id);
          state = ss.receiving ? 2 : (ss.muted ? 3 : 0);
        }
        write_connector_status(cid, state, rsp);
        return {Status::OK, 1};
      }
      return {Status::ParameterError, 0};
    }

    case methods::kMtnAddSourceConnector:
      return add_source_connector_impl(req, rsp);

    case methods::kMtnAddSinkConnector:
      return add_sink_connector_impl(req, rsp);

    case methods::kMtnControlConnector:
      return {Status::NotImplemented, 0};

    case methods::kMtnSetSourceConnectorPinMap: {
      // connectorID(u16) + OcaMap<u16,OcaPortID>: u16 count + [u16 key, u8
      // mode, u16 index]*  (OcaPortID = {OcaPortMode u8, OcaUint16 index})
      if (req.remaining() < 4)
        return {Status::BadFormat, 0};
      uint16_t cid = req.u16();
      auto it = sources_.find(cid);
      if (it == sources_.end())
        return {Status::ParameterError, 0};
      uint16_t count = req.u16();
      std::vector<uint8_t> new_map;
      for (uint16_t i = 0; i < count; ++i) {
        if (req.remaining() < 5)
          return {Status::BadFormat, 0};
        (void)req.u16();  // key = pin index
        (void)req.u8();   // mode (INPUT=1/OUTPUT=2)
        new_map.push_back(
            static_cast<uint8_t>(req.u16() & 0xFF));  // port index
      }
      it->second.map = new_map;
      return {Status::OK, 0};
    }

    case methods::kMtnSetSinkConnectorPinMap: {
      if (req.remaining() < 4)
        return {Status::BadFormat, 0};
      uint16_t cid = req.u16();
      auto it = sinks_.find(cid);
      if (it == sinks_.end())
        return {Status::ParameterError, 0};
      uint16_t count = req.u16();
      std::vector<uint8_t> new_map;
      for (uint16_t i = 0; i < count; ++i) {
        if (req.remaining() < 5)
          return {Status::BadFormat, 0};
        (void)req.u16();
        (void)req.u8();
        new_map.push_back(static_cast<uint8_t>(req.u16() & 0xFF));
      }
      it->second.map = new_map;
      return {Status::OK, 0};
    }

    case methods::kMtnSetConnectorConnection:
    case methods::kMtnSetConnectorCoding:
    case methods::kMtnSetConnectorAlignmentLevel:
    case methods::kMtnSetConnectorAlignmentGain:
      return {Status::NotImplemented, 0};

    case methods::kMtnDeleteConnector: {
      // OcaMediaConnectorID = u16
      if (req.remaining() < 2)
        return {Status::BadFormat, 0};
      uint16_t cid = req.u16();
      return delete_connector_impl(cid, rsp);
    }

    case methods::kMtnGetAlignmentLevel:
    case methods::kMtnGetAlignmentGain:
      return {Status::NotImplemented, 0};

    default:
      return {Status::NotImplemented, 0};
  }
}

// ── connector 序列化 ──

void OcaMediaTransportNetwork::write_source_connector(const SourceConnector& sc,
                                                      ocp1::Writer& rsp) {
  // OcaMediaSourceConnector OCAMicro 格式(逐字段对齐
  // OcaLiteMediaSourceConnector): u16    IDInternal (OcaLiteMediaConnectorID =
  // u16) string IDExternal OcaMediaConnection: u8 secure + blob streamParams +
  // u8 streamCastMode OcaMediaCoding: u16 codingSchemeID + string codecParams +
  // u32 clockONo u16    pinCount OcaLiteMap<u16,PortID>: u16 count + [u16 key,
  // u8 mode, u16 index]*
  //   (OcaLitePortID = {OcaPortMode u8, OcaUint16 index}; mode
  //   INPUT=1/OUTPUT=2)
  // float32 alignmentLevel
  rsp.u16(static_cast<uint16_t>(sc.connector_id & 0xFFFF));
  rsp.string(sc.name);
  // OcaMediaConnection
  rsp.u8(0);             // secure = false
  rsp.blob(nullptr, 0);  // streamParameters(简化:空 blob)
  rsp.u8(sc.dest_addr.empty() ? 1 /*UNICAST*/ : 2 /*MULTICAST*/);
  // OcaMediaCoding
  rsp.u16(1);  // codingSchemeID = PCM
  rsp.string(sc.codec);
  rsp.u32(0);  // clockONo = 0(无关联时钟)
  // pinCount + pinMap
  rsp.u16(static_cast<uint16_t>(sc.map.size()));
  for (uint16_t i = 0; i < static_cast<uint16_t>(sc.map.size()); ++i) {
    rsp.u16(i);          // key = pin index
    rsp.u8(2);           // mode = OUTPUT(source 出口)
    rsp.u16(sc.map[i]);  // port index
  }
  rsp.f32(0.0f);  // alignmentLevel
}

void OcaMediaTransportNetwork::write_sink_connector(const SinkConnector& sc,
                                                    ocp1::Writer& rsp) {
  // OcaMediaSinkConnector(比 Source 多 alignmentGain float32)
  rsp.u16(static_cast<uint16_t>(sc.connector_id & 0xFFFF));
  rsp.string(sc.name);
  // OcaMediaConnection
  rsp.u8(0);  // secure
  rsp.blob(nullptr, 0);
  rsp.u8(sc.dest_addr.empty() ? 1 /*UNICAST*/ : 2 /*MULTICAST*/);
  // OcaMediaCoding
  rsp.u16(1);  // codingSchemeID = PCM
  rsp.string(sc.codec);
  rsp.u32(0);  // clockONo
  // pinCount + pinMap
  rsp.u16(static_cast<uint16_t>(sc.map.size()));
  for (uint16_t i = 0; i < static_cast<uint16_t>(sc.map.size()); ++i) {
    rsp.u16(i);
    rsp.u8(1);  // mode = INPUT(sink 入口)
    rsp.u16(sc.map[i]);
  }
  rsp.f32(0.0f);  // alignmentLevel
  rsp.f32(0.0f);  // alignmentGain(Sink 独有)
}

void OcaMediaTransportNetwork::write_connector_status(uint16_t connector_id,
                                                      uint8_t state,
                                                      ocp1::Writer& rsp) {
  // OcaMediaConnectorStatus: u16 connectorID + u8 state + u16 errorCode
  // (OcaLiteMediaConnectorStatus = {u16 id, u8 state, u16 errorCode})
  rsp.u16(connector_id);
  rsp.u8(state);
  rsp.u16(0);  // errorCode = 0(无故障)
}

}  // namespace oca

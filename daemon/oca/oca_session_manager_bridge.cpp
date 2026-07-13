//  oca_session_manager_bridge.cpp - OcaAudioBridge 的 SessionManager 实现
//
//  此文件 include SessionManager/Config/DriverManager 完整头文件，
//  做类型转换(StreamSource→SourceInfo 等)，注册 Observer。
//  OCA 其余编译单元零依赖这些头文件。

#include "oca/oca_session_manager_bridge.hpp"

#include "config.hpp"
#include "driver_interface.hpp"  // FAKE_DRIVER 感知,条件引入 DriverManager
#include "session_manager.hpp"

namespace oca {

OcaSessionManagerBridge::OcaSessionManagerBridge(
    std::shared_ptr<SessionManager> sm,
    std::shared_ptr<Config> cfg,
    std::shared_ptr<DriverManager> drv)
    : sm_(std::move(sm)), cfg_(std::move(cfg)), drv_(std::move(drv)) {
  // 注册 SessionManager PTP 状态 observer
  if (sm_) {
    sm_->add_ptp_status_observer([this](const std::string& status) -> bool {
      if (ptp_cb_) {
        PtpStatus ps = get_ptp_status();
        ptp_cb_(ps);
      }
      return true;
    });
    // 注册 source add/remove observer
    sm_->add_source_observer(::SessionManager::SourceObserverType::add_source,
                             [this](uint8_t id, const std::string& /*name*/,
                                    const std::string& /*sdp*/) -> bool {
                               if (source_cb_)
                                 source_cb_(id, true);
                               return true;
                             });
    sm_->add_source_observer(
        ::SessionManager::SourceObserverType::remove_source,
        [this](uint8_t id, const std::string& /*name*/,
               const std::string& /*sdp*/) -> bool {
          if (source_cb_)
            source_cb_(id, false);
          return true;
        });
    // 注册 sink add/remove observer
    sm_->add_sink_observer(
        ::SessionManager::SinkObserverType::add_sink,
        [this](uint8_t id, const std::string& /*name*/) -> bool {
          if (sink_cb_)
            sink_cb_(id, true);
          return true;
        });
    sm_->add_sink_observer(
        ::SessionManager::SinkObserverType::remove_sink,
        [this](uint8_t id, const std::string& /*name*/) -> bool {
          if (sink_cb_)
            sink_cb_(id, false);
          return true;
        });
  }
}

OcaSessionManagerBridge::~OcaSessionManagerBridge() = default;

// ── PTP 时钟 ──

OcaAudioBridge::PtpConfig OcaSessionManagerBridge::get_ptp_config() const {
  ::PTPConfig cfg;
  if (sm_)
    sm_->get_ptp_config(cfg);
  return {cfg.domain, cfg.dscp};
}

bool OcaSessionManagerBridge::set_ptp_config(PtpConfig cfg) {
  if (!sm_)
    return false;
  ::PTPConfig c;
  c.domain = cfg.domain;
  c.dscp = cfg.dscp;
  return !sm_->set_ptp_config(c);
}

OcaAudioBridge::PtpStatus OcaSessionManagerBridge::get_ptp_status() const {
  ::PTPStatus st;
  if (sm_)
    sm_->get_ptp_status(st);
  PtpLockState lock = PtpLockState::Unlocked;
  if (st.status == "locked")
    lock = PtpLockState::Locked;
  else if (st.status == "locking")
    lock = PtpLockState::Locking;
  return {lock, st.gmid, st.jitter};
}

// ── 采样率 ──

uint32_t OcaSessionManagerBridge::get_sample_rate() const {
  if (cfg_)
    return cfg_->get_sample_rate();
  return 48000;
}

bool OcaSessionManagerBridge::set_sample_rate(uint32_t hz) {
  if (!drv_)
    return false;
  return !drv_->set_sample_rate(hz);
}

std::vector<uint32_t> OcaSessionManagerBridge::get_supported_sample_rates()
    const {
  // AES67 支持的采样率列表
  return {44100, 48000, 88200, 96000, 176400, 192000};
}

// ── Source ──

std::vector<OcaAudioBridge::SourceInfo> OcaSessionManagerBridge::get_sources()
    const {
  std::vector<SourceInfo> out;
  if (!sm_)
    return out;
  for (const auto& s : sm_->get_sources()) {
    out.push_back({s.id, s.enabled, s.name, s.codec, s.address, s.ttl,
                   s.payload_type, s.dscp, s.refclk_ptp_traceable,
                   s.max_samples_per_packet, s.map});
  }
  return out;
}

bool OcaSessionManagerBridge::add_source(const SourceInfo& s) {
  if (!sm_)
    return false;
  ::StreamSource src;
  src.id = s.id;
  src.enabled = s.enabled;
  src.name = s.name;
  src.io = "Audio Device";
  src.codec = s.codec;
  src.address = s.address;
  src.ttl = s.ttl;
  src.payload_type = s.payload_type;
  src.dscp = s.dscp;
  src.refclk_ptp_traceable = s.refclk_ptp_traceable;
  src.max_samples_per_packet = s.max_samples_per_packet;
  src.map = s.map;
  return !sm_->add_source(src);
}

bool OcaSessionManagerBridge::remove_source(uint8_t id) {
  if (!sm_)
    return false;
  return !sm_->remove_source(id);
}

std::string OcaSessionManagerBridge::get_source_sdp(uint8_t id) const {
  std::string sdp;
  if (sm_)
    sm_->get_source_sdp(id, sdp);
  return sdp;
}

// ── Sink ──

std::vector<OcaAudioBridge::SinkInfo> OcaSessionManagerBridge::get_sinks()
    const {
  std::vector<SinkInfo> out;
  if (!sm_)
    return out;
  for (const auto& s : sm_->get_sinks()) {
    out.push_back({s.id, s.name, s.delay, s.source, s.sdp, s.use_sdp,
                   s.ignore_refclk_gmid, s.map});
  }
  return out;
}

bool OcaSessionManagerBridge::add_sink(const SinkInfo& s) {
  if (!sm_)
    return false;
  ::StreamSink snk;
  snk.id = s.id;
  snk.name = s.name;
  snk.io = "Audio Device";
  snk.delay = s.delay;
  snk.source = s.source_url;
  snk.sdp = s.sdp;
  snk.use_sdp = s.use_sdp;
  snk.ignore_refclk_gmid = s.ignore_refclk_gmid;
  snk.map = s.map;
  return !sm_->add_sink(snk);
}

bool OcaSessionManagerBridge::remove_sink(uint8_t id) {
  if (!sm_)
    return false;
  return !sm_->remove_sink(id);
}

OcaAudioBridge::SinkStatus OcaSessionManagerBridge::get_sink_status(
    uint8_t id) const {
  ::SinkStreamStatus st;
  if (sm_)
    sm_->get_sink_status(id, st);
  return {st.is_rtp_seq_id_error,       st.is_rtp_ssrc_error,
          st.is_rtp_payload_type_error, st.is_rtp_sac_error,
          st.is_receiving_rtp_packet,   st.is_muted};
}

// ── 网络信息 ──

std::string OcaSessionManagerBridge::get_interface_name() const {
  if (cfg_)
    return cfg_->get_interface_name();
  return {};
}

std::string OcaSessionManagerBridge::get_ip_addr() const {
  if (cfg_)
    return cfg_->get_ip_addr_str();
  return {};
}

std::string OcaSessionManagerBridge::get_mac_addr() const {
  if (cfg_)
    return cfg_->get_mac_addr_str();
  return {};
}

// ── I/O 端口 ──

uint32_t OcaSessionManagerBridge::get_input_channels() const {
  if (drv_) {
    int32_t n = 0;
    if (!drv_->get_number_of_inputs(n))
      return static_cast<uint32_t>(n);
  }
  return 0;
}

uint32_t OcaSessionManagerBridge::get_output_channels() const {
  if (drv_) {
    int32_t n = 0;
    if (!drv_->get_number_of_outputs(n))
      return static_cast<uint32_t>(n);
  }
  return 0;
}

// ── Observer ──

void OcaSessionManagerBridge::set_ptp_observer(PtpObserver cb) {
  ptp_cb_ = std::move(cb);
}

void OcaSessionManagerBridge::set_source_observer(SourceObserver cb) {
  source_cb_ = std::move(cb);
}

void OcaSessionManagerBridge::set_sink_observer(SinkObserver cb) {
  sink_cb_ = std::move(cb);
}

}  // namespace oca

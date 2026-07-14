//  oca_audio_bridge.hpp - OCA 协议层与 daemon 音频运行时的桥接接口
//
//  纯虚接口,定义在 daemon/oca/(协议层)。OCA 编译单元零依赖 SessionManager
//  头文件/类型,接口从 OCA 需求倒推,不暴露 AES67 daemon StreamSource/StreamSink
//  等非 OCA 协议层内部类型。唯一实现在 daemon 层 oca_session_manager_bridge.cpp
//  (daemon/ 根,与 config/session_manager/driver_manager 同级)。

#ifndef OCA_OCA_AUDIO_BRIDGE_HPP_
#define OCA_OCA_AUDIO_BRIDGE_HPP_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace oca {

class OcaAudioBridge {
 public:
  virtual ~OcaAudioBridge() = default;

  // ── PTP 时钟 ──
  struct PtpConfig {
    uint8_t domain;
    uint8_t dscp;
  };
  enum class PtpLockState { Unlocked, Locking, Locked };
  struct PtpStatus {
    PtpLockState lock;
    std::string gmid;
    int32_t jitter;
  };

  virtual PtpConfig get_ptp_config() const = 0;
  virtual bool set_ptp_config(PtpConfig cfg) = 0;
  virtual PtpStatus get_ptp_status() const = 0;

  // ── 采样率 ──
  virtual uint32_t get_sample_rate() const = 0;
  virtual bool set_sample_rate(uint32_t hz) = 0;
  virtual std::vector<uint32_t> get_supported_sample_rates() const = 0;

  // ── Source (Talker) ──
  struct SourceInfo {
    uint8_t id;
    bool enabled;
    std::string name;
    std::string codec;  // "L16", "L24", "AM824"
    std::string address;
    uint8_t ttl;
    uint8_t payload_type;
    uint8_t dscp;
    bool refclk_ptp_traceable;
    uint32_t max_samples_per_packet;
    std::vector<uint8_t> map;  // 通道映射
  };

  virtual std::vector<SourceInfo> get_sources() const = 0;
  virtual bool add_source(const SourceInfo& s) = 0;
  virtual bool remove_source(uint8_t id) = 0;
  virtual std::string get_source_sdp(uint8_t id) const = 0;

  // ── Sink (Listener) ──
  struct SinkInfo {
    uint8_t id;
    std::string name;
    uint32_t delay;
    std::string source_url;
    std::string sdp;
    bool use_sdp;
    bool ignore_refclk_gmid;
    std::vector<uint8_t> map;
  };

  struct SinkStatus {
    bool seq_error;
    bool ssrc_error;
    bool pt_error;
    bool sac_error;
    bool receiving;
    bool muted;
  };

  virtual std::vector<SinkInfo> get_sinks() const = 0;
  virtual bool add_sink(const SinkInfo& s) = 0;
  virtual bool remove_sink(uint8_t id) = 0;
  virtual SinkStatus get_sink_status(uint8_t id) const = 0;

  // ── 网络信息 ──
  virtual std::string get_interface_name() const = 0;
  virtual std::string get_ip_addr() const = 0;
  virtual std::string get_mac_addr() const = 0;
  virtual std::string get_device_id()
      const = 0;  // node_id(OcaNetwork.IDAdvertised)

  // ── I/O 端口(从 driver 查询) ──
  virtual uint32_t get_input_channels() const = 0;
  virtual uint32_t get_output_channels() const = 0;

  // ── Observer(daemon → OCA 事件传播) ──
  using PtpObserver = std::function<void(const PtpStatus&)>;
  using SourceObserver = std::function<void(uint8_t id, bool added)>;
  using SinkObserver = std::function<void(uint8_t id, bool added)>;

  virtual void set_ptp_observer(PtpObserver cb) = 0;
  virtual void set_source_observer(SourceObserver cb) = 0;
  virtual void set_sink_observer(SinkObserver cb) = 0;
};

}  // namespace oca

#endif  // OCA_OCA_AUDIO_BRIDGE_HPP_

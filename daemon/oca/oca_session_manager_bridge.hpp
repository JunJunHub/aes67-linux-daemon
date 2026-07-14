//  oca_session_manager_bridge.hpp - OcaAudioBridge 的 SessionManager 实现
//
//  .hpp 仅前向声明 SessionManager/Config/DriverManager，零 include 传递;
//  .cpp include 完整头文件做类型转换。

#ifndef OCA_OCA_SESSION_MANAGER_BRIDGE_HPP_
#define OCA_OCA_SESSION_MANAGER_BRIDGE_HPP_

#include <memory>

#include "oca/oca_audio_bridge.hpp"

// 前向声明:OCA 编译单元不依赖 daemon 核心头文件
class SessionManager;
class Config;
class DriverManager;

namespace oca {

// SessionManager observer 永不注销(SessionManager 无 remove API)。
// 析构后若 SessionManager 仍触发 observer,lambda 持有的 weak_ptr 自我判定
// 失效后直接 no-op,避免对已析构 bridge 解引用。故 bridge 必须由 shared_ptr
// 持有且在 start() 注册 observer。
class OcaSessionManagerBridge
    : public OcaAudioBridge,
      public std::enable_shared_from_this<OcaSessionManagerBridge> {
 public:
  OcaSessionManagerBridge(std::shared_ptr<SessionManager> sm,
                          std::shared_ptr<Config> cfg,
                          std::shared_ptr<DriverManager> drv);
  ~OcaSessionManagerBridge() override;

  // 构造后由 shared_ptr 持有者调用:注册 SessionManager observer。
  // 用 weak_from_this() 捕获自我弱引用,析构后 observer 自动失效。
  void start();

  // 禁止拷贝/移动(持有 observer 回调)
  OcaSessionManagerBridge(const OcaSessionManagerBridge&) = delete;
  OcaSessionManagerBridge& operator=(const OcaSessionManagerBridge&) = delete;

  // OcaAudioBridge 实现
  PtpConfig get_ptp_config() const override;
  bool set_ptp_config(PtpConfig cfg) override;
  PtpStatus get_ptp_status() const override;
  uint32_t get_sample_rate() const override;
  bool set_sample_rate(uint32_t hz) override;
  std::vector<uint32_t> get_supported_sample_rates() const override;
  std::vector<SourceInfo> get_sources() const override;
  bool add_source(const SourceInfo& s) override;
  bool remove_source(uint8_t id) override;
  std::string get_source_sdp(uint8_t id) const override;
  std::vector<SinkInfo> get_sinks() const override;
  bool add_sink(const SinkInfo& s) override;
  bool remove_sink(uint8_t id) override;
  SinkStatus get_sink_status(uint8_t id) const override;
  std::string get_interface_name() const override;
  std::string get_ip_addr() const override;
  std::string get_mac_addr() const override;
  std::string get_device_id() const override;
  uint32_t get_input_channels() const override;
  uint32_t get_output_channels() const override;
  void set_ptp_observer(PtpObserver cb) override;
  void set_source_observer(SourceObserver cb) override;
  void set_sink_observer(SinkObserver cb) override;

 private:
  std::shared_ptr<SessionManager> sm_;
  std::shared_ptr<Config> cfg_;
  std::shared_ptr<DriverManager> drv_;
  PtpObserver ptp_cb_;
  SourceObserver source_cb_;
  SinkObserver sink_cb_;
};

}  // namespace oca

#endif  // OCA_OCA_SESSION_MANAGER_BRIDGE_HPP_

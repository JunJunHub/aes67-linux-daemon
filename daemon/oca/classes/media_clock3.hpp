//  classes/media_clock3.hpp - OcaMediaClock3 {1,2,15} v2
//
//  AES70-2018 替代已废弃 OcaMediaClock{1,2,6}。ONo=8193。
//  暴露 PTP 时钟状态与采样率。bridge 为 nullptr 时返回静态默认值。
//  DefLevel == classID.fieldCount == 3。

#ifndef OCA_CLASSES_MEDIA_CLOCK3_HPP_
#define OCA_CLASSES_MEDIA_CLOCK3_HPP_

#include "oca/oca_audio_bridge.hpp"
#include "oca/classes/agent.hpp"

namespace oca {

// OcaMediaClock3 {1,2,15} v2
class OcaMediaClock3 : public OcaAgent {
 public:
  explicit OcaMediaClock3(ONo ono, ONo owner_ono = 0)
      : OcaAgent(ono, owner_ono) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 2; }
  std::string role() const override { return "MediaClock3"; }
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;

  void set_bridge(OcaAudioBridge* bridge) { bridge_ = bridge; }

  // PTP 状态变化回调(由 bridge observer 触发)
  void on_ptp_status_changed(const OcaAudioBridge::PtpStatus& st);

 private:
  ExecResult handle_media_clock3(uint16_t methodIndex,
                                 ocp1::Reader& req,
                                 ocp1::Writer& rsp);
  OcaAudioBridge* bridge_ = nullptr;
};

}  // namespace oca

#endif  // OCA_CLASSES_MEDIA_CLOCK3_HPP_

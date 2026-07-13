//  classes/media_clock3.cpp - OcaMediaClock3 {1,2,15} v2 实现

#include "oca/classes/media_clock3.hpp"

#include "oca/methods.hpp"

namespace oca {

namespace {
const ClassIdentification kMc3ClassId = {{{1, 2, 15}}, 2};
}  // namespace

const ClassIdentification& OcaMediaClock3::class_id() const {
  return kMc3ClassId;
}

ExecResult OcaMediaClock3::exec(MethodID m,
                                ocp1::Reader& req,
                                ocp1::Writer& rsp,
                                Session& sess) {
  if (m.defLevel == methods::kDefLevelMediaClock3) {
    return handle_media_clock3(m.methodIndex, req, rsp);
  }
  return OcaAgent::exec(m, req, rsp, sess);  // DefLevel 1/2 -> OcaAgent
}

ExecResult OcaMediaClock3::handle_media_clock3(uint16_t idx,
                                               ocp1::Reader& req,
                                               ocp1::Writer& rsp) {
  switch (idx) {
    case methods::kMc3GetAvailability:
      // OcaMediaClockAvailability(u8):AVAILABLE=1
      rsp.u8(1);
      return {Status::OK, 1};
    case methods::kMc3SetAvailability:
      // Optional:NotImplemented
      return {Status::NotImplemented, 0};
    case methods::kMc3GetCurrentRate: {
      // OcaMediaClockRate = {u32 numerator, u32 denominator}
      // + OcaONo timeSourceONo
      uint32_t rate = bridge_ ? bridge_->get_sample_rate() : 48000;
      rsp.u32(rate);  // numerator
      rsp.u32(1);     // denominator
      rsp.u32(0);     // timeSourceONo (0=无外部源)
      return {Status::OK, 2};
    }
    case methods::kMc3SetCurrentRate: {
      // Optional:设置采样率 + emit PropertyChanged
      if (!bridge_)
        return {Status::NotImplemented, 0};
      if (req.remaining() < 8)
        return {Status::BadFormat, 0};
      uint32_t num = req.u32();
      (void)req.u32();  // denominator(忽略,仅用 numerator)
      if (bridge_->set_sample_rate(num)) {
        oca::ocp1::Writer vw;
        vw.u32(num);
        vw.u32(1);
        emit_property_changed(methods::kDefLevelMediaClock3,
                              methods::kMc3PropCurrentRate, vw.data(),
                              static_cast<uint16_t>(vw.size()));
        return {Status::OK, 0};
      }
      return {Status::ProcessingFailed, 0};
    }
    case methods::kMc3GetOffset:
      // OcaLiteTimeOffset = OcaUint64:0
      rsp.u64(0);
      return {Status::OK, 1};
    case methods::kMc3SetOffset:
      // Optional:NotImplemented
      return {Status::NotImplemented, 0};
    case methods::kMc3GetSupportedRates: {
      // Ocp1List<OcaMediaClockRate>
      auto rates = bridge_ ? bridge_->get_supported_sample_rates()
                           : std::vector<uint32_t>{44100, 48000,  88200,
                                                   96000, 176400, 192000};
      rsp.u16(static_cast<uint16_t>(rates.size()));
      for (auto r : rates) {
        rsp.u32(r);  // numerator
        rsp.u32(1);  // denominator
      }
      return {Status::OK, 1};
    }
    case methods::kMc3GetPTPStatus: {
      // Fitcan 私有方法(0x8002):返回 PTP 锁态/模式/偏移
      if (!bridge_)
        return {Status::NotImplemented, 0};
      auto ps = bridge_->get_ptp_status();
      // 自定义编码:u8 lockState + string gmid + i32 jitter
      rsp.u8(static_cast<uint8_t>(ps.lock));
      rsp.string(ps.gmid);
      rsp.i32(ps.jitter);
      return {Status::OK, 1};
    }
    default:
      return {Status::NotImplemented, 0};
  }
}

void OcaMediaClock3::on_ptp_status_changed(
    const OcaAudioBridge::PtpStatus& /*st*/) {
  // PTP 锁态变化可能伴随采样率变化,发射 CurrentRate PropertyChanged
  if (bridge_) {
    oca::ocp1::Writer vw;
    uint32_t rate = bridge_->get_sample_rate();
    vw.u32(rate);
    vw.u32(1);
    emit_property_changed(methods::kDefLevelMediaClock3,
                          methods::kMc3PropCurrentRate, vw.data(),
                          static_cast<uint16_t>(vw.size()));
  }
}

}  // namespace oca

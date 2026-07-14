//  classes/media_clock_manager.cpp - OcaMediaClockManager {1,3,7} v2 实现

#include "oca/classes/media_clock_manager.hpp"

#include "oca/methods.hpp"

namespace oca {

namespace {
const ClassIdentification kMcmClassId = {{{1, 3, 7}}, 2};
}  // namespace

const ClassIdentification& OcaMediaClockManager::class_id() const {
  return kMcmClassId;
}

ExecResult OcaMediaClockManager::exec(MethodID m,
                                      ocp1::Reader& req,
                                      ocp1::Writer& rsp,
                                      Session& sess) {
  if (m.defLevel == methods::kDefLevelMediaClockMngr) {
    switch (m.methodIndex) {
      case methods::kMcmGetClocks:
        // GetClocks:返回 Ocp1List<OcaONo> = 空列表(废弃 Clock,用 Clock3)
        rsp.u16(0);
        return {Status::OK, 1};
      case methods::kMcmGetMediaClockTypesSupported:
        // Mandatory:返空 List<OcaMediaClockType>(设备用 Clock3,无 legacy type)
        rsp.u16(0);
        return {Status::OK, 1};
      case methods::kMcmGetClock3s:
        // GetClock3s:返回 Ocp1List<OcaONo> = [8193]
        rsp.u16(1);
        rsp.u32(8193);
        return {Status::OK, 1};
      default:
        return {Status::NotImplemented, 0};
    }
  }
  return OcaManager::exec(m, req, rsp, sess);  // DefLevel 1 -> OcaRoot
}

}  // namespace oca

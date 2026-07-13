//  classes/media_clock.cpp - OcaMediaClock {1,2,6} v2 (废弃存根) 实现

#include "oca/classes/media_clock.hpp"

#include "oca/methods.hpp"

namespace oca {

namespace {
const ClassIdentification kMcClassId = {{{1, 2, 6}}, 2};
}  // namespace

const ClassIdentification& OcaMediaClock::class_id() const {
  return kMcClassId;
}

ExecResult OcaMediaClock::exec(MethodID m,
                               ocp1::Reader& req,
                               ocp1::Writer& rsp,
                               Session& sess) {
  if (m.defLevel == methods::kDefLevelMediaClock) {
    // 废弃类:所有方法 NotImplemented
    return {Status::NotImplemented, 0};
  }
  return OcaAgent::exec(m, req, rsp, sess);  // DefLevel 1/2 -> OcaAgent
}

}  // namespace oca

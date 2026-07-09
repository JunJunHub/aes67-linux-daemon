//  classes/root.cpp - Root/Worker/Manager/Block 实现

#include "oca/classes/root.hpp"

#include "oca/methods.hpp"
#include "oca/session.hpp"

namespace oca {

namespace {
const ClassIdentification kBlockClassId = {{{1, 1, 3}}, 2};
}  // namespace

const ClassIdentification& OcaBlock::class_id() const {
  return kBlockClassId;
}

Status OcaRoot::handle_root(uint16_t idx, ocp1::Writer& rsp) {
  switch (idx) {
    case methods::kRootGetClassIdentification: {
      const auto& ci = class_id();
      rsp.u16(static_cast<uint16_t>(ci.classID.levels.size()));
      for (auto lvl : ci.classID.levels)
        rsp.u16(lvl);
      rsp.u16(class_version());
      return Status::OK;
    }
    case methods::kRootGetLockable:
      rsp.u8(0);  // Spec1 不实现锁定 -> 不可锁
      return Status::OK;
    case methods::kRootGetRole:
      rsp.string(role());
      return Status::OK;
    default:
      return Status::BadMethod;
  }
}

Status OcaRoot::exec(MethodID m,
                     ocp1::Reader& req,
                     ocp1::Writer& rsp,
                     Session& sess) {
  if (m.defLevel == methods::kDefLevelRoot) {
    return handle_root(m.methodIndex, rsp);
  }
  return Status::BadMethod;
}

Status OcaBlock::exec(MethodID m,
                      ocp1::Reader& req,
                      ocp1::Writer& rsp,
                      Session& sess) {
  if (m.defLevel == methods::kDefLevelBlock) {
    switch (m.methodIndex) {
      case methods::kBlockGetMembers:
        return GetMembers(rsp, sess);
      default:
        return Status::BadMethod;
    }
  }
  return OcaWorker::exec(m, req, rsp, sess);  // 委托 DefLevel 1 -> OcaRoot
}

Status OcaBlock::GetMembers(ocp1::Writer& rsp, Session& sess) {
  auto* reg = sess.registry();
  if (!reg)
    return Status::DeviceError;
  auto objs = reg->objects_in_range(1, 99);  // 管理器成员
  rsp.u16(static_cast<uint16_t>(objs.size()));
  for (auto* o : objs)
    rsp.u32(o->ono());
  return Status::OK;
}

}  // namespace oca

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

ExecResult OcaRoot::handle_root(uint16_t idx, ocp1::Writer& rsp) {
  switch (idx) {
    case methods::kRootGetClassIdentification: {
      // OcaClassIdentification = 1 个结构化参数(ClassID + version)
      const auto& ci = class_id();
      rsp.u16(static_cast<uint16_t>(ci.classID.levels.size()));
      for (auto lvl : ci.classID.levels)
        rsp.u16(lvl);
      rsp.u16(class_version());
      return {Status::OK, 1};
    }
    case methods::kRootGetLockable:
      rsp.u8(0);  // Spec1 不实现锁定 -> 不可锁
      return {Status::OK, 1};
    case methods::kRootGetRole:
      rsp.string(role());
      return {Status::OK, 1};
    case methods::kRootLock:
      // daemon 不可锁(GetLockable=0),Lock/Unlock 为 no-op;返回 OK
      return {Status::OK, 0};
    case methods::kRootUnlock:
      return {Status::OK, 0};
    default:
      return {Status::NotImplemented, 0};
  }
}

ExecResult OcaRoot::exec(MethodID m,
                         ocp1::Reader& req,
                         ocp1::Writer& rsp,
                         Session& sess) {
  if (m.defLevel == methods::kDefLevelRoot) {
    return handle_root(m.methodIndex, rsp);
  }
  return {Status::BadMethod, 0};
}

ExecResult OcaWorker::exec(MethodID m,
                           ocp1::Reader& req,
                           ocp1::Writer& rsp,
                           Session& sess) {
  if (m.defLevel ==
      methods::kDefLevelManager) {  // OcaWorker defLevel 2 ({1,1})
    return handle_worker(m.methodIndex, req, rsp);
  }
  return OcaRoot::exec(m, req, rsp, sess);  // 委托 DefLevel 1 -> OcaRoot
}

ExecResult OcaWorker::handle_worker(uint16_t idx,
                                    ocp1::Reader& req,
                                    ocp1::Writer& rsp) {
  switch (idx) {
    case methods::kWorkerGetEnabled:
      rsp.u8(1);  // Boolean: enabled(daemon 始终启用)
      return {Status::OK, 1};
    case methods::kWorkerSetEnabled:
      // 工具探测可能发空体(paramBytes=0)或 1 字节 Boolean;忽略输入,no-op。
      if (req.remaining() >= 1)
        (void)req.u8();
      return {Status::OK, 0};
    case methods::kWorkerGetPorts:
      rsp.u16(0);  // 空 Ocp1List<OcaPort>(根块无端口)
      return {Status::OK, 1};
    case methods::kWorkerGetLabel:
      // OcaString:返回 role() 作为标签
      rsp.string(role());
      return {Status::OK, 1};
    case methods::kWorkerSetLabel:
      // 读可选 OcaString,no-op。
      if (req.remaining() >= 2)
        (void)req.string();
      return {Status::OK, 0};
    case methods::kWorkerGetOwner:
      // ONo:含有块的对象号。根块=0(无 owner)。
      rsp.u32(owner_ono_);
      return {Status::OK, 1};
    case methods::kWorkerGetPath:
      // OcaPath 结构复杂,YAGNI。
      return {Status::NotImplemented, 0};
    default:
      return {Status::NotImplemented, 0};
  }
}

ExecResult OcaBlock::exec(MethodID m,
                          ocp1::Reader& req,
                          ocp1::Writer& rsp,
                          Session& sess) {
  if (m.defLevel == methods::kDefLevelBlock) {
    switch (m.methodIndex) {
      case methods::kBlockGetMembers:
        return GetMembers(rsp, sess);
      case methods::kBlockGetMembersRecursive:
        return GetMembersRecursive(rsp, sess);
      default:
        return {Status::NotImplemented, 0};
    }
  }
  return OcaWorker::exec(m, req, rsp, sess);  // 委托 DefLevel 1 -> OcaRoot
}

ExecResult OcaBlock::GetMembers(ocp1::Writer& rsp, Session& sess) {
  auto* reg = sess.registry();
  if (!reg)
    return {Status::DeviceError, 0};
  auto mgrs = reg->objects_in_range(1, 99);  // 管理器成员
  auto cm =
      reg->objects_in_range(4096, 65535);  // BlockMember 区段(CM3 网络对象)
  // Ocp1List<OcaObjectIdentification>,每个元素 = {ONo, ClassIdentification}
  // ClassIdentification = {ClassID(fieldCount+levels), ClassVersion}
  rsp.u16(static_cast<uint16_t>(mgrs.size() + cm.size()));
  for (auto* o : mgrs) {
    rsp.u32(o->ono());
    const auto& ci = o->class_id();
    rsp.u16(static_cast<uint16_t>(ci.classID.levels.size()));
    for (auto lvl : ci.classID.levels)
      rsp.u16(lvl);
    rsp.u16(o->class_version());
  }
  for (auto* o : cm) {
    rsp.u32(o->ono());
    const auto& ci = o->class_id();
    rsp.u16(static_cast<uint16_t>(ci.classID.levels.size()));
    for (auto lvl : ci.classID.levels)
      rsp.u16(lvl);
    rsp.u16(o->class_version());
  }
  return {Status::OK, 1};  // 成员 ObjectIdentification 列表 = 1 个参数
}

ExecResult OcaBlock::GetMembersRecursive(ocp1::Writer& rsp, Session& sess) {
  auto* reg = sess.registry();
  if (!reg)
    return {Status::DeviceError, 0};
  // 根块直系成员 = 管理器 [1,99] + BlockMember 区段 [4096,65535](CM3
  // 网络对象)。 根块自身 ONo=100 不列(合规工具 GetObjects 已单独 Add 根块)。
  auto mgrs = reg->objects_in_range(1, 99);
  auto cm = reg->objects_in_range(4096, 65535);
  // Ocp1List<OcaBlockMember>,每元素 = {OcaObjectIdentification, ContainerONo}
  // OcaObjectIdentification = {ONo u32, ClassIdentification}
  // ClassIdentification = {ClassID(fieldCount u16 + levels u16*), ClassVersion
  // u16} ContainerONo = u32(本块 ONo,根块=100)
  rsp.u16(static_cast<uint16_t>(mgrs.size() + cm.size()));
  for (auto* o : mgrs) {
    rsp.u32(o->ono());
    const auto& ci = o->class_id();
    rsp.u16(static_cast<uint16_t>(ci.classID.levels.size()));
    for (auto lvl : ci.classID.levels)
      rsp.u16(lvl);
    rsp.u16(o->class_version());
    rsp.u32(ono());  // ContainerONo = 根块 ONo(100)
  }
  for (auto* o : cm) {
    rsp.u32(o->ono());
    const auto& ci = o->class_id();
    rsp.u16(static_cast<uint16_t>(ci.classID.levels.size()));
    for (auto lvl : ci.classID.levels)
      rsp.u16(lvl);
    rsp.u16(o->class_version());
    rsp.u32(ono());
  }
  return {Status::OK, 1};
}

}  // namespace oca

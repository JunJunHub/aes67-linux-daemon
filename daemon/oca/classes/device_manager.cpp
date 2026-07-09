//  classes/device_manager.cpp

#include "oca/classes/device_manager.hpp"

#include "oca/methods.hpp"
#include "oca/session.hpp"  // Session 完整类型(registry())

namespace oca {

namespace {
const ClassIdentification kDeviceManagerClassId = {{{1, 2, 1}}, 4};
}  // namespace

const ClassIdentification& OcaDeviceManager::class_id() const {
  return kDeviceManagerClassId;
}

Status OcaDeviceManager::exec(MethodID m,
                              ocp1::Reader& req,
                              ocp1::Writer& rsp,
                              Session& sess) {
  if (m.defLevel == methods::kDefLevelDeviceMngr) {
    switch (m.methodIndex) {
      case methods::kDevGetOcaVersion:
        return GetOcaVersion(rsp);
      case methods::kDevGetSerialNumber:
        return GetSerialNumber(rsp);
      case methods::kDevGetDeviceName:
        return GetDeviceName(rsp);
      case methods::kDevGetModelDescription:
        return GetModelDescription(rsp);
      case methods::kDevGetState:
        return GetState(rsp);
      case methods::kDevGetManagers:
        return GetManagers(rsp, sess);
      default:
        return Status::BadMethod;
    }
  }
  return OcaManager::exec(m, req, rsp, sess);  // DefLevel 1 -> OcaRoot
}

Status OcaDeviceManager::GetOcaVersion(ocp1::Writer& rsp) {
  rsp.u16(methods::kProtocolVersion);  // OCA version = 1 (AES70-2023)
  return Status::OK;
}

Status OcaDeviceManager::GetSerialNumber(ocp1::Writer& rsp) {
  rsp.string(identity_.serial_number);
  return Status::OK;
}

Status OcaDeviceManager::GetDeviceName(ocp1::Writer& rsp) {
  rsp.string(identity_.device_name);
  return Status::OK;
}

Status OcaDeviceManager::GetModelDescription(ocp1::Writer& rsp) {
  // OcaModelDescription = {Manufacturer: string, Name: string, Version: string}
  rsp.string(identity_.manufacturer);
  rsp.string(identity_.model_name);
  rsp.string(identity_.model_version);
  return Status::OK;
}

Status OcaDeviceManager::GetState(ocp1::Writer& rsp) {
  rsp.u8(static_cast<uint8_t>(
      DeviceState::Operational));  // Spec1 总是 Operational
  return Status::OK;
}

Status OcaDeviceManager::GetManagers(ocp1::Writer& rsp, Session& sess) {
  auto* reg = sess.registry();
  if (!reg)
    return Status::DeviceError;
  auto objs = reg->objects_in_range(1, 99);
  // Ocp1List<OcaManagerDescriptor>,ManagerDescriptor={ONo, Name(string=Role),
  // ClassIdentification}
  rsp.u16(static_cast<uint16_t>(objs.size()));
  for (auto* o : objs) {
    rsp.u32(o->ono());
    rsp.string(o->role());  // Name = 对象 Role(OcaRoot::role())
    const auto& ci = o->class_id();
    rsp.u16(static_cast<uint16_t>(ci.classID.levels.size()));
    for (auto lvl : ci.classID.levels)
      rsp.u16(lvl);
    rsp.u16(o->class_version());
  }
  return Status::OK;
}

}  // namespace oca

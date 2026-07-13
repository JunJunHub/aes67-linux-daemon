//  classes/device_manager.cpp

#include "oca/classes/device_manager.hpp"

#include "oca/methods.hpp"
#include "oca/session.hpp"  // Session 完整类型(registry())

namespace oca {

namespace {
const ClassIdentification kDeviceManagerClassId = {{{1, 3, 1}}, 2};
}  // namespace

const ClassIdentification& OcaDeviceManager::class_id() const {
  return kDeviceManagerClassId;
}

ExecResult OcaDeviceManager::exec(MethodID m,
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
      case methods::kDevGetModelGUID:
        return GetModelGUID(rsp);
      case methods::kDevGetEnabled:
        return GetEnabled(rsp);
      case methods::kDevSetEnabled:
        return SetEnabled(req);
      case methods::kDevGetDeviceRevisionID:
        return GetDeviceRevisionID(rsp);
      case methods::kDevGetManufacturer:
        return GetManufacturer(rsp);
      case methods::kDevGetProduct:
        return GetProduct(rsp);
      case methods::kDevGetState:
        return GetState(rsp);
      case methods::kDevGetOperationalState:
        return GetOperationalState(rsp);
      case methods::kDevGetManagers:
        return GetManagers(rsp, sess);
      default:
        return {Status::NotImplemented, 0};
    }
  }
  return OcaManager::exec(m, req, rsp, sess);  // DefLevel 1 -> OcaRoot
}

ExecResult OcaDeviceManager::GetOcaVersion(ocp1::Writer& rsp) {
  rsp.u16(methods::kProtocolVersion);  // OCA version = 1 (AES70-2023)
  return {Status::OK, 1};
}

ExecResult OcaDeviceManager::GetSerialNumber(ocp1::Writer& rsp) {
  rsp.string(identity_.serial_number);
  return {Status::OK, 1};
}

ExecResult OcaDeviceManager::GetDeviceName(ocp1::Writer& rsp) {
  rsp.string(identity_.device_name);
  return {Status::OK, 1};
}

ExecResult OcaDeviceManager::GetModelDescription(ocp1::Writer& rsp) {
  // OcaModelDescription = 1 个结构化参数(Manufacturer+Name+Version)
  rsp.string(identity_.manufacturer);
  rsp.string(identity_.model_name);
  rsp.string(identity_.model_version);
  return {Status::OK, 1};
}

ExecResult OcaDeviceManager::GetModelGUID(ocp1::Writer& rsp) {
  // OcaModelGUID = BlobFixedLen<1>(reserved)+BlobFixedLen<3>(mfrCode)
  //                +BlobFixedLen<4>(modelCode) = 8 原始字节,无长度前缀
  // (OCAMicro OcaLiteModelGUID::Marshal)。最小写 8 零字节(未定义厂商,合规)。
  rsp.u64(0);
  return {Status::OK, 1};  // 1 个结构化参数
}

ExecResult OcaDeviceManager::GetEnabled(ocp1::Writer& rsp) {
  // Spec4:返真存储 enabled_(默认 true)。OCAMicro GET_ENABLED 响应
  // NrParameters=1。
  rsp.u8(enabled_ ? 1 : 0);
  return {Status::OK, 1};
}

ExecResult OcaDeviceManager::SetEnabled(ocp1::Reader& req) {
  // Spec4:真存储 enabled_ + 触发 PropertyChanged。空体探测(paramBytes=0)
  // 时 no-op 返回 OK,不破坏 Spec1 回归。
  if (req.remaining() >= 1) {
    uint8_t v = req.u8();  // OcaBoolean
    enabled_ = (v != 0);
    emit_property_changed(methods::kDefLevelDeviceMngr /*DevMgr 引入级*/,
                          methods::kPropEnabled, &v, 1);
  }
  return {Status::OK, 0};
}

ExecResult OcaDeviceManager::GetDeviceRevisionID(ocp1::Writer& rsp) {
  // sphinx 3.20:返 OcaString(daemon 版本号)。deprecated v3(被 GetProduct
  // 取代)。
  rsp.string(identity_.model_version);
  return {Status::OK, 1};
}

ExecResult OcaDeviceManager::GetManufacturer(ocp1::Writer& rsp) {
  // sphinx 3.21(2023 Mandatory G4):返 OcaManufacturer。
  // OcaManufacturer = Name(OcaString) + OrganizationID(OcaBlobFixedLen<3> = 3
  // 原始字节,0=未定义) + Website(OcaString) + BusinessContact(OcaString)
  // + TechnicalContact(OcaString)。后三联系字段无值,空串合规。
  rsp.string(identity_.manufacturer);  // Name
  rsp.u8(0);               // OrganizationID 3 零字节(未定义 OUI)
  rsp.u16(0);              //   (BlobFixedLen<3>,无长度前缀)
  rsp.string("");          // Website
  rsp.string("");          // BusinessContact
  rsp.string("");          // TechnicalContact
  return {Status::OK, 1};  // 1 个结构化参数
}

ExecResult OcaDeviceManager::GetProduct(ocp1::Writer& rsp) {
  // sphinx 3.22(2023 Mandatory G3):返 OcaProduct。
  // OcaProduct = Name + ModelID + RevisionLevel + BrandName + UUID(OcaString)
  // + Description,共 6 个 OcaString。UUID 空串合规(未分配,OcaUUID=OcaString)。
  rsp.string(identity_.model_name);     // Name
  rsp.string(identity_.model_name);     // ModelID(无独立值,用 model_name)
  rsp.string(identity_.model_version);  // RevisionLevel
  rsp.string(identity_.manufacturer);   // BrandName
  rsp.string("");                       // UUID(空)
  rsp.string("");                       // Description
  return {Status::OK, 1};               // 1 个结构化参数
}

ExecResult OcaDeviceManager::GetState(ocp1::Writer& rsp) {
  rsp.u8(static_cast<uint8_t>(
      DeviceState::Operational));  // Spec1 总是 Operational
  return {Status::OK, 1};
}

ExecResult OcaDeviceManager::GetOperationalState(ocp1::Writer& rsp) {
  // OcaDeviceOperationalState = {Generic(u8) + Details(OcaBlob)}
  // Generic = OcaDeviceGenericState::NormalOperation(0); Details = 空 blob
  rsp.u8(0);               // NormalOperation
  rsp.u16(0);              // 空 OcaBlob Details
  return {Status::OK, 1};  // 1 个结构化参数
}

ExecResult OcaDeviceManager::GetManagers(ocp1::Writer& rsp, Session& sess) {
  auto* reg = sess.registry();
  if (!reg)
    return {Status::DeviceError, 0};
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
  return {Status::OK, 1};  // 管理器描述符列表 = 1 个参数
}

}  // namespace oca

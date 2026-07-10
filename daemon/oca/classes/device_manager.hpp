//  classes/device_manager.hpp - OcaDeviceManager + 设备身份

#ifndef OCA_CLASSES_DEVICE_MANAGER_HPP_
#define OCA_CLASSES_DEVICE_MANAGER_HPP_

#include <string>

#include "oca/classes/root.hpp"  // OcaManager

namespace oca {

// 由 OcaServer 从 Config 构造,解耦 DeviceManager 与 Config
struct OcaDeviceIdentity {
  std::string manufacturer = "AES67-Linux-Daemon";
  std::string model_name;     // oca_model,空则取 daemon 版本号
  std::string model_version;  // daemon 版本号
  std::string serial_number;  // oca_serial_number,空则取 node_id
  std::string device_name;    // oca_device_name,空则取 node_id
};

class OcaDeviceManager : public OcaManager {
 public:
  OcaDeviceManager(ONo ono, const OcaDeviceIdentity& identity)
      : OcaManager(ono), identity_(identity) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 2; }
  std::string role() const override { return "DeviceManager"; }
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;

 private:
  ExecResult GetOcaVersion(ocp1::Writer& rsp);
  ExecResult GetSerialNumber(ocp1::Writer& rsp);
  ExecResult GetDeviceName(ocp1::Writer& rsp);
  ExecResult GetModelDescription(ocp1::Writer& rsp);
  ExecResult GetModelGUID(ocp1::Writer& rsp);
  ExecResult GetEnabled(ocp1::Writer& rsp);
  ExecResult SetEnabled(ocp1::Reader& req);
  ExecResult GetDeviceRevisionID(ocp1::Writer& rsp);
  ExecResult GetState(ocp1::Writer& rsp);
  ExecResult GetOperationalState(ocp1::Writer& rsp);
  ExecResult GetManagers(ocp1::Writer& rsp, Session& sess);

  OcaDeviceIdentity identity_;
};

}  // namespace oca

#endif  // OCA_CLASSES_DEVICE_MANAGER_HPP_

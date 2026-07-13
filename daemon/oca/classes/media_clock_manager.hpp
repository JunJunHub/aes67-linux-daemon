//  classes/media_clock_manager.hpp - OcaMediaClockManager {1,3,7} v2
//
//  AES70-2018 时钟集合管理器。ONo=7(AES70 固定)。
//  GetClocks→空(废弃 Clock)、GetClock3s→[8193]。
//  DefLevel == classID.fieldCount == 3。

#ifndef OCA_CLASSES_MEDIA_CLOCK_MANAGER_HPP_
#define OCA_CLASSES_MEDIA_CLOCK_MANAGER_HPP_

#include "oca/classes/root.hpp"

namespace oca {

// OcaMediaClockManager {1,3,7} v2
class OcaMediaClockManager : public OcaManager {
 public:
  explicit OcaMediaClockManager(ONo ono) : OcaManager(ono) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 2; }
  std::string role() const override { return "MediaClockManager"; }
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;
};

}  // namespace oca

#endif  // OCA_CLASSES_MEDIA_CLOCK_MANAGER_HPP_

//  classes/media_clock.hpp - OcaMediaClock {1,2,6} v2 (废弃存根)
//
//  DeprecatedSince AES70-2018;合规工具仍有检查项。
//  所有方法返回 NotImplemented。ONo=8194。
//  DefLevel == classID.fieldCount == 3。

#ifndef OCA_CLASSES_MEDIA_CLOCK_HPP_
#define OCA_CLASSES_MEDIA_CLOCK_HPP_

#include "oca/classes/agent.hpp"

namespace oca {

// OcaMediaClock {1,2,6} v2 (废弃存根)
class OcaMediaClock : public OcaAgent {
 public:
  explicit OcaMediaClock(ONo ono, ONo owner_ono = 0)
      : OcaAgent(ono, owner_ono) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 2; }
  std::string role() const override { return "MediaClock"; }
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;
};

}  // namespace oca

#endif  // OCA_CLASSES_MEDIA_CLOCK_HPP_

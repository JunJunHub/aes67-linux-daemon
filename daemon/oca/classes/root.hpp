//  classes/root.hpp - OcaRoot/Worker/Manager/Block 继承层次

#ifndef OCA_CLASSES_ROOT_HPP_
#define OCA_CLASSES_ROOT_HPP_

#include <string>

#include "oca/object.hpp"

namespace oca {

// OcaRoot {1,1} v2:DefLevel 1 基础方法
class OcaRoot : public Object {
 public:
  using Object::Object;  // 继承 ONo 构造
  Status exec(MethodID m,
              ocp1::Reader& req,
              ocp1::Writer& rsp,
              Session& sess) override;
  virtual std::string role() const { return {}; }

 protected:
  Status handle_root(uint16_t methodIndex, ocp1::Writer& rsp);
};

// OcaWorker {1,1,1} v2:Spec1 无自有 DefLevel-2 方法,委托 OcaRoot
class OcaWorker : public OcaRoot {
 public:
  using OcaRoot::OcaRoot;
};

// OcaManager {1,2} v2:Spec1 无自有 DefLevel-2 方法,委托 OcaRoot
class OcaManager : public OcaRoot {
 public:
  using OcaRoot::OcaRoot;
};

// OcaBlock {1,1,3} v2:DefLevel 3(Root Block, ONo 100)
class OcaBlock : public OcaWorker {
 public:
  explicit OcaBlock(ONo ono) : OcaWorker(ono) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 2; }
  std::string role() const override { return "Root Block"; }
  Status exec(MethodID m,
              ocp1::Reader& req,
              ocp1::Writer& rsp,
              Session& sess) override;

 private:
  Status GetMembers(ocp1::Writer& rsp, Session& sess);
};

}  // namespace oca

#endif  // OCA_CLASSES_ROOT_HPP_

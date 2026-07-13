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
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;
  virtual std::string role() const { return {}; }

 protected:
  ExecResult handle_root(uint16_t methodIndex, ocp1::Writer& rsp);
};

// OcaWorker {1,1,1} v2:Spec1 无自有 DefLevel-2 方法,委托 OcaRoot
// Spec3:test4 对根块(OcaWorker 子类)强制 GetEnabled/SetEnabled/GetPorts,
// 补 DefLevel-2 分派。
class OcaWorker : public OcaRoot {
 public:
  explicit OcaWorker(ONo ono, ONo owner_ono = 0)
      : OcaRoot(ono), owner_ono_(owner_ono) {}
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;

 protected:
  // OcaWorker DefLevel-2 强制方法:最小返值合规。
  // GetEnabled->Boolean(1);SetEnabled 读 Boolean 返 0 params;GetPorts->空
  // List。
  ExecResult handle_worker(uint16_t methodIndex,
                           ocp1::Reader& req,
                           ocp1::Writer& rsp);
  ONo owner_ono_ = 0;
};

// OcaManager {1,2} v2:Spec1 无自有 DefLevel-2 方法,委托 OcaRoot
class OcaManager : public OcaRoot {
 public:
  using OcaRoot::OcaRoot;
};

// OcaBlock {1,1,3} v2:DefLevel 3(Root Block, ONo 100)
class OcaBlock : public OcaWorker {
 public:
  explicit OcaBlock(ONo ono, ONo owner_ono = 0) : OcaWorker(ono, owner_ono) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 2; }
  std::string role() const override { return "Root Block"; }
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;

 private:
  ExecResult GetMembers(ocp1::Writer& rsp, Session& sess);
  // 返回根块直系成员的 OcaBlockMember 列表(含 ContainerONo=100)。
  // Spec3 Fix-A:实装此方法使合规工具 GetObjects 走非覆盖分支,
  // 保留根块 {1,1,3}@100 进 deviceReportedObjects(见 Spec3 根因)。
  ExecResult GetMembersRecursive(ocp1::Writer& rsp, Session& sess);
};

}  // namespace oca

#endif  // OCA_CLASSES_ROOT_HPP_

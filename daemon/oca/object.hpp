//  object.hpp - OCA 对象抽象基类 + ObjectRegistry

#ifndef OCA_OBJECT_HPP_
#define OCA_OBJECT_HPP_

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "oca/ocp1.hpp"
#include "oca/types.hpp"

namespace oca {

class Session;  // 前置声明,定义在 session.hpp

// exec 返回:OCA 状态码 + 响应参数个数(NrParameters,AES70-3 §9)
struct ExecResult {
  Status status = Status::OK;
  uint8_t nrParameters = 0;
};

class Object {
 public:
  virtual ~Object() = default;
  ONo ono() const { return ono_; }
  virtual const ClassIdentification& class_id() const = 0;
  virtual uint16_t class_version() const = 0;
  virtual ExecResult exec(MethodID method,
                          ocp1::Reader& req,
                          ocp1::Writer& rsp,
                          Session& sess) = 0;
  // OcaRoot::GetRole 的默认实现:OCA 对象均有 Role,基类默认空串。
  // 置于 Object 以便注册表持有的 Object* 可取 Role(GetManagers 描述符 Name)。
  virtual std::string role() const { return {}; }

 protected:
  explicit Object(ONo ono) : ono_(ono) {}
  ONo ono_;
};

class ObjectRegistry {
 public:
  void register_object(std::unique_ptr<Object> obj);
  Object* find(ONo ono) const;
  // 返回 [from, to] 闭区间内按 ONo 升序的对象指针
  std::vector<Object*> objects_in_range(ONo from, ONo to) const;
  size_t size() const { return objects_.size(); }

 private:
  std::unordered_map<ONo, std::unique_ptr<Object>> objects_;
};

inline void ObjectRegistry::register_object(std::unique_ptr<Object> obj) {
  ONo ono = obj->ono();
  objects_.emplace(ono, std::move(obj));
}

inline Object* ObjectRegistry::find(ONo ono) const {
  auto it = objects_.find(ono);
  return it == objects_.end() ? nullptr : it->second.get();
}

inline std::vector<Object*> ObjectRegistry::objects_in_range(ONo from,
                                                             ONo to) const {
  std::vector<Object*> out;
  for (ONo o = from; o <= to; ++o) {
    if (auto* p = find(o))
      out.push_back(p);
  }
  return out;
}

}  // namespace oca

#endif  // OCA_OBJECT_HPP_

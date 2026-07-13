# AES70/OCA Spec4 — PropertyChanged 发射与端到端投递验证 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 daemon 从"订阅能成功但永不发通知"补全为"attributable setter 真触发 PropertyChanged 通知，且端到端可验证"。

**Architecture:** OcaRoot 持有 `OcaSubscriptionManager*` 事件总线引用(方案 C)，发射 session 无关。SetLabel(OcaAgent/OcaWorker/OcaApplicationNetwork 三类)+ DeviceManager.SetEnabled 从 no-op 改为真存储 label_/enabled_ + 调 `emit_property_changed` 触发 PropertyChanged 通知，data 负载 = PropertyID{声明类 defLevel, 类属性表 propertyIndex=1} + 已编码新值。oca-test +3 用例(单元/端到端/对称)、oca-probe 新增 PropertyChanged 探测段。

**Tech Stack:** C++17、Boost.Test、OCP.1 over TCP、CMake(WITH_OCA 选项，隔离在 `daemon/oca/`)、oca-dev.sh(out-of-source 构建 `daemon/build/`)。

## Global Constraints

- 注释/文档/提交信息优先中文，API 名/标准术语保留英文原文。
- 提交信息末尾加 `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`。
- 精准修改：仅改本计划涉及的代码，不顺手重构相邻代码；格式由 `.clang-format`(Chromium 风格、2 空格、左指针)统一，不重排既有 include。
- 构建/测试用 `oca-dev.sh`(out-of-source `daemon/build/`)；oca-test 二进制路径 `daemon/build/tests/oca-test`，oca-probe 路径 `daemon/build/oca-probe`。
- AES70 符合性优先：PropertyID 与 methodIndex 是独立命名空间，不得混用。
- SetLabel 空体探测保护保留(剩余字节 < 2 时 no-op 返回 OK)，不破坏 Spec1 回归。
- 不改 CMakeLists.txt(无新源文件)。

## 设计依据

完整设计见 `docs/superpowers/specs/aes70-oca-spec4-design.md`。关键事实(已读源码核实)：

- `trigger_event` 当前**零生产调用点**，只在 `tests/oca_test.cpp` 三处被测试伸手调用。
- 投递管线已完整：`trigger_event`(subscription_manager.cpp:244) → `Session::enqueue_notification`(session.hpp:38) → `Transport::conn_loop` drain(transport.cpp:228-231，每条 PDU 后 + EAGAIN 空闲态)。
- `OcaRoot` 是所有对象共同基类(Spec3 后 Object → OcaRoot → OcaAgent/OcaWorker/OcaAppNet/OcaManager → 各具体类)，在 OcaRoot 一处注入 emitter 即覆盖全部对象，不改构造签名。
- OcaServer 构造(oca_server.cpp:29-46)顺序：new 五对象 → register_object → 构造 Transport。注入点在 line 46(ctrl_net register)之后、line 48(transport 构造)之前。
- 现有 Spec3 单元测试发空体 SetLabel 且不经 OcaServer(emitter=nullptr) → 真存改造后仍 silent，断言不破。
- DeviceManager.GetEnabled 现硬编码 u8(1)(device_manager.cpp:91)；改 enabled_ 默认 true 仍返 1。

---

## File Structure

| 文件 | 职责(本计划涉及部分) |
|------|---------------------|
| `daemon/oca/classes/root.hpp` | OcaRoot 新增 `emitter_` + `set_event_emitter`/`event_emitter` + `emit_property_changed` 声明；OcaWorker 新增 `label_` 成员 |
| `daemon/oca/classes/root.cpp` | `emit_property_changed` 实现；handle_worker 的 GetLabel@8/SetLabel@9 改真存+emit |
| `daemon/oca/classes/agent.hpp` | OcaAgent 新增 `label_` 成员 |
| `daemon/oca/classes/agent.cpp` | handle_agent 的 GetLabel@1/SetLabel@2 改真存+emit |
| `daemon/oca/classes/application_network.hpp` | OcaApplicationNetwork 新增 `label_` 成员 |
| `daemon/oca/classes/application_network.cpp` | handle_appnet 的 GetLabel@1/SetLabel@2 改真存+emit |
| `daemon/oca/classes/device_manager.hpp` | OcaDeviceManager 新增 `enabled_` 成员(默认 true) |
| `daemon/oca/classes/device_manager.cpp` | SetEnabled 真存+emit；GetEnabled 返 enabled_ |
| `daemon/oca/methods.hpp` | 新增 `kPropLabel=1`、`kPropEnabled=1` |
| `daemon/oca/oca_server.cpp` | 注册完对象后统一注入 `set_event_emitter(sub_mgr_)` |
| `daemon/oca/tests/oca_test.cpp` | 新增 3 个用例 |
| `daemon/oca/tools/oca_probe.cpp` | 新增 PropertyChanged 探测段 + `--no-pc` 参数 |
| `docs/ops/oca-design-and-maintenance.md` | 维护手册同步 Spec4 |

任务依赖链：Task 1(常量) → Task 2(OcaRoot emitter + emit helper) → Task 3(三类 SetLabel 真存) + Task 4(DeviceManager SetEnabled 真存，可与 Task 3 并行) → Task 5(OcaServer 注入) → Task 6(单元测试 label emit) → Task 7(单元测试 enabled emit) → Task 8(端到端测试) → Task 9(oca-probe 扩展) → Task 10(维护手册 + 最终验收)。

---

### Task 1: methods.hpp 新增 PropertyIndex 常量

**Files:**
- Modify: `daemon/oca/methods.hpp:135`(在 `kEventOperationalState` 后、`kProtocolVersion` 前插入)

**Interfaces:**
- Produces: `oca::methods::kPropLabel` (uint16_t=1)、`oca::methods::kPropEnabled` (uint16_t=1)，供 Task 3/4 的 emit 调用点引用。

- [ ] **Step 1: 新增常量**

在 `daemon/oca/methods.hpp` 第 135 行(`constexpr uint16_t kEventOperationalState = 1;` 之后、`// ProtocolVersion (AES70-2023)` 之前)插入以下**新增**行(上方 `kEventPropertyChanged`/`kEventOperationalState` 与下方 `kProtocolVersion` 行均已存在,仅作定位锚点,勿重复插入):

```cpp
// Spec4:PropertyChanged 通知负载中的 PropertyID.propertyIndex(AES70
// OcaPropertyID = {声明类 defLevel, 类属性表下标};propertyIndex 与 methodIndex
// 独立命名空间)。Label/Enabled 均为各类首个可报变属性。
constexpr uint16_t kPropLabel = 1;    // OcaAgent/OcaWorker/OcaAppNet 的 Label
constexpr uint16_t kPropEnabled = 1;  // OcaDeviceManager 的 Enabled
```

- [ ] **Step 2: 构建验证常量编译通过**

Run: `./oca-dev.sh build`
Expected: 构建成功(无新错误；常量未被引用，仅编译)。

- [ ] **Step 3: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/oca/methods.hpp && git commit -m "feat(oca): Spec4 - methods.hpp 新增 kPropLabel/kPropEnabled 属性索引常量

PropertyID 按 AES70 规范(声明类 defLevel + 类属性表 propertyIndex)，
与 methodIndex 独立命名空间。Label/Enabled 为各类首个可报变属性。

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: OcaRoot 注入事件总线 + emit_property_changed helper

**Files:**
- Modify: `daemon/oca/classes/root.hpp:13-24`(OcaRoot 类)、`daemon/oca/classes/root.cpp:1-53`

**Interfaces:**
- Consumes: `OcaSubscriptionManager::trigger_event(ONo, EventID, const uint8_t*, uint16_t)`(subscription_manager.hpp:28，已存在)、`oca::ocp1::Writer`、`oca::methods::kDefLevelRoot`/`kEventPropertyChanged`。
- Produces: `OcaRoot::set_event_emitter(OcaSubscriptionManager*)`、`OcaRoot::event_emitter() const`、`OcaRoot::emit_property_changed(uint16_t prop_def_level, uint16_t prop_index, const uint8_t* value_data, uint16_t value_count)`(protected)。供 Task 3/4 的 setter 调用、Task 5 的注入循环调用。

- [ ] **Step 1: 修改 root.hpp —— OcaRoot 增加 emitter 与 emit_property_changed**

在 `daemon/oca/classes/root.hpp` 顶部 include 区添加(在 `#include "oca/object.hpp"` 之后)：

```cpp
#include <string>

#include "oca/object.hpp"

namespace oca {

class OcaSubscriptionManager;  // 前置声明，避免头循环
```

> 注：`<string>` 已在 line 6 存在；只需新增前置声明 `class OcaSubscriptionManager;`，插在 `namespace oca {` 之后、`class OcaRoot` 之前。

修改 OcaRoot 类(line 13-24)为：

```cpp
// OcaRoot {1,1} v2:DefLevel 1 基础方法
class OcaRoot : public Object {
 public:
  using Object::Object;  // 继承 ONo 构造
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;
  virtual std::string role() const { return {}; }

  // Spec4:注入事件总线(PropertyChanged 发射用)。默认 nullptr，
  // setter 在真存储属性后经此触发;为空时该对象不发通知(如纯只读 Manager
  // 或单测中未经 OcaServer 构造的对象)。
  void set_event_emitter(OcaSubscriptionManager* em) { emitter_ = em; }
  OcaSubscriptionManager* event_emitter() const { return emitter_; }

 protected:
  ExecResult handle_root(uint16_t methodIndex, ocp1::Writer& rsp);
  // Spec4:编码并触发 PropertyChanged 通知。data 负载 = PropertyID{u16,u16}
  // + 已编码属性值。emitter_ 为空时静默(只读对象 / 未注入)。
  void emit_property_changed(uint16_t prop_def_level, uint16_t prop_index,
                             const uint8_t* value_data, uint16_t value_count);
  OcaSubscriptionManager* emitter_ = nullptr;
};
```

- [ ] **Step 2: 修改 root.cpp —— 实现 emit_property_changed**

在 `daemon/oca/classes/root.cpp` 顶部 include 区(line 6 `#include "oca/session.hpp"` 之后)新增：

```cpp
#include "oca/methods.hpp"
#include "oca/ocp1.hpp"
#include "oca/session.hpp"
#include "oca/classes/subscription_manager.hpp"  // Spec4:trigger_event 完整类型
```

> 注：`oca/methods.hpp` 与 `oca/session.hpp` 已在 line 5-6；新增 `oca/ocp1.hpp`(为 Writer)与 `subscription_manager.hpp`。

在 `OcaRoot::handle_root` 之前(line 18 之前，`OcaBlock::class_id` 之后)新增 emit_property_changed 实现：

```cpp
void OcaRoot::emit_property_changed(uint16_t prop_def_level,
                                    uint16_t prop_index,
                                    const uint8_t* value_data,
                                    uint16_t value_count) {
  if (!emitter_)
    return;
  // 负载 = PropertyID{u16 defLevel, u16 propertyIndex} + 已编码属性值
  oca::ocp1::Writer w;
  w.u16(prop_def_level);
  w.u16(prop_index);
  for (uint16_t i = 0; i < value_count; ++i)
    w.u8(value_data[i]);
  // PropertyChanged 事件 = OcaRoot event {kDefLevelRoot, kEventPropertyChanged}
  emitter_->trigger_event(ono(),
                          {methods::kDefLevelRoot, methods::kEventPropertyChanged},
                          w.data(), static_cast<uint16_t>(w.size()));
}
```

- [ ] **Step 3: 构建验证**

Run: `./oca-dev.sh build`
Expected: 构建成功。emit_property_changed 尚未被调用，但不影响编译。

- [ ] **Step 4: 跑现有 oca-test 确保未破坏**

Run: `./oca-dev.sh test`
Expected: 31/31 全绿(未改任何 exec 行为，仅新增成员与函数)。

- [ ] **Step 5: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/oca/classes/root.hpp daemon/oca/classes/root.cpp && git commit -m "feat(oca): Spec4 - OcaRoot 注入事件总线 + emit_property_changed helper

OcaRoot 持有 OcaSubscriptionManager* emitter_(默认 nullptr)，
发射 session 无关(符合 AES70 发射/投递解耦语义)。emit_property_changed
拼 PropertyID + 已编码属性值 buffer 后调 trigger_event。

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: OcaAgent / OcaWorker / OcaApplicationNetwork 的 SetLabel 真存 + emit

**Files:**
- Modify: `daemon/oca/classes/agent.hpp:17-32`、`daemon/oca/classes/agent.cpp:19-42`
- Modify: `daemon/oca/classes/root.hpp:29-46`(OcaWorker 新增 `label_`)
- Modify: `daemon/oca/classes/root.cpp:66-100`(handle_worker GetLabel@8/SetLabel@9)
- Modify: `daemon/oca/classes/application_network.hpp:17-32`、`daemon/oca/classes/application_network.cpp:19-50`

**Interfaces:**
- Consumes: Task 1 的 `kPropLabel`、Task 2 的 `emit_property_changed`、`oca::ocp1::Writer::string`/`Reader::string`、`oca::ocp1::Reader::remaining`。
- Produces: 三类 GetLabel 返回 `label_.empty()?role():label_`；SetLabel 真存 `label_` + emit PropertyID{2,kPropLabel}+OcaString。

- [ ] **Step 1: agent.hpp —— OcaAgent 新增 label_ 成员**

修改 `daemon/oca/classes/agent.hpp` 的 OcaWorker/Agent protected 区(line 26-32)为：

```cpp
 protected:
  // OcaAgent DefLevel-2 方法:GetLabel/SetLabel/GetOwner/GetPath。
  ExecResult handle_agent(uint16_t methodIndex,
                          ocp1::Reader& req,
                          ocp1::Writer& rsp);
  ONo owner_ono_;
  std::string label_;  // Spec4:Label 真存储(空则 GetLabel 回退 role())
};
```

> 需 `#include <string>`；agent.hpp 经 `oca/classes/root.hpp` 已传递包含 string(root.hpp line 6)。无需额外 include。

- [ ] **Step 2: agent.cpp —— handle_agent GetLabel/SetLabel 真存+emit**

修改 `daemon/oca/classes/agent.cpp` 的 handle_agent(line 19-42)，替换 kAgentGetLabel 与 kAgentSetLabel 两个 case：

```cpp
ExecResult OcaAgent::handle_agent(uint16_t idx,
                                  ocp1::Reader& req,
                                  ocp1::Writer& rsp) {
  switch (idx) {
    case methods::kAgentGetLabel:
      // Spec4:已 SetLabel 则返 label_,否则回退 role()
      rsp.string(label_.empty() ? role() : label_);
      return {Status::OK, 1};
    case methods::kAgentSetLabel: {
      // Spec4:真存储 label_ + 触发 PropertyChanged。空体探测(无完整
      // OcaString)时 no-op 返回 OK,不破坏 Spec1 回归。
      if (req.remaining() < 2)  // Ocp1List 最少长度 2 字节
        return {Status::OK, 0};
      std::string v = req.string();
      label_ = v;
      oca::ocp1::Writer vw;
      vw.string(v);
      emit_property_changed(methods::kDefLevelManager /*Agent 引入级*/,
                            methods::kPropLabel, vw.data(),
                            static_cast<uint16_t>(vw.size()));
      return {Status::OK, 0};
    }
    case methods::kAgentGetOwner:
      // ONo:含有块的对象号。Network/CtrlNet=100(根块),根块自身=0。
      rsp.u32(owner_ono_);
      return {Status::OK, 1};
    case methods::kAgentGetPath:
      // OcaPath 结构复杂(OcaPathStep 列表),YAGNI。
      return {Status::NotImplemented, 0};
    default:
      return {Status::NotImplemented, 0};
  }
}
```

> `kDefLevelManager`=2 即 Agent{1,2} 的 defLevel(fieldCount=2)，符合"声明类 defLevel"。

- [ ] **Step 3: root.hpp —— OcaWorker 新增 label_ 成员**

修改 `daemon/oca/classes/root.hpp` 的 OcaWorker protected 区(line 38-46)为：

```cpp
 protected:
  // OcaWorker DefLevel-2 强制方法:最小返值合规。
  // GetEnabled->Boolean(1);SetEnabled 读 Boolean 返 0 params;GetPorts->空
  // List。
  ExecResult handle_worker(uint16_t methodIndex,
                           ocp1::Reader& req,
                           ocp1::Writer& rsp);
  ONo owner_ono_ = 0;
  std::string label_;  // Spec4:Label 真存储(空则 GetLabel 回退 role())
};
```

- [ ] **Step 4: root.cpp —— handle_worker GetLabel@8/SetLabel@9 真存+emit**

修改 `daemon/oca/classes/root.cpp` 的 handle_worker(line 66-100)，替换 kWorkerGetLabel 与 kWorkerSetLabel 两个 case(其余 case 不动)：

```cpp
    case methods::kWorkerGetLabel:
      // Spec4:已 SetLabel 则返 label_,否则回退 role()
      rsp.string(label_.empty() ? role() : label_);
      return {Status::OK, 1};
    case methods::kWorkerSetLabel: {
      // Spec4:真存储 label_ + 触发 PropertyChanged。空体探测时 no-op。
      if (req.remaining() < 2)
        return {Status::OK, 0};
      std::string v = req.string();
      label_ = v;
      oca::ocp1::Writer vw;
      vw.string(v);
      emit_property_changed(methods::kDefLevelManager /*Worker 引入级*/,
                            methods::kPropLabel, vw.data(),
                            static_cast<uint16_t>(vw.size()));
      return {Status::OK, 0};
    }
```

- [ ] **Step 5: application_network.hpp —— OcaAppNet 新增 label_ 成员**

修改 `daemon/oca/classes/application_network.hpp` 的 protected 区(line 26-32)为：

```cpp
 protected:
  // OcaApplicationNetwork DefLevel-2 方法。
  ExecResult handle_appnet(uint16_t methodIndex,
                           ocp1::Reader& req,
                           ocp1::Writer& rsp);
  ONo owner_ono_;
  std::string label_;  // Spec4:Label 真存储(空则 GetLabel 回退 role())
};
```

- [ ] **Step 6: application_network.cpp —— handle_appnet GetLabel/SetLabel 真存+emit**

修改 `daemon/oca/classes/application_network.cpp` 的 handle_appnet(line 19-50)，替换 kAppNetGetLabel 与 kAppNetSetLabel 两个 case(其余 case 不动)：

```cpp
    case methods::kAppNetGetLabel:
      // Spec4:已 SetLabel 则返 label_,否则回退 role()
      rsp.string(label_.empty() ? role() : label_);
      return {Status::OK, 1};
    case methods::kAppNetSetLabel: {
      // Spec4:真存储 label_ + 触发 PropertyChanged。空体探测时 no-op。
      if (req.remaining() < 2)
        return {Status::OK, 0};
      std::string v = req.string();
      label_ = v;
      oca::ocp1::Writer vw;
      vw.string(v);
      emit_property_changed(methods::kDefLevelManager /*AppNet 引入级*/,
                            methods::kPropLabel, vw.data(),
                            static_cast<uint16_t>(vw.size()));
      return {Status::OK, 0};
    }
```

- [ ] **Step 7: 构建验证**

Run: `./oca-dev.sh build`
Expected: 构建成功。

- [ ] **Step 8: 跑现有 oca-test 确认 Spec3 单元断言未破**

Run: `./oca-dev.sh test`
Expected: 31/31 全绿。关键回归点：
- `dispatch_agent_methods`：GetLabel 返回 "Network"(label_ 空→role())✓；SetLabel 空体→OK+0 参 ✓
- `dispatch_appnet_methods`：GetLabel 返回 "Control Network" ✓
- `dispatch_worker_label_owner`：GetLabel 返回 "Root Block" ✓
> 这些单元测试直接 new 对象未注入 emitter，SetLabel 空体走 no-op 分支，断言不变。

- [ ] **Step 9: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/oca/classes/agent.hpp daemon/oca/classes/agent.cpp daemon/oca/classes/root.hpp daemon/oca/classes/root.cpp daemon/oca/classes/application_network.hpp daemon/oca/classes/application_network.cpp && git commit -m "feat(oca): Spec4 - OcaAgent/OcaWorker/OcaAppNet SetLabel 真存储 + 触发 PropertyChanged

三类 SetLabel 从 no-op 改为真存 label_ + emit_property_changed
(PropertyID{Agent/Worker/AppNet defLevel=2, kPropLabel=1} + OcaString)。
GetLabel 改为 label_.empty()?role():label_,空体探测保护保留。
单元测试未经 OcaServer(emitter=nullptr)且发空体,silent,Spec3 断言不破。

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: OcaDeviceManager SetEnabled 真存 + emit

**Files:**
- Modify: `daemon/oca/classes/device_manager.hpp:21-49`
- Modify: `daemon/oca/classes/device_manager.cpp:88-102`

**Interfaces:**
- Consumes: Task 1 的 `kPropEnabled`、Task 2 的 `emit_property_changed`、`methods::kDefLevelDeviceMngr`、`oca::ocp1::Reader::u8`/`remaining`。
- Produces: DeviceManager.GetEnabled 返 `enabled_`(默认 true)；SetEnabled 真存 `enabled_` + emit PropertyID{3,kPropEnabled}+OcaBoolean(u8)。

- [ ] **Step 1: device_manager.hpp —— 新增 enabled_ 成员**

修改 `daemon/oca/classes/device_manager.hpp` 的 private 成员区(line 48 附近)为：

```cpp
 private:
  ExecResult GetOcaVersion(ocp1::Writer& rsp);
  ExecResult GetSerialNumber(ocp1::Writer& rsp);
  ExecResult GetDeviceName(ocp1::Writer& rsp);
  ExecResult GetModelDescription(ocp1::Writer& rsp);
  ExecResult GetModelGUID(ocp1::Writer& rsp);
  ExecResult GetEnabled(ocp1::Writer& rsp);
  ExecResult SetEnabled(ocp1::Reader& req);
  ExecResult GetDeviceRevisionID(ocp1::Writer& rsp);
  ExecResult GetManufacturer(ocp1::Writer& rsp);
  ExecResult GetProduct(ocp1::Writer& rsp);
  ExecResult GetState(ocp1::Writer& rsp);
  ExecResult GetOperationalState(ocp1::Writer& rsp);
  ExecResult GetManagers(ocp1::Writer& rsp, Session& sess);

  OcaDeviceIdentity identity_;
  bool enabled_ = true;  // Spec4:Enabled 真存储,SetEnabled 触发 PropertyChanged
};
```

> 需 `#include <string>`(已存在 line 6)。

- [ ] **Step 2: device_manager.cpp —— SetEnabled/GetEnabled 真存+emit**

修改 `daemon/oca/classes/device_manager.cpp` 的 GetEnabled(line 88-93)与 SetEnabled(line 95-102)：

```cpp
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
```

- [ ] **Step 3: 构建验证**

Run: `./oca-dev.sh build`
Expected: 构建成功。

- [ ] **Step 4: 跑现有 oca-test 确认 DeviceManager 断言未破**

Run: `./oca-dev.sh test`
Expected: 31/31 全绿。关键回归点：
- `dispatch_device_manager`：GetEnabled 返回 1(enabled_ 默认 true)✓；SetEnabled 空体→OK+0 参 ✓；SetEnabled u8(1)→OK+0 参 ✓
- `oca_e2e_acceptance`：GetEnabled 经真实 socket 仍返 1 ✓
> DeviceManager 在单测中直接 new(不经 OcaServer)→ emitter=nullptr → SetEnabled 真存 enabled_ 但 emit silent，断言不变。

- [ ] **Step 5: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/oca/classes/device_manager.hpp daemon/oca/classes/device_manager.cpp && git commit -m "feat(oca): Spec4 - OcaDeviceManager SetEnabled 真存储 + 触发 PropertyChanged

SetEnabled 从 no-op 改为真存 enabled_ + emit_property_changed
(PropertyID{DevMgr defLevel=3, kPropEnabled=1} + OcaBoolean u8)。
GetEnabled 返 enabled_(默认 true)。空体探测保护保留,Spec1 回归不破。

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: OcaServer 注册完对象后统一注入事件总线

**Files:**
- Modify: `daemon/oca/oca_server.cpp:40-48`

**Interfaces:**
- Consumes: Task 2 的 `OcaRoot::set_event_emitter`、`ObjectRegistry::objects_in_range`(object.hpp:48)、`sub_mgr_`(oca_server.hpp:44)。
- Produces: 所有已注册对象的 emitter_ 指向 sub_mgr_，使端到端 setter 能真触发(Task 8/9 依赖)。

- [ ] **Step 1: oca_server.cpp 注入循环**

在 `daemon/oca/oca_server.cpp` 的构造函数中，line 46(`registry_.register_object(std::unique_ptr<Object>(ctrl_net));`)之后、line 48(`transport_ = std::make_unique<Transport>(...)`)之前插入：

```cpp
  registry_.register_object(std::unique_ptr<Object>(net2));
  registry_.register_object(std::unique_ptr<Object>(ctrl_net));

  // Spec4:为所有已注册对象注入事件总线,使 attributable setter
  // (SetLabel/SetEnabled)能触发 PropertyChanged。发射经对象成员 emitter_
  // 调用,session 无关;投递由 trigger_event 内部按各订阅者 Session* 入队。
  // objects_in_range(1, 9999) 覆盖管理器[1,99]、根块 100、CM3 网络[4096,...]。
  for (auto* obj : registry_.objects_in_range(1, 9999))
    static_cast<OcaRoot*>(obj)->set_event_emitter(sub_mgr_);

  transport_ = std::make_unique<Transport>(&registry_, sub_mgr_);
```

> **类型转换说明**:`objects_in_range` 返回 `std::vector<Object*>`,而 `set_event_emitter` 声明在 `OcaRoot`(非 `Object`)。需 `static_cast<OcaRoot*>(obj)` 向下转换。安全前提:所有经 OcaServer 注册的对象(DeviceManager/NetworkManager/SubscriptionManager 经 OcaManager→OcaRoot,Root Block 经 OcaWorker→OcaRoot,OcaNetwork 经 OcaAgent→OcaRoot,OcaControlNetwork 经 OcaApplicationNetwork→OcaRoot)均为 OcaRoot 子类,且 `Object` 含虚函数为多态基类,向下转换合法。该不变量由 OcaServer 构造独占维护(测试中 StubObject 等非 OcaRoot 子类不经此路径)。
> 需确认 oca_server.cpp 已 include 能见到 `OcaRoot::set_event_emitter`(经 root.hpp)。oca_server.cpp line 5-9 已 include 各 class 头(device_manager/network/control_network/root 等),root.hpp 经这些传递可见。无需额外 include。
> `sub_mgr_` 在 line 40 已赋值(`sub_mgr_ = sm;`),注入循环在其后,安全。

- [ ] **Step 2: 构建验证**

Run: `./oca-dev.sh build`
Expected: 构建成功。

- [ ] **Step 3: 跑 oca-test 确认 e2e 未破**

Run: `./oca-dev.sh test`
Expected: 31/31 全绿。注入循环不影响单测(单测不经 OcaServer)，但 `oca_server_facade` 与 `oca_e2e_acceptance` 经 OcaServer → 现在所有对象 emitter 非空。e2e_acceptance 的 trigger_event 直接触发路径不变；SetEnabled 空体仍 no-op。应仍全绿。

- [ ] **Step 4: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/oca/oca_server.cpp && git commit -m "feat(oca): Spec4 - OcaServer 注册完对象后统一注入事件总线

遍历 registry_.objects_in_range(1,9999) 调 set_event_emitter(sub_mgr_),
使所有对象 emitter_ 指向 SubscriptionManager。发射 session 无关,
为将来 daemon 自发事件(PTP/链路)留通路。

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: oca-test 新增 dispatch_property_changed_label_emit 单元用例

**Files:**
- Modify: `daemon/oca/tests/oca_test.cpp`(在 `dispatch_worker_label_owner` 用例之后、`dispatch_device_manager` 用例之前，约 line 675 后插入)

**Interfaces:**
- Consumes: Task 2/3 的 `set_event_emitter`、`emit_property_changed`(经 SetLabel)、`OcaSubscriptionManager::exec` AddPropertyChangeSubscription2、`Session::take_notification`、`parse_notifications2`。
- Produces: 验证对象级发射链(单元内手动注入 emitter)。

- [ ] **Step 1: 新增测试用例**

在 `daemon/oca/tests/oca_test.cpp` 的 `dispatch_worker_label_owner` 用例结束(line 675 `}`)之后、`dispatch_device_manager` 用例(line 677)之前插入：

```cpp
BOOST_AUTO_TEST_CASE(dispatch_property_changed_label_emit) {
  // Spec4:对象级 PropertyChanged 发射链验证。
  // 构造 OcaNetwork(4097,继承 OcaAgent)+ 真实 SubscriptionManager,
  // 单元内手动注入 emitter(模拟 OcaServer 注入),SetLabel 后断言:
  //   - Notification2 PDU 入队,emitterONo=4097,eventID={1,1}
  //   - data = PropertyID{2,1} + OcaString == "hello"
  //   - GetLabel 回读 == "hello"(label_ 已真存)
  //   - emitter=nullptr 时 silent(无 ntf)
  oca::OcaSubscriptionManager sm(4);
  oca::OcaNetwork net(4097, 100);
  net.set_event_emitter(&sm);  // 手动注入
  oca::Session sess(1);
  oca::ObjectRegistry reg;
  sess.set_registry(&reg);
  namespace m = oca::methods;

  // 1) AddPropertyChangeSubscription2(target=4, emitter=4097, PropertyID{2,1})
  //    sphinx §3.10:EmitterONo + PropertyID{u16,u16} + DeliveryMode + NetAddr
  oca::ocp1::Writer reqw;
  reqw.u32(4097);                 // EmitterONo
  reqw.u16(m::kDefLevelManager);  // PropertyID defLevel (Agent 引入级=2)
  reqw.u16(m::kPropLabel);        // PropertyID propertyIndex (Label=1)
  reqw.u8(1);                     // NotificationDeliveryMode = Normal
  reqw.u16(0);                    // 空 NetworkAddress
  oca::ocp1::Reader req(reqw.data(), reqw.size());
  oca::ocp1::Writer rspw;
  auto st = sm.exec({m::kDefLevelSubMngr, m::kSubAddPropertyChangeSubscription2},
                    req, rspw, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK(sess.has_subscription(
      4097, {m::kDefLevelRoot, m::kEventPropertyChanged}));

  // 2) SetLabel(Agent defLevel=2, kAgentSetLabel=2, OcaString="hello")
  //    -> 真存 label_ + emit PropertyChanged
  oca::ocp1::Writer labw;
  labw.string("hello");
  oca::ocp1::Reader labreq(labw.data(), labw.size());
  oca::ocp1::Writer setrsp;
  st = net.exec({m::kDefLevelManager, m::kAgentSetLabel}, labreq, setrsp, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 0);

  // 3) 取 Notification2 PDU 并解析
  std::vector<uint8_t> pdu;
  BOOST_REQUIRE(sess.take_notification(pdu));
  BOOST_REQUIRE(pdu.size() > 10);
  BOOST_CHECK_EQUAL(pdu[0], 0x3B);
  auto hdr =
      oca::ocp1::PduReader::try_parse_header(pdu.data() + 1, pdu.size() - 1);
  BOOST_REQUIRE(hdr);
  BOOST_CHECK_EQUAL(hdr->pduType, m::kPduNtf2);
  auto ntfs = oca::ocp1::PduReader::parse_notifications2(
      pdu.data() + 1 + 9, pdu.size() - 1 - 9, hdr->messageCount);
  BOOST_REQUIRE_EQUAL(ntfs.size(), 1u);
  BOOST_CHECK_EQUAL(ntfs[0].emitterONo, 4097u);
  BOOST_CHECK_EQUAL(ntfs[0].eventID.defLevel, m::kDefLevelRoot);
  BOOST_CHECK_EQUAL(ntfs[0].eventID.eventIndex, m::kEventPropertyChanged);
  // data = PropertyID{u16,u16} + OcaString
  oca::ocp1::Reader dr(ntfs[0].data, ntfs[0].dataCount);
  BOOST_CHECK_EQUAL(dr.u16(), m::kDefLevelManager);  // PropertyID defLevel=2
  BOOST_CHECK_EQUAL(dr.u16(), m::kPropLabel);        // PropertyID propertyIndex=1
  BOOST_CHECK_EQUAL(dr.string(), "hello");
  BOOST_CHECK(!sess.take_notification(pdu));  // 队列已空

  // 4) GetLabel 回读 == "hello"(label_ 已真存)
  oca::ocp1::Reader empty(nullptr, 0);
  oca::ocp1::Writer getrsp;
  st = net.exec({m::kDefLevelManager, m::kAgentGetLabel}, empty, getrsp, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(getrsp.data(), getrsp.size()).string(),
                    "hello");

  // 5) emitter=nullptr 时 SetLabel silent(无 ntf)
  net.set_event_emitter(nullptr);
  oca::ocp1::Reader labreq2(labw.data(), labw.size());
  oca::ocp1::Writer setrsp2;
  st = net.exec({m::kDefLevelManager, m::kAgentSetLabel}, labreq2, setrsp2,
                sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK(!sess.take_notification(pdu));  // 无新通知
}
```

- [ ] **Step 2: 构建验证**

Run: `./oca-dev.sh build`
Expected: 构建成功。

- [ ] **Step 3: 跑新用例确认通过**

Run: `daemon/build/tests/oca-test -p -t dispatch_property_changed_label_emit`
Expected: PASS(1 个用例通过)。

- [ ] **Step 4: 跑全量确认未破坏**

Run: `./oca-dev.sh test`
Expected: 32/32 全绿(31 + 本用例)。

- [ ] **Step 5: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/oca/tests/oca_test.cpp && git commit -m "test(oca): Spec4 - 新增 dispatch_property_changed_label_emit 单元用例

验证对象级发射链:SetLabel 真存 + emit -> Notification2 入队,
解析 PropertyID{2,1}+OcaString,GetLabel 回读,emitter=nullptr silent。

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: oca-test 新增 dispatch_property_changed_enabled_emit 单元用例

**Files:**
- Modify: `daemon/oca/tests/oca_test.cpp`(在 Task 6 新增的用例之后插入)

**Interfaces:**
- Consumes: Task 2/4 的 `set_event_emitter`、DeviceManager.SetEnabled/GetEnabled、`OcaSubscriptionManager::exec` AddPropertyChangeSubscription2、`parse_notifications2`。
- Produces: 对称验证 DeviceManager SetEnabled 发射链(emitter=1,PropertyID{3,1}+OcaBoolean)。

- [ ] **Step 1: 新增测试用例**

在 Task 6 新增的 `dispatch_property_changed_label_emit` 用例之后插入：

```cpp
BOOST_AUTO_TEST_CASE(dispatch_property_changed_enabled_emit) {
  // Spec4:DeviceManager SetEnabled 发射链,对称于 label emit 用例。
  // emitter=1(DevMgr ONo),PropertyID{3,1} + OcaBoolean(u8)。
  oca::OcaDeviceIdentity id;
  id.manufacturer = "Acme";
  id.model_name = "AES67-daemon";
  id.model_version = "bondagit-3.1.0";
  oca::OcaDeviceManager dm(1, id);
  oca::OcaSubscriptionManager sm(4);
  dm.set_event_emitter(&sm);
  oca::Session sess(1);
  oca::ObjectRegistry reg;
  sess.set_registry(&reg);
  namespace m = oca::methods;

  // 默认 enabled_=true -> GetEnabled=1
  oca::ocp1::Reader empty(nullptr, 0);
  oca::ocp1::Writer genrsp;
  auto st =
      dm.exec({m::kDefLevelDeviceMngr, m::kDevGetEnabled}, empty, genrsp, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(genrsp.data(), genrsp.size()).u8(), 1u);

  // 订阅 PropertyChanged(emitter=1, PropertyID{3,1})
  oca::ocp1::Writer reqw;
  reqw.u32(1);                       // EmitterONo = DevMgr
  reqw.u16(m::kDefLevelDeviceMngr);  // PropertyID defLevel=3
  reqw.u16(m::kPropEnabled);         // PropertyID propertyIndex=1
  reqw.u8(1);                        // DeliveryMode Normal
  reqw.u16(0);                       // 空 NetworkAddress
  oca::ocp1::Reader req(reqw.data(), reqw.size());
  oca::ocp1::Writer rspw;
  st = sm.exec({m::kDefLevelSubMngr, m::kSubAddPropertyChangeSubscription2}, req,
               rspw, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK(sess.has_subscription(
      1, {m::kDefLevelRoot, m::kEventPropertyChanged}));

  // SetEnabled(u8=0)->真存 enabled_=false + emit
  oca::ocp1::Writer setw;
  setw.u8(0);
  oca::ocp1::Reader setreq(setw.data(), setw.size());
  oca::ocp1::Writer setrsp;
  st = dm.exec({m::kDefLevelDeviceMngr, m::kDevSetEnabled}, setreq, setrsp,
               sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 0);

  // 解析 Notification2:data = PropertyID{3,1} + OcaBoolean u8
  std::vector<uint8_t> pdu;
  BOOST_REQUIRE(sess.take_notification(pdu));
  auto hdr =
      oca::ocp1::PduReader::try_parse_header(pdu.data() + 1, pdu.size() - 1);
  BOOST_REQUIRE(hdr);
  BOOST_CHECK_EQUAL(hdr->pduType, m::kPduNtf2);
  auto ntfs = oca::ocp1::PduReader::parse_notifications2(
      pdu.data() + 1 + 9, pdu.size() - 1 - 9, hdr->messageCount);
  BOOST_REQUIRE_EQUAL(ntfs.size(), 1u);
  BOOST_CHECK_EQUAL(ntfs[0].emitterONo, 1u);
  oca::ocp1::Reader dr(ntfs[0].data, ntfs[0].dataCount);
  BOOST_CHECK_EQUAL(dr.u16(), m::kDefLevelDeviceMngr);
  BOOST_CHECK_EQUAL(dr.u16(), m::kPropEnabled);
  BOOST_CHECK_EQUAL(dr.u8(), 0u);
  BOOST_CHECK(!sess.take_notification(pdu));

  // GetEnabled 回读 == 0(enabled_ 已真存为 false)
  oca::ocp1::Writer genrsp2;
  st = dm.exec({m::kDefLevelDeviceMngr, m::kDevGetEnabled}, empty, genrsp2,
               sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(genrsp2.data(), genrsp2.size()).u8(), 0u);
}
```

- [ ] **Step 2: 构建验证**

Run: `./oca-dev.sh build`
Expected: 构建成功。

- [ ] **Step 3: 跑新用例确认通过**

Run: `daemon/build/tests/oca-test -p -t dispatch_property_changed_enabled_emit`
Expected: PASS。

- [ ] **Step 4: 跑全量确认**

Run: `./oca-dev.sh test`
Expected: 33/33 全绿。

- [ ] **Step 5: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/oca/tests/oca_test.cpp && git commit -m "test(oca): Spec4 - 新增 dispatch_property_changed_enabled_emit 单元用例

对称验证 DeviceManager SetEnabled 发射链:emitter=1,
PropertyID{3,1}+OcaBoolean,GetEnabled 回读 == 0。

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 8: oca-test 新增 oca_e2e_property_changed 端到端用例

**Files:**
- Modify: `daemon/oca/tests/oca_test.cpp`(在 `oca_e2e_acceptance` 用例之后、`transport_empty_body_probe_keeps_connection` 之前，约 line 1608 后插入)

**Interfaces:**
- Consumes: Task 2-5 的完整发射链(经 OcaServer 注入的 emitter)、`oca_e2e_acceptance` 的 sendPdu/recvPdu/cmd helper 模式。
- Produces: Spec4 里程碑验收——真实 socket 经真实 SetLabel 命令触发端到端闭环。

- [ ] **Step 1: 新增端到端用例**

在 `daemon/oca/tests/oca_test.cpp` 的 `oca_e2e_acceptance` 用例结束(line 1608 `}`)之后、`transport_empty_body_probe_keeps_connection`(line 1610)之前插入：

```cpp
BOOST_AUTO_TEST_CASE(oca_e2e_property_changed) {
  // Spec4 里程碑:全真实 socket 端到端 PropertyChanged 投递。
  // 经真实 SetLabel 命令触发(不伸手 trigger_event),验证:
  //   订阅 -> SetLabel -> transport drain -> Notification2 上线 -> 解析
  // 与 oca_e2e_acceptance 区别:后者 OperationalState 经测试代码直接触发,
  // 本用例 PropertyChanged 经真实 setter 触发,证明发射点已接入生产路径。
  oca::OcaServerConfig cfg;
  cfg.port = 0;
  cfg.node_id = "AES67 daemon pc";
  cfg.daemon_version = "bondagit-3.1.0";
  oca::OcaServer server(cfg);
  BOOST_REQUIRE(server.start());
  uint16_t port = server.port();

  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  BOOST_REQUIRE(
      ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

  auto sendPdu = [&](const std::vector<uint8_t>& p) {
    BOOST_REQUIRE_EQUAL(::send(sock, p.data(), p.size(), 0), (ssize_t)p.size());
  };
  auto recvPdu = [&](std::vector<uint8_t>& out) -> bool {
    uint8_t sync;
    if (::recv(sock, &sync, 1, 0) != 1 || sync != 0x3B)
      return false;
    uint8_t hdr[9];
    size_t got = 0;
    while (got < 9) {
      ssize_t r = ::recv(sock, hdr + got, 9 - got, 0);
      if (r <= 0)
        return false;
      got += r;
    }
    auto h = oca::ocp1::PduReader::try_parse_header(hdr, 9);
    if (!h)
      return false;
    size_t plen = h->pduSize - 9;
    out.assign(hdr, hdr + 9);
    out.resize(9 + plen);
    got = 0;
    while (got < plen) {
      ssize_t r = ::recv(sock, out.data() + 9 + got, plen - got, 0);
      if (r <= 0)
        return false;
      got += r;
    }
    out.insert(out.begin(), 0x3B);
    return true;
  };
  // 带参命令封装:SetLabel 需传 OcaString 参数
  auto cmdParams = [&](uint32_t handle, oca::ONo target, oca::MethodID mid,
                       const uint8_t* params, uint32_t paramBytes,
                       uint8_t nrParams) -> oca::ocp1::Response {
    oca::ocp1::Writer cw;
    oca::ocp1::write_command(cw, handle, target, mid, params, paramBytes,
                             nrParams);
    sendPdu(oca::ocp1::PduWriter::build_command_pdu(1, cw.data(), cw.size()));
    std::vector<uint8_t> rsp;
    BOOST_REQUIRE(recvPdu(rsp));
    auto h =
        oca::ocp1::PduReader::try_parse_header(rsp.data() + 1, rsp.size() - 1);
    auto rsps = oca::ocp1::PduReader::parse_responses(
        rsp.data() + 1 + 9, h->pduSize - 9, h->messageCount);
    BOOST_REQUIRE_EQUAL(rsps.size(), 1u);
    return rsps[0];
  };
  auto cmd = [&](uint32_t handle, oca::ONo target, oca::MethodID mid)
      -> oca::ocp1::Response {
    return cmdParams(handle, target, mid, nullptr, 0, 0);
  };
  namespace m = oca::methods;

  // 1) KeepAlive 握手
  sendPdu(oca::ocp1::PduWriter::build_keepalive_pdu(5));
  std::vector<uint8_t> ka;
  BOOST_REQUIRE(recvPdu(ka));

  // 2) AddPropertyChangeSubscription2(target=4, emitter=4097, PropertyID{2,1})
  oca::ocp1::Writer subp;
  subp.u32(4097);                 // EmitterONo
  subp.u16(m::kDefLevelManager);  // PropertyID defLevel=2(Agent)
  subp.u16(m::kPropLabel);        // PropertyID propertyIndex=1
  subp.u8(1);                     // DeliveryMode Normal
  subp.u16(0);                    // 空 NetworkAddress
  auto rsub = cmdParams(1, 4,
                        {m::kDefLevelSubMngr, m::kSubAddPropertyChangeSubscription2},
                        subp.data(), static_cast<uint32_t>(subp.size()), 4);
  BOOST_CHECK(rsub.statusCode == oca::Status::OK);

  // 3) SetLabel(4097, kAgentSetLabel=2, OcaString="world")
  oca::ocp1::Writer labw;
  labw.string("world");
  auto rset = cmdParams(2, 4097, {m::kDefLevelManager, m::kAgentSetLabel},
                        labw.data(), static_cast<uint32_t>(labw.size()), 1);
  BOOST_CHECK(rset.statusCode == oca::Status::OK);
  BOOST_CHECK_EQUAL(rset.nrParameters, 0);

  // 4) ping(GetOcaVersion)->触发 transport drain,通知上线
  auto rping =
      cmd(3, 1, {m::kDefLevelDeviceMngr, m::kDevGetOcaVersion});
  BOOST_CHECK(rping.statusCode == oca::Status::OK);

  // 5) recvPdu -> 期望 kPduNtf2
  std::vector<uint8_t> ntf;
  BOOST_REQUIRE(recvPdu(ntf));
  auto hn =
      oca::ocp1::PduReader::try_parse_header(ntf.data() + 1, ntf.size() - 1);
  BOOST_CHECK_EQUAL(hn->pduType, m::kPduNtf2);
  auto ntfs = oca::ocp1::PduReader::parse_notifications2(
      ntf.data() + 1 + 9, hn->pduSize - 9, hn->messageCount);
  BOOST_REQUIRE_EQUAL(ntfs.size(), 1u);
  BOOST_CHECK_EQUAL(ntfs[0].emitterONo, 4097u);
  BOOST_CHECK_EQUAL(ntfs[0].eventID.defLevel, m::kDefLevelRoot);
  BOOST_CHECK_EQUAL(ntfs[0].eventID.eventIndex, m::kEventPropertyChanged);
  oca::ocp1::Reader dr(ntfs[0].data, ntfs[0].dataCount);
  BOOST_CHECK_EQUAL(dr.u16(), m::kDefLevelManager);
  BOOST_CHECK_EQUAL(dr.u16(), m::kPropLabel);
  BOOST_CHECK_EQUAL(dr.string(), "world");

  // 6) 回读 GetLabel(4097) -> "world"(label_ 经 OcaServer 已真存)
  auto rget = cmd(4, 4097, {m::kDefLevelManager, m::kAgentGetLabel});
  BOOST_CHECK(rget.statusCode == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(rget.paramData, rget.paramBytes).string(),
                    "world");

  ::close(sock);
  server.stop();
}
```

- [ ] **Step 2: 构建验证**

Run: `./oca-dev.sh build`
Expected: 构建成功。

- [ ] **Step 3: 跑新用例确认通过**

Run: `daemon/build/tests/oca-test -p -t oca_e2e_property_changed`
Expected: PASS。这是 Spec4 里程碑——真实命令触发端到端闭环。

- [ ] **Step 4: 跑全量确认**

Run: `./oca-dev.sh test`
Expected: 34/34 全绿(31 + Task 6/7/8 三个新用例)。

- [ ] **Step 5: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/oca/tests/oca_test.cpp && git commit -m "test(oca): Spec4 - 新增 oca_e2e_property_changed 端到端用例

Spec4 里程碑:全真实 socket 经真实 SetLabel 命令触发 PropertyChanged,
transport drain -> Notification2 上线 -> 解析 PropertyID{2,1}+OcaString="world"。
证明发射点已接入生产路径(非测试代码伸手 trigger_event)。oca-test 34/34。

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 9: oca-probe 扩展 PropertyChanged 探测段 + --no-pc 参数

**Files:**
- Modify: `daemon/oca/tools/oca_probe.cpp:258-285`(main 参数解析)、`daemon/oca/tools/oca_probe.cpp:799`(汇总前插入探测段)

**Interfaces:**
- Consumes: Task 2-5 的完整发射链、`Probe::cmd`/`recv_pdu`、`parse_notifications2`、`m::kSubAddPropertyChangeSubscription2`/`kAgentSetLabel`/`kAgentGetLabel`/`kPropLabel`/`kDefLevelManager`。
- Produces: oca-probe 新增"PropertyChanged 订阅与投递"探测段 + `--no-pc` 控制参数。

- [ ] **Step 1: main 新增 do_pc 参数与 --no-pc 解析**

修改 `daemon/oca/tools/oca_probe.cpp` 的 main(line 258-285)，在 `bool do_sub = true;`(line 261)之后新增 `bool do_pc = true;`，并在参数解析循环(line 263-285)的 `--no-sub` 分支之后新增 `--no-pc` 分支：

```cpp
  std::string host = "127.0.0.1";
  uint16_t port = 65037;
  bool do_sub = true;
  bool do_pc = true;  // Spec4:PropertyChanged 探测段

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--no-sub") {
      do_sub = false;
    } else if (a == "--no-pc") {  // Spec4
      do_pc = false;
    } else if (a == "-h" || a == "--help") {
      std::cout << "用法: oca-probe [host] [port] [--no-sub] [--no-pc]\n"
                << "  默认 127.0.0.1 65037\n"
                << "  --no-sub  跳过 EV2 订阅测试\n"
                << "  --no-pc   跳过 PropertyChanged 投递测试\n";
      return 0;
```

- [ ] **Step 2: 新增探测段(在汇总 section 之前)**

在 `daemon/oca/tools/oca_probe.cpp` 的 `// --- 汇总 ---`(line 799)之前插入：

```cpp
  // --- 9. PropertyChanged 订阅与投递(Spec4)-----------------------------------
  if (do_pc) {
    section("PropertyChanged 订阅与投递 (AddPropertyChangeSubscription2 + SetLabel)");
    // 9a. 订阅 OcaNetwork(4097) 的 Label PropertyChanged
    {
      oca::ocp1::Writer p;
      p.u32(4097);                 // EmitterONo
      p.u16(m::kDefLevelManager);  // PropertyID defLevel=2(Agent)
      p.u16(m::kPropLabel);        // PropertyID propertyIndex=1
      p.u8(1);                     // DeliveryMode Normal
      p.u16(0);                    // 空 NetworkAddress
      auto r = probe.cmd(4, {m::kDefLevelSubMngr, m::kSubAddPropertyChangeSubscription2},
                         p.data(), static_cast<uint32_t>(p.size()), 4);
      if (r.ok && r.status == o::Status::OK) {
        std::cout << OK() << "  [OK] AddPropertyChangeSubscription2 成功"
                  << OFF() << "\n";
      } else {
        std::cout << ERR() << "  [FAIL] AddPropertyChangeSubscription2 status="
                  << status_name(r.status) << OFF() << "\n";
        probe.failures++;
      }
    }
    // 9b. SetLabel(4097, "oca-probe-test")->触发 emit + transport drain
    //     (Response 返回即触发 drain,通知上线)
    {
      oca::ocp1::Writer p;
      p.string("oca-probe-test");
      auto r = probe.cmd(4097, {m::kDefLevelManager, m::kAgentSetLabel},
                         p.data(), static_cast<uint32_t>(p.size()), 1);
      if (r.ok && r.status == o::Status::OK && r.params.empty()) {
        std::cout << OK() << "  [OK] SetLabel(4097, \"oca-probe-test\") -> OK"
                  << OFF() << "\n";
      } else {
        std::cout << ERR() << "  [FAIL] SetLabel status=" << status_name(r.status)
                  << OFF() << "\n";
        probe.failures++;
      }
    }
    // 9c. Probe::cmd 接收循环会跨过穿插的 Notification2 并打印。
    //     但本段需主动断言收到 Ntf2,故直接 recv_pdu 等一轮。
    {
      auto rp = recv_pdu(probe.fd);
      bool got_pc = false;
      if (rp && rp->hdr.pduType == m::kPduNtf2) {
        auto ntfs = oca::ocp1::PduReader::parse_notifications2(
            rp->payload.data(), rp->payload.size(), rp->hdr.messageCount);
        for (const auto& n : ntfs) {
          if (n.emitterONo == 4097 &&
              n.eventID.defLevel == m::kDefLevelRoot &&
              n.eventID.eventIndex == m::kEventPropertyChanged) {
            oca::ocp1::Reader dr(n.data, n.dataCount);
            uint16_t pid_dl = dr.u16();
            uint16_t pid_idx = dr.u16();
            std::string val = dr.string();
            std::cout << OK() << "  [OK] 收到 PropertyChanged emitter=4097"
                      << " PropertyID{" << pid_dl << "," << pid_idx << "}"
                      << " newLabel=\"" << val << "\"" << OFF() << "\n";
            got_pc = true;
          }
        }
      }
      if (!got_pc) {
        std::cout << ERR() << "  [FAIL] 未收到预期 PropertyChanged 通知"
                  << OFF() << "\n";
        probe.failures++;
      }
    }
    // 9d. 回读 GetLabel(4097)->"oca-probe-test"
    {
      auto r = probe.cmd0(4097, {m::kDefLevelManager, m::kAgentGetLabel});
      if (r.ok && r.status == o::Status::OK) {
        oca::ocp1::Reader pr(r.params.data(), r.params.size());
        std::string label = pr.string();
        if (label == "oca-probe-test") {
          std::cout << OK() << "  [OK] GetLabel(4097) 回读 == \""
                    << label << "\"" << OFF() << "\n";
        } else {
          std::cout << ERR() << "  [FAIL] GetLabel 回读=\"" << label
                    << "\" (期望 \"oca-probe-test\")" << OFF() << "\n";
          probe.failures++;
        }
      } else {
        std::cout << ERR() << "  [FAIL] GetLabel status=" << status_name(r.status)
                  << OFF() << "\n";
        probe.failures++;
      }
    }
  }

  // --- 汇总 ------------------------------------------------------------------
  section("汇总");
```

> 注意：`recv_pdu` 与 `probe.cmd` 共用同一 fd。9c 直接 `recv_pdu` 等一轮 Ntf2；若期间穿插 KeepAlive，recv_pdu 已同步到 0x3B 头但只读一帧。实测 SetLabel 的 Response 返回后 transport 立即 drain 通知，9c 第一帧即 Ntf2。若偶发穿插 KeepAlive，9c 会判为非 Ntf2 而 FAIL——但 transport 对 KeepAlive 是收才回、不主动发，正常无穿插。

- [ ] **Step 3: 构建验证**

Run: `./oca-dev.sh build`
Expected: 构建成功(oca-probe 目标)。

- [ ] **Step 4: 启动 daemon 跑 probe 验证**

Run: `./oca-dev.sh run -i lo && ./oca-dev.sh probe`
Expected: probe 输出新 section "PropertyChanged 订阅与投递"，4 个 `[OK]`：AddPropertyChangeSubscription2 成功、SetLabel OK、收到 PropertyChanged newLabel="oca-probe-test"、GetLabel 回读一致。汇总"全部探测通过"。

Run: `./oca-dev.sh stop`
Expected: 停止成功。

- [ ] **Step 5: --no-pc 参数验证**

Run: `./oca-dev.sh run -i lo && ./oca-dev.sh probe --no-pc && ./oca-dev.sh stop`
Expected: probe 不出现 PropertyChanged section，其余探测通过，汇总"全部探测通过"。

- [ ] **Step 6: 跑 oca-test 确认未破坏**

Run: `./oca-dev.sh test`
Expected: 34/34 全绿(probe 改动不影响 oca-test)。

- [ ] **Step 7: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/oca/tools/oca_probe.cpp && git commit -m "feat(oca): Spec4 - oca-probe 新增 PropertyChanged 投递探测段 + --no-pc

新增探测段:AddPropertyChangeSubscription2 -> SetLabel(4097) ->
收 Notification2 解析 PropertyID{2,1}+OcaString -> GetLabel 回读。
--no-pc 控制是否跑该段。供人工/脚本二次核验端到端投递。

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 10: 维护手册同步 Spec4 + 最终验收

**Files:**
- Modify: `docs/ops/oca-design-and-maintenance.md`

**Interfaces:**
- Consumes: Task 1-9 的全部成果。
- Produces: 维护手册反映 Spec4 完成状态、新增的发射机制、测试用例数 34、Spec4 阶段行。

- [ ] **Step 1: 更新维护手册**

修改 `docs/ops/oca-design-and-maintenance.md`：

1. **总体架构 L2 对象类描述**(约 line 21)：
   `Root/Agent/Worker/Block/DevMgr/NetMgr/SubMgr/AppNet/Network/CtrlNet` 后补注 Spec4 发射能力——将 L2 对象行改为：
   ```
   CLS["9 个对象类<br/>Root/Agent/Worker/Block<br/>DevMgr/NetMgr/SubMgr<br/>AppNet/Network/CtrlNet<br/>(Spec4: emitter 注入)"]
   ```

2. **OcaRoot 已实现方法表**(约 line 178-188 区段)后新增一节 "OcaRoot 事件发射(Spec4)"，在"OcaAgent"小节前插入：
   ```markdown
   #### OcaRoot 事件发射(Spec4)

   OcaRoot 持有 `OcaSubscriptionManager* emitter_`(默认 nullptr)，经 `set_event_emitter` 注入、`emit_property_changed` 触发。发射 session 无关(符合 AES70 发射/投递解耦)。OcaServer 注册完所有对象后统一注入(`objects_in_range(1,9999)`)。

   `emit_property_changed(prop_def_level, prop_index, value_data, value_count)`：
   - 负载 = `PropertyID{u16 defLevel, u16 propertyIndex}` + 已编码属性值
   - 触发事件 `{kDefLevelRoot, kEventPropertyChanged}` = `{1,1}`
   - emitter_ 为空时静默(只读对象 / 单测未经 OcaServer)

   PropertyID 按 AES70 规范：`{声明类 defLevel, 类属性表 propertyIndex}`，与 methodIndex 独立命名空间。`kPropLabel=1`、`kPropEnabled=1`(各类首个可报变属性)。
   ```

3. **OcaAgent / OcaWorker / OcaApplicationNetwork 方法表**(约 line 189-273)中 SetLabel/GetLabel 行更新来源标注为 "Spec4(真存+emit)"，并在各表后补注：
   - OcaAgent 表后：`> Spec4:SetLabel 真存 label_ + emit PropertyID{2,kPropLabel}+OcaString;GetLabel 返 label_.empty()?role():label_。`
   - OcaWorker 表后：同上 PropertyID{2,kPropLabel}。
   - OcaAppNet 表后：同上 PropertyID{2,kPropLabel}。

4. **OcaDeviceManager 方法表**(约 line 221-238)的 GetEnabled/SetEnabled 行更新来源为 "Spec4(真存+emit)"，表后补注：
   `> Spec4:SetEnabled 真存 enabled_ + emit PropertyID{3,kPropEnabled}+OcaBoolean;GetEnabled 返 enabled_(默认 true)。`

5. **订阅与事件投递节**(约 line 313-323)在 `trigger_event` 说明后补注：
   ```markdown
   > **Spec4 发射点接入**:Spec4 前 trigger_event 仅测试调用;Spec4 后 SetLabel(三类)/SetEnabled 经 emit_property_changed 真触发,发射 session 无关。投递机制不变(drain 在下次 PDU 后 / EAGAIN 空闲态)。
   ```

6. **测试节**(约 line 495-506)：测试用例数 `31` 改 `34`，用例分布表新增行：
   ```markdown
   | L2 单测(Spec4) | dispatch_property_changed_label_emit、dispatch_property_changed_enabled_emit |
   | 验收/回归(Spec4) | oca_e2e_property_changed(PropertyChanged 端到端) |
   ```
   `oca_e2e_acceptance` 描述行后补：`oca_e2e_property_changed 是 Spec4 里程碑:真实 SetLabel 命令触发端到端闭环。`

7. **Spec 阶段与合规状态表**(约 line 512-518)新增 Spec4 行：
   ```markdown
   | Spec4 | PropertyChanged 发射点接入 + 端到端投递验证 | oca-test 34/34,oca-probe PC 段 [OK] | <commit 范围> |
   ```
   `<commit 范围>` 由实际提交填充(见 Step 3)。

8. **后续规划节**(约 line 546-575)的 "Spec4：PropertyChanged 通知投递" 小节标注为 **已完成**，或移至"已完成阶段"。Spec5 media 桥接保持为下一阶段。

9. **已知限制与待办**(约 line 619-640)：
   - 删除"PropertyChanged 实际通知投递——订阅 OK 但触发未验证"条目(已解决)。
   - Session TOCTOU 风险行补注：`Spec4 后 emitter_ 注入使真触发成为现实,Session TOCTOU 风险升级`(因 trigger_event 持 raw Session* 在生产路径被调)。

- [ ] **Step 2: 最终全量验收**

Run: `./oca-dev.sh build && ./oca-dev.sh test`
Expected: 构建成功 + oca-test 34/34 全绿。

Run: `./oca-dev.sh run -i lo && ./oca-dev.sh probe && ./oca-dev.sh stop`
Expected: probe 全部探测通过，含 Spec4 PropertyChanged 段 4 个 `[OK]`。

- [ ] **Step 3: 填充 commit 范围并提交**

先查 Spec4 提交范围：
```bash
cd /home/Share/GitHub/aes67-linux-daemon && git log --oneline e8a3d7f..HEAD
```
将 Task 10 Step 1.7 中 `<commit 范围>` 替换为实际起止 commit(首个 Spec4 实现 commit..最后实现 commit)。

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add docs/ops/oca-design-and-maintenance.md && git commit -m "docs(oca): 维护手册同步 Spec4 — PropertyChanged 发射与端到端投递

- OcaRoot emitter 注入 + emit_property_changed 机制说明
- SetLabel(三类)/SetEnabled 真存+emit,PropertyID 规范定义
- 测试用例 31->34,新增 Spec4 阶段行
- 删除已解决的 PropertyChanged 投递待办,Session TOCTOU 风险升级

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## 验收清单(全计划完成后)

- [ ] `./oca-dev.sh build` 成功
- [ ] `./oca-dev.sh test` → oca-test 34/34
- [ ] `./oca-dev.sh probe` → 含 Spec4 PropertyChanged 段 4 个 `[OK]`，汇总"全部探测通过"
- [ ] `./oca-dev.sh probe --no-pc` → 跳过 PC 段，其余通过
- [ ] 维护手册 `docs/ops/oca-design-and-maintenance.md` 已同步 Spec4
- [ ] Spec1~3 既有断言全不破(回归)
- [ ] 提交历史清晰，每个 Task 一个提交，提交信息含 Spec4 前缀与 Co-Authored-By
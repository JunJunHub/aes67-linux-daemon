# Spec3 实现计划：OcaAgent/OcaApplicationNetwork 中间类 + OcaWorker 方法补齐

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 引入 OcaAgent/OcaApplicationNetwork 中间类，修正 C++ 类层次与 AES70 标准同构，补齐 OcaWorker GetLabel/SetLabel/GetOwner/GetPath 方法，使 test5 OCC Object Compliancy 通过或显著改善。

**Architecture:** 新增 OcaAgent:OcaRoot 和 OcaApplicationNetwork:OcaRoot 两个中间类，将 OcaNetwork 改为继承 OcaAgent、OcaControlNetwork 改为继承 OcaApplicationNetwork。OcaWorker 增加 owner_ono_ 成员和 GetLabel/SetLabel/GetOwner/GetPath 方法。所有新增方法返回 OK（GetPath 返回 NotImplemented）。

**Tech Stack:** C++17, Boost.Test, CMake

## Global Constraints

- 构建路径：`cd daemon && cmake -DWITH_OCA=ON -DWITH_AVAHI=OFF -DFAKE_DRIVER=ON -DWITH_STREAMER=OFF . && make oca-test`
- 测试命令：`./tests/oca-test -p`
- 方法索引来源：OCAMicro 参考实现（`/home/Share/GitHub/oca-tools-probe/OCAMicro`）
- 每步提交后必须：编译通过 + oca-test 全绿
- 代码风格：2-space 缩进，80-col，PointerAlignment: Left（`.clang-format`）

---

### Task 1: methods.hpp 新增方法索引常量

**Files:**
- Modify: `daemon/oca/methods.hpp`

**Interfaces:**
- Produces: `kAgentGetLabel(1)`, `kAgentSetLabel(2)`, `kAgentGetOwner(3)`, `kAgentGetPath(4)`, `kWorkerGetLabel(8)`, `kWorkerSetLabel(9)`, `kWorkerGetOwner(10)`, `kWorkerGetPath(13)`, `kAppNetGetLabel(1)`, `kAppNetSetLabel(2)`, `kAppNetGetOwner(3)`, `kAppNetGetPath(10)`

- [ ] **Step 1: 在 methods.hpp 中 OcaWorker 注释块后添加 OcaAgent 常量**

在 `kWorkerGetPorts = 5` 行之后、OcaApplicationNetwork 注释块之前，插入：

```cpp
// OcaAgent methods (DefLevel 2, classID{1,2}) - OCAMicro OcaLiteAgent
// OcaNetwork{1,2,1} 继承 OcaAgent{1,2},工具对 ONo 4097 测 Agent 强制方法
// (GetLabel/SetLabel/GetOwner/GetPath)。在 OcaAgent 中间类实装。
constexpr uint16_t kAgentGetLabel = 1;   // OCAMicro
constexpr uint16_t kAgentSetLabel = 2;   // OCAMicro
constexpr uint16_t kAgentGetOwner = 3;   // OCAMicro
constexpr uint16_t kAgentGetPath = 4;    // OCAMicro
```

- [ ] **Step 2: 在 OcaWorker 注释块中添加新增方法常量**

在 `kWorkerGetPorts = 5` 行之后添加：

```cpp
constexpr uint16_t kWorkerGetLabel = 8;    // OCAMicro
constexpr uint16_t kWorkerSetLabel = 9;    // OCAMicro
constexpr uint16_t kWorkerGetOwner = 10;   // OCAMicro
constexpr uint16_t kWorkerGetPath = 13;    // OCAMicro
```

- [ ] **Step 3: 在 OcaApplicationNetwork 注释块中添加新增方法常量**

在 `kAppNetGetSystemInterfaces = 6` 行之后添加：

```cpp
constexpr uint16_t kAppNetGetLabel = 1;    // OCAMicro
constexpr uint16_t kAppNetSetLabel = 2;    // OCAMicro
constexpr uint16_t kAppNetGetOwner = 3;    // OCAMicro
constexpr uint16_t kAppNetGetPath = 10;    // OCAMicro
```

- [ ] **Step 4: 编译验证**

Run: `cd daemon/build && make oca-test 2>&1 | tail -5`
Expected: 编译成功（仅新增常量，无行为变化）

- [ ] **Step 5: 运行测试确认无回归**

Run: `./daemon/tests/oca-test -p 2>&1 | tail -3`
Expected: 28/28 passed

- [ ] **Step 6: 提交**

```bash
git add daemon/oca/methods.hpp
git commit -m "feat(oca): Spec3 - methods.hpp 新增 OcaAgent/OcaWorker/OcaAppNet 方法索引常量"
```

---

### Task 2: 创建 OcaAgent 中间类

**Files:**
- Create: `daemon/oca/classes/agent.hpp`
- Create: `daemon/oca/classes/agent.cpp`

**Interfaces:**
- Consumes: `OcaRoot` (from `root.hpp`), `methods::kDefLevelManager`, `kAgentGetLabel/SetLabel/GetOwner/GetPath`
- Produces: `OcaAgent : OcaRoot` 类，`exec()` 处理 defLevel=2 Agent 方法 + 委托 OcaRoot defLevel=1

- [ ] **Step 1: 创建 agent.hpp**

```cpp
//  classes/agent.hpp - OcaAgent {1,2} v2
//
//  AES70 抽象基类,定义 Agent 语义(Label/Owner/Path)。
//  OcaNetwork{1,2,1} 继承 OcaAgent{1,2}。DefLevel == classID.fieldCount == 2。
//  本类方法(defLevel 2)在本类处理;OcaRoot 通用方法(defLevel 1)经
//  exec 委托链回 OcaRoot::handle_root。

#ifndef OCA_CLASSES_AGENT_HPP_
#define OCA_CLASSES_AGENT_HPP_

#include "oca/classes/root.hpp"

namespace oca {

// OcaAgent {1,2} v2:Agent 抽象基类
// OcaNetwork{1,2,1} 继承此类。OcaAgent 本身不直接实例化(与 OcaWorker 同模式)。
class OcaAgent : public OcaRoot {
 public:
  explicit OcaAgent(ONo ono, ONo owner_ono = 0)
      : OcaRoot(ono), owner_ono_(owner_ono) {}
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;

 protected:
  // OcaAgent DefLevel-2 方法:GetLabel/SetLabel/GetOwner/GetPath。
  ExecResult handle_agent(uint16_t methodIndex,
                          ocp1::Reader& req,
                          ocp1::Writer& rsp);
  ONo owner_ono_;
};

}  // namespace oca

#endif  // OCA_CLASSES_AGENT_HPP_
```

- [ ] **Step 2: 创建 agent.cpp**

```cpp
//  classes/agent.cpp - OcaAgent {1,2} v2 实现

#include "oca/classes/agent.hpp"

#include "oca/methods.hpp"

namespace oca {

ExecResult OcaAgent::exec(MethodID m,
                           ocp1::Reader& req,
                           ocp1::Writer& rsp,
                           Session& sess) {
  if (m.defLevel == methods::kDefLevelManager) {  // == classID.fieldCount == 2
    return handle_agent(m.methodIndex, req, rsp);
  }
  return OcaRoot::exec(m, req, rsp, sess);  // 委托 DefLevel 1 -> OcaRoot
}

ExecResult OcaAgent::handle_agent(uint16_t idx,
                                  ocp1::Reader& req,
                                  ocp1::Writer& rsp) {
  switch (idx) {
    case methods::kAgentGetLabel:
      // OcaString:返回 role() 作为标签
      rsp.string(role());
      return {Status::OK, 1};
    case methods::kAgentSetLabel:
      // 读可选 OcaString(remaining>=2 时跳过),no-op。
      if (req.remaining() >= 2)
        (void)req.string();
      return {Status::OK, 0};
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

}  // namespace oca
```

- [ ] **Step 3: 编译验证（新文件未接入 CMake，仅语法检查）**

Run: `cd daemon && g++ -std=c++17 -fsyntax-only -I. oca/classes/agent.cpp 2>&1 | head -5`
Expected: 可能缺少链接依赖，但无语法错误。实际验证在 Task 4 接入 CMake 后进行。

- [ ] **Step 4: 提交**

```bash
git add daemon/oca/classes/agent.hpp daemon/oca/classes/agent.cpp
git commit -m "feat(oca): Spec3 - OcaAgent {1,2} 中间类(GetLabel/SetLabel/GetOwner/GetPath)"
```

---

### Task 3: 创建 OcaApplicationNetwork 中间类

**Files:**
- Create: `daemon/oca/classes/application_network.hpp`
- Create: `daemon/oca/classes/application_network.cpp`

**Interfaces:**
- Consumes: `OcaRoot` (from `root.hpp`), `methods::kDefLevelManager`, `kAppNetGetLabel/SetLabel/GetOwner/GetServiceID/GetSystemInterfaces/GetPath`
- Produces: `OcaApplicationNetwork : OcaRoot` 类，`exec()` 处理 defLevel=2 AppNet 方法 + 委托 OcaRoot defLevel=1

- [ ] **Step 1: 创建 application_network.hpp**

```cpp
//  classes/application_network.hpp - OcaApplicationNetwork {1,4} v1
//
//  AES70-2018 应用网络抽象基类。OcaControlNetwork{1,4,1} 继承此类。
//  DefLevel == classID.fieldCount == 2。本类方法(defLevel 2)在本类处理;
//  OcaRoot 通用方法(defLevel 1)经 exec 委托链回 OcaRoot::handle_root。

#ifndef OCA_CLASSES_APPLICATION_NETWORK_HPP_
#define OCA_CLASSES_APPLICATION_NETWORK_HPP_

#include "oca/classes/root.hpp"

namespace oca {

// OcaApplicationNetwork {1,4} v1:应用网络抽象基类
// AvailableSince AES70-2018。OcaControlNetwork{1,4,1} 继承此类。
// GetServiceID/GetSystemInterfaces 从 OcaControlNetwork 内联分派移入此类。
class OcaApplicationNetwork : public OcaRoot {
 public:
  explicit OcaApplicationNetwork(ONo ono, ONo owner_ono = 0)
      : OcaRoot(ono), owner_ono_(owner_ono) {}
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;

 protected:
  // OcaApplicationNetwork DefLevel-2 方法。
  ExecResult handle_appnet(uint16_t methodIndex,
                           ocp1::Reader& req,
                           ocp1::Writer& rsp);
  ONo owner_ono_;
};

}  // namespace oca

#endif  // OCA_CLASSES_APPLICATION_NETWORK_HPP_
```

- [ ] **Step 2: 创建 application_network.cpp**

```cpp
//  classes/application_network.cpp - OcaApplicationNetwork {1,4} v1 实现

#include "oca/classes/application_network.hpp"

#include "oca/methods.hpp"

namespace oca {

ExecResult OcaApplicationNetwork::exec(MethodID m,
                                       ocp1::Reader& req,
                                       ocp1::Writer& rsp,
                                       Session& sess) {
  if (m.defLevel == methods::kDefLevelManager) {  // == classID.fieldCount == 2
    return handle_appnet(m.methodIndex, req, rsp);
  }
  return OcaRoot::exec(m, req, rsp, sess);  // 委托 DefLevel 1 -> OcaRoot
}

ExecResult OcaApplicationNetwork::handle_appnet(uint16_t idx,
                                                ocp1::Reader& req,
                                                ocp1::Writer& rsp) {
  switch (idx) {
    case methods::kAppNetGetLabel:
      // OcaString:返回 role() 作为标签
      rsp.string(role());
      return {Status::OK, 1};
    case methods::kAppNetSetLabel:
      // 读可选 OcaString,no-op。
      if (req.remaining() >= 2)
        (void)req.string();
      return {Status::OK, 0};
    case methods::kAppNetGetOwner:
      // ONo:含有块的对象号。
      rsp.u32(owner_ono_);
      return {Status::OK, 1};
    case methods::kAppNetGetServiceID:
      // OcaApplicationNetworkServiceID = OcaString:空字符串
      rsp.string("");
      return {Status::OK, 1};
    case methods::kAppNetGetSystemInterfaces:
      // Ocp1List<OcaNetworkSystemInterfaceDescriptor>:空列表(u16 count=0)
      rsp.u16(0);
      return {Status::OK, 1};
    case methods::kAppNetGetPath:
      // OcaPath 结构复杂,YAGNI。
      return {Status::NotImplemented, 0};
    default:
      return {Status::NotImplemented, 0};
  }
}

}  // namespace oca
```

- [ ] **Step 3: 提交**

```bash
git add daemon/oca/classes/application_network.hpp daemon/oca/classes/application_network.cpp
git commit -m "feat(oca): Spec3 - OcaApplicationNetwork {1,4} 中间类(GetLabel/SetLabel/GetOwner/GetServiceID/GetSystemInterfaces/GetPath)"
```

---

### Task 4: OcaNetwork 重基类为 OcaAgent + 接入 CMake

**Files:**
- Modify: `daemon/oca/classes/network.hpp`
- Modify: `daemon/oca/classes/network.cpp`
- Modify: `daemon/CMakeLists.txt`
- Modify: `daemon/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `OcaAgent` (from Task 2)
- Produces: `OcaNetwork : OcaAgent`，exec 委托链 OcaNetwork→OcaAgent→OcaRoot

- [ ] **Step 1: 修改 network.hpp — 继承改为 OcaAgent，构造加 owner_ono**

将 `#include "oca/classes/root.hpp"` 改为 `#include "oca/classes/agent.hpp"`。

将类声明改为：

```cpp
class OcaNetwork : public OcaAgent {
 public:
  explicit OcaNetwork(ONo ono, ONo owner_ono = 0)
      : OcaAgent(ono, owner_ono) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 1; }
  std::string role() const override { return "Network"; }
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;
};
```

- [ ] **Step 2: 修改 network.cpp — exec 委托改为 OcaAgent::exec**

将最后一行 `return OcaRoot::exec(m, req, rsp, sess);` 改为 `return OcaAgent::exec(m, req, rsp, sess);`。

完整 exec 方法：

```cpp
ExecResult OcaNetwork::exec(MethodID m,
                            ocp1::Reader& req,
                            ocp1::Writer& rsp,
                            Session& sess) {
  if (m.defLevel == methods::kDefLevelBlock) {  // == classID.fieldCount == 3
    switch (m.methodIndex) {
      case methods::kNet2GetLinkType:
        rsp.u8(1);
        return {Status::OK, 1};
      case methods::kNet2GetIDAdvertised:
        rsp.blob(nullptr, 0);
        return {Status::OK, 1};
      case methods::kNet2GetControlProtocol:
        rsp.u8(1);
        return {Status::OK, 1};
      case methods::kNet2GetMediaProtocol:
        rsp.u8(0);
        return {Status::OK, 1};
      case methods::kNet2GetSystemInterfaces:
        rsp.u16(0);
        return {Status::OK, 1};
      case methods::kNet2Shutdown:
        return {Status::OK, 0};
      default:
        return {Status::NotImplemented, 0};
    }
  }
  return OcaAgent::exec(m, req, rsp, sess);  // 委托 OcaAgent(defLevel 2+1)
}
```

- [ ] **Step 3: CMakeLists.txt — SOURCES 添加 agent.cpp 和 application_network.cpp**

在 `oca/classes/network.cpp` 之前添加 `oca/classes/agent.cpp oca/classes/application_network.cpp`。

SOURCES 行变为：

```cmake
list(APPEND SOURCES oca/ocp1.cpp oca/session.cpp oca/transport.cpp
     oca/oca_server.cpp oca/classes/root.cpp oca/classes/agent.cpp
     oca/classes/application_network.cpp oca/classes/device_manager.cpp
     oca/classes/network_manager.cpp oca/classes/subscription_manager.cpp
     oca/classes/network.cpp oca/classes/control_network.cpp)
```

- [ ] **Step 4: tests/CMakeLists.txt — oca-test 添加 agent.cpp 和 application_network.cpp**

在 `root.cpp` 行之后添加两行：

```cmake
target_sources(oca-test PRIVATE ${CMAKE_SOURCE_DIR}/oca/classes/agent.cpp)
target_sources(oca-test PRIVATE ${CMAKE_SOURCE_DIR}/oca/classes/application_network.cpp)
```

- [ ] **Step 5: 重新配置 CMake 并编译**

Run: `cd daemon/build && cmake -DWITH_OCA=ON -DWITH_AVAHI=OFF -DFAKE_DRIVER=ON -DWITH_STREAMER=OFF .. && make oca-test 2>&1 | tail -5`
Expected: 编译成功

- [ ] **Step 6: 运行测试确认无回归**

Run: `./daemon/tests/oca-test -p 2>&1 | tail -3`
Expected: 28/28 passed（OcaNetwork 现在通过 OcaAgent 委托 defLevel=2，但现有测试只测 defLevel=1 和 defLevel=3，行为不变）

- [ ] **Step 7: 提交**

```bash
git add daemon/oca/classes/network.hpp daemon/oca/classes/network.cpp daemon/CMakeLists.txt daemon/tests/CMakeLists.txt
git commit -m "feat(oca): Spec3 - OcaNetwork 重基类为 OcaAgent + CMake 接入新源文件"
```

---

### Task 5: OcaControlNetwork 重基类为 OcaApplicationNetwork

**Files:**
- Modify: `daemon/oca/classes/control_network.hpp`
- Modify: `daemon/oca/classes/control_network.cpp`

**Interfaces:**
- Consumes: `OcaApplicationNetwork` (from Task 3)
- Produces: `OcaControlNetwork : OcaApplicationNetwork`，exec 委托链 OcaControlNetwork→OcaApplicationNetwork→OcaRoot；内联 AppNet 分派删除

- [ ] **Step 1: 修改 control_network.hpp — 继承改为 OcaApplicationNetwork，构造加 owner_ono**

将 `#include "oca/classes/root.hpp"` 改为 `#include "oca/classes/application_network.hpp"`。

将类声明改为：

```cpp
class OcaControlNetwork : public OcaApplicationNetwork {
 public:
  explicit OcaControlNetwork(ONo ono, ONo owner_ono = 0)
      : OcaApplicationNetwork(ono, owner_ono) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 1; }
  std::string role() const override { return "Control Network"; }
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;
};
```

- [ ] **Step 2: 修改 control_network.cpp — 删除内联 AppNet 分派，委托改为 OcaApplicationNetwork::exec**

完整替换 exec 方法：

```cpp
ExecResult OcaControlNetwork::exec(MethodID m,
                                   ocp1::Reader& req,
                                   ocp1::Writer& rsp,
                                   Session& sess) {
  // OcaControlNetwork{1,4,1} defLevel=3 自身方法。
  // OcaApplicationNetwork{1,4} defLevel=2 方法由父类 OcaApplicationNetwork::exec
  // 自动处理(GetLabel/SetLabel/GetOwner/GetServiceID/GetSystemInterfaces/GetPath)。
  if (m.defLevel == methods::kDefLevelBlock) {  // == classID.fieldCount == 3
    switch (m.methodIndex) {
      case methods::kCtrlNetGetControlProtocol:
        // OcaNetworkControlProtocol(u8):OCP.1=1
        rsp.u8(1);
        return {Status::OK, 1};
      default:
        return {Status::NotImplemented, 0};
    }
  }
  return OcaApplicationNetwork::exec(m, req, rsp, sess);
}
```

- [ ] **Step 3: 编译验证**

Run: `cd daemon/build && make oca-test 2>&1 | tail -5`
Expected: 编译成功

- [ ] **Step 4: 运行测试确认无回归**

Run: `./daemon/tests/oca-test -p 2>&1 | tail -3`
Expected: 28/28 passed（GetServiceID/GetSystemInterfaces 现由 OcaApplicationNetwork 处理，行为与之前内联分派完全一致）

- [ ] **Step 5: 提交**

```bash
git add daemon/oca/classes/control_network.hpp daemon/oca/classes/control_network.cpp
git commit -m "feat(oca): Spec3 - OcaControlNetwork 重基类为 OcaApplicationNetwork,移除内联 AppNet 分派"
```

---

### Task 6: OcaWorker 增加 owner_ono_ 和 GetLabel/SetLabel/GetOwner/GetPath

**Files:**
- Modify: `daemon/oca/classes/root.hpp`
- Modify: `daemon/oca/classes/root.cpp`

**Interfaces:**
- Consumes: `methods::kWorkerGetLabel/SetLabel/GetOwner/GetPath`
- Produces: `OcaWorker::owner_ono_` 成员，`OcaWorker(ONo, ONo owner_ono=0)` 构造，handle_worker 新增 4 个 case；OcaBlock 构造适配

- [ ] **Step 1: 修改 root.hpp — OcaWorker 加 owner_ono_ 和构造参数，OcaBlock 适配**

OcaWorker 改为：

```cpp
class OcaWorker : public OcaRoot {
 public:
  explicit OcaWorker(ONo ono, ONo owner_ono = 0)
      : OcaRoot(ono), owner_ono_(owner_ono) {}
  ExecResult exec(MethodID m,
                  ocp1::Reader& req,
                  ocp1::Writer& rsp,
                  Session& sess) override;

 protected:
  ExecResult handle_worker(uint16_t methodIndex,
                           ocp1::Reader& req,
                           ocp1::Writer& rsp);
  ONo owner_ono_ = 0;
};
```

OcaBlock 构造改为：

```cpp
class OcaBlock : public OcaWorker {
 public:
  explicit OcaBlock(ONo ono, ONo owner_ono = 0)
      : OcaWorker(ono, owner_ono) {}
  // ... 其余不变 ...
};
```

- [ ] **Step 2: 修改 root.cpp — handle_worker 增加 4 个 case**

在 `case methods::kWorkerGetPorts:` 之后、`default:` 之前添加：

```cpp
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
```

- [ ] **Step 3: 编译验证**

Run: `cd daemon/build && make oca-test 2>&1 | tail -5`
Expected: 编译成功

- [ ] **Step 4: 运行测试确认无回归**

Run: `./daemon/tests/oca-test -p 2>&1 | tail -3`
Expected: 28/28 passed

- [ ] **Step 5: 提交**

```bash
git add daemon/oca/classes/root.hpp daemon/oca/classes/root.cpp
git commit -m "feat(oca): Spec3 - OcaWorker 增加 owner_ono_ + GetLabel/SetLabel/GetOwner/GetPath"
```

---

### Task 7: OcaServer 传递 owner_ono + oca_test.cpp 新增 include

**Files:**
- Modify: `daemon/oca/oca_server.cpp`

**Interfaces:**
- Consumes: `OcaNetwork(ONo, ONo)`, `OcaControlNetwork(ONo, ONo)` (from Tasks 4-5)
- Produces: OcaNetwork(4097, 100) 和 OcaControlNetwork(4098, 100) 的 owner_ono=100

- [ ] **Step 1: 修改 oca_server.cpp — 构造传递 owner_ono=100**

将：

```cpp
auto* net2 = new OcaNetwork(4097);
auto* ctrl_net = new OcaControlNetwork(4098);
```

改为：

```cpp
auto* net2 = new OcaNetwork(4097, 100);      // owner = Root Block(ONo 100)
auto* ctrl_net = new OcaControlNetwork(4098, 100);  // owner = Root Block(ONo 100)
```

- [ ] **Step 2: 编译验证**

Run: `cd daemon/build && make oca-test aes67-daemon 2>&1 | tail -5`
Expected: 编译成功

- [ ] **Step 3: 运行测试确认无回归**

Run: `./daemon/tests/oca-test -p 2>&1 | tail -3`
Expected: 28/28 passed

- [ ] **Step 4: 提交**

```bash
git add daemon/oca/oca_server.cpp
git commit -m "feat(oca): Spec3 - OcaServer 传递 owner_ono=100 给 Network/CtrlNet 对象"
```

---

### Task 8: 新增 dispatch 测试用例

**Files:**
- Modify: `daemon/oca/tests/oca_test.cpp`

**Interfaces:**
- Consumes: `OcaNetwork(ONo, ONo)`, `OcaControlNetwork(ONo, ONo)`, `OcaBlock(ONo, ONo)`, `OcaAgent` 方法常量, `OcaWorker` 方法常量, `OcaAppNet` 方法常量

- [ ] **Step 1: 在 oca_test.cpp 顶部添加新 include**

在现有 `#include "oca/classes/control_network.hpp"` 之后添加：

```cpp
#include "oca/classes/agent.hpp"
#include "oca/classes/application_network.hpp"
```

- [ ] **Step 2: 添加 dispatch_agent_methods 测试用例**

在 `dispatch_cm3_network_objects` 用例之后添加：

```cpp
BOOST_AUTO_TEST_CASE(dispatch_agent_methods) {
  // OcaNetwork(4097) 继承 OcaAgent,测试 Agent defLevel=2 方法分派
  oca::OcaNetwork net(4097, 100);  // owner = Root Block
  oca::Session sess(1);
  oca::ocp1::Reader empty(nullptr, 0);
  namespace m = oca::methods;

  // GetLabel(1) -> role() = "Network"
  oca::ocp1::Writer wLabel;
  auto st = net.exec({m::kDefLevelManager, m::kAgentGetLabel}, empty, wLabel, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 1);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wLabel.data(), wLabel.size()).string(),
                     "Network");

  // SetLabel(2) -> no-op OK
  oca::ocp1::Writer wSetLabel;
  st = net.exec({m::kDefLevelManager, m::kAgentSetLabel}, empty, wSetLabel, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 0);

  // GetOwner(3) -> 100 (Root Block)
  oca::ocp1::Writer wOwner;
  st = net.exec({m::kDefLevelManager, m::kAgentGetOwner}, empty, wOwner, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wOwner.data(), wOwner.size()).u32(), 100u);

  // GetPath(4) -> NotImplemented
  oca::ocp1::Writer wPath;
  st = net.exec({m::kDefLevelManager, m::kAgentGetPath}, empty, wPath, sess);
  BOOST_CHECK(st.status == oca::Status::NotImplemented);

  // 未知 Agent 方法 -> NotImplemented
  oca::ocp1::Writer wUnk;
  st = net.exec({m::kDefLevelManager, 99}, empty, wUnk, sess);
  BOOST_CHECK(st.status == oca::Status::NotImplemented);

  // defLevel=1 委托到 OcaRoot 仍正常
  oca::ocp1::Writer wRole;
  st = net.exec({m::kDefLevelRoot, m::kRootGetRole}, empty, wRole, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wRole.data(), wRole.size()).string(),
                     "Network");
}
```

- [ ] **Step 3: 添加 dispatch_appnet_methods 测试用例**

```cpp
BOOST_AUTO_TEST_CASE(dispatch_appnet_methods) {
  // OcaControlNetwork(4098) 继承 OcaApplicationNetwork,
  // 测试 AppNet defLevel=2 方法分派
  oca::OcaControlNetwork ctrl(4098, 100);  // owner = Root Block
  oca::Session sess(1);
  oca::ocp1::Reader empty(nullptr, 0);
  namespace m = oca::methods;

  // GetLabel(1) -> role() = "Control Network"
  oca::ocp1::Writer wLabel;
  auto st = ctrl.exec({m::kDefLevelManager, m::kAppNetGetLabel}, empty, wLabel, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wLabel.data(), wLabel.size()).string(),
                     "Control Network");

  // SetLabel(2) -> no-op OK
  oca::ocp1::Writer wSetLabel;
  st = ctrl.exec({m::kDefLevelManager, m::kAppNetSetLabel}, empty, wSetLabel, sess);
  BOOST_CHECK(st.status == oca::Status::OK);

  // GetOwner(3) -> 100 (Root Block)
  oca::ocp1::Writer wOwner;
  st = ctrl.exec({m::kDefLevelManager, m::kAppNetGetOwner}, empty, wOwner, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wOwner.data(), wOwner.size()).u32(), 100u);

  // GetServiceID(4) -> 空 OcaString (从 OcaControlNetwork 移入 OcaApplicationNetwork)
  oca::ocp1::Writer wSvc;
  st = ctrl.exec({m::kDefLevelManager, m::kAppNetGetServiceID}, empty, wSvc, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wSvc.data(), wSvc.size()).u16(), 0u);

  // GetSystemInterfaces(6) -> 空 List (从 OcaControlNetwork 移入 OcaApplicationNetwork)
  oca::ocp1::Writer wIf;
  st = ctrl.exec({m::kDefLevelManager, m::kAppNetGetSystemInterfaces},
                 empty, wIf, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wIf.data(), wIf.size()).u16(), 0u);

  // GetPath(10) -> NotImplemented
  oca::ocp1::Writer wPath;
  st = ctrl.exec({m::kDefLevelManager, m::kAppNetGetPath}, empty, wPath, sess);
  BOOST_CHECK(st.status == oca::Status::NotImplemented);
}
```

- [ ] **Step 4: 添加 dispatch_worker_label_owner 测试用例**

```cpp
BOOST_AUTO_TEST_CASE(dispatch_worker_label_owner) {
  // OcaBlock(100) 继承 OcaWorker,测试 Worker defLevel=2 新增方法
  oca::OcaBlock block(100);  // root block, owner_ono 默认 0
  oca::Session sess(1);
  oca::ocp1::Reader empty(nullptr, 0);
  namespace m = oca::methods;

  // GetLabel(8) -> role() = "Root Block"
  oca::ocp1::Writer wLabel;
  auto st = block.exec({m::kDefLevelManager, m::kWorkerGetLabel}, empty, wLabel, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 1);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wLabel.data(), wLabel.size()).string(),
                     "Root Block");

  // SetLabel(9) -> no-op OK
  oca::ocp1::Writer wSetLabel;
  st = block.exec({m::kDefLevelManager, m::kWorkerSetLabel}, empty, wSetLabel, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 0);

  // GetOwner(10) -> 0 (根块无 owner)
  oca::ocp1::Writer wOwner;
  st = block.exec({m::kDefLevelManager, m::kWorkerGetOwner}, empty, wOwner, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wOwner.data(), wOwner.size()).u32(), 0u);

  // GetPath(13) -> NotImplemented
  oca::ocp1::Writer wPath;
  st = block.exec({m::kDefLevelManager, m::kWorkerGetPath}, empty, wPath, sess);
  BOOST_CHECK(st.status == oca::Status::NotImplemented);

  // 已有 Worker 方法不受影响
  oca::ocp1::Writer wEnabled;
  st = block.exec({m::kDefLevelManager, m::kWorkerGetEnabled}, empty, wEnabled, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wEnabled.data(), wEnabled.size()).u8(), 1u);
}
```

- [ ] **Step 5: 编译并运行测试**

Run: `cd daemon/build && make oca-test && ./tests/oca-test -p 2>&1 | tail -5`
Expected: 31/31 passed（28 现有 + 3 新增）

- [ ] **Step 6: 提交**

```bash
git add daemon/oca/tests/oca_test.cpp
git commit -m "test(oca): Spec3 - 新增 dispatch_agent_methods/dispatch_appnet_methods/dispatch_worker_label_owner 用例"
```

---

### Task 9: 更新维护手册

**Files:**
- Modify: `docs/ops/oca-design-and-maintenance.md`

**Interfaces:**
- Consumes: Spec3 设计文档、所有 Task 1-8 的实现结果

- [ ] **Step 1: 更新总体架构 — L2 对象类列表和文件表**

将 L2 subgraph 中的 `7 个对象类` 改为 `9 个对象类`（+OcaAgent +OcaApplicationNetwork）。

L2 对象文件行改为：

```
| L2 对象 | `classes/{root,agent,application_network,device_manager,network_manager,subscription_manager,network,control_network}.{hpp,cpp}` | OcaRoot/Agent/Worker/Manager/Block/AppNet/Network/CtrlNet 层次 + 9 个具体对象 |
```

- [ ] **Step 2: 更新继承层次图 — 添加 OcaAgent 和 OcaApplicationNetwork**

在 classDiagram 中添加：

```
class OcaAgent {
    +exec() Status
    +handle_agent() ExecResult
}
class OcaApplicationNetwork {
    +exec() Status
    +handle_appnet() ExecResult
}
OcaRoot <|-- OcaAgent
OcaRoot <|-- OcaApplicationNetwork
OcaAgent <|-- OcaNetwork
OcaApplicationNetwork <|-- OcaControlNetwork
```

删除原有的 `OcaRoot <|-- OcaNetwork` 和 `OcaRoot <|-- OcaControlNetwork`。

- [ ] **Step 3: 更新 ONo 分配表 — 添加 OcaAgent 和 OcaApplicationNetwork 行**

添加两行：

| 对象 | ONo | ClassID | ClassVersion | role | 来源 |
|------|-----|---------|--------------|------|------|
| OcaAgent（抽象） | — | {1,2} | 2 | — | Spec3 中间类 |
| OcaApplicationNetwork（抽象） | — | {1,4} | 1 | — | Spec3 中间类 |

- [ ] **Step 4: 更新各对象已实现方法 — 添加 OcaAgent、OcaApplicationNetwork、OcaWorker 新方法章节**

添加 OcaAgent 章节：

```
#### OcaAgent（DefLevel 2，抽象中间类）

| 方法 | 索引 | 行为 | 来源 |
|------|------|------|------|
| GetLabel | 1 | 返 role() 字符串 | Spec3 |
| SetLabel | 2 | 读可选 OcaString，no-op OK | Spec3 |
| GetOwner | 3 | 返 owner_ono_(u32) | Spec3 |
| GetPath | 4 | NotImplemented | Spec3 |
```

添加 OcaApplicationNetwork 章节（类似格式，列出 6 个方法）。

更新 OcaWorker 章节，添加 GetLabel(8)/SetLabel(9)/GetOwner(10)/GetPath(13)。

更新 OcaControlNetwork 章节，标注"AppNet 方法由 OcaApplicationNetwork 父类处理"。

- [ ] **Step 5: 更新 exec 分派模式 — 添加 classID 前缀匹配说明和委托链示例**

在 exec 分派模式章节补充 OcaAgent 和 OcaApplicationNetwork 的 defLevel 规则：

```
- OcaAgent{1,2} fieldCount=2 → defLevel=2
- OcaApplicationNetwork{1,4} fieldCount=2 → defLevel=2
```

- [ ] **Step 6: 更新 Spec 阶段与合规状态 — 添加 Spec3 行**

在已完成阶段表添加：

| 阶段 | 目标 | 测试结果 | commit 范围 |
|------|------|---------|-------------|
| Spec3（**test5 改善**） | OcaAgent/AppNet 中间类 + Worker 方法补齐 | oca-test 31/31，test5 改善 | `<first>..`<last>` |

- [ ] **Step 7: 更新测试用例分布表**

将 26 改为 31，L2 单测行添加 `dispatch_agent_methods`、`dispatch_appnet_methods`、`dispatch_worker_label_owner`。

- [ ] **Step 8: 更新已知限制与待办 — 移除已实现项**

从"Spec3 范围外"列表中移除"OcaAgent 方法（GetLabel/SetLabel/GetOwner/GetPath）——test5 失败根因"（已实现）。

添加新的待办项（如有）：Worker 端口/延迟方法、AppNet 状态/控制方法（仍为 Spec3 范围外）。

- [ ] **Step 9: 更新参考资源 — 添加 Spec3 设计文档**

添加：

```
- Spec3 设计文档：`docs/superpowers/specs/aes70-oca-spec3-design.md`
- Spec3 实现计划：`docs/superpowers/plans/aes70-oca-spec3-phase1-plan.md`
```

- [ ] **Step 10: 提交**

```bash
git add docs/ops/oca-design-and-maintenance.md
git commit -m "docs(oca): 维护手册同步 Spec3 — OcaAgent/AppNet 中间类 + Worker 方法补齐"
```

---

### Task 10: 最终验证与 Spec3 验收提交

**Files:**
- 无新文件变更

- [ ] **Step 1: 完整构建**

Run: `cd daemon && cmake -DWITH_OCA=ON -DWITH_AVAHI=OFF -DFAKE_DRIVER=ON -DWITH_STREAMER=OFF . && make oca-test aes67-daemon 2>&1 | tail -5`
Expected: 编译成功

- [ ] **Step 2: 全量测试**

Run: `./daemon/tests/oca-test -p 2>&1 | tail -5`
Expected: 31/31 passed

- [ ] **Step 3: E2E 验收测试**

Run: `./daemon/tests/oca-test -p -t oca_e2e_acceptance 2>&1`
Expected: PASSED

- [ ] **Step 4: Spec3 验收提交（如所有测试通过）**

```bash
git commit --allow-empty -m "docs(oca): Spec3 验收 - OcaAgent/AppNet 中间类 + Worker 方法补齐, oca-test 31/31"
```

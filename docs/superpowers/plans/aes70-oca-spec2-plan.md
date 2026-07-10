# AES67 daemon AES70/OCA Spec2 实施计划(阶段一)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 闭环 Spec1 遗留的协议 bug 与有硬证据的合规缺口,使官方合规工具对象合规测试通过(或经 MITM 坐实剩余失败为 2023 方法缺失 G3/G4/G6/G7/G8,归阶段二)。

**Scope:** 本计划仅覆盖**阶段一**(7 项缺口 G0/G11/G1/G2/G9/G10/G12,均有硬证据)。阶段二(G3/G4/G6/G7/G8,索引已坐实)另起计划。

**Architecture:** 不改 Spec1 四层架构,仅在现有 `daemon/oca/` 内修正常量/分派/入参。无新文件、无新依赖。

**Tech Stack:** C++17、Boost.Test、CMake(`WITH_OCA`)。无新外部依赖。

**基于:** `docs/superpowers/specs/aes70-oca-spec2-design.md`(权威设计文档,§A.1.1/§B/§C 为事实依据)。

## 规格来源与勘误

本计划基于 Spec2 设计文档,所有方法索引/ClassID/ONo 均已 Phase 0 坐实(OCAMicro 编译期常量 + sphinx 2024 RST)。权威优先级:OCAMicro 编译期常量(ClassID 类树)> sphinx 2024 RST(方法索引)> AES70 PDF 正文 > 记忆。

**Phase 0 坐实的关键事实(实施依据):**

1. **ClassID 类树**(OCAMicro `#define`):`OCA_AGENT_CLASSID=ROOT,2`(1.2=Agent)、`OCA_MANAGER_CLASSID=ROOT,3`(1.3=Manager)、`OCA_WORKER_CLASSID=ROOT,1`。Manager 子树:`DeviceManager={1,3,1}`、`SubscriptionManager={1,3,4}`、`NetworkManager={1,3,6}`、`Block={1,1,3}`(Worker 子树)。
2. **EV2 订阅方法索引**(sphinx 2024,defLevel=3):`AddSubscription2=3.8`、`RemoveSubscription2=3.9`、`AddPropertyChangeSubscription2=3.10`、`RemovePropertyChangeSubscription2=3.11`。AddSubscription2 入参:`OcaEvent{EmitterONo(u32),EventID(u16,u16)} + NotificationDeliveryMode(u8) + NetworkAddress`(无 Subscriber/Context,EV2 简化)。
3. **DeviceManager 方法**(sphinx 2024):`GetState=3.13`(v3 deprecated)、`GetOperationalState=3.23`、`GetManufacturer=3.21`、`GetProduct=3.22`。
4. **ONo**(AES70-1 §8.2 + Table 3):ONo 2=OcaSecurityManager(标准)、ONo 6=OcaNetworkManager(标准,optional)。Spec1 把 NetworkManager 错放 ONo 2。
5. **类版本**(OCAMicro 2018):四类均 2。用户决策暂用 2018 值(设计文档决策 #9)。
6. **非强制方法语义**(AES70-2 Annex B.3.0):存在但未实现的方法返回 `NotImplemented`,非法索引返回 `BadMethod`。

**Spec1 被 Phase 0 揭露的协议 bug(本计划修复):**
- **G0**:三 Manager ClassID 标到 Agent 子树({1,2,x}),DeviceManager 自报 {1,2,1}=OcaNetwork -> 对象合规测试首要根因。
- **G11**:EV2 订阅方法索引全错({1,2,3,4}),正确 {8,9,10,11}。AddSubscription2 入参也错(多读了 ctx,缺 DeliveryMode/NetworkAddress)。被 oca-probe/daemon 共用错误常量自洽通过(自测盲点)。
- **G12**:用 deprecated 的 GetState(3.13),应迁移 GetOperationalState(3.23)。

## Global Constraints

- C++17,2 空格缩进,无 tab,`PointerAlignment: Left`,80 列,遵循 `daemon/.clang-format`(advisory)。
- 仅改 `daemon/oca/` 内现有文件 + `daemon/oca/tests/oca_test.cpp` + `daemon/oca/tools/oca_probe.cpp`。不新增文件、不碰 SessionManager/DriverManager。
- 编解码 big-endian,逐字段 stream 读写(沿用 Spec1)。
- 每个任务结束 commit 一次,中文提交信息。
- oca-test 在 `buildfake.sh` 路径下必须全绿。
- **自测盲点防范**:G11 修正后,oca-probe 必须同步改用正确索引,且其订阅验证应断言"对错误索引返回 BadMethod",确保 oca-probe 与 daemon 不再共谋。

## 文件结构

**仅修改(无新建):**

| 文件 | 改动 |
|------|------|
| `daemon/oca/methods.hpp` | ClassID 注释更正;EV2 订阅常量 {1,2,3,4}->{8,9,10,11};新增 GetOperationalState=23 常量 |
| `daemon/oca/classes/device_manager.cpp` | ClassID {1,2,1}->{1,3,1};class_version 4->2;GetState(13)->GetOperationalState(23) |
| `daemon/oca/classes/device_manager.hpp` | class_version 4->2;GetState 声明更名 GetOperationalState |
| `daemon/oca/classes/network_manager.cpp` | ClassID {1,2,3}->{1,3,6};class_version 3->2 |
| `daemon/oca/classes/network_manager.hpp` | class_version 3->2 |
| `daemon/oca/classes/subscription_manager.cpp` | ClassID {1,2,4}->{1,3,4};AddSubscription2 入参对齐 sphinx 2024(去 ctx,加 DeliveryMode/NetworkAddress) |
| `daemon/oca/classes/subscription_manager.hpp` | (若有入参相关声明同步) |
| `daemon/oca/oca_server.cpp` | NetworkManager ONo 2->6(G1/G2) |
| `daemon/oca/classes/root.cpp` | GetMembers 的 objects_in_range 范围确认(若 ONo 变更影响) |
| `daemon/oca/tools/oca_probe.cpp` | EV2 订阅改用正确索引;AddSubscription2 请求参数对齐;新增错误索引断言 |
| `daemon/oca/tests/oca_test.cpp` | 全部受影响断言更新(ClassID/版本/ONo/索引/GetState->GetOperationalState) |

## 任务依赖与构建命令

任务按 T1->T8 顺序。T1(ClassID)是基础,T2(EV2 索引)独立,T3(ONo)依赖 T1,T4(版本)/T5(GetOperationalState)/T6(NotImplemented)独立,T7(oca-probe)依赖 T2,T8 验证依赖全部。

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
cmake \
  -DCPP_HTTPLIB_DIR="../3rdparty/cpp-httplib" \
  -DRAVENNA_ALSA_LKM_DIR="../3rdparty/ravenna-alsa-lkm" \
  -DENABLE_TESTS=ON -DWITH_OCA=ON \
  -DWITH_AVAHI=OFF -DFAKE_DRIVER=ON -DWITH_STREAMER=OFF .
make oca-test aes67-daemon oca-probe
./tests/oca-test -p                  # 或 ./oca-test -p(视目标位置)
```

带 mDNS 的本地验证:`-DWITH_AVAHI=ON`(需 avahi-daemon)。真实控制器验收见 Task 8。

---

## Task 1: G0 修正三 Manager ClassID({1,2,x}->{1,3,x})

**Files:**
- Modify: `daemon/oca/classes/device_manager.cpp`(ClassID 常量)
- Modify: `daemon/oca/classes/network_manager.cpp`(ClassID 常量)
- Modify: `daemon/oca/classes/subscription_manager.cpp`(ClassID 常量)
- Modify: `daemon/oca/methods.hpp`(DefLevel 注释更正)
- Modify: `daemon/oca/tests/oca_test.cpp`(ClassID 断言)

**Interfaces:**
- Produces: 三 Manager 报正确 ClassID;OcaBlock 不变({1,1,3})。
- Consumes: 设计文档 §A.1.1。

- [ ] **Step 1: 更新 methods.hpp DefLevel 注释**

`methods.hpp:22-27` 注释把 `{1,2,x}` 更正为 `{1,3,x}`:
```cpp
constexpr uint16_t kDefLevelRoot       = 1;  // OcaRoot {1}
constexpr uint16_t kDefLevelManager    = 2;  // OcaManager {1,3} / OcaWorker {1,1}
constexpr uint16_t kDefLevelDeviceMngr = 3;  // OcaDeviceManager {1,3,1}
constexpr uint16_t kDefLevelBlock      = 3;  // OcaBlock {1,1,3}
constexpr uint16_t kDefLevelNetworkMngr = 3; // OcaNetworkManager {1,3,6}
constexpr uint16_t kDefLevelSubMngr    = 3;  // OcaSubscriptionManager {1,3,4}
```
注:`kDefLevelManager` 值仍为 2(这是类树**层级深度**,不是 ClassID 第二级;Manager 在第 2 层)。注释仅澄清 ClassID 数值。

- [ ] **Step 2: 修正三 Manager ClassID 常量**

`device_manager.cpp:11`: `{{{1, 2, 1}}, 4}` -> `{{{1, 3, 1}}, 4}`(版本 T4 再改,本步只改 ClassID)
`network_manager.cpp:10`: `{{{1, 2, 3}}, 3}` -> `{{{1, 3, 6}}, 3}`
`subscription_manager.cpp:13`: `{{{1, 2, 4}}, 2}` -> `{{{1, 3, 4}}, 2}`
`root.cpp:11` 的 kBlockClassId 不变({1,1,3})。

- [ ] **Step 3: 更新 oca_test ClassID 断言**

`oca_test.cpp` 中所有 `{1,2,1}`/`{1,2,3}`/`{1,2,4}` 断言(行 435/438/460/731/738/745 等)改为 `{1,3,1}`/`{1,3,6}`/`{1,3,4}`。逐处核对上下文(注释里的类名)。

- [ ] **Step 4: 构建测试**
```bash
make oca-test && ./tests/oca-test -p
```
预期全绿(版本号 T4 才改,本步 ClassID 断言已同步)。

- [ ] **Step 5: commit** `fix(oca): G0 修正三 Manager ClassID 至 Manager 子树 {1,3,x}`

---

## Task 2: G11 修正 EV2 订阅方法索引与入参

**Files:**
- Modify: `daemon/oca/methods.hpp`(EV2 常量 {1,2,3,4}->{8,9,10,11})
- Modify: `daemon/oca/classes/subscription_manager.cpp`(AddSubscription2 入参对齐 sphinx 2024)
- Modify: `daemon/oca/tests/oca_test.cpp`(索引 + 入参断言)

**Interfaces:**
- Produces: EV2 订阅方法用正确索引;AddSubscription2 入参 = OcaEvent+DeliveryMode+NetworkAddress。
- Consumes: 设计文档 §C.1(sphinx 2024)。

- [ ] **Step 1: 更新 methods.hpp EV2 常量**

`methods.hpp:54-57`:
```cpp
constexpr uint16_t kSubAddSubscription2                = 8;   // sphinx 2024 3.8
constexpr uint16_t kSubRemoveSubscription2             = 9;   // 3.9
constexpr uint16_t kSubAddPropertyChangeSubscription2  = 10;  // 3.10
constexpr uint16_t kSubRemovePropertyChangeSubscription2 = 11; // 3.11
```
去掉"候选值,需 XMI 校验"注释(已坐实)。

- [ ] **Step 2: 对齐 AddSubscription2 入参(sphinx 2024)**

`subscription_manager.cpp:37-52` `AddSubscription2` 入参从 `emitter(u32)+EventID(u16,u16)+blob(ctx)` 改为 `OcaEvent{EmitterONo(u32)+EventID(u16,u16)} + NotificationDeliveryMode(u8) + NetworkAddress`。

**NotificationDeliveryMode**(sphinx 2024, u8 枚举):`Normal=1`(2023 名,2018 叫 Reliable)、`NetworkFast=2` 等。**注意是 1,非 0**。Spec1 daemon 仅支持 Normal(1),Local 模式。

**NetworkAddress**(ocac/OCAMicro `OcaLiteNetworkAddress::Unmarshal` 已坐实):OCP.1 编码就是**一个 OcaBlob**(u16 count + bytes,内部 `m_value` 是 `OcaLiteBlob`)。故解码直接 `req.blob()` 读取消费即可,无需处理多态 selector。Spec1 daemon 仅支持 Normal(同 TCP 连接回送通知),读入的 DestinationInformation blob 可忽略(仅消费)。

保留订阅记录逻辑:`entries_.push_back({id, &sess, emitter, eid})`(不再存 ctx,EV2 无 ctx)。
返回 subscriptionID(u32)不变,{OK, 1}。

**RemoveSubscription2(3.9)入参**(sphinx 2024 已坐实):`OcaEvent + DeliveryMode + NetworkAddress`,语义"移除所有匹配订阅"(非按 subscriptionID)。当前实现读 `u32(id)` **错误**。修正:读 Event+DeliveryMode+NetworkAddress,遍历 entries_ 移除所有匹配,返回 {OK, 0}。

- [ ] **Step 3: 更新 oca_test 订阅断言**

`oca_test.cpp:480-530`(AddSubscription2 请求构造)与 `884-895`(E2E 订阅):请求参数改为 OcaEvent+DeliveryMode+NetworkAddress;RemoveSubscription2 改为按 event。新增断言:**错误索引(如 methodIndex=1)返回 BadMethod**,确认不再误命中 EV1。

- [ ] **Step 4: 构建测试**
```bash
make oca-test && ./tests/oca-test -p
```

- [ ] **Step 5: commit** `fix(oca): G11 修正 EV2 订阅方法索引至 {8,9,10,11} 并对齐入参`

---

## Task 3: G1/G2 修正 NetworkManager ONo(2->6)

**Files:**
- Modify: `daemon/oca/oca_server.cpp`(实例化 ONo)
- Modify: `daemon/oca/tests/oca_test.cpp`(ONo 断言 + GetMembers/GetManagers 列表)

**Interfaces:**
- Produces: NetworkManager 在 ONo 6;ONo 2 空置(不实例化 SecurityManager,optional)。
- Consumes: 设计文档 §A.2 Table 3。

- [ ] **Step 1: 修改 oca_server.cpp 实例化 ONo**

`oca_server.cpp:28`: `auto* nm = new OcaNetworkManager(2);` -> `new OcaNetworkManager(6);`

- [ ] **Step 2: 确认 GetMembers/GetManagers 范围**

`device_manager.cpp:76` 与 `root.cpp:69` 用 `objects_in_range(1, 99)`。ONo 6 仍在范围内,无需改范围。GetMembers/GetManagers 返回列表变为 [1,4,6](原 [1,2,4])。

- [ ] **Step 3: 更新 oca_test ONo 断言**

`oca_test.cpp` 中 ONo=2 NetworkManager 的断言(行 738 等)改为 ONo=6;GetMembers/GetManagers 列表 [1,2,4]->[1,4,6]。StubObject/registry 相关测试同步。

- [ ] **Step 4: 构建测试**
```bash
make oca-test && ./tests/oca-test -p
```

- [ ] **Step 5: commit** `fix(oca): G1/G2 NetworkManager 迁至标准 ONo 6(原误占 ONo 2=SecurityManager)`

---

## Task 4: G10 类版本号降至 2(2018 值)

**Files:**
- Modify: `daemon/oca/classes/device_manager.hpp`(class_version 4->2)
- Modify: `daemon/oca/classes/network_manager.hpp`(class_version 3->2)
- Modify: `daemon/oca/tests/oca_test.cpp`(版本断言)

**Interfaces:**
- Produces: 四类版本均为 2(SubscriptionManager/Block 已是 2)。
- Consumes: 设计文档 §A.1.1 + 决策 #9。

- [ ] **Step 1: 修改 class_version**

`device_manager.hpp:26`: `return 4;` -> `return 2;`
`network_manager.hpp:14`: `return 3;` -> `return 2;`
(SubscriptionManager/Block 已是 2,不改)

- [ ] **Step 2: 更新 oca_test 版本断言**

`oca_test.cpp` 中 v4/v3 断言(行 731/738/745 注释 "v4"/"v3")改为 v2。

- [ ] **Step 3: 构建测试**
```bash
make oca-test && ./tests/oca-test -p
```

- [ ] **Step 4: commit** `fix(oca): G10 类版本号统一为 2(OCAMicro 2018 值)`

---

## Task 5: G12 GetState(3.13)->GetOperationalState(3.23)

**Files:**
- Modify: `daemon/oca/methods.hpp`(新增 kDevGetOperationalState=23;kDevGetState 标 deprecated)
- Modify: `daemon/oca/classes/device_manager.hpp`(GetState 更名 GetOperationalState)
- Modify: `daemon/oca/classes/device_manager.cpp`(分派 + 方法名)
- Modify: `daemon/oca/tests/oca_test.cpp`(GetState 断言)

**Interfaces:**
- Produces: DeviceManager 响应 3.23(GetOperationalState);3.13(GetState)保留返回 NotImplemented 或同样实现(spec 允许)。
- Consumes: 设计文档 §C.1。

- [ ] **Step 1: methods.hpp 加常量**

```cpp
constexpr uint16_t kDevGetState             = 13;  // deprecated (v3), 被 GetOperationalState 取代
constexpr uint16_t kDevGetOperationalState  = 23;  // sphinx 2024
```

- [ ] **Step 2: device_manager 分派 + 方法**

`device_manager.cpp` 分派加 `case kDevGetOperationalState: return GetOperationalState(rsp);`。

**GetOperationalState 返回结构**(sphinx 2024 已坐实):`OcaDeviceOperationalState` 是**结构体**,非简单 u8:
- `OcaDeviceGenericState Generic`(u8 枚举):`NormalOperation=0`、`Initializaing=1`、`Updating=2`...(sphinx 2024)。**注意:标准 NormalOperation=0,与我们 Spec1 `DeviceState::Operational=2` 数值不同**。GetOperationalState 应写 u8(0=NormalOperation)。
- `OcaBlob Details`(变长,设备特定细节,optional):Spec1 写空 blob(u16 count=0)。

故 GetOperationalState 编码:`rsp.u8(0/*NormalOperation*/); rsp.u16(0)/*空 Details blob*/;` 返回 {OK, 1}(1 个结构化参数)。

**GetState(3.13)**:sphinx 2024 标 v3 deprecated,返回 `OcaDeviceState`(u8)。Spec1 已实现(返回 Operational=2)。**保留不动**(spec 允许 deprecated 方法仍实现;2018 工具可能仍调它)。也可考虑让 GetState 也返回标准值,但 `OcaDeviceState` 与 `OcaDeviceGenericState` 是不同枚举--保留 Spec1 现状,不动 GetState。

**保守做法**:3.13(GetState)保留 Spec1 实现;3.23(GetOperationalState)新增,返回 OcaDeviceOperationalState 结构体。

- [ ] **Step 3: 更新 oca_test**

`oca_test.cpp:54` `BOOST_CHECK_EQUAL(m::kDevGetState, 13);` 保留;新增 `kDevGetOperationalState==23` 断言;行 419-420 GetState 测试加 GetOperationalState 对应测试。

- [ ] **Step 4: 构建测试**
```bash
make oca-test && ./tests/oca-test -p
```

- [ ] **Step 5: commit** `fix(oca): G12 新增 GetOperationalState(3.23),GetState(3.13) deprecated`

---

## Task 6: G9 非强制方法返回 NotImplemented

**Files:**
- Modify: `daemon/oca/classes/device_manager.cpp`(default 分支)
- Modify: `daemon/oca/classes/network_manager.cpp`(default 分支)
- Modify: `daemon/oca/classes/subscription_manager.cpp`(default 分支)
- Modify: `daemon/oca/classes/root.cpp`(handle_root default)
- Modify: `daemon/oca/tests/oca_test.cpp`(语义断言)

**Interfaces:**
- Produces: 类内已知但未实现的方法返回 NotImplemented;非法/未知方法索引返回 BadMethod。
- Consumes: 设计文档 §B G9 + Annex B.3.0。

**关键判断(实施前明确):** Annex B.3.0 "Non-mandatory methods shall nevertheless be present in the device model, but shall return a NotImplemented result." 这指**类定义中存在但本设备未实现的方法**(如 DeviceManager 的 SetDeviceName=5,我们没实现)。而**完全不在类定义中的索引**(如 DeviceManager methodIndex=99)应返回 BadMethod。

- [ ] **Step 1: 区分 NotImplemented vs BadMethod**

对每个类,把"类定义中存在但未实现"的方法索引显式 case 返回 `{Status::NotImplemented, 0}`;default 仍返回 `{Status::BadMethod, 0}`。

DeviceManager(3.x,已实现 1/3/4/6/13/19/23):未实现但存在的有 2(GetModelGUID)、5(SetDeviceName)、14(SetResetKey)等。**保守起见**:只对 Annex B 强制相关或明确存在的高频方法标 NotImplemented,其余走 default BadMethod。或更简单:default 改为 NotImplemented(因大多数合法索引都"存在")。

**实施决策**:default 分支返回 `{Status::NotImplemented, 0}`(合法但未实现),仅在确定索引越界时 BadMethod。但 OCP.1 无法从单 methodIndex 判断"是否在类定义中",故**统一 default = NotImplemented** 更安全(符合 B.3.0 精神:未知方法视为"存在但未实现")。BadMethod 仅用于 defLevel 不匹配(已有逻辑)。

- [ ] **Step 2: 修改各类 default 分支**

把 `return {Status::BadMethod, 0};`(default)改为 `return {Status::NotImplemented, 0};`。涉及 device_manager.cpp:37、network_manager.cpp:27、subscription_manager.cpp:31、root.cpp:36(handle_root default)。

- [ ] **Step 3: 更新 oca_test**

`oca_test.cpp` 中"未知方法 -> BadMethod"的断言改为 NotImplemented。核对 dispatch_unknown_method 等测试用例。

- [ ] **Step 4: 构建测试**
```bash
make oca-test && ./tests/oca-test -p
```

- [ ] **Step 5: commit** `fix(oca): G9 非强制方法返回 NotImplemented(Annex B.3.0)`

---

## Task 7: G11 配套 - 更新 oca-probe(消除自测盲点)

**Files:**
- Modify: `daemon/oca/tools/oca_probe.cpp`(EV2 索引 + 入参 + 错误索引断言)

**Interfaces:**
- Produces: oca-probe 用正确 EV2 索引/入参;新增"错误索引返回 BadMethod/NotImplemented"断言。
- Consumes: Task 2 结果。

- [ ] **Step 1: oca-probe 订阅调用改正确索引/入参**

`oca_probe.cpp:475` AddSubscription2 调用:methodIndex 改 kSubAddSubscription2(现=8);请求参数改为 OcaEvent+DeliveryMode+NetworkAddress(对齐 Task 2)。

- [ ] **Step 2: 新增错误索引断言(防范自测盲点)**

oca-probe 增加一个探测:用**错误索引**(如 methodIndex=1,旧值)调 SubscriptionManager,断言返回 NotImplemented 或 BadMethod(**非 OK**)。这确保 oca-probe 不再与 daemon 共用错误常量而误判通过。

- [ ] **Step 3: 构建并跑 oca-probe**
```bash
make oca-probe
# 起 daemon(另一终端),跑:
./oca-probe 127.0.0.1 65037
```
预期:AddSubscription2 用正确索引成功;错误索引失败。

- [ ] **Step 4: commit** `test(oca): oca-probe 用正确 EV2 索引,新增错误索引断言防自测盲点`

---

## Task 8: 阶段一验证(oca-test + MITM + 真实控制器)

**Files:** 无(验证任务)

- [ ] **Step 1: oca-test 全绿**
```bash
make oca-test && ./tests/oca-test -p
```
22+ 用例全绿(含 T1-T7 新增/修改断言)。

- [ ] **Step 2: oca-probe 端到端**

起 daemon(`oca-daemonctl.sh start` 或手动),跑 oca-probe,确认:发现/身份/订阅(正确索引)全过,错误索引断言生效。

- [ ] **Step 3: MITM 抓包确认字节**

起 MITM 代理(`daemon/oca/tools/oca-mitm.py`)+ avahi-publish-service 重定向。用 oca-probe 经 MITM 连 daemon,抓 GetClassIdentification/GetManagers/GetMembers/AddSubscription2 响应字节,确认:
- ClassID = {1,3,1}/{1,3,4}/{1,3,6}(非 {1,2,x})
- EV2 订阅响应 methodIndex=8 路径正确
- GetOperationalState(3.23)响应

- [ ] **Step 4: 真实控制器重跑(用户执行)**

用户在 Win 跑 Aes70CompliancyTestTool,MITM 抓包。预期:对象合规测试从失败转通过(G0 修正 DeviceManager 不再自报 OcaNetwork);订阅测试若之前失败应转通过(G11)。记录通过项数(目标:>=3/5,理想 5/5 或坐实剩余为 G3/G4/G6/G7/G8 缺失)。

- [ ] **Step 5: 记录验证结果到设计文档 + 记忆**

更新 `aes70-oca-spec2-design.md` §C 的 C5 状态(2018 工具实际拒了什么 -> 已坐实);更新记忆 `aes70-oca-spec1.md`。

- [ ] **Step 6: commit** `test(oca): Spec2 阶段一验证通过 - 对象合规测试结果记录`

---

## 阶段一完成标志

- G0/G11/G1/G2/G9/G10/G12 全部闭环,oca-test 全绿。
- 真实控制器对象合规测试通过,或剩余失败经 MITM 坐实为 G3/G4/G6/G7/G8(2023 方法缺失,归阶段二)。
- oca-probe 自测盲点消除(错误索引断言生效)。

## 阶段二预告(另起计划)

G3(GetProduct 3.22)、G4(GetManufacturer 3.21)、G6(AddPropertyChangeSubscription2 3.10)、G7(RemovePropertyChangeSubscription2 3.11)、G8(event PropertyChanged)。索引均已 sphinx 2024 坐实,可立即实施。

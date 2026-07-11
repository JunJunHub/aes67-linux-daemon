# AES67 daemon AES70/OCA Spec2 实施计划(阶段二)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在阶段一(真实控制器 3/5)基础上,补齐 2018 工具强制方法 + EV1/EV2 订阅 + 2023 GetProduct/GetManufacturer,使 **OCC Object Compliancy 测试转过(目标 4/5)**,坚守 AES70-2023 Annex B 基准(不新增 CM3 弃用对象)。

**Scope:** 本计划覆盖**阶段二**(用户选定 Option A:补方法 + EV1 订阅,不加 CM3)。阶段一(G0/G11/G1/G2/G9/G10/G12)已完成,见 `aes70-oca-spec2-phase1-plan.md`。

**Architecture:** 不改 Spec1 四层架构,仅在现有 `daemon/oca/` 内新增方法分派 + 实现。无新文件、无新依赖。

**Tech Stack:** C++17、Boost.Test、CMake(`WITH_OCA`)。无新外部依赖。

**基于:** `docs/superpowers/specs/aes70-oca-spec2-design.md`(权威设计文档,§C5 阶段一验证结果 + 阶段二根因为事实依据)。

## 规格来源与勘误

权威优先级:**OCAMicro 编译期 C++ 源码(EV1/Lock/GetModelGUID 等签名,最硬)** > **sphinx 2024 RST(EV2 PropertyChange2/GetProduct/GetManufacturer + 方法 id)** > **2018 工具参考表(强制方法判定 + 通过判据)** > AES70 PDF 正文 > 记忆。

OCAMicro 源码位置:`/home/Share/GitHub/oca-tools-probe/OCAMicro/OCAMicro/Src/common/OCALite/OCC/`。
sphinx 源码位置:`/home/Share/GitHub/AES70-OCC-sphinx/source/`。

**阶段一真实控制器验证(2026-07-10,Aes70CompliancyTestTool v2.0.1 AES70-2018)结论:3/5 通过。** 剩余 2 项失败根因经 MITM 字节级坐实(见设计文档 §C5):

1. **强制方法未实现返回 NotImplemented(result 8)**:2018 工具对 mandatory 方法要求实现(result≠8),未实现即 ERROR。
2. **★★ 事件订阅失败**:2018 工具用 **EV1 AddSubscription(mi=1)** 订阅 PropertyChanged(及 SubscriptionManager 的 NotificationDisabled/SynchronizeState 事件),daemon 只实现 EV2(mi=8/9),EV1 mi=1/2/5/6 未实现 -> result 8。**实现 EV1 订阅是事件测试通过的枢纽**。
3. **"Missing mandatory object OcaBlock/OcaNetwork/OcaControlNetwork"**:2018 完整对象模型 vs 2023 Annex B 最小模型差异。CM3 网络类 2023 弃用;用户定 Option A 不新增。

**通过判据(关键):** 2018 工具对 mandatory 方法/事件,`result==8(NotImplemented)` -> ERROR;`result!=8`(OK/InvalidRequest 等) -> "implements" 通过。故强制方法返回 OK(或语义合理的非 8 状态)即过;事件订阅 Add*Subscription 返回 OK 即过"事件已实现"判定(实际通知投递非工具判定项)。

**OCC Object Compliancy(test 5)无 missing-object 错误**(它测对象 1/4/6 的方法+事件),补齐强制方法 + EV1 订阅即可转过 -> 4/5。Minimum object compliancy(test 4)即使补齐方法仍因缺 CM3 对象失败 -> 5/5 需新增弃用对象,不在本阶段。

## 阶段二缺口范围(Option A,已坐实签名)

| 类别 | 方法(索引) | 入参(顺序) | 响应 | 来源 | 2018强制 | 2023AnnexB |
|------|------------|------------|------|------|---------|-----------|
| EV1 订阅 | AddSubscription(1) | OcaEvent{EmitterONo u32,EventID{u16,u16}}+OcaMethod{ONo u32,MethodID{u16,u16}}+OcaBlob(ctx)+DeliveryMode u8+NetworkAddress(blob) | 0 参 {OK,0} | OCAMicro | ✓ | deprecated |
| EV1 订阅 | RemoveSubscription(2) | OcaEvent+OcaMethod | 0 参 {OK,0} | OCAMicro | ✓ | deprecated |
| EV1 订阅 | AddPropertyChangeSubscription(5) | EmitterONo u32+PropertyID{u16,u16}+OcaMethod{u32,u16,u16}+OcaBlob+DeliveryMode u8+NetworkAddress(blob) | 0 参 {OK,0} | OCAMicro | ✓ | deprecated |
| EV1 订阅 | RemovePropertyChangeSubscription(6) | EmitterONo u32+PropertyID{u16,u16}+OcaMethod{u32,u16,u16} | 0 参 {OK,0} | OCAMicro | ✓ | deprecated |
| EV2 订阅 | AddPropertyChangeSubscription2(10) | EmitterONo u32+PropertyID{u16,u16}+DeliveryMode u8+NetworkAddress(blob) | 0 参 {OK,0} | sphinx | 工具用EV1 | ✓Mandatory(G6) |
| EV2 订阅 | RemovePropertyChangeSubscription2(11) | EmitterONo u32+PropertyID{u16,u16}+DeliveryMode u8+NetworkAddress(blob) | 0 参 {OK,0} | sphinx | 工具用EV1 | ✓Mandatory(G7) |
| OcaRoot | Lock(3) | 无 | 0 参 {OK,0} | OCAMicro | ✓ | ✓ |
| OcaRoot | Unlock(4) | 无 | 0 参 {OK,0} | OCAMicro | ✓ | ✓ |
| DeviceManager | GetModelGUID(2) | 无 | OcaModelGUID(8 原始字节)+{OK,1} | OCAMicro+sphinx | ✓ | deprecated(v3) |
| DeviceManager | GetEnabled(11) | 无 | OcaBoolean u8(1)+{OK,1} | OCAMicro | ✓ | ✓ |
| DeviceManager | SetEnabled(12) | OcaBoolean u8 | 0 参 {OK,0} | OCAMicro | ✓ | ✓ |
| DeviceManager | GetDeviceRevisionID(20) | 无 | OcaString+{OK,1} | sphinx | ✓ | deprecated(v3) |
| DeviceManager | GetManufacturer(21) | 无 | OcaManufacturer+{OK,1} | sphinx | 2018无 | ✓Mandatory(G4) |
| DeviceManager | GetProduct(22) | 无 | OcaProduct+{OK,1} | sphinx | 2018无 | ✓Mandatory(G3) |
| NetworkManager | GetStreamNetworks(2) | 无 | List<ONo> 空+{OK,1} | OCAMicro | ✓ | ✓ |
| NetworkManager | GetControlNetworks(3) | 无 | List<ONo> 空+{OK,1} | OCAMicro | ✓ | ✓ |
| NetworkManager | GetMediaTransportNetworks(4) | 无 | List<ONo> 空+{OK,1} | OCAMicro | ✓ | ✓ |

**类型编码(已坐实):**
- **OcaEvent** = EmitterONo(u32) + EventID{defLevel u16, eventIndex u16}(OCAMicro `OcaLiteEvent::Unmarshal`)。
- **OcaMethod** = ONo(u32) + MethodID{defLevel u16, methodIndex u16}(OCAMicro `OcaLiteMethod::Unmarshal`)。subscriber 回调方法,daemon 同会话回送通知,故读入后**忽略**(不用于投递)。
- **PropertyID** = defLevel u16 + propertyIndex u16(OCAMicro `OcaLitePropertyID::Unmarshal`)。
- **NetworkAddress** = OcaBlob(u16 计数 + bytes),daemon 仅消费不解析(同会话回送)。
- **OcaModelGUID** = reserved(1B BlobFixedLen) + mfrCode(3B) + modelCode(4B) = **8 原始字节,BlobFixedLen 无长度前缀**(OCAMicro `OcaLiteModelGUID::Marshal`,常量 RESERVED=1/MFR=3/MODEL=4)。最小实现写 8 零字节(`rsp.u64(0)`,未定义厂商,合规)。
- **OcaOrganizationID** = BlobFixedLen<3> = **3 原始字节**(IEEE OUI/CID,0=未定义)。最小写 3 零字节(`rsp.u8(0); rsp.u16(0);`)。
- **OcaManufacturer** = Name(OcaString) + OrganizationID(3B) + Website(OcaString)(sphinx `Management Datatypes.rst`)。
- **OcaProduct** = Name + ModelID + RevisionLevel + BrandName + UUID + Description,**6 个 OcaString**(sphinx;OcaUUID = OcaString)。
- **List<ONo>** = u16 计数 + N×u32(空列表 = u16(0))。
- **EV1 vs EV2 响应差异**:EV1 AddSubscription 响应**0 参(无 subID)**;EV2 AddSubscription2 响应**1 参(subID)**。本阶段 EV1 四方法均 0 参响应。

## 不在阶段二范围

- **CM3 网络对象**(OcaNetwork/OcaControlNetwork/ApplicationNetwork/MediaTransportNetwork):2023 弃用,Option A 不新增。NetworkManager 的 GetStreamNetworks/GetControlNetworks/GetMediaTransportNetworks **方法**仍实现(返空列表,合规且过工具),但**不实例化**网络对象。
- **Minimum object compliancy(test 4)5/5**:需 CM3 对象 + 解决 "missing OcaBlock"(2018 约定),Option A 接受 4/5。
- **OcaWorker defLevel 2 方法**(GetEnabled(1)/SetEnabled(2)/GetPorts(5),ClassID 1.1):2018 工具未测(Block 非 mandatory 对象,工具未对任何 Worker 对象测 defLevel 2 方法),顺延 Spec3。
- **event PropertyChanged 实际通知投递**:EV1 AddSubscription 返回 OK 即过工具"事件已实现"判定;实际 Notification2 投递到 EV1 订阅者为 bonus(2018 客户端是否解析 Notification2 type=5 未验证),本阶段不强制。SubscriptionManager 事件 NotificationDisabled/SynchronizeState 同理(订阅返回 OK 即过)。
- 媒体桥接、TLS/UDP/WebSocket、Dataset/Patch -> Spec3+。

## Global Constraints

- C++17,2 空格缩进,无 tab,`PointerAlignment: Left`,80 列,遵循 `daemon/.clang-format`(advisory)。
- 仅改 `daemon/oca/` 内现有文件 + `daemon/oca/tests/oca_test.cpp` + `daemon/oca/tools/oca_probe.cpp`。不新增文件(types.hpp 可加结构体声明)。
- 编解码 big-endian,逐字段 stream 读写(沿用 Spec1/阶段一)。
- 每个任务结束 commit 一次,中文提交信息,结尾 `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`。
- oca-test 在 `buildfake.sh` 路径下必须全绿(阶段一 21/21 基线,本阶段新增用例后仍全绿)。
- **自测盲点防范**:每类新方法须有 oca-test 用例;EV1 订阅须断言"响应 0 参(无 subID)",区别于 EV2 的 1 参;oca-probe 须实际调用新方法验证。
- **methods.hpp 编辑注意**:该文件未预格式化,PostToolUse hook 会整体重排。改常量用定向 Bash(perl/sed)保持 diff 最小(阶段一 T1 经验)。

## 文件结构

**仅修改(无新建):**

| 文件 | 改动 |
|------|------|
| `daemon/oca/methods.hpp` | 新增 EV1 订阅常量(1/2/5/6)+ DeviceManager(2/11/12/20/21/22)+ Network(2/3/4)方法常量 |
| `daemon/oca/classes/subscription_manager.cpp` | 实现 EV1 四方法(mi=1/2/5/6)+ EV2 PropertyChange2 两方法(mi=10/11);共享 Entry 记录 |
| `daemon/oca/classes/subscription_manager.hpp` | 新增 6 私有方法声明 |
| `daemon/oca/classes/root.cpp` | handle_root 加 Lock(3)/Unlock(4) 分派,no-op {OK,0} |
| `daemon/oca/classes/device_manager.cpp` | 实现 GetModelGUID(2)/GetEnabled(11)/SetEnabled(12)/GetDeviceRevisionID(20)/GetManufacturer(21)/GetProduct(22);**顺手把 ClassID 常量内嵌 version 4->2**(阶段一 T4 遗留死值,见下注) |
| `daemon/oca/classes/device_manager.hpp` | 新增 6 方法声明 |
| `daemon/oca/classes/network_manager.cpp` | 实现 GetStreamNetworks(2)/GetControlNetworks(3)/GetMediaTransportNetworks(4);**顺手把 ClassID 常量内嵌 version 3->2** |
| `daemon/oca/classes/network_manager.hpp` | 新增 3 方法声明 |
| `daemon/oca/types.hpp` | (可选)新增 OcaManufacturer/OcaProduct 结构体,或内联于 device_manager |
| `daemon/oca/tools/oca_probe.cpp` | 新增 EV1 AddSubscription + Lock/Unlock + GetModelGUID/GetEnabled + Network 方法 + GetProduct/GetManufacturer 探测 |
| `daemon/oca/tests/oca_test.cpp` | 全部新方法用例 + EV1/EV2 响应参差断言 |

> **注(ClassID 常量内嵌 version 死值):** `device_manager.cpp:11` `{{{1,3,1}},4}` 与 `network_manager.cpp:10` `{{{1,3,6}},3}` 的第二字段(version)是阶段一 T4 改 `class_version()` 返回 2 时未同步的遗留死值(`GetClassIdentification` 用虚函数 `class_version()`=2,不用常量字段,故 MITM 实测报 v2 正确)。因 T3/T4 正好改这两行所在文件,顺手对齐为 2,消除误导。属本次修改相关清理(ai-collaboration.md 允许)。

## 任务依赖与构建命令

任务按 T1->T6 顺序。T1(EV1/EV2 订阅,最高价值,事件测试枢纽)最先。T2(Lock)/T3(DevMgr 2018)/T4(Network)独立。T5(DevMgr 2023)与 T3 同文件,顺序执行。T6(oca-probe + 验证)依赖全部。

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
cmake \
  -DCPP_HTTPLIB_DIR="../3rdparty/cpp-httplib" \
  -DRAVENNA_ALSA_LKM_DIR="../3rdparty/ravenna-alsa-lkm" \
  -DENABLE_TESTS=ON -DWITH_OCA=ON \
  -DWITH_AVAHI=OFF -DFAKE_DRIVER=ON -DWITH_STREAMER=OFF .
make oca-test aes67-daemon oca-probe
./tests/oca-test -p
```

真实控制器验收见 Task 6(用户在 Win 执行,MITM 抓包)。

---

## Task 1: EV1 + EV2 订阅方法(mi=1/2/5/6/10/11)

**Files:**
- Modify: `daemon/oca/methods.hpp`(EV1 常量 1/2/5/6)
- Modify: `daemon/oca/classes/subscription_manager.cpp`(6 方法实现)
- Modify: `daemon/oca/classes/subscription_manager.hpp`(6 方法声明)
- Modify: `daemon/oca/tests/oca_test.cpp`(EV1/EV2 用例)

**Interfaces:**
- Produces: SubscriptionManager 实现 EV1 四方法(mi=1/2/5/6)+ EV2 PropertyChange2 两方法(mi=10/11);EV1 响应 0 参,EV2 PropertyChange2 响应 0 参。
- Consumes: OCAMicro `OcaLiteSubscriptionManager.cpp` dispatch(ADD_SUBSCRIPTION/REMOVE_SUBSCRIPTION/ADD_PROPERTY_CHANGE_SUBSCRIPTION/REMOVE_PROPERTY_CHANGE_SUBSCRIPTION)+ sphinx 2024(3.10/3.11)。

**关键设计:** EV1/EV2 PropertyChange 订阅内部都记录为对 PropertyChanged 事件 {EmitterONo, EventID{1,1}} 的订阅(PropertyChanged = OcaRoot event,defLevel 1 eventIndex 1,`kEventPropertyChanged=1`)。复用现有 `entries_`(Entry{id,sess,emitterONo,eventID})+ `trigger_event` 投递基础设施。subscriber ONo/MethodID(EV1)、PropertyID、context、DeliveryMode、NetworkAddress 均读入后**忽略**(daemon 同会话 Notification2 回送,不用 subscriber 回调)。

- [x] **Step 1: methods.hpp 加 EV1 常量**

用定向 perl/sed(避免整体重排)在 EV2 常量块前加:
```cpp
// OcaSubscriptionManager EV1 methods (DefLevel 3) - OCAMicro 校验(deprecated v3)
constexpr uint16_t kSubAddSubscription                   = 1;   // OCAMicro 3.1
constexpr uint16_t kSubRemoveSubscription                = 2;   // 3.2
constexpr uint16_t kSubAddPropertyChangeSubscription     = 5;   // 3.5
constexpr uint16_t kSubRemovePropertyChangeSubscription  = 6;   // 3.6
```
(EV2 kSubAddPropertyChangeSubscription2=10/kSubRemovePropertyChangeSubscription2=11 阶段一已存在,确认保留。)

- [x] **Step 2: subscription_manager.cpp 实现 6 方法**

dispatch switch 加 6 case(mi=1/2/5/6/10/11),均委托新增私有方法。实现要点:

**AddSubscription(mi=1,5 参):** 读 `u32(emitter)+u16+u16(eventID)+u32+u16+u16(OcaMethod,忽略)+blob(ctx,忽略)+u8(deliveryMode,忽略)+blob(networkAddress,忽略)`。记录 `entries_.push_back({next_id_++, &sess, emitter, eid})` + `sess.add_subscription({emitter, eid})`。响应 **0 参**:`return {Status::OK, 0};`(不写 subID,区别 EV2)。

**RemoveSubscription(mi=2,2 参):** 读 `u32+u16+u16(OcaEvent)+u32+u16+u16(OcaMethod,忽略)`。按 {emitter, eid, sess} 移除全部匹配(幂等),`sess.remove_subscription(emitter, eid)`。`return {Status::OK, 0};`。

**AddPropertyChangeSubscription(mi=5,6 参):** 读 `u32(emitter)+u16+u16(PropertyID,忽略)+u32+u16+u16(OcaMethod,忽略)+blob+u8+blob`。记录订阅 PropertyChanged 事件:`EventID pce{1, kEventPropertyChanged}; entries_.push_back({..., emitter, pce}); sess.add_subscription({emitter, pce});`。`return {Status::OK, 0};`。

**RemovePropertyChangeSubscription(mi=6,3 参):** 读 `u32+u16+u16+u32+u16+u16`。按 {emitter, PropertyChanged 事件, sess} 移除。`return {Status::OK, 0};`。

**AddPropertyChangeSubscription2(mi=10,4 参,sphinx):** 读 `u32(emitter)+u16+u16(PropertyID,忽略)+u8(deliveryMode,忽略)+blob(networkAddress,忽略)`。记录 PropertyChanged 事件订阅(同 mi=5)。`return {Status::OK, 0};`(sphinx 规定重复返 InvalidRequest,最小实现忽略去重,返 OK)。

**RemovePropertyChangeSubscription2(mi=11,4 参,sphinx):** 读 `u32+u16+u16+u8+blob`。按 {emitter, PropertyChanged 事件, sess} 移除全部。`return {Status::OK, 0};`。

- [x] **Step 3: subscription_manager.hpp 加 6 声明**

私有方法声明:`AddSubscription/RemoveSubscription/AddPropertyChangeSubscription/RemovePropertyChangeSubscription/AddPropertyChangeSubscription2/RemovePropertyChangeSubscription2`,签名 `(ocp1::Reader&, ocp1::Writer&, Session&)`。

- [x] **Step 4: oca_test 加用例**

- EV1 AddSubscription(mi=1):构造 5 参请求,断言响应 status OK + **nrParameters==0**(无 subID,区别 EV2 的 1)。
- EV1 RemoveSubscription(mi=2):2 参,断言 OK + 0 参 + 幂等(重复调用仍 OK)。
- EV1 AddPropertyChangeSubscription(mi=5):6 参,断言 OK + 0 参;触发 trigger_event(emitter, {1,1}) 后会话收到 Notification2(验证记录生效)。
- EV1 RemovePropertyChangeSubscription(mi=6):3 参,断言 OK + 0 参。
- EV2 AddPropertyChangeSubscription2(mi=10):4 参,断言 OK + 0 参。
- EV2 RemovePropertyChangeSubscription2(mi=11):4 参,断言 OK + 0 参。
- **参差断言**:EV1 AddSubscription 响应 nrParameters=0,EV2 AddSubscription2 响应 nrParameters=1(subID),两者并存验证语义正确。

- [x] **Step 5: 构建测试**
```bash
make oca-test && ./tests/oca-test -p
```
全绿(含新用例)。

- [x] **Step 6: commit** `feat(oca): G6/G7/EV1 实现 SubscriptionManager EV1+EV2 订阅方法(mi=1/2/5/6/10/11)`

---

## Task 2: OcaRoot Lock/Unlock(mi=3/4)

**Files:**
- Modify: `daemon/oca/classes/root.cpp`(handle_root 加分派)
- Modify: `daemon/oca/tests/oca_test.cpp`(Lock/Unlock 用例)

**Interfaces:**
- Produces: OcaRoot 响应 Lock(3)/Unlock(4),no-op 返回 {OK,0}(daemon 不可锁,GetLockable=0,锁定为空操作)。
- Consumes: OCAMicro `OcaLiteRoot::Lock/Unlock`(签名无线上参;不可锁时 OCAMicro 返 InvalidRequest,本实现返 OK no-op,工具通过判据 result≠8 均满足)。

- [x] **Step 1: root.cpp handle_root 加 case**

`handle_root` switch 加:
```cpp
case methods::kRootLock:
  // daemon 不可锁(GetLockable=0),Lock/Unlock 为 no-op;返回 OK
  return {Status::OK, 0};
case methods::kRootUnlock:
  return {Status::OK, 0};
```
(kRootLock=3/kRootUnlock=4 阶段一已定义。)

- [x] **Step 2: oca_test 加用例**

- Lock(ONo 1, defLevel 1, mi=3):断言 OK + 0 参。
- Unlock(ONo 1, defLevel 1, mi=4):断言 OK + 0 参。
- 对 ONo 4/6 同样验证(工具对三对象均测 Lock/Unlock)。

- [x] **Step 3: 构建测试**
```bash
make oca-test && ./tests/oca-test -p
```

- [x] **Step 4: commit** `feat(oca): OcaRoot 实现 Lock/Unlock(mi=3/4,no-op)`

---

## Task 3: DeviceManager 2018 强制方法(GetModelGUID/GetEnabled/SetEnabled/GetDeviceRevisionID)

**Files:**
- Modify: `daemon/oca/methods.hpp`(常量 2/11/12/20)
- Modify: `daemon/oca/classes/device_manager.cpp`(4 方法 + ClassID 常量 version 4->2)
- Modify: `daemon/oca/classes/device_manager.hpp`(4 声明)
- Modify: `daemon/oca/tests/oca_test.cpp`(用例)

**Interfaces:**
- Produces: DeviceManager 响应 GetModelGUID(2)/GetEnabled(11)/SetEnabled(12)/GetDeviceRevisionID(20)。
- Consumes: OCAMicro `OcaLiteDeviceManager.cpp`(GetModelGUID/GetEnabled/SetEnabled)+ sphinx 2024(GetDeviceRevisionID=3.20 返 OcaString)。

- [x] **Step 1: methods.hpp 加常量**

确认/新增(kDevGetModelGUID=2 阶段一已定义;加 11/12/20):
```cpp
constexpr uint16_t kDevGetModelGUID         = 2;   // OCAMicro,deprecated v3
constexpr uint16_t kDevGetEnabled           = 11;  // OCAMicro
constexpr uint16_t kDevSetEnabled           = 12;  // OCAMicro
constexpr uint16_t kDevGetDeviceRevisionID  = 20;  // sphinx 3.20,deprecated v3
```

- [x] **Step 2: device_manager.cpp 实现 4 方法 + 修 ClassID 常量**

**ClassID 常量 version 对齐(顺手):** `device_manager.cpp:11` `{{{1, 3, 1}}, 4}` -> `{{{1, 3, 1}}, 2}`。

dispatch switch 加 4 case。方法实现:

**GetModelGUID(2):** OcaModelGUID = 8 原始字节(reserved 1B + mfrCode 3B + modelCode 4B,BlobFixedLen 无前缀)。最小写 8 零字节:`rsp.u64(0);` 返回 `{Status::OK, 1};`(1 结构化参数)。

**GetEnabled(11):** `rsp.u8(1);`(OcaBoolean,enabled)返回 `{Status::OK, 1};`。

**SetEnabled(12):** `(void)req.u8();`(读 OcaBoolean,忽略;daemon 总 enabled,no-op)返回 `{Status::OK, 0};`。

**GetDeviceRevisionID(20):** `rsp.string(identity_.model_version);` 返回 `{Status::OK, 1};`。

- [x] **Step 3: device_manager.hpp 加 4 声明**

- [x] **Step 4: oca_test 加用例**

- GetModelGUID(mi=2):断言 OK + nrParameters==1 + 响应 8 字节全 0。
- GetEnabled(mi=11):断言 OK + 1 参 + u8==1。
- SetEnabled(mi=12):发 u8(1),断言 OK + 0 参。
- GetDeviceRevisionID(mi=20):断言 OK + 1 参 + 字符串 == model_version。

- [x] **Step 5: 构建测试**
```bash
make oca-test && ./tests/oca-test -p
```

- [x] **Step 6: commit** `feat(oca): DeviceManager 实现 GetModelGUID/GetEnabled/SetEnabled/GetDeviceRevisionID`

---

## Task 4: NetworkManager GetStreamNetworks/GetControlNetworks/GetMediaTransportNetworks(mi=2/3/4)

**Files:**
- Modify: `daemon/oca/methods.hpp`(常量 2/3/4)
- Modify: `daemon/oca/classes/network_manager.cpp`(3 方法 + ClassID 常量 version 3->2)
- Modify: `daemon/oca/classes/network_manager.hpp`(3 声明)
- Modify: `daemon/oca/tests/oca_test.cpp`(用例)

**Interfaces:**
- Produces: NetworkManager 响应三方法,各返回空 List<ONo>(daemon 无网络对象)。
- Consumes: OCAMicro `OcaLiteNetworkManager.cpp`(三方法返 `OcaLiteList<ONo>`)。

**关键判断:** CM3 网络对象 2023 弃用,但 NetworkManager 这三个**方法**仍 mandatory(返网络对象 ONo 列表,可为空)。返空列表既合规(无网络对象)又过工具(result OK≠8)。**不实例化**任何网络对象。

- [x] **Step 1: methods.hpp 加常量**

```cpp
constexpr uint16_t kNetGetStreamNetworks         = 2;  // OCAMicro
constexpr uint16_t kNetGetControlNetworks        = 3;  // OCAMicro
constexpr uint16_t kNetGetMediaTransportNetworks = 4;  // OCAMicro
```
(kNetGetNetworks=1 阶段一已存在,保留;去掉"候选,需 XMI 校验"注释。)

- [x] **Step 2: network_manager.cpp 实现 3 方法 + 修 ClassID 常量**

**ClassID 常量 version 对齐(顺手):** `network_manager.cpp:10` `{{{1, 3, 6}}, 3}` -> `{{{1, 3, 6}}, 2}`。

dispatch switch 加 3 case,各方法:`rsp.u16(0);`(空 List<ONo>)返回 `{Status::OK, 1};`。

- [x] **Step 3: network_manager.hpp 加 3 声明**

- [x] **Step 4: oca_test 加用例**

- GetStreamNetworks(mi=2)/GetControlNetworks(mi=3)/GetMediaTransportNetworks(mi=4):各断言 OK + 1 参 + u16(0) 空列表。

- [x] **Step 5: 构建测试**
```bash
make oca-test && ./tests/oca-test -p
```

- [x] **Step 6: commit** `feat(oca): NetworkManager 实现 GetStreamNetworks/GetControlNetworks/GetMediaTransportNetworks(返空列表)`

---

## Task 5: DeviceManager 2023 方法 GetProduct(22)/GetManufacturer(21)

**Files:**
- Modify: `daemon/oca/methods.hpp`(常量 21/22)
- Modify: `daemon/oca/classes/device_manager.cpp`(2 方法)
- Modify: `daemon/oca/classes/device_manager.hpp`(2 声明)
- Modify: `daemon/oca/types.hpp`(OcaManufacturer/OcaProduct 结构体,可选)
- Modify: `daemon/oca/tests/oca_test.cpp`(用例)

**Interfaces:**
- Produces: DeviceManager 响应 GetManufacturer(3.21)/GetProduct(3.22),2023 Annex B 强制(G3/G4)。2018 工具不测(2023 新增),但实现达 2023 合规。
- Consumes: sphinx 2024(`OcaDeviceManager.rst` 3.21/3.22 + `Management Datatypes.rst` OcaManufacturer/OcaProduct)。

- [x] **Step 1: methods.hpp 加常量**

```cpp
constexpr uint16_t kDevGetManufacturer      = 21;  // sphinx 3.21
constexpr uint16_t kDevGetProduct           = 22;  // sphinx 3.22
```

- [x] **Step 2: device_manager.cpp 实现 2 方法**

dispatch switch 加 2 case。

**GetManufacturer(21):** OcaManufacturer = Name(string) + OrganizationID(3 原始字节) + Website(string)。
```cpp
rsp.string(identity_.manufacturer);  // Name
rsp.u8(0); rsp.u16(0);               // OrganizationID = 3 零字节(未定义 OUI)
rsp.string("");                       // Website(空)
return {Status::OK, 1};
```

**GetProduct(22):** OcaProduct = 6 个 OcaString(Name/ModelID/RevisionLevel/BrandName/UUID/Description)。
```cpp
rsp.string(identity_.model_name);     // Name
rsp.string(identity_.model_name);     // ModelID(无独立值,用 model_name)
rsp.string(identity_.model_version);  // RevisionLevel
rsp.string(identity_.manufacturer);   // BrandName
rsp.string("");                        // UUID(OcaString,空;未生成)
rsp.string("");                        // Description
return {Status::OK, 1};
```
注:OcaUUID = OcaString(RFC 4122),空串合规(未分配)。字段值取自现有 `OcaDeviceIdentity`(阶段一已有 manufacturer/model_name/model_version)。

- [x] **Step 3: device_manager.hpp 加 2 声明**

- [x] **Step 4: oca_test 加用例**

- GetManufacturer(mi=21):断言 OK + 1 参 + 解析 Name/OrgID(3B)/Website。
- GetProduct(mi=22):断言 OK + 1 参 + 解析 6 字符串。

- [x] **Step 5: 构建测试**
```bash
make oca-test && ./tests/oca-test -p
```

- [x] **Step 6: commit** `feat(oca): G3/G4 DeviceManager 实现 GetProduct(3.22)/GetManufacturer(3.21)`

---

## Task 6: oca-probe 更新 + 阶段二验证

**Files:**
- Modify: `daemon/oca/tools/oca_probe.cpp`(新增探测)

**Interfaces:**
- Produces: oca-probe 覆盖全部新方法;阶段二验证(oca-test + oca-probe e2e + MITM + 真实控制器)。
- Consumes: Task 1-5 结果。

- [x] **Step 1: oca-probe 加探测**

新增探测项(对运行中的 daemon):
- EV1 AddSubscription(mi=1)订阅 OperationalState 事件,断言响应 0 参(无 subID)。
- Lock(3)/Unlock(4)。
- GetModelGUID(2)/GetEnabled(11)/SetEnabled(12)/GetDeviceRevisionID(20)。
- GetStreamNetworks(2)/GetControlNetworks(3)/GetMediaTransportNetworks(4)。
- GetProduct(22)/GetManufacturer(21)。

- [x] **Step 2: oca-test 全绿**
```bash
make oca-test oca-probe && ./tests/oca-test -p
```
全绿(阶段一 21 + 阶段二新增)。

- [x] **Step 3: oca-probe 端到端**

起 daemon(`./oca-dev.sh run -i lo`,http=8080/oca=65037),跑 `./oca-probe 127.0.0.1 65037`,确认全部探测通过(含 EV1 订阅 0 参响应)。

- [x] **Step 4: MITM 抓包确认字节**

起 MITM(`daemon/oca/tools/oca-mitm.py`)+ avahi-publish-service。oca-probe 经 MITM 连 daemon,抓 EV1 AddSubscription/Lock/GetModelGUID/GetProduct 响应字节,确认:
- EV1 AddSubscription 响应 nrParameters=0(无 subID)。
- GetModelGUID 响应 8 字节。
- GetManufacturer/GetProduct 字段顺序正确。
- 新方法 status=0(非 8)。

- [x] **Step 5: 真实控制器重跑(用户执行)**

用户在 Win 跑 Aes70CompliancyTestTool,MITM 抓包。预期:**OCC Object Compliancy(test 5)从 Failed 转 Passed**(强制方法 result≠8 + EV1 订阅 OK + 事件订阅 OK)-> **4/5**。Minimum object compliancy(test 4)仍 Failed(缺 CM3 对象,Option A 接受)。记录通过项数与剩余失败根因。

- [x] **Step 6: 记录验证结果到设计文档 + 记忆**

更新 `aes70-oca-spec2-design.md` §C5(阶段二验证结果表);更新记忆 `aes70-oca-spec1.md`。

- [x] **Step 7: commit** `test(oca): Spec2 阶段二验证 - OCC Object Compliancy 转过(4/5)`

---

## 阶段二完成标志

- EV1(mi=1/2/5/6)+ EV2 PropertyChange2(mi=10/11)订阅方法实现,oca-test 全绿。
- OcaRoot Lock/Unlock、DeviceManager(GetModelGUID/GetEnabled/SetEnabled/GetDeviceRevisionID/GetProduct/GetManufacturer)、NetworkManager(GetStreamNetworks/GetControlNetworks/GetMediaTransportNetworks)实现。
- 真实控制器 OCC Object Compliancy(test 5)通过 -> 4/5。
- 剩余 1 项失败(Minimum object compliancy,缺 CM3 对象)经 MITM 坐实为 Option A 接受范围。
- 达 AES70-2023 Annex B 订阅 + 强制方法最低合规(除 CM3 对象 + Worker defLevel 2 顺延项)。

## 阶段三预告(另起计划)

- CM3 网络对象(若需 5/5):OcaNetwork/OcaControlNetwork stub(2023 弃用,权衡 2018 兼容)。
- OcaWorker defLevel 2:GetEnabled/SetEnabled/GetPorts(Block 作为 Worker 时)。
- event PropertyChanged 实际投递验证(Notification2 到 EV1 订阅者)。
- 媒体桥接(MediaClock/StreamConnector/SessionManager)-> Spec3 主线。

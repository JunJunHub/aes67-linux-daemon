# AES70/OCA Spec2 设计文档 - 合规闭环

> 版本: 2026-07-10
> 范围: Spec2 = AES70-2023 Annex B 最低合规闭环(补齐强制对象/方法/事件 + ONo↔ClassID 纠配)
> 产出: 官方合规工具对象树/身份/订阅类测试通过;控制面对 spec 控制器完全可用
> 基于: `docs/AES70-2023/` 官方 PDF 原文(§引用见下);Spec1 已交付实现
> 前置: Spec1(协议栈基础,4 commit 入库,oca-test 22/22 绿,真实控制器 2/5 通过)
> 关联: 本文档取代 Spec1 设计文档 §下一步 中"Spec2 = 媒体桥接"的原口径——经真实控制器验证,合规缺口先于媒体桥接,故 Spec2 改为合规闭环,媒体桥接顺延为 Spec3

---

## 决策记录

| # | 决策 | 选定 | 理由 |
|---|------|------|------|
| 1 | Spec2 口径 | 合规闭环优先(原媒体桥接顺延 Spec3) | 控制器连对象树都探测不全,媒体桥接无意义;合规缺口有界可闭环 |
| 2 | 权威来源 | OCAMicro 编译期常量 > AES70 PDF 原文 > 记忆 > 转换 md | Spec1 多处判断被推翻;Phase 0 进一步证明:PDF 正文 ClassID 在 UML 图中提取不到,据 PDF 推断会出错,OCAMicro `#define` 常量才是最硬证据 |
| 3 | 验证先行 | Phase 0 用 MITM+OCAMicro 源码坐实每条缺口 | Spec1 "GetModelDescription 满足 GetProduct" 即未坐实之误,前车之鉴 |
| 4 | 非强制方法语义 | 存在但未实现的方法返回 NotImplemented,非法索引返回 BadMethod | AES70-2 Annex B.3.0 明文要求 |
| 5 | 合规基准 | 以 AES70-2023 Annex B 为权威 | GetProduct 等是 2023 新增,实现不损 2018 兼容;ONo 修复对两者都有益;2023 是 2018 超集,既向前看又最大化当前 2018 工具通过率;2018 工具作可用验证手段 + MITM 消歧 |
| 6 | 范围边界 | 分两阶段:阶段一最小过测试,阶段二补事件达完整 | 先 G1+G3+G4+G5+G9 验证 ONo 修复是否真让对象合规测试过(确认根因假设),再投 G6/G7/G8 事件机制;降低"建了事件机制却发现根因在别处"风险 |
| 7 | 阶段一范围(Phase 0 后修订) | 只做有硬证据项 G0/G1/G2/G9/G10 | Phase 0 证明 G0(ClassID)才是首要根因;G3-G8 为 2023 新增方法,OCAMicro(2018)无文本源,等阶段二 |
| 8 | 2023 方法文本源 | Claude 联网查 OCAAlliance 官方资源 | G3-G7 方法索引只在 AES70-2A/XMI,本地无;联网获取为阶段二铺路 |
| 9 | 类版本号取值 | 暂用 OCAMicro 2018 值(四类全 2) | 有源码证据,与 2018 工具预期吻合;2023 值待 2A 确认 |

---

## §A PDF 核对结论(Phase 0 事实依据)

以下结论均来自 `docs/AES70-2023/*.pdf` 原文(pdftotext 提取核对),每条标注出处。

### A.1 跨版本兼容性:ClassID / 方法索引 / 类版本

**结论:设计上跨 2018↔2023 通用兼容,这正是标准目标。**

- AES70-1-2023 扉页明示为 "Revision of AES70-1-2018"(修订版,非重写)。
- **ClassID 跨版本稳定**:AES70-1 §6.2.2.5.3 Rule 8——"标准类的 Class Index 由 `[AES70-2A]` 分配"。2A 是独立数据类型注册表,标准类 ClassID(如 DeviceManager={1,2,1})在版本间不变;变了所有现存控制器都得崩,标准不会这么做。
- **类版本号机制专为兼容而设**:AES70-1 §6.2.2.7 "Inheritance and updating rules"——更新已有类时(1)版本号递增,(2)必须实现上一版**全部**方法/属性/事件,(4)**element ID 保持不变**。即 2023 给某类加新方法 → 版本号+1,但老方法索引/签名原样保留,2018 控制器照样调老方法。
- **方法索引编码 LLtNN 跨版本一致**:AES70-1 §6.2.2.6.1——LL=定义层级,t=类型(m/p/e),NN=序号。同一方法索引跨版本通用。
- **推论(初稿,⚠️ 已被 A.1.1 推翻)**:初稿称 Spec1 ClassID 方向正确--错误,见 A.1.1。

### A.1.1 ★★ ClassID 类树结构(OCAMicro 编译期常量坐实,Phase 0)

**⚠️ 本节推翻初稿 §A.1 末尾"Spec1 ClassID 方向正确"的错误判断。** 初稿据 PDF 推断"{1,2,x}=Manager 子树",但 PDF 正文 ClassID 在 UML 图中、pdftotext 提取不到,该推断无原文支撑。Phase 0 用 OCAMicro 源码 `#define` 编译期常量(最硬证据)坐实真实类树:

```
Root      = {1}
 ├─ Worker  = {1,1}          (OCA_WORKER_CLASSID  = ROOT,1)
 │   └─ Block = {1,1,3}      (OCA_BLOCK_CLASSID   = WORKER,3)
 ├─ Agent   = {1,2}          (OCA_AGENT_CLASSID   = ROOT,2)   ← 1.2 是 Agent,不是 Manager!
 │   ├─ Network    = {1,2,1}
 │   └─ MediaClock = {1,2,6}
 └─ Manager = {1,3}          (OCA_MANAGER_CLASSID = ROOT,3)   ← 1.3 才是 Manager
     ├─ DeviceManager       = {1,3,1}   (DEVICEMANAGER_CLASSID = MANAGER,1)
     ├─ FirmwareManager     = {1,3,3}   (FIRMWAREMANAGER_CLASSID = MANAGER,3)
     ├─ SubscriptionManager = {1,3,4}   (SUBSCRIPTION_MANAGER_CLASSID = MANAGER,4)
     └─ NetworkManager      = {1,3,6}   (NETWORKMANAGER_CLASSID = MANAGER,6)
```

证据:`OCAMicro/.../OcaLiteRoot.h:33`(`OCA_ROOT_CLASSID=1`)、`OcaLiteAgent.h:25`(`OCA_AGENT_CLASSID=ROOT,2`)、`OcaLiteManager.h:24`(`OCA_MANAGER_CLASSID=ROOT,3`)、`OcaLiteWorker.h:26`(`OCA_WORKER_CLASSID=ROOT,1`),及四目标类各自 .h 的 `*_CLASSID` 宏。

**Spec1 现状对比(代码实读 `daemon/oca/classes/*.cpp`):**

| 类 | Spec1 代码(错) | OCAMicro 权威(对) | 后果 |
|---|---|---|---|
| DeviceManager | {1,2,1} | **{1,3,1}** | 自报为 OcaNetwork(恰为 {1,2,1}) |
| NetworkManager | {1,2,3} | **{1,3,6}** | 自报为某 Agent |
| SubscriptionManager | {1,2,4} | **{1,3,4}** | 自报为某 Agent |
| Block | {1,1,3} | {1,1,3} | ✓ 正确 |

**这是 Spec1 对象合规测试失败的首要根因**:三个 Manager 被标到 Agent 子树,控制器按 ClassID 把 DeviceManager 当 OcaNetwork,要求其 GetIDAdvertised/GetMediaProtocol 等网络方法 -> 报错。比初稿判定的"ONo 2 错配"更根本。修正 ClassID 是 Spec2 阶段一最高优先级(新 G0,见 §B)。

**类版本号(OCAMicro 2018 值):** OCAMicro 中所有类 `CLASS_VERSION_INCREMENT=0`,均继承 Root 版本 2 -> 四类版本全为 2。Spec1 代码 DeviceManager=4、NetworkManager=3、SubscriptionManager=2、Block=2。SubscriptionManager/Block 与 2018 一致;DeviceManager/NetworkManager 偏高(可能反映更晚 spec 版本,待 §C-4 校验)。注意 OCAMicro 是 2018 版,2023 类版本可能更高,需 AES70-2A 确认。

### A.2 ONo 分配权威(纠正 Spec1 记忆/文档两处错误)

出处:AES70-1 §8.2 + Table 3 + AES70-2 Annex B Table B.1。

- **ONo 1–4095 预留给预定义对象**(AES70-1 §8.2 Rule 4)。Spec1 文档/记忆写的"1–99"是错的,实际整段 1–4095。
- **ONo 100 = Root Block 是标准明文规定**(Table 3 + Table B.1),**完全合规**。Spec1 记忆"RootBlock=100 违规"是错的,无需改。
- **Table 3 标准 Manager ONo 分配**(纠正 Spec1 对象树):

| ONo | 标准类 | 强制性 | Spec1 现状 | 问题 |
|-----|--------|--------|-----------|------|
| 1 | OcaDeviceManager | **M** | ✓ 实例化 | 无 |
| 2 | OcaSecurityManager | O | ✗ 错放 NetworkManager | **ONo↔ClassID 错配** |
| 3 | OcaFirmwareManager | O | —(空缺) | optional,可不实例化 |
| 4 | OcaSubscriptionManager | **M** | ✓ 实例化 | 无 |
| 6 | OcaNetworkManager | O | ✗ 占了 ONo 2 | 应迁到 ONo 6(若保留) |
| 100 | OcaBlock(Root Block) | **M** | ✓ 实例化 | 无 |

- **强制对象只有 3 个**(Table B.1):ONo 1 DeviceManager、ONo 4 SubscriptionManager、ONo 100 Root Block。其余 Manager 全 optional。
- **关键错配**:Spec1 把 NetworkManager 放在 ONo 2,但标准 ONo 2 = SecurityManager。控制器按 ONo 查到 ClassID={1,2,3}(NetworkManager)与预期 SecurityManager 不符 → 对象合规判异常。**这是 Spec1 对象合规测试失败的首要嫌疑**。

### A.3 Annex B 最低合规强制方法/事件(2023)

出处:AES70-2 Annex B.3.1 / B.3.2 / B.3.3。

**B.3.1 基集(所有对象,OcaRoot 继承):**

| 方法/事件 | Spec1 现状 |
|-----------|-----------|
| GetLockable | ✓(返回 0=不可锁) |
| SetLockNoReadWrite(Lock) | 缺(仅 lockable 对象需要,我们不可锁 → 可不实现) |
| SetLockNoWrite | 缺(同上) |
| Unlock | 缺(同上) |
| **event PropertyChanged** | **✗ 缺** |

**B.3.2 DeviceManager 强制方法:**

| 方法 | Spec1 现状 |
|------|-----------|
| GetOcaVersion | ✓ |
| GetSerialNumber | ✓ |
| GetDeviceName | ✓ |
| GetManagers | ✓ |
| GetOperationalState(我们 GetState,索引 13) | ✓ |
| **GetProduct** | **✗ 缺**(Spec1 用 GetModelDescription 顶替,见 A.5) |
| **GetManufacturer** | **✗ 缺**(同上) |

**B.3.3 SubscriptionManager + Root Block 强制方法:**

| 对象 | 强制方法 | Spec1 现状 |
|------|---------|-----------|
| SubscriptionManager | AddSubscription2 | ✓ |
| SubscriptionManager | RemoveSubscription2 | ✓ |
| SubscriptionManager | **AddPropertyChangeSubscription2** | **✗ 缺** |
| SubscriptionManager | **RemovePropertyChangeSubscription2** | **✗ 缺** |
| Root Block | **GetControlObjects** | **✗ 缺**(Spec1 只有 GetMembers,见 A.6) |

### A.4 2023 弃用项(纠正 Spec1 规划)

出处:AES70-1 Annex G/H。

- **Annex H.4:CM3 网络类整体弃用**,`OcaControlNetwork`、`OcaApplicationNetwork`、`OcaMediaTransportNetwork`、`OcaCodingManager` 均弃用,被 CM4 取代。
  - **影响**:Spec1 设计文档/记忆提的"补 OcaNetwork/ControlNetwork"是**错的**——2023 里 ControlNetwork 已弃用。Spec2 不应补这些类。
- **Annex H.3:EV1 事件弃用,被 EV2 取代**。Spec1 已用 EV2 ✓,方向对。
- **Annex G:GetProduct/GetManufacturer 是 2023 新增**(改进 OcaDeviceManager 产品/厂商信息)。2018 用 GetModelDescription(打包返回)。→ 见 A.5。

### A.5 GetModelDescription vs GetProduct/GetManufacturer(存疑,待 Phase 0)

- AES70-2 Annex B.3.2(2023)把 `GetProduct` 与 `GetManufacturer` **分别**列为强制。
- AES70-1 Annex G 说 2023 "Improved product and manufacturer information from OcaDeviceManager"——强烈暗示 GetProduct/GetManufacturer 是 2023 新增,改进旧的 GetModelDescription。
- Spec1 plan 勘误 #3 判断"GetModelDescription(索引 6)满足 GetProduct/GetManufacturer"——**此结论与 Annex B 冲突,存疑**。
- **待 Phase 0 坐实**:用 OCAMicro 源码确认 GetProduct/GetManufacturer 的方法索引、签名、返回结构,以及 GetModelDescription 在 2023 是否仍存在。在坐实前不采信勘误 #3。

### A.6 GetMembers vs GetControlObjects(已坐实缺口)

- AES70-2 Annex B.3.3 强制 Root Block 实现 **GetControlObjects**。
- Spec1 Root Block 只实现 GetMembers(索引 5)。GetControlObjects 是不同方法(返回控制对象子集,方法索引待 Phase 0 查 OCAMicro/XMI)。
- **结论**:Root Block 缺强制方法 GetControlObjects,是真实合规缺口。GetMembers 可保留(对象树发现仍需要),但 GetControlObjects 必须补。

---

## §B 合规缺口清单(Spec1 现状 vs Annex B)

汇总 §A,Spec2 需闭环的缺口:

| # | 缺口 | 类别 | 依据 | 风险 |
|---|------|------|------|------|
| **G0** | **三个 Manager ClassID 标到 Agent 子树**({1,2,x}->应为 {1,3,x}) | ClassID 错误 | A.1.1 | **极高:对象合规测试首要根因** |
| **G11** | **EV2 订阅方法索引全错**({1,2,3,4}->应为 {8,9,10,11}) | 方法索引错误 | C.1/§C | **高:Spec1 第三个被自测掩盖的协议 bug** |
| G1 | ONo 2 错放 NetworkManager(应为 SecurityManager) | ONo↔ClassID 错配 | A.2 Table 3 | 高(次于 G0) |
| G2 | NetworkManager 应迁 ONo 6(若保留)或删除 | ONo 分配 | A.2 | 中 |
| G3 | DeviceManager 缺 GetProduct(3.22) | 强制方法(2023) | C.1 | 高,sphinx 2024 已坐实索引/结构 |
| G4 | DeviceManager 缺 GetManufacturer(3.21) | 强制方法(2023) | C.1 | 高,已坐实 |
| G5 | Root Block 缺 GetControlObjects | ~~强制方法~~ | C.1 | **降级:已实现(3.5,GetMembers 改名链),非缺口** |
| G6 | SubscriptionManager 缺 AddPropertyChangeSubscription2(3.10) | 强制方法(EV2) | C.1 | 中,已坐实 |
| G7 | SubscriptionManager 缺 RemovePropertyChangeSubscription2(3.11) | 强制方法(EV2) | C.1 | 中,已坐实 |
| G8 | 所有对象缺 event PropertyChanged | 强制事件 | A.3 | 中:基集要求 |
| G9 | 非强制方法返回 BadMethod 而非 NotImplemented | 方法语义 | B.3.0/决策4 | 低:细节合规 |
| G10 | 类版本号(DeviceManager=4/NetworkManager=3)-> 暂用 2018 值 2 | 类版本 | A.1.1/决策9 | 低:用户已定暂用 2 |
| G12 | GetState(3.13,deprecated)应迁移到 GetOperationalState(3.23) | deprecated 方法 | C.1 | 中:2018 工具可能探测 |

**不在 Spec2 范围**(确认弃用或属 Spec3):
- OcaControlNetwork/ApplicationNetwork/MediaTransportNetwork(CM3 弃用,A.4)
- 媒体桥接(MediaClock/StreamConnector/SessionManager 双向桥接)→ Spec3
- TLS/UDP/WebSocket 传输 → 后续
- Dataset/Patch → 后续

---

## §C Phase 0 坐实状态(2026-07-10,sphinx 2024 考证后定稿)

Phase 0 分两步:(1) OCAMicro 源码(本地,2018 版)坐实 ClassID 类树与 2018 方法;(2) 联网克隆 OCAAlliance/AES70-OCC-sphinx(2024 版,RST 含明确方法 id ``LL.N``),坐实 2023/2024 新增方法索引。**sphinx 2024 是目前最权威的方法索引文本源**(RST 明文标注 method id,优于 PDF 正文)。

### C.1 OCAMicro(2018)+ sphinx(2024)坐实结果

**类树 ClassID(OCAMicro 编译期常量,见 §A.1.1):** 1.2=Agent、1.3=Manager、1.1=Worker。三 Manager 须 {1,2,x}->{1,3,x}。

**OcaRoot 方法索引(sphinx 2024,与我们 methods.hpp 一致 ✓):** GetClassIdentification=1.1、GetLockable=1.2、SetLockNoReadWrite=1.3、Unlock=1.4、GetRole=1.5、SetLockNoWrite=1.6、GetLockState=1.7。event PropertyChanged 存在(1.e)。

**OcaDeviceManager 方法索引(sphinx 2024):**

| 方法 | 索引 | 返回 | Spec1 现状 |
|------|------|------|-----------|
| GetModelDescription | 3.6 | OcaModelDescription{mfr,name,ver} | ✓ 但 v3 **deprecated** |
| GetState | 3.13 | OcaDeviceState | ✓ 但 v3 **deprecated** |
| GetManagers | 3.19 | List<ManagerDescriptor> | ✓ |
| **GetManufacturer** | **3.21** | OcaManufacturer{Name,OrganizationID} | ✗ 缺(G3) |
| **GetProduct** | **3.22** | OcaProduct{Name,ModelID,RevisionLevel,...} | ✗ 缺(G4) |
| **GetOperationalState** | **3.23** | OcaDeviceOperationalState | ✗ 我们用 deprecated 的 GetState(3.13)顶替 |
| SetResetKey | 3.14 | - | ✗ 缺(device reset,2018 工具测过) |

**OcaBlock 方法索引(sphinx 2024):** 关键发现--`GetMembers`(2018,OCAMicro,3.5)-> `GetControlObjects`(2023,PDF)-> `GetActionObjects`(2024,sphinx,3.5)**是同一索引 3.5 的三次改名**,返回均为 `List<OcaObjectIdentification>`。sphinx 原文:"GetActionObjects... Previously named GetMembers."。

| 方法 | 索引 | Spec1 现状 |
|------|------|-----------|
| GetMembers/GetControlObjects/GetActionObjects | 3.5 | ✓ **已实现**(Spec1 db371a2,索引+返回结构均对) |
| GetMembersRecursive/GetActionObjectsRecursive | 3.6 | 返回 BadMethod(spec-allowed) |

**★ G5 降级:Root Block 并不缺方法**--我们已实现索引 3.5(名 GetMembers),对 2018(GetMembers)/2023(GetControlObjects)/2024(GetActionObjects)工具均应识别(索引同)。G5 从"高优先缺口"降为"已实现"。

**OcaSubscriptionManager EV2 方法索引(sphinx 2024):**

| 方法 | sphinx 2024 索引 | Spec1 methods.hpp 候选值 | 判定 |
|------|-----------------|------------------------|------|
| AddSubscription2 | **3.8** | kSubAddSubscription2=1 | **❌ 错(应 8)** |
| RemoveSubscription2 | **3.9** | kSubRemoveSubscription2=2 | **❌ 错(应 9)** |
| AddPropertyChangeSubscription2 | **3.10** | kSubAddPropertyChangeSubscription2=3 | **❌ 错(应 10)** |
| RemovePropertyChangeSubscription2 | **3.11** | kSubRemovePropertyChangeSubscription2=4 | **❌ 错(应 11)** |

**★ 新增缺口 G11:Spec1 EV2 订阅方法索引全错。** 我们用 {1,2,3,4}(疑似误抄 EV1 的 3.1-3.4 或自造),正确为 {8,9,10,11}。AddSubscription2 入参(sphinx 2024):`OcaEvent{EmitterONo,EventID}, NotificationDeliveryMode, NetworkAddress`(比 EV1 少了 Subscriber/Context)。**Spec1 验证"AddSubscription2 通过真实控制器"的结论存疑**--oca-probe 与 daemon 共用错误常量自洽通过(自测盲点),真实控制器(2018)若调 EV2 AddSubscription2 用 methodIndex=8 会落入 default->BadMethod;真实控制器 5 项测试 3 项失败,订阅很可能是其中之一。这是 Spec1 第三个被自测掩盖的协议 bug(继 NrParameters、GetMembers 之后),归入 Spec2 阶段一(G11)。

### C.2 开放问题状态

| # | 问题 | 状态 | 结论 |
|---|------|------|------|
| C1 | GetProduct/GetManufacturer 索引+结构 | ✅ 坐实 | GetManufacturer=3.21{Name,OrgID}、GetProduct=3.22{Name,ModelID,RevisionLevel,...}(sphinx 2024) |
| C2 | GetControlObjects 索引+结构 | ✅ 坐实 | =GetMembers=3.5,已实现。G5 降级 |
| C3 | EV2 四方法索引 | ✅ 坐实 | AddSub2=3.8、RemoveSub2=3.9、AddPropChgSub2=3.10、RemovePropChgSub2=3.11。Spec1 全错,新增 G11 |
| C4 | 类版本号 | ⚠ 部分 | OCAMicro(2018)四类均 2;用户定暂用 2018 值(决策 #9) |
| C5 | 2018 工具实际拒了什么 | ⏳ 待重跑 | 首要嫌疑仍是 G0(ClassID);G11(EV2 索引)是新增嫌疑 |
| C6 | PropertyChanged 事件投递 | ⏳ 待设计 | EV2 帧格式已确认;PropertyChange 订阅=3.10,投递路径阶段二设计 |

**Phase 0 已坐实的硬结论(可直接用于实施):**
- ✅ ClassID 类树(A.1.1):1.2=Agent、1.3=Manager。三 Manager {1,2,x}->{1,3,x}。
- ✅ OcaRoot 方法索引与我们一致(1.1-1.7)。
- ✅ DeviceManager:GetManufacturer=3.21、GetProduct=3.22、GetOperationalState=3.23(GetState=3.13 deprecated,应迁移)。
- ✅ Block:GetMembers/GetControlObjects/GetActionObjects=3.5,已实现,G5 降级。
- ✅ EV2 订阅:3.8/3.9/3.10/3.11。Spec1 用 1/2/3/4 全错 -> G11。
- ✅ EV2 Notification2 帧格式(AES70-3 §9.4.4),Spec1 已实现。

**sphinx 2024 仓库:** 已克隆至 `/tmp/AES70-OCC-sphinx`(OCAAlliance/AES70-OCC-sphinx,最新提交 "Update OCC to 2024")。关键 RST:`source/Control Classes/{OcaDeviceManager,OcaBlock,OcaSubscriptionManager,OcaRoot}.rst`。**考证能力边界已突破**--2023/2024 方法索引均有文本源,阶段二不再阻塞。

---

## §D Spec2 目标口径(定稿,2026-07-10 修订)

> **修订背景:** Phase 0 揭示两件事,需对原定稿(决策 #5/#6)做调整:(1) Spec1 失败首要根因是 **G0 ClassID 错误**(原误判为 G1 ONo);(2) 本地 OCAMicro 是 2018 版,2023 新增方法(G3-G7)无文本源坐实,与"2023 Annex B 完整合规"目标存在客观障碍。下列 §D 反映修订,口径调整点待用户确认(见末尾"待用户确认")。

### 合规基准:以 AES70-2023 Annex B 为权威(sphinx 2024 作方法索引文本源)

- 长期目标对齐 AES70-2023 Annex B;sphinx 2024 RST(明文 method id)为方法索引权威文本源,Phase 0 已坐实全部所需索引。
- 2018 官方工具作为可用验证手段;G0+G11 修正后预期对象合规测试大幅改善。

### 范围边界:分两阶段(sphinx 2024 坐实后修订)

**阶段一(修 Spec1 协议 bug + 有硬证据的项,先过对象合规测试):**

| 缺口 | 改动 | 证据 | 验证 |
|------|------|------|------|
| **G0** | **三 Manager ClassID {1,2,x}->{1,3,x}**(DeviceManager->{1,3,1}、SubscriptionManager->{1,3,4}、NetworkManager->{1,3,6});methods.hpp DefLevel 注释更正 | A.1.1 OCAMicro 编译期常量 | oca-test + MITM + 2018 工具 |
| **G11** | **EV2 订阅方法索引 {1,2,3,4}->{8,9,10,11}**(methods.hpp kSub* 常量);AddSubscription2 入参对齐 sphinx 2024(OcaEvent+DeliveryMode+NetworkAddress,去 Subscriber/Context) | C.1 sphinx 2024 | oca-test + oca-probe + 2018 工具 |
| G1 | ONo 2 错配修正:NetworkManager 迁出 ONo 2(迁 ONo 6 或删除) | A.2 Table 3 | MITM + 2018 工具 |
| G2 | 与 G1 一并处理 | A.2 | 同上 |
| G9 | 非强制方法返回 NotImplemented(而非 BadMethod) | B.3.0 | oca-test 单测 |
| G10 | 类版本号:DeviceManager/NetworkManager 降到 2(暂用 OCAMicro 2018 值) | A.1.1/决策9 | oca-test |
| G12 | GetState(3.13 deprecated)-> GetOperationalState(3.23) | C.1 | oca-test |

阶段一完成标志:G0/G11/G1/G2/G9/G10/G12 闭环,官方工具对象合规测试通过(或经 MITM 坐实剩余失败为 2023 方法缺失 G3/G4/G6/G7/G8)。

### 阶段一实现与验证结果(2026-07-10,SDD 完成 T1-T8)

7 项缺口全部闭环,提交 `2411497..e087df9`(8 commit,分支 `feature/aes70-oca`):

| Task | 缺口 | 实现 | 验证 |
|------|------|------|------|
| T1 | G0 | 三 Manager ClassID -> {1,3,x}(DeviceManager {1,3,1}、SubscriptionManager {1,3,4}、NetworkManager {1,3,6}),版本暂留(T4 改) | oca-test + MITM |
| T2 | G11 | EV2 索引 {1,2,3,4}->{8,9,10,11};AddSubscription2/RemoveSubscription2 入参对齐 sphinx 2024(OcaEvent+DeliveryMode(u8,Normal=1)+NetworkAddress(blob),去 ctx);RemoveSub2 改按 event 移除全部+幂等 | oca-test + oca-probe + MITM |
| T3 | G1/G2 | NetworkManager ONo 2->6(ONo 2=SecurityManager 空置);GetMembers/GetManagers 列表 [1,2,4]->[1,4,6] | oca-test + MITM |
| T4 | G10 | DeviceManager/NetworkManager class_version 4/3->2(与 OCAMicro 2018 一致) | oca-test + MITM |
| T5 | G12 | 新增 GetOperationalState(3.23)返回 OcaDeviceOperationalState{u8 Generic=NormalOperation(0)+空 Details blob};GetState(3.13) deprecated 保留 | oca-test |
| T6 | G9 | 各类 default 分支(defLevel 匹配但方法未实现)BadMethod->NotImplemented;defLevel 不匹配仍 BadMethod(Annex B.3.0) | oca-test |
| T7 | — | oca-probe 用正确 EV2 索引/入参;新增"旧索引 1 返回非 OK"断言,消除自测盲点 | oca-probe e2e + MITM |

**验证(Step1-3 全 PASS):**
- oca-test 21/21 绿(22 宏/21 运行,1 个 Avahi 条件用例 OFF 时不跑,预存在)。
- oca-probe e2e 经 MITM(65038->65037)全部探测通过(exit 0):发现/身份/GetManagers/GetMembers/GetClassIdentification/AddSubscription2/旧索引断言。
- MITM 字节级裁决(权威):GetManagers 与 GetMembers 均返回 `ONo=1{1,3,1}v2 / ONo=4{1,3,4}v2 / ONo=6{1,3,6}v2`;GetClassIdentification(target=1)返回 `{1,3,1} v2`;AddSubscription2 命令 `target=4 dl=3 mi=8 nr=3`->OK 返回 subscriptionID;旧索引 `target=4 mi=1`->**NotImplemented**(自测盲点消除)。
- 附带修复 `e087df9`:oca-probe GetMembers 解析对齐 List<OcaObjectIdentification>(db371a2 改返回结构后 oca-probe 解析陈旧,T8 暴露)。

**真实控制器验证结果(2026-07-10 Step4,Aes70CompliancyTestTool v2.0.1 AES70-2018,Win 172.16.1.211,MITM 抓包 3 连接 236 PDU):3/5 通过(Spec1 2/5,进步 1 项)。**

| 测试 | Spec1 | Spec2 | 关键 |
|------|-------|-------|------|
| OCA Service Discovery | ❌ | ✅ | GetMembers+GetManagers status 0(db371a2+ef19171) |
| OCP.1 device reset | ❌ SetResetKey result 11 | ✅ | **G9 直接生效**:NotImplemented->工具"skip with success" |
| OCP.1 KeepAlive | ✅ | ✅ | 3000ms 在区间内 |

**字节级裁决(工具流量,全对齐):** GetClassIdentification ONo=1{1,3,1}v2 / ONo=4{1,3,4}v2 / ONo=6{1,3,6}v2(G0/G3/G10 PASS);GetMembers(100,mi=5) OK 3 members;GetState(mi=13)/GetManagers(mi=19) OK;未实现方法(mi=11/12/14/20)返回 NotImplemented 非 BadMethod(G9 PASS);旧 EV2 索引 mi=1 -> NotImpl(G11 自测盲点修复)。

**剩余 2 项失败根因(MITM 坐实,均为阶段二范围,非阶段一遗漏):**
1. **强制方法未实现**:DeviceManager GetModelGUID(2)/GetEnabled(11)/SetEnabled(12)/GetDeviceRevisionID(20);OcaRoot Lock(3)/Unlock(4);NetworkManager GetStreamNetworks(2)/GetControlNetworks(3)/GetMediaTransportNetworks(4)。均正确返回 NotImplemented(2018 工具对 mandatory 方法要求实现,非 skip)。
2. **★★ 事件 PropertyChanged 订阅失败(新发现,原阶段二 G6/G7 计划未覆盖)**:MITM 显示 2018 工具用 **EV1 AddSubscription(mi=1,5 参数:emitter+EventID+subscriberONo+ctx+subctx)** 订阅 PropertyChanged,**非 EV2**(mi=8/10)。daemon 只实现 EV2(mi=8/9),EV1 订阅方法(mi=1/2)未实现->NotImpl。**阶段二须实现 EV1 AddSubscription/RemoveSubscription(mi=1/2)**,而非仅 EV2 PropertyChange 变体。
3. Missing mandatory object OcaBlock/OcaNetwork/OcaControlNetwork(724-726):2018 工具"完整对象模型"要求;Block 实际在 ONo=100 已存在,工具可能按 ONo>=4096 遍历(2018 约定);CM3 网络类 2023 弃用,需权衡 2018 兼容。

**阶段一目标达成**:G0/G1/G2/G9/G10/G11/G12 全部字节级验证生效,Service Discovery(核心)稳定通过。MITM 日志 /tmp/spec2-realcap-mitm.log,测试日志 /tmp/oca_test_log.txt(均 /tmp,重启失效,结论已固化本文)。

**阶段二(补 2023 强制方法/事件,索引已坐实可立即实施):**

| 缺口 | 改动 | 索引/结构(已坐实) |
|------|------|--------------------|
| G3 | DeviceManager 补 GetProduct | 3.22,返回 OcaProduct{Name,ModelID,RevisionLevel,...} |
| G4 | DeviceManager 补 GetManufacturer | 3.21,返回 OcaManufacturer{Name,OrganizationID} |
| G6 | SubscriptionManager 补 AddPropertyChangeSubscription2 | 3.10 |
| G7 | SubscriptionManager 补 RemovePropertyChangeSubscription2 | 3.11 |
| G8 | 所有对象支持 event PropertyChanged | 订阅=3.10,Notification2 投递(Spec1 帧格式已实现) |

阶段二完成标志:达 AES70-2023 Annex B 完整最低合规。G5 已确认非缺口(3.5 已实现)。

### 已定稿(2026-07-10 用户确认)

1. **阶段一范围**:只做有硬证据项 G0/G1/G2/G9/G10,先把对象合规测试跑通;G3-G8(2023 新增方法)等文本源到位再做(决策 #7)。
2. **2023 方法文本源途径**:由 Claude 联网查 OCAAlliance 官方资源(AES70-2A 注册表/Annex A XMI 在线版)获取 GetProduct/GetManufacturer/GetControlObjects/EV2 方法索引(决策 #8)。
3. **类版本号取值**:G10 暂用 OCAMicro 2018 值(四类全 2),与 2018 工具预期吻合;2023 值待 AES70-2A 确认后再调(决策 #9)。

### 不在 Spec2 范围

- OcaControlNetwork/ApplicationNetwork/MediaTransportNetwork(CM3 弃用,A.4)
- 媒体桥接(MediaClock/StreamConnector/SessionManager 双向桥接)-> Spec3
- TLS/UDP/WebSocket 传输 -> 后续
- Dataset/Patch -> 后续

### 下一步

1. **用户确认上述口径调整**(阶段一先行 + 文本源途径 + 类版本取值)。
2. 据确认结果拆分 `docs/superpowers/plans/aes70-oca-spec2-plan.md`:阶段一(有证据项)立即可实施,阶段二标注前置条件。
3. 阶段一实施 -> oca-test + MITM + 2018 工具验证 -> 阶段二待文本源。

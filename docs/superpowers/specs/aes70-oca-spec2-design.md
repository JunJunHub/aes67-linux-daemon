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
| G1 | ONo 2 错放 NetworkManager(应为 SecurityManager) | ONo↔ClassID 错配 | A.2 Table 3 | 高(次于 G0) |
| G2 | NetworkManager 应迁 ONo 6(若保留)或删除 | ONo 分配 | A.2 | 中 |
| G3 | DeviceManager 缺 GetProduct | 强制方法(2023) | A.3/A.5 | 高,但 OCAMicro(2018)无此方法,索引无文本源 |
| G4 | DeviceManager 缺 GetManufacturer | 强制方法(2023) | A.3/A.5 | 高,同上 |
| G5 | Root Block 缺 GetControlObjects | 强制方法(2023) | A.3/A.6 | 高,OCAMicro(2018)无此方法,索引无文本源 |
| G6 | SubscriptionManager 缺 AddPropertyChangeSubscription2 | 强制方法(EV2) | A.3 | 中,OCAMicro(2018)仅 EV1,索引无文本源 |
| G7 | SubscriptionManager 缺 RemovePropertyChangeSubscription2 | 强制方法(EV2) | A.3 | 中,同上 |
| G8 | 所有对象缺 event PropertyChanged | 强制事件 | A.3 | 中:基集要求 |
| G9 | 非强制方法返回 BadMethod 而非 NotImplemented | 方法语义 | B.3.0/决策4 | 低:细节合规 |
| G10 | 类版本号(DeviceManager=4/NetworkManager=3)未经 2023 校验 | 类版本 | A.1.1 | 待查:OCAMicro(2018)均为 2,2023 值需 AES70-2A |

**不在 Spec2 范围**(确认弃用或属 Spec3):
- OcaControlNetwork/ApplicationNetwork/MediaTransportNetwork(CM3 弃用,A.4)
- 媒体桥接(MediaClock/StreamConnector/SessionManager 双向桥接)→ Spec3
- TLS/UDP/WebSocket 传输 → 后续
- Dataset/Patch → 后续

---

## §C Phase 0 坐实状态(2026-07-10 更新)

Phase 0 已用 OCAMicro 源码考证。**关键限制:本地 OCAMicro(`oca-tools-probe`)是 2018 版**--2023 新增方法(GetProduct/GetManufacturer/GetControlObjects/EV2 四方法)在其源码中**不存在**,无法从 OCAMicro 坐实这些方法的方法索引/签名。逐项状态:

| # | 开放问题 | 状态 | 证据/出路 |
|---|---------|------|----------|
| C1 | GetProduct/GetManufacturer 方法索引与签名 | ✗ OCAMicro 无此方法 | 2018 OCAMicro 仅有 GetModelDescription(index=6,返回 {manufacturer,name,version})。2023 方法索引无文本源(AES70-2A/XMI)。出路:获 AES70-2-2023 XMI,或 MITM 抓 2023 控制器 |
| C2 | GetControlObjects 方法索引与返回结构 | ✗ OCAMicro 无此方法 | 2018 OCAMicro Block 仅有 GetMembers(index=5)、GetMembersRecursive(index=6)。GetControlObjects 全局搜索 0 命中,疑为 2023 新增。出路同 C1 |
| C3 | AddPropertyChangeSubscription2 / Remove 索引 | ✗ OCAMicro 仅 EV1 | 2018 OCAMicro SubscriptionManager 是 EV1:AddSubscription=1、RemoveSubscription=2、AddPropertyChangeSubscription=5、RemovePropertyChangeSubscription=6。EV2 四方法不存在。Spec1 methods.hpp 的 EV2 候选索引仍需 AES70-2A XMI 校验 |
| C4 | 类版本号(DeviceManager=4/NetworkManager=3) | ⚠ 部分 | OCAMicro(2018)四类均为 2。Spec1 SubscriptionManager/Block=2 与之一致;DeviceManager=4/NetworkManager=3 偏高(疑反映更晚 spec)。2023 当前版本需 AES70-2A |
| C5 | 2018 工具实际拒了什么 | ⏳ 待重跑 | 需重跑官方工具 + MITM 抓包。现强烈怀疑首要拒因是 **G0 ClassID 错误**(DeviceManager 自报 {1,2,1}=OcaNetwork),而非 G1 ONo |
| C6 | PropertyChanged 事件投递路径 | ⏳ 待设计 | EV2 机制已确认(AES70-3 §9.4,Spec1 已用)。PropertyChange 订阅编码待 C3 坐实后设计 |

**Phase 0 已坐实的硬结论(可直接用于实施):**
- ✅ ClassID 类树结构(A.1.1):1.2=Agent、1.3=Manager。三 Manager ClassID 必须从 {1,2,x} 改 {1,3,x}。
- ✅ GetMembers 索引=5、返回 `List<ObjectIdentification{ONo,ClassID,ClassVersion}>`(Spec1 db371a2 已正确)。
- ✅ EV2 是 2023 现行机制(AES70-3 §9.4),Spec1 方向正确。
- ✅ EV2 Notification2 帧格式(AES70-3 §9.4.4),Spec1 已实现。

**Phase 0 暴露的考证能力边界:** 2023 新增方法(G3-G7)的方法索引在 OCAMicro(2018)和 PDF 正文均无文本源,只在 AES70-2A 注册表/Annex A XMI 中。当前无该文本源,这构成"2023 Annex B 完整合规"的客观障碍。

---

## §D Spec2 目标口径(定稿,2026-07-10 修订)

> **修订背景:** Phase 0 揭示两件事,需对原定稿(决策 #5/#6)做调整:(1) Spec1 失败首要根因是 **G0 ClassID 错误**(原误判为 G1 ONo);(2) 本地 OCAMicro 是 2018 版,2023 新增方法(G3-G7)无文本源坐实,与"2023 Annex B 完整合规"目标存在客观障碍。下列 §D 反映修订,口径调整点待用户确认(见末尾"待用户确认")。

### 合规基准:以 AES70-2023 Annex B 为权威(原则保留,实施分层)

- 长期目标仍是对齐 AES70-2023 Annex B。
- **但 2023 新增方法(G3-G7)的方法索引当前无文本源**,需先获 AES70-2-2023 XMI 或 MITM 抓 2023 控制器才能正确实现。在此之前,先闭环**有硬证据的项**(G0/G1/G2/G9/G10)。
- 2018 官方工具作为可用验证手段;G0 修正后预期对象合规测试大幅改善(因 DeviceManager 不再自报为 OcaNetwork)。

### 范围边界:分两阶段(修订)

**阶段一(有硬证据的项,先过对象合规测试):**

| 缺口 | 改动 | 证据 | 验证 |
|------|------|------|------|
| **G0** | **三 Manager ClassID {1,2,x}->{1,3,x}**(DeviceManager->{1,3,1}、SubscriptionManager->{1,3,4}、NetworkManager->{1,3,6});methods.hpp DefLevel 注释更正 | A.1.1 OCAMicro 编译期常量 | oca-test + MITM + 2018 工具 |
| G1 | ONo 2 错配修正:NetworkManager 迁出 ONo 2(迁 ONo 6 或删除) | A.2 Table 3 | MITM + 2018 工具 |
| G2 | 与 G1 一并处理 | A.2 | 同上 |
| G9 | 非强制方法返回 NotImplemented(而非 BadMethod) | B.3.0 | oca-test 单测 |
| G10 | 类版本号:DeviceManager/NetworkManager 降到与证据一致(暂用 OCAMicro 2018 值 2,或保留待 2A 确认) | A.1.1 | oca-test |

阶段一完成标志:G0/G1/G2/G9/G10 闭环,官方工具对象合规测试通过(或经 MITM 坐实剩余失败为 2023 方法缺失/2018 工具局限)。

**阶段二(2023 新增方法,需先补文本源):**

| 缺口 | 改动 | 前置条件 |
|------|------|----------|
| G3 | DeviceManager 补 GetProduct | 获 AES70-2-2023 XMI 或抓 2023 控制器定索引/签名 |
| G4 | DeviceManager 补 GetManufacturer | 同上 |
| G5 | Root Block 补 GetControlObjects | 同上 |
| G6 | SubscriptionManager 补 AddPropertyChangeSubscription2 | 获 EV2 方法索引(2A XMI) |
| G7 | SubscriptionManager 补 RemovePropertyChangeSubscription2 | 同上 |
| G8 | 所有对象支持 event PropertyChanged | C3 坐实后设计投递路径 |

阶段二完成标志:达 AES70-2023 Annex B 完整最低合规(需 2023 文本源到位)。

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

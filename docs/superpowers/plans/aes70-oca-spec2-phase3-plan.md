# AES67 daemon AES70/OCA Spec3 实施计划(草案,阶段二完结后规划)

> 状态:**5/5 已达成**。Aes70CompliancyTestTool v2.0.1 AES70-2018 test4 Minimum Object Compliancy **Passed**(2026-07-11 第三次 Win 验收,commit `51f737d`)。
> 上游:`docs/superpowers/specs/aes70-oca-spec2-design.md`(权威设计,§Spec3)。
> 分支:延续 `feature/aes70-oca`。

## Spec1/Spec2 已完成回顾

- Spec1(2/5):控制平面最小可用。commit 范围 `4b3d1d2`..`4008ef8`。
- Spec2 阶段一(3/5):硬证据缺口 G0/G1/G2/G9/G10/G12。commit `2411497`..`eb7afc1`。
- Spec2 阶段二(**4/5**):补 2018 强制方法 + EV1 订阅 + 2023 GetProduct/GetManufacturer + transport 单命令异常修复。commit `82e66cd`..`44015c7`。oca-test 26/26 全绿。OCC Object Compliancy(test5)从 Failed 转 **Passed**。
- 已达 AES70-2023 Annex B 订阅 + 强制方法最低合规(除 CM3 对象)。

## test4 真实失败根因调研(2026-07-10,Aes70CompliancyTestTool v2.0.1 AES70-2018)

### 失败现象
test4 Minimum object compliancy 报:
```
ERROR: Missing mandatory object OcaBlock
ERROR: Missing mandatory object OcaNetwork
ERROR: Missing mandatory object OcaControlNetwork
Testing 0 mandatory object(s) for compliancy
```
最后一行 `mandatoryObjects=0` 是关键:工具的 deviceReportedObjects 里有根块{1,1,3}@ONo 100(pcap 确认 GetClassIdentification 返回 `{1,1,3} v2`),但**所有** mandatory BlockMemberInfo(含 OcaBlock{1.1.3})在 TestObjects 第一个匹配循环中 `bFound=false`。

### 根因已坐实(2026-07-10 精读 MinimumObjectCompliancyTest.cpp GetObjects line 246-291)
`mandatoryObjects=0` / "Missing OcaBlock" **不是 classID 编码问题**,而是**工具侧 GetObjects 回退路径的赋值覆盖丢根块**:

1. line 252 `GetClassIdentification(OCA_ROOT_BLOCK_ONO=100)` OK → line 255 `outputMembers.Add(根块{1,1,3}@100)`(根块已入列表)。
2. line 260 `GetMembersRecursive(100)`:daemon 当前对 `kBlockGetMembersRecursive`(defLevel3 mi=6)返 **NotImplemented(6)** → `bTestRootBlockResult=false`。
3. 走 else 回退(line 272-287):line 276 `GetMembers(100)` 返 OK + [1,4,6],line 279 **`outputMembers = members;`(赋值覆盖)** → **根块 100 被丢弃**,outputMembers 只剩 [1,4,6]。
4. `HandleMembers` 递归(line 280)只对 classID 是 OcaBlock{1,1,3} 前缀的成员做 GetMembers 递归;DevMgr{1,3,1}/SubMgr{1,3,4}/NetMgr{1,3,6} 均非 Block 前缀 → 不递归。
5. 最终 `deviceReportedObjects` = [1,4,6],**根块{1,1,3}@100 不在**。testObjects 第一个匹配循环遍历 m_blockMemberInfos,根块缺席 → OcaBlock{1.1.3} `bFound=false` → "Missing mandatory object OcaBlock";同理 OcaNetwork/OcaControlNetwork 不在 deviceReportedObjects(未实例化)→ 一起 Missing。`mandatoryObjects=0`。

匹配逻辑本身(line 537-543)无 bug:OcaBlock{1,1,3} vs 根块{1,1,3} fieldCount 均 3、memcmp==0 → **本应匹配**。问题纯粹是根块没进 `deviceReportedObjects`。

### daemon 侧解法(dual fix)
- **Fix-A(消除"Missing OcaBlock"):实现 `OcaBlock::GetMembersRecursive`(defLevel3 mi=6)返 OK + 非空**。工具即走 GetObjects line 262 的 if 分支(累加到 outputMembers,**不覆盖**根块),deviceReportedObjects 含根块 100 → OcaBlock{1.1.3} 匹配命中。返回结构:Ocp1List<OcaBlockMember>,每元素 = {ONo u32 + ClassID(fieldCount u16+levels u16*)+ ClassVersion u16 + ContainerONo u32}。成员 = 根块直系 [1,4,6],ContainerONo=100。
- **Fix-B(消除"Missing OcaNetwork/OcaControlNetwork"):实例化 CM3 网络对象**,加入 GetMembers / GetMembersRecursive 成员列表。OcaBlock 既已由 Fix-A 解决,无需新增 OcaBlock 实例(根块自身即 OcaBlock)。

### CM3 对象权威事实(ReferenceOCCMembers.xml 坐实)

| 类 | ClassID | AvailableSince | DeprecatedSince | 2018 Mandatory | 必备方法(2018) |
|----|---------|----------------|-----------------|----------------|----------------|
| OcaBlock | 1.1.3 | - | - | true(自 2015) | GetMembers(5);GetMembersRecursive(6)2018 起非强制 |
| OcaNetwork | 1.2.1 | - | **AES70-2018** | true(2018 仍 mandatory,无 false 覆盖) | GetLinkType(1)/GetIDAdvertised(2)/GetControlProtocol(4)/GetMediaProtocol(5)/GetSystemInterfaces(9)(2018 强制);SetIDAdvertised(3)/GetStatus(6)/GetStatistics(7)/ResetStatistics(8) 2018 起 Mandatory=false |
| OcaApplicationNetwork | 1.4 | AES70-2018 | - | **非 mandatory**(无 MandatoryMap=true 条目) | GetServiceID(4)/GetSystemInterfaces(6) 2018 强制(若实例化) |
| OcaControlNetwork | 1.4.1 | AES70-2018 | - | **true** | GetControlProtocol(1) |
| OcaMediaTransportNetwork | 1.4.2 | AES70-2018 | - | **true(DeviceType=Streaming)** | GetMediaProtocol(1)/GetPorts(2)/GetMaxSourceConnectors(5)/GetMaxSinkConnectors(6)/GetMaxPinsPerConnector(7) |

BlockMembers ONo 范围:**4096-4294967295**(非根块成员;根块自身 ONo=100 例外)。

### Spec3 冲 5/5 路径(v2,根因坐实后收敛)
**范围收敛**:日志只报 3 条 Missing(OcaBlock/OcaNetwork/OcaControlNetwork),**未报 OcaMediaTransportNetwork**。源码精读印证:MediaTransportNetwork 的 mandatory 受 `DeviceType=Streaming` 门控(BaseTestClass.cpp:499-504);用户运行工具时未传 `-t streaming` → `GetSupportedDeviceTypes()` 不含 OCA_STREAMING → 该 Mandatory 节不满足 → isMandatory 留 false → 不入 m_blockMemberInfos → 不报 Missing。同理 OcaApplicationNetwork 无 MandatoryMap=true 条目 → 非强制。**CM3 实例化范围收敛到 2 类:OcaNetwork{1.2.1} + OcaControlNetwork{1.4.1}**。
1. **Fix-A(GetMembersRecursive 实装)**:`OcaBlock::exec` 新增 `kBlockGetMembersRecursive`(defLevel3 mi=6)分支,返 OK + Ocp1List<OcaBlockMember>。每元素 = `{ONo u32 + ClassID(fieldCount u16+levels u16*) + ClassVersion u16 + ContainerONo u32}`,成员 = 根块直系 [1,4,6, +CM3 ONo],ContainerONo=100。工具 GetObjects line 260 rc==OK && Count>0 → 走 if 累加分支(line 262-271),**不再赋值覆盖根块** → deviceReportedObjects 含根块 100 → OcaBlock{1.1.3} 匹配命中。**单此一项即消除 "Missing OcaBlock"**。
2. **Fix-B(实例化 2 个 CM3 网络对象)**:新增 `network.cpp/.hpp`(OcaNetwork{1.2.1},defLevel 4)、`control_network.cpp/.hpp`(OcaControlNetwork{1.4.1},defLevel 3)。注释标注 **`DeprecatedSince AES70-2018` / `2023 立场:已弃用,本实例仅为兼容 AES70-2018 工具的最小强制实例`**。分配 ONo >=4096(network=4097,control_network=4098,避开 4096 边界),注册 registry,加入根块 GetMembers/GetMembersRecursive 成员列表。
3. **实现 2018 强制方法(最小合规)**:
   - OcaNetwork{1.2.1}:GetLinkType(1)/GetIDAdvertised(2)/GetControlProtocol(4)/GetMediaProtocol(5)/GetSystemInterfaces(9) 五个 2018 仍强制方法。最小返:LinkType 默认值(如 Ethernet=0)、IDAdvertised 空 string、ControlProtocol=OCP1(1)、MediaProtocol=Undefined(0)、SystemInterfaces 空 List<NetworkAddress>。
   - OcaControlNetwork{1.4.1}:GetControlProtocol(1) 返 OCP1=1。
   - **DefLevel 一致性**:OcaNetwork{1.2.1} defLevel 4(OcaNetwork 在 OcaAgent{1.2} 之下,OcaRoot{1}.OcaAgent{1.2}.OcaNetwork{1.2.1}),对照 OCAMicro `OcaLiteNetwork` 类树核定;OcaControlNetwork{1.4.1} defLevel 3。两者 exec 第一层按 defLevel 分派,非本层方法委托基类(OcaRoot.handle_root / Worker)以确保 GetClassIdentification 等 OcaRoot 方法仍工作。
4. **DefLevel 常量**:在 `methods.hpp` 新增 `kDefLevelNetwork=4`、`kDefLevelControlNetwork=3`;方法索引 `kNet2GetLinkType=1`/`kNet2GetIDAdvertised=2`/`kNet2GetControlProtocol=4`/`kNet2GetMediaProtocol=5`/`kNet2GetSystemInterfaces=9`(OcaNetwork Agent 层级,需对照 OCAMicro `OcaLiteNetwork.defLevel` 核实,可能 defLevel=4 也可能继承 Agent 的 2,以上线协议无差别——exec 仅按 defLevel 分派到本类或委托)。
5. **验证**:oca-test 加 GetMembersRecursive wire 用例 + CM3 对象 classID/方法用例;oca-probe 扩展;用户 Win 重跑 test4(确认未传 -t streaming)→ **5/5**(预期)。若仍缺,根据新日志的 Missing 条目二次收敛。

### 关键风险/开放问题
- **OcaNetwork defLevel 待核实**:OCAMicro `OcaLiteNetwork` 的 defLevel 是 4(Network 在 Agent{1.2} 之下)还是别的值。需读 `OcaLiteNetwork.h` 的 `static const OcaLiteClassID CLASS_ID` 与 defLevel 常量,确保 exec 分派层与 GetClassIdentification 返回的 classID 一致(**工具 memcmp 比较的是线端 classID 字段,defLevel 仅 daemon 内部分派用**——线端只看 classID{1,2,1} 三个字段)。
- OcaNetwork{1.2.1} 在 2023 已弃用,但 2018 工具 mandatory → 补它有违 2023 Annex B 权威取舍(用户明确"符合 2018 但标 2023 已弃用",Acceptable)。源码注释与设计文档须显式声明"为 2018 工具兼容添加 2023 弃用对象"。
- OcaMediaTransportNetwork/OcaApplicationNetwork **本次不实例化**:前者非 streaming 门控下非 mandatory(用户不传 -t streaming),后者永非 mandatory。若将来用户传 -t streaming,再评估 MediaTransportNetwork。
- 补 CM3 后需重跑 oca-test 26/26 + test4 真机,确认无方法被工具判 BadMethod/NotImplemented(尤其 OcaNetwork 5 个强制方法的 status 不能是 BAD_METHOD/NOT_IMPLEMENTED,见 CheckMethods line 55-57)。

## 后续任务(Spec3 及之后,粗规划)

1. **Spec3 阶段 CM3 对象补齐**(本计划,目标 5/5)。
2. **OcaWorker defLevel 2 方法**(GetEnabled/SetEnabled/GetPorts,Block 作为 Worker 时):2018 工具未测当前 Block 对象顺延项,Spec3 评估。
3. **event PropertyChanged 实际投递**:EV1 AddSubscription 返回 OK 已过"事件已实现"判定;实际 Notification2 投递到 EV1 订阅者为 bonus(2018 客户端 type-5 解析未验证),Spec3 验证。
4. **media 桥接主线**:MediaClock/StreamConnector/SessionManager(真正音频控制功能,非合规)→ 独立 Spec。
5. **上游同步检查**:feature/aes70-oca 合并 master 前跑 buildfake + daemon-test;CM3 对象属 fork 专有不回上游。

## 验证基线
- oca-test 26/26(Spec2 终态)。
- 真实控制器:Aes70CompliancyTestTool v2.0.1 AES70-2018,当前 **4/5**。
- 抓包工具链:tcpdump 旁路 + `daemon/oca/tools/oca-parse-pcap.py`(零依赖)。
- daemon 构建路径注意:`oca-dev.sh` 用 out-of-source `daemon/build/` 二进制。
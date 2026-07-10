# AES67 daemon AES70/OCA Spec3 实施计划(草案,阶段二完结后规划)

> 状态:**规划草案**。基于 Spec2 阶段二 4/5 达成(commit 44015c7)+ test4 真实失败根因调研(2026-07-10)。
> 上游:`docs/superpowers/specs/aes70-oca-spec2-design.md`(权威设计,§阶段二验收)。
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

### 调研结论(test4 匹配逻辑,MinimumObjectCompliancyTest.cpp TestObjects + BaseTestClass.cpp ParseObject)
- 匹配条件:`xmlClassId.fieldCount == reported.fieldCount`(或 xml 更短基类前缀)+ `memcmp(fields)==0`。OcaBlock{1,1,3} vs 根块{1,1,3}:fieldCount 均为 3,字段相同 → **应匹配**。
- AvailableSince 版本门:OcaBlock 无 AvailableSince(availableSince=0),OcaNetwork{1.2.1} DeprecatedSince=AES70-2018(但 ParseObject **未对 deprecatedSince 做排除**——只看 availableSince),OcaControlNetwork{1.4.1}/OcaApplicationNetwork{1.4}/OcaMediaTransportNetwork{1.4.2} AvailableSince=AES70-2018 → 测试 AES70-2018 下 bAvailable=true,均加入 m_blockMemberInfos。
- **未解疑点**:mandatoryObjects=0 表明连 OcaBlock{1.1.3} 也未匹配到根块{1,1,3},与源码匹配逻辑表面矛盾。两种可能:
  1. 根块 GetMembersRecursive(3,6)返 NotImplemented(6),回退 GetMembers(3,5)返 managers [1,4,6](**不含根块自身**),但 GetObjects line 255 显式补根块 → 应有 4 对象。
  2. classID 比较存在未发现的语义差异(如 OcaLiteClassID 字段数/编码)。**需 T1 spec-0 阶段先复现确认**(见下)。
- test5 OCC Compliancy(对象 1/4/6)**Passed** → 根块成员枚举与管理器对象在 test5 语境下可接受。test4 失败**仅**因 mandatory BlockMember 对象(OcaBlock/OcaNetwork/OcaControlNetwork),**非**强制方法实现问题(DevMgr/SubMgr/NetMgr 方法 test5 全 implements 通过)。

### CM3 对象权威事实(ReferenceOCCMembers.xml 坐实)

| 类 | ClassID | AvailableSince | DeprecatedSince | 2018 Mandatory | 必备方法(2018) |
|----|---------|----------------|-----------------|----------------|----------------|
| OcaBlock | 1.1.3 | - | - | true(自 2015) | GetMembers(5);GetMembersRecursive(6)2018 起非强制 |
| OcaNetwork | 1.2.1 | - | **AES70-2018** | true(2018 仍 mandatory,无 false 覆盖) | GetLinkType(1)/GetIDAdvertised(2)/GetControlProtocol(4)/GetMediaProtocol(5)/GetSystemInterfaces(9)(2018 强制);SetIDAdvertised(3)/GetStatus(6)/GetStatistics(7)/ResetStatistics(8) 2018 起 Mandatory=false |
| OcaApplicationNetwork | 1.4 | AES70-2018 | - | **非 mandatory**(无 MandatoryMap=true 条目) | GetServiceID(4)/GetSystemInterfaces(6) 2018 强制(若实例化) |
| OcaControlNetwork | 1.4.1 | AES70-2018 | - | **true** | GetControlProtocol(1) |
| OcaMediaTransportNetwork | 1.4.2 | AES70-2018 | - | **true(DeviceType=Streaming)** | GetMediaProtocol(1)/GetPorts(2)/GetMaxSourceConnectors(5)/GetMaxSinkConnectors(6)/GetMaxPinsPerConnector(7) |

BlockMembers ONo 范围:**4096-4294967295**(非根块成员;根块自身 ONo=100 例外)。

### Spec3 冲 5/5 路径(v1 草案)
1. **spec-0 复现**:对当前 4/5 daemon 跑 test4 + tcpdump,逐字节核对 GetObjects 的 deviceReportedObjects 内容、TestObjects 匹配循环为何 bFound=0。**先解疑点**,避免补错对象。
2. **新增 CM3 对象类**:在 `daemon/oca/classes/` 新增 `network.cpp/.hpp`(OcaNetwork{1.2.1})、`application_network.hpp`(OcaApplicationNetwork{1.4})、`control_network.hpp`(OcaControlNetwork{1.4.1})、`media_transport_network.hpp`(OcaMediaTransportNetwork{1.4.2})。源码/头注释**标注 `DeprecatedSince AES70-2018`(OcaNetwork)/ `2023 进一步弃用文档说明`**,保留 2023 立场(2018 先例:OcaNetwork 即 DeprecatedSince 2018 仍 mandatory)。
3. **实例化**:给 4 个网络对象分配 ONo(>=4096,如 4096-4099),注册到 registry,加入根块 GetMembers 成员列表(让 deviceReportedObjects 含它们)。**根块自身**已 ONo 100 = OcaBlock{1.1.3},若 spec-0 确认根块匹配没问题则无需动。
4. **实现 2018 强制方法**:OcaControlNetwork GetControlProtocol(1);OcaMediaTransportNetwork GetMediaProtocol(1)/GetPorts(2)/GetMax*(5/6/7);OcaNetwork 五个 2018 仍强制方法(最小返空/默认值合规)。ApplicationNetwork 若实例化实现 GetServiceID/GetSystemInterfaces。
5. **DefLevel 常量**:OcaNetwork defLevel 4?需对照 OCAMicro `OcaLiteNetwork` 类树。OcaApplicationNetwork{1.4} defLevel 2,OcaControlNetwork{1.4.1}/MediaTransport{1.4.2} defLevel 3。
6. **验证**:oca-test 加 CM3 对象用例;oca-probe 扩展;用户 Win 重跑 test4 → **5/5**(预期)。

### 关键风险/开放问题
- **spec-0 疑点未解**:mandatoryObjects=0 表明即使 OcaBlock{1.1.3} 也未匹配根块——若这是 classID 编码问题,补 CM3 对象也不一定能修。**必须先复现坐实**。
- OcaNetwork{1.2.1} 在 2023 已弃用,但 2018 工具 mandatory → 补它有违 2023 Annex B 权威取舍(用户已明确"符合 2018 但标 2023 弃用",Acceptable)。
- OcaMediaTransportNetwork Mandatory DeviceType=Streaming → daemon 是否算 streaming 设备需确认(TestContext::GetSupportedDeviceTypes)。
- 补 CM3 后 test4 过 5/5,但 fork 的 2023 合规立场需在设计文档显式说明"为 2018 工具兼容添加 2023 弃用对象"。

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
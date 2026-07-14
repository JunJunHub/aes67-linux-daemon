# Spec5 设计：媒体桥接——OCA 对象映射 AES67 音频运行时

**版本**：2026-07-13
**状态**：草案
**前置**：Spec4 已完成(oca-test 34/34，PropertyChanged 端到端验证通过)

## 目标

把 daemon 从"协议合规自包含门面"升级为"可通过 AES70 控制器真实操控音频流"的设备：

1. **OcaAudioBridge 抽象接口**：OCA 层零依赖 SessionManager，通过纯虚接口读写音频运行时状态。
2. **OcaMediaTransportNetworkAES67** {1,4,2,0xFFFF,0xFA,0x2EE9,1}：暴露 Source/Sink 连接器，支持 AddSourceConnector/AddSinkConnector/DeleteConnector/SetConnectorPinMap/SetConnectorConnection 等控制器核心操作。
3. **OcaMediaClock3** {1,2,15}：暴露 PTP 时钟状态与采样率，支持 GetCurrentRate/SetCurrentRate，PTP 锁态变化自发射 PropertyChanged。
4. **OcaMediaClockManager** {1,3,7}：时钟集合管理器，返回 Clock3 列表。
5. **OcaMediaClock** {1,2,6} 废弃类 NOT_IMPLEMENTED 存根：合规工具仍检查，返回 NotImplemented。
6. **现有类现实化**：OcaNetwork 读真实接口/IP/MAC；OcaNetworkManager 返回真实网络对象列表。
7. **mDNS `_oca._tcp` TXT 记录丰富**：添加设备 IP/MAC/通道数等 Fitcan 控制器期望的 TXT 字段。
8. **合规**：AES70-2018 合规工具通过（新增媒体类检查项）。
9. **真实控制器验证**：Fitcan AES70Controller 能发现设备、枚举连接器、创建路由。

## 验收

- 构建：`oca-dev.sh` 成功
- 合规：Aes70CompliancyTestTool AES70-2018 通过（含媒体类检查）
- oca-test：34 现有 + N 新增全部通过
- oca-probe：新增媒体探测段（PTP 状态读、连接器 CRUD、Sink 状态读）输出 `[OK]`
- 真实控制器：Fitcan AES70Controller 能 mDNS 发现设备、GetMembersRecursive 枚举对象、GetPorts/GetSourceConnectors/GetSinkConnectors 读取连接器、AddSourceConnector/AddSinkConnector 创建连接器
- 回归：Spec1~4 既有断言不破

## 不在 Spec5 范围

- OcaStreamConnector {1,2,11} 独立对象注册（合规 optional，控制器不直接交互，数据由 MTN 内部管理）
- Sink status 主动推送（无 SessionManager observer 支持，仅响应 GetConnectorStatus 查询）
- OcaBooleanSensor / OcaNetworkSignalChannel（控制器用于静音检测/信号通道，本阶段不实现）
- Fitcan 私有方法 UpdateRouteTableCommand/TriggerUpdateRouteTableNotification（daemon 不需要显式路由更新命令）
- TLS / UDP / WebSocket 传输
- Dataset 序列化

## 设计决策

### 决策 1：OcaAudioBridge 抽象接口 vs 直接持有 SessionManager

**选择：OcaAudioBridge 抽象接口**。

| 方案 | 优点 | 缺点 |
|------|------|------|
| A: OcaServer 直接持有 `shared_ptr<SessionManager>` | 简单直接 | OCA 编译单元依赖 SessionManager 头文件/类型；测试必须起 SessionManager |
| **B: OcaAudioBridge 纯虚接口** | OCA 层零依赖 SessionManager；测试可用 OcaFakeBridge；接口从 OCA 需求倒推，更干净 | 多一层间接 |

层次关系：

```
OCA 对象 → OcaAudioBridge（纯虚接口，OCA 层视角）
                ↓
      OcaSessionManagerBridge（实现，持有 SessionManager + Config）
                ↓
          SessionManager（业务编排层）
                ↓
      DriverManager / FakeDriverManager（驱动抽象）
```

Bridge 接口从 OCA 需求倒推，不暴露 `StreamSource`/`StreamSink` 等内部类型。

### 决策 2：动态 ONos vs 固定槽位

**选择：动态 ONos**。

Source/Sink 数量是动态的（0-64）。OcaBlock.GetMembers 和 OcaNetworkManager.GetNetworks 必须反映实际存在的流。预注册 128 个空对象与 AES70 语义冲突。

实现方式：connector 数据作为 OcaMediaTransportNetworkAES67 的内部状态管理，不注册为独立 OcaStreamConnector 对象。理由：

- 合规工具标记 OcaStreamConnector 所有方法为 optional
- 真实控制器不直接与 StreamConnector 对象交互（通过 MTN 的 connector API 管理）
- 简化实现，YAGNI

### 决策 3：OcaMediaClock vs OcaMediaClock3

**两者都实现**：

- OcaMediaClock3 {1,2,15}：AES70-2018 必选，真实控制器使用，完整实现
- OcaMediaClock {1,2,6}：已废弃，合规工具仍有检查项，所有方法返回 NOT_IMPLEMENTED

### 决策 4：OcaMediaTransportNetworkAES67 私有方法

控制器使用的三个私有扩展方法：

| 方法 | MethodIndex | 处理方式 |
|------|-----------|---------|
| DeleteAllConnectors | 1000 | 实现：遍历删除所有 connector，实用价值高 |
| UpdateRouteTableCommand | 0x8000 | NOT_IMPLEMENTED：daemon 不需要显式路由更新 |
| GetPTPStatus | 0x8002 | 实现：返回 PTP 锁态/模式/偏移 |

AES67 子类额外 6 个方法（GetSendPacketTimes 等）全部 NOT_IMPLEMENTED。

### 决策 5：SinkStatus 推送

**选择 B：仅响应 GetConnectorStatus 查询**。

SessionManager 没有 sink status 变化回调（只有 add/remove observer）。实现定时轮询违反 YAGNI。控制器的 ConnectorStatusChanged 事件暂不发射，留待后续。

### 决策 6：mDNS TXT 记录扩展

Fitcan 控制器通过 mDNS `_oca._tcp` 的 TXT 记录提取设备信息。当前 MdnsPublisher 仅发布 `txtvers=1` + `protovers=1`。需要扩展：

```
txtvers=1
protovers=1
ip_addr=<primary IP>
ip_addr_sec=<secondary IP, if ST 2022-7>
mac_addr=<MAC>
device_id=<node_id>
channels=<max channels>
firmware=<daemon_version>
```

MdnsPublisher 需接收这些字段（从 OcaServerConfig 扩展传入）。

## 架构

### ONO 分配

| ONo | 类 | ClassID | 来源 |
|-----|---|---------|------|
| 1 | OcaDeviceManager | {1,3,1} | 已有 |
| 4 | OcaSubscriptionManager | {1,3,4} | 已有 |
| 6 | OcaNetworkManager | {1,3,6} | 已有 |
| **7** | **OcaMediaClockManager** | **{1,3,7}** | 新增（AES70 固定） |
| 100 | OcaBlock (Root) | {1,1,3} | 已有 |
| 4097 | OcaNetwork | {1,2,1} | 已有 |
| 4098 | OcaControlNetwork | {1,4,1} | 已有 |
| **8192** | **OcaMediaTransportNetworkAES67** | **{1,4,2,0xFFFF,0xFA,0x2EE9,1}** | 新增 |
| **8193** | **OcaMediaClock3** | **{1,2,15}** | 新增 |
| **8194** | **OcaMediaClock** | **{1,2,6}** | 新增（废弃存根） |

### 类继承关系（新增部分高亮）

```
Object (abstract)
  +-- OcaRoot {1} v2
        +-- OcaWorker {1,1,1} v2
        |     +-- OcaBlock {1,1,3} v2
        +-- OcaManager {1,2} v2
        |     +-- OcaDeviceManager {1,3,1} v2
        |     +-- OcaNetworkManager {1,3,6} v2
        |     +-- OcaSubscriptionManager {1,3,4} v2
        |     +-- ★OcaMediaClockManager {1,3,7} v2
        +-- OcaAgent {1,2} v2
        |     +-- OcaNetwork {1,2,1} v1
        |     +-- ★OcaMediaClock3 {1,2,15} v2
        |     +-- ★OcaMediaClock {1,2,6} v2 (废弃)
        |     +-- ★OcaStreamConnector {1,2,11} v2 (不注册独立对象)
        +-- OcaApplicationNetwork {1,4} v1
              +-- OcaControlNetwork {1,4,1} v1
              +-- ★OcaMediaTransportNetwork {1,4,2} v1 (抽象基类)
                    +-- ★OcaMediaTransportNetworkAES67 {1,4,2,...} v1
```

### OcaAudioBridge 接口

```cpp
// oca/oca_audio_bridge.hpp
namespace oca {

class OcaAudioBridge {
 public:
  virtual ~OcaAudioBridge() = default;

  // ── PTP 时钟 ──
  struct PtpConfig { uint8_t domain; uint8_t dscp; };
  enum class PtpLockState { Unlocked, Locking, Locked };
  struct PtpStatus { PtpLockState lock; std::string gmid; int32_t jitter; };

  virtual PtpConfig  get_ptp_config() const = 0;
  virtual bool       set_ptp_config(PtpConfig cfg) = 0;
  virtual PtpStatus  get_ptp_status() const = 0;

  // ── 采样率 ──
  virtual uint32_t   get_sample_rate() const = 0;
  virtual bool       set_sample_rate(uint32_t hz) = 0;
  virtual std::vector<uint32_t> get_supported_sample_rates() const = 0;

  // ── Source (Talker) ──
  struct SourceInfo {
    uint8_t id;
    bool enabled;
    std::string name;
    std::string codec;        // "L16", "L24", "AM824"
    std::string address;      // 组播 IP 或空
    uint8_t ttl, payload_type, dscp;
    bool refclk_ptp_traceable;
    uint32_t max_samples_per_packet;
    std::vector<uint8_t> map; // 通道映射
  };
  virtual std::vector<SourceInfo> get_sources() const = 0;
  virtual bool  add_source(const SourceInfo& s) = 0;
  virtual bool  remove_source(uint8_t id) = 0;
  virtual std::string get_source_sdp(uint8_t id) const = 0;

  // ── Sink (Listener) ──
  struct SinkInfo {
    uint8_t id;
    std::string name;
    uint32_t delay;
    std::string source_url;  // SDP URL
    std::string sdp;         // 字面 SDP
    bool use_sdp;
    bool ignore_refclk_gmid;
    std::vector<uint8_t> map;
  };
  struct SinkStatus {
    bool seq_error, ssrc_error, pt_error, sac_error;
    bool receiving, muted;
  };
  virtual std::vector<SinkInfo>   get_sinks() const = 0;
  virtual bool  add_sink(const SinkInfo& s) = 0;
  virtual bool  remove_sink(uint8_t id) = 0;
  virtual SinkStatus get_sink_status(uint8_t id) const = 0;

  // ── 网络信息 ──
  virtual std::string get_interface_name() const = 0;
  virtual std::string get_ip_addr() const = 0;
  virtual std::string get_mac_addr() const = 0;
  // I/O 端口数（从 driver 查询）
  virtual uint32_t get_input_channels() const = 0;
  virtual uint32_t get_output_channels() const = 0;

  // ── Observer（daemon → OCA 事件传播） ──
  using PtpObserver      = std::function<void(const PtpStatus&)>;
  using SourceObserver   = std::function<void(uint8_t id, bool added)>;
  using SinkObserver     = std::function<void(uint8_t id, bool added)>;
  virtual void set_ptp_observer(PtpObserver cb) = 0;
  virtual void set_source_observer(SourceObserver cb) = 0;
  virtual void set_sink_observer(SinkObserver cb) = 0;
};

}  // namespace oca
```

### OcaSessionManagerBridge 实现

```cpp
// oca/oca_session_manager_bridge.hpp
namespace oca {

class OcaSessionManagerBridge : public OcaAudioBridge {
 public:
  OcaSessionManagerBridge(std::shared_ptr<SessionManager> sm,
                           std::shared_ptr<Config> cfg);
  ~OcaSessionManagerBridge() override;

  // OcaAudioBridge 实现
  PtpConfig get_ptp_config() const override;
  bool set_ptp_config(PtpConfig cfg) override;
  PtpStatus get_ptp_status() const override;
  uint32_t get_sample_rate() const override;
  bool set_sample_rate(uint32_t hz) override;
  // ... 其余方法

 private:
  std::shared_ptr<SessionManager> sm_;
  std::shared_ptr<Config> cfg_;
  PtpObserver ptp_cb_;
  SourceObserver source_cb_;
  SinkObserver sink_cb_;
};

}  // namespace oca
```

构造时注册 SessionManager observer（add_ptp_status_observer / add_source_observer / add_sink_observer），在回调中转发到 OCA 层 observer。

### Connector 模型映射

OcaMediaTransportNetwork 内部维护 connector 列表，与 daemon Source/Sink 的映射：

| AES70 概念 | daemon 概念 | 映射方式 |
|-----------|------------|---------|
| SourceConnector | StreamSource (id=0..63) | AddSourceConnector → bridge.add_source() |
| SinkConnector | StreamSink (id=0..63) | AddSinkConnector → bridge.add_sink() |
| Connector.pinMap | Source/Sink .map | SetConnectorPinMap → 更新 map |
| Connector.connection | SDP/URL/地址参数 | SetConnectorConnection → 构造 SinkInfo |
| Connector.coding | codec 字段 | SetConnectorCoding → 更新 codec |
| Connector.status | SinkStreamStatus | GetConnectorStatus → bridge.get_sink_status() |
| DeleteConnector | remove_source/remove_sink | 按 connectorID 查找 |

#### Connector ID 分配

ConnectorID 使用 OcaUint32，按以下规则生成：
- Source connector：`base_id + source_id`（如 `0x0001` + id）
- Sink connector：`base_id + sink_id`（如 `0x0101` + id）

确保 Source 和 Sink connector ID 不冲突。

#### OcaMediaSourceConnector 序列化

```
OcaMediaSourceConnector:
  u32  connectorID
  u8   ownerNetwork(ONo 高位=0, 仅写低 16 位? 实际为 OcaONo=u32)
  OcaBlob  idAdvertised
  u8   sourceOrSink   // 0=Source, 1=Sink
  OcaMediaCoding  coding
  u16  pinCount
  for each pin:
    OcaMap<u16, u16> pinMap  // pinIndex → portIndex
  OcaMediaConnection connection
```

对应 OCP.1 编码（参考 OCAMicro OcaLiteMediaSourceConnector）：
- connectorID: u32
- ownerNetwork: u32 (ONo)
- idAdvertised: blob (u16 len + data)
- sourceOrSink: u8
- coding: OcaMediaCoding = {u8 codingFormatID, u8 ancillaryFormatID, u32 mediaSubType, u8 ancillarySubTypeID}
- pinCount: u16
- pinMap: OcaMap<u16, u16> = {u16 count, [u16 key, u16 value]*}
- connection: OcaMediaConnection = variant

对于 AES67，connection 使用 OcaLiteMediaStreamParametersAes67：
- u8 streamConnectorType (1=AES67)
- u32 sessionID
- u32 sessionVersion
- std::string originAddress (IP)
- u16 originPort
- std::string destinationAddress (IP)
- u16 destinationPort
- u8 ttl
- u8 payloadType
- u32 ptime (samples per packet)

### 事件传播路径

```
SessionManager worker 线程
    │ PTP 锁态变化 / Source 增删 / Sink 增删
    ▼
OcaSessionManagerBridge observer 回调
    │ 持有 OCA 对象裸指针或弱引用
    ▼
OcaMediaClock3::on_ptp_status_changed()
    → emit_property_changed(kPropLockState, ...)
    → emit_property_changed(kPropCurrentRate, ...)
OcaMediaTransportNetworkAES67::on_source_changed(id, added)
    → trigger_event(kEventSourceConnectorChanged, ITEM_ADDED/ITEM_DELETED)
OcaMediaTransportNetworkAES67::on_sink_changed(id, added)
    → trigger_event(kEventSinkConnectorChanged, ITEM_ADDED/ITEM_DELETED)
```

### mDNS TXT 记录扩展

当前 MdnsPublisher 签名为 `MdnsPublisher(string name, uint16_t port)`。

扩展为 `MdnsPublisher(string name, uint16_t port, TxtRecords txt)`：

```cpp
struct TxtRecords {
  std::string ip_addr;
  std::string ip_addr_sec;     // ST 2022-7
  std::string mac_addr;
  std::string device_id;
  uint32_t channels = 0;
  std::string firmware;
};
```

OcaServerConfig 扩展对应字段，由 main.cpp 从 Config 填充。

### 现有类修改

#### OcaNetwork

- `GetSystemInterfaces`：从 bridge 读取真实接口名/IP/MAC
- `GetMediaProtocol`：返回 AES67 编号（而非 None=0）
- `GetIDAdvertised`：返回 node_id blob

#### OcaNetworkManager

- `GetNetworks`：返回 [4097, 4098, 8192]（OcaNetwork + OcaControlNetwork + OcaMediaTransportNetworkAES67）
- `GetStreamNetworks`：返回 [8192]（OcaMediaTransportNetworkAES67）
- `GetControlNetworks`：返回 [4098]（OcaControlNetwork）
- `GetMediaTransportNetworks`：返回 [8192]（OcaMediaTransportNetworkAES67）

需注入 bridge 引用以获取运行时信息。

#### OcaBlock

- GetMembers / GetMembersRecursive：包含新增 ONo [7, 8192, 8193, 8194]
- 当前已动态查 registry，无需额外修改

#### OcaServer

- 构造时接收 `std::shared_ptr<OcaAudioBridge>`
- 注册新对象：OcaMediaClockManager(7)、OcaMediaTransportNetworkAES67(8192)、OcaMediaClock3(8193)、OcaMediaClock(8194)
- 注入 bridge 到需要它的对象（MTN、Clock3、Network、NetworkManager）
- 为新对象注入 emitter
- MdnsPublisher 传入扩展 TXT 记录

#### OcaServerConfig 扩展

```cpp
struct OcaServerConfig {
  // ... 已有字段 ...
  std::string ip_addr;
  std::string ip_addr_sec;
  std::string mac_addr;
  uint32_t channels = 0;
};
```

## 新增类详细设计

### OcaMediaClockManager {1,3,7}

**文件**：`daemon/oca/classes/media_clock_manager.hpp` / `.cpp`
**ClassID**：{1,3,7}，defLevel=3
**ONo**：7（AES70 固定）
**父类**：OcaManager

| MethodIndex | 方法 | 必选 | 实现 |
|------------|------|------|------|
| 1 | GetClocks | ✅ | 返回 []（废弃 Clock，用 Clock3） |
| 2 | GetMediaClockTypesSupported | ✅ | 返回 NotImplemented |
| 3 | GetClock3s | ✅ | 返回 [8193] |

PropertyIndex：1=ClockSourceTypesSupported, 2=Clocks, 3=Clock3s

### OcaMediaClock3 {1,2,15}

**文件**：`daemon/oca/classes/media_clock3.hpp` / `.cpp`
**ClassID**：{1,2,15}，defLevel=3
**ONo**：8193
**父类**：OcaAgent

| MethodIndex | 方法 | 必选 | 实现 |
|------------|------|------|------|
| 1 | GetAvailability | ✅ | 返回 AVAILABLE(1) |
| 2 | SetAvailability | optional | NotImplemented |
| 3 | GetCurrentRate | ✅ | bridge.get_sample_rate() → OcaMediaClockRate |
| 4 | SetCurrentRate | optional | bridge.set_sample_rate() + emit PropertyChanged |
| 5 | GetOffset | ✅ | 返回 0 |
| 6 | SetOffset | optional | NotImplemented |
| 7 | GetSupportedRates | ✅ | bridge.get_supported_sample_rates() → 列表 |
| 私有 0x8002 | GetPTPStatus | N/A | bridge.get_ptp_status() → 自定义编码 |

PropertyIndex：1=Availability, 2=TimeSourceONo, 3=Offset, 4=CurrentRate

事件：PropertyChanged via PropertyIndex 4 (CurrentRate) 和自定义 PTPStatusChanged。

#### PTP 锁态自发射

```
SessionManager::on_ptp_status_changed()
  → bridge.ptp_observer_(status)
    → OcaMediaClock3::on_ptp_status_changed(status)
      → emit_property_changed(kDefLevelAgent, kPropCurrentRate, ...)
      → trigger_event(kDefLevelRoot, kEventPropertyChanged, ...)
```

### OcaMediaClock {1,2,6}（废弃存根）

**文件**：`daemon/oca/classes/media_clock.hpp` / `.cpp`
**ClassID**：{1,2,6}，defLevel=3
**ONo**：8194
**父类**：OcaAgent

所有方法返回 NotImplemented：

| MethodIndex | 方法 |
|------------|------|
| 1 | GetType |
| 2 | SetType |
| 3 | GetDomainID |
| 4 | SetDomainID |
| 5 | GetRatesSupported |
| 6 | GetRate |
| 7 | SetRate |
| 8 | GetLockState |
| 9 | GetTypesSupported |

### OcaMediaTransportNetwork {1,4,2}（抽象基类）

**文件**：`daemon/oca/classes/media_transport_network.hpp` / `.cpp`
**ClassID**：{1,4,2}，defLevel=3
**父类**：OcaApplicationNetwork

实现 MTN 共享逻辑：connector 存储与序列化、connector 列表管理、status 查询。AES67 子类覆盖纯虚方法提供实际行为。

**方法（DefLevel 3）**：

| MethodIndex | 方法 | 必选 | 基类实现 |
|------------|------|------|---------|
| 1 | GetMediaProtocol | ✅ | 纯虚（子类返回协议类型） |
| 2 | GetPorts | ✅ | 从 bridge 获取 I/O 端口 |
| 3 | GetPortName | optional | NotImplemented |
| 4 | SetPortName | optional | NotImplemented |
| 5 | GetMaxSourceConnectors | ✅ | 纯虚（子类返回 64） |
| 6 | GetMaxSinkConnectors | ✅ | 纯虚（子类返回 64） |
| 7 | GetMaxPinsPerConnector | ✅ | 纯虚 |
| 8 | GetMaxPortsPerPin | ✅ | 纯虚（返回 1） |
| 9 | GetSourceConnectors | optional | 返回内部 source connector 列表 |
| 10 | GetSourceConnector | optional | 按 ID 查找 |
| 11 | GetSinkConnectors | optional | 返回内部 sink connector 列表 |
| 12 | GetSinkConnector | optional | 按 ID 查找 |
| 13 | GetConnectorsStatuses | ✅ | 遍历 connector 列表查 status |
| 14 | GetConnectorStatus | ✅ | 单个 connector 状态 |
| 15 | AddSourceConnector | optional | 纯虚（子类调用 bridge） |
| 16 | AddSinkConnector | optional | 纯虚（子类调用 bridge） |
| 17 | ControlConnector | optional | NotImplemented |
| 18 | SetSourceConnectorPinMap | optional | 更新 pin map |
| 19 | SetSinkConnectorPinMap | optional | 更新 pin map |
| 20 | SetConnectorConnection | optional | 更新连接参数 |
| 21 | SetConnectorCoding | optional | 更新编码 |
| 22 | SetConnectorAlignmentLevel | optional | NotImplemented |
| 23 | SetConnectorAlignmentGain | optional | NotImplemented |
| 24 | DeleteConnector | ✅ | 纯虚（子类调用 bridge） |
| 25 | GetAlignmentLevel | optional | NotImplemented |
| 26 | GetAlignmentGain | optional | NotImplemented |

EventIndex：1=SourceConnectorChanged, 2=SinkConnectorChanged, 3=ConnectorStatusChanged

### OcaMediaTransportNetworkAES67 {1,4,2,0xFFFF,0xFA,0x2EE9,1}

**文件**：`daemon/oca/classes/media_transport_network_aes67.hpp` / `.cpp`
**ClassID**：{1,4,2,0xFFFF,0xFA,0x2EE9,1}，defLevel=7
**ONo**：8192
**父类**：OcaMediaTransportNetwork

覆盖基类纯虚方法：

| 方法 | 实现 |
|------|------|
| GetMediaProtocol | 返回 AES67(3) |
| GetMaxSourceConnectors | 返回 64 |
| GetMaxSinkConnectors | 返回 64 |
| GetMaxPinsPerConnector | 返回 64（最大通道映射） |
| GetMaxPortsPerPin | 返回 1 |
| AddSourceConnector | bridge.add_source() + 存 connector + emit SourceConnectorChanged |
| AddSinkConnector | bridge.add_sink() + 存 connector + emit SinkConnectorChanged |
| DeleteConnector | bridge.remove_source/remove_sink + 清 connector + emit 事件 |
| DeleteAllConnectors(1000) | 遍历删除所有 connector |

AES67 defLevel 7 方法（全部 NOT_IMPLEMENTED）：

| MethodIndex | 方法 |
|------------|------|
| 1 | GetSendPacketTimes |
| 2 | GetReceivePacketTimes |
| 3 | GetMinReceiveBufferCapacity |
| 4 | GetMaxReceiveBufferCapacity |
| 5 | GetTransmissionTimeVariation |
| 6 | GetSupportedDiscoverySystems |

## methods.hpp 新增常量

```cpp
// OcaMediaClockManager methods (DefLevel 3, classID{1,3,7})
constexpr uint16_t kDefLevelMediaClockMngr = 3;
constexpr uint16_t kMcmGetClocks = 1;
constexpr uint16_t kMcmGetMediaClockTypesSupported = 2;
constexpr uint16_t kMcmGetClock3s = 3;

// OcaMediaClock3 methods (DefLevel 3, classID{1,2,15})
constexpr uint16_t kDefLevelMediaClock3 = 3;
constexpr uint16_t kMc3GetAvailability = 1;
constexpr uint16_t kMc3SetAvailability = 2;
constexpr uint16_t kMc3GetCurrentRate = 3;
constexpr uint16_t kMc3SetCurrentRate = 4;
constexpr uint16_t kMc3GetOffset = 5;
constexpr uint16_t kMc3SetOffset = 6;
constexpr uint16_t kMc3GetSupportedRates = 7;
// 私有
constexpr uint16_t kMc3GetPTPStatus = 0x8002;

// OcaMediaClock methods (DefLevel 3, classID{1,2,6}) - 废弃
constexpr uint16_t kDefLevelMediaClock = 3;
constexpr uint16_t kMcGetType = 1;
constexpr uint16_t kMcSetType = 2;
constexpr uint16_t kMcGetDomainID = 3;
constexpr uint16_t kMcSetDomainID = 4;
constexpr uint16_t kMcGetRatesSupported = 5;
constexpr uint16_t kMcGetRate = 6;
constexpr uint16_t kMcSetRate = 7;
constexpr uint16_t kMcGetLockState = 8;
constexpr uint16_t kMcGetTypesSupported = 9;

// OcaMediaTransportNetwork methods (DefLevel 3, classID{1,4,2})
constexpr uint16_t kDefLevelMtn = 3;
constexpr uint16_t kMtnGetMediaProtocol = 1;
constexpr uint16_t kMtnGetPorts = 2;
constexpr uint16_t kMtnGetPortName = 3;
constexpr uint16_t kMtnSetPortName = 4;
constexpr uint16_t kMtnGetMaxSourceConnectors = 5;
constexpr uint16_t kMtnGetMaxSinkConnectors = 6;
constexpr uint16_t kMtnGetMaxPinsPerConnector = 7;
constexpr uint16_t kMtnGetMaxPortsPerPin = 8;
constexpr uint16_t kMtnGetSourceConnectors = 9;
constexpr uint16_t kMtnGetSourceConnector = 10;
constexpr uint16_t kMtnGetSinkConnectors = 11;
constexpr uint16_t kMtnGetSinkConnector = 12;
constexpr uint16_t kMtnGetConnectorsStatuses = 13;
constexpr uint16_t kMtnGetConnectorStatus = 14;
constexpr uint16_t kMtnAddSourceConnector = 15;
constexpr uint16_t kMtnAddSinkConnector = 16;
constexpr uint16_t kMtnControlConnector = 17;
constexpr uint16_t kMtnSetSourceConnectorPinMap = 18;
constexpr uint16_t kMtnSetSinkConnectorPinMap = 19;
constexpr uint16_t kMtnSetConnectorConnection = 20;
constexpr uint16_t kMtnSetConnectorCoding = 21;
constexpr uint16_t kMtnSetConnectorAlignmentLevel = 22;
constexpr uint16_t kMtnSetConnectorAlignmentGain = 23;
constexpr uint16_t kMtnDeleteConnector = 24;
constexpr uint16_t kMtnGetAlignmentLevel = 25;
constexpr uint16_t kMtnGetAlignmentGain = 26;

// OcaMediaTransportNetwork events (DefLevel 3)
constexpr uint16_t kEventSourceConnectorChanged = 1;
constexpr uint16_t kEventSinkConnectorChanged = 2;
constexpr uint16_t kEventConnectorStatusChanged = 3;

// OcaMediaTransportNetworkAES67 methods (DefLevel 7)
constexpr uint16_t kDefLevelMtnAes67 = 7;
constexpr uint16_t kMtnAes67GetSendPacketTimes = 1;
constexpr uint16_t kMtnAes67GetReceivePacketTimes = 2;
constexpr uint16_t kMtnAes67GetMinReceiveBufferCapacity = 3;
constexpr uint16_t kMtnAes67GetMaxReceiveBufferCapacity = 4;
constexpr uint16_t kMtnAes67GetTransmissionTimeVariation = 5;
constexpr uint16_t kMtnAes67GetSupportedDiscoverySystems = 6;

// OcaMediaTransportNetworkAES67 私有方法
constexpr uint16_t kMtnAes67DeleteAllConnectors = 1000;
constexpr uint16_t kMtnAes67UpdateRouteTableCommand = 0x8000;

// MTN Property indices
constexpr uint16_t kMtnPropProtocol = 1;
constexpr uint16_t kMtnPropPorts = 2;
constexpr uint16_t kMtnPropMaxSourceConnectors = 3;
constexpr uint16_t kMtnPropMaxSinkConnectors = 4;
constexpr uint16_t kMtnPropMaxPinsPerConnector = 5;
constexpr uint16_t kMtnPropMaxPortsPerPin = 6;

// MediaClock3 Property indices
constexpr uint16_t kMc3PropAvailability = 1;
constexpr uint16_t kMc3PropTimeSourceONo = 2;
constexpr uint16_t kMc3PropOffset = 3;
constexpr uint16_t kMc3PropCurrentRate = 4;

// MediaClock Property indices (废弃)
constexpr uint16_t kMcPropType = 1;
constexpr uint16_t kMcPropDomainID = 2;
constexpr uint16_t kMcPropRatesSupported = 3;
constexpr uint16_t kMcPropCurrentRate = 4;
constexpr uint16_t kMcPropLockState = 5;

// MediaClockManager Property indices
constexpr uint16_t kMcmPropClockSourceTypesSupported = 1;
constexpr uint16_t kMcmPropClocks = 2;
constexpr uint16_t kMcmPropClock3s = 3;
```

## OCP.1 编码细节

### OcaMediaClockRate

AES70 定义：OcaMediaClockRate = 结构体 { OcaUint32 numerator, OcaUint32 denominator }

```
编码: u32 numerator + u32 denominator
例: 48000Hz = {48000, 1} → u32(48000) + u32(1)
```

### OcaMediaClockLockState

```
枚举: UNDEFINED=0, LOCKED=1, SYNCHRONIZING=2, FREERUN=3, STOPPED=4
编码: u8
```

映射：daemon PTP "locked" → LOCKED(1), "locking" → SYNCHRONIZING(2), "unlocked" → FREERUN(3)

### OcaMediaClockType

```
枚举: NONE=0, INTERNAL=1, NETWORK=2, EXTERNAL=3
编码: u8
```

AES67 daemon 使用 PTP 网络时钟 → NETWORK(2)

### OcaMediaClockAvailability

```
枚举: UNAVAILABLE=0, AVAILABLE=1
编码: u8
```

### OcaPort

```
OcaPort = 结构体 { OcaPortID id(u16), u8 mode(INPUT=0/OUTPUT=1), OcaPortName string }
编码: u16 id + u8 mode + string name
```

GetPorts 返回 `Ocp1List<OcaPort>` = u16 count + [OcaPort]*

### OcaMediaSourceConnector

```
OcaMediaSourceConnector:
  u32  connectorID
  u32  ownerNetwork (ONo)
  blob idAdvertised (u16 len + data)
  u8   sourceOrSink (0=Source, 1=Sink)
  OcaMediaCoding:
    u8  codingFormatID
    u8  ancillaryFormatID
    u32 mediaSubType
    u8  ancillarySubTypeID
  u16  pinCount
  for each pin:
    OcaMap<u16, u16>: u16 count + [u16 key, u16 value]*
  OcaMediaConnection (variant)
```

OcaMediaCoding 编码参考：
- codingFormatID: 1=PCM, 2=DSD
- PCM (codingFormatID=1): mediaSubType = 位深度 (16/24/32)
- DSD (codingFormatID=2): mediaSubType = DSD 速率 (64/128)

OcaMediaConnection（AES67 专用）：
- u8 streamConnectorType (1=AES67)
- AES67 stream parameters:
  - u32 sessionID
  - u32 sessionVersion
  - string originAddress (IP)
  - u16 originPort
  - string destinationAddress (IP)
  - u16 destinationPort
  - u8 ttl
  - u8 payloadType
  - u32 ptime (samples per packet)

### OcaMediaConnectorStatus

```
OcaMediaConnectorStatus:
  u32 connectorID
  u8  status (NOTAVAILABLE=0, IDLE=1, CONNECTED=2, PAUSED=3)
```

Source connector status 映射：
- enabled → CONNECTED(2)
- disabled → IDLE(1)

Sink connector status 映射：
- receiving → CONNECTED(2)
- not receiving, enabled → IDLE(1)
- not enabled → NOTAVAILABLE(0)

### SourceConnectorChanged / SinkConnectorChanged 事件数据

```
OcaMediaConnectorChangedEventData:
  u8  changeType (ITEM_ADDED=0, ITEM_DELETED=1, ITEM_CHANGED=2)
  OcaMediaSourceConnector 或 OcaMediaSinkConnector (完整数据)
```

## Connector CRUD 与 bridge 的交互

### AddSourceConnector 流程

```
控制器: AddSourceConnector(connectorID, idAdvertised, coding, pinMap, connection)
  ↓
OcaMediaTransportNetworkAES67::handle_add_source_connector()
  1. 构造 OcaAudioBridge::SourceInfo:
     - id = 分配下一个可用 source id (0..63)
     - name = "Source {id}"
     - enabled = true
     - codec = 从 OcaMediaCoding 推导 ("L16"/"L24"/"AM824")
     - map = 从 pinMap 推导
     - address, ttl, payload_type, dscp 从 connection AES67 参数推导
     - max_samples_per_packet = 从 ptime 推导
  2. bridge.add_source(source_info)
  3. 存 connector 到内部列表
  4. trigger_event(kEventSourceConnectorChanged, ITEM_ADDED + connector 数据)
  5. 返回 OK
```

### DeleteConnector 流程

```
控制器: DeleteConnector(connectorID)
  ↓
OcaMediaTransportNetworkAES67::handle_delete_connector()
  1. 查找 connector → 确定 sourceOrSink + 对应 id
  2. if source: bridge.remove_source(id)
     if sink: bridge.remove_sink(id)
  3. 从内部列表移除
  4. trigger_event(kEventSourceConnectorChanged/SinkConnectorChanged, ITEM_DELETED)
  5. 返回 OK
```

### SetConnectorConnection 流程

```
控制器: SetConnectorConnection(connectorID, connection)
  ↓
OcaMediaTransportNetworkAES67::handle_set_connector_connection()
  1. 查找 connector
  2. if sink:
     - 从 connection AES67 参数构造 SinkInfo
     - bridge.add_sink(sink_info)  // add_sink 对已有 id 做覆写
  3. if source:
     - 从 connection AES67 参数更新 SourceInfo
     - bridge.add_source(source_info)  // 覆写
  4. 更新内部 connector 数据
  5. trigger_event(ConnectorChanged, ITEM_CHANGED)
```

## 文件清单（新增）

| 文件 | 说明 |
|------|------|
| `daemon/oca/oca_audio_bridge.hpp` | OcaAudioBridge 纯虚接口 |
| `daemon/oca/oca_session_manager_bridge.hpp` | Bridge 实现声明 |
| `daemon/oca/oca_session_manager_bridge.cpp` | Bridge 实现（持有 SessionManager/Config） |
| `daemon/oca/classes/media_clock_manager.hpp` / `.cpp` | OcaMediaClockManager {1,3,7} |
| `daemon/oca/classes/media_clock3.hpp` / `.cpp` | OcaMediaClock3 {1,2,15} |
| `daemon/oca/classes/media_clock.hpp` / `.cpp` | OcaMediaClock {1,2,6} 废弃存根 |
| `daemon/oca/classes/media_transport_network.hpp` / `.cpp` | OcaMediaTransportNetwork {1,4,2} 基类 |
| `daemon/oca/classes/media_transport_network_aes67.hpp` / `.cpp` | OcaMediaTransportNetworkAES67 子类 |

## 文件清单（修改）

| 文件 | 修改内容 |
|------|---------|
| `daemon/oca/oca_server.hpp` | 新增 `OcaAudioBridge*` 成员；OcaServerConfig 扩展 mDNS 字段 |
| `daemon/oca/oca_server.cpp` | 注册新对象；注入 bridge 到需要它的对象；扩展 mDNS TXT |
| `daemon/oca/methods.hpp` | 新增媒体类常量（见上文） |
| `daemon/oca/classes/network.hpp` / `.cpp` | GetSystemInterfaces/GetMediaProtocol/GetIDAdvertised 从 bridge 读 |
| `daemon/oca/classes/network_manager.hpp` / `.cpp` | 四个 Get* 方法返回真实列表 |
| `daemon/oca/mdns_publisher.hpp` / `.cpp` | 构造签名扩展 TXT 字段 |
| `daemon/main.cpp` | 构造 OcaSessionManagerBridge 传入 OcaServer；扩展 OcaServerConfig |
| `daemon/CMakeLists.txt` | 新增源文件 |
| `daemon/oca/tools/oca_probe.cpp` | 新增媒体探测段 |
| `daemon/tests/oca_test.cpp` | 新增媒体类测试用例 |

## 风险与缓解

| 风险 | 说明 | 缓解 |
|------|------|------|
| bridge 回调线程安全 | SessionManager worker 线程调 observer → OCA 对象 trigger_event | trigger_event 已是线程安全的（Session 队列有锁）；emit_property_changed 需确保 Writer 无竞争 |
| Connector 数据一致性 | OCA 对象内部 connector 列表与 SessionManager 实际流状态可能不一致 | 以 bridge 为唯一真实源；connector 列表仅作缓存/序列化用；Get* 方法每次从 bridge 读取 |
| AES67 编码复杂度 | OcaMediaConnection/OcaMediaCoding/OcaMediaSourceConnector 编码规则多 | 参考 OCAMicro OcaLiteMediaSourceConnector 的序列化实现，逐字段对照 |
| 控制器兼容性 | Fitcan 控制器的私有方法/事件格式未知细节 | 先实现标准方法，私有方法以 OCAMicro 为参照；实际联调时微调 |
| OcaMediaClock NOT_IMPLEMENTED 合规 | 合规工具对废弃类仍检查方法存在性 | 方法存在（switch case）但返回 NotImplemented——合规工具对 optional 方法接受此返回 |

## 参考资源

- AES70-2023 规范文本（AES70-2 OCC 数据类型、AES70-3 OCP.1 传输）
- OCAMicro 参考实现（C++）：defLevel/methodIndex/PropertyIndex 来源
- OCAMicro 合规工具 ReferenceOCCMembers.xml：必选/可选方法判定
- Fitcan AES70Controller：`/home/Share/Project/FCPro/MFitcanManager/vsxprojs/`
- 设计文档：`docs/superpowers/specs/aes70-oca-spec1-design.md` ~ `spec4`
- 维护手册：`docs/ops/oca-design-and-maintenance.md`
- Fork 维护规范：`.claude/rules/fork-maintenance.md`

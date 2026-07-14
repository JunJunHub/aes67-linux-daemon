# AES70/OCA Spec5 — 媒体桥接：OCA 对象映射 AES67 音频运行时 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 daemon 从"协议合规自包含门面"升级为"可通过 AES70 控制器真实操控音频流"的设备——OcaAudioBridge 抽象接口解耦 OCA 层与 SessionManager，OcaMediaTransportNetworkAES67/ OcaMediaClock3/ OcaMediaClockManager 暴露真实音频运行时，现有类现实化，mDNS TXT 丰富，合规通过，真实控制器可发现并操控。

**Architecture:** OcaAudioBridge 纯虚接口倒推 OCA 需求，OcaSessionManagerBridge 实现(持有 SessionManager+Config，注册 Observer)。新增 4 个 OCA 类：OcaMediaClockManager{1,3,7}@7、OcaMediaClock3{1,2,15}@8193、OcaMediaClock{1,2,6}@8194(废弃存根)、OcaMediaTransportNetworkAES67{1,4,2,0xFFFF,0xFA,0x2EE9,1}@8192。OcaMediaTransportNetwork{1,4,2}为共享基类。OcaNetwork/OcaNetworkManager 现实化。mDNS TXT 扩展设备元数据。

**Tech Stack:** C++17、Boost.Test、OCP.1 over TCP、CMake(WITH_OCA 选项，隔离在 `daemon/oca/`)、oca-dev.sh(out-of-source 构建 `daemon/build/`)。

## Global Constraints

- 注释/文档/提交信息优先中文，API 名/标准术语保留英文原文。
- 提交信息末尾加 `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`。
- 精准修改：仅改本计划涉及的代码，不顺手重构相邻代码；格式由 `.clang-format`(Chromium 风格、2 空格、左指针)统一，不重排既有 include。
- 构建/测试用 `oca-dev.sh`(out-of-source `daemon/build/`)；oca-test 二进制路径 `daemon/build/tests/oca-test`，oca-probe 路径 `daemon/build/oca-probe`。
- AES70 符合性优先：PropertyID 与 methodIndex 是独立命名空间；ClassID 必须精确匹配 AES70/OCAMicro 定义。
- OcaAudioBridge 接口设计原则：从 OCA 需求倒推，不暴露 `StreamSource`/`StreamSink` 等内部类型。OCA 编译单元零依赖 SessionManager 头文件。
- Connector 数据作为 OcaMediaTransportNetworkAES67 内部状态管理，不注册独立 OcaStreamConnector 对象。
- 所有新增文件放在 `daemon/oca/` 下，CMakeLists.txt 新增源文件条目。

## 设计依据

完整设计见 `docs/superpowers/specs/aes70-oca-spec5-design.md`。关键事实：

- SessionManager 提供 source/sink/PTP 的完整 CRUD + Observer，是自然桥接点。
- Fitcan AES70Controller 通过 mDNS `_oca._tcp` 发现设备，`GetMembersRecursive(100)` 按 ClassID 搜索 `OcaLiteMediaTransportNetworkAES67` {1,4,2,0xFFFF,0xFA,0x2EE9,1} 和 `OcaLiteMediaClock3` {1,2,15}。**ClassID 不匹配则控制器完全看不到设备**。
- 合规工具(ReferenceOCCMembers.xml)对 AES70-2018 streaming 设备强制检查：OcaMediaClockManager GetClocks/GetClock3s、OcaMediaClock3 GetAvailability/GetCurrentRate/GetOffset/GetSupportedRates、OcaMediaTransportNetwork 8 个 mandatory 方法。
- OcaMediaClock {1,2,6} 已废弃但合规工具仍有检查项，所有方法返回 NOT_IMPLEMENTED 可过。
- OcaStreamConnector {1,2,11} 全部方法为 optional，控制器不直接交互，不注册独立对象。
- 当前 MdnsPublisher 仅发布 `txtvers=1` + `protovers=1`，控制器期望 TXT 含 ip_addr/mac_addr/device_id/channels/firmware。
- OcaNetwork.GetSystemInterfaces 返回空列表、OcaNetworkManager 四个 Get* 返回空列表——Spec5 需现实化。

---

## File Structure

| 文件 | 职责(本计划涉及部分) |
|------|---------------------|
| `daemon/oca/oca_audio_bridge.hpp` | 新增：OcaAudioBridge 纯虚接口 |
| `daemon/oca/oca_session_manager_bridge.hpp` | 新增：Bridge 实现声明 |
| `daemon/oca/oca_session_manager_bridge.cpp` | 新增：Bridge 实现(持有 SessionManager/Config，注册 Observer) |
| `daemon/oca/classes/media_clock_manager.hpp` / `.cpp` | 新增：OcaMediaClockManager {1,3,7}@7 |
| `daemon/oca/classes/media_clock3.hpp` / `.cpp` | 新增：OcaMediaClock3 {1,2,15}@8193 |
| `daemon/oca/classes/media_clock.hpp` / `.cpp` | 新增：OcaMediaClock {1,2,6}@8194 废弃存根 |
| `daemon/oca/classes/media_transport_network.hpp` / `.cpp` | 新增：OcaMediaTransportNetwork {1,4,2} 基类 |
| `daemon/oca/classes/media_transport_network_aes67.hpp` / `.cpp` | 新增：OcaMediaTransportNetworkAES67 子类 |
| `daemon/oca/methods.hpp` | 新增媒体类常量 |
| `daemon/oca/oca_server.hpp` | 新增 bridge 成员；OcaServerConfig 扩展 |
| `daemon/oca/oca_server.cpp` | 注册新对象；注入 bridge；emitter 注入 |
| `daemon/oca/classes/network.hpp` / `.cpp` | 现实化：GetSystemInterfaces/GetMediaProtocol/GetIDAdvertised |
| `daemon/oca/classes/network_manager.hpp` / `.cpp` | 现实化：四个 Get* 返回真实列表 |
| `daemon/oca/mdns_publisher.hpp` / `.cpp` | TXT 记录扩展 |
| `daemon/main.cpp` | 构造 OcaSessionManagerBridge 传入 OcaServer |
| `daemon/CMakeLists.txt` | 新增源文件 |
| `daemon/oca/tools/oca_probe.cpp` | 新增媒体探测段 |
| `daemon/tests/oca_test.cpp` | 新增媒体类测试用例 |
| `docs/ops/oca-design-and-maintenance.md` | 维护手册同步 Spec5 |

任务依赖链：Task 1(常量) → Task 2(Bridge 接口) → Task 3(Bridge 实现，依赖 SessionManager 头文件，但 oca/ 内不 include) → Task 4(四个新类 stub + 注册，可与 Task 3 并行但依赖 Task 1) → Task 5(MTN connector 逻辑 + bridge CRUD 接通) → Task 6(OcaMediaClock3 PTP 接通 + 自发射) → Task 7(现有类现实化 + NetworkManager) → Task 8(mDNS TXT 扩展) → Task 9(main.cpp 集成) → Task 10(oca-test 新增用例) → Task 11(oca-probe 媒体探测段) → Task 12(维护手册 + 最终验收)。

---

### Task 1: methods.hpp 新增媒体类常量

**Files:**
- Modify: `daemon/oca/methods.hpp`

**Interfaces:**
- Produces: 所有媒体类 DefLevel/MethodIndex/PropertyIndex/EventIndex 常量，供 Task 4-7 引用。

- [ ] **Step 1: 新增常量**

在 `daemon/oca/methods.hpp` 的 `kMcmPropClock3s` 之后(即文件末尾 `}  // namespace oca::methods` 之前)插入：

```cpp
// ── OcaMediaClockManager methods (DefLevel 3, classID{1,3,7}) ──
constexpr uint16_t kDefLevelMediaClockMngr = 3;
constexpr uint16_t kMcmGetClocks = 1;                    // AES70-2018 Mandatory
constexpr uint16_t kMcmGetMediaClockTypesSupported = 2;  // AES70-2018 Mandatory
constexpr uint16_t kMcmGetClock3s = 3;                   // AES70-2018 Mandatory

// OcaMediaClockManager Property indices
constexpr uint16_t kMcmPropClockSourceTypesSupported = 1;
constexpr uint16_t kMcmPropClocks = 2;
constexpr uint16_t kMcmPropClock3s = 3;

// ── OcaMediaClock3 methods (DefLevel 3, classID{1,2,15}) ──
// AvailableSince AES70-2018,替代已废弃 OcaMediaClock{1,2,6}。
constexpr uint16_t kDefLevelMediaClock3 = 3;
constexpr uint16_t kMc3GetAvailability = 1;     // AES70-2018 Mandatory
constexpr uint16_t kMc3SetAvailability = 2;     // Optional
constexpr uint16_t kMc3GetCurrentRate = 3;      // AES70-2018 Mandatory
constexpr uint16_t kMc3SetCurrentRate = 4;      // Optional
constexpr uint16_t kMc3GetOffset = 5;           // AES70-2018 Mandatory
constexpr uint16_t kMc3SetOffset = 6;           // Optional
constexpr uint16_t kMc3GetSupportedRates = 7;   // AES70-2018 Mandatory
constexpr uint16_t kMc3GetPTPStatus = 0x8002;   // Fitcan 私有

// OcaMediaClock3 Property indices
constexpr uint16_t kMc3PropAvailability = 1;
constexpr uint16_t kMc3PropTimeSourceONo = 2;
constexpr uint16_t kMc3PropOffset = 3;
constexpr uint16_t kMc3PropCurrentRate = 4;

// ── OcaMediaClock methods (DefLevel 3, classID{1,2,6}) ──
// DeprecatedSince AES70-2018;合规工具仍检查,全部 NotImplemented。
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

// OcaMediaClock Property indices (废弃)
constexpr uint16_t kMcPropType = 1;
constexpr uint16_t kMcPropDomainID = 2;
constexpr uint16_t kMcPropRatesSupported = 3;
constexpr uint16_t kMcPropCurrentRate = 4;
constexpr uint16_t kMcPropLockState = 5;

// ── OcaMediaTransportNetwork methods (DefLevel 3, classID{1,4,2}) ──
constexpr uint16_t kDefLevelMtn = 3;
constexpr uint16_t kMtnGetMediaProtocol = 1;           // AES70-2018 Mandatory
constexpr uint16_t kMtnGetPorts = 2;                   // AES70-2018 Mandatory
constexpr uint16_t kMtnGetPortName = 3;                // Optional
constexpr uint16_t kMtnSetPortName = 4;                // Optional
constexpr uint16_t kMtnGetMaxSourceConnectors = 5;     // AES70-2018 Mandatory
constexpr uint16_t kMtnGetMaxSinkConnectors = 6;       // AES70-2018 Mandatory
constexpr uint16_t kMtnGetMaxPinsPerConnector = 7;     // AES70-2018 Mandatory
constexpr uint16_t kMtnGetMaxPortsPerPin = 8;          // AES70-2018 Mandatory
constexpr uint16_t kMtnGetSourceConnectors = 9;        // Optional
constexpr uint16_t kMtnGetSourceConnector = 10;        // Optional
constexpr uint16_t kMtnGetSinkConnectors = 11;         // Optional
constexpr uint16_t kMtnGetSinkConnector = 12;          // Optional
constexpr uint16_t kMtnGetConnectorsStatuses = 13;     // AES70-2018 Mandatory
constexpr uint16_t kMtnGetConnectorStatus = 14;        // AES70-2018 Mandatory
constexpr uint16_t kMtnAddSourceConnector = 15;        // Optional
constexpr uint16_t kMtnAddSinkConnector = 16;          // Optional
constexpr uint16_t kMtnControlConnector = 17;          // Optional
constexpr uint16_t kMtnSetSourceConnectorPinMap = 18;  // Optional
constexpr uint16_t kMtnSetSinkConnectorPinMap = 19;    // Optional
constexpr uint16_t kMtnSetConnectorConnection = 20;    // Optional
constexpr uint16_t kMtnSetConnectorCoding = 21;        // Optional
constexpr uint16_t kMtnSetConnectorAlignmentLevel = 22;   // Optional
constexpr uint16_t kMtnSetConnectorAlignmentGain = 23;     // Optional
constexpr uint16_t kMtnDeleteConnector = 24;           // AES70-2018 Mandatory
constexpr uint16_t kMtnGetAlignmentLevel = 25;         // Optional
constexpr uint16_t kMtnGetAlignmentGain = 26;          // Optional

// OcaMediaTransportNetwork Property indices
constexpr uint16_t kMtnPropProtocol = 1;
constexpr uint16_t kMtnPropPorts = 2;
constexpr uint16_t kMtnPropMaxSourceConnectors = 3;
constexpr uint16_t kMtnPropMaxSinkConnectors = 4;
constexpr uint16_t kMtnPropMaxPinsPerConnector = 5;
constexpr uint16_t kMtnPropMaxPortsPerPin = 6;

// OcaMediaTransportNetwork Event indices
constexpr uint16_t kEventSourceConnectorChanged = 1;
constexpr uint16_t kEventSinkConnectorChanged = 2;
constexpr uint16_t kEventConnectorStatusChanged = 3;

// ── OcaMediaTransportNetworkAES67 methods (DefLevel 7) ──
// ClassID {1,4,2,0xFFFF,0xFA,0x2EE9,1}, defLevel = fieldCount = 7
constexpr uint16_t kDefLevelMtnAes67 = 7;
constexpr uint16_t kMtnAes67GetSendPacketTimes = 1;
constexpr uint16_t kMtnAes67GetReceivePacketTimes = 2;
constexpr uint16_t kMtnAes67GetMinReceiveBufferCapacity = 3;
constexpr uint16_t kMtnAes67GetMaxReceiveBufferCapacity = 4;
constexpr uint16_t kMtnAes67GetTransmissionTimeVariation = 5;
constexpr uint16_t kMtnAes67GetSupportedDiscoverySystems = 6;
// Fitcan 私有
constexpr uint16_t kMtnAes67DeleteAllConnectors = 1000;
constexpr uint16_t kMtnAes67UpdateRouteTableCommand = 0x8000;

// ── OcaMediaTransportNetworkAES67 媒体协议编号 ──
constexpr uint8_t kMediaProtocolAes67 = 3;  // AES67
```

- [ ] **Step 2: 构建验证常量编译通过**

Run: `./oca-dev.sh build`
Expected: 构建成功(常量未被引用，仅编译)。

- [ ] **Step 3: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/oca/methods.hpp && git commit -m "feat(oca): Spec5 - methods.hpp 新增媒体类常量

MediaClockManager/MediaClock3/MediaClock/MediaTransportNetwork/
MediaTransportNetworkAES67 的 DefLevel/MethodIndex/PropertyIndex/
EventIndex/私有方法/媒体协议编号。来源:OCAMicro + ReferenceOCCMembers.xml。

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: OcaAudioBridge 纯虚接口

**Files:**
- Create: `daemon/oca/oca_audio_bridge.hpp`

**Interfaces:**
- Produces: `oca::OcaAudioBridge` 纯虚基类，供 Task 3 实现和 Task 4-7 的 OCA 对象通过指针消费。

- [ ] **Step 1: 创建 oca_audio_bridge.hpp**

创建 `daemon/oca/oca_audio_bridge.hpp`：

```cpp
//  oca_audio_bridge.hpp - OCA 层与 daemon 音频运行时的桥接接口
//
//  OCA 编译单元零依赖 SessionManager 头文件/类型。
//  接口从 OCA 需求倒推，不暴露 StreamSource/StreamSink 等内部类型。
//  唯一实现在 oca_session_manager_bridge.cpp。

#ifndef OCA_OCA_AUDIO_BRIDGE_HPP_
#define OCA_OCA_AUDIO_BRIDGE_HPP_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace oca {

class OcaAudioBridge {
 public:
  virtual ~OcaAudioBridge() = default;

  // ── PTP 时钟 ──
  struct PtpConfig {
    uint8_t domain;
    uint8_t dscp;
  };
  enum class PtpLockState { Unlocked, Locking, Locked };
  struct PtpStatus {
    PtpLockState lock;
    std::string gmid;
    int32_t jitter;
  };

  virtual PtpConfig get_ptp_config() const = 0;
  virtual bool set_ptp_config(PtpConfig cfg) = 0;
  virtual PtpStatus get_ptp_status() const = 0;

  // ── 采样率 ──
  virtual uint32_t get_sample_rate() const = 0;
  virtual bool set_sample_rate(uint32_t hz) = 0;
  virtual std::vector<uint32_t> get_supported_sample_rates() const = 0;

  // ── Source (Talker) ──
  struct SourceInfo {
    uint8_t id;
    bool enabled;
    std::string name;
    std::string codec;  // "L16", "L24", "AM824"
    std::string address;
    uint8_t ttl;
    uint8_t payload_type;
    uint8_t dscp;
    bool refclk_ptp_traceable;
    uint32_t max_samples_per_packet;
    std::vector<uint8_t> map;  // 通道映射
  };

  virtual std::vector<SourceInfo> get_sources() const = 0;
  virtual bool add_source(const SourceInfo& s) = 0;
  virtual bool remove_source(uint8_t id) = 0;
  virtual std::string get_source_sdp(uint8_t id) const = 0;

  // ── Sink (Listener) ──
  struct SinkInfo {
    uint8_t id;
    std::string name;
    uint32_t delay;
    std::string source_url;
    std::string sdp;
    bool use_sdp;
    bool ignore_refclk_gmid;
    std::vector<uint8_t> map;
  };

  struct SinkStatus {
    bool seq_error;
    bool ssrc_error;
    bool pt_error;
    bool sac_error;
    bool receiving;
    bool muted;
  };

  virtual std::vector<SinkInfo> get_sinks() const = 0;
  virtual bool add_sink(const SinkInfo& s) = 0;
  virtual bool remove_sink(uint8_t id) = 0;
  virtual SinkStatus get_sink_status(uint8_t id) const = 0;

  // ── 网络信息 ──
  virtual std::string get_interface_name() const = 0;
  virtual std::string get_ip_addr() const = 0;
  virtual std::string get_mac_addr() const = 0;

  // ── I/O 端口(从 driver 查询) ──
  virtual uint32_t get_input_channels() const = 0;
  virtual uint32_t get_output_channels() const = 0;

  // ── Observer(daemon → OCA 事件传播) ──
  using PtpObserver = std::function<void(const PtpStatus&)>;
  using SourceObserver = std::function<void(uint8_t id, bool added)>;
  using SinkObserver = std::function<void(uint8_t id, bool added)>;

  virtual void set_ptp_observer(PtpObserver cb) = 0;
  virtual void set_source_observer(SourceObserver cb) = 0;
  virtual void set_sink_observer(SinkObserver cb) = 0;
};

}  // namespace oca

#endif  // OCA_OCA_AUDIO_BRIDGE_HPP_
```

- [ ] **Step 2: 构建验证**

Run: `./oca-dev.sh build`
Expected: 构建成功(仅头文件，无实现引用)。

- [ ] **Step 3: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/oca/oca_audio_bridge.hpp && git commit -m "feat(oca): Spec5 - OcaAudioBridge 纯虚接口

OCA 层零依赖 SessionManager。接口从 OCA 需求倒推，
不暴露 StreamSource/StreamSink 等内部类型。
包含 PTP/采样率/Source/Sink/网络信息/IO端口/Observer。

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: OcaSessionManagerBridge 实现

**Files:**
- Create: `daemon/oca/oca_session_manager_bridge.hpp`
- Create: `daemon/oca/oca_session_manager_bridge.cpp`

**Interfaces:**
- Consumes: `OcaAudioBridge`(Task 2)、`SessionManager`/`Config`(daemon 核心头文件，仅 .cpp 引用)
- Produces: `oca::OcaSessionManagerBridge`，供 Task 9(main.cpp)构造注入 OcaServer

- [ ] **Step 1: 创建 oca_session_manager_bridge.hpp**

创建 `daemon/oca/oca_session_manager_bridge.hpp`：

```cpp
//  oca_session_manager_bridge.hpp - OcaAudioBridge 的 SessionManager 实现
//
//  .hpp 仅前向声明 SessionManager/Config，零 include 传递;
//  .cpp include SessionManager/Config 完整头文件。

#ifndef OCA_OCA_SESSION_MANAGER_BRIDGE_HPP_
#define OCA_OCA_SESSION_MANAGER_BRIDGE_HPP_

#include <memory>

#include "oca/oca_audio_bridge.hpp"

// 前向声明:OCA 编译单元不依赖 daemon 核心头文件
class SessionManager;
class Config;

namespace oca {

class OcaSessionManagerBridge : public OcaAudioBridge {
 public:
  OcaSessionManagerBridge(std::shared_ptr<SessionManager> sm,
                           std::shared_ptr<Config> cfg);
  ~OcaSessionManagerBridge() override;

  // 禁止拷贝/移动(持有 observer 回调)
  OcaSessionManagerBridge(const OcaSessionManagerBridge&) = delete;
  OcaSessionManagerBridge& operator=(const OcaSessionManagerBridge&) = delete;

  // OcaAudioBridge 实现
  PtpConfig get_ptp_config() const override;
  bool set_ptp_config(PtpConfig cfg) override;
  PtpStatus get_ptp_status() const override;
  uint32_t get_sample_rate() const override;
  bool set_sample_rate(uint32_t hz) override;
  std::vector<uint32_t> get_supported_sample_rates() const override;
  std::vector<SourceInfo> get_sources() const override;
  bool add_source(const SourceInfo& s) override;
  bool remove_source(uint8_t id) override;
  std::string get_source_sdp(uint8_t id) const override;
  std::vector<SinkInfo> get_sinks() const override;
  bool add_sink(const SinkInfo& s) override;
  bool remove_sink(uint8_t id) override;
  SinkStatus get_sink_status(uint8_t id) const override;
  std::string get_interface_name() const override;
  std::string get_ip_addr() const override;
  std::string get_mac_addr() const override;
  uint32_t get_input_channels() const override;
  uint32_t get_output_channels() const override;
  void set_ptp_observer(PtpObserver cb) override;
  void set_source_observer(SourceObserver cb) override;
  void set_sink_observer(SinkObserver cb) override;

 private:
  std::shared_ptr<SessionManager> sm_;
  std::shared_ptr<Config> cfg_;
  PtpObserver ptp_cb_;
  SourceObserver source_cb_;
  SinkObserver sink_cb_;
};

}  // namespace oca

#endif  // OCA_OCA_SESSION_MANAGER_BRIDGE_HPP_
```

- [ ] **Step 2: 创建 oca_session_manager_bridge.cpp**

创建 `daemon/oca/oca_session_manager_bridge.cpp`：

此文件 include SessionManager 和 Config 完整头文件，做类型转换（`StreamSource` → `SourceInfo` 等），注册 Observer。实现细节参考设计文档 §OcaSessionManagerBridge。

关键转换：
- `PtpStatus.status` string "unlocked"/"locking"/"locked" → `PtpLockState` 枚举
- `StreamSource` → `SourceInfo` 字段映射
- `StreamSink` → `SinkInfo` 字段映射
- `SinkStreamStatus` → `SinkStatus` 字段映射
- SessionManager Observer 注册：`add_ptp_status_observer`/`add_source_observer`/`add_sink_observer` → 转发到 OCA 层 callback

> **注意**：构造函数注册 SessionManager observer 时，callback 持有 `this` 指针。析构时需确保 SessionManager 先于 Bridge 析构（由 main.cpp 中 unique_ptr 析构顺序保证：OcaServer 先析构，其内 Bridge 随之析构，SessionManager 后析构）。

- [ ] **Step 3: 构建验证**

Run: `./oca-dev.sh build`
Expected: 构建成功(需同时更新 CMakeLists.txt，见 Step 4)。

- [ ] **Step 4: CMakeLists.txt 新增源文件**

修改 `daemon/CMakeLists.txt` line 51 的 WITH_OCA 源文件列表，在末尾追加：

```cmake
  list(APPEND SOURCES oca/ocp1.cpp oca/session.cpp oca/transport.cpp
       oca/oca_server.cpp oca/classes/root.cpp oca/classes/agent.cpp
       oca/classes/application_network.cpp oca/classes/device_manager.cpp
       oca/classes/network_manager.cpp oca/classes/subscription_manager.cpp
       oca/classes/network.cpp oca/classes/control_network.cpp
       oca/oca_session_manager_bridge.cpp)
```

> 后续 Task 4 新增类时再追加。

- [ ] **Step 5: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/oca/oca_session_manager_bridge.hpp daemon/oca/oca_session_manager_bridge.cpp daemon/CMakeLists.txt && git commit -m "feat(oca): Spec5 - OcaSessionManagerBridge 实现

OcaAudioBridge 的 SessionManager 实现。类型转换
(StreamSource→SourceInfo 等)，SessionManager Observer 注册，
PTP/Source/Sink 变化事件转发到 OCA 层 callback。

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: 四个新 OCA 类 + OcaServer 注册

**Files:**
- Create: `daemon/oca/classes/media_clock_manager.hpp` / `.cpp`
- Create: `daemon/oca/classes/media_clock3.hpp` / `.cpp`
- Create: `daemon/oca/classes/media_clock.hpp` / `.cpp`
- Create: `daemon/oca/classes/media_transport_network.hpp` / `.cpp`
- Create: `daemon/oca/classes/media_transport_network_aes67.hpp` / `.cpp`
- Modify: `daemon/oca/oca_server.hpp` / `oca_server.cpp`
- Modify: `daemon/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 常量、Task 2 OcaAudioBridge、现有 OcaAgent/OcaApplicationNetwork 继承
- Produces: 4 个新 OCA 类注册到 OcaServer，可响应 OCP.1 命令（初期 bridge 指针为 nullptr，方法返回静态值）

- [ ] **Step 1: 创建 OcaMediaClockManager**

`daemon/oca/classes/media_clock_manager.hpp`：
- ClassID {1,3,7} v2, ONo=7, 父类 OcaManager
- 方法：GetClocks→空列表、GetMediaClockTypesSupported→NotImplemented、GetClock3s→[8193]

`daemon/oca/classes/media_clock_manager.cpp`：
- ClassIdentification 常量 `{{{1, 3, 7}}, 2}`
- exec 分派 defLevel==3 到 handle_media_clock_manager()
- handle 内 switch methodIndex

- [ ] **Step 2: 创建 OcaMediaClock3**

`daemon/oca/classes/media_clock3.hpp`：
- ClassID {1,2,15} v2, ONo=8193, 父类 OcaAgent
- 持有 `OcaAudioBridge* bridge_`(由 OcaServer 注入)
- 方法：GetAvailability→AVAILABLE(1)、GetCurrentRate→bridge 或 48000、SetCurrentRate→bridge、GetOffset→0、SetOffset→NI、GetSupportedRates→列表、GetPTPStatus(0x8002)→bridge
- 新增 `on_ptp_status_changed(const OcaAudioBridge::PtpStatus&)` 供 bridge observer 调用

`daemon/oca/classes/media_clock3.cpp`：
- ClassIdentification 常量 `{{{1, 2, 15}}, 2}`
- exec 分派 defLevel==3 到 handle_media_clock3()
- bridge_ 为 nullptr 时返回静态默认值

- [ ] **Step 3: 创建 OcaMediaClock (废弃存根)**

`daemon/oca/classes/media_clock.hpp`：
- ClassID {1,2,6} v2, ONo=8194, 父类 OcaAgent
- 所有方法返回 NotImplemented

`daemon/oca/classes/media_clock.cpp`：
- ClassIdentification 常量 `{{{1, 2, 6}}, 2}`
- exec 分派 defLevel==3，switch 全 default→NotImplemented

- [ ] **Step 4: 创建 OcaMediaTransportNetwork 基类**

`daemon/oca/classes/media_transport_network.hpp`：
- ClassID {1,4,2} v1, 父类 OcaApplicationNetwork
- 持有 `OcaAudioBridge* bridge_`
- 内部 connector 存储结构：`struct SourceConnector`/`struct SinkConnector`（connectorID + pin map + coding + connection data）
- 纯虚方法：`GetMediaProtocol`/`GetMaxSourceConnectors`/`GetMaxSinkConnectors`/`GetMaxPinsPerConnector`/`GetMaxPortsPerPin`/`AddSourceConnectorImpl`/`AddSinkConnectorImpl`/`DeleteConnectorImpl`
- 实现方法：GetPorts/GetSourceConnectors/GetSinkConnectors/GetConnectorsStatuses/GetConnectorStatus/DeleteConnector(调 impl)/SetSourceConnectorPinMap/SetSinkConnectorPinMap

`daemon/oca/classes/media_transport_network.cpp`：
- ClassIdentification 常量 `{{{1, 4, 2}}, 1}`
- 26 个方法的分派与实现
- connector 列表操作与序列化

- [ ] **Step 5: 创建 OcaMediaTransportNetworkAES67 子类**

`daemon/oca/classes/media_transport_network_aes67.hpp`：
- ClassID {1,4,2,0xFFFF,0xFA,0x2EE9,1}, defLevel=7, ONo=8192
- 父类 OcaMediaTransportNetwork
- 覆盖纯虚方法：GetMediaProtocol→AES67(3)、MaxConnectors→64、AddSourceConnectorImpl→bridge.add_source()等
- AES67 defLevel=7 方法：全部 NOT_IMPLEMENTED
- 私有方法：DeleteAllConnectors(1000)→遍历删除、UpdateRouteTableCommand(0x8000)→NOT_IMPLEMENTED

`daemon/oca/classes/media_transport_network_aes67.cpp`：
- ClassIdentification 常量 7 字段 `{1, 4, 2, 0xFFFF, 0x00FA, 0x2EE9, 1}`
- exec 分派 defLevel==7 和 defLevel==3(基类) + 父类
- connector CRUD 实现：构造 SourceInfo/SinkInfo → bridge.add_source/add_sink → 存内部列表 → emit 事件

- [ ] **Step 6: 修改 OcaServer**

修改 `daemon/oca/oca_server.hpp`：
- 新增 `OcaAudioBridge* bridge_ = nullptr;` 成员
- 构造签名新增 `OcaAudioBridge* bridge` 参数
- OcaServerConfig 扩展：`ip_addr`/`ip_addr_sec`/`mac_addr`/`channels`

修改 `daemon/oca/oca_server.cpp`：
- 构造函数注册新对象：OcaMediaClockManager(7)、OcaMediaTransportNetworkAES67(8192, owner=100)、OcaMediaClock3(8193, owner=100)、OcaMediaClock(8194, owner=100)
- 注入 bridge 到需要它的对象：MTN_AES67、MediaClock3、Network(现实化用)
- emitter 注入循环不变(新对象自动覆盖)

- [ ] **Step 7: CMakeLists.txt 追加新源文件**

在 Task 3 已追加的 `oca_session_manager_bridge.cpp` 之后追加：

```cmake
       oca/oca_session_manager_bridge.cpp
       oca/classes/media_clock_manager.cpp oca/classes/media_clock3.cpp
       oca/classes/media_clock.cpp oca/classes/media_transport_network.cpp
       oca/classes/media_transport_network_aes67.cpp)
```

- [ ] **Step 8: 构建验证**

Run: `./oca-dev.sh build`
Expected: 构建成功。新对象 bridge_ 为 nullptr，方法返回静态默认值。

- [ ] **Step 9: 跑 oca-test 确认回归不破**

Run: `./oca-dev.sh test`
Expected: 34/34 全绿(新对象不影响既有测试，bridge_ 为 nullptr)。

- [ ] **Step 10: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/oca/classes/media_clock_manager.hpp daemon/oca/classes/media_clock_manager.cpp daemon/oca/classes/media_clock3.hpp daemon/oca/classes/media_clock3.cpp daemon/oca/classes/media_clock.hpp daemon/oca/classes/media_clock.cpp daemon/oca/classes/media_transport_network.hpp daemon/oca/classes/media_transport_network.cpp daemon/oca/classes/media_transport_network_aes67.hpp daemon/oca/classes/media_transport_network_aes67.cpp daemon/oca/oca_server.hpp daemon/oca/oca_server.cpp daemon/CMakeLists.txt && git commit -m "feat(oca): Spec5 - 新增媒体类 + OcaServer 注册

OcaMediaClockManager{1,3,7}@7、OcaMediaClock3{1,2,15}@8193、
OcaMediaClock{1,2,6}@8194(废弃存根)、OcaMediaTransportNetwork{1,4,2}
基类、OcaMediaTransportNetworkAES67{1,4,2,...}@8192。
OcaServer 注册新对象并注入 bridge(初期 nullptr,静态默认值)。
CMakeLists.txt 追加新源文件。

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: MTN Connector 逻辑 + Bridge CRUD 接通

**Files:**
- Modify: `daemon/oca/classes/media_transport_network.hpp` / `.cpp`
- Modify: `daemon/oca/classes/media_transport_network_aes67.hpp` / `.cpp`

**Interfaces:**
- Consumes: Task 2 OcaAudioBridge(bridge_ 非空时调 CRUD)
- Produces: 完整的 connector 生命周期：AddSourceConnector/AddSinkConnector/DeleteConnector/DeleteAllConnectors/SetConnectorPinMap/SetConnectorConnection/Get*Connectors/Get*Status

- [ ] **Step 1: 实现 connector 内部数据结构**

在 `media_transport_network.hpp` 定义：

```cpp
struct SourceConnector {
  uint32_t connector_id;
  std::string name;
  std::string codec;  // "L16","L24","AM824"
  std::vector<uint8_t> map;  // pin → port
  // AES67 connection params
  std::string dest_addr;
  uint16_t dest_port = 0;
  uint8_t ttl = 0;
  uint8_t payload_type = 0;
  uint32_t ptime = 0;
  uint32_t session_id = 0;
  uint32_t session_version = 0;
};

struct SinkConnector {
  uint32_t connector_id;
  std::string name;
  std::string codec;
  std::vector<uint8_t> map;
  uint32_t delay = 0;
  std::string source_url;
  std::string sdp;
  bool use_sdp = false;
  // AES67 connection params (同 Source)
  std::string dest_addr;
  uint16_t dest_port = 0;
  uint8_t ttl = 0;
  uint8_t payload_type = 0;
  uint32_t ptime = 0;
  uint32_t session_id = 0;
  uint32_t session_version = 0;
};
```

- [ ] **Step 2: 实现 OcaMediaTransportNetwork 的 connector 序列化/反序列化**

OCP.1 编码参考设计文档 §OCP.1 编码细节。关键序列化方法：

- `write_source_connector(const SourceConnector&, ocp1::Writer&)`：编码 OcaMediaSourceConnector
- `write_sink_connector(const SinkConnector&, ocp1::Writer&)`：编码 OcaMediaSinkConnector
- `write_connector_status(uint32_t id, uint8_t status, ocp1::Writer&)`：编码 OcaMediaConnectorStatus
- `read_media_coding(ocp1::Reader&)` → codec string
- `read_pin_map(ocp1::Reader&)` → vector<uint8_t>
- `read_aes67_connection(ocp1::Reader&, ...)` → AES67 流参数

- [ ] **Step 3: 实现 OcaMediaTransportNetworkAES67 的 CRUD 方法**

`AddSourceConnector`：
1. 解析请求：connectorID + idAdvertised + coding + pinMap + connection
2. 分配 source id(从 connector_id 或自动)
3. 构造 SourceInfo → bridge_->add_source()
4. 存内部 source_connectors_ 列表
5. trigger_event(kEventSourceConnectorChanged, ITEM_ADDED)
6. 返回 OK

`AddSinkConnector`：对称，bridge_->add_sink()

`DeleteConnector`：
1. 查找 connector→确定 source/sink + id
2. bridge_->remove_source(id) 或 remove_sink(id)
3. 从内部列表移除
4. trigger_event(kEventSinkConnectorChanged/Sink, ITEM_DELETED)

`DeleteAllConnectors`(1000)：遍历删除

`SetConnectorPinMap`：更新 map → bridge add_source/add_sink 覆写

`SetConnectorConnection`：更新连接参数 → bridge 覆写

`SetConnectorCoding`：更新 codec → bridge 覆写

- [ ] **Step 4: 构建验证**

Run: `./oca-dev.sh build`
Expected: 构建成功。

- [ ] **Step 5: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/oca/classes/media_transport_network.hpp daemon/oca/classes/media_transport_network.cpp daemon/oca/classes/media_transport_network_aes67.hpp daemon/oca/classes/media_transport_network_aes67.cpp && git commit -m "feat(oca): Spec5 - MTN Connector CRUD + Bridge 接通

完整 connector 生命周期:AddSourceConnector/AddSinkConnector/
DeleteConnector/DeleteAllConnectors/SetConnectorPinMap/
SetConnectorConnection/SetConnectorCoding。bridge 非空时调 CRUD。
OCP.1 编码/解码 OcaMediaSourceConnector/OcaMediaSinkConnector/
OcaMediaCoding/OcaMediaConnection(AES67)。

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: OcaMediaClock3 PTP 接通 + 自发射

**Files:**
- Modify: `daemon/oca/classes/media_clock3.hpp` / `.cpp`

**Interfaces:**
- Consumes: Task 2 OcaAudioBridge(bridge_ PTP/采样率方法)
- Produces: GetCurrentRate/SetCurrentRate 读/写真实采样率；PTP 锁态变化自发射 PropertyChanged

- [ ] **Step 1: 实现 bridge 接通的 MediaClock3 方法**

`GetCurrentRate`：bridge_->get_sample_rate() → OcaMediaClockRate{rate, 1}
`SetCurrentRate`：bridge_->set_sample_rate(rate.numerator) + emit PropertyChanged(kMc3PropCurrentRate)
`GetSupportedRates`：bridge_->get_supported_sample_rates() → Ocp1List<OcaMediaClockRate>
`GetPTPStatus`(0x8002)：bridge_->get_ptp_status() → 自定义编码{lock_state, gmid, jitter}

bridge_ 为 nullptr 时返回静态默认值。

- [ ] **Step 2: 实现 on_ptp_status_changed 自发射**

```cpp
void OcaMediaClock3::on_ptp_status_changed(const OcaAudioBridge::PtpStatus& st) {
  // 发射 CurrentRate PropertyChanged(PTP 锁态变化可能伴随采样率变化)
  if (bridge_) {
    ocp1::Writer w;
    uint32_t rate = bridge_->get_sample_rate();
    w.u32(rate);
    w.u32(1);  // denominator
    emit_property_changed(methods::kDefLevelMediaClock3,
                          methods::kMc3PropCurrentRate,
                          w.data(), static_cast<uint16_t>(w.size()));
  }
}
```

- [ ] **Step 3: 构建验证**

Run: `./oca-dev.sh build`
Expected: 构建成功。

- [ ] **Step 4: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/oca/classes/media_clock3.hpp daemon/oca/classes/media_clock3.cpp && git commit -m "feat(oca): Spec5 - OcaMediaClock3 PTP 接通 + 自发射

GetCurrentRate/SetCurrentRate 读/写真实采样率(bridge)。
GetSupportedRates 返回支持列表。GetPTPStatus 私有方法。
on_ptp_status_changed 自发射 PropertyChanged(CurrentRate)。

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: OcaNetwork 现实化 + OcaNetworkManager 真实列表

**Files:**
- Modify: `daemon/oca/classes/network.hpp` / `.cpp`
- Modify: `daemon/oca/classes/network_manager.hpp` / `.cpp`

**Interfaces:**
- Consumes: Task 2 OcaAudioBridge(网络信息方法)
- Produces: OcaNetwork 返回真实接口/IP/MAC；OcaNetworkManager 返回真实对象 ONo 列表

- [ ] **Step 1: OcaNetwork 现实化**

修改 `daemon/oca/classes/network.hpp`：新增 `OcaAudioBridge* bridge_ = nullptr;` 和 `void set_bridge(OcaAudioBridge* b)` 方法。

修改 `daemon/oca/classes/network.cpp`：
- `GetSystemInterfaces`：bridge 非空时编码真实接口信息(OcaNetworkSystemInterfaceDescriptor: IP/MAC/interface name)
- `GetMediaProtocol`：返回 AES67(3) 而非 None(0)
- `GetIDAdvertised`：bridge 非空时返回 node_id blob

- [ ] **Step 2: OcaNetworkManager 真实列表**

修改 `daemon/oca/classes/network_manager.hpp`：新增 `OcaAudioBridge* bridge_ = nullptr;` 和 `void set_bridge(OcaAudioBridge* b)` 方法。

修改 `daemon/oca/classes/network_manager.cpp`：
- `GetNetworks`：返回 [4097, 4098, 8192]
- `GetStreamNetworks`：返回 [8192]
- `GetControlNetworks`：返回 [4098]
- `GetMediaTransportNetworks`：返回 [8192]

- [ ] **Step 3: OcaServer 注入 bridge 到 Network/NetworkManager**

修改 `daemon/oca/oca_server.cpp`：在 bridge 注入处，对 Network 和 NetworkManager 也调 `set_bridge(bridge)`。

- [ ] **Step 4: 构建验证**

Run: `./oca-dev.sh build`
Expected: 构建成功。

- [ ] **Step 5: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/oca/classes/network.hpp daemon/oca/classes/network.cpp daemon/oca/classes/network_manager.hpp daemon/oca/classes/network_manager.cpp daemon/oca/oca_server.cpp && git commit -m "feat(oca): Spec5 - OcaNetwork 现实化 + OcaNetworkManager 真实列表

OcaNetwork:GetSystemInterfaces 返回真实接口/IP/MAC,
GetMediaProtocol 返回 AES67,GetIDAdvertised 返回 node_id。
OcaNetworkManager:GetNetworks/StreamNetworks/ControlNetworks/
MediaTransportNetworks 返回真实对象 ONo 列表。
OcaServer 注入 bridge 到 Network/NetworkManager。

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 8: mDNS TXT 记录扩展

**Files:**
- Modify: `daemon/oca/mdns_publisher.hpp` / `.cpp`
- Modify: `daemon/oca/oca_server.hpp` / `.oca_server.cpp`

**Interfaces:**
- Consumes: OcaServerConfig 新增字段
- Produces: `_oca._tcp` TXT 记录含 ip_addr/mac_addr/device_id/channels/firmware

- [ ] **Step 1: 扩展 MdnsPublisher 签名**

修改 `daemon/oca/mdns_publisher.hpp`：

```cpp
struct MdnsTxtRecords {
  std::string ip_addr;
  std::string ip_addr_sec;
  std::string mac_addr;
  std::string device_id;
  uint32_t channels = 0;
  std::string firmware;
};

class MdnsPublisher {
 public:
  MdnsPublisher(std::string name, uint16_t port, MdnsTxtRecords txt = {});
  // ...
 private:
  MdnsTxtRecords txt_;
  // ...
};
```

- [ ] **Step 2: 修改 create_service 编码 TXT**

修改 `daemon/oca/mdns_publisher.cpp` 的 `create_service()`：

```cpp
int r = avahi_entry_group_add_service(
    group_, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
    static_cast<AvahiPublishFlags>(0), name_.c_str(), "_oca._tcp", nullptr,
    nullptr, port_,
    "txtvers=1", "protovers=1",
    txt_.ip_addr.empty() ? nullptr : ("ip_addr=" + txt_.ip_addr).c_str(),
    txt_.ip_addr_sec.empty() ? nullptr : ("ip_addr_sec=" + txt_.ip_addr_sec).c_str(),
    txt_.mac_addr.empty() ? nullptr : ("mac_addr=" + txt_.mac_addr).c_str(),
    txt_.device_id.empty() ? nullptr : ("device_id=" + txt_.device_id).c_str(),
    txt_.channels ? ("channels=" + std::to_string(txt_.channels)).c_str() : nullptr,
    txt_.firmware.empty() ? nullptr : ("firmware=" + txt_.firmware).c_str(),
    nullptr);
```

- [ ] **Step 3: OcaServerConfig 扩展 + OcaServer 传参**

修改 `daemon/oca/oca_server.hpp`：OcaServerConfig 新增 `ip_addr`/`ip_addr_sec`/`mac_addr`/`channels` 字段。

修改 `daemon/oca/oca_server.cpp`：start() 中构造 MdnsPublisher 时传入 TXT 记录。

- [ ] **Step 4: 构建验证**

Run: `./oca-dev.sh build`
Expected: 构建成功(需 WITH_AVAHI=ON 构建)。

- [ ] **Step 5: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/oca/mdns_publisher.hpp daemon/oca/mdns_publisher.cpp daemon/oca/oca_server.hpp daemon/oca/oca_server.cpp && git commit -m "feat(oca): Spec5 - mDNS TXT 记录扩展

_oca._tcp TXT 新增 ip_addr/ip_addr_sec/mac_addr/device_id/
channels/firmware,供 Fitcan 控制器 mDNS 发现时提取设备元数据。
OcaServerConfig 扩展对应字段。

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 9: main.cpp 集成

**Files:**
- Modify: `daemon/main.cpp`

**Interfaces:**
- Consumes: Task 3 OcaSessionManagerBridge、Task 8 OcaServerConfig 扩展字段
- Produces: OcaServer 接收真实 bridge 和配置，完整媒体桥接上线

- [ ] **Step 1: 构造 OcaSessionManagerBridge 并传入 OcaServer**

修改 `daemon/main.cpp` line 209-226 的 OCA 启动段：

```cpp
      /* start OCA server */
#ifdef _USE_OCA_
      std::unique_ptr<oca::OcaServer> oca_server;
      if (config->get_oca_enabled()) {
        auto oca_bridge = std::make_shared<oca::OcaSessionManagerBridge>(
            session_manager, config);
        oca::OcaServerConfig ocacfg;
        ocacfg.port = config->get_oca_port();
        ocacfg.device_name = config->get_oca_device_name();
        ocacfg.manufacturer = config->get_oca_manufacturer();
        ocacfg.model = config->get_oca_model();
        ocacfg.serial_number = config->get_oca_serial_number();
        ocacfg.node_id = config->get_node_id();
        ocacfg.daemon_version = get_version();
        ocacfg.mdns_enabled = config->get_mdns_enabled();
        // Spec5:设备元数据(mDNS TXT + OcaNetwork 现实化)
        ocacfg.ip_addr = config->get_ip_addr();
        ocacfg.mac_addr = config->get_mac_addr();
        ocacfg.channels = /* 从 driver 查询或 config 默认 */;
        oca_server = std::make_unique<oca::OcaServer>(ocacfg, oca_bridge.get());
        if (!oca_server->start()) {
          throw std::runtime_error(std::string("OcaServer:: start failed"));
        }
        BOOST_LOG_TRIVIAL(info)
            << "main:: OCA server listening on port " << oca_server->port();
        // bridge 生命周期:由 shared_ptr 持有,确保比 OcaServer 长
        // (oca_server 析构时 disconnect observer;bridge 析构在 oca_server 之后)
      }
#else
      (void)0;
#endif
```

> 需 `#include "oca/oca_session_manager_bridge.hpp"`。

- [ ] **Step 2: 构建验证**

Run: `./oca-dev.sh build`
Expected: 构建成功。完整媒体桥接已上线。

- [ ] **Step 3: 启动 daemon 验证 OcaServer 正常**

Run: `./oca-dev.sh run -i lo`
Expected: OCA server 启动成功，日志显示端口。

Run: `./oca-dev.sh stop`

- [ ] **Step 4: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/main.cpp && git commit -m "feat(oca): Spec5 - main.cpp 集成 OcaSessionManagerBridge

构造 OcaSessionManagerBridge(持有 SessionManager+Config)并注入
OcaServer。OcaServerConfig 扩展 ip_addr/mac_addr/channels。
OCA 媒体桥接完整上线。

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 10: oca-test 新增媒体类测试用例

**Files:**
- Modify: `daemon/tests/oca_test.cpp`

**Interfaces:**
- Consumes: Task 4-7 的全部 OCA 对象(经 OcaServer 或直接构造)
- Produces: 媒体类方法分派、connector CRUD、PTP 状态读写的测试覆盖

- [ ] **Step 1: 新增 dispatch_media_clock_manager 用例**

验证 OcaMediaClockManager 的 GetClocks/GetClock3s/GetMediaClockTypesSupported 方法分派。

- [ ] **Step 2: 新增 dispatch_media_clock3_methods 用例**

验证 OcaMediaClock3 的 GetAvailability/GetCurrentRate/GetSupportedRates/GetOffset。初期 bridge 为 nullptr，验证静态默认值返回正确。

- [ ] **Step 3: 新增 dispatch_media_clock_deprecated 用例**

验证 OcaMediaClock 废弃存根所有方法返回 NotImplemented。

- [ ] **Step 4: 新增 dispatch_mtn_readonly 用例**

验证 OcaMediaTransportNetworkAES67 的只读方法：GetMediaProtocol/GetMaxSourceConnectors/GetMaxSinkConnectors/GetMaxPinsPerConnector/GetMaxPortsPerPin/GetPorts。

- [ ] **Step 5: 新增 oca_e2e_media_clock3 端到端用例**

经真实 socket 验证 GetMembersRecursive 包含新增对象、GetClock3s 返回正确 ONo、GetCurrentRate/GetSupportedRates 通过 socket 可读。

- [ ] **Step 6: 新增 oca_e2e_mtn_connector_crud 端到端用例**

经真实 socket 验证 AddSourceConnector/AddSinkConnector/DeleteConnector/GetSourceConnectors/GetSinkConnectors 的端到端闭环（bridge 需非空，即经 OcaServer 构造）。

> 注：此用例依赖 Task 9 的 main.cpp 集成或测试内手动构造 OcaSessionManagerBridge(需 SessionManager)。若测试环境为 FAKE_DRIVER，SessionManager 可用 FakeDriverManager。

- [ ] **Step 7: 构建验证 + 全量测试**

Run: `./oca-dev.sh build && ./oca-dev.sh test`
Expected: 构建成功 + oca-test 全绿(34 + N 新增)。

- [ ] **Step 8: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/tests/oca_test.cpp && git commit -m "test(oca): Spec5 - 新增媒体类测试用例

MediaClockManager/MediaClock3/MediaClock(废弃)/MTN_AES67
只读方法分派验证。端到端:MediaClock3 采样率/PTP、
MTN connector CRUD 闭环。

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 11: oca-probe 媒体探测段

**Files:**
- Modify: `daemon/oca/tools/oca_probe.cpp`

**Interfaces:**
- Consumes: Task 4-7 的 OCA 对象(经 socket)、`--no-media` 参数
- Produces: oca-probe 新增"媒体时钟 + 连接器"探测段

- [ ] **Step 1: main 新增 do_media 参数与 --no-media 解析**

- [ ] **Step 2: 新增媒体时钟探测段**

探测内容：
- GetClock3s → 验证返回 [8193]
- GetCurrentRate(8193) → 读当前采样率
- GetSupportedRates(8193) → 读支持列表
- GetAvailability(8193) → AVAILABLE
- GetOffset(8193) → 0

- [ ] **Step 3: 新增 MTN 探测段**

探测内容：
- GetMembersRecursive(100) → 验证包含 MTN_AES67 ONo
- GetMediaProtocol(MTN) → AES67
- GetMaxSourceConnectors → 64
- GetMaxSinkConnectors → 64
- GetPorts → 端口列表
- GetConnectorsStatuses → 状态列表

- [ ] **Step 4: 构建 + 实际运行验证**

Run: `./oca-dev.sh build && ./oca-dev.sh run -i lo && ./oca-dev.sh probe && ./oca-dev.sh stop`
Expected: probe 新增段输出 `[OK]`，汇总"全部探测通过"。

- [ ] **Step 5: 提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add daemon/oca/tools/oca_probe.cpp && git commit -m "feat(oca): Spec5 - oca-probe 新增媒体时钟 + MTN 探测段

媒体时钟:GetClock3s/GetCurrentRate/GetSupportedRates/
GetAvailability/GetOffset。MTN:GetMediaProtocol/
GetMaxConnectors/GetPorts/GetConnectorsStatuses。
--no-media 控制是否跑该段。

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 12: 维护手册同步 Spec5 + 最终验收

**Files:**
- Modify: `docs/ops/oca-design-and-maintenance.md`

**Interfaces:**
- Consumes: Task 1-11 的全部成果
- Produces: 维护手册反映 Spec5 完成状态

- [ ] **Step 1: 更新维护手册**

修改 `docs/ops/oca-design-and-maintenance.md`：

1. **L2 架构图**：CLS 节点从"9 个对象类"改为"13 个对象类"，新增 MediaClockManager/MediaClock3/MediaClock(废弃)/MediaTransportNetwork/MediaTransportNetworkAES67。

2. **OcaAudioBridge 节**：新增章节，描述 Bridge 接口设计、层次关系、OcaSessionManagerBridge 实现。

3. **新增类方法表**：OcaMediaClockManager/OcaMediaClock3/OcaMediaClock(废弃)/OcaMediaTransportNetwork/OcaMediaTransportNetworkAES67 各方法表，含必选/可选标注。

4. **Connector 模型映射节**：描述 AES70 connector → daemon Source/Sink 的映射方式。

5. **OcaNetwork/OcaNetworkManager 更新**：方法表标注"Spec5(现实化)"，描述 GetSystemInterfaces/GetNetworks 的真实数据来源。

6. **mDNS TXT 记录更新**：描述新增 TXT 字段和 Fitcan 控制器期望格式。

7. **事件传播路径节**：新增 PTP 锁态自发射路径（SessionManager observer → bridge → OcaMediaClock3 emit PropertyChanged）和 connector 变化事件路径。

8. **测试节**：用例数更新(34+N)，新增 Spec5 测试行。

9. **Spec 阶段表**：新增 Spec5 行。

10. **已知限制更新**：移除"媒体类(OcaAudioSource/Sink/MediaClock)"条目(已实现)。新增"SinkStatus 主动推送未实现"条目。

11. **验证基线**：更新测试数。

- [ ] **Step 2: 最终全量验收**

Run: `./oca-dev.sh build && ./oca-dev.sh test`
Expected: 构建成功 + oca-test 全绿。

Run: `./oca-dev.sh run -i lo && ./oca-dev.sh probe && ./oca-dev.sh stop`
Expected: probe 全部探测通过，含 Spec5 媒体段。

- [ ] **Step 3: 合规工具验证(如有条件)**

Run Aes70CompliancyTestTool 连接 daemon
Expected: 新增媒体类检查项通过。

- [ ] **Step 4: 填充 commit 范围并提交**

```bash
cd /home/Share/GitHub/aes67-linux-daemon && git add docs/ops/oca-design-and-maintenance.md && git commit -m "docs(oca): 维护手册同步 Spec5 — 媒体桥接

- OcaAudioBridge 接口设计与层次关系
- 新增 5 个类方法表(MediaClockManager/Clock3/Clock/MTN/MTN_AES67)
- Connector 模型映射、PTP 自发射路径、mDNS TXT 扩展
- OcaNetwork/OcaNetworkManager 现实化
- 测试用例数更新、Spec5 阶段行

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## 验收清单(全计划完成后)

- [ ] `./oca-dev.sh build` 成功
- [ ] `./oca-dev.sh test` → oca-test 全绿(34 + N 新增)
- [ ] `./oca-dev.sh probe` → 含 Spec5 媒体段全部 `[OK]`，汇总"全部探测通过"
- [ ] `./oca-dev.sh probe --no-media` → 跳过媒体段，其余通过
- [ ] Aes70CompliancyTestTool AES70-2018 通过(含媒体类检查)
- [ ] Fitcan AES70Controller mDNS 发现设备、GetMembersRecursive 枚举、GetPorts/GetSourceConnectors 可读
- [ ] 维护手册 `docs/ops/oca-design-and-maintenance.md` 已同步 Spec5
- [ ] Spec1~4 既有断言全不破(回归)
- [ ] 提交历史清晰，每个 Task 一个提交，提交信息含 Spec5 前缀与 Co-Authored-By

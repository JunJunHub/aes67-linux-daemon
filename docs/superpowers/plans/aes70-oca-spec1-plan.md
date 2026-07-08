# AES67 daemon AES70/OCA Spec1 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 daemon 增加一个可选的 AES70/OCA 控制面(`WITH_OCA`),使任意 AES70 控制器能经 mDNS 发现 daemon、TCP 连接、浏览对象树、查询设备身份、订阅事件。

**Architecture:** 自底向上四层 C++ 栈:`L0 OCC 类型`(纯头) → `L1 OCP.1 流式编解码 + PDU 分帧` → `L2 对象模型(Object 抽象基类 + Registry + 4 个管理器 + Session)` → `L3 TCP 传输 + Avahi mDNS + OcaServer 门面`。Spec1 的 `OcaServer` 只依赖 `Config`,不依赖 `SessionManager`(媒体桥接推迟到 Spec2),因此在 `FAKE_DRIVER` 下完全可构建可测试。采用 TDD:每层先写 Boost.Test 再写实现。

**Tech Stack:** C++17、BSD socket、Avahi client API、Boost.Test、CMake(`WITH_OCA` 可选模块)。无新外部依赖。

## 规格来源与勘误

本计划基于 `docs/superpowers/specs/aes70-oca-spec1-design.md`,并对照 `docs/AES70-2023/` 官方规范与 `/home/Share/GitHub/ocac`(C99 参考实现,其方法索引派生自 AES70 XMI)做了如下**具体化/勘误**(实施时以本计划为准):

1. **OcaStatus 枚举**:规格 §B 的枚举名是简化版。采用 AES70-2A 规范值(经 ocac 核对):`OK=0, ProtocolVersionError=1, DeviceError=2, Locked=3, BadFormat=4, BadONo=5, ParameterError=6, ParameterOutOfRange=7, NotImplemented=8, InvalidRequest=9, ProcessingFailed=10, BadMethod=11, PartiallySucceeded=12, Timeout=13, BufferOverflow=14`。其中 `BadONo`=对象不存在,`BadMethod`=方法不存在。
2. **OcaDeviceManager 方法索引**:规格 §C 的 switch 示例用的是 1-7 的示意编号,**非真实索引**。采用 ocac 核对的真实 XMI 索引(DefLevel=3):`GetOcaVersion=1, GetModelGUID=2, GetSerialNumber=3, GetDeviceName=4, SetDeviceName=5, GetModelDescription=6, GetState(=GetOperationalState)=13, GetManagers=19`。
3. **GetProduct/GetManufacturer → GetModelDescription**:AES70-2 B.3.2 列出的 `GetProduct/GetManufacturer` 在真实类定义中对应 `GetModelDescription`(索引 6),返回 `OcaModelDescription{Manufacturer, Name, Version}` 一次取回。Spec1 用 `GetModelDescription` 满足"查身份"验收。
4. **OcaRoot 方法索引**(DefLevel=1,ocac 核对):`GetClassIdentification=1, GetLockable=2, SetLockNoReadWrite=3, Unlock=4, GetRole=5, SetLockNoWrite=6`。Spec1 实现 1/2/5,其余返回 `NotImplemented`。
5. **OcaBlock.GetMembers**:规格 §C 写的是 `GetControlObjects`,但其真实 MethodIndex 仅存在于 AES70-2 Annex A XMI(文本源不可得)。`GetMembers`(DefLevel=3,索引 5,ocac 核对)是标准的对象树发现方法,返回成员 ONo 列表,完全满足"浏览对象树"验收。Spec1 Root Block 实现 `GetMembers(5)` 返回 `[1,2,4]`;`GetControlObjects` 列为 Spec1 收尾项(取得 XMI 后补)。
6. **Notification2 帧格式**:规格 §B 的 `Notification2` 结构漏了 `Data` 字段。采用 AES70-3-2023 §9.4.4 真实布局:`NotificationSize(u32) + OcaEvent{EmitterONo(u32), EventID{DefLevel(u16), EventIndex(u16)}} + NotificationType(u8) + Data(Ocp1List<u8> = u16 count + bytes)`。
7. **OcaSubscriptionManager EV2 索引**:ocac 仅含 EV1(AddSubscription=3m01…)。EV2(AddSubscription2 等)的真实索引需 AES70-2-2023 Annex A XMI 确认。本计划在 `methods.hpp` 中给出候选值并设 XMI 校验关卡(见 Task 2);因所有分派引用命名常量,若候选错误为单行修正,真实控制器订阅测试会捕获。

## Global Constraints

- C++17,2 空格缩进,无 tab,`PointerAlignment: Left`,80 列,遵循 `daemon/.clang-format`(advisory)。
- 所有 OCA 代码置于 `daemon/oca/` 命名空间 `oca`(编解码子命名空间 `oca::ocp1`)。
- 新代码**不依赖** `SessionManager`、`DriverManager`、`Browser`;仅 `OcaServer` 依赖 `Config`。
- 编解码一律 big-endian,逐字段 stream 读写 + 边界检查,禁止 packed struct 指针强转。
- `ProtocolVersion` 硬编码 `1`;`SyncVal` 硬编码 `0x3B`;仅实现 EV2 通知。
- 构建开关:`WITH_OCA`(默认 OFF)。`WITH_OCA && WITH_AVAHI` 才编译 mDNS 发布器;`WITH_OCA` 开但 `WITH_AVAHI` 关 → TCP 照跑、mDNS 不发。
- 与 `FAKE_DRIVER` 完全兼容;OCA 单测在 `buildfake.sh` 路径下必须全绿。
- 频繁提交:每个 Task 结束 commit 一次,中文提交信息。

## 文件结构

**新建文件(`daemon/oca/`):**

| 文件 | 层 | 职责 |
|------|----|------|
| `methods.hpp` | L0 | 命名常量:DefLevel、MethodIndex、EventIndex、PduType、ClassID/ClassVersion |
| `types.hpp` | L0 | OCC 标量别名、ClassID/MethodID/EventID/PropertyID、ClassIdentification、OcaStatus、OcaBitstring |
| `ocp1.hpp` / `ocp1.cpp` | L1 | Reader/Writer(标量/string/blob/bitstring/list)+ PDU 结构(Header/Command/Response/Notification2)+ PduReader/PduWriter |
| `object.hpp` | L2 | Object 抽象基类 + ObjectRegistry + Session 前置声明 |
| `session.hpp` / `session.cpp` | L2 | 每连接 Session:订阅表 + 写队列 + registry 反向引用 |
| `classes/root.hpp` / `classes/root.cpp` | L2 | OcaRoot/OcaWorker/OcaManager/OcaBlock 继承层次 + Root Block(ONo 100) |
| `classes/device_manager.hpp` / `classes/device_manager.cpp` | L2 | OcaDeviceManager(ONo 1)+ OcaDeviceIdentity |
| `classes/network_manager.hpp` / `classes/network_manager.cpp` | L2 | OcaNetworkManager(ONo 2) |
| `classes/subscription_manager.hpp` / `classes/subscription_manager.cpp` | L2 | OcaSubscriptionManager(ONo 4)+ EV2 订阅 + Notification2 机制 |
| `transport.hpp` / `transport.cpp` | L3 | TCP accept 线程 + 每连接读线程 + KeepAlive + 分派回写 |
| `mdns_publisher.hpp` / `mdns_publisher.cpp` | L3 | Avahi `_oca._tcp` 发布(WITH_AVAHI) |
| `oca_server.hpp` / `oca_server.cpp` | L3 | 门面:持有 registry+transport(+mdns),从 Config 构造对象树 |
| `tests/oca_test.cpp` | — | Boost.Test:L0/L1/L2/L3 单测与集成测 |

**修改文件:**

| 文件 | 改动 |
|------|------|
| `daemon/CMakeLists.txt` | 加 `option(WITH_OCA …)`、`_USE_OCA_` 宏、OCA 源文件列表 |
| `daemon/tests/CMakeLists.txt` | 加 `oca-test` 可执行目标(编译 OCA 源 + oca_test.cpp),`if(WITH_OCA)` 守卫 |
| `daemon/config.hpp` | 加 `oca_enabled/oca_port/oca_device_name/oca_manufacturer/oca_model/oca_serial_number` 字段 + getter/setter + `==` 比较 |
| `daemon/config.cpp` | `parse` 中加默认值校验(oca_port 范围) |
| `daemon/json.cpp` | `config_to_json` / `json_to_config` 序列化/反序列化 oca_* 字段 |
| `daemon/daemon.conf` | 加 oca_* 默认值 |
| `daemon/main.cpp` | `#ifdef _USE_OCA_` 实例化 `OcaServer` 并 start/terminate |

**ONo 分配约定:** 1=DeviceManager, 2=NetworkManager, 4=SubscriptionManager, 100=RootBlock;101~999 预留 Spec2;≥1000 动态对象。

## 任务依赖与构建命令

任务按 T1→T17 顺序执行,前序任务的"Produces"是后序的"Consumes"。所有测试用以下命令构建运行:

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
cmake \
  -DCPP_HTTPLIB_DIR="../3rdparty/cpp-httplib" \
  -DRAVENNA_ALSA_LKM_DIR="../3rdparty/ravenna-alsa-lkm" \
  -DENABLE_TESTS=ON -DWITH_OCA=ON \
  -DWITH_AVAHI=OFF -DFAKE_DRIVER=ON -DWITH_STREAMER=OFF .
make oca-test
./oca-test -p                       # 跑全部 OCA 测试
./oca-test -p -t Ocp1Codec          # 只跑某 suite
```

带 mDNS 的本地验证(需 avahi-daemon 运行):把上面 `-DWITH_AVAHI=OFF` 换成 `-DWITH_AVAHI=ON`。带真实控制器的验收见 Task 17。

---

## Task 1: 构建脚手架与 `WITH_OCA` 开关

建立 `daemon/oca/` 目录、CMake `WITH_OCA` 选项、`oca-test` 空测试目标。目标:整条构建管线打通,空测试能编译并绿。

**Files:**
- Create: `daemon/oca/.gitkeep`
- Create: `daemon/oca/tests/oca_test.cpp`
- Modify: `daemon/CMakeLists.txt`(加 `WITH_OCA` 选项与源文件列表占位)
- Modify: `daemon/tests/CMakeLists.txt`(加 `oca-test` 目标)

**Interfaces:**
- Produces: `WITH_OCA` CMake 选项、`_USE_OCA_` 宏、可构建的 `oca-test` 目标;后续任务往源文件列表里追加 `.cpp`。

- [ ] **Step 1: 在 `daemon/CMakeLists.txt` 加 `WITH_OCA` 选项**

在 `daemon/CMakeLists.txt:15`(`WITH_STREAMER` option 之后)插入:

```cmake
option(WITH_OCA "AES70/OCA device control surface" OFF)
```

- [ ] **Step 2: 在 `daemon/CMakeLists.txt` 加 OCA 源文件与宏**

在 `WITH_STREAMER` 块(`daemon/CMakeLists.txt:40-44`)之后、`FAKE_DRIVER` 块之前插入:

```cmake
if(WITH_OCA)
  MESSAGE(STATUS "WITH_OCA")
  add_definitions(-D_USE_OCA_)
  include_directories(${CMAKE_CURRENT_SOURCE_DIR})  # 让 #include "oca/..." 可解析
  list(APPEND SOURCES
    oca/ocp1.cpp
    oca/session.cpp
    oca/classes/root.cpp
    oca/classes/device_manager.cpp
    oca/classes/network_manager.cpp
    oca/classes/subscription_manager.cpp
    oca/transport.cpp
    oca/oca_server.cpp)
  if(WITH_AVAHI)
    list(APPEND SOURCES oca/mdns_publisher.cpp)
  endif()
endif()
```

**include 约定:** 所有 OCA 头文件内部与测试统一用 `oca/` 前缀引用(如 `#include "oca/types.hpp"`、`#include "oca/classes/root.hpp"`),靠上面新增的 `daemon/` include 目录解析。

注:`oca_test.cpp` 不进 `aes67-daemon`,由 `tests/CMakeLists.txt` 单独编译。上述源文件此刻尚不存在,但 `WITH_OCA=OFF` 默认不编译,不报错;开启 `WITH_OCA=ON` 时后续任务会创建它们。

- [ ] **Step 3: 在 `daemon/tests/CMakeLists.txt` 加 `oca-test` 目标**

在 `daemon/tests/CMakeLists.txt` 末尾(`WITH_AVAHI` 块之后)追加:

```cmake
if(WITH_OCA)
  MESSAGE(STATUS "tests: WITH_OCA")
  add_executable(oca-test
    ${CMAKE_SOURCE_DIR}/oca/tests/oca_test.cpp
    ${CMAKE_SOURCE_DIR}/oca/ocp1.cpp
    ${CMAKE_SOURCE_DIR}/oca/session.cpp
    ${CMAKE_SOURCE_DIR}/oca/classes/root.cpp
    ${CMAKE_SOURCE_DIR}/oca/classes/device_manager.cpp
    ${CMAKE_SOURCE_DIR}/oca/classes/network_manager.cpp
    ${CMAKE_SOURCE_DIR}/oca/classes/subscription_manager.cpp
    ${CMAKE_SOURCE_DIR}/oca/transport.cpp
    ${CMAKE_SOURCE_DIR}/oca/oca_server.cpp)
  target_include_directories(oca-test PRIVATE ${CMAKE_SOURCE_DIR})
  target_link_libraries(oca-test ${Boost_LIBRARIES})
  add_test(oca-test oca-test)
  if(WITH_AVAHI)
    target_sources(oca-test PRIVATE ${CMAKE_SOURCE_DIR}/oca/mdns_publisher.cpp)
    target_include_directories(oca-test PRIVATE ${AVAHI_INCLUDE_DIRS})
    target_link_libraries(oca-test ${AVAHI_LIBRARIES})
  endif()
endif()
```

- [ ] **Step 4: 写空测试 `daemon/oca/tests/oca_test.cpp`**

```cpp
//  oca_test.cpp - AES70/OCA Spec1 unit & integration tests (Boost.Test)

#define BOOST_TEST_MODULE oca_test
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_CASE(placeholder) {
  BOOST_CHECK_EQUAL(1, 1);
}
```

并创建空文件 `daemon/oca/.gitkeep`(保证目录被 git 跟踪):

```bash
touch /home/Share/GitHub/aes67-linux-daemon/daemon/oca/.gitkeep
```

- [ ] **Step 5: 配置并构建 `oca-test`,验证通过**

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
cmake -DCPP_HTTPLIB_DIR="../3rdparty/cpp-httplib" \
  -DRAVENNA_ALSA_LKM_DIR="../3rdparty/ravenna-alsa-lkm" \
  -DENABLE_TESTS=ON -DWITH_OCA=ON \
  -DWITH_AVAHI=OFF -DFAKE_DRIVER=ON -DWITH_STREAMER=OFF .
make oca-test
./oca-test -p
```

Expected: `WITH_OCA` 与 `tests: WITH_OCA` 状态行打印;`make oca-test` 成功;`./oca-test -p` 输出 `placeholder` 通过、`No errors detected`。

- [ ] **Step 6: Commit**

```bash
cd /home/Share/GitHub/aes67-linux-daemon
git add daemon/CMakeLists.txt daemon/tests/CMakeLists.txt daemon/oca/.gitkeep daemon/oca/tests/oca_test.cpp
git commit -m "feat(oca): 搭建 WITH_OCA 构建脚手架与 oca-test 目标"
```

---

## Task 2: L0 命名常量 `methods.hpp` + OCC 类型 `types.hpp`

定义方法/事件/类标识的命名常量与 OCC 数据类型。纯头文件,零依赖。方法索引来自 ocac(派生自 AES70 XMI);EV2 订阅索引设 XMI 校验关卡。

**Files:**
- Create: `daemon/oca/methods.hpp`
- Create: `daemon/oca/types.hpp`
- Modify: `daemon/oca/tests/oca_test.cpp`(加 types 测试)

**Interfaces:**
- Produces: `oca::methods::*`(DefLevel/MethodIndex/EventIndex 常量)、`oca::Status`、`oca::ONo`、`oca::ClassID`、`oca::MethodID`、`oca::EventID`、`oca::PropertyID`、`oca::ClassIdentification`、`oca::OcaBitstring`、`oca::OcaDeviceState`。

- [ ] **Step 1: 写 `daemon/oca/methods.hpp`**

```cpp
//  methods.hpp - AES70/OCA 命名常量(DefLevel/MethodIndex/EventIndex/PduType/ClassID)
//
//  方法索引来源:ocac 仓库(派生自 AES70 XMI),用于 OcaRoot/OcaDeviceManager/OcaBlock。
//  EV2 订阅方法索引为候选值,需对照 AES70-2-2023 Annex A XMI 校验(见 Step 5)。

#ifndef OCA_METHODS_HPP_
#define OCA_METHODS_HPP_

#include <cstdint>

namespace oca::methods {

// Ocp1MessageType (AES70-3-2023 §9.1.3)
constexpr uint8_t kPduCommand    = 0;  // Ocp1Command
constexpr uint8_t kPduCommandRrq = 1;  // Ocp1CommandRrq
constexpr uint8_t kPduNtf1       = 2;  // deprecated EV1
constexpr uint8_t kPduResponse   = 3;  // Ocp1Response
constexpr uint8_t kPduKeepAlive  = 4;  // Ocp1KeepAlive
constexpr uint8_t kPduNtf2       = 5;  // Ocp1Notification2 (EV2)

// Definition levels (ClassID 深度)
constexpr uint16_t kDefLevelRoot       = 1;  // OcaRoot {1,1}
constexpr uint16_t kDefLevelManager    = 2;  // OcaManager {1,2} / OcaWorker {1,1,1}
constexpr uint16_t kDefLevelDeviceMngr = 3;  // OcaDeviceManager {1,2,1}
constexpr uint16_t kDefLevelBlock      = 3;  // OcaBlock {1,1,3}
constexpr uint16_t kDefLevelNetworkMngr = 3; // OcaNetworkManager {1,2,3}
constexpr uint16_t kDefLevelSubMngr    = 3;  // OcaSubscriptionManager {1,2,4}

// OcaRoot methods (DefLevel 1) - ocac 核对
constexpr uint16_t kRootGetClassIdentification = 1;
constexpr uint16_t kRootGetLockable            = 2;
constexpr uint16_t kRootLock                   = 3;  // SetLockNoReadWrite
constexpr uint16_t kRootUnlock                 = 4;
constexpr uint16_t kRootGetRole                = 5;
constexpr uint16_t kRootLockReadonly           = 6;  // SetLockNoWrite

// OcaDeviceManager methods (DefLevel 3) - ocac 核对
constexpr uint16_t kDevGetOcaVersion          = 1;
constexpr uint16_t kDevGetModelGUID           = 2;
constexpr uint16_t kDevGetSerialNumber        = 3;
constexpr uint16_t kDevGetDeviceName          = 4;
constexpr uint16_t kDevSetDeviceName          = 5;
constexpr uint16_t kDevGetModelDescription    = 6;
constexpr uint16_t kDevGetState               = 13;  // = GetOperationalState
constexpr uint16_t kDevGetManagers            = 19;

// OcaBlock methods (DefLevel 3) - ocac 核对
constexpr uint16_t kBlockGetMembers           = 5;

// OcaNetworkManager methods (DefLevel 3)
constexpr uint16_t kNetGetNetworks            = 1;  // 候选,需 XMI 校验

// OcaSubscriptionManager EV2 methods (DefLevel 3) - 候选值,需 XMI 校验
constexpr uint16_t kSubAddSubscription2                = 1;  // 候选
constexpr uint16_t kSubRemoveSubscription2             = 2;  // 候选
constexpr uint16_t kSubAddPropertyChangeSubscription2  = 3;  // 候选
constexpr uint16_t kSubRemovePropertyChangeSubscription2 = 4;  // 候选

// OcaRoot events (DefLevel 1)
constexpr uint16_t kEventPropertyChanged = 1;

// OcaDeviceManager events (DefLevel 3)
constexpr uint16_t kEventOperationalState = 1;  // DeviceState 变化(演示事件)

// ProtocolVersion (AES70-2023)
constexpr uint16_t kProtocolVersion = 1;
constexpr uint8_t  kSyncVal         = 0x3B;

}  // namespace oca::methods

#endif  // OCA_METHODS_HPP_
```

- [ ] **Step 2: 写 `daemon/oca/types.hpp`**

```cpp
//  types.hpp - AES70 OCC 数据类型(L0,纯头,零依赖)

#ifndef OCA_TYPES_HPP_
#define OCA_TYPES_HPP_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace oca {

// --- 基础标量 (AES70-3-2023 §8.2.3 Table 8) ---
using Boolean = uint8_t;
using Int8 = int8_t;
using Int16 = int16_t;
using Int32 = int32_t;
using Int64 = int64_t;
using Uint8 = uint8_t;
using Uint16 = uint16_t;
using Uint32 = uint32_t;
using Uint64 = uint64_t;
using Float32 = float;
using Float64 = double;

// --- OCA 标识类型 ---
using ONo = uint32_t;

struct ClassID {
  std::vector<uint16_t> levels;  // 多级,如 {1,2,1}
};

struct MethodID {
  uint16_t defLevel = 0;
  uint16_t methodIndex = 0;
};

struct EventID {
  uint16_t defLevel = 0;
  uint16_t eventIndex = 0;
};

struct PropertyID {
  uint16_t defLevel = 0;
  uint16_t propertyIndex = 0;
};

struct ClassIdentification {
  ClassID classID{};
  uint16_t classVersion = 0;
};

// --- OcaStatus (AES70-2A,经 ocac 核对) ---
enum class Status : uint8_t {
  OK = 0,
  ProtocolVersionError = 1,
  DeviceError = 2,
  Locked = 3,
  BadFormat = 4,
  BadONo = 5,
  ParameterError = 6,
  ParameterOutOfRange = 7,
  NotImplemented = 8,
  InvalidRequest = 9,
  ProcessingFailed = 10,
  BadMethod = 11,
  PartiallySucceeded = 12,
  Timeout = 13,
  BufferOverflow = 14
};

// --- OcaDeviceState (AES70-2A) ---
enum class DeviceState : uint8_t {
  Initializing = 0,
  Updating = 1,
  Operational = 2,
  Degraded = 3,
  Fault = 4
};

// --- 容器(编码格式由 ocp1 Writer 处理,C++ 侧用原生类型) ---
using OcaString = std::string;                  // 编码为 Ocp1List<Utf8CodePoint>
using OcaBlob = std::vector<uint8_t>;           // 编码为 Ocp1List<u8>

struct OcaBitstring {
  uint16_t numBits = 0;
  std::vector<uint8_t> bytes;
};

// OcaModelDescription (GetModelDescription 返回)
struct OcaModelDescription {
  std::string manufacturer;
  std::string name;
  std::string version;
};

}  // namespace oca

#endif  // OCA_TYPES_HPP_
```

- [ ] **Step 3: 在 `oca_test.cpp` 写类型测试**

把 `oca_test.cpp` 的 `placeholder` case 替换为:

```cpp
#define BOOST_TEST_MODULE oca_test
#include <boost/test/unit_test.hpp>

#include "oca/methods.hpp"
#include "oca/types.hpp"

BOOST_AUTO_TEST_CASE(types_and_constants) {
  namespace m = oca::methods;
  BOOST_CHECK_EQUAL((int)oca::Status::OK, 0);
  BOOST_CHECK_EQUAL((int)oca::Status::BadONo, 5);
  BOOST_CHECK_EQUAL((int)oca::Status::NotImplemented, 8);
  BOOST_CHECK_EQUAL((int)oca::Status::BadMethod, 11);
  BOOST_CHECK_EQUAL(m::kSyncVal, 0x3B);
  BOOST_CHECK_EQUAL(m::kProtocolVersion, 1);
  BOOST_CHECK_EQUAL(m::kPduKeepAlive, 4);
  BOOST_CHECK_EQUAL(m::kPduNtf2, 5);
  // 关键方法索引(ocac 核对)
  BOOST_CHECK_EQUAL(m::kDevGetOcaVersion, 1);
  BOOST_CHECK_EQUAL(m::kDevGetSerialNumber, 3);
  BOOST_CHECK_EQUAL(m::kDevGetModelDescription, 6);
  BOOST_CHECK_EQUAL(m::kDevGetState, 13);
  BOOST_CHECK_EQUAL(m::kDevGetManagers, 19);
  BOOST_CHECK_EQUAL(m::kBlockGetMembers, 5);
  BOOST_CHECK_EQUAL(m::kRootGetClassIdentification, 1);
  BOOST_CHECK_EQUAL(m::kRootGetRole, 5);
  // OcaBitstring 默认
  oca::OcaBitstring bs;
  BOOST_CHECK_EQUAL(bs.numBits, 0);
}
```

- [ ] **Step 4: 构建并跑测试,验证通过**

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
make oca-test && ./oca-test -p -t types_and_constants
```

Expected: `types_and_constants` 通过。

- [ ] **Step 5: XMI 校验关卡(记录,不阻塞构建)**

在 `methods.hpp` 头注释已标注候选索引。执行人需对照 AES70-2-2023 Annex A XMI(OCA Alliance 公开仓库 `OCAAlliance/ocaMicron` 的 `.xmi` 文件,搜索 `OcaSubscriptionManager` / `OcaNetworkManager` 的 `ownedOperation` 顺序)确认 `kSubAddSubscription2` 等与 `kNetGetNetworks` 的值。若与候选不符,直接改 `methods.hpp` 单行常量(所有分派引用命名常量,无需改其他文件)。校验结果在 Task 17 真实控制器订阅测试中最终验证。

- [ ] **Step 6: Commit**

```bash
git add daemon/oca/methods.hpp daemon/oca/types.hpp daemon/oca/tests/oca_test.cpp
git commit -m "feat(oca): L0 OCC 类型与方法索引命名常量"
```

---

## Task 3: L1 编解码 - Reader/Writer 标量

定义 `ocp1.hpp` 完整接口(后续 Task 4/5 复用,不改头),实现标量读写(u8..u64/i8..i64/f32/f64)+ 边界检查。big-endian,逐字段。

**Files:**
- Create: `daemon/oca/ocp1.hpp`
- Create: `daemon/oca/ocp1.cpp`
- Modify: `daemon/oca/tests/oca_test.cpp`

**Interfaces:**
- Produces: `oca::ocp1::Reader`、`oca::ocp1::Writer`(标量方法;string/blob/list/PDU 方法签名已声明,Task 4/5 实现)。
- Consumes: `oca::methods`、`oca::types`。

- [ ] **Step 1: 写 `daemon/oca/ocp1.hpp`(完整接口)**

```cpp
//  ocp1.hpp - OCP.1 流式编解码 + PDU 分帧 (AES70-3-2023 §8/§9)

#ifndef OCA_OCP1_HPP_
#define OCA_OCP1_HPP_

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "oca/methods.hpp"
#include "oca/types.hpp"

namespace oca::ocp1 {

class Reader {
 public:
  Reader(const uint8_t* data, size_t len) : p_(data), end_(data + len) {}

  uint8_t  u8();
  uint16_t u16();
  uint32_t u32();
  uint64_t u64();
  int8_t   i8();
  int16_t  i16();
  int32_t  i32();
  int64_t  i64();
  float    f32();
  double   f64();

  // Ocp1List<Utf8CodePoint>:u16 码点计数 + UTF-8 字节
  std::string string();
  // Ocp1List<u8>:u16 计数 + bytes
  std::vector<uint8_t> blob();
  OcaBitstring bitstring();
  template <typename T>
  std::vector<T> list(std::function<T(Reader&)> read_item);

  size_t remaining() const { return static_cast<size_t>(end_ - p_); }

 private:
  void check(size_t n) const;  // 越界抛 std::runtime_error
  const uint8_t* p_;
  const uint8_t* end_;
};

class Writer {
 public:
  Writer() = default;

  void u8(uint8_t v);
  void u16(uint16_t v);
  void u32(uint32_t v);
  void u64(uint64_t v);
  void i8(int8_t v);
  void i16(int16_t v);
  void i32(int32_t v);
  void i64(int64_t v);
  void f32(float v);
  void f64(double v);

  void string(const std::string& utf8);
  void blob(const uint8_t* data, size_t len);
  void blob(const std::vector<uint8_t>& data) { blob(data.data(), data.size()); }
  void bitstring(const OcaBitstring& bs);
  template <typename T>
  void list(const std::vector<T>& items,
            std::function<void(Writer&, const T&)> write_item);

  const uint8_t* data() const { return buf_.data(); }
  size_t size() const { return buf_.size(); }
  std::vector<uint8_t> take() { return std::move(buf_); }

 private:
  std::vector<uint8_t> buf_;
};

// --- PDU 帧结构 (AES70-3-2023 §9) ---
struct Header {
  uint16_t protocolVersion = methods::kProtocolVersion;
  uint32_t pduSize = 0;       // 不含 SyncVal,含 Header + data
  uint8_t  pduType = 0;       // methods::kPdu*
  uint16_t messageCount = 0;
};

struct Command {
  uint32_t commandSize = 0;
  uint32_t handle = 0;
  ONo      targetONo = 0;
  MethodID methodID{};
  const uint8_t* paramData = nullptr;
  uint8_t  paramCount = 0;    // Ocp1Parameters.ParameterCount
};

struct Response {
  uint32_t responseSize = 0;
  uint32_t handle = 0;
  Status   statusCode = Status::OK;
  const uint8_t* paramData = nullptr;
  uint8_t  paramCount = 0;
};

struct Notification2 {
  uint32_t notificationSize = 0;
  ONo      emitterONo = 0;
  EventID  eventID{};
  uint8_t  notificationType = 0;  // 0=Event, 1=Exception
  const uint8_t* data = nullptr;
  uint32_t dataCount = 0;
};

struct PduReader {
  static std::optional<Header> try_parse_header(const uint8_t* buf, size_t len);
  static std::vector<Command>  parse_commands(const uint8_t* data, size_t len,
                                              uint16_t count);
  static std::vector<Response> parse_responses(const uint8_t* data, size_t len,
                                               uint16_t count);
  static std::vector<Notification2> parse_notifications2(const uint8_t* data,
                                                         size_t len,
                                                         uint16_t count);
};

struct PduWriter {
  // cmds/rsps/ntfs 是已序列化好的消息字节(含各自 size 前缀)
  static std::vector<uint8_t> build_command_pdu(uint16_t msgCount,
                                                const uint8_t* cmds, size_t len);
  static std::vector<uint8_t> build_response_pdu(uint16_t msgCount,
                                                 const uint8_t* rsps, size_t len);
  static std::vector<uint8_t> build_notification2_pdu(uint16_t msgCount,
                                                       const uint8_t* ntfs,
                                                       size_t len);
  static std::vector<uint8_t> build_keepalive_pdu(uint16_t heartbeatTimeSec);
};

// 模板方法定义(放头文件)
template <typename T>
std::vector<T> Reader::list(std::function<T(Reader&)> read_item) {
  uint16_t count = u16();
  std::vector<T> out;
  out.reserve(count);
  for (uint16_t i = 0; i < count; ++i) out.push_back(read_item(*this));
  return out;
}

template <typename T>
void Writer::list(const std::vector<T>& items,
                  std::function<void(Writer&, const T&)> write_item) {
  u16(static_cast<uint16_t>(items.size()));
  for (const auto& it : items) write_item(*this, it);
}

}  // namespace oca::ocp1

#endif  // OCA_OCP1_HPP_
```

- [ ] **Step 2: 写 `daemon/oca/ocp1.cpp`(标量实现)**

```cpp
//  ocp1.cpp - OCP.1 编解码实现

#include "oca/ocp1.hpp"

#include <cstring>
#include <stdexcept>

namespace oca::ocp1 {

// ---------- Reader ----------
void Reader::check(size_t n) const {
  if (static_cast<size_t>(end_ - p_) < n) {
    throw std::runtime_error("ocp1::Reader: buffer underflow");
  }
}

uint8_t  Reader::u8()  { check(1); return *p_++; }
uint16_t Reader::u16() { check(2); uint16_t v = (uint16_t(p_[0]) << 8) | p_[1]; p_ += 2; return v; }
uint32_t Reader::u32() { check(4); uint32_t v = (uint32_t(p_[0]) << 24) | (uint32_t(p_[1]) << 16) | (uint32_t(p_[2]) << 8) | p_[3]; p_ += 4; return v; }
uint64_t Reader::u64() { check(8); uint64_t v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | p_[i]; p_ += 8; return v; }

int8_t  Reader::i8()  { return static_cast<int8_t>(u8()); }
int16_t Reader::i16() { return static_cast<int16_t>(u16()); }
int32_t Reader::i32() { return static_cast<int32_t>(u32()); }
int64_t Reader::i64() { return static_cast<int64_t>(u64()); }

float  Reader::f32() { uint32_t bits = u32(); float f; std::memcpy(&f, &bits, 4); return f; }
double Reader::f64() { uint64_t bits = u64(); double d; std::memcpy(&d, &bits, 8); return d; }

// string/blob/bitstring/list 在 Task 4 实现(此处暂留,Task 4 补全)

// ---------- Writer ----------
void Writer::u8(uint8_t v)  { buf_.push_back(v); }
void Writer::u16(uint16_t v) { buf_.push_back(v >> 8); buf_.push_back(v & 0xff); }
void Writer::u32(uint32_t v) { for (int i = 3; i >= 0; --i) buf_.push_back((v >> (i * 8)) & 0xff); }
void Writer::u64(uint64_t v) { for (int i = 7; i >= 0; --i) buf_.push_back((v >> (i * 8)) & 0xff); }

void Writer::i8(int8_t v)   { u8(static_cast<uint8_t>(v)); }
void Writer::i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
void Writer::i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
void Writer::i64(int64_t v) { u64(static_cast<uint64_t>(v)); }

void Writer::f32(float v)  { uint32_t bits; std::memcpy(&bits, &v, 4); u32(bits); }
void Writer::f64(double v) { uint64_t bits; std::memcpy(&bits, &v, 8); u64(bits); }

// string/blob/bitstring/list 在 Task 4 实现

// PDU 分帧在 Task 5 实现

}  // namespace oca::ocp1
```

- [ ] **Step 3: 在 `oca_test.cpp` 加标量 roundtrip 测试**

在 `types_and_constants` case 之后追加:

```cpp
#include "oca/ocp1.hpp"

BOOST_AUTO_TEST_CASE(ocp1_scalar_roundtrip) {
  oca::ocp1::Writer w;
  w.u8(0x12);
  w.u16(0x1234);
  w.u32(0x12345678);
  w.u64(0x123456789ABCDEF0ULL);
  w.i8(-1);
  w.i16(-1000);
  w.i32(-123456);
  w.i64(-9999999999LL);
  w.f32(3.14f);
  w.f64(2.718281828);

  oca::ocp1::Reader r(w.data(), w.size());
  BOOST_CHECK_EQUAL(r.u8(), 0x12);
  BOOST_CHECK_EQUAL(r.u16(), 0x1234);
  BOOST_CHECK_EQUAL(r.u32(), 0x12345678u);
  BOOST_CHECK_EQUAL(r.u64(), 0x123456789ABCDEF0ULL);
  BOOST_CHECK_EQUAL(r.i8(), -1);
  BOOST_CHECK_EQUAL(r.i16(), -1000);
  BOOST_CHECK_EQUAL(r.i32(), -123456);
  BOOST_CHECK_EQUAL(r.i64(), -9999999999LL);
  BOOST_CHECK_CLOSE(r.f32(), 3.14f, 0.0001);
  BOOST_CHECK_CLOSE(r.f64(), 2.718281828, 0.0000001);
  BOOST_CHECK_EQUAL(r.remaining(), 0u);

  // big-endian 字节序校验
  BOOST_CHECK_EQUAL(w.data()[1], 0x12);  // u16 高字节
  BOOST_CHECK_EQUAL(w.data()[2], 0x34);
}

BOOST_AUTO_TEST_CASE(ocp1_reader_bounds) {
  uint8_t buf[2] = {0x01, 0x02};
  oca::ocp1::Reader r(buf, 2);
  r.u16();  // ok
  BOOST_CHECK_THROW(r.u8(), std::runtime_error);  // 越界
}
```

- [ ] **Step 4: 构建并跑测试**

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
make oca-test && ./oca-test -p -t ocp1_scalar_roundtrip -t ocp1_reader_bounds
```

Expected: 两个 case 通过。

- [ ] **Step 5: Commit**

```bash
git add daemon/oca/ocp1.hpp daemon/oca/ocp1.cpp daemon/oca/tests/oca_test.cpp
git commit -m "feat(oca): L1 OCP.1 Reader/Writer 标量编解码"
```

---

## Task 4: L1 编解码 - string / blob / bitstring / list

实现 `OcaString`(码点计数)、`OcaBlob`、`OcaBitstring`、`list<T>` 的编解码。验证中文/emoji 码点计数。

**Files:**
- Modify: `daemon/oca/ocp1.cpp`(实现 `Reader::string/blob/bitstring` 与 `Writer::string/blob/bitstring`)
- Modify: `daemon/oca/tests/oca_test.cpp`

**Interfaces:**
- Produces:`Reader::string/blob/bitstring/list`、`Writer::string/blob/bitstring/list` 可用。

- [ ] **Step 1: 在 `ocp1.cpp` 实现 string/blob/bitstring**

把 `ocp1.cpp` 中 `// string/blob/bitstring/list 在 Task 4 实现` 两处注释替换。Reader 部分(在 `f64` 之后)插入:

```cpp
std::string Reader::string() {
  uint16_t count = u16();  // 码点数
  std::string out;
  for (uint16_t i = 0; i < count; ++i) {
    check(1);
    unsigned char c = *p_++;
    out.push_back(static_cast<char>(c));
    int extra = (c < 0x80) ? 0
              : ((c >> 5) == 0x6) ? 1    // 110xxxxx
              : ((c >> 4) == 0xE) ? 2    // 1110xxxx
              : ((c >> 3) == 0x1E) ? 3   // 11110xxx
              : 0;
    check(extra);
    for (int j = 0; j < extra; ++j) out.push_back(static_cast<char>(*p_++));
  }
  return out;
}

std::vector<uint8_t> Reader::blob() {
  uint16_t count = u16();
  check(count);
  std::vector<uint8_t> out(p_, p_ + count);
  p_ += count;
  return out;
}

OcaBitstring Reader::bitstring() {
  OcaBitstring bs;
  bs.numBits = u16();
  uint16_t nbytes = (bs.numBits + 7) / 8;
  bs.bytes = blob_like(nbytes);  // 见下,或内联:check + copy
  return bs;
}
```

`bitstring` 内部用定长字节读取,把 `bs.bytes = blob_like(nbytes);` 改为内联实现(避免新增函数):

```cpp
OcaBitstring Reader::bitstring() {
  OcaBitstring bs;
  bs.numBits = u16();
  uint16_t nbytes = static_cast<uint16_t>((bs.numBits + 7) / 8);
  check(nbytes);
  bs.bytes.assign(p_, p_ + nbytes);
  p_ += nbytes;
  return bs;
}
```

Writer 部分(在 `f64` 之后)插入:

```cpp
void Writer::string(const std::string& utf8) {
  size_t i = 0, count = 0;
  while (i < utf8.size()) {
    unsigned char c = static_cast<unsigned char>(utf8[i]);
    i += (c < 0x80) ? 1
       : ((c >> 5) == 0x6) ? 2
       : ((c >> 4) == 0xE) ? 3
       : ((c >> 3) == 0x1E) ? 4
       : 1;
    ++count;
  }
  u16(static_cast<uint16_t>(count));  // 码点计数
  buf_.insert(buf_.end(), utf8.begin(), utf8.end());
}

void Writer::blob(const uint8_t* data, size_t len) {
  u16(static_cast<uint16_t>(len));
  if (len) buf_.insert(buf_.end(), data, data + len);
}

void Writer::bitstring(const OcaBitstring& bs) {
  u16(bs.numBits);
  uint16_t nbytes = static_cast<uint16_t>((bs.numBits + 7) / 8);
  u16(nbytes);
  if (nbytes) buf_.insert(buf_.end(), bs.bytes.begin(), bs.bytes.begin() + nbytes);
}
```

- [ ] **Step 2: 在 `oca_test.cpp` 加 string/blob/bitstring/list 测试**

```cpp
BOOST_AUTO_TEST_CASE(ocp1_string_codepoints) {
  oca::ocp1::Writer w;
  w.string("AES67");       // 5 码点
  // 码点计数应为 5(=0x0005),后跟 5 字节 ASCII
  BOOST_CHECK_EQUAL(w.size(), 2u + 5u);
  BOOST_CHECK_EQUAL(w.data()[0], 0x00);
  BOOST_CHECK_EQUAL(w.data()[1], 0x05);

  oca::ocp1::Reader r(w.data(), w.size());
  BOOST_CHECK_EQUAL(r.string(), "AES67");

  // 中文/emoji 码点计数(非字节数)
  oca::ocp1::Writer w2;
  w2.string("音频");       // 2 码点,6 字节 UTF-8
  BOOST_CHECK_EQUAL(w2.size(), 2u + 6u);          // u16 计数 + 字节
  BOOST_CHECK_EQUAL(w2.data()[1], 0x02);          // 码点数=2
  oca::ocp1::Reader r2(w2.data(), w2.size());
  BOOST_CHECK_EQUAL(r2.string(), "音频");

  oca::ocp1::Writer w3;
  w3.string("😀");        // 1 码点,4 字节 UTF-8
  BOOST_CHECK_EQUAL(w3.data()[1], 0x01);          // 码点数=1
  oca::ocp1::Reader r3(w3.data(), w3.size());
  BOOST_CHECK_EQUAL(r3.string(), "😀");
}

BOOST_AUTO_TEST_CASE(ocp1_blob_and_bitstring) {
  oca::ocp1::Writer w;
  std::vector<uint8_t> b{0xDE, 0xAD, 0xBE, 0xEF};
  w.blob(b);
  // u16 count(4) + 4 bytes
  BOOST_CHECK_EQUAL(w.size(), 6u);
  oca::ocp1::Reader r(w.data(), w.size());
  BOOST_CHECK(r.blob() == b);

  oca::OcaBitstring bs;
  bs.numBits = 10;
  bs.bytes = {0xFF, 0x03};
  oca::ocp1::Writer wb;
  wb.bitstring(bs);
  // u16 numBits(10) + u16 nbytes(2) + 2 bytes
  BOOST_CHECK_EQUAL(wb.size(), 6u);
  oca::ocp1::Reader rb(wb.data(), wb.size());
  auto bs2 = rb.bitstring();
  BOOST_CHECK_EQUAL(bs2.numBits, 10u);
  BOOST_CHECK(bs2.bytes == std::vector<uint8_t>{0xFF, 0x03});
}

BOOST_AUTO_TEST_CASE(ocp1_list_roundtrip) {
  std::vector<uint32_t> in{1, 2, 4, 100};
  oca::ocp1::Writer w;
  w.list<uint32_t>(in, [](oca::ocp1::Writer& ww, const uint32_t& v) { ww.u32(v); });
  oca::ocp1::Reader r(w.data(), w.size());
  auto out = r.list<uint32_t>([](oca::ocp1::Reader& rr) { return rr.u32(); });
  BOOST_CHECK(out == in);
}
```

- [ ] **Step 3: 构建并跑测试**

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
make oca-test && ./oca-test -p -t ocp1_string_codepoints -t ocp1_blob_and_bitstring -t ocp1_list_roundtrip
```

Expected: 三个 case 通过。

- [ ] **Step 4: Commit**

```bash
git add daemon/oca/ocp1.cpp daemon/oca/tests/oca_test.cpp
git commit -m "feat(oca): L1 string/blob/bitstring/list 编解码(码点计数)"
```

---

## Task 5: L1 编解码 - PDU 分帧与消息序列化

实现 `PduReader`(header/commands/responses/notifications2 解析)与 `PduWriter`(build_*_pdu),以及单条消息序列化辅助函数 `write_command/write_response/write_notification2`(供 L2/L3 与 CI 客户端复用)。

**Files:**
- Modify: `daemon/oca/ocp1.hpp`(加三个 free 函数声明)
- Modify: `daemon/oca/ocp1.cpp`(实现 PduReader/PduWriter + 辅助函数)
- Modify: `daemon/oca/tests/oca_test.cpp`

**Interfaces:**
- Produces:`PduReader::try_parse_header/parse_commands/parse_responses/parse_notifications2`、`PduWriter::build_command_pdu/build_response_pdu/build_notification2_pdu/build_keepalive_pdu`、`write_command/write_response/write_notification2`。

- [ ] **Step 1: 在 `ocp1.hpp` 加辅助函数声明**

在 `PduWriter` 结构体定义之后、`Reader` 模板方法之前(`}  // namespace oca::ocp1` 之前)插入:

```cpp
// 单条消息序列化(不含 sync/header,写入 Writer)
void write_command(Writer& w, uint32_t handle, ONo targetONo, MethodID methodID,
                   const uint8_t* params, uint8_t paramCount);
void write_response(Writer& w, uint32_t handle, Status status,
                    const uint8_t* params, uint8_t paramCount);
void write_notification2(Writer& w, ONo emitterONo, EventID eventID,
                         uint8_t notificationType,
                         const uint8_t* data, uint16_t dataCount);
```

- [ ] **Step 2: 在 `ocp1.cpp` 实现辅助函数与 PDU 分帧**

把 `ocp1.cpp` 中 `// PDU 分帧在 Task 5 实现` 替换为:

```cpp
// ---------- 单条消息序列化 ----------
void write_command(Writer& w, uint32_t handle, ONo targetONo, MethodID methodID,
                   const uint8_t* params, uint8_t paramCount) {
  // commandSize = 4+4+4+4+1 + paramCount = 17 + paramCount
  uint32_t cmdSize = 17u + paramCount;
  w.u32(cmdSize);
  w.u32(handle);
  w.u32(targetONo);
  w.u16(methodID.defLevel);
  w.u16(methodID.methodIndex);
  w.u8(paramCount);
  for (uint8_t i = 0; i < paramCount; ++i) w.u8(params[i]);
}

void write_response(Writer& w, uint32_t handle, Status status,
                    const uint8_t* params, uint8_t paramCount) {
  // responseSize = 4+4+1+1 + paramCount = 10 + paramCount
  uint32_t rspSize = 10u + paramCount;
  w.u32(rspSize);
  w.u32(handle);
  w.u8(static_cast<uint8_t>(status));
  w.u8(paramCount);
  for (uint8_t i = 0; i < paramCount; ++i) w.u8(params[i]);
}

void write_notification2(Writer& w, ONo emitterONo, EventID eventID,
                         uint8_t notificationType,
                         const uint8_t* data, uint16_t dataCount) {
  // notificationSize = 4+4+4+1+2 + dataCount = 15 + dataCount
  uint32_t ntfSize = 15u + dataCount;
  w.u32(ntfSize);
  w.u32(emitterONo);
  w.u16(eventID.defLevel);
  w.u16(eventID.eventIndex);
  w.u8(notificationType);
  w.u16(dataCount);
  for (uint16_t i = 0; i < dataCount; ++i) w.u8(data[i]);
}

// ---------- PduReader ----------
std::optional<Header> PduReader::try_parse_header(const uint8_t* buf, size_t len) {
  if (len < 9) return std::nullopt;
  Reader r(buf, 9);
  Header h;
  h.protocolVersion = r.u16();
  h.pduSize = r.u32();
  h.pduType = r.u8();
  h.messageCount = r.u16();
  return h;
}

std::vector<Command> PduReader::parse_commands(const uint8_t* data, size_t len,
                                               uint16_t count) {
  Reader r(data, len);
  std::vector<Command> out;
  for (uint16_t i = 0; i < count; ++i) {
    Command c;
    c.commandSize = r.u32();
    c.handle = r.u32();
    c.targetONo = r.u32();
    c.methodID.defLevel = r.u16();
    c.methodID.methodIndex = r.u16();
    c.paramCount = r.u8();
    size_t consumed = len - r.remaining();
    c.paramData = data + consumed;  // 指向 buffer 内 params 起点
    for (uint8_t p = 0; p < c.paramCount; ++p) r.u8();  // 跳过 params
    out.push_back(c);
  }
  return out;
}

std::vector<Response> PduReader::parse_responses(const uint8_t* data, size_t len,
                                                 uint16_t count) {
  Reader r(data, len);
  std::vector<Response> out;
  for (uint16_t i = 0; i < count; ++i) {
    Response rsp;
    rsp.responseSize = r.u32();
    rsp.handle = r.u32();
    rsp.statusCode = static_cast<Status>(r.u8());
    rsp.paramCount = r.u8();
    size_t consumed = len - r.remaining();
    rsp.paramData = data + consumed;
    for (uint8_t p = 0; p < rsp.paramCount; ++p) r.u8();
    out.push_back(rsp);
  }
  return out;
}

std::vector<Notification2> PduReader::parse_notifications2(const uint8_t* data,
                                                           size_t len,
                                                           uint16_t count) {
  Reader r(data, len);
  std::vector<Notification2> out;
  for (uint16_t i = 0; i < count; ++i) {
    Notification2 n;
    n.notificationSize = r.u32();
    n.emitterONo = r.u32();
    n.eventID.defLevel = r.u16();
    n.eventID.eventIndex = r.u16();
    n.notificationType = r.u8();
    n.dataCount = r.u16();
    size_t consumed = len - r.remaining();
    n.data = data + consumed;
    for (uint32_t d = 0; d < n.dataCount; ++d) r.u8();
    out.push_back(n);
  }
  return out;
}

// ---------- PduWriter ----------
static void write_pdu_header(Writer& w, uint8_t pduType, uint16_t msgCount,
                             uint32_t payloadLen) {
  w.u16(methods::kProtocolVersion);
  w.u32(9u + payloadLen);  // pduSize = header(9) + payload
  w.u8(pduType);
  w.u16(msgCount);
}

std::vector<uint8_t> PduWriter::build_command_pdu(uint16_t msgCount,
                                                  const uint8_t* cmds, size_t len) {
  std::vector<uint8_t> out;
  out.push_back(methods::kSyncVal);
  Writer h;
  write_pdu_header(h, methods::kPduCommand, msgCount, static_cast<uint32_t>(len));
  out.insert(out.end(), h.data(), h.data() + h.size());
  out.insert(out.end(), cmds, cmds + len);
  return out;
}

std::vector<uint8_t> PduWriter::build_response_pdu(uint16_t msgCount,
                                                   const uint8_t* rsps, size_t len) {
  std::vector<uint8_t> out;
  out.push_back(methods::kSyncVal);
  Writer h;
  write_pdu_header(h, methods::kPduResponse, msgCount, static_cast<uint32_t>(len));
  out.insert(out.end(), h.data(), h.data() + h.size());
  out.insert(out.end(), rsps, rsps + len);
  return out;
}

std::vector<uint8_t> PduWriter::build_notification2_pdu(uint16_t msgCount,
                                                        const uint8_t* ntfs,
                                                        size_t len) {
  std::vector<uint8_t> out;
  out.push_back(methods::kSyncVal);
  Writer h;
  write_pdu_header(h, methods::kPduNtf2, msgCount, static_cast<uint32_t>(len));
  out.insert(out.end(), h.data(), h.data() + h.size());
  out.insert(out.end(), ntfs, ntfs + len);
  return out;
}

std::vector<uint8_t> PduWriter::build_keepalive_pdu(uint16_t heartbeatTimeSec) {
  std::vector<uint8_t> out;
  out.push_back(methods::kSyncVal);
  Writer h;
  write_pdu_header(h, methods::kPduKeepAlive, 1, 2u);  // payload = u16
  out.insert(out.end(), h.data(), h.data() + h.size());
  Writer payload;
  payload.u16(heartbeatTimeSec);
  out.insert(out.end(), payload.data(), payload.data() + payload.size());
  return out;
}
```

- [ ] **Step 3: 在 `oca_test.cpp` 加 PDU 往返测试**

```cpp
BOOST_AUTO_TEST_CASE(ocp1_command_pdu_roundtrip) {
  // 构造一条 command:handle=42, target=1, method {3,1} (GetOcaVersion), 无参
  oca::ocp1::Writer cw;
  oca::ocp1::write_command(cw, 42, 1, {oca::methods::kDefLevelDeviceMngr,
                                       oca::methods::kDevGetOcaVersion},
                           nullptr, 0);
  auto pdu = oca::ocp1::PduWriter::build_command_pdu(1, cw.data(), cw.size());

  // 校验 sync + header
  BOOST_CHECK_EQUAL(pdu[0], 0x3B);
  auto hdr = oca::ocp1::PduReader::try_parse_header(pdu.data() + 1, pdu.size() - 1);
  BOOST_REQUIRE(hdr);
  BOOST_CHECK_EQUAL(hdr->protocolVersion, 1);
  BOOST_CHECK_EQUAL(hdr->pduType, oca::methods::kPduCommand);
  BOOST_CHECK_EQUAL(hdr->messageCount, 1);
  BOOST_CHECK_EQUAL(hdr->pduSize, 9u + cw.size());

  // 解析 command
  const uint8_t* payload = pdu.data() + 1 + 9;
  size_t payloadLen = cw.size();
  auto cmds = oca::ocp1::PduReader::parse_commands(payload, payloadLen,
                                                   hdr->messageCount);
  BOOST_REQUIRE_EQUAL(cmds.size(), 1u);
  BOOST_CHECK_EQUAL(cmds[0].handle, 42u);
  BOOST_CHECK_EQUAL(cmds[0].targetONo, 1u);
  BOOST_CHECK_EQUAL(cmds[0].methodID.defLevel, oca::methods::kDefLevelDeviceMngr);
  BOOST_CHECK_EQUAL(cmds[0].methodID.methodIndex, oca::methods::kDevGetOcaVersion);
  BOOST_CHECK_EQUAL(cmds[0].paramCount, 0);
}

BOOST_AUTO_TEST_CASE(ocp1_response_and_notification2_roundtrip) {
  // response: handle=7, status=OK, params={0x00,0x01} (u16 OcaVersion=1)
  uint8_t params[2] = {0x00, 0x01};
  oca::ocp1::Writer rw;
  oca::ocp1::write_response(rw, 7, oca::Status::OK, params, 2);
  auto pdu = oca::ocp1::PduWriter::build_response_pdu(1, rw.data(), rw.size());
  auto hdr = oca::ocp1::PduReader::try_parse_header(pdu.data() + 1, pdu.size() - 1);
  auto rsps = oca::ocp1::PduReader::parse_responses(pdu.data() + 1 + 9,
                                                    rw.size(), hdr->messageCount);
  BOOST_REQUIRE_EQUAL(rsps.size(), 1u);
  BOOST_CHECK_EQUAL(rsps[0].handle, 7u);
  BOOST_CHECK(rsps[0].statusCode == oca::Status::OK);
  BOOST_CHECK_EQUAL(rsps[0].paramCount, 2);
  BOOST_CHECK_EQUAL(rsps[0].paramData[1], 0x01);

  // notification2: emitter=1, event {3,1}, type=0(event), data={0x00}(DeviceState Operational)
  uint8_t evdata[1] = {static_cast<uint8_t>(oca::DeviceState::Operational)};
  oca::ocp1::Writer nw;
  oca::ocp1::write_notification2(nw, 1,
                                 {oca::methods::kDefLevelDeviceMngr,
                                  oca::methods::kEventOperationalState},
                                 0, evdata, 1);
  auto npdu = oca::ocp1::PduWriter::build_notification2_pdu(1, nw.data(), nw.size());
  auto nhdr = oca::ocp1::PduReader::try_parse_header(npdu.data() + 1, npdu.size() - 1);
  BOOST_CHECK_EQUAL(nhdr->pduType, oca::methods::kPduNtf2);
  auto ntfs = oca::ocp1::PduReader::parse_notifications2(npdu.data() + 1 + 9,
                                                         nw.size(), nhdr->messageCount);
  BOOST_REQUIRE_EQUAL(ntfs.size(), 1u);
  BOOST_CHECK_EQUAL(ntfs[0].emitterONo, 1u);
  BOOST_CHECK_EQUAL(ntfs[0].eventID.eventIndex, oca::methods::kEventOperationalState);
  BOOST_CHECK_EQUAL(ntfs[0].dataCount, 1u);
  BOOST_CHECK_EQUAL(ntfs[0].data[0], static_cast<uint8_t>(oca::DeviceState::Operational));
}

BOOST_AUTO_TEST_CASE(ocp1_keepalive_pdu) {
  auto pdu = oca::ocp1::PduWriter::build_keepalive_pdu(5);
  BOOST_CHECK_EQUAL(pdu[0], 0x3B);
  auto hdr = oca::ocp1::PduReader::try_parse_header(pdu.data() + 1, pdu.size() - 1);
  BOOST_REQUIRE(hdr);
  BOOST_CHECK_EQUAL(hdr->pduType, oca::methods::kPduKeepAlive);
  BOOST_CHECK_EQUAL(hdr->messageCount, 1);
  BOOST_CHECK_EQUAL(hdr->pduSize, 11u);  // 9 header + 2 heartbeat
  // heartbeat = u16 at offset 1(sync)+9(header) = 10
  BOOST_CHECK_EQUAL(pdu[10], 0x00);
  BOOST_CHECK_EQUAL(pdu[11], 0x05);
}

BOOST_AUTO_TEST_CASE(ocp1_fuzz_no_crash) {
  // 随机字节不应让 header 解析崩溃
  uint8_t junk[9] = {0x3B, 0, 1, 0, 0, 0, 9, 0, 0};
  auto hdr = oca::ocp1::PduReader::try_parse_header(junk + 1, 8);
  BOOST_CHECK(!hdr);  // 不足 9 字节
  hdr = oca::ocp1::PduReader::try_parse_header(junk + 1, 8);
  BOOST_CHECK(!hdr);
}
```

- [ ] **Step 4: 构建并跑测试**

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
make oca-test && ./oca-test -p -t ocp1_command_pdu_roundtrip -t ocp1_response_and_notification2_roundtrip -t ocp1_keepalive_pdu -t ocp1_fuzz_no_crash
```

Expected: 四个 case 通过。

- [ ] **Step 5: Commit**

```bash
git add daemon/oca/ocp1.hpp daemon/oca/ocp1.cpp daemon/oca/tests/oca_test.cpp
git commit -m "feat(oca): L1 PDU 分帧与 command/response/notification2 序列化"
```

---

# L2 对象模型

## Task 6: Object 抽象基类 + ObjectRegistry

定义 `Object`(虚基类,`exec` 分派入口)与 `ObjectRegistry`(ONo -> Object 映射,支持区间枚举)。Session 前置声明。

**Files:**
- Create: `daemon/oca/object.hpp`
- Modify: `daemon/oca/tests/oca_test.cpp`

**Interfaces:**
- Produces:`oca::Object`(虚基类,`ono()/class_id()/class_version()/exec()`)、`oca::ObjectRegistry`(`register_object/find/objects_in_range`)、`oca::Session` 前置声明。
- Consumes:`oca::types`、`oca::ocp1`。

- [ ] **Step 1: 写 `daemon/oca/object.hpp`**

```cpp
//  object.hpp - OCA 对象抽象基类 + ObjectRegistry

#ifndef OCA_OBJECT_HPP_
#define OCA_OBJECT_HPP_

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "oca/ocp1.hpp"
#include "oca/types.hpp"

namespace oca {

class Session;  // 前置声明,定义在 session.hpp

class Object {
 public:
  virtual ~Object() = default;
  ONo ono() const { return ono_; }
  virtual const ClassIdentification& class_id() const = 0;
  virtual uint16_t class_version() const = 0;
  virtual Status exec(MethodID method, ocp1::Reader& req,
                      ocp1::Writer& rsp, Session& sess) = 0;

 protected:
  explicit Object(ONo ono) : ono_(ono) {}
  ONo ono_;
};

class ObjectRegistry {
 public:
  void register_object(std::unique_ptr<Object> obj);
  Object* find(ONo ono) const;
  // 返回 [from, to] 闭区间内按 ONo 升序的对象指针
  std::vector<Object*> objects_in_range(ONo from, ONo to) const;
  size_t size() const { return objects_.size(); }

 private:
  std::unordered_map<ONo, std::unique_ptr<Object>> objects_;
};

}  // namespace oca

#endif  // OCA_OBJECT_HPP_
```

- [ ] **Step 2: 在 `ocp1.cpp` 之外新增实现 - 把 ObjectRegistry 实现放进 `object.hpp` 内联(简单)**

`ObjectRegistry` 方法较短,作为内联放头文件末尾(`}  // namespace oca` 之前)追加:

```cpp
inline void ObjectRegistry::register_object(std::unique_ptr<Object> obj) {
  ONo ono = obj->ono();
  objects_.emplace(ono, std::move(obj));
}

inline Object* ObjectRegistry::find(ONo ono) const {
  auto it = objects_.find(ono);
  return it == objects_.end() ? nullptr : it->second.get();
}

inline std::vector<Object*> ObjectRegistry::objects_in_range(ONo from, ONo to) const {
  std::vector<Object*> out;
  for (ONo o = from; o <= to; ++o) {
    if (auto* p = find(o)) out.push_back(p);
  }
  return out;
}
```

- [ ] **Step 3: 在 `oca_test.cpp` 加 registry 测试**

需要一个小桩 Object 来测试。在测试文件 include 之后加:

```cpp
#include "oca/object.hpp"

namespace {
class StubObject : public oca::Object {
 public:
  explicit StubObject(oca::ONo ono) : oca::Object(ono) {}
  const oca::ClassIdentification& class_id() const override { return id_; }
  uint16_t class_version() const override { return 1; }
  oca::Status exec(oca::MethodID, oca::ocp1::Reader&, oca::ocp1::Writer&,
                   oca::Session&) override {
    return oca::Status::OK;
  }
 private:
  oca::ClassIdentification id_{};
};
}  // namespace

BOOST_AUTO_TEST_CASE(registry_find_and_range) {
  oca::ObjectRegistry reg;
  reg.register_object(std::make_unique<StubObject>(1));
  reg.register_object(std::make_unique<StubObject>(4));
  reg.register_object(std::make_unique<StubObject>(100));

  BOOST_CHECK(reg.find(1) != nullptr);
  BOOST_CHECK(reg.find(4) != nullptr);
  BOOST_CHECK(reg.find(2) == nullptr);  // 未注册
  BOOST_CHECK_EQUAL(reg.size(), 3u);

  auto mgrs = reg.objects_in_range(1, 99);
  BOOST_REQUIRE_EQUAL(mgrs.size(), 2u);  // 1 和 4
  BOOST_CHECK_EQUAL(mgrs[0]->ono(), 1u);
  BOOST_CHECK_EQUAL(mgrs[1]->ono(), 4u);

  auto all = reg.objects_in_range(1, 100);
  BOOST_CHECK_EQUAL(all.size(), 3u);
}
```

注:`StubObject::exec` 引用 `oca::Session`,但 `Session` 此刻仅有前置声明。`exec` 不被调用、仅取地址,前置声明足够编译。

- [ ] **Step 4: 构建并跑测试**

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
make oca-test && ./oca-test -p -t registry_find_and_range
```

Expected: 通过。若链接报 `Session` 未定义,确认 `StubObject::exec` 未被实例化调用(仅声明)。

- [ ] **Step 5: Commit**

```bash
git add daemon/oca/object.hpp daemon/oca/tests/oca_test.cpp
git commit -m "feat(oca): L2 Object 抽象基类与 ObjectRegistry"
```

---

## Task 7: Session(每 TCP 连接一个)

定义 `Session`:订阅表 + 线程安全写队列 + registry 反向引用 + KeepAlive 心跳跟踪。`Subscription2` 结构定义于此。

**Files:**
- Create: `daemon/oca/session.hpp`
- Create: `daemon/oca/session.cpp`
- Modify: `daemon/oca/tests/oca_test.cpp`

**Interfaces:**
- Produces:`oca::Session`、`oca::Subscription2`。`Session` 提供 `session_id/registry/add_subscription/remove_subscription/has_subscription/enqueue_notification/take_notification/set_heartbeat/touch/expired`。
- Consumes:`oca::ObjectRegistry`、`oca::ocp1`、`oca::types`。

- [ ] **Step 1: 写 `daemon/oca/session.hpp`**

```cpp
//  session.hpp - OCP.1 控制会话(每 TCP 连接一个)

#ifndef OCA_SESSION_HPP_
#define OCA_SESSION_HPP_

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "oca/object.hpp"
#include "oca/types.hpp"

namespace oca {

struct Subscription2 {
  ONo emitterONo = 0;
  EventID eventID{};
  OcaBlob subscriberContext;
};

class Session {
 public:
  explicit Session(ONo id) : id_(id) {}

  ONo session_id() const { return id_; }

  void set_registry(ObjectRegistry* r) { registry_ = r; }
  ObjectRegistry* registry() const { return registry_; }

  // 订阅(EV2)
  void add_subscription(const Subscription2& sub);
  void remove_subscription(ONo emitter, EventID event);
  bool has_subscription(ONo emitter, EventID event) const;
  std::vector<Subscription2> subscriptions() const;

  // 写队列(线程安全)
  void enqueue_notification(std::vector<uint8_t> pdu);
  bool take_notification(std::vector<uint8_t>& out);

  // KeepAlive / 心跳
  void set_heartbeat(uint16_t sec) { heartbeat_sec_ = sec; }
  uint16_t heartbeat() const { return heartbeat_sec_; }
  void touch(uint64_t now_sec) { last_seen_sec_ = now_sec; }
  bool expired(uint64_t now_sec) const {
    return now_sec > last_seen_sec_ &&
           (now_sec - last_seen_sec_) > 3u * heartbeat_sec_;
  }

 private:
  ONo id_;
  ObjectRegistry* registry_ = nullptr;
  mutable std::mutex mutex_;
  std::vector<Subscription2> subs_;
  std::deque<std::vector<uint8_t>> write_queue_;
  uint16_t heartbeat_sec_ = 15;
  uint64_t last_seen_sec_ = 0;
};

}  // namespace oca

#endif  // OCA_SESSION_HPP_
```

- [ ] **Step 2: 写 `daemon/oca/session.cpp`**

```cpp
//  session.cpp - Session 实现

#include "oca/session.hpp"

#include <algorithm>

namespace oca {

void Session::add_subscription(const Subscription2& sub) {
  std::lock_guard<std::mutex> lk(mutex_);
  // 去重:同一 emitter+event 只存一份
  for (const auto& s : subs_) {
    if (s.emitterONo == sub.emitterONo &&
        s.eventID.defLevel == sub.eventID.defLevel &&
        s.eventID.eventIndex == sub.eventID.eventIndex) {
      return;
    }
  }
  subs_.push_back(sub);
}

void Session::remove_subscription(ONo emitter, EventID event) {
  std::lock_guard<std::mutex> lk(mutex_);
  subs_.erase(std::remove_if(subs_.begin(), subs_.end(),
      [&](const Subscription2& s) {
        return s.emitterONo == emitter &&
               s.eventID.defLevel == event.defLevel &&
               s.eventID.eventIndex == event.eventIndex;
      }), subs_.end());
}

bool Session::has_subscription(ONo emitter, EventID event) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& s : subs_) {
    if (s.emitterONo == emitter &&
        s.eventID.defLevel == event.defLevel &&
        s.eventID.eventIndex == event.eventIndex) {
      return true;
    }
  }
  return false;
}

std::vector<Subscription2> Session::subscriptions() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return subs_;
}

void Session::enqueue_notification(std::vector<uint8_t> pdu) {
  std::lock_guard<std::mutex> lk(mutex_);
  write_queue_.push_back(std::move(pdu));
}

bool Session::take_notification(std::vector<uint8_t>& out) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (write_queue_.empty()) return false;
  out = std::move(write_queue_.front());
  write_queue_.pop_front();
  return true;
}

}  // namespace oca
```

注:已包含 `<algorithm>`(`std::remove_if`)与 `oca/session.hpp`。

- [ ] **Step 3: 在 `oca_test.cpp` 加 Session 测试**

```cpp
#include "oca/session.hpp"

BOOST_AUTO_TEST_CASE(session_subscription_and_queue) {
  oca::Session s(1);
  BOOST_CHECK_EQUAL(s.session_id(), 1u);

  oca::Subscription2 sub;
  sub.emitterONo = 1;
  sub.eventID = {oca::methods::kDefLevelDeviceMngr,
                 oca::methods::kEventOperationalState};
  s.add_subscription(sub);
  s.add_subscription(sub);  // 去重
  BOOST_CHECK_EQUAL(s.subscriptions().size(), 1u);
  BOOST_CHECK(s.has_subscription(1, sub.eventID));

  s.remove_subscription(1, sub.eventID);
  BOOST_CHECK(!s.has_subscription(1, sub.eventID));
  BOOST_CHECK_EQUAL(s.subscriptions().size(), 0u);

  // 写队列
  s.enqueue_notification({0x3B, 0x00, 0x01});
  s.enqueue_notification({0x3B, 0x00, 0x02});
  std::vector<uint8_t> out;
  BOOST_CHECK(s.take_notification(out));
  BOOST_CHECK_EQUAL(out[2], 0x01);
  BOOST_CHECK(s.take_notification(out));
  BOOST_CHECK_EQUAL(out[2], 0x02);
  BOOST_CHECK(!s.take_notification(out));
}

BOOST_AUTO_TEST_CASE(session_keepalive_expiry) {
  oca::Session s(2);
  s.set_heartbeat(5);
  s.touch(100);
  BOOST_CHECK(!s.expired(110));   // 10s < 15s
  BOOST_CHECK(!s.expired(115));   // 15s == 3*5,不超(严格大于)
  BOOST_CHECK(s.expired(116));    // 16s > 15s
}
```

- [ ] **Step 4: 构建并跑测试**

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
make oca-test && ./oca-test -p -t session_subscription_and_queue -t session_keepalive_expiry
```

Expected: 两个 case 通过。

- [ ] **Step 5: Commit**

```bash
git add daemon/oca/session.hpp daemon/oca/session.cpp daemon/oca/tests/oca_test.cpp
git commit -m "feat(oca): L2 Session 订阅表与线程安全写队列"
```

---

## Task 8: OcaRoot/OcaWorker/OcaManager/OcaBlock 继承层次 + Root Block

实现类继承层次(照 AES70-2 Appendix A)与 `OcaRoot` 的 DefLevel-1 基础方法分派(GetClassIdentification/GetLockable/GetRole),以及 `OcaBlock`(Root Block, ONo 100)的 `GetMembers`。

**Files:**
- Create: `daemon/oca/classes/root.hpp`
- Create: `daemon/oca/classes/root.cpp`
- Modify: `daemon/oca/tests/oca_test.cpp`

**Interfaces:**
- Produces:`oca::OcaRoot`、`oca::OcaWorker`、`oca::OcaManager`、`oca::OcaBlock`。`OcaRoot::exec` 处理 DefLevel 1;`OcaBlock::exec` 处理 DefLevel 3 的 `GetMembers(5)` 并向上委托。
- Consumes:`oca::Object`、`oca::ObjectRegistry`(经 `Session::registry()`)、`oca::methods`、`oca::ocp1`。

- [ ] **Step 1: 写 `daemon/oca/classes/root.hpp`**

```cpp
//  classes/root.hpp - OcaRoot/Worker/Manager/Block 继承层次

#ifndef OCA_CLASSES_ROOT_HPP_
#define OCA_CLASSES_ROOT_HPP_

#include <string>

#include "oca/object.hpp"

namespace oca {

// OcaRoot {1,1} v2:DefLevel 1 基础方法
class OcaRoot : public Object {
 public:
  using Object::Object;  // 继承 ONo 构造
  Status exec(MethodID m, ocp1::Reader& req, ocp1::Writer& rsp,
              Session& sess) override;
  virtual std::string role() const { return {}; }

 protected:
  Status handle_root(uint16_t methodIndex, ocp1::Writer& rsp);
};

// OcaWorker {1,1,1} v2:Spec1 无自有 DefLevel-2 方法,委托 OcaRoot
class OcaWorker : public OcaRoot {
 public:
  using OcaRoot::OcaRoot;
};

// OcaManager {1,2} v2:Spec1 无自有 DefLevel-2 方法,委托 OcaRoot
class OcaManager : public OcaRoot {
 public:
  using OcaRoot::OcaRoot;
};

// OcaBlock {1,1,3} v2:DefLevel 3(Root Block, ONo 100)
class OcaBlock : public OcaWorker {
 public:
  explicit OcaBlock(ONo ono) : OcaWorker(ono) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 2; }
  std::string role() const override { return "Root Block"; }
  Status exec(MethodID m, ocp1::Reader& req, ocp1::Writer& rsp,
              Session& sess) override;

 private:
  Status GetMembers(ocp1::Writer& rsp, Session& sess);
};

}  // namespace oca

#endif  // OCA_CLASSES_ROOT_HPP_
```

- [ ] **Step 2: 写 `daemon/oca/classes/root.cpp`**

```cpp
//  classes/root.cpp - Root/Worker/Manager/Block 实现

#include "oca/classes/root.hpp"

#include "oca/methods.hpp"

namespace oca {

namespace {
const ClassIdentification kBlockClassId = {{{1, 1, 3}}, 2};
}  // namespace

const ClassIdentification& OcaBlock::class_id() const {
  return kBlockClassId;
}

Status OcaRoot::handle_root(uint16_t idx, ocp1::Writer& rsp) {
  switch (idx) {
    case methods::kRootGetClassIdentification: {
      const auto& ci = class_id();
      rsp.u16(static_cast<uint16_t>(ci.classID.levels.size()));
      for (auto lvl : ci.classID.levels) rsp.u16(lvl);
      rsp.u16(class_version());
      return Status::OK;
    }
    case methods::kRootGetLockable:
      rsp.u8(0);  // Spec1 不实现锁定 -> 不可锁
      return Status::OK;
    case methods::kRootGetRole:
      rsp.string(role());
      return Status::OK;
    default:
      return Status::BadMethod;
  }
}

Status OcaRoot::exec(MethodID m, ocp1::Reader& req, ocp1::Writer& rsp,
                     Session& sess) {
  if (m.defLevel == methods::kDefLevelRoot) {
    return handle_root(m.methodIndex, rsp);
  }
  return Status::BadMethod;
}

Status OcaBlock::exec(MethodID m, ocp1::Reader& req, ocp1::Writer& rsp,
                      Session& sess) {
  if (m.defLevel == methods::kDefLevelBlock) {
    switch (m.methodIndex) {
      case methods::kBlockGetMembers:
        return GetMembers(rsp, sess);
      default:
        return Status::BadMethod;
    }
  }
  return OcaWorker::exec(m, req, rsp, sess);  // 委托 DefLevel 1 -> OcaRoot
}

Status OcaBlock::GetMembers(ocp1::Writer& rsp, Session& sess) {
  auto* reg = sess.registry();
  if (!reg) return Status::DeviceError;
  auto objs = reg->objects_in_range(1, 99);  // 管理器成员
  rsp.u16(static_cast<uint16_t>(objs.size()));
  for (auto* o : objs) rsp.u32(o->ono());
  return Status::OK;
}

}  // namespace oca
```

- [ ] **Step 3: 在 `oca_test.cpp` 加 Root Block 分派测试**

```cpp
#include "oca/classes/root.hpp"

BOOST_AUTO_TEST_CASE(dispatch_root_block) {
  oca::OcaBlock root(100);
  oca::ObjectRegistry reg;
  reg.register_object(std::make_unique<StubObject>(1));
  reg.register_object(std::make_unique<StubObject>(2));
  reg.register_object(std::make_unique<StubObject>(4));
  reg.register_object(std::make_unique<StubObject>(100));  // root 自己

  oca::Session sess(1);
  sess.set_registry(&reg);

  // GetClassIdentification {1,1} defLevel=1 methodIndex=1
  oca::ocp1::Reader empty(nullptr, 0);
  oca::ocp1::Writer rsp;
  auto st = root.exec({oca::methods::kDefLevelRoot,
                       oca::methods::kRootGetClassIdentification},
                      empty, rsp, sess);
  BOOST_CHECK(st == oca::Status::OK);
  // ClassID = u16 count(3) + 1,1,3 + u16 version(2)
  oca::ocp1::Reader r(rsp.data(), rsp.size());
  BOOST_CHECK_EQUAL(r.u16(), 3u);  // 3 levels
  BOOST_CHECK_EQUAL(r.u16(), 1u);
  BOOST_CHECK_EQUAL(r.u16(), 1u);
  BOOST_CHECK_EQUAL(r.u16(), 3u);
  BOOST_CHECK_EQUAL(r.u16(), 2u);  // classVersion

  // GetMembers {3,5} -> [1,2,4]
  oca::ocp1::Writer rsp2;
  st = root.exec({oca::methods::kDefLevelBlock, oca::methods::kBlockGetMembers},
                 empty, rsp2, sess);
  BOOST_CHECK(st == oca::Status::OK);
  oca::ocp1::Reader r2(rsp2.data(), rsp2.size());
  BOOST_CHECK_EQUAL(r2.u16(), 3u);  // 3 members
  BOOST_CHECK_EQUAL(r2.u32(), 1u);
  BOOST_CHECK_EQUAL(r2.u32(), 2u);
  BOOST_CHECK_EQUAL(r2.u32(), 4u);

  // 未知方法 -> BadMethod
  oca::ocp1::Writer rsp3;
  st = root.exec({oca::methods::kDefLevelBlock, 99}, empty, rsp3, sess);
  BOOST_CHECK(st == oca::Status::BadMethod);

  // 未知 defLevel -> BadMethod
  oca::ocp1::Writer rsp4;
  st = root.exec({99, 1}, empty, rsp4, sess);
  BOOST_CHECK(st == oca::Status::BadMethod);
}
```

注:`StubObject`(Task 6 定义于测试匿名命名空间)在此复用,作为占位管理器对象。`reader` 用 `nullptr,0` 构造表示无参数(Reader 仅在方法读参数时访问,空方法不读)。

- [ ] **Step 4: 构建并跑测试**

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
make oca-test && ./oca-test -p -t dispatch_root_block
```

Expected: 通过。

- [ ] **Step 5: Commit**

```bash
git add daemon/oca/classes/root.hpp daemon/oca/classes/root.cpp daemon/oca/tests/oca_test.cpp
git commit -m "feat(oca): L2 Root/Worker/Manager/Block 层次与 Root Block GetMembers"
```

---

## Task 9: OcaDeviceManager + OcaDeviceIdentity

实现 `OcaDeviceManager`(ONo 1, ClassID {1,2,1} v4)的设备身份方法,数据来自 `OcaDeviceIdentity`(由 `OcaServer` 从 `Config` 构造,使 DeviceManager 不直接依赖 `Config`)。

**Files:**
- Create: `daemon/oca/classes/device_manager.hpp`
- Create: `daemon/oca/classes/device_manager.cpp`
- Modify: `daemon/oca/tests/oca_test.cpp`

**Interfaces:**
- Produces:`oca::OcaDeviceIdentity`、`oca::OcaDeviceManager`。方法(DefLevel 3):`GetOcaVersion(1)`、`GetSerialNumber(3)`、`GetDeviceName(4)`、`GetModelDescription(6)`、`GetState(13)`、`GetManagers(19)`。
- Consumes:`oca::OcaManager`、`oca::ObjectRegistry`、`oca::methods`、`oca::ocp1`。

- [ ] **Step 1: 写 `daemon/oca/classes/device_manager.hpp`**

```cpp
//  classes/device_manager.hpp - OcaDeviceManager + 设备身份

#ifndef OCA_CLASSES_DEVICE_MANAGER_HPP_
#define OCA_CLASSES_DEVICE_MANAGER_HPP_

#include <string>

#include "oca/classes/root.hpp"  // OcaManager

namespace oca {

// 由 OcaServer 从 Config 构造,解耦 DeviceManager 与 Config
struct OcaDeviceIdentity {
  std::string manufacturer = "AES67-Linux-Daemon";
  std::string model_name;      // oca_model,空则取 daemon 版本号
  std::string model_version;   // daemon 版本号
  std::string serial_number;   // oca_serial_number,空则取 node_id
  std::string device_name;     // oca_device_name,空则取 node_id
};

class OcaDeviceManager : public OcaManager {
 public:
  OcaDeviceManager(ONo ono, const OcaDeviceIdentity& identity)
      : OcaManager(ono), identity_(identity) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 4; }
  std::string role() const override { return "DeviceManager"; }
  Status exec(MethodID m, ocp1::Reader& req, ocp1::Writer& rsp,
              Session& sess) override;

 private:
  Status GetOcaVersion(ocp1::Writer& rsp);
  Status GetSerialNumber(ocp1::Writer& rsp);
  Status GetDeviceName(ocp1::Writer& rsp);
  Status GetModelDescription(ocp1::Writer& rsp);
  Status GetState(ocp1::Writer& rsp);
  Status GetManagers(ocp1::Writer& rsp, Session& sess);

  OcaDeviceIdentity identity_;
};

}  // namespace oca

#endif  // OCA_CLASSES_DEVICE_MANAGER_HPP_
```

- [ ] **Step 2: 写 `daemon/oca/classes/device_manager.cpp`**

```cpp
//  classes/device_manager.cpp

#include "oca/classes/device_manager.hpp"

#include "oca/methods.hpp"

namespace oca {

namespace {
const ClassIdentification kDeviceManagerClassId = {{{1, 2, 1}}, 4};
}  // namespace

const ClassIdentification& OcaDeviceManager::class_id() const {
  return kDeviceManagerClassId;
}

Status OcaDeviceManager::exec(MethodID m, ocp1::Reader& req,
                              ocp1::Writer& rsp, Session& sess) {
  if (m.defLevel == methods::kDefLevelDeviceMngr) {
    switch (m.methodIndex) {
      case methods::kDevGetOcaVersion:       return GetOcaVersion(rsp);
      case methods::kDevGetSerialNumber:     return GetSerialNumber(rsp);
      case methods::kDevGetDeviceName:       return GetDeviceName(rsp);
      case methods::kDevGetModelDescription: return GetModelDescription(rsp);
      case methods::kDevGetState:            return GetState(rsp);
      case methods::kDevGetManagers:         return GetManagers(rsp, sess);
      default:                               return Status::BadMethod;
    }
  }
  return OcaManager::exec(m, req, rsp, sess);  // DefLevel 1 -> OcaRoot
}

Status OcaDeviceManager::GetOcaVersion(ocp1::Writer& rsp) {
  rsp.u16(methods::kProtocolVersion);  // OCA version = 1 (AES70-2023)
  return Status::OK;
}

Status OcaDeviceManager::GetSerialNumber(ocp1::Writer& rsp) {
  rsp.string(identity_.serial_number);
  return Status::OK;
}

Status OcaDeviceManager::GetDeviceName(ocp1::Writer& rsp) {
  rsp.string(identity_.device_name);
  return Status::OK;
}

Status OcaDeviceManager::GetModelDescription(ocp1::Writer& rsp) {
  // OcaModelDescription = {Manufacturer: string, Name: string, Version: string}
  rsp.string(identity_.manufacturer);
  rsp.string(identity_.model_name);
  rsp.string(identity_.model_version);
  return Status::OK;
}

Status OcaDeviceManager::GetState(ocp1::Writer& rsp) {
  rsp.u8(static_cast<uint8_t>(DeviceState::Operational));  // Spec1 总是 Operational
  return Status::OK;
}

Status OcaDeviceManager::GetManagers(ocp1::Writer& rsp, Session& sess) {
  auto* reg = sess.registry();
  if (!reg) return Status::DeviceError;
  auto objs = reg->objects_in_range(1, 99);
  // Ocp1List<OcaManagerDescriptor>,ManagerDescriptor={ONo, ClassIdentification}
  rsp.u16(static_cast<uint16_t>(objs.size()));
  for (auto* o : objs) {
    rsp.u32(o->ono());
    const auto& ci = o->class_id();
    rsp.u16(static_cast<uint16_t>(ci.classID.levels.size()));
    for (auto lvl : ci.classID.levels) rsp.u16(lvl);
    rsp.u16(o->class_version());
  }
  return Status::OK;
}

}  // namespace oca
```

- [ ] **Step 3: 在 `oca_test.cpp` 加 DeviceManager 分派测试**

```cpp
#include "oca/classes/device_manager.hpp"

BOOST_AUTO_TEST_CASE(dispatch_device_manager) {
  oca::OcaDeviceIdentity id;
  id.manufacturer = "Acme";
  id.model_name = "AES67-daemon";
  id.model_version = "bondagit-3.1.0";
  id.serial_number = "node-42";
  id.device_name = "Studio A";

  oca::OcaDeviceManager dm(1, id);
  oca::ObjectRegistry reg;
  reg.register_object(std::make_unique<oca::OcaDeviceManager>(1, id));
  reg.register_object(std::make_unique<StubObject>(2));
  reg.register_object(std::make_unique<StubObject>(4));
  oca::Session sess(1);
  sess.set_registry(&reg);
  oca::ocp1::Reader empty(nullptr, 0);

  auto call = [&](uint16_t idx) {
    oca::ocp1::Writer w;
    auto st = dm.exec({oca::methods::kDefLevelDeviceMngr, idx}, empty, w, sess);
    return std::make_pair(st, w.take());
  };

  // GetOcaVersion -> 1
  auto [st1, b1] = call(oca::methods::kDevGetOcaVersion);
  BOOST_CHECK(st1 == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(b1.data(), b1.size()).u16(), 1u);

  // GetDeviceName -> "Studio A"
  auto [st2, b2] = call(oca::methods::kDevGetDeviceName);
  BOOST_CHECK(st2 == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(b2.data(), b2.size()).string(), "Studio A");

  // GetModelDescription -> {Acme, AES67-daemon, bondagit-3.1.0}
  auto [st3, b3] = call(oca::methods::kDevGetModelDescription);
  BOOST_CHECK(st3 == oca::Status::OK);
  {
    oca::ocp1::Reader r(b3.data(), b3.size());
    BOOST_CHECK_EQUAL(r.string(), "Acme");
    BOOST_CHECK_EQUAL(r.string(), "AES67-daemon");
    BOOST_CHECK_EQUAL(r.string(), "bondagit-3.1.0");
  }

  // GetState -> Operational(2)
  auto [st4, b4] = call(oca::methods::kDevGetState);
  BOOST_CHECK(st4 == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(b4.data(), b4.size()).u8(),
                    static_cast<uint8_t>(oca::DeviceState::Operational));

  // GetManagers -> 3 descriptors, first is ONo 1
  auto [st5, b5] = call(oca::methods::kDevGetManagers);
  BOOST_CHECK(st5 == oca::Status::OK);
  {
    oca::ocp1::Reader r(b5.data(), b5.size());
    BOOST_CHECK_EQUAL(r.u16(), 3u);
    BOOST_CHECK_EQUAL(r.u32(), 1u);  // first manager ONo
    uint16_t levels = r.u16();       // ClassID level count
    BOOST_CHECK_EQUAL(levels, 3u);   // {1,2,1}
    r.u16(); r.u16(); r.u16();       // skip 1,2,1
    BOOST_CHECK_EQUAL(r.u16(), 4u);  // classVersion
  }

  // 未实现方法 -> BadMethod
  auto [st6, b6] = call(oca::methods::kDevSetDeviceName);
  BOOST_CHECK(st6 == oca::Status::BadMethod);
}
```

- [ ] **Step 4: 构建并跑测试**

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
make oca-test && ./oca-test -p -t dispatch_device_manager
```

Expected: 通过。

- [ ] **Step 5: Commit**

```bash
git add daemon/oca/classes/device_manager.hpp daemon/oca/classes/device_manager.cpp daemon/oca/tests/oca_test.cpp
git commit -m "feat(oca): L2 OcaDeviceManager 设备身份方法"
```

---

## Task 10: OcaNetworkManager

实现 `OcaNetworkManager`(ONo 2, ClassID {1,2,3} v3)。Spec1 的 `GetNetworks` 返回空列表(网络对象推迟到 Spec2)。

**Files:**
- Create: `daemon/oca/classes/network_manager.hpp`
- Create: `daemon/oca/classes/network_manager.cpp`
- Modify: `daemon/oca/tests/oca_test.cpp`

**Interfaces:**
- Produces:`oca::OcaNetworkManager`。方法(DefLevel 3):`GetNetworks(1)` 返回空 `Ocp1List<ONo>`。
- Consumes:`oca::OcaManager`、`oca::methods`、`oca::ocp1`。

- [ ] **Step 1: 写 `daemon/oca/classes/network_manager.hpp`**

```cpp
//  classes/network_manager.hpp - OcaNetworkManager

#ifndef OCA_CLASSES_NETWORK_MANAGER_HPP_
#define OCA_CLASSES_NETWORK_MANAGER_HPP_

#include "oca/classes/root.hpp"  // OcaManager

namespace oca {

class OcaNetworkManager : public OcaManager {
 public:
  explicit OcaNetworkManager(ONo ono) : OcaManager(ono) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 3; }
  std::string role() const override { return "NetworkManager"; }
  Status exec(MethodID m, ocp1::Reader& req, ocp1::Writer& rsp,
              Session& sess) override;
};

}  // namespace oca

#endif  // OCA_CLASSES_NETWORK_MANAGER_HPP_
```

- [ ] **Step 2: 写 `daemon/oca/classes/network_manager.cpp`**

```cpp
//  classes/network_manager.cpp

#include "oca/classes/network_manager.hpp"

#include "oca/methods.hpp"

namespace oca {

namespace {
const ClassIdentification kNetworkManagerClassId = {{{1, 2, 3}}, 3};
}  // namespace

const ClassIdentification& OcaNetworkManager::class_id() const {
  return kNetworkManagerClassId;
}

Status OcaNetworkManager::exec(MethodID m, ocp1::Reader& req,
                               ocp1::Writer& rsp, Session& sess) {
  if (m.defLevel == methods::kDefLevelNetworkMngr) {
    switch (m.methodIndex) {
      case methods::kNetGetNetworks:
        rsp.u16(0);  // 空 Ocp1List<ONo>(Spec1 无网络对象)
        return Status::OK;
      default:
        return Status::BadMethod;
    }
  }
  return OcaManager::exec(m, req, rsp, sess);  // DefLevel 1 -> OcaRoot
}

}  // namespace oca
```

- [ ] **Step 3: 在 `oca_test.cpp` 加 NetworkManager 测试**

```cpp
#include "oca/classes/network_manager.hpp"

BOOST_AUTO_TEST_CASE(dispatch_network_manager) {
  oca::OcaNetworkManager nm(2);
  oca::Session sess(1);
  oca::ocp1::Reader empty(nullptr, 0);
  oca::ocp1::Writer rsp;
  auto st = nm.exec({oca::methods::kDefLevelNetworkMngr,
                     oca::methods::kNetGetNetworks}, empty, rsp, sess);
  BOOST_CHECK(st == oca::Status::OK);
  oca::ocp1::Reader r(rsp.data(), rsp.size());
  BOOST_CHECK_EQUAL(r.u16(), 0u);  // 空网络列表

  // GetClassIdentification(继承自 Root) -> {1,2,3} v3
  oca::ocp1::Writer rsp2;
  st = nm.exec({oca::methods::kDefLevelRoot,
                oca::methods::kRootGetClassIdentification}, empty, rsp2, sess);
  BOOST_CHECK(st == oca::Status::OK);
  oca::ocp1::Reader r2(rsp2.data(), rsp2.size());
  BOOST_CHECK_EQUAL(r2.u16(), 3u);
  BOOST_CHECK_EQUAL(r2.u16(), 1u);
  BOOST_CHECK_EQUAL(r2.u16(), 2u);
  BOOST_CHECK_EQUAL(r2.u16(), 3u);
  BOOST_CHECK_EQUAL(r2.u16(), 3u);  // classVersion
}
```

- [ ] **Step 4: 构建并跑测试**

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
make oca-test && ./oca-test -p -t dispatch_network_manager
```

Expected: 通过。

- [ ] **Step 5: Commit**

```bash
git add daemon/oca/classes/network_manager.hpp daemon/oca/classes/network_manager.cpp daemon/oca/tests/oca_test.cpp
git commit -m "feat(oca): L2 OcaNetworkManager(GetNetworks 空列表)"
```

---

## Task 11: OcaSubscriptionManager + EV2 通知机制

实现 `OcaSubscriptionManager`(ONo 4, ClassID {1,2,4} v2)的 EV2 订阅方法与通知投递机制。`AddSubscription2`/`RemoveSubscription2` 跨会话记录订阅;`trigger_event` 编码 Notification2 PDU 投递到订阅会话的写队列。演示事件为 DeviceManager.OperationalState。

**Files:**
- Create: `daemon/oca/classes/subscription_manager.hpp`
- Create: `daemon/oca/classes/subscription_manager.cpp`
- Modify: `daemon/oca/tests/oca_test.cpp`

**Interfaces:**
- Produces:`oca::OcaSubscriptionManager`。方法(DefLevel 3,候选索引):`AddSubscription2(1)`、`RemoveSubscription2(2)`;public `trigger_event()`、`remove_session()`。
- Consumes:`oca::OcaManager`、`oca::Session`、`oca::ocp1`(PduWriter/write_notification2)、`oca::methods`。

**参数布局假设(EV2 候选,Task 2 XMI 关卡验证):**
- `AddSubscription2` 入参:`OcaEvent{EmitterONo u32, EventID.DefLevel u16, EventID.EventIndex u16}` + `OcaBlob subscriberContext{u16 count, bytes}`;出参:`OcaSubscriptionID u32`。
- `RemoveSubscription2` 入参:`OcaSubscriptionID u32`;出参:无。

- [ ] **Step 1: 写 `daemon/oca/classes/subscription_manager.hpp`**

```cpp
//  classes/subscription_manager.hpp - EV2 订阅管理器

#ifndef OCA_CLASSES_SUBSCRIPTION_MANAGER_HPP_
#define OCA_CLASSES_SUBSCRIPTION_MANAGER_HPP_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "oca/classes/root.hpp"  // OcaManager
#include "oca/session.hpp"

namespace oca {

class OcaSubscriptionManager : public OcaManager {
 public:
  explicit OcaSubscriptionManager(ONo ono) : OcaManager(ono) {}
  const ClassIdentification& class_id() const override;
  uint16_t class_version() const override { return 2; }
  std::string role() const override { return "SubscriptionManager"; }
  Status exec(MethodID m, ocp1::Reader& req, ocp1::Writer& rsp,
              Session& sess) override;

  // 触发事件:遍历订阅者,编码 Notification2 PDU 投递到各会话写队列
  void trigger_event(ONo emitterONo, EventID eventID,
                     const uint8_t* data, uint16_t dataCount);
  // 连接断开时清理该会话的所有订阅
  void remove_session(Session* sess);

 private:
  Status AddSubscription2(ocp1::Reader& req, ocp1::Writer& rsp, Session& sess);
  Status RemoveSubscription2(ocp1::Reader& req, ocp1::Writer& rsp, Session& sess);

  struct Entry {
    uint32_t id;
    Session* sess;
    ONo emitterONo;
    EventID eventID;
  };
  std::mutex mutex_;
  std::vector<Entry> entries_;
  std::atomic<uint32_t> next_id_{1};
};

}  // namespace oca

#endif  // OCA_CLASSES_SUBSCRIPTION_MANAGER_HPP_
```

- [ ] **Step 2: 写 `daemon/oca/classes/subscription_manager.cpp`**

```cpp
//  classes/subscription_manager.cpp

#include "oca/classes/subscription_manager.hpp"

#include "oca/methods.hpp"
#include "oca/ocp1.hpp"

namespace oca {

namespace {
const ClassIdentification kSubscriptionManagerClassId = {{{1, 2, 4}}, 2};
}  // namespace

const ClassIdentification& OcaSubscriptionManager::class_id() const {
  return kSubscriptionManagerClassId;
}

Status OcaSubscriptionManager::exec(MethodID m, ocp1::Reader& req,
                                    ocp1::Writer& rsp, Session& sess) {
  if (m.defLevel == methods::kDefLevelSubMngr) {
    switch (m.methodIndex) {
      case methods::kSubAddSubscription2:    return AddSubscription2(req, rsp, sess);
      case methods::kSubRemoveSubscription2: return RemoveSubscription2(req, rsp, sess);
      default: return Status::BadMethod;  // PropertyChange 变体 Spec1 不实现
    }
  }
  return OcaManager::exec(m, req, rsp, sess);
}

Status OcaSubscriptionManager::AddSubscription2(ocp1::Reader& req,
                                                ocp1::Writer& rsp,
                                                Session& sess) {
  ONo emitter = req.u32();
  EventID eid{req.u16(), req.u16()};
  OcaBlob ctx = req.blob();  // subscriberContext(未使用,仅消费)

  uint32_t id = next_id_.fetch_add(1);
  {
    std::lock_guard<std::mutex> lk(mutex_);
    entries_.push_back({id, &sess, emitter, eid});
  }
  sess.add_subscription({emitter, eid, std::move(ctx)});
  rsp.u32(id);  // 返回 subscriptionID
  return Status::OK;
}

Status OcaSubscriptionManager::RemoveSubscription2(ocp1::Reader& req,
                                                   ocp1::Writer& rsp,
                                                   Session& sess) {
  uint32_t id = req.u32();
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (it->id == id) {
      sess.remove_subscription(it->emitterONo, it->eventID);
      entries_.erase(it);
      return Status::OK;
    }
  }
  return Status::BadONo;  // 未知 subscriptionID
}

void OcaSubscriptionManager::trigger_event(ONo emitterONo, EventID eventID,
                                           const uint8_t* data,
                                           uint16_t dataCount) {
  std::vector<std::pair<Session*, Entry>> targets;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& e : entries_) {
      if (e.emitterONo == emitterONo &&
          e.eventID.defLevel == eventID.defLevel &&
          e.eventID.eventIndex == eventID.eventIndex) {
        targets.emplace_back(e.sess, e);
      }
    }
  }
  for (auto& [sp, e] : targets) {
    oca::ocp1::Writer nw;
    oca::ocp1::write_notification2(nw, emitterONo, eventID, 0 /*Event*/,
                                   data, dataCount);
    auto pdu = oca::ocp1::PduWriter::build_notification2_pdu(1, nw.data(),
                                                             nw.size());
    sp->enqueue_notification(std::move(pdu));
  }
}

void OcaSubscriptionManager::remove_session(Session* sess) {
  std::lock_guard<std::mutex> lk(mutex_);
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
      [sess](const Entry& e) { return e.sess == sess; }), entries_.end());
}

}  // namespace oca
```

注:`subscription_manager.cpp` 用 `std::remove_if`,顶部加 `#include <algorithm>`。

- [ ] **Step 3: 修正 `subscription_manager.cpp` include 段**

```cpp
#include "oca/classes/subscription_manager.hpp"

#include <algorithm>

#include "oca/methods.hpp"
#include "oca/ocp1.hpp"
```

- [ ] **Step 4: 在 `oca_test.cpp` 加 SubscriptionManager 测试**

```cpp
#include "oca/classes/subscription_manager.hpp"

BOOST_AUTO_TEST_CASE(dispatch_subscription_ev2) {
  oca::OcaSubscriptionManager sm(4);
  oca::Session sess(1);
  oca::ObjectRegistry reg;
  sess.set_registry(&reg);

  // 构造 AddSubscription2 请求:emitter=1, event={3,1}, ctx=空
  oca::ocp1::Writer reqw;
  reqw.u32(1);  // EmitterONo
  reqw.u16(oca::methods::kDefLevelDeviceMngr);
  reqw.u16(oca::methods::kEventOperationalState);
  reqw.u16(0);  // 空 subscriberContext
  oca::ocp1::Reader req(reqw.data(), reqw.size());

  oca::ocp1::Writer rspw;
  auto st = sm.exec({oca::methods::kDefLevelSubMngr,
                     oca::methods::kSubAddSubscription2}, req, rspw, sess);
  BOOST_CHECK(st == oca::Status::OK);
  oca::ocp1::Reader rspr(rspw.data(), rspw.size());
  uint32_t subId = rspr.u32();
  BOOST_CHECK(subId != 0);
  BOOST_CHECK(sess.has_subscription(1, {oca::methods::kDefLevelDeviceMngr,
                                        oca::methods::kEventOperationalState}));

  // 触发 OperationalState 事件 -> 会话写队列应收到 Notification2 PDU
  uint8_t evdata = static_cast<uint8_t>(oca::DeviceState::Operational);
  sm.trigger_event(1, {oca::methods::kDefLevelDeviceMngr,
                       oca::methods::kEventOperationalState}, &evdata, 1);
  std::vector<uint8_t> pdu;
  BOOST_CHECK(sess.take_notification(pdu));
  // 校验 PDU:sync + header(pduType=Ntf2) + notification2
  BOOST_REQUIRE(pdu.size() > 10);
  BOOST_CHECK_EQUAL(pdu[0], 0x3B);
  auto hdr = oca::ocp1::PduReader::try_parse_header(pdu.data() + 1,
                                                    pdu.size() - 1);
  BOOST_REQUIRE(hdr);
  BOOST_CHECK_EQUAL(hdr->pduType, oca::methods::kPduNtf2);
  auto ntfs = oca::ocp1::PduReader::parse_notifications2(
      pdu.data() + 1 + 9, pdu.size() - 1 - 9, hdr->messageCount);
  BOOST_REQUIRE_EQUAL(ntfs.size(), 1u);
  BOOST_CHECK_EQUAL(ntfs[0].emitterONo, 1u);
  BOOST_CHECK_EQUAL(ntfs[0].dataCount, 1u);
  BOOST_CHECK_EQUAL(ntfs[0].data[0], evdata);
  BOOST_CHECK(!sess.take_notification(pdu));  // 队列已空

  // RemoveSubscription2
  oca::ocp1::Writer reqw2;
  reqw2.u32(subId);
  oca::ocp1::Reader req2(reqw2.data(), reqw2.size());
  oca::ocp1::Writer rspw2;
  st = sm.exec({oca::methods::kDefLevelSubMngr,
                oca::methods::kSubRemoveSubscription2}, req2, rspw2, sess);
  BOOST_CHECK(st == oca::Status::OK);
  BOOST_CHECK(!sess.has_subscription(1, {oca::methods::kDefLevelDeviceMngr,
                                         oca::methods::kEventOperationalState}));
}
```

- [ ] **Step 5: 构建并跑测试**

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
make oca-test && ./oca-test -p -t dispatch_subscription_ev2
```

Expected: 通过。

- [ ] **Step 6: Commit**

```bash
git add daemon/oca/classes/subscription_manager.hpp daemon/oca/classes/subscription_manager.cpp daemon/oca/tests/oca_test.cpp
git commit -m "feat(oca): L2 OcaSubscriptionManager EV2 订阅与通知投递"
```

---

# L3 传输层、门面、mDNS、集成与验收

## Task 12: TCP 传输层(accept + 每连接读线程 + KeepAlive + 分派)

实现 `Transport`:accept 线程 + 每连接读线程(TCP 字节流上 PDU 分帧:sync 0x3B + 9 字节头 + pduSize 载荷)+ KeepAlive 处理 + 命令分派回写 + 通知队列排空。BSD socket,无新依赖。

**Files:**
- Create: `daemon/oca/transport.hpp`
- Create: `daemon/oca/transport.cpp`
- Modify: `daemon/oca/tests/oca_test.cpp`

**Interfaces:**
- Produces:`oca::Transport`(`start(port)/stop()/port()`)。`port=0` 自动分配,`port()` 返回实际端口。
- Consumes:`oca::ObjectRegistry`、`oca::OcaSubscriptionManager`、`oca::ocp1`、`oca::methods`。

- [ ] **Step 1: 写 `daemon/oca/transport.hpp`**

```cpp
//  transport.hpp - OCP.1 TCP 传输层

#ifndef OCA_TRANSPORT_HPP_
#define OCA_TRANSPORT_HPP_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "oca/object.hpp"

namespace oca {

class OcaSubscriptionManager;

class Transport {
 public:
  Transport(ObjectRegistry* registry, OcaSubscriptionManager* sub_mgr = nullptr);
  ~Transport();
  bool start(uint16_t port);  // 0 = 自动分配
  void stop();
  uint16_t port() const { return port_; }

 private:
  void accept_loop();
  void conn_loop(int fd, ONo session_id);
  static bool send_all(int fd, const uint8_t* data, size_t len);

  ObjectRegistry* registry_;
  OcaSubscriptionManager* sub_mgr_;
  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::atomic<bool> running_{false};
  std::thread accept_thread_;
  std::atomic<ONo> next_session_id_{1};
  std::mutex conns_mutex_;
  std::vector<std::thread> conn_threads_;
};

}  // namespace oca

#endif  // OCA_TRANSPORT_HPP_
```

- [ ] **Step 2: 写 `daemon/oca/transport.cpp`**

```cpp
//  transport.cpp - TCP 传输实现

#include "oca/transport.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <utility>

#include "oca/classes/subscription_manager.hpp"
#include "oca/methods.hpp"
#include "oca/ocp1.hpp"
#include "oca/session.hpp"

namespace oca {

Transport::Transport(ObjectRegistry* registry, OcaSubscriptionManager* sub_mgr)
    : registry_(registry), sub_mgr_(sub_mgr) {}

Transport::~Transport() { stop(); }

bool Transport::start(uint16_t port) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return false;
  int yes = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (::listen(listen_fd_, 8) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  // 取实际端口
  socklen_t l = sizeof(addr);
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &l);
  port_ = ntohs(addr.sin_port);
  running_ = true;
  accept_thread_ = std::thread(&Transport::accept_loop, this);
  return true;
}

void Transport::stop() {
  if (!running_.exchange(false)) return;
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (accept_thread_.joinable()) accept_thread_.join();
  std::lock_guard<std::mutex> lk(conns_mutex_);
  for (auto& t : conn_threads_) {
    if (t.joinable()) t.join();
  }
  conn_threads_.clear();
}

void Transport::accept_loop() {
  while (running_) {
    int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (!running_) break;
      continue;
    }
    // 1s 接收超时,便于排空通知队列与检测心跳
    struct timeval tv {1, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ONo sid = next_session_id_.fetch_add(1);
    std::thread t(&Transport::conn_loop, this, fd, sid);
    std::lock_guard<std::mutex> lk(conns_mutex_);
    conn_threads_.push_back(std::move(t));
    // 清理已结束的线程
    for (auto it = conn_threads_.begin(); it != conn_threads_.end();) {
      // 无法非阻塞 join,保留线程;stop() 时统一 join。这里仅去重已 detach 的(无)。
      ++it;
    }
  }
}

bool Transport::send_all(int fd, const uint8_t* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

// 阻塞读取精确 n 字节;false = EOF/错误/超时中途
static bool recv_exact(int fd, uint8_t* buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = ::recv(fd, buf + got, n - got, 0);
    if (r <= 0) return false;  // EOF/错误/超时
    got += static_cast<size_t>(r);
  }
  return true;
}

static uint64_t now_sec() {
  return static_cast<uint64_t>(std::time(nullptr));
}

void Transport::conn_loop(int fd, ONo session_id) {
  Session sess(session_id);
  sess.set_registry(registry_);
  std::mutex write_mutex;

  auto send_pdu = [&](const std::vector<uint8_t>& pdu) {
    std::lock_guard<std::mutex> lk(write_mutex);
    send_all(fd, pdu.data(), pdu.size());
  };

  while (running_) {
    // 1. 读 sync 字节
    uint8_t b = 0;
    ssize_t r = ::recv(fd, &b, 1, 0);
    if (r <= 0) {
      if (r < 0 && errno == EAGAIN) {
        // 空闲超时:排空通知队列、检测心跳
        std::vector<uint8_t> pdu;
        while (sess.take_notification(pdu)) send_pdu(pdu);
        if (sess.expired(now_sec())) break;
        continue;
      }
      break;  // EOF/错误
    }
    if (b != methods::kSyncVal) continue;  // 重新同步

    // 2. 读 9 字节头
    uint8_t hdrbuf[9];
    if (!recv_exact(fd, hdrbuf, 9)) break;
    auto hdr = ocp1::PduReader::try_parse_header(hdrbuf, 9);
    if (!hdr || hdr->protocolVersion != methods::kProtocolVersion) break;
    if (hdr->pduSize < 9) break;

    // 3. 读 payload(pduSize - 9 字节)
    size_t payloadLen = hdr->pduSize - 9;
    std::vector<uint8_t> payload(payloadLen);
    if (payloadLen && !recv_exact(fd, payload.data(), payloadLen)) break;

    sess.touch(now_sec());

    // 4. 按 PduType 分派
    if (hdr->pduType == methods::kPduKeepAlive) {
      uint16_t hb = 15;
      if (payloadLen >= 2) {
        ocp1::Reader hr(payload.data(), payloadLen);
        hb = hr.u16();
      }
      sess.set_heartbeat(hb);
      send_pdu(ocp1::PduWriter::build_keepalive_pdu(hb));  // 回应 KeepAlive
    } else if (hdr->pduType == methods::kPduCommand ||
               hdr->pduType == methods::kPduCommandRrq) {
      auto cmds = ocp1::PduReader::parse_commands(
          payload.data(), payloadLen, hdr->messageCount);
      ocp1::Writer rspAcc;
      for (const auto& c : cmds) {
        ocp1::Writer params;
        Status st;
        Object* obj = registry_->find(c.targetONo);
        if (!obj) {
          st = Status::BadONo;
        } else {
          ocp1::Reader pr(c.paramData, c.paramCount);
          st = obj->exec(c.methodID, pr, params, sess);
        }
        ocp1::write_response(rspAcc, c.handle, st, params.data(),
                             static_cast<uint8_t>(params.size()));
      }
      send_pdu(ocp1::PduWriter::build_response_pdu(
          static_cast<uint16_t>(cmds.size()), rspAcc.data(), rspAcc.size()));
    }
    // 其他 PduType(Response/Ntf)在设备侧忽略

    // 5. 排空通知队列
    std::vector<uint8_t> pdu;
    while (sess.take_notification(pdu)) send_pdu(pdu);
  }

  if (sub_mgr_) sub_mgr_->remove_session(&sess);
  ::close(fd);
}

}  // namespace oca
```

注:`conn_loop` 中 `params.size()` 最大 255 字节(`paramCount` 是 u8)。若某方法响应参数 >255 字节,需分块或改用 Ocp1LongList;Spec1 的方法响应均远小于 255,可接受。

- [ ] **Step 3: 在 `oca_test.cpp` 加传输集成测试**

```cpp
#include "oca/transport.hpp"
#include "oca/classes/device_manager.hpp"
#include "oca/classes/network_manager.hpp"
#include "oca/classes/subscription_manager.hpp"

BOOST_AUTO_TEST_CASE(transport_keepalive_and_command) {
  // 构造对象树
  oca::OcaDeviceIdentity id;
  id.model_name = "daemon"; id.model_version = "v1";
  id.serial_number = "n1"; id.device_name = "dev";
  oca::ObjectRegistry reg;
  auto* dm = new oca::OcaDeviceManager(1, id);
  auto* nm = new oca::OcaNetworkManager(2);
  auto* sm = new oca::OcaSubscriptionManager(4);
  auto* root = new oca::OcaBlock(100);
  reg.register_object(std::unique_ptr<oca::Object>(dm));
  reg.register_object(std::unique_ptr<oca::Object>(nm));
  reg.register_object(std::unique_ptr<oca::Object>(sm));
  reg.register_object(std::unique_ptr<oca::Object>(root));

  oca::Transport transport(&reg, sm);
  BOOST_REQUIRE(transport.start(0));  // 自动端口
  uint16_t port = transport.port();

  // 连接
  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{}; addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  BOOST_REQUIRE(::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

  auto sendPdu = [&](const std::vector<uint8_t>& p) {
    BOOST_CHECK_EQUAL(::send(sock, p.data(), p.size(), 0), (ssize_t)p.size());
  };
  auto recvPdu = [&](std::vector<uint8_t>& out) -> bool {
    uint8_t sync; if (::recv(sock, &sync, 1, 0) != 1) return false;
    if (sync != 0x3B) return false;
    uint8_t hdr[9];
    size_t got = 0;
    while (got < 9) {
      ssize_t r = ::recv(sock, hdr + got, 9 - got, 0);
      if (r <= 0) return false;
      got += r;
    }
    auto h = oca::ocp1::PduReader::try_parse_header(hdr, 9);
    if (!h) return false;
    size_t plen = h->pduSize - 9;
    out.assign(hdr, hdr + 9);
    out.resize(9 + plen);
    got = 0;
    while (got < plen) {
      ssize_t r = ::recv(sock, out.data() + 9 + got, plen - got, 0);
      if (r <= 0) return false;
      got += r;
    }
    out.insert(out.begin(), 0x3B);
    return true;
  };

  // 1) KeepAlive(5s) -> 应收到 KeepAlive 回应
  sendPdu(oca::ocp1::PduWriter::build_keepalive_pdu(5));
  std::vector<uint8_t> ka;
  BOOST_CHECK(recvPdu(ka));
  auto kah = oca::ocp1::PduReader::try_parse_header(ka.data() + 1, ka.size() - 1);
  BOOST_REQUIRE(kah);
  BOOST_CHECK_EQUAL(kah->pduType, oca::methods::kPduKeepAlive);

  // 2) GetOcaVersion 命令(handle=7)
  oca::ocp1::Writer cw;
  oca::ocp1::write_command(cw, 7, 1,
      {oca::methods::kDefLevelDeviceMngr, oca::methods::kDevGetOcaVersion},
      nullptr, 0);
  sendPdu(oca::ocp1::PduWriter::build_command_pdu(1, cw.data(), cw.size()));
  std::vector<uint8_t> rsp;
  BOOST_CHECK(recvPdu(rsp));
  auto rh = oca::ocp1::PduReader::try_parse_header(rsp.data() + 1, rsp.size() - 1);
  BOOST_REQUIRE(rh);
  BOOST_CHECK_EQUAL(rh->pduType, oca::methods::kPduResponse);
  auto rsps = oca::ocp1::PduReader::parse_responses(rsp.data() + 1 + 9,
                                                    rh->pduSize - 9, rh->messageCount);
  BOOST_REQUIRE_EQUAL(rsps.size(), 1u);
  BOOST_CHECK_EQUAL(rsps[0].handle, 7u);
  BOOST_CHECK(rsps[0].statusCode == oca::Status::OK);
  BOOST_CHECK_EQUAL(rsps[0].paramCount, 2);
  // OcaVersion=1 big-endian
  BOOST_CHECK_EQUAL(rsps[0].paramData[0], 0x00);
  BOOST_CHECK_EQUAL(rsps[0].paramData[1], 0x01);

  ::close(sock);
  transport.stop();
}
```

注:测试顶部需 `#include <arpa/inet.h>` `<sys/socket.h>` `<unistd.h>`(网络头)。在 `oca_test.cpp` include 段补上。

- [ ] **Step 4: 补 `oca_test.cpp` 网络头**

在 `oca_test.cpp` 的 Boost include 之前加:

```cpp
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
```

- [ ] **Step 5: 构建并跑测试**

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
make oca-test && ./oca-test -p -t transport_keepalive_and_command
```

Expected: 通过(KeepAlive 回应 + GetOcaVersion=1)。若偶发端口/时序问题,重跑。

- [ ] **Step 6: Commit**

```bash
git add daemon/oca/transport.hpp daemon/oca/transport.cpp daemon/oca/tests/oca_test.cpp
git commit -m "feat(oca): L3 TCP 传输层(分帧/KeepAlive/分派/通知排空)"
```

---

## Task 13: OcaServer 门面

`OcaServer` 持有 `ObjectRegistry` + `Transport`(+ `MdnsPrinter`),从 `OcaServerConfig`(POD,由 main.cpp 从 `Config` 填充)构造对象树并启停。**勘误**:规格说"OcaServer 依赖 Config",此处细化为"OcaServer 依赖 POD `OcaServerConfig`"(由 main.cpp 从 Config 填充),使 OcaServer 在 oca-test 中无需链接 config.cpp 即可测。

**Files:**
- Create: `daemon/oca/oca_server.hpp`
- Create: `daemon/oca/oca_server.cpp`
- Modify: `daemon/oca/tests/oca_test.cpp`

**Interfaces:**
- Produces:`oca::OcaServerConfig`、`oca::OcaServer`(`start/stop/port/subscription_manager`)。
- Consumes:`oca::Transport`、4 个对象类、`oca::OcaDeviceIdentity`。

- [ ] **Step 1: 写 `daemon/oca/oca_server.hpp`**

```cpp
//  oca_server.hpp - OCA 门面

#ifndef OCA_OCA_SERVER_HPP_
#define OCA_OCA_SERVER_HPP_

#include <cstdint>
#include <memory>
#include <string>

#include "oca/object.hpp"

namespace oca {

class Transport;
class OcaSubscriptionManager;
#ifdef _USE_AVAHI_
class MdnsPublisher;
#endif

// 由 main.cpp 从 Config 填充(解耦 OcaServer 与 Config)
struct OcaServerConfig {
  uint16_t port = 65037;
  std::string device_name;       // 空 -> node_id
  std::string manufacturer = "AES67-Linux-Daemon";
  std::string model;             // 空 -> daemon_version
  std::string serial_number;     // 空 -> node_id
  std::string node_id;           // device_name/serial 的回退
  std::string daemon_version;
  bool mdns_enabled = false;
};

class OcaServer {
 public:
  explicit OcaServer(const OcaServerConfig& cfg);
  ~OcaServer();
  bool start();  // 在 cfg.port 监听(0=自动)
  void stop();
  uint16_t port() const;
  OcaSubscriptionManager* subscription_manager();

 private:
  OcaServerConfig cfg_;
  ObjectRegistry registry_;
  OcaSubscriptionManager* sub_mgr_ = nullptr;  // 归 registry_ 所有
  std::unique_ptr<Transport> transport_;
#ifdef _USE_AVAHI_
  std::unique_ptr<MdnsPublisher> mdns_;
#endif
};

}  // namespace oca

#endif  // OCA_OCA_SERVER_HPP_
```

- [ ] **Step 2: 写 `daemon/oca/oca_server.cpp`**

```cpp
//  oca_server.cpp

#include "oca/oca_server.hpp"

#include "oca/classes/device_manager.hpp"
#include "oca/classes/network_manager.hpp"
#include "oca/classes/root.hpp"
#include "oca/classes/subscription_manager.hpp"
#include "oca/transport.hpp"

#ifdef _USE_AVAHI_
#include "oca/mdns_publisher.hpp"
#endif

namespace oca {

OcaServer::OcaServer(const OcaServerConfig& cfg) : cfg_(cfg) {
  OcaDeviceIdentity id;
  id.manufacturer = cfg_.manufacturer.empty() ? "AES67-Linux-Daemon"
                                              : cfg_.manufacturer;
  id.model_name = cfg_.model.empty() ? cfg_.daemon_version : cfg_.model;
  id.model_version = cfg_.daemon_version;
  id.serial_number = cfg_.serial_number.empty() ? cfg_.node_id
                                                : cfg_.serial_number;
  id.device_name = cfg_.device_name.empty() ? cfg_.node_id : cfg_.device_name;

  auto* dm = new OcaDeviceManager(1, id);
  auto* nm = new OcaNetworkManager(2);
  auto* sm = new OcaSubscriptionManager(4);
  auto* root = new OcaBlock(100);
  sub_mgr_ = sm;
  registry_.register_object(std::unique_ptr<Object>(dm));
  registry_.register_object(std::unique_ptr<Object>(nm));
  registry_.register_object(std::unique_ptr<Object>(sm));
  registry_.register_object(std::unique_ptr<Object>(root));

  transport_ = std::make_unique<Transport>(&registry_, sub_mgr_);
}

OcaServer::~OcaServer() { stop(); }

bool OcaServer::start() {
  if (!transport_ || !transport_->start(cfg_.port)) return false;
#ifdef _USE_AVAHI_
  if (cfg_.mdns_enabled) {
    mdns_ = std::make_unique<MdnsPublisher>(cfg_.device_name.empty()
                                                ? cfg_.node_id
                                                : cfg_.device_name,
                                            transport_->port());
    mdns_->start();
  }
#endif
  return true;
}

void OcaServer::stop() {
#ifdef _USE_AVAHI_
  if (mdns_) mdns_->stop();
  mdns_.reset();
#endif
  if (transport_) transport_->stop();
}

uint16_t OcaServer::port() const {
  return transport_ ? transport_->port() : 0;
}

OcaSubscriptionManager* OcaServer::subscription_manager() {
  return sub_mgr_;
}

}  // namespace oca
```

- [ ] **Step 3: 在 `oca_test.cpp` 加 OcaServer 门面测试**

```cpp
#include "oca/oca_server.hpp"

BOOST_AUTO_TEST_CASE(oca_server_facade) {
  oca::OcaServerConfig cfg;
  cfg.port = 0;  // 自动
  cfg.node_id = "AES67 daemon abc123";
  cfg.daemon_version = "bondagit-3.1.0";
  cfg.manufacturer = "AES67-Linux-Daemon";

  oca::OcaServer server(cfg);
  BOOST_REQUIRE(server.start());
  uint16_t port = server.port();

  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{}; addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  BOOST_REQUIRE(::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

  // 复用 Task 12 的 sendPdu/recvPdu 逻辑(此处内联简版)
  auto sendPdu = [&](const std::vector<uint8_t>& p) {
    ::send(sock, p.data(), p.size(), 0);
  };
  auto recvPdu = [&](std::vector<uint8_t>& out) -> bool {
    uint8_t sync; if (::recv(sock, &sync, 1, 0) != 1 || sync != 0x3B) return false;
    uint8_t hdr[9]; size_t got = 0;
    while (got < 9) { ssize_t r = ::recv(sock, hdr+got, 9-got, 0); if (r<=0) return false; got += r; }
    auto h = oca::ocp1::PduReader::try_parse_header(hdr, 9);
    if (!h) return false;
    size_t plen = h->pduSize - 9;
    out.assign(hdr, hdr+9); out.resize(9+plen); got = 0;
    while (got < plen) { ssize_t r = ::recv(sock, out.data()+9+got, plen-got, 0); if (r<=0) return false; got += r; }
    out.insert(out.begin(), 0x3B);
    return true;
  };

  // KeepAlive
  sendPdu(oca::ocp1::PduWriter::build_keepalive_pdu(5));
  std::vector<uint8_t> ka; BOOST_CHECK(recvPdu(ka));

  // GetDeviceName(4) -> node_id("AES67 daemon abc123")
  oca::ocp1::Writer cw;
  oca::ocp1::write_command(cw, 1, 1,
      {oca::methods::kDefLevelDeviceMngr, oca::methods::kDevGetDeviceName},
      nullptr, 0);
  sendPdu(oca::ocp1::PduWriter::build_command_pdu(1, cw.data(), cw.size()));
  std::vector<uint8_t> rsp; BOOST_CHECK(recvPdu(rsp));
  auto rh = oca::ocp1::PduReader::try_parse_header(rsp.data()+1, rsp.size()-1);
  auto rsps = oca::ocp1::PduReader::parse_responses(rsp.data()+1+9,
                  rh->pduSize-9, rh->messageCount);
  BOOST_REQUIRE_EQUAL(rsps.size(), 1u);
  BOOST_CHECK(rsps[0].statusCode == oca::Status::OK);
  oca::ocp1::Reader pr(rsps[0].paramData, rsps[0].paramCount);
  BOOST_CHECK_EQUAL(pr.string(), "AES67 daemon abc123");

  // GetMembers(5) on Root Block(ONo 100) -> [1,2,4]
  oca::ocp1::Writer cw2;
  oca::ocp1::write_command(cw2, 2, 100,
      {oca::methods::kDefLevelBlock, oca::methods::kBlockGetMembers}, nullptr, 0);
  sendPdu(oca::ocp1::PduWriter::build_command_pdu(1, cw2.data(), cw2.size()));
  std::vector<uint8_t> rsp2; BOOST_CHECK(recvPdu(rsp2));
  auto rh2 = oca::ocp1::PduReader::try_parse_header(rsp2.data()+1, rsp2.size()-1);
  auto rsps2 = oca::ocp1::PduReader::parse_responses(rsp2.data()+1+9,
                   rh2->pduSize-9, rh2->messageCount);
  BOOST_REQUIRE_EQUAL(rsps2.size(), 1u);
  oca::ocp1::Reader pr2(rsps2[0].paramData, rsps2[0].paramCount);
  BOOST_CHECK_EQUAL(pr2.u16(), 3u);
  BOOST_CHECK_EQUAL(pr2.u32(), 1u);
  BOOST_CHECK_EQUAL(pr2.u32(), 2u);
  BOOST_CHECK_EQUAL(pr2.u32(), 4u);

  ::close(sock);
  server.stop();
}
```

- [ ] **Step 4: 构建并跑测试**

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
make oca-test && ./oca-test -p -t oca_server_facade
```

Expected: 通过。GetDeviceName 回退到 node_id;GetMembers 返回 [1,2,4]。

- [ ] **Step 5: Commit**

```bash
git add daemon/oca/oca_server.hpp daemon/oca/oca_server.cpp daemon/oca/tests/oca_test.cpp
git commit -m "feat(oca): L3 OcaServer 门面(对象树装配与启停)"
```

---

## Task 14: Avahi mDNS 发布器(`_oca._tcp`)

`MdnsPublisher` 用 Avahi threaded client 发布 `_oca._tcp` 服务,TXT:`txtvers=1`、`protovers=1`,实例名取设备名,端口取 `oca_port`。仅 `WITH_AVAHI` 编译。

**Files:**
- Create: `daemon/oca/mdns_publisher.hpp`
- Create: `daemon/oca/mdns_publisher.cpp`
- Modify: `daemon/oca/tests/oca_test.cpp`(AVAHI 守卫的冒烟测试)

**Interfaces:**
- Produces:`oca::MdnsPublisher`(`start/stop`)。仅 `_USE_AVAHI_` 下可见。
- Consumes:Avahi client API。

- [ ] **Step 1: 写 `daemon/oca/mdns_publisher.hpp`**

```cpp
//  mdns_publisher.hpp - Avahi _oca._tcp 发布器(仅 WITH_AVAHI)

#ifndef OCA_MDNS_PUBLISHER_HPP_
#define OCA_MDNS_PUBLISHER_HPP_

#ifdef _USE_AVAHI_

#include <cstdint>
#include <string>

struct AvahiThreadedPoll;
struct AvahiClient;
struct AvahiEntryGroup;

namespace oca {

class MdnsPublisher {
 public:
  MdnsPublisher(std::string name, uint16_t port);
  ~MdnsPublisher();
  bool start();
  void stop();

 private:
  static void client_cb(struct AvahiClient* c, int state, void* userdata);
  static void group_cb(struct AvahiEntryGroup* g, int state, void* userdata);
  void create_service(struct AvahiClient* c);

  std::string name_;
  uint16_t port_;
  struct AvahiThreadedPoll* poll_ = nullptr;
  struct AvahiClient* client_ = nullptr;
  struct AvahiEntryGroup* group_ = nullptr;
};

}  // namespace oca

#endif  // _USE_AVAHI_
#endif  // OCA_MDNS_PUBLISHER_HPP_
```

- [ ] **Step 2: 写 `daemon/oca/mdns_publisher.cpp`**

```cpp
//  mdns_publisher.cpp

#ifdef _USE_AVAHI_

#include "oca/mdns_publisher.hpp"

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/threadwatch.h>

namespace oca {

MdnsPublisher::MdnsPublisher(std::string name, uint16_t port)
    : name_(std::move(name)), port_(port) {}

MdnsPublisher::~MdnsPublisher() { stop(); }

bool MdnsPublisher::start() {
  if (poll_) return true;
  poll_ = avahi_threaded_poll_new();
  if (!poll_) return false;
  int err;
  client_ = avahi_client_new(avahi_threaded_poll_get(poll_),
                             AVAHI_CLIENT_NO_FAIL, &MdnsPublisher::client_cb,
                             this, &err);
  if (!client_) {
    avahi_threaded_poll_free(poll_);
    poll_ = nullptr;
    return false;
  }
  avahi_threaded_poll_start(poll_);
  return true;
}

void MdnsPublisher::stop() {
  if (!poll_) return;
  avahi_threaded_poll_stop(poll_);
  if (group_) { avahi_entry_group_free(group_); group_ = nullptr; }
  if (client_) { avahi_client_free(client_); client_ = nullptr; }
  avahi_threaded_poll_free(poll_);
  poll_ = nullptr;
}

void MdnsPublisher::create_service(struct AvahiClient* c) {
  if (!group_) {
    group_ = avahi_entry_group_new(c, &MdnsPublisher::group_cb, this);
    if (!group_) return;
  }
  if (!avahi_entry_group_is_empty(group_)) return;

  // TXT: txtvers=1, protovers=1
  int r = avahi_entry_group_add_service(
      group_, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
      static_cast<AvahiPublishFlags>(0), name_.c_str(), "_oca._tcp", nullptr,
      nullptr, port_, "txtvers=1", "protovers=1", nullptr);
  if (r < 0) {
    if (r == AVAHI_ERR_COLLISION) {
      // 名称冲突:换一个名(AVAHI_CLIENT_S_COLLISION 重连时处理)
    }
    return;
  }
  avahi_entry_group_commit(group_);
}

void MdnsPublisher::group_cb(struct AvahiEntryGroup* g, int state,
                             void* userdata) {
  auto* self = static_cast<MdnsPublisher*>(userdata);
  if (state == AVAHI_ENTRY_GROUP_COLLISION) {
    // 名称冲突:取替代名重发
    char* alt = avahi_alternative_service_name(self->name_.c_str());
    self->name_ = alt;
    avahi_free(alt);
    avahi_entry_group_reset(g);
    self->create_service(avahi_entry_group_get_client(g));
  }
}

void MdnsPublisher::client_cb(struct AvahiClient* c, int state,
                              void* userdata) {
  auto* self = static_cast<MdnsPublisher*>(userdata);
  switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
      self->create_service(c);  // 服务器就绪,发布
      break;
    case AVAHI_CLIENT_S_COLLISION:
    case AVAHI_CLIENT_FAILURE:
      self->group_ = nullptr;  // 重建
      break;
    default:
      break;
  }
}

}  // namespace oca

#endif  // _USE_AVAHI_
```

- [ ] **Step 3: 在 `oca_test.cpp` 加 AVAHI 守卫的冒烟测试**

```cpp
#ifdef _USE_AVAHI_
#include "oca/mdns_publisher.hpp"

BOOST_AUTO_TEST_CASE(mdns_publisher_smoke) {
  // 需要 avahi-daemon 运行才真正发布;此处仅验证 start/stop 不崩溃
  oca::MdnsPublisher pub("aes67-oca-test", 65037);
  BOOST_CHECK(pub.start());
  pub.stop();
  // 重复 stop 不崩溃
  pub.stop();
}
#endif
```

- [ ] **Step 4: 构建并跑测试**

`WITH_AVAHI=OFF`(buildfake 默认)时,该 case 不编译,跳过:

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
make oca-test && ./oca-test -p
```

本地验证 mDNS(需 avahi-daemon 运行):

```bash
cmake -DCPP_HTTPLIB_DIR="../3rdparty/cpp-httplib" \
  -DRAVENNA_ALSA_LKM_DIR="../3rdparty/ravenna-alsa-lkm" \
  -DENABLE_TESTS=ON -DWITH_OCA=ON -DWITH_AVAHI=ON \
  -DFAKE_DRIVER=ON -DWITH_STREAMER=OFF .
make oca-test && ./oca-test -p -t mdns_publisher_smoke
# 另开终端:avahi-browse -r _oca._tcp 应能看到 aes67-oca-test 服务
```

Expected: 冒烟通过;`avahi-browse` 能发现 `_oca._tcp` 服务。

- [ ] **Step 5: Commit**

```bash
git add daemon/oca/mdns_publisher.hpp daemon/oca/mdns_publisher.cpp daemon/oca/tests/oca_test.cpp
git commit -m "feat(oca): L3 Avahi _oca._tcp mDNS 发布器"
```

---

## Task 15: Config 集成(oca_* 字段)

在 `Config` 增加 `oca_enabled/oca_port/oca_device_name/oca_manufacturer/oca_model/oca_serial_number` 字段,打通 JSON 序列化与 `daemon.conf` 默认值。

**Files:**
- Modify: `daemon/config.hpp`(字段 + getter/setter + `operator!=`)
- Modify: `daemon/config.cpp`(`parse` 校验 + `save` 的 daemon_restart 判定)
- Modify: `daemon/json.cpp`(`config_to_json` + `json_to_config`)
- Modify: `daemon/daemon.conf`(默认值)

**Interfaces:**
- Produces:`Config::get_oca_*()` / `set_oca_*()`,JSON 字段 `oca_*`。
- Consumes:无新依赖。

- [ ] **Step 1: 在 `config.hpp` 加私有字段**

在 `config.hpp:244`(`bool auto_sinks_update_{true};` 之后)插入:

```cpp
  bool oca_enabled_{false};
  uint16_t oca_port_{65037};
  std::string oca_device_name_;
  std::string oca_manufacturer_;
  std::string oca_model_;
  std::string oca_serial_number_;
```

- [ ] **Step 2: 在 `config.hpp` 加 getter**

在 `config.hpp:75`(`bool get_auto_sinks_update() …` 之后)插入:

```cpp
  bool get_oca_enabled() const { return oca_enabled_; };
  uint16_t get_oca_port() const { return oca_port_; };
  const std::string& get_oca_device_name() const { return oca_device_name_; };
  const std::string& get_oca_manufacturer() const { return oca_manufacturer_; };
  const std::string& get_oca_model() const { return oca_model_; };
  const std::string& get_oca_serial_number() const {
    return oca_serial_number_;
  };
```

- [ ] **Step 3: 在 `config.hpp` 加 setter**

在 `config.hpp:171`(`set_auto_sinks_update` 之后)插入:

```cpp
  void set_oca_enabled(bool v) { oca_enabled_ = v; };
  void set_oca_port(uint16_t v) { oca_port_ = v; };
  void set_oca_device_name(std::string_view v) { oca_device_name_ = v; };
  void set_oca_manufacturer(std::string_view v) { oca_manufacturer_ = v; };
  void set_oca_model(std::string_view v) { oca_model_ = v; };
  void set_oca_serial_number(std::string_view v) { oca_serial_number_ = v; };
```

- [ ] **Step 4: 在 `config.hpp` 的 `operator!=` 加比较**

在 `config.hpp:205`(`lhs.get_custom_node_id() != rhs.get_custom_node_id();` 之前)插入一行:

```cpp
           lhs.get_oca_enabled() != rhs.get_oca_enabled() ||
           lhs.get_oca_port() != rhs.get_oca_port() ||
           lhs.get_oca_device_name() != rhs.get_oca_device_name() ||
           lhs.get_oca_manufacturer() != rhs.get_oca_manufacturer() ||
           lhs.get_oca_model() != rhs.get_oca_model() ||
           lhs.get_oca_serial_number() != rhs.get_oca_serial_number() ||
```

- [ ] **Step 5: 在 `config.cpp::parse` 加端口校验**

在 `config.cpp:108`(`if (config.ptp_domain_ > 127)` 之前)插入:

```cpp
  if (config.oca_port_ == 0)
    config.oca_port_ = 65037;
```

- [ ] **Step 6: 在 `config.cpp::save` 加 daemon_restart 判定**

在 `config.cpp:185`(`get_streamer_enabled() != config.get_streamer_enabled();` 这一行的 `daemon_restart_ =` 表达式末尾)追加:

```cpp
        get_oca_enabled() != config.get_oca_enabled() ||
        get_oca_port() != config.get_oca_port();
```

(即把这两项并入 `daemon_restart_` 的 `||` 链;`oca_device_name/manufacturer/model/serial_number` 改动不触发重启,仅写盘。)

- [ ] **Step 7: 在 `json.cpp::config_to_json` 序列化 oca_***

在 `json.cpp` 的 `config_to_json` 中,`streamer_enabled` 输出之后(约 `json.cpp:124`)追加:

```cpp
     << ",\n  \"oca_enabled\": " << std::boolalpha << config.get_oca_enabled()
     << ",\n  \"oca_port\": " << config.get_oca_port()
     << ",\n  \"oca_device_name\": \""
     << escape_json(config.get_oca_device_name()) << "\""
     << ",\n  \"oca_manufacturer\": \""
     << escape_json(config.get_oca_manufacturer()) << "\""
     << ",\n  \"oca_model\": \"" << escape_json(config.get_oca_model()) << "\""
     << ",\n  \"oca_serial_number\": \""
     << escape_json(config.get_oca_serial_number()) << "\""
```

- [ ] **Step 8: 在 `json.cpp::json_to_config` 反序列化 oca_***

在 `json.cpp` 的 `json_to_config` 中,`streamer_enabled` 分支之后(约 `json.cpp:328`)追加:

```cpp
      } else if (key == "oca_enabled") {
        config.set_oca_enabled(val.get_value<bool>());
      } else if (key == "oca_port") {
        config.set_oca_port(
            static_cast<uint16_t>(val.get_value<uint32_t>()));
      } else if (key == "oca_device_name") {
        config.set_oca_device_name(val.get_value<std::string>());
      } else if (key == "oca_manufacturer") {
        config.set_oca_manufacturer(val.get_value<std::string>());
      } else if (key == "oca_model") {
        config.set_oca_model(val.get_value<std::string>());
      } else if (key == "oca_serial_number") {
        config.set_oca_serial_number(val.get_value<std::string>());
```

注:确认该 `else if` 链中每个分支以 `}` 结尾、`val` 的类型与周围分支一致(参考同文件 `streamer_enabled`/`custom_node_id` 的写法)。

- [ ] **Step 9: 在 `daemon.conf` 加默认值**

在 `daemon/daemon.conf` 的 `"auto_sinks_update": true` 之后(闭合 `}` 之前)追加:

```json
,
  "oca_enabled": false,
  "oca_port": 65037,
  "oca_device_name": "",
  "oca_manufacturer": "",
  "oca_model": "",
  "oca_serial_number": ""
```

- [ ] **Step 10: 构建守护进程并验证 config 往返**

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
cmake -DCPP_HTTPLIB_DIR="../3rdparty/cpp-httplib" \
  -DRAVENNA_ALSA_LKM_DIR="../3rdparty/ravenna-alsa-lkm" \
  -DENABLE_TESTS=ON -DWITH_OCA=ON \
  -DWITH_AVAHI=OFF -DFAKE_DRIVER=ON -DWITH_STREAMER=OFF .
make aes67-daemon daemon-test
# 跑既有 daemon-test,确认 config 增字段后 HTTP config 往返仍通过
./daemon-test -p
# 手动确认字段出现在 config JSON
./aes67-daemon -c daemon.conf &
sleep 1
curl -s http://127.0.0.1:8080/api/config | grep oca
kill %1
```

Expected:`daemon-test` 通过;`curl … | grep oca` 输出 `oca_enabled`/`oca_port`/`oca_device_name`/`oca_manufacturer`/`oca_model`/`oca_serial_number` 六行。

- [ ] **Step 11: Commit**

```bash
git add daemon/config.hpp daemon/config.cpp daemon/json.cpp daemon/daemon.conf
git commit -m "feat(oca): Config 增加 oca_* 字段与 JSON 序列化"
```

---

## Task 16: main.cpp 接线

`_USE_OCA_` 下,当 `oca_enabled` 为真时实例化 `OcaServer`,从 `Config` + `get_version()` 填充 `OcaServerConfig`,start/stop。

**Files:**
- Modify: `daemon/main.cpp`

**Interfaces:**
- Consumes:`oca::OcaServer`、`oca::OcaServerConfig`、`Config::get_oca_*()`、`Config::get_node_id()`、`Config::get_mdns_enabled()`、`get_version()`。

- [ ] **Step 1: 在 `main.cpp` 加 include**

在 `main.cpp:36`(`#ifdef _USE_STREAMER_` include 块)之后加:

```cpp
#ifdef _USE_OCA_
#include "oca/oca_server.hpp"
#endif
```

- [ ] **Step 2: 在 `main.cpp` 启动 OcaServer**

在 `main.cpp:201`(`if (!http_server.init()) { … }` 块之后,`session_manager->load_status();` 之前)插入:

```cpp
      /* start OCA server */
#ifdef _USE_OCA_
      std::unique_ptr<oca::OcaServer> oca_server;
      if (config->get_oca_enabled()) {
        oca::OcaServerConfig ocacfg;
        ocacfg.port = config->get_oca_port();
        ocacfg.device_name = config->get_oca_device_name();
        ocacfg.manufacturer = config->get_oca_manufacturer();
        ocacfg.model = config->get_oca_model();
        ocacfg.serial_number = config->get_oca_serial_number();
        ocacfg.node_id = config->get_node_id();
        ocacfg.daemon_version = get_version();
        ocacfg.mdns_enabled = config->get_mdns_enabled();
        oca_server = std::make_unique<oca::OcaServer>(ocacfg);
        if (!oca_server->start()) {
          throw std::runtime_error(std::string("OcaServer:: start failed"));
        }
        BOOST_LOG_TRIVIAL(info) << "main:: OCA server listening on port "
                                << oca_server->port();
      }
#else
      (void)0;
#endif
```

- [ ] **Step 3: 在 `main.cpp` 终止 OcaServer**

在 `main.cpp:255`(`if (!http_server.terminate()) { … }` 块之后)插入:

```cpp
      /* stop OCA server */
#ifdef _USE_OCA_
      if (oca_server) {
        oca_server->stop();
      }
#endif
```

- [ ] **Step 4: 构建守护进程并冒烟验证**

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
cmake -DCPP_HTTPLIB_DIR="../3rdparty/cpp-httplib" \
  -DRAVENNA_ALSA_LKM_DIR="../3rdparty/ravenna-alsa-lkm" \
  -DENABLE_TESTS=ON -DWITH_OCA=ON \
  -DWITH_AVAHI=OFF -DFAKE_DRIVER=ON -DWITH_STREAMER=OFF .
make aes67-daemon
```

准备一份 `oca_enabled=true` 的临时配置(基于 `daemon.conf`,把 `"oca_enabled": false` 改 `true`,`interface_name` 保持 `lo`),启动并验证 OCP.1 端口响应 KeepAlive:

```bash
sed 's/"oca_enabled": false/"oca_enabled": true/' daemon.conf > /tmp/oca.conf
./aes67-daemon -c /tmp/oca.conf &
sleep 1
# 发 KeepAlive(5s)并读回应:sync(3B)+header(9)+heartbeat(2)
printf '\x3b\x00\x01\x00\x00\x00\x0b\x04\x00\x01\x00\x05' | nc -q1 127.0.0.1 65037 | xxd | head
kill %1
```

Expected:守护进程日志 `main:: OCA server listening on port 65037`;`nc` 收到 12 字节回应(`3b 00 01 00 00 00 0b 04 00 01 00 05`)。若系统无 `nc`,改用 Task 17 的 CI 客户端验证。

- [ ] **Step 5: Commit**

```bash
git add daemon/main.cpp
git commit -m "feat(oca): main.cpp 接线 OcaServer 启停"
```

---

## Task 17: 验收 - CI E2E 客户端 + 真实控制器清单

自写 CI 客户端(Boost.Test E2E)覆盖完整 Spec1 流程:发现 -> 身份 -> 订阅 -> 事件。这是回归闸门。另附真实控制器手动验收清单。

**Files:**
- Modify: `daemon/oca/tests/oca_test.cpp`

**Interfaces:**
- Consumes:`oca::OcaServer`、`oca::ocp1` 全套。

- [ ] **Step 1: 在 `oca_test.cpp` 加 E2E 验收测试**

```cpp
BOOST_AUTO_TEST_CASE(oca_e2e_acceptance) {
  oca::OcaServerConfig cfg;
  cfg.port = 0;
  cfg.node_id = "AES67 daemon e2e";
  cfg.daemon_version = "bondagit-3.1.0";
  oca::OcaServer server(cfg);
  BOOST_REQUIRE(server.start());
  uint16_t port = server.port();

  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{}; addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  BOOST_REQUIRE(::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

  auto sendPdu = [&](const std::vector<uint8_t>& p) {
    BOOST_REQUIRE_EQUAL(::send(sock, p.data(), p.size(), 0), (ssize_t)p.size());
  };
  auto recvPdu = [&](std::vector<uint8_t>& out) -> bool {
    uint8_t sync; if (::recv(sock, &sync, 1, 0) != 1 || sync != 0x3B) return false;
    uint8_t hdr[9]; size_t got = 0;
    while (got < 9) { ssize_t r = ::recv(sock, hdr+got, 9-got, 0); if (r<=0) return false; got += r; }
    auto h = oca::ocp1::PduReader::try_parse_header(hdr, 9);
    if (!h) return false;
    size_t plen = h->pduSize - 9;
    out.assign(hdr, hdr+9); out.resize(9+plen); got = 0;
    while (got < plen) { ssize_t r = ::recv(sock, out.data()+9+got, plen-got, 0); if (r<=0) return false; got += r; }
    out.insert(out.begin(), 0x3B);
    return true;
  };
  auto cmd = [&](uint32_t handle, oca::ONo target, oca::MethodID mid)
      -> oca::ocp1::Response {
    oca::ocp1::Writer cw;
    oca::ocp1::write_command(cw, handle, target, mid, nullptr, 0);
    sendPdu(oca::ocp1::PduWriter::build_command_pdu(1, cw.data(), cw.size()));
    std::vector<uint8_t> rsp; BOOST_REQUIRE(recvPdu(rsp));
    auto h = oca::ocp1::PduReader::try_parse_header(rsp.data()+1, rsp.size()-1);
    auto rsps = oca::ocp1::PduReader::parse_responses(rsp.data()+1+9,
                    h->pduSize-9, h->messageCount);
    BOOST_REQUIRE_EQUAL(rsps.size(), 1u);
    return rsps[0];
  };

  // 1) KeepAlive
  sendPdu(oca::ocp1::PduWriter::build_keepalive_pdu(5));
  std::vector<uint8_t> ka; BOOST_REQUIRE(recvPdu(ka));

  // 2) 身份:GetOcaVersion=1
  auto r1 = cmd(1, 1, {oca::methods::kDefLevelDeviceMngr,
                       oca::methods::kDevGetOcaVersion});
  BOOST_CHECK(r1.statusCode == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(r1.paramData, r1.paramCount).u16(), 1u);

  // 3) 身份:GetModelDescription -> {mfr, model=version, version}
  auto r2 = cmd(2, 1, {oca::methods::kDefLevelDeviceMngr,
                       oca::methods::kDevGetModelDescription});
  BOOST_CHECK(r2.statusCode == oca::Status::OK);
  {
    oca::ocp1::Reader r(r2.paramData, r2.paramCount);
    BOOST_CHECK_EQUAL(r.string(), "AES67-Linux-Daemon");
    BOOST_CHECK_EQUAL(r.string(), "bondagit-3.1.0");
    BOOST_CHECK_EQUAL(r.string(), "bondagit-3.1.0");
  }

  // 4) 发现:GetMembers(ONo 100) -> [1,2,4]
  auto r3 = cmd(3, 100, {oca::methods::kDefLevelBlock,
                         oca::methods::kBlockGetMembers});
  BOOST_CHECK(r3.statusCode == oca::Status::OK);
  {
    oca::ocp1::Reader r(r3.paramData, r3.paramCount);
    BOOST_CHECK_EQUAL(r.u16(), 3u);
    BOOST_CHECK_EQUAL(r.u32(), 1u);
    BOOST_CHECK_EQUAL(r.u32(), 2u);
    BOOST_CHECK_EQUAL(r.u32(), 4u);
  }

  // 5) 订阅:AddSubscription2(emitter=1, OperationalState)
  oca::ocp1::Writer cw;
  cw.u32(1);  // EmitterONo
  cw.u16(oca::methods::kDefLevelDeviceMngr);
  cw.u16(oca::methods::kEventOperationalState);
  cw.u16(0);  // 空 context
  sendPdu(oca::ocp1::PduWriter::build_command_pdu(1, cw.data(), cw.size()));
  std::vector<uint8_t> rspsub; BOOST_REQUIRE(recvPdu(rspsub));
  auto hs = oca::ocp1::PduReader::try_parse_header(rspsub.data()+1, rspsub.size()-1);
  auto subrsps = oca::ocp1::PduReader::parse_responses(rspsub.data()+1+9,
                       hs->pduSize-9, hs->messageCount);
  BOOST_REQUIRE_EQUAL(subrsps.size(), 1u);
  BOOST_CHECK(subrsps[0].statusCode == oca::Status::OK);
  uint32_t subId = oca::ocp1::Reader(subrsps[0].paramData,
                                     subrsps[0].paramCount).u32();

  // 6) 触发事件,然后发一个 ping 让传输层排空通知队列
  uint8_t evdata = static_cast<uint8_t>(oca::DeviceState::Operational);
  server.subscription_manager()->trigger_event(1,
      {oca::methods::kDefLevelDeviceMngr, oca::methods::kEventOperationalState},
      &evdata, 1);
  cmd(4, 1, {oca::methods::kDefLevelDeviceMngr,
             oca::methods::kDevGetOcaVersion});  // ping -> 触发排空

  // 7) 收 Notification2
  std::vector<uint8_t> ntf; BOOST_REQUIRE(recvPdu(ntf));
  auto hn = oca::ocp1::PduReader::try_parse_header(ntf.data()+1, ntf.size()-1);
  BOOST_CHECK_EQUAL(hn->pduType, oca::methods::kPduNtf2);
  auto ntfs = oca::ocp1::PduReader::parse_notifications2(ntf.data()+1+9,
                       hn->pduSize-9, hn->messageCount);
  BOOST_REQUIRE_EQUAL(ntfs.size(), 1u);
  BOOST_CHECK_EQUAL(ntfs[0].emitterONo, 1u);
  BOOST_CHECK_EQUAL(ntfs[0].eventID.eventIndex,
                    oca::methods::kEventOperationalState);
  BOOST_CHECK_EQUAL(ntfs[0].dataCount, 1u);
  BOOST_CHECK_EQUAL(ntfs[0].data[0], evdata);

  ::close(sock);
  server.stop();
}
```

- [ ] **Step 2: 跑全部 OCA 测试**

```bash
cd /home/Share/GitHub/aes67-linux-daemon/daemon
make oca-test && ./oca-test -p
```

Expected:全部 case 通过(含 `oca_e2e_acceptance`)。

- [ ] **Step 3: 真实控制器手动验收清单**

在装有 AES70 控制器(如 OCA Alliance 参考工具 `ocaMicron` / `OCaPAS`,或商业控制器)与本机 `avahi-daemon` 的环境:

1. 用 `WITH_OCA=ON -DWITH_AVAHI=ON` 构建 daemon,`daemon.conf` 设 `oca_enabled=true`、`oca_port=65037`、`interface_name=<真实网卡>`。
2. 启动 daemon,确认日志 `main:: OCA server listening on port 65037`。
3. 在控制器端浏览 mDNS,应发现 `_oca._tcp` 服务,实例名=`node_id`。
4. 控制器连接后,浏览对象树:Root Block(ONo 100)`GetMembers` 应返回 ONo 1/2/4。
5. 查 DeviceManager(ONo 1)身份:`GetOcaVersion`=1、`GetDeviceName`、`GetModelDescription`。
6. 订阅 DeviceManager.OperationalState 事件(AddSubscription2)。
7. 若 `AddSubscription2` 返回 `NotImplemented`/`BadMethod`:EV2 方法索引候选错误,按 Task 2 Step 5 校验 `methods.hpp` 中 `kSubAddSubscription2` 等并修正单行常量后重测。
8. (Spec1 演示)触发事件应收到 OperationalState 通知;Spec2 接入 PTP 状态后此通道复用。

- [ ] **Step 4: Commit**

```bash
git add daemon/oca/tests/oca_test.cpp
git commit -m "test(oca): Spec1 E2E 验收(发现/身份/订阅/事件)与真实控制器清单"
```

---

## Self-Review

**1. 规格覆盖:** 逐条对照 spec §A-§D:
- L0 类型(types.hpp):Task 2 ✓
- L1 编解码 + PDU 分帧:Task 3/4/5 ✓
- L2 Object + Registry:Task 6 ✓;Session:Task 7 ✓;4 管理器 + Root Block:Task 8/9/10/11 ✓
- L3 TCP 传输 + KeepAlive + 分派:Task 12 ✓;OcaServer 门面:Task 13 ✓;mDNS:Task 14 ✓
- Config 集成(6 字段):Task 15 ✓;CMake WITH_OCA:Task 1 ✓;main 接线:Task 16 ✓
- 测试策略(L0/L1/L2 单测 + L3 集成 + 真实控制器):Task 2-5/8-13/17 ✓
- 明确不在范围(EV1/TLS/UDP/WebSocket/Dataset/媒体类):未实现,符合 ✓
- 勘误落地:OcaStatus 真值(Task 2)、DeviceManager 真索引(Task 9)、GetModelDescription 替代 GetProduct/GetManufacturer(Task 9)、GetMembers 替代 GetControlObjects(Task 8)、Notification2 含 Data(Task 5)、EV2 索引 XMI 关卡(Task 2/11)。

**2. 占位符扫描:** 无 TBD/TODO/"implement later"。EV2 订阅索引与 GetNetworks 索引为候选值,均落在 `methods.hpp` 命名常量,并有明确的 XMI 校验关卡(Task 2 Step 5)与真实控制器验收(Task 17 Step 3.7)兜底——这是合法的"查规范常量"任务,非占位。

**3. 类型一致性:**
- `Object::exec(MethodID, ocp1::Reader&, ocp1::Writer&, Session&)` 全链路一致(Task 6 定义,Task 8/9/10/11 复用)。
- `OcaServerConfig` 字段在 Task 13 定义、Task 16 消费,字段名一致(`port/device_name/manufacturer/model/serial_number/node_id/daemon_version/mdns_enabled`)。
- `OcaDeviceIdentity` 字段在 Task 9 定义、Task 13 填充,一致。
- `methods::k*` 常量在 Task 2 定义,Task 8/9/10/11/12/17 引用名一致。
- `PduWriter::build_*_pdu` / `write_command/write_response/write_notification2` 在 Task 5 定义,Task 11/12/17 引用一致。
- `Session` API(`add_subscription/remove_subscription/has_subscription/enqueue_notification/take_notification/set_heartbeat/touch/expired/registry`)Task 7 定义,Task 11/12 引用一致。

**已知限制(非阻塞,记入 Spec2/后续):**
- 响应参数受 `paramCount`(u8,≤255 字节)限制;Spec1 方法响应均远小于 255。
- 通知投递依赖传输层读循环排空(命令后或 1s 空闲);Spec2 可加独立写线程做及时推送。
- EV2 订阅/GetNetworks 索引为候选,需 XMI 校验(真实控制器兜底)。
- OcaBlock.GetControlObjects 未实现(用 GetMembers 替代),列入 Spec1 收尾。
- `oca_*` 配置改动不支持热加载(需 daemon 重启)。

---

## 执行交接

计划已保存到 `docs/superpowers/plans/aes70-oca-spec1-plan.md`。两种执行方式:

**1. Subagent 驱动(推荐)** - 每个 Task 派一个全新 subagent 实现,任务间做两阶段 review,迭代快。

**2. 内联执行** - 在当前会话用 executing-plans 批量执行,带检查点 review。

请选择执行方式。


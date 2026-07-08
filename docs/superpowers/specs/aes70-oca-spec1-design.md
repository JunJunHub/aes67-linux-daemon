# AES70/OCA Spec1 设计文档 — 协议栈基础

> 版本: 2026-07-08
> 范围: Spec1 = 通用 OCA 协议栈基础(类型/编解码/对象模型/TCP 传输/mDNS 发现/核心管理器)
> 产出: 任意 OCA 控制器能经 mDNS 发现 daemon、TCP 连接、浏览对象树、查设备身份、订阅事件
> 基于: AES70-1/2/3-2023 官方规范; ocac(C99)仅作参考
> 下一步: Spec2 = AES67 媒体桥接(MediaClock/Transport/StreamConnector + SessionManager 双向桥接)

---

## 决策记录

| # | 决策 | 选定 | 理由 |
|---|------|------|------|
| 1 | Spec 划分 | 2 个 spec | Spec1 协议基础 + Spec2 媒体桥接,隔离控制面与媒体面 |
| 2 | 整体方案 | 分层 C++ 栈(方案1) | 虚拟类继承+流式编解码,in-tree;ocac 仅作参考;XMI 代码生成记为 Spec2 后战略选项 |
| 3 | 传输层 | 仅 TCP | Linux daemon 自然选择;UDP 面向小设备;TLS 加 OpenSSL;WebSocket 面向浏览器;均可后续追加 |
| 4 | mDNS | 复用 Avahi,OCA 独立发布器 | 与 daemon MDNSServer(Ravenna 专用)解耦;随 WITH_AVAHI 开关 |
| 5 | 验收基准 | 真实控制器 + 自写 CI 客户端 | 真实控制器逼出 spec 偏差;CI 客户端保证回归 |
| 6 | IO 模型 | 独立线程 + BSD socket | 每连接一线程;零新依赖;匹配 daemon 风格(cpp-httplib 同步 + worker 线程) |
| 7 | 通知版本 | EV2 only | EV1 deprecated in 2023;新控制器走 EV2 |
| 8 | 协议版本 | ProtocolVersion=1 | AES70-2023 规定 |

---

## §A 架构总览与模块布局

### 分层(自底向上)

```
┌───────────────────────────────────────────────────────────┐
│  daemon main.cpp  ──WITH_OCA──►  OcaServer (门面)         │
│                                  持有 Config(身份/网络)    │
│ ┌───────────────────────────────────────────────────────┐ │
│ │ L3 传输层   transport.*  TCP accept + per-conn 读线程  │ │
│ │             mdns_publisher.*  Avahi 发 _oca._tcp      │ │
│ ├───────────────────────────────────────────────────────┤ │
│ │ L2 对象层   object.hpp  OcaObject 抽象 + ObjectRegistry│ │
│ │             classes/*.hpp  Root/Device/Network/Sub    │ │
│ ├───────────────────────────────────────────────────────┤ │
│ │ L1 编解码   ocp1.*  OcpReader/OcpWriter + PDU 分帧     │ │
│ ├───────────────────────────────────────────────────────┤ │
│ │ L0 类型     types.hpp  OCC 数据类型(纯头文件)           │ │
│ └───────────────────────────────────────────────────────┘ │
└───────────────────────────────────────────────────────────┘
        Spec1 不依赖 SessionManager(媒体桥接是 Spec2)
        仅依赖 Config → FAKE_DRIVER 下可独立构建/测试
```

### 模块布局

```
daemon/oca/
  types.hpp                  L0 OCC 类型(纯头)
  ocp1.hpp / ocp1.cpp        L1 reader/writer + PDU 分帧
  object.hpp                 L2 OcaObject 基类 + ObjectRegistry
  classes/
    root.hpp
    device_manager.hpp/.cpp
    network_manager.hpp/.cpp
    subscription_manager.hpp/.cpp
  transport.hpp/.cpp         L3 TCP 服务端 + Session
  mdns_publisher.hpp/.cpp    L3 Avahi _oca._tcp 发布
  oca_server.hpp/.cpp        门面:持有 registry+transport+mdns,构造取 Config
  tests/                     Boost.Test(与 daemon/tests 一致)
```

### 命令数据流(以 GetIdentification 为例)

1. 连接读线程:读 sync `0x3B` + 9 字节头 + `pduSize` 载荷
2. 按 `messageCount` 拆出每条 command:`{handle, targetONo, methodID, params}`
3. `ObjectRegistry::find(targetONo)` → `OcaObject*`
4. `obj->exec(methodID, OcpReader, OcpWriter, session)` → `Status`
5. 包成 response PDU `{handle, status, params}`,per-连接写锁回写
6. Notification 通路:订阅触发 → 编码 notification → 投递到订阅会话的写队列

### 并发模型

- `OcaServer` 自有线程跑 accept 循环;每连接一个读线程
- 对象 `exec` 大多读 `Config`(身份/网络),无竞争;Spec2 才调 `SessionManager`(其互斥锁保护)
- Notification 推送:per-连接写锁 + 投递队列。Spec1 先确立机制(用 DeviceManager 状态变化做演示事件),Spec2 接媒体事件时复用

### 关键隔离决策

**Spec1 的 `OcaServer` 只依赖 `Config`,不依赖 `SessionManager`。** 设备身份(厂商/型号/序列号/版本)和网络信息都从 `Config` + `interface` 取。Spec1 在 `FAKE_DRIVER` 下完全可构建可测试,媒体面耦合推迟到 Spec2。

---

## §B L0 OCC 类型 + L1 OCP.1 编解码

### L0: OCC 数据类型(`types.hpp`)

纯头文件,零依赖。对照 AES70-3-2023 §8.2.3 Table 8:

```cpp
namespace oca {

// --- 基础标量 (spec Table 8) ---
using Boolean = uint8_t;   // 0=FALSE, else TRUE
using Int8 = int8_t; using Int16 = int16_t; using Int32 = int32_t; using Int64 = int64_t;
using Uint8 = uint8_t; using Uint16 = uint16_t; using Uint32 = uint32_t; using Uint64 = uint64_t;
using Float32 = float; using Float64 = double;

// --- OCA 标识类型 ---
using ONo = uint32_t;

struct ClassID { std::vector<uint16_t> levels; }; // 多级,如 {1,2,15}

struct MethodID   { uint16_t defLevel; uint16_t methodIndex; };
struct EventID    { uint16_t defLevel; uint16_t eventIndex; };
struct PropertyID { uint16_t defLevel; uint16_t propertyIndex; };

struct ClassIdentification { ClassID classID; uint16_t classVersion; };

// --- OcaStatus (AES70-2A) ---
enum class Status : uint8_t {
  OK = 0, BadFormat, NoSuchMethod, NotImplemented,
  InvalidRequest, OutOfRange, PropertyNotSupported,
  // ... 完整枚举从 AES70-2A 补全
};

// --- 容器(编码格式,非 C++ 内存表示) ---
// Ocp1List = u16 count + items; Ocp1LongList = u32 count + items
// 由 OcpWriter 的 list_begin/list_end 编码;C++ 侧用 std::vector<T>

using OcaString = std::string;       // UTF-8,编码为 Ocp1List<Utf8CodePoint>
using OcaBlob   = std::vector<uint8_t>; // 编码为 Ocp1List<u8>

struct OcaBitstring { uint16_t numBits; std::vector<uint8_t> bytes; };

template<typename K, typename V> using OcaMap = std::vector<std::pair<K,V>>;
struct OcaVariant { uint16_t selector; /* data per selector */ };

} // namespace oca
```

要点:
- `ClassID` 用 `vector<uint16_t>` — AES70 类层级深度不固定
- `OcaString` = `std::string`(UTF-8);编码时按 spec 转成 `Ocp1List<Utf8CodePoint>`(count = 码点数,非字节数)
- 容器类型不定义 C++ struct — 它们是编码格式,由 Writer 在序列化时写 count 前缀

### L1: OCP.1 编解码(`ocp1.hpp`/`ocp1.cpp`)

**核心:stream reader/writer,逐字段读写,内部处理 big-endian + 边界检查。** 不用 packed struct + 指针强转(对齐/UB 风险)。

```cpp
namespace oca::ocp1 {

class Reader {
 public:
  Reader(const uint8_t* data, size_t len);
  uint8_t  u8();  uint16_t u16();  uint32_t u32();  uint64_t u64();
  int8_t   i8();  int16_t i16();   int32_t i32();   int64_t i64();
  float    f32(); double  f64();
  std::string string();           // Ocp1List<Utf8CodePoint>
  std::vector<uint8_t> blob();    // Ocp1List<u8>
  OcaBitstring bitstring();
  template<typename T> std::vector<T> list(std::function<T(Reader&)> read_item);
  size_t remaining() const;
 private:
  const uint8_t *p_, *end_;
};

class Writer {
 public:
  Writer();
  void u8(uint8_t);  void u16(uint16_t);  void u32(uint32_t);  void u64(uint64_t);
  void i8(int8_t);   void i16(int16_t);   void i32(int32_t);   void i64(int64_t);
  void f32(float);   void f64(double);
  void string(const std::string& utf8);
  void blob(const uint8_t* data, size_t len);
  void bitstring(const OcaBitstring&);
  template<typename T> void list(const std::vector<T>&, std::function<void(Writer&,const T&)>);
  const uint8_t* data() const;  size_t size() const;
 private:
  std::vector<uint8_t> buf_;
};

// --- PDU 帧结构 (AES70-3-2023 §9) ---
struct Header {
  uint16_t protocolVersion = 1;  // 2023 = 1
  uint32_t pduSize = 0;
  uint8_t  pduType = 0;          // Ocp1MessageType
  uint16_t messageCount = 0;
};

struct Command {
  uint32_t commandSize = 0;
  uint32_t handle = 0;
  ONo      targetONo = 0;
  MethodID methodID{};
  const uint8_t* paramData = nullptr;
  uint8_t  paramCount = 0;
};

struct Response {
  uint32_t responseSize = 0;
  uint32_t handle = 0;
  Status   statusCode = Status::OK;
};

struct Notification2 {  // EV2 (AES70-3-2023 §9.4.4)
  uint32_t notificationSize = 0;
  ONo      emitterONo = 0;
  EventID  eventID{};
  uint8_t  notificationType = 0;  // 0=event, 1=exception
};

// --- PDU 分帧 ---
struct PduReader {
  static std::optional<Header> try_parse_header(const uint8_t* buf, size_t len);
  static std::vector<Command>  parse_commands(const uint8_t* data, size_t len, uint16_t count);
  // parse_responses, parse_notifications2 ...
};

struct PduWriter {
  static std::vector<uint8_t> build_command_pdu(uint16_t msgCount, const uint8_t* cmds, size_t len);
  static std::vector<uint8_t> build_response_pdu(uint16_t msgCount, const uint8_t* rsps, size_t len);
  static std::vector<uint8_t> build_notification2_pdu(uint16_t msgCount, const uint8_t* ntfs, size_t len);
  static std::vector<uint8_t> build_keepalive_pdu(uint16_t heartbeatTimeSec);
};

} // namespace oca::ocp1
```

关键设计决策:
1. **Reader/Writer 不感知 PDU 帧边界** — 只管逐字段读/写。帧处理由 PduReader/PduWriter + 传输层完成。Reader/Writer 可独立单测。
2. **Command 参数延迟解码** — `Command.paramData` 只是指针+count,实际参数由 `OcaObject::exec()` 按方法签名用 Reader 解码。编解码层不感知方法签名 — 分层隔离。
3. **OcaString 按码点计数编码** — count = 码点数(非字节数),严格遵循 spec Table 8。
4. **EV2 only** — `Notification2` 对应 2023 spec §9.4.4;EV1 deprecated 不实现。
5. **ProtocolVersion = 1** — 硬编码为 AES70-2023 版本号。

---

## §C 对象模型、调度与 Spec1 对象树

### 类继承层次(照 AES70-2 Appendix A)

```
OcaObject (抽象基类,纯C++设施)
  └─ OcaRoot          ClassID={1,1}    v2
      ├─ OcaWorker     ClassID={1,1,1}  v2
      │   └─ OcaBlock  ClassID={1,1,3}  v2   (Root Block, ONo=100)
      └─ OcaManager    ClassID={1,2}    v2
          ├─ OcaDeviceManager        ClassID={1,2,1}  v4   ONo=1
          ├─ OcaNetworkManager       ClassID={1,2,3}  v3   ONo=2
          └─ OcaSubscriptionManager  ClassID={1,2,4}  v2   ONo=4
```

### OcaObject 抽象基类 + 分派机制

```cpp
namespace oca {

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

} // namespace oca
```

分派链(以 OcaDeviceManager 为例):

```cpp
Status OcaDeviceManager::exec(MethodID m, Reader& req, Writer& rsp, Session& s) {
  if (m.defLevel == kMyDefLevel) {  // kMyDefLevel=3
    return handle_method(m.methodIndex, req, rsp, s);
  }
  return OcaManager::exec(m, req, rsp, s);
}

Status OcaDeviceManager::handle_method(uint16_t idx, Reader& req, Writer& rsp, Session& s) {
  switch (idx) {
    case 1: return GetOcaVersion(req, rsp, s);
    case 2: return GetSerialNumber(req, rsp, s);
    case 3: return GetDeviceName(req, rsp, s);
    case 4: return GetManagers(req, rsp, s);
    case 5: return GetOperationalState(req, rsp, s);
    case 6: return GetProduct(req, rsp, s);
    case 7: return GetManufacturer(req, rsp, s);
    default: return Status::NotImplemented;
  }
}
```

为什么"虚继承 + switch"而非每方法一虚函数:
- MethodID.DefLevel 不映射到 C++ vtable
- switch 按 MethodIndex 分派与 spec 方法编号一一对应,调试可直接读
- 非 defLevel 请求自动沿继承链向上委托

### ObjectRegistry

```cpp
class ObjectRegistry {
 public:
  void register_object(std::unique_ptr<Object> obj);
  Object* find(ONo ono) const;
  std::vector<Object*> objects_in_range(ONo from, ONo to) const;
 private:
  std::unordered_map<ONo, std::unique_ptr<Object>> objects_;
};
```

### Spec1 对象树

```
ONo 1    OcaDeviceManager       GetOcaVersion/GetSerialNumber/GetDeviceName/GetManagers/...
ONo 2    OcaNetworkManager      GetNetworks(返回空列表)
ONo 4    OcaSubscriptionManager AddSubscription2/RemoveSubscription2/...
ONo 100  OcaBlock (Root Block)  GetControlObjects(返回 ONo 1,2,4)
```

ONo 分配约定:
- 1~99: 标准管理器(固定,与 spec Appendix B 对齐)
- 100: Root Block
- 101~999: 预留 Spec2 网络/媒体对象
- ≥1000: 动态对象(Spec2 的 Source/Sink connector)

### OcaDeviceManager 数据来源(从 Config,不碰 SessionManager)

| 方法 | 返回值来源 |
|------|-----------|
| GetOcaVersion | 硬编码 `1` (AES70-2023) |
| GetSerialNumber | Config `oca_serial_number`(空则取 `node_id`) |
| GetDeviceName | Config `oca_device_name`(空则取 `node_id`) |
| GetManagers | ObjectRegistry 枚举 ONo 1~99 |
| GetOperationalState | `OcaDeviceState::Normal` (Spec1 总是正常) |
| GetProduct | Config `oca_model`(空则取 daemon 版本号) |
| GetManufacturer | Config `oca_manufacturer`(空则取 `"AES67-Linux-Daemon"`) |

### OcaSubscriptionManager — 完整 EV2 实现

```cpp
struct Subscription2 {
  ONo       emitterONo;
  EventID   eventID;
  OcaBlob   subscriberContext;
};
```

AddSubscription2 流程:记录订阅 → 在 emitter 对象注册 event callback → 属性变化时触发 → 遍历订阅者 → 编码 Notification2 PDU → 投递到 Session 写队列。

Spec1 演示事件:OcaDeviceManager.OperationalState 变化(总 Normal,但机制走通;Spec2 接 PTP 状态变化复用)。

### Session(每 TCP 连接一个)

```cpp
class Session {
 public:
  ONo session_id() const;
  void add_subscription(const Subscription2& sub);
  void remove_subscription(ONo emitter, EventID event);
  void enqueue_notification(std::vector<uint8_t> ntfPdu); // 线程安全
 private:
  std::vector<Subscription2> subscriptions_;
  std::mutex write_mutex_;
  std::deque<std::vector<uint8_t>> write_queue_;
};
```

---

## §D 传输层、mDNS、构建集成、测试

### TCP 传输详细设计

**PDU 分帧(TCP 字节流上):**
1. 找 sync `0x3B`,丢弃前面垃圾
2. 读 9 字节 header(ProtocolVersion:2 + PduSize:4 + PduType:1 + MessageCount:2)
3. 读 PduSize 字节 data(不含 sync,含 header)
4. 缓冲区不足则 break 等下次 recv 补齐
5. 完整 PDU → 按 PduType 分派;剩余字节留在缓冲区

**KeepAlive(心跳):**
- 控制器建立连接后必须先发 KeepAlive(含 HeartbeatTime 秒数)
- 设备侧:超时 `3 × HeartbeatTime` 未收到任何消息 → 关闭连接
- 设备侧:每 HeartbeatTime 保证发一条消息(KeepAlive 或其他)
- PDU: sync + header(PduType=4) + HeartbeatTime(u16秒/u32毫秒,由长度决定)

**连接生命周期:** TCP connect → 收到 KeepAlive → 建立 Session → 交互 → (超时/错误 → 清理订阅 → 关闭)

### mDNS 发布

发布 `_oca._tcp`,TXT: `txtvers=1`, `protovers=1`。实例名取 `node_id`。端口取 `oca_port`。

构建条件: `WITH_OCA && WITH_AVAHI` 都开才编译。WITH_OCA 开但 WITH_AVAHI 关 → TCP 照跑,mDNS 不发。

### Config 集成

新增 `daemon.conf` 字段:

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `oca_enabled` | `false` | 运行时开关 |
| `oca_port` | `65037` | OCP.1 TCP 端口 |
| `oca_device_name` | `""` | 空=取 node_id |
| `oca_manufacturer` | `""` | 空=`"AES67-Linux-Daemon"` |
| `oca_model` | `""` | 空=取 daemon 版本号 |
| `oca_serial_number` | `""` | 空=取 node_id |

### CMake 构建集成

```cmake
option(WITH_OCA "AES70/OCA device control surface" OFF)

if(WITH_OCA)
  add_definitions(-D_USE_OCA_)
  list(APPEND SOURCES
    oca/ocp1.cpp
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

无新外部依赖。与 FAKE_DRIVER 完全兼容。

### 测试策略

**L0+L1 编解码(Boost.Test,CI):**
- roundtrip: 所有标量/string/blob/list/bitstring 往返
- OcaString 非 ASCII(中文/emoji)验证码点计数
- fuzz: 随机字节 → Reader 不 crash
- PDU command/response 往返
- ocac 黄金样本比对

**L2 对象分派(Boost.Test,CI):**
- dispatch_root_getlockable, dispatch_device_getocaversion, dispatch_device_getmanagers
- dispatch_unknown_ono → NoSuchObject
- dispatch_unknown_method → NotImplemented

**L3 传输集成(需 TCP):**
- connect_keepalive, command_response, subscription_notify

**真实控制器验收(手动):**
- mDNS 发现 → 连接 → 浏览对象树 → 查身份 → 订阅事件

### 明确不在 Spec1 范围内

| 事项 | 归属 |
|------|------|
| OcaMediaClockNetwork3 (PTP) | Spec2 |
| OcaMediaTransportNetwork3 | Spec2 |
| OcaStreamConnector4 (Source/Sink) | Spec2 |
| OcaMatrix (声道矩阵) | Spec2 |
| SessionManager 双向桥接 | Spec2 |
| 双控制面事件去环 | Spec2 |
| TLS 传输 | 后续 |
| UDP / WebSocket 传输 | 后续 |
| EV1 通知 | 不实现(deprecated) |
| Dataset / Patch | 后续 |
| OcaNetwork 对象(IP 接口) | Spec2 |
| ST-2022-7 冗余 OCA 连接 | 后续 |
| ONo 持久化 | 后续 |

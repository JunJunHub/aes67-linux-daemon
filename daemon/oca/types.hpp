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

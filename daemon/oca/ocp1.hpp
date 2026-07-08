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

  uint8_t u8();
  uint16_t u16();
  uint32_t u32();
  uint64_t u64();
  int8_t i8();
  int16_t i16();
  int32_t i32();
  int64_t i64();
  float f32();
  double f64();

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
  void blob(const std::vector<uint8_t>& data) {
    blob(data.data(), data.size());
  }
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
  uint32_t pduSize = 0;  // 不含 SyncVal,含 Header + data
  uint8_t pduType = 0;   // methods::kPdu*
  uint16_t messageCount = 0;
};

struct Command {
  uint32_t commandSize = 0;
  uint32_t handle = 0;
  ONo targetONo = 0;
  MethodID methodID{};
  const uint8_t* paramData = nullptr;
  uint8_t paramCount = 0;  // Ocp1Parameters.ParameterCount
};

struct Response {
  uint32_t responseSize = 0;
  uint32_t handle = 0;
  Status statusCode = Status::OK;
  const uint8_t* paramData = nullptr;
  uint8_t paramCount = 0;
};

struct Notification2 {
  uint32_t notificationSize = 0;
  ONo emitterONo = 0;
  EventID eventID{};
  uint8_t notificationType = 0;  // 0=Event, 1=Exception
  const uint8_t* data = nullptr;
  uint32_t dataCount = 0;
};

struct PduReader {
  static std::optional<Header> try_parse_header(const uint8_t* buf, size_t len);
  static std::vector<Command> parse_commands(const uint8_t* data,
                                             size_t len,
                                             uint16_t count);
  static std::vector<Response> parse_responses(const uint8_t* data,
                                               size_t len,
                                               uint16_t count);
  static std::vector<Notification2> parse_notifications2(const uint8_t* data,
                                                         size_t len,
                                                         uint16_t count);
};

struct PduWriter {
  // cmds/rsps/ntfs 是已序列化好的消息字节(含各自 size 前缀)
  static std::vector<uint8_t> build_command_pdu(uint16_t msgCount,
                                                const uint8_t* cmds,
                                                size_t len);
  static std::vector<uint8_t> build_response_pdu(uint16_t msgCount,
                                                 const uint8_t* rsps,
                                                 size_t len);
  static std::vector<uint8_t> build_notification2_pdu(uint16_t msgCount,
                                                      const uint8_t* ntfs,
                                                      size_t len);
  static std::vector<uint8_t> build_keepalive_pdu(uint16_t heartbeatTimeSec);
};

// 单条消息序列化(不含 sync/header,写入 Writer)
void write_command(Writer& w,
                   uint32_t handle,
                   ONo targetONo,
                   MethodID methodID,
                   const uint8_t* params,
                   uint8_t paramCount);
void write_response(Writer& w,
                    uint32_t handle,
                    Status status,
                    const uint8_t* params,
                    uint8_t paramCount);
void write_notification2(Writer& w,
                         ONo emitterONo,
                         EventID eventID,
                         uint8_t notificationType,
                         const uint8_t* data,
                         uint16_t dataCount);

// 模板方法定义(放头文件)
template <typename T>
std::vector<T> Reader::list(std::function<T(Reader&)> read_item) {
  uint16_t count = u16();
  std::vector<T> out;
  out.reserve(count);
  for (uint16_t i = 0; i < count; ++i)
    out.push_back(read_item(*this));
  return out;
}

template <typename T>
void Writer::list(const std::vector<T>& items,
                  std::function<void(Writer&, const T&)> write_item) {
  u16(static_cast<uint16_t>(items.size()));
  for (const auto& it : items)
    write_item(*this, it);
}

}  // namespace oca::ocp1

#endif  // OCA_OCP1_HPP_

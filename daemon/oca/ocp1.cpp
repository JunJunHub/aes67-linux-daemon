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

uint8_t Reader::u8() {
  check(1);
  return *p_++;
}
uint16_t Reader::u16() {
  check(2);
  uint16_t v = (uint16_t(p_[0]) << 8) | p_[1];
  p_ += 2;
  return v;
}
uint32_t Reader::u32() {
  check(4);
  uint32_t v = (uint32_t(p_[0]) << 24) | (uint32_t(p_[1]) << 16) |
               (uint32_t(p_[2]) << 8) | p_[3];
  p_ += 4;
  return v;
}
uint64_t Reader::u64() {
  check(8);
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v = (v << 8) | p_[i];
  p_ += 8;
  return v;
}

int8_t Reader::i8() {
  return static_cast<int8_t>(u8());
}
int16_t Reader::i16() {
  return static_cast<int16_t>(u16());
}
int32_t Reader::i32() {
  return static_cast<int32_t>(u32());
}
int64_t Reader::i64() {
  return static_cast<int64_t>(u64());
}

float Reader::f32() {
  uint32_t bits = u32();
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
double Reader::f64() {
  uint64_t bits = u64();
  double d;
  std::memcpy(&d, &bits, 8);
  return d;
}

std::string Reader::string() {
  uint16_t count = u16();  // 码点数
  std::string out;
  for (uint16_t i = 0; i < count; ++i) {
    check(1);
    unsigned char c = *p_++;
    out.push_back(static_cast<char>(c));
    int extra = (c < 0x80)           ? 0
                : ((c >> 5) == 0x6)  ? 1  // 110xxxxx
                : ((c >> 4) == 0xE)  ? 2  // 1110xxxx
                : ((c >> 3) == 0x1E) ? 3  // 11110xxx
                                     : 0;
    check(extra);
    for (int j = 0; j < extra; ++j)
      out.push_back(static_cast<char>(*p_++));
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
  uint16_t nbytes = static_cast<uint16_t>((bs.numBits + 7) / 8);
  check(nbytes);
  bs.bytes.assign(p_, p_ + nbytes);
  p_ += nbytes;
  return bs;
}

// ---------- Writer ----------
void Writer::u8(uint8_t v) {
  buf_.push_back(v);
}
void Writer::u16(uint16_t v) {
  buf_.push_back(v >> 8);
  buf_.push_back(v & 0xff);
}
void Writer::u32(uint32_t v) {
  for (int i = 3; i >= 0; --i)
    buf_.push_back((v >> (i * 8)) & 0xff);
}
void Writer::u64(uint64_t v) {
  for (int i = 7; i >= 0; --i)
    buf_.push_back((v >> (i * 8)) & 0xff);
}

void Writer::i8(int8_t v) {
  u8(static_cast<uint8_t>(v));
}
void Writer::i16(int16_t v) {
  u16(static_cast<uint16_t>(v));
}
void Writer::i32(int32_t v) {
  u32(static_cast<uint32_t>(v));
}
void Writer::i64(int64_t v) {
  u64(static_cast<uint64_t>(v));
}

void Writer::f32(float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  u32(bits);
}
void Writer::f64(double v) {
  uint64_t bits;
  std::memcpy(&bits, &v, 8);
  u64(bits);
}

void Writer::string(const std::string& utf8) {
  size_t i = 0, count = 0;
  while (i < utf8.size()) {
    unsigned char c = static_cast<unsigned char>(utf8[i]);
    i += (c < 0x80)           ? 1
         : ((c >> 5) == 0x6)  ? 2
         : ((c >> 4) == 0xE)  ? 3
         : ((c >> 3) == 0x1E) ? 4
                              : 1;
    ++count;
  }
  u16(static_cast<uint16_t>(count));  // 码点计数
  buf_.insert(buf_.end(), utf8.begin(), utf8.end());
}

void Writer::blob(const uint8_t* data, size_t len) {
  u16(static_cast<uint16_t>(len));
  if (len)
    buf_.insert(buf_.end(), data, data + len);
}

void Writer::bitstring(const OcaBitstring& bs) {
  u16(bs.numBits);
  uint16_t nbytes = static_cast<uint16_t>((bs.numBits + 7) / 8);
  if (nbytes)
    buf_.insert(buf_.end(), bs.bytes.begin(), bs.bytes.begin() + nbytes);
}

// ---------- 单条消息序列化 ----------
void write_command(Writer& w,
                   uint32_t handle,
                   ONo targetONo,
                   MethodID methodID,
                   const uint8_t* params,
                   uint32_t paramBytes,
                   uint8_t nrParameters) {
  // commandSize = 4+4+4+4+1 + paramBytes = 17 + paramBytes
  uint32_t cmdSize = 17u + paramBytes;
  w.u32(cmdSize);
  w.u32(handle);
  w.u32(targetONo);
  w.u16(methodID.defLevel);
  w.u16(methodID.methodIndex);
  w.u8(nrParameters);  // NrParameters = 参数个数(AES70-3 §9)
  for (uint32_t i = 0; i < paramBytes; ++i)
    w.u8(params[i]);
}

void write_response(Writer& w,
                    uint32_t handle,
                    Status status,
                    const uint8_t* params,
                    uint32_t paramBytes,
                    uint8_t nrParameters) {
  // responseSize = 4+4+1+1 + paramBytes = 10 + paramBytes
  uint32_t rspSize = 10u + paramBytes;
  w.u32(rspSize);
  w.u32(handle);
  w.u8(static_cast<uint8_t>(status));
  w.u8(nrParameters);  // NrParameters = 参数个数(AES70-3 §9)
  for (uint32_t i = 0; i < paramBytes; ++i)
    w.u8(params[i]);
}

void write_notification2(Writer& w,
                         ONo emitterONo,
                         EventID eventID,
                         uint8_t notificationType,
                         const uint8_t* data,
                         uint16_t dataCount) {
  // notificationSize = 4+4+4+1+2 + dataCount = 15 + dataCount
  uint32_t ntfSize = 15u + dataCount;
  w.u32(ntfSize);
  w.u32(emitterONo);
  w.u16(eventID.defLevel);
  w.u16(eventID.eventIndex);
  w.u8(notificationType);
  w.u16(dataCount);
  for (uint16_t i = 0; i < dataCount; ++i)
    w.u8(data[i]);
}

// ---------- PduReader ----------
std::optional<Header> PduReader::try_parse_header(const uint8_t* buf,
                                                  size_t len) {
  if (len < 9)
    return std::nullopt;
  Reader r(buf, 9);
  Header h;
  h.protocolVersion = r.u16();
  h.pduSize = r.u32();
  h.pduType = r.u8();
  h.messageCount = r.u16();
  return h;
}

std::vector<Command> PduReader::parse_commands(const uint8_t* data,
                                               size_t len,
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
    c.nrParameters = r.u8();
    // 参数块字节数由 commandSize 隐式得出(头 17 字节:cmdSize4+handle4+
    // target4+methodID4+nrParameters1)
    if (c.commandSize < 17u)
      throw std::runtime_error("ocp1: commandSize too small");
    c.paramBytes = c.commandSize - 17u;
    size_t consumed = len - r.remaining();
    c.paramData = data + consumed;  // 指向 buffer 内参数块起点
    for (uint32_t p = 0; p < c.paramBytes; ++p)
      r.u8();  // 跳过参数字节(经 check() 防越界)
    out.push_back(c);
  }
  return out;
}

std::vector<Response> PduReader::parse_responses(const uint8_t* data,
                                                 size_t len,
                                                 uint16_t count) {
  Reader r(data, len);
  std::vector<Response> out;
  for (uint16_t i = 0; i < count; ++i) {
    Response rsp;
    rsp.responseSize = r.u32();
    rsp.handle = r.u32();
    rsp.statusCode = static_cast<Status>(r.u8());
    rsp.nrParameters = r.u8();
    // 参数块字节数由 responseSize 隐式得出(头 10 字节:rspSize4+handle4+
    // status1+nrParameters1)
    if (rsp.responseSize < 10u)
      throw std::runtime_error("ocp1: responseSize too small");
    rsp.paramBytes = rsp.responseSize - 10u;
    size_t consumed = len - r.remaining();
    rsp.paramData = data + consumed;
    for (uint32_t p = 0; p < rsp.paramBytes; ++p)
      r.u8();
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
    for (uint32_t d = 0; d < n.dataCount; ++d)
      r.u8();
    out.push_back(n);
  }
  return out;
}

// ---------- PduWriter ----------
static void write_pdu_header(Writer& w,
                             uint8_t pduType,
                             uint16_t msgCount,
                             uint32_t payloadLen) {
  w.u16(methods::kProtocolVersion);
  w.u32(9u + payloadLen);  // pduSize = header(9) + payload
  w.u8(pduType);
  w.u16(msgCount);
}

std::vector<uint8_t> PduWriter::build_command_pdu(uint16_t msgCount,
                                                  const uint8_t* cmds,
                                                  size_t len) {
  std::vector<uint8_t> out;
  out.push_back(methods::kSyncVal);
  Writer h;
  write_pdu_header(h, methods::kPduCommand, msgCount,
                   static_cast<uint32_t>(len));
  out.insert(out.end(), h.data(), h.data() + h.size());
  out.insert(out.end(), cmds, cmds + len);
  return out;
}

std::vector<uint8_t> PduWriter::build_response_pdu(uint16_t msgCount,
                                                   const uint8_t* rsps,
                                                   size_t len) {
  std::vector<uint8_t> out;
  out.push_back(methods::kSyncVal);
  Writer h;
  write_pdu_header(h, methods::kPduResponse, msgCount,
                   static_cast<uint32_t>(len));
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

}  // namespace oca::ocp1

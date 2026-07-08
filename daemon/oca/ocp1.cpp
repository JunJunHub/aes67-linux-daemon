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

// string/blob/bitstring/list 在 Task 4 实现(此处暂留,Task 4 补全)

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

// string/blob/bitstring/list 在 Task 4 实现

// PDU 分帧在 Task 5 实现

}  // namespace oca::ocp1

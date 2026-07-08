#define BOOST_TEST_MODULE oca_test
#include <boost/test/unit_test.hpp>

#include "oca/methods.hpp"
#include "oca/ocp1.hpp"
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
  r.u16();                                        // ok
  BOOST_CHECK_THROW(r.u8(), std::runtime_error);  // 越界
}

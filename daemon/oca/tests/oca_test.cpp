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

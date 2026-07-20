// daemon/noise/tests/noise_test.cpp
// Noise 模块 Boost.Test 入口。Spec1 Task 1 仅放 trivial 占位，
// 后续 task 追加 RcuPtr / PcmCaptureService / Bridge 单测。
#define BOOST_TEST_MODULE noise_test
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_CASE(placeholder) {
  BOOST_CHECK(true);
}

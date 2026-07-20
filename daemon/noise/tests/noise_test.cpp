// daemon/noise/tests/noise_test.cpp
// Noise 模块 Boost.Test 入口。Spec1 Task 1 仅放 trivial 占位，
// 后续 task 追加 RcuPtr / PcmCaptureService / Bridge 单测。
#define BOOST_TEST_MODULE noise_test
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_CASE(placeholder) {
  BOOST_CHECK(true);
}

#include "rcu_ptr.hpp"
#include <atomic>
#include <memory>

struct Foo {
  int value;
  explicit Foo(int v) : value(v) {}
};

BOOST_AUTO_TEST_SUITE(rcu_ptr_tests)

BOOST_AUTO_TEST_CASE(publish_load_returns_bare_pointer) {
  noise::RcuPtr<Foo> rcu;
  rcu.publish(std::make_shared<Foo>(42));
  Foo* loaded = rcu.load();
  BOOST_CHECK(loaded != nullptr);
  BOOST_CHECK_EQUAL(loaded->value, 42);
}

BOOST_AUTO_TEST_CASE(publish_replaces_value) {
  noise::RcuPtr<Foo> rcu;
  rcu.publish(std::make_shared<Foo>(1));
  rcu.publish(std::make_shared<Foo>(2));
  BOOST_CHECK_EQUAL(rcu.load()->value, 2);
}

BOOST_AUTO_TEST_CASE(constructor_publishes_init_never_null) {
  noise::RcuPtr<Foo> rcu(std::make_shared<Foo>(99));
  BOOST_CHECK(rcu.load() != nullptr);
  BOOST_CHECK_EQUAL(rcu.load()->value, 99);
}

BOOST_AUTO_TEST_CASE(two_epoch_retire_releases_old_after_two_advances) {
  static std::atomic<int> deleter_count{0};
  deleter_count.store(0);
  struct Tracked {
    int v;
    explicit Tracked(int x) : v(x) {}
  };
  auto make_tracked = [](int v) {
    return std::shared_ptr<Tracked>(new Tracked(v), [](Tracked* p) {
      deleter_count.fetch_add(1);
      delete p;
    });
  };
  noise::RcuPtr<Tracked> rcu;
  rcu.publish(make_tracked(1));
  std::shared_ptr<Tracked> old = rcu.publish(make_tracked(2));
  noise::RetireQueue<Tracked> rq;
  uint64_t retire_epoch = rcu.epoch();  // 0
  rq.retire(std::move(old), retire_epoch);

  rq.reclaim_older_than(rcu.epoch());  // epoch 0
  BOOST_CHECK_EQUAL(deleter_count.load(), 0);
  rcu.advance_epoch();  // epoch 1
  rq.reclaim_older_than(rcu.epoch());
  BOOST_CHECK_EQUAL(deleter_count.load(), 0);
  rcu.advance_epoch();  // epoch 2
  rq.reclaim_older_than(rcu.epoch());
  BOOST_CHECK_EQUAL(deleter_count.load(), 1);
}

BOOST_AUTO_TEST_CASE(const_t_supported) {
  noise::RcuPtr<const Foo> rcu;
  rcu.publish(std::make_shared<Foo>(7));
  const Foo* loaded = rcu.load();
  BOOST_CHECK(loaded != nullptr);
  BOOST_CHECK_EQUAL(loaded->value, 7);
}

BOOST_AUTO_TEST_SUITE_END()

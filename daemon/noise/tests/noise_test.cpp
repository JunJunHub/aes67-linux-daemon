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

#include "pcm_capture_service.hpp"
#include <chrono>
#include <thread>

// ── SessionManager 桩 ────────────────────────────────────────────────────
// PcmCaptureService::init() / is_sink_receiving() 引用以下 SessionManager
// 成员。fake_capture_loop 测试走 create_for_test() 旁路 init()，这些方法从不
// 被调用，仅为满足链接器符号解析。
#include "session_manager.hpp"

void SessionManager::add_ptp_status_observer(const PtpStatusObserver&) {}
void SessionManager::add_sink_observer(SinkObserverType, const SinkObserver&) {}
void SessionManager::get_ptp_status(PTPStatus&) const {}
std::error_code SessionManager::get_sink_status(uint32_t,
                                                SinkStreamStatus&) const {
  return {};
}

BOOST_AUTO_TEST_SUITE(pcm_capture_service_tests)

// fake_capture_loop：注册 stub provider，断言帧回调触发、frame_count==6144。
BOOST_AUTO_TEST_CASE(fake_capture_loop_dispatches_to_provider) {
  // FAKE_DRIVER 模式：PcmCaptureService 忽略 PTP，直接跑 fake_capture_loop。
  // SessionManager/Config 在测试中用最小 stub（见 Step 3 的测试辅助）。
  auto svc = PcmCaptureService::create_for_test();
  static std::atomic<int> callback_count{0};
  static std::atomic<size_t> last_frame_count{0};
  static std::atomic<uint8_t> last_channels{0};
  static std::atomic<uint32_t> last_rate{0};
  auto token =
      svc->register_provider([](const uint8_t* /*pcm*/, size_t frame_count,
                                uint8_t channels, uint32_t rate) {
        callback_count.fetch_add(1);
        last_frame_count.store(frame_count);
        last_channels.store(channels);
        last_rate.store(rate);
      });
  svc->start_fake_for_test(48000, 2);  // 48kHz, 2ch, 内置静音帧
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  svc->stop_for_test();
  svc->unregister_provider(token);
  BOOST_CHECK_GT(callback_count.load(), 0);
  BOOST_CHECK_EQUAL(last_frame_count.load(), 6144u);
  BOOST_CHECK_EQUAL(last_channels.load(), 2);
  BOOST_CHECK_EQUAL(last_rate.load(), 48000u);
}

BOOST_AUTO_TEST_SUITE_END()

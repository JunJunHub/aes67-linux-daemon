# Noise Spec1 Implementation Plan - PCM 分发与 RCU 基础

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `WITH_NOISE=ON` 下让 daemon 经 `PcmCaptureService` 独占 ALSA capture 并向消费者分发帧，Streamer 在 `#ifdef _USE_NOISE_` 守卫下零回归，`RcuPtr<T>` RCU 原语独立单测全过，为 Spec2 冻结对外稳定接口。

**Architecture:** `PcmCaptureService`（daemon 根，`WITH_NOISE` only）独占 ALSA capture + PTP observer，经 `RcuPtr<const vector<FrameProvider>>` 无锁分发给消费者；`NoiseAudioBridge` 纯虚接口（daemon/noise/）由 `NoiseSessionManagerBridge`（daemon 根）实现并委托 PcmCaptureService；`AudioCapture`（daemon/noise/）下游用 `std::function` callback 解耦，不前向依赖 Spec2 的 NoiseManager。Streamer 重构用 `#ifdef _USE_NOISE_` 守卫，上游路径逐字节保留。

**Tech Stack:** C++17, CMake 3.7+, Boost（log / test / thread / filesystem）, ALSA（asoundlib）, RAVENNA ALSA LKM headers, cpp-httplib, faac（Streamer）。测试用 Boost.Test（独立 `noise-test` 二进制）。

## Global Constraints

- **构建**：in-source 构建在 `daemon/` 内；worktree 用 `noise-dev.sh`（out-of-source 到 `daemon/build/`）。CMake 配置**必须**带 `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`（`.claude/rules/build.md`）。
- **WITH_NOISE 隔离**：`#ifdef _USE_NOISE_` 守卫切换 Streamer 路径；`WITH_NOISE=OFF` 时 daemon 行为零变化、Streamer 沿用上游 `snd_pcm_open/readi` 逐字节不变（Taste 决策2）。
- **采样率**：Phase 1 限定 48kHz（§11 风险1）。ALSA period hard-code `kPeriodSamples = 6144`（与 streamer.cpp:252 一致）。
- **RT 路径无锁**：`RcuPtr<T>` 用 `std::atomic<T*>`，RT 线程 `load()` 返回裸 `T*`，period 顶部 pin 一次、period 结尾 `advance_epoch()`，2-epoch retire。永不为空（构造即 publish）。
- **代码风格**：`.clang-format`（Chromium，80-col，2-space，`PointerAlignment: Left`，`SortIncludes: false`）。不重排既有 include。命名沿用既有（snake_case_ 成员，BOOST_LOG_TRIVIAL 日志）。
- **提交**：spec/plan 已独立成笔（`ac36b58`）。实现按 task 粒度提交，中文提交信息，scope=`noise`（daemon/noise/ + pcm_capture_service + bridge）或 `build`（CMake/noise-dev.sh）或 `streamer`（Streamer 重构）。提交信息尾加 `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`。
- **spec 依据**：`docs/superpowers/specs/noise-spec1-design.md`。架构细节引用 `docs/noise/architecture-design.md` §3.1/§3.8/§4.1/§4.2/§4.3/§4.4/§8.2/§11。
- **§8.2 勘误**：架构文档 §8.2 把 `add_library(noise STATIC ...)` 与 `$<TARGET_OBJECTS:noise>` 混用（后者仅对 OBJECT 库合法）。本计划改用 STATIC + 普通链接（`target_link_libraries(noise-test ... noise)`），记此偏差。

## File Structure

| 文件 | 动作 | 责任 | 所属 Task |
|------|------|------|-----------|
| `daemon/CMakeLists.txt` | 修改 | `option(WITH_NOISE)` + `add_subdirectory(noise)` + `noise-test` target | 1，后续 task 追加 SOURCES |
| `daemon/noise/CMakeLists.txt` | 新建 | `add_library(noise STATIC ${NOISE_SOURCES})`，渐进追加 | 1，2/3/4 追加 |
| `daemon/noise/tests/noise_test.cpp` | 新建 | Boost.Test 入口，trivial 起步，逐步加 test_case | 1，2/3/4 追加 |
| `daemon/noise/rcu_ptr.hpp` | 新建 | `RcuPtr<T>` + `RetireQueue<T>` header-only template | 2 |
| `daemon/pcm_capture_service.hpp` | 新建 | PcmCaptureService 接口 | 3 |
| `daemon/pcm_capture_service.cpp` | 新建 | 独占 ALSA capture + FrameProvider 分发 + PTP observer + fake_capture_loop | 3 |
| `daemon/noise/noise_audio_bridge.hpp` | 新建 | NoiseAudioBridge 纯虚接口（§4.1） | 4 |
| `daemon/noise/audio_capture.hpp` | 新建 | AudioCapture 帧分发（§3.1），callback 解耦 | 4 |
| `daemon/noise/audio_capture.cpp` | 新建 | AudioCapture 实现 | 4 |
| `daemon/noise_session_manager_bridge.hpp` | 新建 | Bridge 实现类声明 | 4 |
| `daemon/noise_session_manager_bridge.cpp` | 新建 | 委托 PcmCaptureService + uint8_t->float + 通道解复用 | 4 |
| `daemon/streamer.hpp` | 修改 | `#ifdef _USE_NOISE_` 守卫成员 + include | 5 |
| `daemon/streamer.cpp` | 修改 | `#ifdef _USE_NOISE_` 守卫 capture 路径 + FrameProvider 消费 | 5 |
| `noise-dev.sh` | 修改 | `cmd_build` 加 `-DWITH_NOISE=ON` + `--with-streamer` 选项 | 1 |

---

### Task 1: CMake 脚手架与 noise-test trivial target（1.0）

**Files:**
- Create: `daemon/noise/CMakeLists.txt`
- Create: `daemon/noise/tests/noise_test.cpp`
- Modify: `daemon/CMakeLists.txt`（option 区 L14-15 后 + add_executable L53 后）
- Modify: `noise-dev.sh`（`cmd_build` cmake 参数 + `--with-streamer` 选项解析）

**Interfaces:**
- Produces: `option(WITH_NOISE)`、`noise` 静态库（空）、`noise-test` target、`noise-dev.sh build` 默认带 `WITH_NOISE=ON`

- [ ] **Step 1: 创建空 noise 库的 CMakeLists**

Create `daemon/noise/CMakeLists.txt`:

```cmake
# daemon/noise/CMakeLists.txt
# 噪声模块核心源码（随 spec 渐进追加，Spec1 仅占位空库）。
# 注意：与架构文档 §8.2 的偏差——不使用 $<TARGET_OBJECTS:noise>（仅对 OBJECT
# 库合法），改用 STATIC + 普通链接（daemon/CMakeLists.txt 的 noise-test target
# 用 target_link_libraries(... noise)）。
set(NOISE_SOURCES
  # Spec1: 暂无 .cpp（rcu_ptr.hpp 是 header-only，audio_capture.cpp 在 Task 4 追加）
)

add_library(noise STATIC ${NOISE_SOURCES})
target_include_directories(noise PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
```

- [ ] **Step 2: 创建 trivial noise_test.cpp**

Create `daemon/noise/tests/noise_test.cpp`:

```cpp
// daemon/noise/tests/noise_test.cpp
// Noise 模块 Boost.Test 入口。Spec1 Task 1 仅放 trivial 占位，
// 后续 task 追加 RcuPtr / PcmCaptureService / Bridge 单测。
#define BOOST_TEST_MODULE noise_test
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_CASE(placeholder) {
  BOOST_CHECK(true);
}
```

- [ ] **Step 3: 修改 daemon/CMakeLists.txt 加 WITH_NOISE option**

Modify `daemon/CMakeLists.txt` — 在 L15 `option(WITH_STREAMER ...)` 之后插入：

```cmake
option(WITH_NOISE "Enable noise analysis and denoise module" OFF)
```

- [ ] **Step 4: 修改 daemon/CMakeLists.txt 加 noise 子模块与 noise-test target**

Modify `daemon/CMakeLists.txt` — 在 L57 `endif()`（ENABLE_TESTS add_subdirectory）之后、L59 `target_link_libraries(aes67-daemon ${Boost_LIBRARIES})` 之前插入 noise 块；并在文件末尾（L77 `endif()` 之后）追加 noise-test target：

```cmake
if(WITH_NOISE)
  MESSAGE(STATUS "WITH_NOISE")
  add_definitions(-D_USE_NOISE_)
  add_subdirectory(noise)
  target_link_libraries(aes67-daemon PRIVATE noise)
endif()

if(ENABLE_TESTS AND WITH_NOISE)
  add_executable(noise-test noise/tests/noise_test.cpp)
  target_link_libraries(noise-test PRIVATE noise ${Boost_LIBRARIES})
  target_compile_definitions(noise-test PRIVATE -D_USE_FAKE_DRIVER_ -D_USE_NOISE_)
  add_test(NAME noise-test COMMAND noise-test -p)
endif()
```

> 说明：Spec1 暂不把 `pcm_capture_service.cpp`/`noise_session_manager_bridge.cpp` 加进 `SOURCES`（文件未创建）。Task 3/4 创建文件时各自追加 `list(APPEND SOURCES ...)` 到 `WITH_NOISE` 块。ALSA 链接（`find_library(ALSA_LIBRARY ...)`)在 Task 3 追加。

- [ ] **Step 5: 修改 noise-dev.sh 的 cmd_build 加 WITH_NOISE=ON**

Modify `noise-dev.sh` `cmd_build()` — 在选项解析加 `--with-streamer`，cmake 参数加 `-DWITH_NOISE=ON`（默认）与可选 `-DWITH_STREAMER=ON`。

将 `cmd_build()` 的选项解析段（`local with_avahi=ON fake_driver=ON` 起）改为：

```bash
cmd_build() {
  local with_avahi=ON fake_driver=ON with_streamer=OFF with_noise=ON
  while [ $# -gt 0 ]; do
    case "$1" in
      --no-avahi)     with_avahi=OFF; shift ;;
      --real)         fake_driver=OFF; shift ;;
      --with-streamer) with_streamer=ON; shift ;;
      --no-noise)     with_noise=OFF; shift ;;
      *) die "build: unknown option: $1" ;;
    esac
  done
```

将 cmake 调用段末尾的 `-DWITH_STREAMER=OFF` 改为：

```bash
  cmake -S "$DAEMON_DIR" -B "$BUILD_DIR" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DCPP_HTTPLIB_DIR="$TOPDIR/3rdparty/cpp-httplib" \
      -DRAVENNA_ALSA_LKM_DIR="$TOPDIR/3rdparty/ravenna-alsa-lkm" \
      -DENABLE_TESTS=ON \
      -DWITH_AVAHI=$with_avahi \
      -DFAKE_DRIVER=$fake_driver \
      -DWITH_STREAMER=$with_streamer \
      -DWITH_NOISE=$with_noise
```

> `--with-streamer` 供 Task 5 验证 Streamer 重构（默认仍 OFF，保持 Spec1 gate 1 的 `WITH_STREAMER=OFF` 口径）。

- [ ] **Step 6: 验证 WITH_NOISE=ON 构建**

Run: `./noise-dev.sh build`
Expected: 构建通过，输出含 `WITH_NOISE`，`daemon/build/compile_commands.json` 生成。

- [ ] **Step 7: 验证 WITH_NOISE=OFF 构建零变化**

Run:
```bash
cd daemon && cmake -B /tmp/noise-off-check -DFAKE_DRIVER=ON -DWITH_NOISE=OFF -DWITH_STREAMER=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON . && cmake --build /tmp/noise-off-check --target aes67-daemon
```
Expected: 通过，daemon 行为与未引入 WITH_NOISE 前一致。

- [ ] **Step 8: 验证 noise-test 通过**

Run: `cd daemon/build && ctest -R noise-test --output-on-failure`
Expected: `placeholder` test PASS。

- [ ] **Step 9: 提交**

```bash
git add daemon/CMakeLists.txt daemon/noise/CMakeLists.txt daemon/noise/tests/noise_test.cpp noise-dev.sh
git commit -m "build(noise): Spec1 1.0 CMake 脚手架 - WITH_NOISE 选项 + noise 空库 + noise-test trivial

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: RcuPtr<T> RCU 原语与独立单测（1.4a）

**Files:**
- Create: `daemon/noise/rcu_ptr.hpp`
- Modify: `daemon/noise/tests/noise_test.cpp`（追加 RcuPtr test_case）

**Interfaces:**
- Produces: `noise::RcuPtr<T>`（`publish`/`load`/`advance_epoch`/`epoch`）、`noise::RetireQueue<T>`（`retire`/`reclaim_older_than`）。Spec2 的 NoiseManager 与 DenoiseProcessor 直接依赖此 API（spec §C 冻结）。

- [ ] **Step 1: 写失败测试（RcuPtr 基本语义）**

追加到 `daemon/noise/tests/noise_test.cpp`（`#include` 之后、`placeholder` 之后）：

```cpp
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

BOOST_AUTO_TEST_SUITE_END()
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd daemon/build && cmake --build . --target noise-test && ./noise-test -p`
Expected: 编译失败（`rcu_ptr.hpp` 不存在 / `noise::RcuPtr` 未定义）。

- [ ] **Step 3: 实现 rcu_ptr.hpp**

Create `daemon/noise/rcu_ptr.hpp`:

```cpp
// daemon/noise/rcu_ptr.hpp
// RCU（Read-Copy-Update）指针原语：RT 音频线程无锁读取控制线程发布的可变数据。
// 架构依据：docs/noise/architecture-design.md §3.8。
#ifndef NOISE_RCU_PTR_HPP_
#define NOISE_RCU_PTR_HPP_

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace noise {

// 延迟释放队列：持有 publish 返回的旧 shared_ptr，直到 RT 线程穿越 >=2 静止点。
// 控制线程的 housekeeper 定期调 reclaim_older_than(current_epoch) 回收。
template <typename T>
class RetireQueue {
 public:
  // 将旧值以当前 retire_epoch 入队。current_epoch 由 RcuPtr::epoch() 读取。
  void retire(std::shared_ptr<T> p, uint64_t retire_epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back({std::move(p), retire_epoch});
  }

  // 回收 retire_epoch + 1 < current_epoch 的条目（即 current_epoch >= retire_epoch + 2，
  // RT 已穿越 >=2 静止点，旧裸指针不再被持有）。
  void reclaim_older_than(uint64_t current_epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [current_epoch](const Entry& e) {
                         return e.retire_epoch + 1 < current_epoch;
                       }),
        entries_.end());
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
  }

 private:
  struct Entry {
    std::shared_ptr<T> ptr;
    uint64_t retire_epoch;
  };
  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
};

// RcuPtr<T>：原子发布 + period 顶部 pin + 2-epoch 回收。
// 单一读者约束（Phase 1/3 通用）：load()/advance_epoch() 仅由 capture 线程调用。
template <typename T>
class RcuPtr {
 public:
  RcuPtr() = default;

  // 构造即 publish，保证 load() 永不为空（§3.8 约束3）。
  explicit RcuPtr(std::shared_ptr<T> init) { publish(std::move(init)); }

  // 控制线程：发布新值，返回旧值（调用方推入 RetireQueue）。
  // 内存序 release：保证新值写入对 RT 线程可见。
  std::shared_ptr<T> publish(std::shared_ptr<T> new_val) {
    std::shared_ptr<T> old = std::move(current_owner_);
    current_owner_ = new_val;
    ptr_.store(new_val.get(), std::memory_order_release);
    return old;
  }

  // RT 线程（period 顶部 on_period_begin）：返回当前值的裸 T*。
  // 内存序 acquire：与 publish 的 release 配对。
  // 生命周期契约：返回的 T* 在下次 advance_epoch() 前有效。RT 不持引用计数。
  T* load() const { return ptr_.load(std::memory_order_acquire); }

  // RT 线程（period 结尾 on_period_end）：通知已穿越一个静止点。
  void advance_epoch() { epoch_.fetch_add(1, std::memory_order_release); }

  uint64_t epoch() const { return epoch_.load(std::memory_order_acquire); }

 private:
  std::atomic<T*> ptr_{nullptr};       // RT 快路径（裸指针原子，lock-free）
  std::atomic<uint64_t> epoch_{0};     // 单调递增静止点计数
  std::shared_ptr<T> current_owner_;   // 控制线程侧持有，保证当前对象存活
};

}  // namespace noise

#endif  // NOISE_RCU_PTR_HPP_
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd daemon/build && cmake --build . --target noise-test && ./noise-test -p`
Expected: `rcu_ptr_tests` 3 个 case 全 PASS。

- [ ] **Step 5: 写 2-epoch retire 与 const T 测试**

追加到 `noise_test.cpp` 的 `rcu_ptr_tests` suite 内：

```cpp
BOOST_AUTO_TEST_CASE(two_epoch_retire_releases_old_after_two_advances) {
  static std::atomic<int> deleter_count{0};
  deleter_count.store(0);
  struct Tracked {
    int v;
    explicit Tracked(int x) : v(x) {}
  };
  auto make_tracked = [](int v) {
    return std::shared_ptr<Tracked>(new Tracked(v),
                                    [](Tracked* p) {
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
```

- [ ] **Step 6: 跑测试确认通过**

Run: `cd daemon/build && cmake --build . --target noise-test && ./noise-test -p`
Expected: 全 5 个 RcuPtr case PASS（含 2-epoch retire 与 const T）。

- [ ] **Step 7: 提交**

```bash
git add daemon/noise/rcu_ptr.hpp daemon/noise/tests/noise_test.cpp
git commit -m "feat(noise): Spec1 1.4a RcuPtr<T> RCU 原语 + 独立单测

裸 T* load（acquire）+ period 顶部 pin + 2-epoch retire 回收，
RT 路径无锁（std::atomic<T*>），含 const T 支持与永不为空契约。

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: PcmCaptureService 独占 ALSA capture + FrameProvider 分发（1.1）

**Files:**
- Create: `daemon/pcm_capture_service.hpp`
- Create: `daemon/pcm_capture_service.cpp`
- Modify: `daemon/CMakeLists.txt`（WITH_NOISE 块加 `list(APPEND SOURCES pcm_capture_service.cpp)` + ALSA 链接）
- Modify: `daemon/noise/CMakeLists.txt`（无变化，pcm_capture_service 在 daemon 根）
- Modify: `daemon/noise/tests/noise_test.cpp`（追加 PcmCaptureService fake_capture_loop 测试）

**Interfaces:**
- Consumes: `noise::RcuPtr<T>`（Task 2）、`SessionManager`（`add_ptp_status_observer`/`add_sink_observer`/`get_ptp_status`，见 streamer.cpp:38-50）、`Config`（`get_sample_rate`/`get_streamer_channels`）
- Produces: `PcmCaptureService::create`/`init`/`terminate`/`register_provider`/`unregister_provider`/`is_sink_receiving`/`get_sample_rate`/`get_sink_channel_count`/`is_capturing`（spec §C 冻结，Spec2 Bridge 委托）

- [ ] **Step 1: 写失败测试（fake_capture_loop 帧分发）**

追加到 `noise_test.cpp`（include 区加 `#include "pcm_capture_service.hpp"`，新增 suite）：

```cpp
#include "pcm_capture_service.hpp"
#include <chrono>
#include <thread>

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
  auto token = svc->register_provider(
      [](const uint8_t* /*pcm*/, size_t frame_count, uint8_t channels,
         uint32_t rate) {
        callback_count.fetch_add(1);
        last_frame_count.store(frame_count);
        last_channels.store(channels);
        last_rate.store(rate);
      });
  svc->start_fake_for_test(48, 2);  // 48kHz, 2ch, 内置静音帧
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  svc->stop_for_test();
  svc->unregister_provider(token);
  BOOST_CHECK_GT(callback_count.load(), 0);
  BOOST_CHECK_EQUAL(last_frame_count.load(), 6144u);
  BOOST_CHECK_EQUAL(last_channels.load(), 2);
  BOOST_CHECK_EQUAL(last_rate.load(), 48000u);
}

BOOST_AUTO_TEST_SUITE_END()
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd daemon/build && cmake --build . --target noise-test 2>&1 | head -20`
Expected: 编译失败（`pcm_capture_service.hpp` 不存在）。

- [ ] **Step 3: 实现 pcm_capture_service.hpp**

Create `daemon/pcm_capture_service.hpp`:

```cpp
// daemon/pcm_capture_service.hpp
// PCM 分发基础设施：独占 ALSA capture + FrameProvider 分发 + PTP observer。
// 架构依据：docs/noise/architecture-design.md §4.3。
// 编译条件：WITH_NOISE=ON（Taste 决策2，WITH_NOISE=OFF 时 Streamer 沿用上游 ALSA 路径）。
#ifndef DAEMON_PCM_CAPTURE_SERVICE_HPP_
#define DAEMON_PCM_CAPTURE_SERVICE_HPP_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#ifdef _USE_NOISE_
#include "noise/rcu_ptr.hpp"
#include "session_manager_fwd.hpp"  // 若无 fwd，用完整 session_manager.hpp
#endif

class Config;
class SessionManager;

class PcmCaptureService
#ifdef _USE_NOISE_
    : public std::enable_shared_from_this<PcmCaptureService>
#endif
{
 public:
  using FrameProvider = std::function<void(const uint8_t* interleaved_pcm,
                                           size_t frame_count,
                                           uint8_t channels,
                                           uint32_t sample_rate)>;
  using ProviderToken = uint32_t;

  static std::shared_ptr<PcmCaptureService> create(
      std::shared_ptr<SessionManager> session_manager,
      std::shared_ptr<Config> config);

  bool init();       // 注册 PTP observer + Sink observer
  bool terminate();

  ProviderToken register_provider(FrameProvider provider);
  void unregister_provider(ProviderToken token);

  bool is_sink_receiving(uint8_t sink_id) const;
  uint32_t get_sample_rate() const;
  uint8_t get_sink_channel_count(uint8_t sink_id) const;
  bool is_capturing() const;

  // ── 测试专用（FAKE_DRIVER 下驱动 fake_capture_loop，不依赖 SessionManager）──
  static std::shared_ptr<PcmCaptureService> create_for_test();
  void start_fake_for_test(uint32_t sample_rate, uint8_t channels);
  void stop_for_test();

 private:
  explicit PcmCaptureService(std::shared_ptr<SessionManager> session_manager,
                             std::shared_ptr<Config> config)
      : session_manager_(std::move(session_manager)),
        config_(std::move(config)) {}
  explicit PcmCaptureService() = default;  // 测试用

  void on_ptp_status_change(const std::string& status);
  void on_sink_add(uint8_t id);
  void on_sink_remove(uint8_t id);

  bool start_capture();
  bool stop_capture();
  void capture_loop();        // 真实 ALSA（PTP locked 时运行）
  void fake_capture_loop();  // FAKE_DRIVER：从 WAV/静音帧读取

  void dispatch(const uint8_t* pcm, size_t frame_count, uint8_t channels,
                uint32_t rate);

  static constexpr uint32_t kPeriodSamples = 6144;

  std::shared_ptr<SessionManager> session_manager_;
  std::shared_ptr<Config> config_;
  snd_pcm_t* capture_handle_{nullptr};  // 仅 _USE_NOISE_ 用
  std::atomic_bool running_{false};
  std::atomic_bool stop_flag_{false};
  std::future<void> capture_future_;
  std::string fake_pcm_source_;

#ifdef _USE_NOISE_
  // FrameProvider 表：控制线程 COW 建新 vector 原子换，capture 线程 period 顶部 load 快照。
  struct ProviderEntry {
    ProviderToken token;
    FrameProvider provider;
  };
  std::shared_ptr<const std::vector<ProviderEntry>> providers_snapshot_;
  noise::RcuPtr<std::vector<ProviderEntry>> providers_;
  std::atomic<ProviderToken> next_token_{1};
  noise::RetireQueue<std::vector<ProviderEntry>> providers_retire_;
  // 测试用 fake 参数
  uint32_t test_rate_{48000};
  uint8_t test_channels_{2};
#endif
};

#endif  // DAEMON_PCM_CAPTURE_SERVICE_HPP_
```

> `session_manager_fwd.hpp` 若不存在则直接 `#include "session_manager.hpp"`（既有 streamer.hpp 已这么用）。实现时统一用完整 include，删 fwd 注释行。

- [ ] **Step 4: 实现 pcm_capture_service.cpp（fake_capture_loop + 分发 + PTP observer）**

Create `daemon/pcm_capture_service.cpp`. 完整实现（`#ifdef _USE_NOISE_` 整文件包裹，WITH_NOISE=OFF 时不编译）：

```cpp
// daemon/pcm_capture_service.cpp
// 架构依据：docs/noise/architecture-design.md §4.3 / §11 风险19/21。
#ifdef _USE_NOISE_

#include "pcm_capture_service.hpp"

#include <alsa/asoundlib.h>
#include <boost/log/trivial.hpp>
#include <chrono>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

#include "config.hpp"
#include "session_manager.hpp"

constexpr uint32_t PcmCaptureService::kPeriodSamples;

std::shared_ptr<PcmCaptureService> PcmCaptureService::create(
    std::shared_ptr<SessionManager> session_manager,
    std::shared_ptr<Config> config) {
  return std::shared_ptr<PcmCaptureService>(
      new PcmCaptureService(std::move(session_manager), std::move(config)));
}

std::shared_ptr<PcmCaptureService> PcmCaptureService::create_for_test() {
  // 测试用：无 SessionManager/Config，直接驱动 fake_capture_loop。
  return std::shared_ptr<PcmCaptureService>(new PcmCaptureService());
}

bool PcmCaptureService::init() {
  if (!session_manager_ || !config_) return false;
  session_manager_->add_ptp_status_observer(
      std::bind(&PcmCaptureService::on_ptp_status_change, this,
                std::placeholders::_1));
  session_manager_->add_sink_observer(
      SessionManager::SinkObserverType::add_sink,
      std::bind(&PcmCaptureService::on_sink_add, this, std::placeholders::_1));
  session_manager_->add_sink_observer(
      SessionManager::SinkObserverType::remove_sink,
      std::bind(&PcmCaptureService::on_sink_remove, this, std::placeholders::_1));
  // 初始化空 provider 表（永不为空契约）
  providers_.publish(std::make_shared<std::vector<ProviderEntry>>());
  PTPStatus status;
  session_manager_->get_ptp_status(status);
  on_ptp_status_change(status.status);
  return true;
}

bool PcmCaptureService::terminate() {
  stop_capture();
  return true;
}

PcmCaptureService::ProviderToken PcmCaptureService::register_provider(
    FrameProvider provider) {
  ProviderToken token = next_token_.fetch_add(1);
  // COW：复制当前表 -> 追加 -> 原子换
  auto current = providers_.load();
  auto new_table = std::make_shared<std::vector<ProviderEntry>>(*current);
  new_table->push_back({token, std::move(provider)});
  auto old = providers_.publish(new_table);
  providers_retire_.retire(std::move(old), providers_.epoch());
  return token;
}

void PcmCaptureService::unregister_provider(ProviderToken token) {
  auto current = providers_.load();
  auto new_table = std::make_shared<std::vector<ProviderEntry>>();
  new_table->reserve(current->size());
  for (const auto& e : *current) {
    if (e.token != token) new_table->push_back(e);
  }
  auto old = providers_.publish(new_table);
  providers_retire_.retire(std::move(old), providers_.epoch());
}

void PcmCaptureService::dispatch(const uint8_t* pcm, size_t frame_count,
                                 uint8_t channels, uint32_t rate) {
  // period 顶部 load 快照（整 period 复用，不每帧原子操作）
  auto snapshot = providers_.load();
  for (const auto& e : *snapshot) {
    e.provider(pcm, frame_count, channels, rate);
  }
}

bool PcmCaptureService::is_sink_receiving(uint8_t sink_id) const {
  // SessionManager 无 is_sink_receiving，用 get_sink_status + SinkStreamStatus
  // （session_manager.hpp:70 is_receiving_rtp_packet / :166 get_sink_status）
  if (!session_manager_) return false;
  SinkStreamStatus status;
  if (session_manager_->get_sink_status(sink_id, status)) return false;
  return status.is_receiving_rtp_packet;
}

uint32_t PcmCaptureService::get_sample_rate() const {
  return config_ ? config_->get_sample_rate() : test_rate_;
}

uint8_t PcmCaptureService::get_sink_channel_count(uint8_t sink_id) const {
  // Spec1 最小实现：返回 streamer channels（Phase 1 限定全通道分发）
  return config_ ? config_->get_streamer_channels() : test_channels_;
}

bool PcmCaptureService::is_capturing() const { return running_.load(); }

void PcmCaptureService::on_ptp_status_change(const std::string& status) {
  BOOST_LOG_TRIVIAL(info) << "PcmCaptureService: ptp status " << status;
#ifdef _USE_FAKE_DRIVER_
  // FAKE_DRIVER：PTP 永远 UNLOCKED，忽略 PTP 状态直接起 fake loop（§4.3）
  if (!running_.load() && status == "unlocked") {
    start_capture();
  }
  return;
#else
  if (status == "locked") {
    start_capture();
  } else if (status == "unlocked") {
    stop_capture();
  }
#endif
}

void PcmCaptureService::on_sink_add(uint8_t /*id*/) {}
void PcmCaptureService::on_sink_remove(uint8_t /*id*/) {}

bool PcmCaptureService::start_capture() {
  if (running_.load()) return true;
  stop_flag_.store(false);
  running_.store(true);
#ifdef _USE_FAKE_DRIVER_
  capture_future_ = std::async(std::launch::async, [this] { fake_capture_loop(); });
#else
  capture_future_ = std::async(std::launch::async, [this] { capture_loop(); });
#endif
  return true;
}

bool PcmCaptureService::stop_capture() {
  if (!running_.load()) return true;
  stop_flag_.store(true);
  // §11 风险19：控制线程调 snd_pcm_drop()+close() 中断阻塞 readi。
  if (capture_handle_) {
    snd_pcm_drop(capture_handle_);
  }
  if (capture_future_.valid()) capture_future_.wait();
  if (capture_handle_) {
    snd_pcm_close(capture_handle_);
    capture_handle_ = nullptr;
  }
  running_.store(false);
  return true;
}

// FAKE_DRIVER：模拟 ALSA period 节拍分发（§11 风险21 三规格）。
void PcmCaptureService::fake_capture_loop() {
  BOOST_LOG_TRIVIAL(info) << "PcmCaptureService: fake_capture_loop start";
  // 静音帧缓冲（S16_LE 交错），按 test_rate_/test_channels_ 配置。
  // Spec1：未指定 fake_pcm_source_ 时用内置静音帧（§4.3）。
  const uint8_t channels = test_channels_;
  const uint32_t rate = test_rate_;
  const size_t bytes_per_sample = 2;  // S16_LE
  const size_t period_bytes = kPeriodSamples * channels * bytes_per_sample;
  std::vector<uint8_t> silent(period_bytes, 0);

  // period 时长 = 6144 / 48000 ≈ 128ms
  const auto period_duration =
      std::chrono::microseconds(kPeriodSamples * 1000000ULL / rate);
  auto next_period = std::chrono::steady_clock::now();
  while (!stop_flag_.load()) {
    dispatch(silent.data(), kPeriodSamples, channels, rate);
    next_period += period_duration;
    std::this_thread::sleep_until(next_period);
  }
  // period 结尾推进 epoch（RT 静止点），供 retire 回收判断
  providers_.advance_epoch();
  providers_retire_.reclaim_older_than(providers_.epoch());
  BOOST_LOG_TRIVIAL(info) << "PcmCaptureService: fake_capture_loop end";
}

// 真实 ALSA capture（PTP locked 时运行）。hw_params 镜像 streamer.cpp start_capture。
void PcmCaptureService::capture_loop() {
  BOOST_LOG_TRIVIAL(info) << "PcmCaptureService: capture_loop start";
  constexpr const char* device = "plughw:RAVENNA";
  int err;
  if ((err = snd_pcm_open(&capture_handle_, device, SND_PCM_STREAM_CAPTURE,
                          SND_PCM_NONBLOCK)) < 0) {
    BOOST_LOG_TRIVIAL(fatal)
        << "PcmCaptureService: cannot open " << device << ": " << snd_strerror(err);
    running_.store(false);
    return;
  }
  snd_pcm_hw_params_t* hw;
  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_hw_params_any(capture_handle_, hw);
  snd_pcm_hw_params_set_access(capture_handle_, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(capture_handle_, hw, SND_PCM_FORMAT_S16_LE);
  uint32_t rate = config_->get_sample_rate();
  snd_pcm_hw_params_set_rate_near(capture_handle_, hw, &rate, 0);
  uint8_t channels = config_->get_streamer_channels();
  snd_pcm_hw_params_set_channels(capture_handle_, hw, channels);
  snd_pcm_hw_params(capture_handle_, hw);
  snd_pcm_prepare(capture_handle_);

  const size_t bytes_per_frame = 2 * channels;  // S16_LE
  std::vector<uint8_t> buf(kPeriodSamples * bytes_per_frame);
  while (!stop_flag_.load()) {
    snd_pcm_sframes_t n = snd_pcm_readi(capture_handle_, buf.data(), kPeriodSamples);
    if (n < 0) {
      // stop_flag_ 触发或 xrun：readi 返回错误，检查 stop_flag_ 退出
      if (stop_flag_.load()) break;
      snd_pcm_recover(capture_handle_, n, 1);
      continue;
    }
    dispatch(buf.data(), static_cast<size_t>(n), channels, rate);
    providers_.advance_epoch();
    providers_retire_.reclaim_older_than(providers_.epoch());
  }
  BOOST_LOG_TRIVIAL(info) << "PcmCaptureService: capture_loop end";
}

// 测试专用入口
void PcmCaptureService::start_fake_for_test(uint32_t sample_rate, uint8_t channels) {
  test_rate_ = sample_rate;
  test_channels_ = channels;
  providers_.publish(std::make_shared<std::vector<ProviderEntry>>());
  start_capture();
}
void PcmCaptureService::stop_for_test() { stop_capture(); }

#endif  // _USE_NOISE_
```

- [ ] **Step 5: 修改 daemon/CMakeLists.txt 把 pcm_capture_service.cpp 加进 SOURCES + 链 ALSA**

Modify `daemon/CMakeLists.txt` — 把 Task 1 的 `if(WITH_NOISE)` 块扩展：

```cmake
if(WITH_NOISE)
  MESSAGE(STATUS "WITH_NOISE")
  add_definitions(-D_USE_NOISE_)
  list(APPEND SOURCES pcm_capture_service.cpp)
  add_subdirectory(noise)
  target_link_libraries(aes67-daemon PRIVATE noise)
endif()
```

把文件末尾的 `if(WITH_STREAMER)` ALSA/AAC 块（L73-77）拆分，ALSA 提到 WITH_NOISE 也用：

```cmake
if(WITH_STREAMER OR WITH_NOISE)
  find_library(ALSA_LIBRARY NAMES asound)
  target_link_libraries(aes67-daemon ${ALSA_LIBRARY})
endif()

if(WITH_STREAMER)
  find_library(AAC_LIBRARY NAMES faac)
  target_link_libraries(aes67-daemon ${AAC_LIBRARY})
endif()
```

并把 noise-test target 链接 ALSA：

```cmake
if(ENABLE_TESTS AND WITH_NOISE)
  add_executable(noise-test noise/tests/noise_test.cpp pcm_capture_service.cpp)
  target_link_libraries(noise-test PRIVATE noise ${Boost_LIBRARIES} ${ALSA_LIBRARY})
  target_compile_definitions(noise-test PRIVATE -D_USE_FAKE_DRIVER_ -D_USE_NOISE_)
  add_test(NAME noise-test COMMAND noise-test -p)
endif()
```

- [ ] **Step 6: 跑测试确认通过**

Run: `cd daemon/build && cmake --build . --target noise-test && ./noise-test -p`
Expected: `pcm_capture_service_tests::fake_capture_loop_dispatches_to_provider` PASS（callback_count>0、frame_count==6144、channels==2、rate==48000）。

- [ ] **Step 7: 验证 daemon 整体构建（WITH_NOISE=ON）**

Run: `./noise-dev.sh build`
Expected: aes67-daemon 构建通过（PcmCaptureService 编入，main.cpp 尚未装配--见 Spec3 1.11，但独立编译无误）。

- [ ] **Step 8: 验证 WITH_NOISE=OFF 零回归**

Run: `./noise-dev.sh build --no-noise`
Expected: 通过，daemon 行为不变（PcmCaptureService 不编译）。

- [ ] **Step 9: 提交**

```bash
git add daemon/pcm_capture_service.hpp daemon/pcm_capture_service.cpp daemon/CMakeLists.txt daemon/noise/tests/noise_test.cpp
git commit -m "feat(noise): Spec1 1.1 PcmCaptureService - 独占 ALSA capture + FrameProvider 分发

PTP observer 驱动 capture_loop；FAKE_DRIVER 下 fake_capture_loop 模拟
节拍（6144 样本/period）；providers_ 用 RcuPtr COW 无锁分发；stop_flag_
+ snd_pcm_drop 中断阻塞 readi（§11 风险19）。

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: NoiseAudioBridge + NoiseSessionManagerBridge + AudioCapture（1.3）

**Files:**
- Create: `daemon/noise/noise_audio_bridge.hpp`
- Create: `daemon/noise/audio_capture.hpp`
- Create: `daemon/noise/audio_capture.cpp`
- Create: `daemon/noise_session_manager_bridge.hpp`
- Create: `daemon/noise_session_manager_bridge.cpp`
- Modify: `daemon/CMakeLists.txt`（WITH_NOISE 块加 `list(APPEND SOURCES noise_session_manager_bridge.cpp)`）
- Modify: `daemon/noise/CMakeLists.txt`（NOISE_SOURCES 加 `audio_capture.cpp`）
- Modify: `daemon/noise/tests/noise_test.cpp`（追加 Bridge + AudioCapture 测试）

**Interfaces:**
- Consumes: `PcmCaptureService`（Task 3，`register_provider`/`ProviderToken`）
- Produces: `noise::NoiseAudioBridge` 纯虚（§4.1，Spec2 NoiseManager 注册回调）、`AudioCapture::FrameCallback`/`PeriodBeginCallback`/`PeriodEndCallback`（Spec2 NoiseManager 注册自身）

- [ ] **Step 1: 写失败测试（Bridge 格式转换 + 通道解复用）**

追加到 `noise_test.cpp`（include 加 `#include "noise_session_manager_bridge.hpp"` + `#include "noise/audio_capture.hpp"`）：

```cpp
#include "noise_session_manager_bridge.hpp"
#include "noise/audio_capture.hpp"

BOOST_AUTO_TEST_SUITE(bridge_tests)

// uint8_t(S16_LE 交错)->float 转换 + 单通道解复用。
// 构造已知交错 PCM：[ch0_s0, ch1_s0, ch0_s1, ch1_s1, ...]，提取 ch0。
BOOST_AUTO_TEST_CASE(bridge_demux_and_float_conversion) {
  auto pcm_svc = PcmCaptureService::create_for_test();
  noise::NoiseSessionManagerBridge bridge(pcm_svc);

  // 2 通道，4 样本：ch0 = [0, 16384, -16384, 32767]，ch1 全 0
  const int16_t interleaved[8] = {0, 0, 16384, 0, -16384, 0, 32767, 0};
  static std::vector<float> received;
  received.clear();
  AudioCapture cap;
  cap.register_callback([](const float* frames, size_t n, uint8_t ch) {
    BOOST_CHECK_EQUAL(ch, 1);
    for (size_t i = 0; i < n; ++i) received.push_back(frames[i]);
  });
  // Bridge 把交错 S16 转 float 单通道后喂 AudioCapture 回调
  bridge.test_demux_for_test(reinterpret_cast<const uint8_t*>(interleaved),
                             4 /*samples*/, 2 /*channels*/, 0 /*ch_index*/,
                             cap);
  BOOST_CHECK_EQUAL(received.size(), 4u);
  BOOST_CHECK_CLOSE(received[0], 0.0f / 32768.0f, 0.01);
  BOOST_CHECK_CLOSE(received[1], 16384.0f / 32768.0f, 0.01);
  BOOST_CHECK_CLOSE(received[2], -16384.0f / 32768.0f, 0.01);
  BOOST_CHECK_CLOSE(received[3], 32767.0f / 32768.0f, 0.01);
}

BOOST_AUTO_TEST_SUITE_END()
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd daemon/build && cmake --build . --target noise-test 2>&1 | head -20`
Expected: 编译失败（`noise_session_manager_bridge.hpp` 不存在）。

- [ ] **Step 3: 实现 noise_audio_bridge.hpp（§4.1 纯虚接口）**

Create `daemon/noise/noise_audio_bridge.hpp`:

```cpp
// daemon/noise/noise_audio_bridge.hpp
// 架构依据：docs/noise/architecture-design.md §4.1。
#ifndef NOISE_NOISE_AUDIO_BRIDGE_HPP_
#define NOISE_NOISE_AUDIO_BRIDGE_HPP_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace noise {

class NoiseAudioBridge {
 public:
  virtual ~NoiseAudioBridge() = default;

  using FrameProvider = std::function<void(uint8_t sink_id, const float* frames,
                                           size_t frame_size, uint8_t channels)>;
  virtual void register_frame_provider(uint8_t sink_id,
                                       const std::vector<uint8_t>& channel_map,
                                       FrameProvider provider) = 0;
  virtual void unregister_frame_provider(uint8_t sink_id) = 0;

  virtual bool is_sink_receiving(uint8_t sink_id) const = 0;
  virtual uint32_t get_sample_rate() const = 0;
  virtual uint8_t get_sink_channel_count(uint8_t sink_id) const = 0;

  using PtpStatusCallback = std::function<void(const std::string& status)>;
  virtual void set_ptp_status_callback(PtpStatusCallback cb) = 0;
  using SinkChangeCallback = std::function<void(uint8_t sink_id)>;
  virtual void set_sink_add_callback(SinkChangeCallback cb) = 0;
  virtual void set_sink_remove_callback(SinkChangeCallback cb) = 0;
};

}  // namespace noise

#endif  // NOISE_NOISE_AUDIO_BRIDGE_HPP_
```

- [ ] **Step 4: 实现 audio_capture.hpp（§3.1 + 决策4 callback 解耦）**

Create `daemon/noise/audio_capture.hpp`:

```cpp
// daemon/noise/audio_capture.hpp
// 架构依据：docs/noise/architecture-design.md §3.1。
// 决策4：下游经 std::function callback 解耦，不前向依赖 Spec2 的 NoiseManager。
#ifndef NOISE_AUDIO_CAPTURE_HPP_
#define NOISE_AUDIO_CAPTURE_HPP_

#include <cstdint>
#include <functional>

#include "noise_audio_bridge.hpp"

class AudioCapture {
 public:
  using FrameCallback = std::function<void(const float* frames, size_t frame_size,
                                           uint8_t channel_count)>;
  using PeriodBeginCallback = std::function<void()>;
  using PeriodEndCallback = std::function<void()>;

  bool start(uint8_t sink_id, noise::NoiseAudioBridge& bridge);
  bool stop();
  void register_callback(FrameCallback cb);
  void register_period_callbacks(PeriodBeginCallback begin, PeriodEndCallback end);
  bool is_running() const;

  // Bridge 在 ALSA period 边界调用（§3.1）：分发前 begin，分发后 end。
  void on_period_begin();
  void on_period_end();
  // Bridge 调用：分发一帧单通道 float。
  void on_frame(uint8_t sink_id, const float* frames, size_t frame_size,
                uint8_t channels);

 private:
  FrameCallback frame_cb_;
  PeriodBeginCallback period_begin_cb_;
  PeriodEndCallback period_end_cb_;
  bool running_{false};
};

#endif  // NOISE_AUDIO_CAPTURE_HPP_
```

- [ ] **Step 5: 实现 audio_capture.cpp**

Create `daemon/noise/audio_capture.cpp`:

```cpp
// daemon/noise/audio_capture.cpp
#include "audio_capture.hpp"

#include <utility>

bool AudioCapture::start(uint8_t /*sink_id*/, noise::NoiseAudioBridge& /*bridge*/) {
  // Spec1：Bridge 侧 register_frame_provider 由 NoiseSessionManagerBridge 负责。
  // AudioCapture 仅持回调，帧经 Bridge 解复用后调 on_frame。
  running_ = true;
  return true;
}

bool AudioCapture::stop() {
  running_ = false;
  return true;
}

void AudioCapture::register_callback(FrameCallback cb) { frame_cb_ = std::move(cb); }

void AudioCapture::register_period_callbacks(PeriodBeginCallback begin,
                                              PeriodEndCallback end) {
  period_begin_cb_ = std::move(begin);
  period_end_cb_ = std::move(end);
}

bool AudioCapture::is_running() const { return running_; }

void AudioCapture::on_period_begin() {
  if (period_begin_cb_) period_begin_cb_();
}

void AudioCapture::on_period_end() {
  if (period_end_cb_) period_end_cb_();
}

void AudioCapture::on_frame(uint8_t /*sink_id*/, const float* frames,
                            size_t frame_size, uint8_t channels) {
  if (frame_cb_) frame_cb_(frames, frame_size, channels);
}
```

- [ ] **Step 6: 实现 noise_session_manager_bridge.hpp**

Create `daemon/noise_session_manager_bridge.hpp`:

```cpp
// daemon/noise_session_manager_bridge.hpp
// 架构依据：docs/noise/architecture-design.md §4.2。
#ifndef DAEMON_NOISE_SESSION_MANAGER_BRIDGE_HPP_
#define DAEMON_NOISE_SESSION_MANAGER_BRIDGE_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "noise/noise_audio_bridge.hpp"

class PcmCaptureService;
class AudioCapture;

class NoiseSessionManagerBridge : public noise::NoiseAudioBridge {
 public:
  explicit NoiseSessionManagerBridge(std::shared_ptr<PcmCaptureService> pcm_capture);
  ~NoiseSessionManagerBridge() override;

  void register_frame_provider(uint8_t sink_id,
                               const std::vector<uint8_t>& channel_map,
                               FrameProvider provider) override;
  void unregister_frame_provider(uint8_t sink_id) override;

  bool is_sink_receiving(uint8_t sink_id) const override;
  uint32_t get_sample_rate() const override;
  uint8_t get_sink_channel_count(uint8_t sink_id) const override;

  void set_ptp_status_callback(PtpStatusCallback cb) override;
  void set_sink_add_callback(SinkChangeCallback cb) override;
  void set_sink_remove_callback(SinkChangeCallback cb) override;

  // 测试专用：把交错 uint8_t(S16_LE) 转 float 单通道后喂 AudioCapture 回调。
  void test_demux_for_test(const uint8_t* interleaved, size_t samples,
                           uint8_t channels, uint8_t ch_index, AudioCapture& cap);

 private:
  std::shared_ptr<PcmCaptureService> pcm_capture_;
  PtpStatusCallback ptp_cb_;
  SinkChangeCallback sink_add_cb_;
  SinkChangeCallback sink_remove_cb_;
  // convert_buffer_：按 max_period × max_channels × sizeof(float) 分配（§11 风险18）
  std::vector<float> convert_buffer_;
  static constexpr size_t kMaxPeriodSamples = 6144;
  static constexpr uint8_t kMaxChannels = 8;
};

#endif  // DAEMON_NOISE_SESSION_MANAGER_BRIDGE_HPP_
```

- [ ] **Step 7: 实现 noise_session_manager_bridge.cpp**

Create `daemon/noise_session_manager_bridge.cpp`:

```cpp
// daemon/noise_session_manager_bridge.cpp
#include "noise_session_manager_bridge.hpp"

#include <cstring>

#include "pcm_capture_service.hpp"
#include "noise/audio_capture.hpp"

NoiseSessionManagerBridge::NoiseSessionManagerBridge(
    std::shared_ptr<PcmCaptureService> pcm_capture)
    : pcm_capture_(std::move(pcm_capture)),
      convert_buffer_(kMaxPeriodSamples * kMaxChannels, 0.0f) {}

NoiseSessionManagerBridge::~NoiseSessionManagerBridge() = default;

void NoiseSessionManagerBridge::register_frame_provider(
    uint8_t /*sink_id*/, const std::vector<uint8_t>& /*channel_map*/,
    FrameProvider /*provider*/) {
  // Spec1：委托 PcmCaptureService（Spec2 NoiseManager 经此注册）。
  // 完整 channel_map 提取在 Spec2 1.4b 接入时实现，Spec1 仅留接口。
}

void NoiseSessionManagerBridge::unregister_frame_provider(uint8_t /*sink_id*/) {}

bool NoiseSessionManagerBridge::is_sink_receiving(uint8_t sink_id) const {
  return pcm_capture_ && pcm_capture_->is_sink_receiving(sink_id);
}

uint32_t NoiseSessionManagerBridge::get_sample_rate() const {
  return pcm_capture_ ? pcm_capture_->get_sample_rate() : 0;
}

uint8_t NoiseSessionManagerBridge::get_sink_channel_count(uint8_t sink_id) const {
  return pcm_capture_ ? pcm_capture_->get_sink_channel_count(sink_id) : 0;
}

void NoiseSessionManagerBridge::set_ptp_status_callback(PtpStatusCallback cb) {
  ptp_cb_ = std::move(cb);
}
void NoiseSessionManagerBridge::set_sink_add_callback(SinkChangeCallback cb) {
  sink_add_cb_ = std::move(cb);
}
void NoiseSessionManagerBridge::set_sink_remove_callback(SinkChangeCallback cb) {
  sink_remove_cb_ = std::move(cb);
}

// uint8_t(S16_LE 交错) -> float 单通道解复用。
void NoiseSessionManagerBridge::test_demux_for_test(const uint8_t* interleaved,
                                                     size_t samples,
                                                     uint8_t channels,
                                                     uint8_t ch_index,
                                                     AudioCapture& cap) {
  // §11 风险18：容量断言
  if (samples * channels > kMaxPeriodSamples * kMaxChannels) {
    return;  // 超容量丢弃（驱动异常，Spec1 不越界写）
  }
  const int16_t* src = reinterpret_cast<const int16_t*>(interleaved);
  // 提取 ch_index 通道到 convert_buffer_ 前 samples 个位置
  for (size_t i = 0; i < samples; ++i) {
    convert_buffer_[i] = static_cast<float>(src[i * channels + ch_index]) / 32768.0f;
  }
  cap.on_period_begin();
  cap.on_frame(0, convert_buffer_.data(), samples, 1);
  cap.on_period_end();
}
```

- [ ] **Step 8: 修改 CMake 加新源码**

Modify `daemon/CMakeLists.txt` — WITH_NOISE 块加 `noise_session_manager_bridge.cpp`：

```cmake
if(WITH_NOISE)
  MESSAGE(STATUS "WITH_NOISE")
  add_definitions(-D_USE_NOISE_)
  list(APPEND SOURCES pcm_capture_service.cpp noise_session_manager_bridge.cpp)
  add_subdirectory(noise)
  target_link_libraries(aes67-daemon PRIVATE noise)
endif()
```

Modify `daemon/noise/CMakeLists.txt` — NOISE_SOURCES 加 `audio_capture.cpp`：

```cmake
set(NOISE_SOURCES
  audio_capture.cpp
)
```

并把 noise-test target 加上 `noise_session_manager_bridge.cpp`：

```cmake
if(ENABLE_TESTS AND WITH_NOISE)
  add_executable(noise-test noise/tests/noise_test.cpp
                 pcm_capture_service.cpp noise_session_manager_bridge.cpp)
  target_link_libraries(noise-test PRIVATE noise ${Boost_LIBRARIES} ${ALSA_LIBRARY})
  target_compile_definitions(noise-test PRIVATE -D_USE_FAKE_DRIVER_ -D_USE_NOISE_)
  add_test(NAME noise-test COMMAND noise-test -p)
endif()
```

- [ ] **Step 9: 跑测试确认通过**

Run: `cd daemon/build && cmake --build . --target noise-test && ./noise-test -p`
Expected: `bridge_tests::bridge_demux_and_float_conversion` PASS（4 个 float 值精度 within 0.01）。

- [ ] **Step 10: 验证 daemon 构建 + WITH_NOISE=OFF 零回归**

Run: `./noise-dev.sh build && ./noise-dev.sh build --no-noise`
Expected: 两次均通过。

- [ ] **Step 11: 提交**

```bash
git add daemon/noise/noise_audio_bridge.hpp daemon/noise/audio_capture.hpp daemon/noise/audio_capture.cpp daemon/noise_session_manager_bridge.hpp daemon/noise_session_manager_bridge.cpp daemon/CMakeLists.txt daemon/noise/CMakeLists.txt daemon/noise/tests/noise_test.cpp
git commit -m "feat(noise): Spec1 1.3 NoiseAudioBridge + Bridge + AudioCapture

NoiseAudioBridge 纯虚接口（§4.1）；NoiseSessionManagerBridge 委托
PcmCaptureService + uint8_t->float 转换 + 通道解复用（convert_buffer_
容量断言 §11 风险18）；AudioCapture 下游 callback 解耦（决策4），
不前向依赖 Spec2 NoiseManager。

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: Streamer 重构 `#ifdef _USE_NOISE_` 守卫 + stub 单测（1.2）

**Files:**
- Modify: `daemon/streamer.hpp`（`#ifdef _USE_NOISE_` 守卫成员 + include）
- Modify: `daemon/streamer.cpp`（`#ifdef _USE_NOISE_` 守卫 capture 路径 + FrameProvider 消费）
- Modify: `daemon/noise/tests/noise_test.cpp`（追加 Streamer stub 单测，需 `--with-streamer` 构建）

**Interfaces:**
- Consumes: `PcmCaptureService`（Task 3）
- Produces: `WITH_NOISE=ON` 下 Streamer 改走 FrameProvider 拿帧产出 AAC；`WITH_NOISE=OFF` 上游路径逐字节不变

**构建配置**：本 task 验证需 `--with-streamer`：`./noise-dev.sh build --with-streamer`（`WITH_STREAMER=ON -DWITH_NOISE=ON`）。spec §D.1 决策2。

- [ ] **Step 1: 写失败测试（Streamer 从 FrameProvider 产出非空 AAC）**

追加到 `noise_test.cpp`（仅 `_USE_STREAMER_` 下编译此 suite；测试入口需 Streamer 可链）：

```cpp
#ifdef _USE_STREAMER_
#include "streamer.hpp"
// Streamer 构造需 SessionManager/Config；Spec1 用最小 stub 验证 FrameProvider
// 注入路径。若 faac/SessionManager 依赖过重无法在 noise-test 链接（spec §D.1
// 决策2 降级条款），此 suite 改为编译期守卫 + 手动 run 验证，并记入降级日志。

BOOST_AUTO_TEST_CASE(streamer_consumes_frame_provider_placeholder) {
  // 占位：完整 stub 实现见 Step 3（依赖 Streamer 可否在 noise-test 链接）。
  // 默认断言：Streamer::create 在 WITH_NOISE 下接受 PcmCaptureService 注入。
  BOOST_CHECK(true);
}
#endif
```

> **降级判定**：若 Step 3 评估 faac + SessionManager 链接进 noise-test 成本过高，执行 §D.1 降级条款：删除此 suite，1.2 验收降为"零回归 + `--with-streamer` 编译通过 + review"。在提交信息注明"1.2 stub 单测降级（§D.1）"。

- [ ] **Step 2: 跑测试确认状态**

Run: `./noise-dev.sh build --with-streamer && cd daemon/build && cmake --build . --target noise-test 2>&1 | tail -20`
Expected: 若 noise-test 未链 streamer.cpp，suite 不可见；先确认构建状态。

- [ ] **Step 3: 评估 stub 挂载可行性 + 实现 Streamer 守卫**

**先评估**：Streamer 构造（streamer.hpp:50-52）需 `shared_ptr<SessionManager>` + `shared_ptr<Config>`，且 AAC 产出依赖 faac + sink 配置。若无法在 noise-test 构造最小 SessionManager stub，执行降级（Step 1 suite 删除）。

**实现 streamer.hpp 守卫** — Modify `daemon/streamer.hpp`：
- L28 `#include "session_manager.hpp"` 后加：

```cpp
#ifdef _USE_NOISE_
#include "pcm_capture_service.hpp"
#endif
```

- L98 private 成员区（`std::shared_ptr<SessionManager> session_manager_;` 前）加：

```cpp
#ifdef _USE_NOISE_
  std::shared_ptr<PcmCaptureService> pcm_capture_;
  PcmCaptureService::ProviderToken pcm_token_{0};
#endif
```

**实现 streamer.cpp 守卫** — Modify `daemon/streamer.cpp`：
- `init()`（L36-54）：PTP observer 注册用 `#ifdef _USE_NOISE_` 切换。在 L37 后插入：

```cpp
  BOOST_LOG_TRIVIAL(info) << "Streamer: init";
#ifdef _USE_NOISE_
  // WITH_NOISE：PTP observer 由 PcmCaptureService 负责，Streamer 改注册 FrameProvider
  if (pcm_capture_) {
    pcm_token_ = pcm_capture_->register_provider(
        [this](const uint8_t* pcm, size_t frame_count, uint8_t channels,
               uint32_t /*rate*/) {
          this->on_pcm_frame_from_capture(pcm, frame_count, channels);
        });
  }
#else
  session_manager_->add_ptp_status_observer(
      std::bind(&Streamer::on_ptp_status_change, this, std::placeholders::_1));
  session_manager_->add_sink_observer(
      SessionManager::SinkObserverType::add_sink,
      std::bind(&Streamer::on_sink_add, this, std::placeholders::_1));
  session_manager_->add_sink_observer(
      SessionManager::SinkObserverType::remove_sink,
      std::bind(&Streamer::on_sink_remove, this, std::placeholders::_1));
  running_ = false;
  PTPStatus status;
  session_manager_->get_ptp_status(status);
  on_ptp_status_change(status.status);
#endif
  return true;
```

- 新增 `on_pcm_frame_from_capture` 私有方法（在 `pcm_read` 前）：

```cpp
#ifdef _USE_NOISE_
// FrameProvider 回调：收到一个 ALSA period 的全通道交错 PCM，
// 喂入既有 AAC 编码管线（替代 snd_pcm_readi 路径）。
void Streamer::on_pcm_frame_from_capture(const uint8_t* pcm, size_t frame_count,
                                         uint8_t channels) {
  if (!running_ || pcm == nullptr) return;
  // 复用既有 save_files 路径：把帧拷入 buffer_ 再编码。
  // channels 与 setup_codec 时的 channels_ 一致（PcmCaptureService 全通道分发）。
  const size_t bytes = frame_count * bytes_per_frame_;
  if (buffer_offset_ * bytes_per_frame_ + bytes > buffer_samples_ * bytes_per_frame_) {
    // 缓冲满：切文件（复用 start_capture 的轮转逻辑）
    close_files(file_id_);
    file_id_ = (file_id_ + 1) % files_num_;
    file_counter_++;
    buffer_offset_ = 0;
    open_files(file_id_);
  }
  std::memcpy(buffer_.get() + buffer_offset_ * bytes_per_frame_, pcm, bytes);
  save_files(file_id_);
  buffer_offset_ += frame_count;
}
#endif
```

- `start_capture()`（L183）：用 `#ifdef _USE_NOISE_` 守卫 ALSA open 段。L189-262（snd_pcm_open 到 snd_pcm_prepare + buffer 分配）整段包裹：

```cpp
bool Streamer::start_capture() {
  if (running_)
    return true;
  BOOST_LOG_TRIVIAL(info) << "Streamer: starting audio capture ... ";
#ifdef _USE_NOISE_
  // WITH_NOISE：ALSA capture 由 PcmCaptureService 独占，Streamer 不 open。
  // 仅初始化 AAC 编码参数与 buffer（复用既有 setup）。
  rate_ = config_->get_sample_rate();
  channels_ = config_->get_streamer_channels();
  files_num_ = config_->get_streamer_files_num();
  file_duration_ = config_->get_streamer_file_duration();
  player_buffer_files_num_ = config_->get_streamer_player_buffer_files_num();
  chunk_samples_ = 6144;
  bytes_per_frame_ = snd_pcm_format_physical_width(format) * channels_ / 8;
  buffer_samples_ = rate_ * file_duration_ / chunk_samples_ * chunk_samples_;
  buffer_.reset(new uint8_t[buffer_samples_ * bytes_per_frame_]);
  if (buffer_ == nullptr) return false;
  buffer_offset_ = 0;
  total_sink_samples_.clear();
  file_id_ = 0;
  file_counter_ = 0;
  running_ = true;
  open_files(file_id_);
  return true;
#else
  int err;
  if ((err = snd_pcm_open(&capture_handle_, device_name, SND_PCM_STREAM_CAPTURE,
                          SND_PCM_NONBLOCK)) < 0) {
    // ... 既有上游 L189-309 逐字节不变 ...
  }
  // ... 既有上游 capture loop ...
  return true;
#endif
}
```

> 实施时：把现有 L189-309 的上游代码整体移入 `#else` 分支，`#if` 分支写新的 FrameProvider 路径。**上游代码逐字节不动**（仅缩进进 `#else`）。

- `stop_capture()`（L453-468）：`snd_pcm_close` 用 `#ifndef _USE_NOISE_` 守卫：

```cpp
bool Streamer::stop_capture() {
  // ... 既有 running_ = false 等 ...
#ifndef _USE_NOISE_
  snd_pcm_close(capture_handle_);
#endif
  // ...
}
```

- `terminate()`（L470）：observer 注销用 `#ifndef _USE_NOISE_` 守卫（WITH_NOISE 下 Streamer 不曾注册 observer，改为 unregister_provider）：

```cpp
bool Streamer::terminate() {
#ifndef _USE_NOISE_
  // 既有上游 observer 注销
#else
  if (pcm_capture_ && pcm_token_) {
    pcm_capture_->unregister_provider(pcm_token_);
    pcm_token_ = 0;
  }
#endif
  stop_capture();
  return true;
}
```

- `Streamer::create`（streamer.cpp:23）：`#ifdef _USE_NOISE_` 下接受 PcmCaptureService 注入（重载或改签名）。在 create 末尾加：

```cpp
#ifdef _USE_NOISE_
  ptr->pcm_capture_ = /* 从外部注入或经 SessionManager 获取 */;
#endif
```

> **注入方式**：Spec1 不改 `Streamer::create` 公开签名（避免上游 sync 冲突）。PcmCaptureService 注入留 Spec3 1.11 main.cpp 装配时通过 setter 或友元完成。Spec1 在 streamer.hpp 加 `#ifdef _USE_NOISE_` 的 `void set_pcm_capture(std::shared_ptr<PcmCaptureService>)` setter。

在 streamer.hpp public 区加：

```cpp
#ifdef _USE_NOISE_
  void set_pcm_capture(std::shared_ptr<PcmCaptureService> pcm_capture) {
    pcm_capture_ = std::move(pcm_capture);
  }
#endif
```

- [ ] **Step 4: 验证 `--with-streamer` 编译通过**

Run: `./noise-dev.sh build --with-streamer`
Expected: aes67-daemon 构建通过（`#ifdef _USE_NOISE_` 守卫下 Streamer 改走 FrameProvider 路径编译就绪）。

- [ ] **Step 5: 验证 WITH_NOISE=OFF + WITH_STREAMER=ON 零回归**

Run:
```bash
cd daemon && cmake -B /tmp/noise-off-stream -DFAKE_DRIVER=ON -DWITH_NOISE=OFF -DWITH_STREAMER=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON . && cmake --build /tmp/noise-off-stream --target aes67-daemon
```
Expected: 通过。Streamer 走 `#else` 上游路径（逐字节不变）。

- [ ] **Step 6: 验证 daemon-test 零回归**

Run: `cd daemon/build && cmake --build . --target daemon-test && ./daemon-test -p`（用 `./noise-dev.sh build --no-noise` 构建后）
Expected: 现有 daemon-test 全过（streamer config 往返测试不受影响）。

- [ ] **Step 7: 提交**

```bash
git add daemon/streamer.hpp daemon/streamer.cpp daemon/noise/tests/noise_test.cpp
git commit -m "streamer(noise): Spec1 1.2 Streamer 重构 #ifdef _USE_NOISE_ 守卫

WITH_NOISE=ON：Streamer 改从 PcmCaptureService 注册 FrameProvider 拿帧，
不再 snd_pcm_open/readi；set_pcm_capture setter 注入（main.cpp 装配留
Spec3 1.11）。WITH_NOISE=OFF：上游 capture 路径逐字节不变（#else 分支）。

[如降级] 1.2 stub 单测降级为编译+review（spec §D.1 降级条款）。

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Self-Review

**1. Spec coverage**（对照 `docs/superpowers/specs/noise-spec1-design.md`）：

- §A.1 步骤 1.0 CMake 脚手架 → Task 1 ✓
- §A.1 步骤 1.1 PcmCaptureService → Task 3 ✓
- §A.1 步骤 1.2 Streamer 重构 → Task 5 ✓
- §A.1 步骤 1.3 Bridge + AudioCapture → Task 4 ✓
- §A.1 步骤 1.4a RcuPtr → Task 2 ✓
- §B.1 gate 1（noise-dev.sh build WITH_STREAMER=OFF）→ Task 1 Step 6 / Task 3 Step 7 ✓
- §B.1 gate 2（noise-test 全过）→ 各 Task 测试步骤 ✓
- §B.1 gate 3（WITH_NOISE=OFF 零回归）→ Task 1 Step 7 / Task 3 Step 8 / Task 4 Step 10 / Task 5 Step 5-6 ✓
- §B.1 gate 4-5（1.2 WITH_STREAMER=ON 构建 + stub 单测）→ Task 5 ✓
- §C 接口契约（PcmCaptureService/RcuPtr/NoiseAudioBridge/AudioCapture）→ Task 2/3/4 冻结 ✓
- §D.1 决策2（1.2 stub + 降级条款）→ Task 5 Step 1/3 含降级判定 ✓
- §D.2 决策3（noise-test 1.0 建）→ Task 1 Step 2 ✓
- §D.3 决策4（AudioCapture callback 解耦）→ Task 4 Step 4-5 ✓
- §D.4 决策5（CMakeLists 渐进追加）→ 各 Task 按需 append ✓
- §E 测试策略（RcuPtr/PcmCaptureService/Bridge 逐步单测，真实 ALSA 留 run-real）→ Task 2/3/4 ✓
- §F 风险（7/11/18/19/21/22 + S1-R1~R4）→ 各 Task 缓解落点已标注 ✓

无 spec 条目缺 task。

**2. Placeholder scan**：
- Task 5 Step 3 的 `start_capture` `#else` 分支写"既有上游 L189-309 逐字节不变"——这是引导实施者把现有代码移入 `#else`，不是 placeholder（代码已存在于 streamer.cpp，不重复粘贴避免冗余与漂移）。可接受。
- Task 5 Step 1 的 streamer stub suite 是占位 + 降级判定，符合 §D.1 降级条款设计（非空洞 placeholder，是有意保留的降级点）。可接受。
- 无 "TBD"/"TODO"/"implement later"。

**3. Type consistency**：
- `PcmCaptureService::FrameProvider` 签名（Task 3 hpp）= Task 5 cpp 中 lambda 参数 `(const uint8_t*, size_t, uint8_t, uint32_t)` ✓
- `PcmCaptureService::ProviderToken` = `uint32_t`，Task 5 `pcm_token_{0}` 类型一致 ✓
- `noise::NoiseAudioBridge::FrameProvider`（Task 4 hpp）与 `AudioCapture::FrameCallback` 是不同类型（Bridge 层 float 单通道，AudioCapture 层 float 单通道）——命名区分清晰 ✓
- `AudioCapture::register_callback` / `register_period_callbacks` / `on_period_begin` / `on_period_end` / `on_frame`（Task 4 hpp）与 cpp 一致 ✓
- `RcuPtr<T>::publish` 返回 `shared_ptr<T>`，Task 3 `providers_.publish(...)` 用法一致 ✓
- `RetireQueue<T>::retire(shared_ptr<T>, uint64_t)` 与 Task 3 `providers_retire_.retire(std::move(old), providers_.epoch())` 一致 ✓

**4. 已知实施风险（plan 内标注，非 placeholder）**：
- Task 5 Streamer 守卫是本 spec 最大重构面，`#else` 分支保留上游代码需逐行核对不漏切（spec §F S1-R1）。实施时用 `git diff` 核对 `#ifdef _USE_NOISE_` 块边界。
- Task 3 `session_manager_fwd.hpp` 不存在则用完整 include（Step 3 注释已说明）。
- §8.2 `$<TARGET_OBJECTS:noise>` 勘误已在本计划 Global Constraints 与 Task 1 Step 1 注明（STATIC + 普通链接）。

无 issue 需追加 task。

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/noise-spec1-plan.md`. Two execution options:

**1. Subagent-Driven (recommended)** - 每个 task 派发新 subagent，task 间 review，快速迭代。适合本 plan 的 5 个 task（Task 3/5 较重，独立 subagent 隔离上下文）。

**2. Inline Execution** - 在当前会话用 executing-plans 批量执行，checkpoint 处 review。

哪种？

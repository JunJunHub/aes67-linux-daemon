// daemon/noise/sse_broadcaster.hpp
// Spec4 Task 3：SSE 传输基础设施。
// 架构依据：docs/superpowers/specs/noise-spec4-design.md D-S4.1 +
//   docs/noise/architecture-design.md §5.1（SSE 端点）+ §11 风险 9/17
//   （RT 不阻塞 / 双缓冲 swap 背压）。
//
// 设计要点（D-S4.1）：
// - per-订阅者 SPSC 环形队列：单写者（capture 线程 on_period_end
//   push）+ 单读者（SSE handler 线程 drain）。
// - 实现选型（偏离 brief 的"lock-free atomic 索引"）：
//   brief 指定 std::vector<std::string> ring + atomic 读/写索引。但 SPSC
//   + drop-oldest 的纯 lock-free 实现有 ABA 隐患（writer 推进 read_ 与
//   reader 推进 read_ 竞争；cap 次后 writer 可能覆写 reader 正在读的
//   slot）。本实现改用 mutex + try_lock：
//     - try_lock 永不阻塞（满足风险 9 RT 不阻塞硬约束）。
//     - try_lock 失败（reader 正 drain，罕见：4ms push vs 100ms drain）
//       时 drop 新事件 + dropped_++（背压）。
//     - 满则 drop oldest + dropped_++。
//     - 定长 ring（预分配 capacity 个 std::string slot），稳态无堆分配
//       （slot 的 string 复用既有 capacity，赋值不 realloc）。
//   与 route_to_ref_comparators 持 ref_mutex_ 同模式：短临界区（memcpy
//   + 索引更新），不影响 RT 预算。
// - 背压（风险 17）：满则 drop oldest，保证 capture 线程 push 永不阻塞。
// - 订阅注册/注销：mutex 保护 subscribers_ vector（低频控制平面操作，HTTP
//   线程 subscribe/unsubscribe）。push 持锁拷贝 shared_ptr 列表后锁外
//   遍历 try_push（与 route_to_ref_comparators 同模式）。
// - 事件字符串（"data: {...}\n\n" 完整 SSE 帧）由调用方组装，
//   broadcaster 只透传。base64 编码等重活在调用方组装阶段完成（capture
//   线程的 base64 编码若每 period 一次且帧不大，可接受；详见
//   on_period_end 注释）。
//
// 线程模型：
//   - subscribe/unsubscribe：HTTP 控制线程（低频）。
//   - push：capture 线程 on_period_end（RT，必须 non-blocking）。
//   - drain：SSE handler 线程（cpp-httplib 工作线程，drain + socket write）。
//   - dropped_count()/subscriber_count()：测试或监控线程（持锁或 atomic）。
#ifndef NOISE_SSE_BROADCASTER_HPP_
#define NOISE_SSE_BROADCASTER_HPP_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace noise {

// 单订阅者 SPSC 环形队列（mutex + try_lock 实现，非阻塞）。
// 容量固定（构造时确定，默认 64），预分配 slots_，稳态零堆分配。
class SseSubscriberQueue {
 public:
  explicit SseSubscriberQueue(size_t capacity = 64);

  // 写者（capture 线程）：try_push 一条 SSE 帧（"data: {...}\n\n"）。
  // 满则 drop oldest + dropped_++；try_lock 失败则 drop 新事件 + dropped_++。
  // 返回 true 若入队成功（可能伴随 drop oldest）。
  bool try_push(const std::string& event);

  // 读者（SSE handler 线程）：try_drain 取出所有事件追加到 out。
  // 非阻塞：try_lock 失败或队列空则返回 false。
  // 返回 true 若取到 >=1 事件。
  bool try_drain(std::vector<std::string>& out);

  size_t dropped_count() const {
    return dropped_.load(std::memory_order_relaxed);
  }
  size_t pushed_count() const {
    return pushed_.load(std::memory_order_relaxed);
  }
  size_t drained_count() const {
    return drained_.load(std::memory_order_relaxed);
  }
  // 近似当前队列长度（持锁读，与 push/drain 互斥但临界区极短）。
  size_t size_approx() const;

 private:
  const size_t capacity_;
  mutable std::mutex mutex_;
  std::vector<std::string> slots_;  // 预分配 capacity_ 个空 string
  size_t head_{0};                  // 下一个读位置（mutex 保护）
  size_t tail_{0};                  // 下一个写位置（mutex 保护）
  size_t count_{0};                 // 当前元素数（mutex 保护）
  std::atomic<size_t> dropped_{0};
  std::atomic<size_t> pushed_{0};
  std::atomic<size_t> drained_{0};
};

// 订阅者句柄。shared_ptr 持有队列，handler 线程退出时 release。
// 不可拷贝（避免多 handle 共享同一队列语义混乱），可 move。
struct SseSubscriberHandle {
  uint64_t id{0};
  std::shared_ptr<SseSubscriberQueue> queue;
  SseSubscriberHandle() = default;
  SseSubscriberHandle(uint64_t i, std::shared_ptr<SseSubscriberQueue> q)
      : id(i), queue(std::move(q)) {}
  SseSubscriberHandle(SseSubscriberHandle&&) = default;
  SseSubscriberHandle& operator=(SseSubscriberHandle&&) = default;
  SseSubscriberHandle(const SseSubscriberHandle&) = delete;
  SseSubscriberHandle& operator=(const SseSubscriberHandle&) = delete;
  explicit operator bool() const { return queue != nullptr; }
};

// SseBroadcaster：管理多个订阅者队列，capture 线程 push 到全部。
// 一个 broadcaster 对应一类事件流（如 per-sensor metrics、per-sink PCM、
// alerts）。NoiseManager 持有多个 broadcaster 实例。
class SseBroadcaster {
 public:
  SseBroadcaster() = default;
  ~SseBroadcaster();

  // HTTP 控制线程：注册一个订阅者，返回 handle（持队列 shared_ptr）。
  // capacity 指定队列容量（默认 64）。
  SseSubscriberHandle subscribe(size_t capacity = 64);

  // HTTP 控制线程：注销指定 id 的订阅者。
  // handler 线程仍持 SseSubscriberHandle（shared_ptr 队列），可继续 drain
  // 直到 handler 退出（共享指针延长队列生命周期）。unsubscribe 仅从注册表
  // 移除 id，使后续 push 不再向该队列投递。
  bool unsubscribe(uint64_t id);

  // capture 线程：push 一条 SSE 帧到所有订阅者队列。
  // 非阻塞：持 subscribers_mutex_ 拷贝 shared_ptr 列表（短临界区），
  // 锁外遍历各队列 try_push（满或锁竞争则 drop，不阻塞）。
  void push(const std::string& event);

  // 监控/测试：当前订阅者数量。
  size_t subscriber_count() const;

  // 监控/测试：累计 dropped 总数（所有订阅者队列）。
  size_t total_dropped() const;

 private:
  struct SubscriberEntry {
    uint64_t id{0};
    std::shared_ptr<SseSubscriberQueue> queue;
  };
  mutable std::mutex subscribers_mutex_;
  std::vector<SubscriberEntry> subscribers_;
  std::atomic<uint64_t> next_id_{1};
};

}  // namespace noise

#endif  // NOISE_SSE_BROADCASTER_HPP_

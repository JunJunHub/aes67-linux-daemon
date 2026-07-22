// daemon/noise/sse_broadcaster.cpp
// Spec4 Task 3：SseBroadcaster 实现。详见 sse_broadcaster.hpp 注释。
#include "sse_broadcaster.hpp"

#include <algorithm>
#include <utility>

namespace noise {

// ── SseSubscriberQueue ──────────────────────────────────────────────────

SseSubscriberQueue::SseSubscriberQueue(size_t capacity) : capacity_(capacity) {
  // 预分配 capacity_ 个空 string slot，稳态 try_push 赋值复用既有 capacity
  // （string move 赋值不 realloc，short-string-optimization 亦可能命中）。
  slots_.resize(capacity_);
}

bool SseSubscriberQueue::try_push(const std::string& event) {
  // try_lock 非阻塞：reader（SSE handler 线程）持锁 drain 时返回 false。
  // drain 频率远低于 push（drain 每 ~ms 级 socket write 完成一次，push
  // 每 4ms period 一次），锁竞争罕见；竞争时 drop 新事件 + dropped_++
  // （背压，风险 9/17）。
  std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (count_ >= capacity_) {
    // 满则 drop oldest：head 推进 1，count 减 1，再写入新事件。
    slots_[tail_] = event;
    tail_ = (tail_ + 1) % capacity_;
    head_ = (head_ + 1) % capacity_;  // drop oldest
    dropped_.fetch_add(1, std::memory_order_relaxed);
    pushed_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  slots_[tail_] = event;
  tail_ = (tail_ + 1) % capacity_;
  ++count_;
  pushed_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool SseSubscriberQueue::try_drain(std::vector<std::string>& out) {
  std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock())
    return false;
  if (count_ == 0)
    return false;
  out.reserve(out.size() + count_);
  for (size_t i = 0; i < count_; ++i) {
    out.push_back(std::move(slots_[head_]));
    slots_[head_].clear();  // 复位 slot（保留 capacity，下次 push 复用）
    slots_[head_].shrink_to_fit();  // 避免空 string 持有堆内存
    head_ = (head_ + 1) % capacity_;
  }
  drained_.fetch_add(count_, std::memory_order_relaxed);
  count_ = 0;
  return true;
}

size_t SseSubscriberQueue::size_approx() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return count_;
}

// ── SseBroadcaster ──────────────────────────────────────────────────────

SseBroadcaster::~SseBroadcaster() {
  // 析构时清空订阅者列表。handler 线程持有的 shared_ptr<queue> 自然延长
  // 队列生命周期直到 handler 退出（push 已不再投递）。
  std::lock_guard<std::mutex> lock(subscribers_mutex_);
  subscribers_.clear();
}

SseSubscriberHandle SseBroadcaster::subscribe(size_t capacity) {
  auto queue = std::make_shared<SseSubscriberQueue>(capacity);
  uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    subscribers_.push_back({id, queue});
  }
  return SseSubscriberHandle{id, queue};
}

bool SseBroadcaster::unsubscribe(uint64_t id) {
  std::lock_guard<std::mutex> lock(subscribers_mutex_);
  auto it = std::find_if(subscribers_.begin(), subscribers_.end(),
                         [id](const SubscriberEntry& e) { return e.id == id; });
  if (it == subscribers_.end())
    return false;
  subscribers_.erase(it);
  return true;
}

void SseBroadcaster::push(const std::string& event) {
  // 持锁拷贝 shared_ptr 列表（短临界区：vector copy ~ns 级），锁外遍历
  // 各队列 try_push（try_push 内部各持各的 mutex，互不影响）。
  // 锁外遍历避免 push 期间 subscribe/unsubscribe 等待 subscribers_mutex_。
  std::vector<std::shared_ptr<SseSubscriberQueue>> snapshots;
  {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    snapshots.reserve(subscribers_.size());
    for (const auto& e : subscribers_)
      snapshots.push_back(e.queue);
  }
  for (auto& q : snapshots) {
    if (q)
      q->try_push(event);
  }
}

bool SseBroadcaster::has_subscribers() const {
  // 短临界区：仅检查 vector empty，不分配不阻塞（RT 安全）。
  // 与 subscriber_count() 同模式，但省去返回 size 的开销（虽然差异可忽略）。
  std::lock_guard<std::mutex> lock(subscribers_mutex_);
  return !subscribers_.empty();
}

size_t SseBroadcaster::subscriber_count() const {
  std::lock_guard<std::mutex> lock(subscribers_mutex_);
  return subscribers_.size();
}

size_t SseBroadcaster::total_dropped() const {
  std::lock_guard<std::mutex> lock(subscribers_mutex_);
  size_t total = 0;
  for (const auto& e : subscribers_) {
    if (e.queue)
      total += e.queue->dropped_count();
  }
  return total;
}

}  // namespace noise

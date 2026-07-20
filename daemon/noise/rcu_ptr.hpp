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

  // 回收 retire_epoch + 1 < current_epoch 的条目（即 current_epoch >=
  // retire_epoch + 2， RT 已穿越 >=2 静止点，旧裸指针不再被持有）。
  void reclaim_older_than(uint64_t current_epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
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
// 单一读者约束（Phase 1/3 通用）：load()/advance_epoch() 仅由 capture
// 线程调用。
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
  std::atomic<T*> ptr_{nullptr};  // RT 快路径（裸指针原子，lock-free）
  std::atomic<uint64_t> epoch_{0};  // 单调递增静止点计数
  std::shared_ptr<T> current_owner_;  // 控制线程侧持有，保证当前对象存活
};

}  // namespace noise

#endif  // NOISE_RCU_PTR_HPP_

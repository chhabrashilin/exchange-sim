// Bounded lock-free single-producer / single-consumer ring.
#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <stdexcept>
#include <type_traits>

#include "exsim/common.hpp"
#include "exsim/memory.hpp"

namespace exsim {

// The pipeline's only synchronization. Its properties:
//  * Power-of-two capacity: wrap is a mask, never a modulo. Indices are free-running 64-bit
//    counters, so full/empty is plain subtraction and never needs a wasted slot.
//  * Acquire/release only. The producer publishes a slot with a release store of tail_, and the
//    consumer frees it with a release store of head_. No seq_cst fences, no CAS.
//  * The producer-owned line holds {tail_, cached head}, the consumer-owned line {head_, cached
//    tail}, and the two sit kFalseSharingRange apart. In steady state each side reads only its own
//    line, and touches the other side's line only when its cached view says the ring looks
//    full/empty (Rigtorp's cached-index optimization).
//  * front()/pop() let the consumer read in place without a copy.
template <class T>
class SpscQueue {
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  explicit SpscQueue(std::size_t capacity)
      : cap_(std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity)), mask_(cap_ - 1), buf_(cap_) {}

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  // Producer side.
  EXSIM_ALWAYS_INLINE bool try_push(const T& v) noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (EXSIM_UNLIKELY(t - head_cache_ == cap_)) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (t - head_cache_ == cap_) return false;
    }
    buf_[t & mask_] = v;
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  // Consumer side: pointer to the oldest element, or nullptr if empty. Valid until pop().
  EXSIM_ALWAYS_INLINE T* front() noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    if (h == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (h == tail_cache_) return nullptr;
    }
    return &buf_[h & mask_];
  }

  // Consumer side: start pulling an already-published element `ahead` slots past the front into
  // this core's cache. The element was just written on the producer's core, so reading it costs a
  // cross-core transfer, and issuing it early overlaps that transfer with work on the current
  // element. Slots not yet published are never touched: prefetching a line the producer is about to
  // write would bounce it between cores.
  EXSIM_ALWAYS_INLINE void prefetch(std::size_t ahead) const noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    if (tail_cache_ - h > ahead) {
      const auto* p = reinterpret_cast<const char*>(&buf_[(h + ahead) & mask_]);
      __builtin_prefetch(p);
      __builtin_prefetch(p + sizeof(T) - 1);
    }
  }

  EXSIM_ALWAYS_INLINE void pop() noexcept {
    head_.store(head_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

  EXSIM_ALWAYS_INLINE bool try_pop(T& out) noexcept {
    T* p = front();
    if (p == nullptr) return false;
    out = *p;
    pop();
    return true;
  }

  std::size_t capacity() const noexcept { return cap_; }
  std::size_t size_approx() const noexcept {
    return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
  }

 private:
  alignas(kFalseSharingRange) std::atomic<std::size_t> tail_{0};
  std::size_t head_cache_ = 0;
  alignas(kFalseSharingRange) std::atomic<std::size_t> head_{0};
  std::size_t tail_cache_ = 0;
  alignas(kFalseSharingRange) const std::size_t cap_;
  const std::size_t mask_;
  PageArray<T> buf_;
};

}  // namespace exsim

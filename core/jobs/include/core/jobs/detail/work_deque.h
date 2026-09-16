#pragma once

// Chase-Lev work-stealing deque (Chase & Lev 2005; memory model per Lê et al. 2013).
// One owner pushes and pops at the bottom; any number of thieves steal from the top.
// Fixed capacity (a power of two); push reports failure when full so the caller can fall
// back to another queue or run the job inline.

#include <core/base/assert.h>
#include <core/base/types.h>
#include <core/memory/memory.h>

#include <atomic>
#include <bit>
#include <cstring>
#include <new>
#include <type_traits>

namespace engine::jobs::detail {

template <class T>
class WorkDeque {
  static_assert(std::is_trivially_copyable_v<T>, "WorkDeque: T must be trivially copyable");

 public:
  explicit WorkDeque(u32 capacity) : mask_(capacity - 1) {
    ENGINE_VERIFY(capacity >= 2 && std::has_single_bit(capacity),
                  "WorkDeque: capacity must be a power of two");
    buffer_ =
        static_cast<T*>(mem::allocate(sizeof(T) * capacity, alignof(T) > 64 ? alignof(T) : 64));
  }
  ~WorkDeque() {
    mem::deallocate(buffer_, sizeof(T) * (mask_ + 1), alignof(T) > 64 ? alignof(T) : 64);
  }
  WorkDeque(const WorkDeque&) = delete;
  WorkDeque& operator=(const WorkDeque&) = delete;

  u32 capacity() const noexcept { return mask_ + 1; }

  // Approximate; exact only when called by the owner with no thieves active.
  u32 size() const noexcept {
    const i64 b = bottom_.load(std::memory_order_relaxed);
    const i64 t = top_.load(std::memory_order_relaxed);
    return b > t ? static_cast<u32>(b - t) : 0;
  }

  // Owner only. Returns false when full.
  bool push(const T& item) noexcept {
    const i64 b = bottom_.load(std::memory_order_relaxed);
    const i64 t = top_.load(std::memory_order_acquire);
    if (b - t >= static_cast<i64>(capacity())) return false;
    buffer_[static_cast<u64>(b) & mask_] = item;
    std::atomic_thread_fence(std::memory_order_release);
    bottom_.store(b + 1, std::memory_order_relaxed);
    return true;
  }

  // Owner only. LIFO: the most recently pushed item, for cache locality.
  bool pop(T& out) noexcept {
    const i64 b = bottom_.load(std::memory_order_relaxed) - 1;
    bottom_.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    i64 t = top_.load(std::memory_order_relaxed);
    if (t <= b) {
      out = buffer_[static_cast<u64>(b) & mask_];
      if (t == b) {
        // Last element: race with thieves for it.
        if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                          std::memory_order_relaxed)) {
          bottom_.store(b + 1, std::memory_order_relaxed);
          return false;
        }
        bottom_.store(b + 1, std::memory_order_relaxed);
      }
      return true;
    }
    bottom_.store(b + 1, std::memory_order_relaxed);
    return false;
  }

  // Any thread. FIFO from the top: the oldest item, which is the least likely to be in the
  // owner's cache.
  bool steal(T& out) noexcept {
    i64 t = top_.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const i64 b = bottom_.load(std::memory_order_acquire);
    if (t < b) {
      out = buffer_[static_cast<u64>(t) & mask_];
      return top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                          std::memory_order_relaxed);
    }
    return false;
  }

 private:
  alignas(64) std::atomic<i64> top_{0};
  alignas(64) std::atomic<i64> bottom_{0};
  alignas(64) T* buffer_ = nullptr;
  u32 mask_;
};

}  // namespace engine::jobs::detail

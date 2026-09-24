#pragma once

// A bounded single-producer, single-consumer ring of trivially copyable items: the channel between
// the thread that controls the mix and the thread that renders it (docs/subsystems/audio.md, "The
// thread model").
//
// Why a ring and not a mutex-guarded vector: the consumer is an audio callback running at the
// operating system's real-time priority, and a lock it has to take is a lock a lower-priority
// thread can be holding when the device asks for the next block — priority inversion, heard as a
// dropout. Here both ends are wait-free: `try_push` and `try_pop` each do one acquire load in the
// common case (and none at all while the cached index says there is room or there are items), a
// copy, and one release store, and neither ever waits for the other. A full ring refuses a push
// and an empty one refuses a pop; what to do about that is the caller's decision, made where it
// has the context to make it.
//
// The indices are 64-bit and never wrap in practice (at a million items a second that is half a
// million years), so "full" is `tail - head == capacity` with no ABA and no wasted slot.
// Producer-owned and consumer-owned state sit on separate cache lines, and each side keeps a
// private copy of the other side's index so that a steady stream costs a shared-line read only
// when the copy says the ring is full or empty.
//
// It lives in this module rather than in core/containers because a capability does not edit
// core/ (ADR-0027); `core/containers`' planned `RingBuffer` (plan 11 §11.2) is where it goes the
// day a second user appears.

#include <core/base/assert.h>
#include <core/base/types.h>
#include <core/containers/vector.h>

#include <atomic>
#include <bit>
#include <type_traits>

namespace engine::audio {

template <class T>
class SpscQueue {
  static_assert(std::is_trivially_copyable_v<T>, "SpscQueue copies items with plain assignment");
  static_assert(std::atomic<u64>::is_always_lock_free, "the audio thread may not take a lock");

 public:
  // Capacity is rounded up to a power of two and allocated here, once.
  explicit SpscQueue(u32 min_capacity) {
    const u32 capacity = std::bit_ceil(min_capacity < 2u ? 2u : min_capacity);
    slots_.resize_exact(capacity);
    mask_ = capacity - 1u;
  }
  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  // Producer only. False when the ring is full; nothing is written.
  bool try_push(const T& item) noexcept {
    const u64 tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_cache_ > mask_) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (tail - head_cache_ > mask_) return false;
    }
    slots_[static_cast<u32>(tail & mask_)] = item;
    tail_.store(tail + 1u, std::memory_order_release);
    return true;
  }

  // Consumer only. False when the ring is empty; `out` is untouched.
  bool try_pop(T& out) noexcept {
    const u64 head = head_.load(std::memory_order_relaxed);
    if (head == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (head == tail_cache_) return false;
    }
    out = slots_[static_cast<u32>(head & mask_)];
    head_.store(head + 1u, std::memory_order_release);
    return true;
  }

  u32 capacity() const noexcept { return mask_ + 1u; }

  // A snapshot from either side; exact only when the other side is quiescent.
  u32 size_approx() const noexcept {
    const u64 tail = tail_.load(std::memory_order_acquire);
    const u64 head = head_.load(std::memory_order_acquire);
    return static_cast<u32>(tail - head);
  }

 private:
  // Producer's line.
  alignas(64) std::atomic<u64> tail_{0};
  u64 head_cache_ = 0;
  // Consumer's line.
  alignas(64) std::atomic<u64> head_{0};
  u64 tail_cache_ = 0;
  // Read by both, written by neither after construction.
  alignas(64) Vector<T> slots_;
  u32 mask_ = 0;
};

}  // namespace engine::audio

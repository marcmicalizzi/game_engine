#pragma once

// Chase-Lev work-stealing deque (Chase & Lev, SPAA 2005; the C11 memory orders of Le, Pop, Cohen
// and Zappa Nardelli, PPoPP 2013). One owner pushes and pops at the bottom; any number of thieves
// steal from the top. Fixed capacity (a power of two); push reports failure when full so the
// caller can fall back to another queue or run the job inline.
//
// The orders, and why each is there (docs/subsystems/jobs.md, "What a job may assume"):
// - Every store of `bottom_` is a release. A push's is what publishes the slot it wrote to the
// thief
//   whose acquire load of `bottom_` reads it. The paper writes a release fence and a relaxed store;
//   a release store orders the same writes before that store, is the same instruction on x86 (a
//   plain mov) and an stlr on ARM, and is what ThreadSanitizer models (it ignores fences, so it saw
//   every stolen job as unpublished: T179's 138 reports). The owner's other stores (pop's decrement
//   and its restore) are releases too: C++20 dropped a thread's later plain stores from a release
//   sequence (P0982), so a thief that reads `bottom_` from one of them would otherwise acquire
//   nothing, where the paper's C11 proof had them in the push's release sequence.
// - A slot is read and written as relaxed atomic words. A thief copies the slot before its CAS on
//   `top_` decides whether the item is its own (Chase-Lev's read-before-CAS: once the CAS succeeds
//   the owner may refill the slot); a thief whose CAS loses may have read a slot the owner was
//   rewriting, and with plain accesses that read is a data race, undefined behaviour, whatever the
//   discarded value. The paper's buffer is atomic for that reason. A torn copy is harmless: it is
//   kept only when the CAS wins, and then no write to that slot can have happened since the push
//   that published it (the owner reaches it again only after an acquire load of `top_` reads a
//   value past it, which orders the thief's read before the owner's write).
// - push loads `top_` with acquire, for that last reason.
// - pop's seq_cst fence between its store of `bottom_` and its load of `top_`, and steal's between
//   its loads of `top_` and `bottom_`, are what stop the owner and a thief both taking the last
//   item (a store-load order no weaker fence gives, x86 included: an mfence or locked instruction).
// - The CASes on `top_` are seq_cst on success, relaxed on failure, as in the paper.

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
  static_assert(alignof(T) <= alignof(u64), "WorkDeque: T is stored as 8-byte words");
  static_assert(std::atomic_ref<u64>::is_always_lock_free,
                "WorkDeque: slot words must be lock-free");
  static constexpr u32 k_words = static_cast<u32>((sizeof(T) + sizeof(u64) - 1) / sizeof(u64));
  static constexpr usize k_align = 64;

 public:
  explicit WorkDeque(u32 capacity) : mask_(capacity - 1) {
    ENGINE_VERIFY(capacity >= 2 && std::has_single_bit(capacity),
                  "WorkDeque: capacity must be a power of two");
    // Not written here: a slot is read only after a push wrote it, so the pages a deque never
    // reached stay untouched.
    words_ = static_cast<u64*>(mem::allocate(bytes(), k_align));
  }
  ~WorkDeque() { mem::deallocate(words_, bytes(), k_align); }
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
    store_slot(b, item);
    bottom_.store(b + 1, std::memory_order_release);
    return true;
  }

  // Owner only. LIFO: the most recently pushed item, for cache locality.
  bool pop(T& out) noexcept {
    const i64 b = bottom_.load(std::memory_order_relaxed) - 1;
    bottom_.store(b, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    i64 t = top_.load(std::memory_order_relaxed);
    if (t <= b) {
      out = load_slot(b);
      if (t == b) {
        // Last element: race with thieves for it.
        const bool won = top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                                      std::memory_order_relaxed);
        bottom_.store(b + 1, std::memory_order_release);
        return won;
      }
      return true;
    }
    bottom_.store(b + 1, std::memory_order_release);
    return false;
  }

  // Any thread. FIFO from the top: the oldest item, which is the least likely to be in the
  // owner's cache.
  bool steal(T& out) noexcept {
    i64 t = top_.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const i64 b = bottom_.load(std::memory_order_acquire);
    if (t < b) {
      // Read before the CAS: once it succeeds the owner may write the slot again.
      const T item = load_slot(t);
      if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                        std::memory_order_relaxed)) {
        return false;  // another thief, or the owner's pop of the last item, took it
      }
      out = item;
      return true;
    }
    return false;
  }

 private:
  usize bytes() const noexcept { return sizeof(u64) * k_words * (static_cast<usize>(mask_) + 1); }

  u64* slot(i64 index) const noexcept {
    return words_ + (static_cast<u64>(index) & mask_) * k_words;
  }

  void store_slot(i64 index, const T& item) noexcept {
    u64 words[k_words] = {};
    std::memcpy(words, &item, sizeof(T));
    u64* dst = slot(index);
    for (u32 w = 0; w < k_words; ++w)
      std::atomic_ref<u64>(dst[w]).store(words[w], std::memory_order_relaxed);
  }

  T load_slot(i64 index) const noexcept {
    u64 words[k_words];
    u64* src = slot(index);
    for (u32 w = 0; w < k_words; ++w)
      words[w] = std::atomic_ref<u64>(src[w]).load(std::memory_order_relaxed);
    T item;
    std::memcpy(&item, words, sizeof(T));
    return item;
  }

  alignas(64) std::atomic<i64> top_{0};
  alignas(64) std::atomic<i64> bottom_{0};
  alignas(64) u64* words_ = nullptr;
  u32 mask_;
};

}  // namespace engine::jobs::detail

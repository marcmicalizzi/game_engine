#pragma once

// Bounded multi-producer multi-consumer queue (Vyukov). Used as each pool's inbox for jobs
// submitted from outside the pool. Fixed power-of-two capacity; push reports failure when
// full.

#include <core/base/assert.h>
#include <core/base/types.h>
#include <core/memory/memory.h>

#include <atomic>
#include <bit>
#include <new>
#include <type_traits>

namespace engine::jobs::detail {

template <class T>
class MpmcQueue {
  static_assert(std::is_trivially_copyable_v<T>, "MpmcQueue: T must be trivially copyable");

 public:
  explicit MpmcQueue(u32 capacity) : mask_(capacity - 1) {
    ENGINE_VERIFY(capacity >= 2 && std::has_single_bit(capacity), "MpmcQueue: capacity must be a power of two");
    cells_ = static_cast<Cell*>(mem::allocate(sizeof(Cell) * capacity, alignof(Cell)));
    for (u32 i = 0; i < capacity; ++i) {
      new (cells_ + i) Cell();
      cells_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }
  ~MpmcQueue() { mem::deallocate(cells_, sizeof(Cell) * (mask_ + 1), alignof(Cell)); }
  MpmcQueue(const MpmcQueue&) = delete;
  MpmcQueue& operator=(const MpmcQueue&) = delete;

  u32 capacity() const noexcept { return mask_ + 1; }

  bool try_push(const T& item) noexcept {
    u64 pos = enqueue_pos_.load(std::memory_order_relaxed);
    while (true) {
      Cell& cell = cells_[pos & mask_];
      const u64 seq = cell.sequence.load(std::memory_order_acquire);
      const i64 diff = static_cast<i64>(seq) - static_cast<i64>(pos);
      if (diff == 0) {
        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          cell.item = item;
          cell.sequence.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;  // full
      } else {
        pos = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }
  }

  bool try_pop(T& out) noexcept {
    u64 pos = dequeue_pos_.load(std::memory_order_relaxed);
    while (true) {
      Cell& cell = cells_[pos & mask_];
      const u64 seq = cell.sequence.load(std::memory_order_acquire);
      const i64 diff = static_cast<i64>(seq) - static_cast<i64>(pos + 1);
      if (diff == 0) {
        if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          out = cell.item;
          cell.sequence.store(pos + mask_ + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;  // empty
      } else {
        pos = dequeue_pos_.load(std::memory_order_relaxed);
      }
    }
  }

  // Approximate.
  bool empty_hint() const noexcept {
    return enqueue_pos_.load(std::memory_order_relaxed) == dequeue_pos_.load(std::memory_order_relaxed);
  }

 private:
  struct Cell {
    std::atomic<u64> sequence{0};
    T item{};
  };

  alignas(64) std::atomic<u64> enqueue_pos_{0};
  alignas(64) std::atomic<u64> dequeue_pos_{0};
  alignas(64) Cell* cells_ = nullptr;
  u32 mask_;
};

}  // namespace engine::jobs::detail

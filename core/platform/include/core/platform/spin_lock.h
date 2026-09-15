#pragma once

// A test-and-test-and-set spinlock with constant initialization. For registries that are
// touched during static initialization (where a std::mutex global would be a construction-
// order hazard) and for critical sections of a few dozen instructions. Not fair; never hold
// it across anything that can block, allocate, or log.

#include <core/base/macros.h>
#include <core/platform/thread.h>

#include <atomic>

namespace engine::platform {

class SpinLock {
 public:
  constexpr SpinLock() noexcept = default;
  ENGINE_NON_COPYABLE(SpinLock);

  void lock() noexcept {
    while (flag_.exchange(true, std::memory_order_acquire)) {
      while (flag_.load(std::memory_order_relaxed))
        pause_cpu();
    }
  }
  bool try_lock() noexcept { return !flag_.exchange(true, std::memory_order_acquire); }
  void unlock() noexcept { flag_.store(false, std::memory_order_release); }

 private:
  std::atomic<bool> flag_{false};
};

}  // namespace engine::platform

#pragma once

// **The large-allocation log** (docs/subsystems/memory.md, "Who sized the largest blocks"): while
// it is on, every allocation through `mem::allocate` of `k_large_allocation_bytes` or more that is
// still live is kept in a fixed table with its address, size, tag and the return addresses of its
// call stack, so a memory report can say which line of source sized a block the address-space walk
// found (core/platform/address_space.h; engine-view's `--memory-report`).
//
// Off — the default — it costs an allocation of that size one relaxed load, and anything smaller
// nothing beyond the size compare it already makes; on, a large allocation takes a spin lock and a
// stack capture, which is nothing beside committing 16 MiB. The table is static: turning the log on
// allocates nothing. Allocations made while it was off are not in it, so a host turns it on before
// its load; a table that fills counts what it could not keep.

#include <core/base/types.h>
#include <core/memory/memory.h>

namespace engine::mem {

inline constexpr usize k_large_allocation_bytes = usize{16} << 20;
inline constexpr u32 k_large_allocation_slots = 512;
inline constexpr u32 k_large_allocation_frames = 12;

struct LargeAllocation {
  u64 address = 0;
  u64 bytes = 0;
  TagId tag{};
  u16 depth = 0;
  void* stack[k_large_allocation_frames]{};
};

void set_large_allocation_log(bool on) noexcept;
bool large_allocation_log() noexcept;
// Copies the live large allocations (up to `capacity`) and returns how many there are; those past
// a full table are counted in `large_allocations_dropped`.
u32 large_allocations(LargeAllocation* out, u32 capacity) noexcept;
u64 large_allocations_dropped() noexcept;

}  // namespace engine::mem

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

// The default floor of the log, and the least a host may lower it to
// (`set_large_allocation_floor`): below 1 MiB the table would be the heap, and every allocation would pay the log's compare.
inline constexpr usize k_large_allocation_bytes = usize{16} << 20;
inline constexpr usize k_large_allocation_min_bytes = usize{1} << 20;
inline constexpr u32 k_large_allocation_slots = 8192;
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
// **The log's floor** (W51-2; memory.md, "Who sized the largest blocks"): the least size it keeps,
// `k_large_allocation_bytes` unless a host lowers it, clamped to `k_large_allocation_min_bytes` or
// more. A memory report by owner wants the blocks between 1 and 16 MiB too (a tile's tables, a
// generator's arrays), which together hold gigabytes no 16 MiB block names. Set it before the log
// is turned on: it empties the table, since a block kept under one floor and freed under another
// would otherwise stay in it. An allocation under 1 MiB still pays nothing for the log.
void set_large_allocation_floor(usize bytes) noexcept;
usize large_allocation_floor() noexcept;
// Copies the live large allocations (up to `capacity`) and returns how many there are; those past
// a full table are counted in `large_allocations_dropped`.
u32 large_allocations(LargeAllocation* out, u32 capacity) noexcept;
u64 large_allocations_dropped() noexcept;

}  // namespace engine::mem

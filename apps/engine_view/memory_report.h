#pragma once

// **`engine-view --memory-report`** (docs/subsystems/apps.md, "--memory-report: where the commit
// is"; roadmap T129): where this process's commit is, taken twice — at the first frame and at exit
// — as the summary's `memory` block, a block of `--report`'s file, and one log line each.
//
// Each snapshot is the address space walked by kind (core/platform/address_space.h: private, mapped
// file, image; committed, resident, reserved only), the owners the engine can name — its own heap
// (`mem::total_stats`), the C runtime's heaps as they count themselves, the device's allocator by
// heap kind (gfx::DeviceMemoryReport) — and the largest private allocations, each matched to the
// large allocation that made it when the large-allocation log has it (core/memory, "Who sized the
// largest blocks"), with that allocation's call stack as module-relative addresses. Live large
// allocations are also summed by call stack with each site's resident estimate, which is what says
// *who sized memory nobody touches*.
//
// **Off — no flag — none of it runs**: the large-allocation log stays off, nothing is walked, and
// the summary's block is null. On, the walk runs twice per process, off the frame's path.

#include <core/base/types.h>
#include <core/json/json_value.h>

namespace engine::gfx {
class Device;
}

namespace engine::view {

class MemoryReport {
 public:
  // Turns the large-allocation log on: call before the load, so the load's blocks are in it.
  void enable() noexcept;
  bool on() const noexcept { return on_; }
  // A snapshot named `when` ("first_frame", "exit"), logged as one line and kept for `block`. The
  // device may be null (no device yet, or none any more). Does nothing when off or when a snapshot
  // of that name was already taken.
  void take(const char* when, const gfx::Device* device);
  // `{ "first_frame": {...}, "exit": {...} }` as taken so far, or null when off.
  JsonValue block() const;

 private:
  bool on_ = false;
  JsonValue first_frame_;
  JsonValue exit_;
};

// The one report engine-view has: enabled by the flag, read by the summary and `--report`.
MemoryReport& memory_report();

}  // namespace engine::view

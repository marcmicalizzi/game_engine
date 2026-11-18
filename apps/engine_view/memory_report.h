#pragma once

// **`engine-view --memory-report`** (docs/subsystems/apps.md, "--memory-report: where the commit
// is"; roadmap T129): where this process's commit is, taken at the first frame and at exit (and
// once uploaded, W51-2) — as the summary's `memory` block, a block of `--report`'s file, and one log line each.
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
// the summary's block is null. On, the walk runs twice per process, outside the timed frames: the
// first-frame snapshot is taken between two frames, at the top of the loop, and both loops move
// their clocks on by what it took (`take`'s return), so no frame's interval, no tick and no path
// time counts it (T168: until 2026-11-17 it ran inside a frame of the session loop, 4.5 s of the
// owner's walk read as an engine stall, and the session's clock ran those seconds of ticks on the
// next frame). The block says what each walk took (`taken_ms`).

#include <core/base/types.h>
#include <core/json/json_value.h>

namespace engine::gfx {
class Device;
}
namespace engine::renderer {
class GpuScene;
}

namespace engine::view {

class MemoryReport {
 public:
  // Turns the large-allocation log on: call before the load, so the load's blocks are in it. A
  // `floor_bytes` other than zero lowers both logs' floor first (`--memory-report-floor-mib`,
  // W51-2: a table by owner wants the blocks between 1 and 16 MiB too; clamped to 1 MiB).
  void enable(u64 floor_bytes = 0) noexcept;
  bool on() const noexcept { return on_; }
  // A snapshot named `when` ("uploaded", "first_frame", "exit"), logged as one line and kept for
  // `block`. The device may be null (no device yet, or none any more). With a scene, the snapshot carries
  // `place`: what that scene holds where the eye stands, by kind (`GpuScene::place_bytes`, T138),
  // and one more log line. Does nothing when off or when a snapshot of that name was already taken.
  // Returns the nanoseconds it took (0 when it did nothing), for a loop to take out of its clocks.
  i64 take(const char* when, const gfx::Device* device, const renderer::GpuScene* scene = nullptr);
  // `{ "uploaded": {...}, "first_frame": {...}, "exit": {...}, "floor_mib": n }` as taken so far,
  // or null when off.
  JsonValue block() const;

 private:
  bool on_ = false;
  JsonValue uploaded_;
  JsonValue first_frame_;
  JsonValue exit_;
};

// The one report engine-view has: enabled by the flag, read by the summary and `--report`.
MemoryReport& memory_report();

}  // namespace engine::view

#pragma once

// GPU timestamps per frame slot (docs/plan/10-roadmap-risks.md, experiments E1/E2 need numbers
// from the GPU, not the CPU). One query pool per frame in flight; begin_frame() reads the
// results of the frame that last used the slot (the frame context has already waited for it)
// and resets the pool from the host, then zones bracket work in the command buffer.
//
//     timer.begin_frame(commands, frames.slot());
//     timer.begin(commands, "cull");   ...   timer.end(commands);
//     timer.begin(commands, "raster"); ...   timer.end(commands);
//     for (const GpuTimer::Zone& zone : timer.results()) { zone.name, zone.ms }   // last completed
//     frame
//
// Timestamps are written at ALL_COMMANDS on both ends, so a zone measures the GPU work
// between them as the queue executes it, including any barrier waits inside.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/gfx/commands.h>
#include <domain/gfx/device.h>
#include <domain/gfx/rhi.h>

#include <span>
#include <string>

namespace engine::gfx {

class GpuTimer {
 public:
  struct Zone {
    const char* name = "";
    f64 ms = 0.0;
  };

  GpuTimer() noexcept = default;
  ~GpuTimer();
  ENGINE_NON_COPYABLE(GpuTimer);

  bool create(const Device& device, u32 frames_in_flight, u32 max_zones = 32,
              std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return device_ != nullptr; }
  bool supported() const noexcept { return period_ns_ > 0.0; }

  // Reads the slot's previous results and resets its queries. Call right after
  // FrameContext::begin_frame() with FrameContext::slot().
  void begin_frame(CommandList commands, u32 slot);
  // Zones nest in declaration order only (no overlap); `name` must outlive the results.
  void begin(CommandList commands, const char* name);
  void end(CommandList commands);

  // Zones of the most recently completed frame in the slot begin_frame() was last called with.
  std::span<const Zone> results() const noexcept { return {results_.data(), results_.size()}; }
  f64 total_ms() const noexcept;
  // Sum of `name` across results(), 0 when absent.
  f64 ms(const char* name) const noexcept;

 private:
  struct Slot {
    QueryPoolHandle pool;
    Vector<const char*> names;  // zones recorded in this slot's last frame
    u32 used = 0;               // queries written
  };
  const Device* device_ = nullptr;
  Vector<Slot> slots_;
  Vector<Zone> results_;
  // A frame's timestamps as read back, sized once for every zone a slot can hold: `begin_frame`
  // runs every frame, and the frame loop allocates nothing in steady state.
  Vector<u64> stamps_;
  u32 max_zones_ = 0;
  u32 current_ = 0;
  bool open_ = false;
  f64 period_ns_ = 0.0;
};

}  // namespace engine::gfx

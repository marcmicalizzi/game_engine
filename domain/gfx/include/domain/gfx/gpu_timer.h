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
//
// **The frame's own span** (W155; docs/subsystems/gfx.md, "The frame's own span"): `begin_frame`
// also writes a mark at the head of the frame's commands and `end_frame` one at their end, so
// `frame_span()` says, for the frame `results()` describe, how long the GPU took from its first
// command to its last — work no zone names included, which the zones' sum cannot say. Where the
// device can calibrate its clock against the host's (VK_KHR_calibrated_timestamps) the two marks
// are also given on `time::monotonic_ns`'s clock, so a caller can place the GPU's frame against
// its own submission and its own wake: a frame waited for while the GPU ran it, a frame the GPU
// had and did not start (the card busy with something else), and a wake that came late are three
// different answers. The calibration is redone every half second; between, the tick period is the
// device's own.

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
  // The frame `results()` describe, mark to mark (see "The frame's own span" above). `valid` when
  // both marks came back; `first_ns` and `last_ns` are on the host's clock and zero when the device
  // cannot calibrate.
  struct FrameSpan {
    bool valid = false;
    f64 span_ms = 0.0;
    i64 first_ns = 0;
    i64 last_ns = 0;
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
  // The frame's last mark: call after its last zone and before its commands are submitted. A frame
  // that never calls it has no span (its zones still count).
  void end_frame(CommandList commands);

  // Zones of the most recently completed frame in the slot begin_frame() was last called with.
  std::span<const Zone> results() const noexcept { return {results_.data(), results_.size()}; }
  const FrameSpan& frame_span() const noexcept { return span_; }
  // Whether the marks are placed on the host's clock (the device calibrates).
  bool calibrated() const noexcept { return calibrated_; }
  f64 total_ms() const noexcept;
  // Sum of `name` across results(), 0 when absent.
  f64 ms(const char* name) const noexcept;

 private:
  struct Slot {
    QueryPoolHandle pool;
    Vector<const char*> names;  // zones recorded in this slot's last frame
    u32 used = 0;               // queries written
    u8 marks = 0;               // the frame's marks written: 1 the first, 2 the last
  };
  bool calibrate() noexcept;
  i64 to_host_ns(u64 stamp) const noexcept;
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
  FrameSpan span_;
  bool calibrated_ = false;
  u64 calibration_ticks_ = 0;  // a device timestamp and the host's time at it
  i64 calibration_ns_ = 0;
};

}  // namespace engine::gfx

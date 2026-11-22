#pragma once

// **What both frame loops keep of a frame until its GPU numbers fold in, and how it becomes a
// record** (roadmap T212; docs/subsystems/apps.md, "Flythroughs" and "The report's long frames").
// engine-view has two loops that record frames — the window's and the offscreen replay's — and
// until T212 each ended the previous frame's interval by hand, and only a flight
// (`renderer::fly_camera_path`) measured the frame thread's CPU, so the owner's live walk had
// eleven frames over 100 ms with no word on whether the thread was even running. Both loops now
// call `frame_started` at the top of a frame and `fill_record` when it folds, so a window's records
// carry what a flight's do: the thread's CPU and off-CPU time over the frame
// (`sections.thread_cpu_ms`, `off_cpu_ms`), beside `gpu_frame` (the renderer's,
// `renderer::frame_record`), and what only a window has — the acquire, the present, the pacer's and
// the ceiling's waits in the tail
// (`sections.pace_ms`, version 3) and the game module's tick (`world.game_ms`).
//
// No window is opened by a test: `tests/frame_records_tests.cpp` drives these two functions as the
// window loop does, with no device, and the owner's next walk is the proof of the window itself.

#include "fly_camera.h"
#include "long_frames.h"

#include <core/base/types.h>
#include <systems/renderer/flythrough.h>

#include <optional>
#include <schemas/scene.h>

namespace engine::view {

// The per-frame line a loop keeps until the frame's GPU numbers fold in (`Stats::last`), a slot's
// worth of frames later. A ring, because a session is open-ended: the records themselves are the
// only thing that grows.
struct PendingFrame {
  u64 submission = ~u64{0};
  u32 frame = 0;
  u32 ticks = 0;
  f64 time = 0.0;
  f64 cpu_ms = 0.0;
  f64 frame_ms = 0.0;
  // A windowed frame's waits (FrameRecord's `wait_ms`, `acquire_ms`, `present_ms`, `pace_ms`),
  // and the id its present carried, which is how its display time finds it later.
  f64 wait_ms = 0.0;
  f64 acquire_ms = 0.0;
  f64 present_ms = 0.0;
  f64 pace_ms = 0.0;
  u32 pace_depth = 0;
  f64 submit_ms = 0.0;
  u64 present_id = 0;
  // The camera the frame drew and the simulated time it stands for (FrameRecord's `pose_*`).
  FlyState pose;
  f64 pose_time = 0.0;
  // The time-lapse as the frame drew it (FrameRecord's `terrain`).
  std::optional<scene::FrameTerrain> terrain;
  // The walker's ticks of this frame (FrameRecord's `walk`).
  std::optional<scene::FrameWalk> walk;
  // The live wind the frame drew (FrameRecord's `wind`, W151).
  std::optional<scene::FrameWind> wind;
  // The host's time between frames, by what it did (FrameRecord's `world`, W82).
  std::optional<scene::FrameWorld> world;
  // Every span of the loop around the frame (FrameRecord's `sections`, R130); `next_start` is set
  // when the next iteration starts, and the slot's wait and the resize come from the renderer's
  // record of the frame when it folds.
  renderer::FrameSpans spans;
  // The frame thread's CPU clock at this frame's start and the next's (`frame_started`).
  i64 start_cpu = 0;
  i64 next_cpu = 0;
};
inline constexpr u32 k_pending_frames = 8;  // more than the most frames in flight (3) + 1

// **A frame's start ends the one before it**: the previous frame's interval ends at `start` on the
// wall clock and now on the frame thread's CPU clock, and `tail_pace_ms` is what of its tail a
// window's present path held (the pacer's wait, the ceiling's yield). A loop's drain calls it with
// no frame after. Returns the CPU clock for the frame that starts, which its entry keeps as
// `start_cpu` once it is submitted.
i64 frame_started(PendingFrame (&pending)[k_pending_frames], u64 submissions, i64 start,
                  f64 tail_pace_ms = 0.0);

// A folded frame's record (`renderer::frame_record`'s) given the loop's own numbers: its CPU,
// ticks, waits, pose, terrain, walk, world and wind, and — once the next frame has started — its
// sections with the thread's CPU, whose `wall_ms` is the record's `frame_ms` (version 25).
void fill_record(const PendingFrame& pending, scene::FrameRecord& record);

// A folded frame's sections, which `fill_record` gives its record: none until the next frame has
// started. `slot_wait_ms` and `resize_ms` are the renderer's (`FrameStats::cpu`).
std::optional<scene::FrameSections> pending_sections(const PendingFrame& pending, f64 slot_wait_ms,
                                                     f64 resize_ms);

// **A folded frame classified with no record** (`--report` without `--benchmark`): what
// `fill_record` and `renderer::frame_record` would give `LongFrames` — the sections, the world,
// the slot wait's split and the slowest pass — read from the renderer's stats and the loop's entry
// in place, so a window that keeps no records allocates nothing a frame for its long frames.
void classify_folded(const renderer::FrameStats& stats, const PendingFrame& pending,
                     LongFrames& into);

}  // namespace engine::view

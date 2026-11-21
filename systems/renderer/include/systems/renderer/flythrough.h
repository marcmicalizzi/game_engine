#pragma once

// What a flythrough benchmark records and how it is summarized (docs/plan/09-testing-profiling.md
// §9.4, docs/subsystems/renderer.md "Scenes, camera paths and flythroughs"). Both hosts fly a
// camera path the same way — engine-view `--camera-path --benchmark` and `render.benchmark` with
// a `camera_path` — and both turn `Stats::last` into the same `engine.scene.FrameRecord`s and the
// same percentiles, so a number from either names the same thing.
//
// **Per frame, then per path.** A run flies the path `repeats` times. Each frame's cost is the
// median of its repeats — which takes out a one-off hiccup without hiding a frame that is always
// slow — and the path is then summarized by the median, p95 and p99 of those per-frame figures:
// the median says what the scene costs, the tails say what its worst stretch costs, and a
// flythrough exists to find the worst stretch.
//
// **The census is a second pass, on purpose.** Visible pairs by DAG level and by mesh need the
// frame's visible list, and the renderer never reads a buffer back inside a frame. Reading it
// between frames would put a wait in the timed loop and let the clocks fall, so the census flies
// the path again with a fresh renderer, one frame at a time, reading each list back — and
// compares its visible pairs with the timed pass's, frame by frame, which is also a determinism
// check.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/gfx/device.h>
#include <systems/renderer/camera_path.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>

#include <schemas/scene.h>
#include <span>
#include <string>

namespace engine::renderer {

// The frame `stats` describes, as the benchmark's JSON line has it. `cpu_ms`, `frame_ms` and
// `ticks` are left at zero for the caller: they are measured around the renderer by whoever runs
// the frame loop (`fly_camera_path` below, engine-view's interactive loop), which the renderer
// does not see.
//
// **The measurement rule is here** (ADR-0064; renderer.md, "Sampling over time: the measurement
// rule"): the record carries `stats.reconstruction` as its `reconstruction` block, and its times
// are its `gpu_ms` only when that is a native measurement (`Reconstruction::native`); otherwise
// they are under `reconstructed` and `gpu_ms` is null. `summarize_frames` holds a summary to the
// same rule over its records: one record that was not native and the summary has no `gpu_ms`.
scene::FrameRecord frame_record(const FrameStats& stats, u32 repeat, u32 frame, f64 time);
// The block a record and a summary carry for `state`.
scene::Reconstruction reconstruction_block(const Reconstruction& state);
// A record's times wherever it carries them: `gpu_ms`, or `reconstructed.gpu_ms`; null for neither.
// For a reader that prints or compares a figure knowing which it is; a cost reader takes `gpu_ms`.
const scene::PassMs* record_pass_ms(const scene::FrameRecord& record) noexcept;
// The same rule for a summary line a host writes by hand (engine-view's plain summary): `open` goes
// where `"gpu_ms":` stood and `close` after the times' closing brace. Native, `open` is the
// `reconstruction` block and `"gpu_ms":`; otherwise the block, `"gpu_ms":null` and
// `"reconstructed":{"gpu_ms":`, with `close` the brace that ends it.
struct MeasurementKeys {
  std::string open;
  const char* close = "";
};
MeasurementKeys measurement_keys(const Reconstruction& state);

// **Every millisecond of a frame** (R130, roadmap T71 part 3; `scene::FrameSections`): what a
// frame loop measured around one frame, in its order, on `time::monotonic_ns`'s clock. `start` is
// the loop iteration's start, `end` when the last measured part returned, `next_start` the next
// iteration's start; the parts are the loop's own spans in milliseconds, zero for a part the loop
// does not have (a flight has no acquire, ticks or present). `begin_call_ms` is the whole
// `begin_frame` call, which the renderer's own `FrameCpu::wait_ms` (`slot_wait_ms`) splits into the
// slot's wait and the rest, and `resize_ms` is inside the rest. The loop's tail is
// `next_start - end`, and `other_ms` what the parts leave of the wall time.
struct FrameSpans {
  i64 start = 0;
  i64 end = 0;
  i64 next_start = 0;
  f64 host_ms = 0.0;
  f64 begin_call_ms = 0.0;
  f64 slot_wait_ms = 0.0;
  f64 resize_ms = 0.0;
  f64 fold_ms = 0.0;
  f64 acquire_ms = 0.0;
  f64 ticks_ms = 0.0;
  f64 submit_ms = 0.0;
  f64 after_ms = 0.0;
  f64 present_ms = 0.0;
  // The frame's thread's own CPU time from `start` to `next_start` (W155;
  // `profiling::thread_cpu_ns`), negative when the loop did not measure it.
  f64 thread_cpu_ms = -1.0;
};
scene::FrameSections frame_sections(const FrameSpans& spans);
// The run's ray tracing chain into the summary's `rt` block, all but the per-frame percentiles
// `summarize_frames` fills; both hosts call it after the flight so the numbers are the run's.
void summarize_rt(const RtStats& stats, scene::FlythroughRt& out);
// The scene's textures into the summary's `textures` block: distinct textures built and decoded,
// the images that shared another mesh's upload, and the bytes uploaded and saved
// (docs/subsystems/renderer.md, "One upload per distinct image").
void summarize_textures(const GpuScene& scene, scene::FlythroughTextures& out);

// One submission of a flight, as the hook before it sees it (`FlightOptions::before_frame`).
struct FlightStep {
  u32 repeat = 0;
  u32 frame = 0;          // the path frame; a warm-up's is 0 and the drain's the last
  u32 warmup = ~0u;       // its place in its repeat's warm-up, or ~0 for a path frame or the drain
  bool recorded = false;  // a path frame whose numbers the flight records
  Camera camera;          // the camera the frame is drawn from
  // With `FlightOptions::measure_world`: the frame record's `world` (W82), whose parts the hook
  // fills — the world's update, the ground's frame — and whose `total_ms` and `other_ms` the flight
  // fills from the hook's own wall time. Null otherwise.
  scene::FrameWorld* world = nullptr;
};
// Called before every submission, warm-ups and the drain included, between two frames: where a
// host that changes the scene as the camera moves — a streamed world (docs/subsystems/world.md) —
// does it. False stops the flight with `error`.
using FlightHook = bool (*)(void* context, const FlightStep& step, std::string* error);

// **What the frame loop allocated is counted, and a measurement is not the frame loop.** A flight
// counts the engine allocations and device buffers its thread makes in each frame's host turn,
// `begin_frame` and `submit_frame` (`FrameRecord::allocations`, `buffers`; docs/subsystems/
// renderer.md, "The frame loop allocates nothing"). A host whose hook also *records* the frame for
// the benchmark — a time-lapse's state for the record, a world log's line — holds one of these
// round that recording, and what it allocates is left out of the count: the record exists because
// somebody is measuring, and a game's frame does not make it. On any thread, nested or not, flying
// or not; outside a flight it changes nothing anybody reads.
class FlightBookkeeping {
 public:
  FlightBookkeeping() noexcept;
  ~FlightBookkeeping();
  FlightBookkeeping(const FlightBookkeeping&) = delete;
  FlightBookkeeping& operator=(const FlightBookkeeping&) = delete;

  // What every scope on this thread has left out since the thread started: allocations and
  // buffers. `fly_camera_path` subtracts the change across a frame from that frame's count.
  static u64 allocations_left_out() noexcept;
  static u64 buffers_left_out() noexcept;

 private:
  u64 allocations_ = 0;
  u64 buffers_ = 0;
  bool outermost_ = false;
};

// How a timed flight is flown. `frames` of 0 is the path's own frame count; any other count
// resamples it. `frames_in_flight` must be the renderer's (`SceneRenderer::Desc`), because that
// many frames are drawn after the last one so that its numbers come back.
struct FlightOptions {
  FlightHook before_frame = nullptr;
  void* before_frame_context = nullptr;
  // Each record carries `world`: the hook's wall time as `total_ms`, split by the parts the hook
  // names in `FlightStep::world` and the rest as `other_ms` (W82). A flight's `cpu_ms` stays the
  // renderer's own; the hook's turn is in `frame_ms`, and now named beside it.
  bool measure_world = false;
  u32 frames = 0;
  u32 repeats = 1;
  u32 warmup = 0;  // frames at the path's first camera before every repeat
  // And at least this much wall time of them. A frame count alone cannot warm a fast card: 240
  // frames of a 0.1 ms scene is 24 ms of work, which leaves the clocks where idle left them, and
  // the first frames of the path are then measured on a card still coming up (E1 on Pascal found
  // the same frames reading 1.55, 0.77 and 0.71 ms back to back for exactly that reason).
  f64 warmup_seconds = 0.0;
  u32 frames_in_flight = 2;
  // **The flight's clocks** (clocks.h; renderer.md, "One world clock, and the day's length"). A
  // frame is a sixtieth of a real second, the frame index's clock, so path frame f's sky stands
  // `sky_start_s + sun_rate * f / 60` game seconds past the scene's epoch (`FrameDesc::sky_time_s`)
  // — the hour engine-view's plain offscreen run lights frame f with — and its water, fires and
  // trees `world_rate * f / 60` world seconds in; every repeat, and every warm-up frame (at the
  // path's first camera), alike. `sun_rate` is game seconds of the sky's day per real second
  // (lighting.h, "The sun's day"), `sky_start_s` the `--time-of-day` offset from the epoch.
  f64 sun_rate = 0.0;
  f64 world_rate = 1.0;
  f64 sky_start_s = 0.0;
};

struct Flight {
  u32 frames = 0;                      // path frames flown per repeat
  Vector<scene::FrameRecord> records;  // one per path frame per repeat, in fold order
  f64 seconds = 0.0;                   // wall time of the path frames, all repeats
  u64 submitted = 0;                   // every frame submitted, warm-ups and the drain included
};

// The timed pass: `repeats` flights of `path`, each after `warmup` frames at its first camera,
// with the renderer's frames kept in flight and each frame's numbers read out of `Stats::last`
// when its slot comes around. The renderer's statistics are reset first. A frame's `frame_index`
// is its path frame, so a frame and its repeats are lit alike. False with `error` when a frame
// could not be recorded.
bool fly_camera_path(SceneRenderer& renderer, const CameraPath& path, const FlightOptions& options,
                     Flight& out, std::string* error = nullptr);

// Median, p95, p99, max and mean of `values`, with linear interpolation between the closest
// ranks. Zeros for an empty span.
scene::Percentiles percentiles(std::span<const f64> values);

// The summary's statistics over a run's records: every frame of `frames` has one record per
// repeat, in any order. Fills `gpu_ms`, `visible_pairs`, `deterministic`, `mismatched_frames`,
// the streaming totals, and one `markers` entry per marker of `path` (mapped onto `frames` when
// the run resampled the path). Records past `frames` or `repeats` are ignored.
void summarize_frames(std::span<const scene::FrameRecord> records, u32 frames, u32 repeats,
                      const CameraPath& path, scene::FlythroughSummary& out);

// **A run's frames by their own intervals, the slowest named** (T137; apps.md, "The summary's slow
// frames"). A record's `sections.wall_ms` is its frame's own loop iteration and every part of the
// record is a part of it; `frame_ms` is the interval *before* the frame, and reading a long one
// against its own record's parts is how the forest walk's 32 stalls came to be called time "no
// section names" for a batch — each was named, `world.ground_ms`, in the record before.
// `summarize_frames` calls `summarize_slow` with `k_slow_frame_ms`: the counts over 33, 50 and
// 100 ms, the percentiles to p99.9, and the `k_slow_frames_named` slowest frames over the limit,
// each with `slowest_span`'s answer — the largest part at each step down the record's sections,
// `world` and `cpu` blocks while one part holds at least half of the one above it
// (`host.ground.wait`, `host.update.handover.write`, `submit.tables`, `slot_wait`). It reads the
// records after the run, so it costs a frame nothing.
inline constexpr f64 k_slow_frame_ms = 50.0;
inline constexpr u32 k_slow_frames_named = 32;
scene::SlowFrame slowest_span(const scene::FrameRecord& record);
void summarize_slow(std::span<const scene::FrameRecord> records, f64 limit_ms,
                    scene::FlythroughSummary& out);

// The run frame a marker at the path's own rate lands on when the path is flown in `frames`.
u32 marker_frame(const CameraPath& path, u32 marker, u32 frames) noexcept;

// Reads a completed frame's visible list back and counts it. Create once per scene; `count` after
// each `SceneRenderer::render_offscreen` (which waits for the frame and folds its visible counts
// into `stats`). Blocking, and so never inside a timed run.
class VisibleCensus {
 public:
  VisibleCensus() noexcept = default;
  ~VisibleCensus();
  VisibleCensus(const VisibleCensus&) = delete;
  VisibleCensus& operator=(const VisibleCensus&) = delete;

  bool create(const gfx::Device& device, const GpuScene& scene, std::string* error = nullptr);
  void destroy() noexcept;

  // `levels[l]` is the frame's visible pairs at DAG level l (0 is the source geometry), and
  // `meshes[m]` its visible pairs of scene mesh m. Both are resized to fit.
  //
  // **And the chain's census** (R152; docs/experiments/rt-chain-walk-2026-11-12.md): the same
  // entries, and the shadow casters' run after them, by what the ray tracing chain does with their
  // instance (`GpuScene::rt_instance_kind`, 0 to 7) and by its rank in the drop order
  // (`rt_instance_rank`: 0 the prefix, ring + 1 a tail's, the last rank everything past it):
  // `chain()[(caster * k_chain_ranks + rank) * k_chain_kinds + kind]`, which says what a frame's
  // structures are for — its own builds against references, the deforming against the rigid, ring
  // by ring. With `renderer.rt.census_log` (false) every counted frame logs it, one line.
  static constexpr u32 k_chain_ranks = 6;
  static constexpr u32 k_chain_kinds = 8;
  bool count(const SceneData& data, const GpuScene& scene, const Stats& stats, Vector<u32>& levels,
             Vector<u32>& meshes, std::string* error = nullptr);
  std::span<const u32> chain() const noexcept {
    return std::span<const u32>(chain_, 2 * k_chain_ranks * k_chain_kinds);
  }

 private:
  bool copy_runs(const GpuScene& scene, const Stats& stats, bool casters, u64& entries,
                 std::string* error);
  const gfx::Device* device_ = nullptr;
  gfx::BufferResource staging_;
  u32 chain_[2 * k_chain_ranks * k_chain_kinds] = {};
  u32 counted_ = 0;  // frames counted, for the log's line
};

}  // namespace engine::renderer

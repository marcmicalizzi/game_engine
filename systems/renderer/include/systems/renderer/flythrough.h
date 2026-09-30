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
scene::FrameRecord frame_record(const FrameStats& stats, u32 repeat, u32 frame, f64 time);
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
};
// Called before every submission, warm-ups and the drain included, between two frames: where a
// host that changes the scene as the camera moves — a streamed world (docs/subsystems/world.md) —
// does it. False stops the flight with `error`.
using FlightHook = bool (*)(void* context, const FlightStep& step, std::string* error);

// How a timed flight is flown. `frames` of 0 is the path's own frame count; any other count
// resamples it. `frames_in_flight` must be the renderer's (`SceneRenderer::Desc`), because that
// many frames are drawn after the last one so that its numbers come back.
struct FlightOptions {
  FlightHook before_frame = nullptr;
  void* before_frame_context = nullptr;
  u32 frames = 0;
  u32 repeats = 1;
  u32 warmup = 0;  // frames at the path's first camera before every repeat
  // And at least this much wall time of them. A frame count alone cannot warm a fast card: 240
  // frames of a 0.1 ms scene is 24 ms of work, which leaves the clocks where idle left them, and
  // the first frames of the path are then measured on a card still coming up (E1 on Pascal found
  // the same frames reading 1.55, 0.77 and 0.71 ms back to back for exactly that reason).
  f64 warmup_seconds = 0.0;
  u32 frames_in_flight = 2;
  // Game seconds per real second of the sun's day (lighting.h, "The sun's day"). A frame is a
  // sixtieth of a second, the frame index's clock, so path frame f is lit `sun_rate * f / 60` game
  // seconds into the day — the numbers engine-view's plain offscreen run lights frame f with — and
  // every repeat, and every warm-up frame (at the path's first camera), is lit alike.
  f64 sun_rate = 0.0;
  // Where the day stands at path frame 0, game seconds (`FrameDesc::sun_time_s`): path frame f is
  // lit `sun_time_s + sun_rate * f / 60` into it. A sky's hour (engine-view's `--time-of-day`,
  // renderer.md "One clock"); 0 is the ground's own time.
  f64 sun_time_s = 0.0;
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
  bool count(const SceneData& data, const GpuScene& scene, const Stats& stats, Vector<u32>& levels,
             Vector<u32>& meshes, std::string* error = nullptr);

 private:
  const gfx::Device* device_ = nullptr;
  gfx::BufferResource staging_;
};

}  // namespace engine::renderer

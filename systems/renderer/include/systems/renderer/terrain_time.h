#pragma once

// The dunes in time-lapse (docs/subsystems/renderer.md, "The dunes in time-lapse"; terrain.md,
// "Re-evaluation"; ADR-0043): game time running at `rate` game seconds per real second, the dune
// field evaluated off the frame at game times the fields' own motion chooses, and the terrain on
// the GPU **blended between the last evaluated field and the next** every frame, so the sand moves
// a little every frame and never all at once.
//
// **The blend.** Each terrain level holds two evaluated height fields, a at `time_a` and b at
// `time_b`, and a *surface time* between them. The pool pass draws `a (1 - t) + b t` with
// `t = (surface - time_a) / (time_b - time_a)` (deform.slang's terrain stage), so the surface is a
// continuous function of the surface time. Every frame the surface time moves towards the game
// time, but no further than b's time and no faster than moves any vertex by `fraction * spacing`
// in the frame: a vertex's change is `|b - a| dt`, so a pair whose largest difference is `delta`
// may advance `t` by at most `fraction * spacing / delta` a frame (`terrain_blend_step`). When the
// surface reaches b exactly, b is the next pair's a — the same heights to the bit — and the next
// field becomes b. If it is not there yet the surface **holds at b** and continues from there when
// it arrives, catching up at the same capped speed: a late field costs the time-lapse a moment of
// stillness and never a step.
//
// **One surface time for every level** (`terrain_surface_frame`). The scene's grid and the rings
// round the camera are the same function of time at different spacings, and where two meet they
// must draw the same sand: a ring a game hour ahead of the grid round it would stand a step above
// it wherever the dunes move, which no skirt hides. So the levels share the surface time, each with
// its own pair, and it moves no further than the nearest of their b's and no faster than any
// level's bound allows; a level whose next field is late holds them all.
//
// **The cadence is by displacement, not the calendar** (`terrain_next_time`). The generator knows
// how far each band travels — the wind's flux path over the band's height, Bagnold's rule, storms
// included — so the next field is timed for when the fastest band will have travelled
// `fraction * spacing`: the blend then crosses at most a fraction of a sample, where a straight
// line between two fields is the motion. At a normal clock that is days of game time and the field
// is effectively still; at a game day a real second it is a field every few frames for the waves,
// which no evaluation of a whole grid keeps up with, so the next time is also pushed out to what an
// evaluation costs (`lead` times the last one's wall time, at the rate): the blend then crosses
// more than a fraction of a sample and the waves cross-fade rather than slide, still without a
// single step. `wait` (offscreen runs and tests) times fields by displacement alone and waits for a
// late one instead, so the pictures of a run are a function of its frames.
//
// **Never on the frame.** A field is evaluated on a worker thread of its own, a 64 x 64 block of
// the lattice per job on the job pool, into a host-visible staging buffer the next frame copies
// from and retires (GpuScene::terrain_upload). Nothing waits for it but `wait` and `finish`.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/resources.h>
#include <domain/scene_gen/tile_source.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_levels.h>
#include <systems/renderer/terrain_rings.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>

namespace engine::renderer {

class GpuScene;

struct TimeLapseConfig {
  f64 rate = 0.0;  // game seconds per real second; 0 is still
  // How far, as a share of a level's spacing, the fastest band may travel between two fields — and
  // how far any vertex may move in one frame: `renderer.terrain.move_fraction`.
  f64 fraction = 0.25;
  // Game seconds between fields at least: `renderer.terrain.min_step_s`. A second since
  // 2026-09-28 (a minute before): a storm with a transport gain moves the sand at the game's own
  // rate fast enough that a minute between fields let a pair span half a sample and cross-fade
  // (terrain_clock_tests.cpp, "a storm at the game's own rate").
  f64 min_step_s = 1.0;
  f64 max_step_s = 2'592'000.0;  // and at most (30 days): `renderer.terrain.max_step_s`
  // How far ahead of what an evaluation costs the next field is timed, as a multiple of the last
  // evaluation's wall time at the rate: `renderer.terrain.lead`. Not with `wait`.
  f64 lead = 1.5;
  // A field the surface has caught up with is waited for rather than held at, and fields are
  // timed by displacement alone: the frames of a run then draw the same pictures however fast the
  // machine is. For offscreen runs and tests; an interactive one never waits.
  bool wait = false;
};

// The rate asked for, with the rest from their tunables.
TimeLapseConfig time_lapse_config_from_tunables(f64 rate);
// The worker threads a host gives the pool a window's terrain is built on
// (`renderer.terrain.workers`; 0 is half the logical CPUs, at least two): fewer than the machine
// has, because a rebuild or a window's move fills every worker for tens of milliseconds and a
// worker on every CPU took the frame thread's time slices (renderer.md, "What a frame waits for").
u32 terrain_window_workers() noexcept;

// **The cadence rule**: the game time after `from_s` at which the generator's fastest band will
// have travelled `fraction * spacing_m` along the wind's path (`terrain_band_travel_m`), clamped to
// [from + min_step, from + max_step]. Found by bisection on the closed form, whose cost does not
// depend on the time. Without a generator, `from_s + max_step_s`.
f64 terrain_next_time(const TerrainSampler& sampler, f64 from_s, f64 spacing_m, f64 fraction,
                      f64 min_step_s, f64 max_step_s) noexcept;
// The same rule over a tile source's travel (scene_gen/tile_source.h): what a tile level's fields
// are timed by. A source that does not move is `from_s + max_step_s`.
// A far level's lattice is filtered (`filter_mm`, renderer.md "Ground to the horizon"), and its
// cadence is the fastest band it carries: the mean it stands in for the rest with does not move.
f64 terrain_next_time(const scene_gen::TileSource& source, f64 from_s, f64 spacing_m, f64 fraction,
                      f64 min_step_s, f64 max_step_s, i64 filter_mm = 0) noexcept;

// **The blend of one level** (above), as plain numbers the tests drive a frame at a time.
struct TerrainBlend {
  f64 time_a = 0.0;
  f64 time_b = 0.0;
  bool has_b = false;
  f64 delta_m = 0.0;    // the largest |b - a| over the level's samples, metres
  f64 surface_s = 0.0;  // the game time the surface stands for, in [time_a, time_b]
  // What the pool pass blends by: 0 at a, 1 at b, 0 with no b.
  f64 blend() const noexcept;
};
struct TerrainBlendStep {
  f64 surface_s = 0.0;
  f64 moved_m = 0.0;  // the most any vertex moved: delta times the change of the blend
  bool at_b = false;  // the surface reached b's time, so b may become the next pair's a
};
// One move of the surface towards `target_s`: never backwards, never past b, and never by more
// than `budget_m` of height anywhere. With no b the surface stays at a.
TerrainBlendStep terrain_blend_step(const TerrainBlend& state, f64 target_s, f64 budget_m) noexcept;

// **One frame of one level's surface**: it moves towards `target_s` within `budget_m`, and whenever
// it stands at b (or has no b) with a later game time still to reach, the next field — when it is
// `ready` — becomes b and the old b becomes a, the same heights to the bit, and it moves on with
// what is left of the budget. `next.ready` is cleared when it is taken. What `TerrainMotion` runs
// every frame for every level, and what the tests drive with fields that arrive on time, late and
// never.
struct TerrainNextField {
  bool ready = false;
  f64 time_s = 0.0;
  f64 delta_m = 0.0;  // its largest difference from the field before it
};
struct TerrainFrameResult {
  f64 moved_m = 0.0;    // the most any vertex moved this frame: never more than the budget
  u32 installed = 0;    // fields taken as b (0 or 1)
  bool held = false;    // the surface stood at b with game time still to reach and no next field
  bool capped = false;  // the budget stopped the surface short of both the game's time and b's
};
TerrainFrameResult terrain_blend_frame(TerrainBlend& blend, f64 target_s, f64 budget_m,
                                       TerrainNextField& next) noexcept;

// **One frame of the whole terrain's surface** (above): the same rules for several levels that
// share one surface time, every `blends[k].surface_s` equal on entry and left equal. The surface
// moves towards `target_s`, never past any level's b, never by more than `budgets_m[k]` of height
// on level k; a level standing at its b takes its `next[k]` when it is ready (cleared when taken)
// and the surface moves on. `moved_m[k]` is what level k's vertices moved at most, `installed[k]`
// the fields it took. At most `k_max_surface_levels` levels, which is every level a level set can
// have: when the far levels raised that to sixteen (renderer.md, "Ground to the horizon") this
// stayed at eight, and the endless desert's ninth drawn level — its finest ring — was left out of
// the surface's frame, its `installed` never written, and its count of fields after b wrapped
// below zero on the uninitialized word; the next frame walked off the end of that level's two
// fields and engine-view stopped with an access violation and nothing on its output (2026-10-03).
inline constexpr u32 k_max_surface_levels = k_max_terrain_levels;
struct TerrainSurfaceResult {
  f64 surface_s = 0.0;
  bool held = false;    // it stood at a level's b with game time still to reach and no next field
  bool capped = false;  // a level's bound stopped it short of the game's time and of every b
  u32 limiting = ~0u;   // the level that held or capped it
};
TerrainSurfaceResult terrain_surface_frame(std::span<TerrainBlend> blends, f64 target_s,
                                           std::span<const f64> budgets_m,
                                           std::span<TerrainNextField> next, std::span<f64> moved_m,
                                           std::span<u32> installed) noexcept;

// **When to time a level's next field so that the surface never waits for it and never outruns
// the per-frame bound** (renderer.md, "A clock that never stops"): the displacement rule's `at_s`,
// pushed out to `from_s + lead * rate * turnaround_s` — the real time from the level wanting a
// field to its being ready to draw, which is the evaluation *and* the wait behind the other levels'
// on the one field worker, at the rate — and to the span over which a pair as different as the last
// one (`delta_m`) crosses at the rate within `budget_m` a frame of `frame_s`, with a quarter to
// spare; no further than `longest_step_s` past `from_s`. `late` says a push decided it.
struct TerrainKeepUp {
  f64 rate = 0.0;          // game seconds per real second
  f64 turnaround_s = 0.0;  // real seconds from wanting a field to its being ready
  f64 lead = 1.5;          // renderer.terrain.lead
  f64 frame_s = 0.0;       // a frame's real seconds
  f64 delta_m = 0.0;       // the level's last pair's largest difference
  f64 budget_m = 0.0;      // the level's per-frame bound
  f64 longest_step_s = 2'592'000.0;
};
f64 terrain_keep_up_time(f64 from_s, f64 at_s, const TerrainKeepUp& keep,
                         bool* late = nullptr) noexcept;

// **The surface clock** (renderer.md, "A clock that never stops"): where an interactive time-lapse
// puts the surface time each frame, so that the sand's speed is continuous. The surface runs at
// game time minus a latency L, and at the rate while it is there. L is `margin` times the worst of
// the levels' recent **turnarounds** at the rate — the real time from a level wanting its next
// field to that field being ready to draw, the evaluation and the wait behind the other levels':
// how long, in game time, a field is on its way, which is how far behind game time the surface must
// stand for the field it needs to be there already. The levels share one surface, so it is the
// largest of theirs. (Not the interval between arrivals: at a slow rate that is the displacement
// cadence, hours of game time between fields that were ready in milliseconds, and no delay at all.)
// The speed changes by at most the rate every `response_s` of real time, closes a gap to its target
// over `settle_s`, never passes `catch_up` times the rate, and brakes at the same rate for the
// newest field every level has (`horizon_s`), so a field later than L allows slows the sand down
// gently and the sand sets off again gently: never a stop followed by a sprint. The per-frame bound
// of `terrain_surface_frame` still applies after it: the clock chooses a target, the blend's own
// rule how far the surface goes towards it.
//
// **A change of rate** (`set_rate`; renderer.md, "Changing the rate while it runs") is a change of
// what the clock steers by, never of where the surface stands: L is the new rate's at once (the
// same turnarounds, at the new rate), and the speed the surface had carries over and changes at
// **the acceleration of that speed** — the old rate once the clock has settled, whatever it had
// reached when a second key comes before that — for a latency's worth of real time (`margin` times
// the worst turnaround), and after that for as long as it is still above the new rate's reach.
// Down, that is the larger acceleration: a week a second slowed to a minute a second decelerates in
// under a second, as it set off, rather than over the hours the new rate's own would take, and a
// rate of zero brings the sand to rest the same way rather than at once. Up, it is the smaller, and
// on purpose: the fields on their way were timed at the old rate and hold about a latency of it, so
// a sand that took the new rate's acceleration at once outran them and braked to a stand before
// the first field timed at the new rate could come (the CPU model measured five frames of it from
// a tenth of a day to a day); for a latency it gains its old speed every `response_s`, and then
// the new rate's. From rest there is nothing to carry over, and the new rate's own starts it. At a
// constant rate nothing here differs from a clock that never changed.
struct TerrainClockConfig {
  f64 rate = 0.0;        // game seconds per real second
  f64 margin = 1.2;      // L is this times the worst recent turnaround at the rate
  f64 catch_up = 1.5;    // the surface never runs faster than this times the rate
  f64 response_s = 0.5;  // real seconds for its speed to change by the whole rate, at most
  f64 settle_s = 2.0;    // real seconds over which a gap to game time minus L closes
};
inline constexpr u32 k_clock_samples = 8;  // arrivals a level remembers
class TerrainClock {
 public:
  void reset(const TerrainClockConfig& config) noexcept;
  // The rate changes (above): L follows it now, the speed over the next second or so. The
  // turnarounds are real seconds and are kept. A negative rate is taken as zero.
  void set_rate(f64 rate) noexcept;
  f64 rate() const noexcept { return config_.rate; }
  // Level `level`'s next field became ready to draw `turnaround_s` real seconds after the level
  // came to want it.
  void arrived(u32 level, f64 turnaround_s) noexcept;
  // Where the surface should stand after a frame of `real_dt_s`, from `surface_s`, with game time
  // at `game_s` and every level's newest field at or after `horizon_s`: never backwards, never past
  // the horizon. At a rate of zero a surface still moving from a faster rate decelerates to rest;
  // one at rest stays where it is.
  f64 advance(f64 real_dt_s, f64 game_s, f64 surface_s, f64 horizon_s) noexcept;
  // Where the frame actually left it (the per-frame bound may have stopped it short): the speed
  // the next frame starts from.
  void settle(f64 real_dt_s, f64 from_s, f64 to_s) noexcept;
  // The worst of level `level`'s recent turnarounds, real seconds (0 before its first arrival):
  // what its next field is timed by (`TerrainKeepUp::turnaround_s`), because a field timed by a
  // typical turnaround is late whenever it waits behind a longer evaluation.
  f64 turnaround_s(u32 level) const noexcept;
  f64 latency_s() const noexcept { return latency_s_; }  // L, game seconds
  f64 speed() const noexcept { return speed_; }          // game seconds per real second
  // The last `advance` slowed the surface for the horizon: a field is later than L allowed.
  bool braked() const noexcept { return braked_; }

 private:
  struct Level {
    f64 turnaround_s[k_clock_samples] = {};
    u32 count = 0;
    u32 next = 0;
  };
  void update_latency() noexcept;
  TerrainClockConfig config_;
  Level levels_[k_max_surface_levels];
  f64 latency_s_ = 0.0;
  f64 speed_ = 0.0;
  // The speed at the last change while its ramp lasts (0 otherwise): what the acceleration is
  // taken from for `ramp_left_s_` more real seconds, and after them while the speed is above the
  // new rate's reach.
  f64 ramp_rate_ = 0.0;
  f64 ramp_left_s_ = 0.0;
  bool braked_ = false;
};

// The largest |b - a| over the samples two windows of one lattice share (`a` and `b` are `nx * nz`
// heights, rows of x in order of z), and the largest |f - rest|: what the blend's speed and the
// cull's padding are computed from.
f64 terrain_field_delta(std::span<const f32> a, const gfx::TerrainField& wa, std::span<const f32> b,
                        const gfx::TerrainField& wb) noexcept;

// **The time-lapse itself, and the rings round the camera**: the levels' fields evaluated off the
// frame, handed to the GPU scene and blended a frame at a time; and, with rings, their re-centres —
// the chunks rebuilt off the frame, uploaded into free slots a few a frame, and swapped in whole in
// one frame (renderer.md, "The rings in the scene").
class TerrainMotion {
 public:
  TerrainMotion() = default;
  ~TerrainMotion();
  TerrainMotion(const TerrainMotion&) = delete;
  TerrainMotion& operator=(const TerrainMotion&) = delete;

  // False, with a sentence, for a scene with no terrain levels (`ResolvedSettings::terrain_levels`)
  // or a negative rate. `rings` is the set the GPU scene was made with — the rings round the camera
  // or the world's tiles — or null; the motion owns it from here (its worker rebuilds it).
  // Evaluates every level's field at the scene's own time on the calling thread (through `jobs`
  // when given) and hands it over, so the first frame draws it: the rest pose, to the bit on the
  // scene's grid. A tile level's heights come from the set's tile source, and the scene's grid,
  // which draws nothing under tiles, is evaluated not at all and takes no part in the surface's
  // time. A rate of zero is a still field whose rings follow the camera. The scene and the rings
  // must outlive the motion.
  bool start(GpuScene& scene, TerrainLevelSet* rings, const TimeLapseConfig& config,
             jobs::JobSystem* jobs, std::string* error = nullptr);
  // Before a frame: game time moves on by `real_dt_s * rate`, each level's surface towards it, a
  // finished field goes to the GPU scene and the next is asked for; with rings, a camera that has
  // left a ring's middle half asks for a re-centre, and a finished one is uploaded and swapped in.
  // What the frame draws is handed to the scene (`GpuScene::terrain_show`, `terrain_chunk_show`).
  // The camera is a `WorldPos` (ADR-0053): the layout is chosen from it in f64 and whole
  // millimetres, never from a float32 rounding of it.
  void frame(f64 real_dt_s, WorldPos camera = WorldPos::origin());
  // Stops the worker, waiting for a field or a re-centre in flight.
  void finish();

  // **The rate changes while the sand moves** (renderer.md, "Changing the rate while it runs";
  // engine-view's `,` and `.`). Game time runs on at the new rate from the next frame; the surface
  // clock takes it (`TerrainClock::set_rate`: L at the new rate at once, the speed towards it at
  // the acceleration of the speed it had for a latency and then the new rate's, so the sand slows
  // down within a second, speeds up over one to three without outrunning the fields already on
  // their way, and comes to rest at zero rather than stopping dead); and **the next field is timed
  // at the new
  // rate** — a level that has no field on its way comes to want its next one now, since the time
  // spent at a rate of zero, when nothing is asked for, is no field's turnaround, and whatever it
  // asks for next is timed by the cadence rule and the keep-up rule at the new rate. Fields already
  // evaluated or on their way keep their times: they are the same function of time, and a pair of
  // them is crossed within the per-frame bound whatever it spans (at a slower rate, by a
  // cross-fade over the span the faster one chose, until the fields timed at the new rate arrive).
  // Nothing the surface stands on is touched — not the surface time, not a pair, not a blend — so a
  // change never steps the sand, and every frame after it goes through `terrain_surface_frame`'s
  // bound like every frame before. With `wait` (offscreen) the fields are timed by displacement
  // alone as ever, so a run that changes its rate at the same frames draws the same pictures;
  // engine-view's offscreen runs never change it. False, with nothing changed, before `start` or
  // for a rate that is negative or not finite.
  bool set_rate(f64 rate) noexcept;
  u32 rate_changes() const noexcept { return rate_changes_; }  // changes that changed the rate
  f64 start_rate() const noexcept { return start_rate_; }      // the rate `start` was given

  bool active() const noexcept { return scene_ != nullptr; }
  const TimeLapseConfig& config() const noexcept { return config_; }  // `rate` is the current one
  f64 game_time_s() const noexcept { return start_s_ + game_s_; }

  // What the rings did (renderer.md, "The rings in the scene").
  struct RingStats {
    u32 rebuilds = 0;         // re-centres the worker built
    u32 swaps = 0;            // and the frames that swapped them in
    u32 chunks_built = 0;     // chunks rebuilt over all re-centres
    u32 chunks_kept = 0;      // chunks a re-centre kept
    u32 chunks_dropped = 0;   // chunks a re-centre let go: held before it and not kept
    u32 chunks_resident = 0;  // chunks the last re-centre left the levels holding
    u32 most_chunks = 0;      // and the most any did
    u32 chunks_uploaded = 0;  // chunks copied onto the device after the first frame
    u64 upload_bytes = 0;
    u32 upload_frames = 0;      // frames that uploaded chunks
    u32 failed = 0;             // re-centres dropped (no room: a slot or an arena)
    f64 last_rebuild_ms = 0.0;  // the ring worker's wall time for the last re-centre's chunks
    f64 max_rebuild_ms = 0.0;
    f64 last_pairs_ms = 0.0;  // the field worker's for carrying the rings' pairs over
    f64 max_pairs_ms = 0.0;
    u32 last_upload_frames = 0;  // frames the last re-centre's chunks took to upload
    u64 last_upload_bytes = 0;
    u32 last_swap_frames = 0;    // frames from asking for the last re-centre to drawing it
    u32 last_frozen_frames = 0;  // of which the rings held their pairs
    f64 arena_peak_share = 0.0;  // the fullest a ring's arenas were, at a swap: old and new chunks
  };
  const RingStats& ring_stats() const noexcept { return ring_stats_; }
  // **The last frame's layout** (the `--benchmark` records' `terrain`; renderer.md, "What a frame
  // waits for"): how far behind the camera the drawn rings or tiles were — the layout drawn was
  // asked for `lag_frames` frames ago, with the camera `lag_m` metres on the ground from where it
  // is now (0 offscreen, where a frame waits for its layout) — what a rebuild swapped in this frame
  // built and let go and how long its worker took, the chunk bytes this frame staged, and the
  // milliseconds `frame` spent on the frame's own thread.
  struct FrameLayout {
    f64 lag_m = 0.0;
    u32 lag_frames = 0;
    u32 built = 0;
    u32 dropped = 0;
    f64 rebuild_ms = 0.0;
    u64 upload_bytes = 0;
    f64 host_ms = 0.0;
  };
  const FrameLayout& frame_layout() const noexcept { return frame_layout_; }
  // The most the layout fell behind over the run, metres and frames (RingStats-like, for a
  // summary).
  f64 max_layout_lag_m() const noexcept { return max_lag_m_; }
  u32 max_layout_lag_frames() const noexcept { return max_lag_frames_; }
  bool has_rings() const noexcept { return rings_ != nullptr; }
  // The level set the rebuilds run on: the rings, or the world's tiles.
  const TerrainLevelSet* level_set() const noexcept { return rings_; }
  // The layout the frames draw: the last re-centre swapped in.
  const TerrainRingLayout& ring_layout() const noexcept { return shown_layout_; }

  // What a summary reports (renderer.md, "The dunes in time-lapse").
  struct LevelStats {
    f64 spacing_m = 0.0;
    f64 surface_s = 0.0;  // absolute game time the surface stands for
    f64 time_a = 0.0;
    f64 time_b = 0.0;
    f64 blend = 0.0;
    f64 padding_m = 0.0;
    u32 evaluated = 0;      // fields evaluated after the first
    u32 installed = 0;      // fields that became a pair's b
    u32 held = 0;           // frames the surface waited at b for a field
    u32 capped = 0;         // frames the per-frame bound slowed the surface
    u32 waited = 0;         // frames that waited for a field (`wait`)
    u32 late = 0;           // fields timed past the displacement rule to keep up (`lead`)
    f64 max_move_m = 0.0;   // the most any vertex moved in one frame
    f64 max_delta_m = 0.0;  // the largest |b - a| of a pair
    f64 max_step_s = 0.0;   // the longest game-time step between two fields
    f64 max_lag_s = 0.0;    // the most the surface fell behind game time
    f64 last_eval_ms = 0.0;
    f64 total_eval_ms = 0.0;
    f64 last_turnaround_s = 0.0;  // real seconds from wanting the last field to its being ready
    f64 max_turnaround_s = 0.0;
    u64 last_hash = 0;  // of the last field's bytes
    // This frame's (the `--benchmark` records' `terrain` object): the most any vertex moved, and
    // whether this level's newest field was what the surface stood or braked for.
    f64 frame_move_m = 0.0;
    bool frame_held = false;
    bool frame_late = false;
    u32 ahead = 0;  // fields after b: evaluating, uploading or ready
    u32 ready = 0;  // of which ready to be taken as b
  };
  u32 level_count() const noexcept { return levels_.size(); }
  LevelStats level_stats(u32 level) const noexcept;
  // The move of every vertex of every level in the last frame, metres: the largest.
  f64 last_move_m() const noexcept { return last_move_m_; }
  // The surface clock (renderer.md, "A clock that never stops"): L, how far behind game time the
  // surface is meant to stand, and its largest over the run, game seconds (0 with `wait`, whose
  // surface keeps game time and waits for its fields instead).
  f64 latency_s() const noexcept { return clock_.latency_s(); }
  f64 max_latency_s() const noexcept { return max_latency_s_; }
  // How far behind game time the surface stands now, game seconds.
  f64 lag_s() const noexcept;
  // The surface's speed over the rate in the last frame: 1 is the true speed.
  f64 speed_ratio() const noexcept { return speed_ratio_; }
  // Frames the surface stood still while game time ran (after it first moved), and frames the
  // clock braked for a field later than L allowed.
  u32 stopped_frames() const noexcept { return stopped_frames_; }
  u32 braked_frames() const noexcept { return braked_frames_; }

 private:
  struct Field {
    u32 slot = ~0u;
    f64 time_s = 0.0;
    f64 padding_m = 0.0;  // largest |field - rest| over what the level draws
    f64 delta_m = 0.0;    // largest |field - the field before it|
    gfx::TerrainField window;
  };
  // A field the worker keeps on the CPU: a level's newest few, for the next one's delta and a
  // re-centre's chunks.
  struct Cached {
    f64 time_s = 0.0;
    gfx::TerrainField window;
    Vector<f32> heights;
  };
  struct Level {
    f64 spacing_m = 0.0;
    // The scene's grid under the world's tiles: drawn by nothing, evaluated never, and no part of
    // the surface's time (it would hold it at a field it never gets).
    bool hidden = false;
    // Where a tile level's heights come from (the set's tile source); null: the scene's sampler.
    const scene_gen::TileSource* source = nullptr;
    u32 slots = 0;             // field slots the GPU scene gave it
    TerrainLattice lattice;    // the level's (GpuScene::terrain_lattice)
    gfx::TerrainField window;  // what the level's fields cover under the layout drawn
    Field a;
    Field b;
    bool has_b = false;
    // The fields after b, oldest first: asked of the worker, being copied onto the device a budget
    // a frame, and ready to be taken as b once their last piece is in a frame. Up to two of them —
    // a ring's four slots are a, b and these — so a level asks for the one after next while the
    // next waits to be drawn, and a field that waits behind another level's on the one worker is
    // not late (renderer.md, "A clock that never stops"). One at a time is on its way.
    static constexpr u32 k_ahead = 2;
    // How many the level's slots leave room for: two for a ring (four slots), one for the scene's
    // grid (three). A level with room for one times its fields with twice the lead, because it
    // has no second field to cover a wait behind the others' evaluations.
    u32 max_ahead() const noexcept { return slots > 3 ? k_ahead : 1u; }
    enum class Next : u8 { none, evaluating, uploading, ready };
    Field next[k_ahead];
    Next next_state[k_ahead] = {Next::none, Next::none};
    u32 ahead = 0;
    bool on_its_way() const noexcept { return ahead > 0 && next_state[ahead - 1] != Next::ready; }
    f64 newest_s() const noexcept {
      return ahead > 0 ? next[ahead - 1].time_s : (has_b ? b.time_s : a.time_s);
    }
    f64 newest_delta_m = 0.0;  // the newest field's difference from the one before it
    f64 wanted_s = 0.0;  // real time it came to want a field; negative while it has room for none
    TerrainBlend blend;
    f64 eval_ms_ema = 0.0;
    LevelStats stats;
    Vector<u32> shown_slots;   // a ring level's chunks drawn: their slots
    Vector<u8> shown_mask;     // and the same, one byte a slot (sized for the level's slots)
    bool shown_stale = false;  // a swap of changes moved the marks and not the list
    // The worker's alone once the motion has started (the frame never reads them): the heights
    // the scene's grid was built from (level 0), and the level's recent fields.
    Vector<f32> rest;
    Vector<Cached> cache;
  };
  // A ring level's pair over a re-centre's layout: the pair drawn when the rings froze, over the
  // new layout's window when the ring moved (slots of their own), or the same fields with their
  // paddings measured again when only its chunks changed.
  struct Pair {
    bool moved = false;           // its chunks changed
    bool window_changed = false;  // and the window its fields cover with them
    bool has_b = false;
    // The level held its pair for this rebuild (`freeze_mask_`). A tile level whose window stays
    // where it was is not frozen: it goes on taking fields while its tiles are swapped, and the
    // pairs step measures every field it holds against the new tiles instead (`measured_*`), which
    // the swap applies to whichever of them the level still holds, by time.
    bool frozen = true;
    u32 measured = 0;
    f64 measured_time[8] = {};
    f64 measured_padding[8] = {};
    Field a;
    Field b;
    gfx::BufferResource staging_a;
    gfx::BufferResource staging_b;
  };
  // One piece of the field worker's work, handed to it and back: a field, or a re-centre's pairs.
  struct Task {
    enum class Kind : u8 { field, pairs } kind = Kind::field;
    // A field of `level` at `time_s` over `window`, written into `staging`; its delta is measured
    // from the level's field at `from_s`, and cached fields before `keep_from_s` are dropped.
    u32 level = 0;
    f64 time_s = 0.0;
    f64 from_s = 0.0;
    f64 keep_from_s = 0.0;
    gfx::TerrainField window;
    gfx::BufferResource staging;
    f64 delta_m = 0.0;
    f64 padding_m = 0.0;
    f64 eval_ms = 0.0;
    u64 hash = 0;
    u64 evaluated = 0;  // the samples the source was asked for (the rest were copied)
    // A re-centre's pairs: per ring level, the pair to carry over to the new layout.
    Pair pairs[k_max_terrain_levels];
    bool done = false;
  };
  // A re-centre's chunks, on the ring worker: the rule for a camera at (x, z), the chunks it
  // changes rebuilt from the field at `time_s`.
  struct RingTask {
    WorldPos camera;
    f64 time_s = 0.0;
    TerrainRingLayout layout;  // where the frame expects the rings to land
    u32 moved = 0;
    u32 built = 0;
    u32 kept = 0;
    u32 dropped = 0;  // chunks the levels held before and did not keep
    f64 ms = 0.0;
    bool ok = true;
    std::string error;
    bool done = false;
  };
  void worker_main();
  void ring_worker_main();
  // A level's heights over a window at a time: from its tile source, or the scene's sampler.
  void evaluate_level(const Level& level, f64 time_s, const gfx::TerrainField& window,
                      Vector<f32>& out);
  // When a level's next field after `from_s` is due by the cadence rule.
  f64 next_time_of(const Level& level, f64 from_s) const noexcept;
  // The one surface time every level shares: the first drawn level's.
  f64 surface_s() const noexcept;
  void run_field(Task& task);
  void run_pairs(Task& task);
  void run_rings(RingTask& task);
  const f32* field_heights(Level& level, f64 time_s, const gfx::TerrainField& window,
                           Vector<f32>& scratch);
  // Returns the samples it asked the source for.
  u64 field_over(Level& level, f64 time_s, const gfx::TerrainField& window, Vector<f32>& out);
  void keep_field(Level& level, f64 time_s, const gfx::TerrainField& window, Vector<f32>&& heights,
                  f64 keep_from_s);
  bool worker_busy();
  void wait_worker();
  void post(Task&& task);
  bool schedule(u32 level);
  void schedule_next();
  void ask_recentre(WorldPos camera);
  bool schedule_pairs();
  void advance_recentre(bool complete);
  bool take_rings(bool block);
  void take_finished();
  void take_field(Task& finished);
  void take_pairs(Task& finished);
  bool upload_chunks(bool all);
  void freeze_rings();
  void swap_rings();
  void abandon_recentre(const std::string& why);
  u32 free_slot(const Level& level, u32 besides = ~0u) const noexcept;
  void field_ready(u32 level, u32 entry);
  void drop_ahead(u32 level);
  void show(u32 level);

  GpuScene* scene_ = nullptr;
  TerrainLevelSet* rings_ = nullptr;
  TimeLapseConfig config_;
  jobs::JobSystem* jobs_ = nullptr;
  std::unique_ptr<TerrainDesc> desc_;
  std::unique_ptr<TerrainSampler> sampler_;
  Vector<Level> levels_;
  f64 start_s_ = 0.0;
  f64 game_s_ = 0.0;
  f64 real_s_ = 0.0;  // real seconds the frames have stood for
  f64 frame_s_ema_ = 1.0 / 60.0;
  f64 last_move_m_ = 0.0;
  TerrainClock clock_;
  f64 max_latency_s_ = 0.0;
  f64 speed_ratio_ = 0.0;
  f64 start_rate_ = 0.0;
  u32 rate_changes_ = 0;
  bool moved_once_ = false;
  u32 stopped_frames_ = 0;
  u32 braked_frames_ = 0;
  // **A re-centre, from its asking to its swap**: the chunks rebuilt on the ring worker while the
  // sand moves on (`rebuilding`), uploaded into free slots a budget a frame (`uploading`); then the
  // rings freeze their pairs (`pairs_asked`) while the field worker carries them over to the new
  // layout (`pairing`), whose copies are handed to the scene (`swapping`); then the swap, in the
  // frame that records those copies.
  enum class Recentre : u8 {
    none,
    rebuilding,
    uploading,
    pairs_asked,
    pairing,
    swapping
  } recentre_ = Recentre::none;
  // A ring level holds its pair from the freeze to the swap: the pair is what the swap carries
  // over to the new layout, at the same blend. Every ring level is frozen at every re-centre; a
  // tile level only when its window moves (`freeze_rings`), since a tile set's rebuilds come with
  // every tile a camera crosses and a freeze drops the fields a level has after b (the model in
  // terrain_clock_tests.cpp: freezing every level at every rebuild stood the sand still a frame in
  // a hundred and sent it on at a third more than the rate).
  bool frozen(u32 level) const noexcept {
    return level > 0 && recentre_ >= Recentre::pairs_asked && (freeze_mask_ & (1u << level)) != 0;
  }
  u32 freeze_mask_ = 0;
  TerrainRingLayout shown_layout_;  // what the frames draw
  TerrainRingLayout pending_layout_;
  u32 pending_moved_ = 0;
  Pair pending_[k_max_terrain_levels];
  struct Upload {
    u32 level = 0;
    u32 chunk = 0;
  };
  Vector<Upload> uploads_;
  // The swap's scratch, sized at `start` for the most slots a level has, so a swap allocates
  // nothing: the slots a level draws from the swap on, and one byte a slot marking them.
  Vector<u32> swap_slots_;
  Vector<u8> swap_mark_;
  u32 upload_next_ = 0;
  u32 upload_frames_ = 0;  // of the re-centre being uploaded
  u64 upload_bytes_ = 0;
  f64 stage_ms_ = 0.0;     // the frame's thread staging its chunks, over every frame it took
  u64 pairs_samples_ = 0;  // the samples its pairs asked the source for
  u64 upload_budget_bytes_ = 0;
  bool rings_stopped_ = false;  // a re-centre failed: the rings stay where they are
  u64 frames_ = 0;
  u64 asked_frame_ = 0;
  u64 frozen_frame_ = 0;
  RingStats ring_stats_;
  // Where the camera was when the layout drawn, and the one being made, were asked for, and the
  // frame each was asked on: what a frame's lag is measured from (`FrameLayout`).
  WorldPos shown_camera_;
  u64 shown_asked_frame_ = 0;
  bool shown_camera_set_ = false;
  WorldPos pending_camera_;
  FrameLayout frame_layout_;
  f64 max_lag_m_ = 0.0;
  u32 max_lag_frames_ = 0;
  u32 pending_built_ = 0;  // the rebuild on its way to a swap: what it built and let go
  u32 pending_dropped_ = 0;
  f64 pending_rebuild_ms_ = 0.0;
  // The field worker: one task at a time, in the order asked.
  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable done_;
  Task task_;
  bool busy_ = false;  // a task is posted and not yet taken back
  bool stop_ = false;
  // The ring worker: one re-centre's chunks at a time.
  std::thread ring_worker_;
  std::mutex ring_mutex_;
  std::condition_variable ring_wake_;
  std::condition_variable ring_done_;
  RingTask ring_task_;
  bool ring_busy_ = false;
  bool ring_stop_ = false;
};

}  // namespace engine::renderer

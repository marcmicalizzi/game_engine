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
#include <systems/renderer/terrain.h>
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
  f64 min_step_s = 60.0;  // game seconds between fields at least: `renderer.terrain.min_step_s`
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

// **The cadence rule**: the game time after `from_s` at which the generator's fastest band will
// have travelled `fraction * spacing_m` along the wind's path (`terrain_band_travel_m`), clamped to
// [from + min_step, from + max_step]. Found by bisection on the closed form, whose cost does not
// depend on the time. Without a generator, `from_s + max_step_s`.
f64 terrain_next_time(const TerrainSampler& sampler, f64 from_s, f64 spacing_m, f64 fraction,
                      f64 min_step_s, f64 max_step_s) noexcept;

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
// the fields it took. At most `k_max_surface_levels` levels.
inline constexpr u32 k_max_surface_levels = 8;
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
  // or a negative rate. `rings` is the set the GPU scene was made with, or null; the motion owns it
  // from here (its worker rebuilds it). Evaluates every level's field at the scene's own time on
  // the calling thread (through `jobs` when given) and hands it over, so the first frame draws it:
  // the rest pose, to the bit on the scene's grid. A rate of zero is a still field whose rings
  // follow the camera. The scene and the rings must outlive the motion.
  bool start(GpuScene& scene, TerrainRingSet* rings, const TimeLapseConfig& config,
             jobs::JobSystem* jobs, std::string* error = nullptr);
  // Before a frame: game time moves on by `real_dt_s * rate`, each level's surface towards it, a
  // finished field goes to the GPU scene and the next is asked for; with rings, a camera that has
  // left a ring's middle half asks for a re-centre, and a finished one is uploaded and swapped in.
  // What the frame draws is handed to the scene (`GpuScene::terrain_show`, `terrain_chunk_show`).
  void frame(f64 real_dt_s, f32 camera_x = 0.0f, f32 camera_z = 0.0f);
  // Stops the worker, waiting for a field or a re-centre in flight.
  void finish();

  bool active() const noexcept { return scene_ != nullptr; }
  const TimeLapseConfig& config() const noexcept { return config_; }
  f64 game_time_s() const noexcept { return start_s_ + game_s_; }

  // What the rings did (renderer.md, "The rings in the scene").
  struct RingStats {
    u32 rebuilds = 0;         // re-centres the worker built
    u32 swaps = 0;            // and the frames that swapped them in
    u32 chunks_built = 0;     // chunks rebuilt over all re-centres
    u32 chunks_kept = 0;      // chunks a re-centre kept
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
  bool has_rings() const noexcept { return rings_ != nullptr; }
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
    u64 last_hash = 0;  // of the last field's bytes
  };
  u32 level_count() const noexcept { return levels_.size(); }
  LevelStats level_stats(u32 level) const noexcept;
  // The move of every vertex of every level in the last frame, metres: the largest.
  f64 last_move_m() const noexcept { return last_move_m_; }

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
    u32 slots = 0;             // field slots the GPU scene gave it
    TerrainLattice lattice;    // the level's (GpuScene::terrain_lattice)
    gfx::TerrainField window;  // what the level's fields cover under the layout drawn
    Field a;
    Field b;
    Field next;
    bool has_b = false;
    // The next field: asked of the worker, being copied onto the device a budget a frame, and
    // ready to be taken as b once its last piece is in a frame.
    enum class Next : u8 { none, evaluating, uploading, ready } next_state = Next::none;
    TerrainBlend blend;
    f64 eval_ms_ema = 0.0;
    LevelStats stats;
    Vector<u32> shown_slots;  // a ring level's chunks drawn: their slots
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
    // A re-centre's pairs: per ring level, the pair to carry over to the new layout.
    Pair pairs[k_max_terrain_levels];
    bool done = false;
  };
  // A re-centre's chunks, on the ring worker: the rule for a camera at (x, z), the chunks it
  // changes rebuilt from the field at `time_s`.
  struct RingTask {
    f32 camera_x = 0.0f;
    f32 camera_z = 0.0f;
    f64 time_s = 0.0;
    TerrainRingLayout layout;  // where the frame expects the rings to land
    u32 moved = 0;
    u32 built = 0;
    u32 kept = 0;
    f64 ms = 0.0;
    bool ok = true;
    std::string error;
    bool done = false;
  };
  void worker_main();
  void ring_worker_main();
  void run_field(Task& task);
  void run_pairs(Task& task);
  void run_rings(RingTask& task);
  const f32* field_heights(Level& level, f64 time_s, const gfx::TerrainField& window,
                           Vector<f32>& scratch);
  void field_over(Level& level, f64 time_s, const gfx::TerrainField& window, Vector<f32>& out);
  void keep_field(Level& level, f64 time_s, const gfx::TerrainField& window, Vector<f32>&& heights,
                  f64 keep_from_s);
  bool worker_busy();
  void wait_worker();
  void post(Task&& task);
  bool schedule(u32 level);
  void schedule_next();
  void ask_recentre(f32 camera_x, f32 camera_z);
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
  void show(u32 level);

  GpuScene* scene_ = nullptr;
  TerrainRingSet* rings_ = nullptr;
  TimeLapseConfig config_;
  jobs::JobSystem* jobs_ = nullptr;
  std::unique_ptr<TerrainDesc> desc_;
  std::unique_ptr<TerrainSampler> sampler_;
  Vector<Level> levels_;
  f64 start_s_ = 0.0;
  f64 game_s_ = 0.0;
  f64 last_move_m_ = 0.0;
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
  // over to the new layout, at the same blend.
  bool frozen(u32 level) const noexcept { return level > 0 && recentre_ >= Recentre::pairs_asked; }
  TerrainRingLayout shown_layout_;  // what the frames draw
  TerrainRingLayout pending_layout_;
  u32 pending_moved_ = 0;
  Pair pending_[k_max_terrain_levels];
  struct Upload {
    u32 level = 0;
    u32 chunk = 0;
  };
  Vector<Upload> uploads_;
  u32 upload_next_ = 0;
  u32 upload_frames_ = 0;  // of the re-centre being uploaded
  u64 upload_bytes_ = 0;
  u64 upload_budget_bytes_ = 0;
  bool rings_stopped_ = false;  // a re-centre failed: the rings stay where they are
  u64 frames_ = 0;
  u64 asked_frame_ = 0;
  u64 frozen_frame_ = 0;
  RingStats ring_stats_;
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

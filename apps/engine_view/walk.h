#pragma once

// **The walk mode** of `engine-view --interactive` (docs/subsystems/apps.md, "Walking"): the camera
// at eye height on a walker that stands on the scene's ground, the look as in flight, W A S D along
// the ground in the look's heading, Shift to sprint and Space to jump.
//
// **Two walkers, one interface.** Where this build carries the physics and scene-collision
// capabilities, the walker is a `physics::CharacterBody` — a capsule with a slope limit, a step
// height, gravity and a jump — on the static bodies `scene_collision::SceneCollision` keeps round
// it: the ground as a heightfield a tile, the ruins' walls and rubble as a compound a tile. Where
// it does not (`ENGINE_WITH_PHYSICS=OFF`, the minimal build), it **follows the ground**: the feet
// on the scene's ground provider's height, a jump an arc over it, and nothing else in the way — the
// summary says which (`walk.collision`: "physics" or "ground-follow").
//
// **Why it is here and not in a module.** The walker turns a camera's yaw into a direction with
// `fly_sin_cos` (fly_camera.h: the C library's sine is not the same function on every compiler), it
// is fed by the fly session's tick, and it is the camera's, as `fly_camera.h` is: a game writes its
// own. The two modules underneath — the character and the collision round it — are the reusable
// half.
//
// **Determinism.** A walk is a function of the session's ticks and events, the header's numbers and
// the scene: the collision ring is updated once a tick from the walker's own feet, not once a frame
// from the camera, so the bodies are made and let go at the same ticks in a session and in its
// replay, and the character meets the same bodies with the same ids (physics.md, "The character").
// A ground the time-lapse moves is the exception: its drawn time is set once a frame from the
// renderer, whose clock a window's display drives.
//
// **The feet are a `WorldPos`** (ADR-0053): the character's own, or the ground-follow's, integrated
// in f64 from a float32 tick (fly_camera.h, "Where the camera is"), so a walk 10,000 km out moves
// as one by the origin does. Until 2026-10-05 they were float32, and 420 km out a 6 mm tick along
// x rounded away.

#include <core/base/types.h>
#include <core/json/json_value.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/scene_gen/scene_gen.h>

#include <memory>
#include <schemas/scene.h>
#include <string>

namespace engine::renderer {
struct SceneData;
struct FrameFlora;
}  // namespace engine::renderer

namespace engine::view {

// **The walk integration's version.** Everything a walk is a function of besides the events, the
// header's numbers and the scene — the heading's arithmetic, the order of the ring, the refresh and
// the step, the ground-follow's jump — is this number. A session header records it, and a replay
// under another is refused rather than walked somewhere else.
//
// **2** (2026-10-05): the feet are f64 and hashed as doubles. A version 1 block — every walk
// recorded before, whose numbers mean what they meant — is read and walked as version 2, with the
// fly camera's version 1 (`k_fly_version_f32`, fly_camera.h).
inline constexpr u32 k_walk_version = 2;
inline constexpr u32 k_walk_version_f32 = 1;

// The walker's numbers: the session header's `walk` block, read from the `view.walk.*` and
// `walk.collision.*` tunables when a live session starts, so a replay walks with the recording's.
struct WalkParams {
  f32 height = 1.8f;
  f32 radius = 0.3f;
  f32 eye_height = 1.65f;
  f32 speed = 1.5f;           // walking, metres a second
  f32 sprint = 5.0f;          // with the `fast` action held
  f32 max_slope_deg = 40.0f;  // steeper slides and cannot be walked up
  f32 step_height = 0.35f;
  f32 gravity = 9.81f;
  f32 jump_speed = 4.0f;
  // The collision's, when the build carries it (scene_collision.h).
  f32 spacing_m = 1.0f;
  f32 radius_tiles = 1.5f;
  u32 max_tiles = 2;
  f32 mesh_error_m = 0.05f;
  f32 ground_error_m = 0.05f;
  u32 max_refreshes = 2;
};
WalkParams walk_params_from_tunables();
JsonValue walk_params_to_json(const WalkParams& params);
// False with the reason for a block this engine-view cannot walk with: another walk version, a
// missing or mistyped field.
bool walk_params_from_json(const JsonValue& value, WalkParams& out, std::string* error);

// One tick's intent: the `move` axis (x right, y forward, a stick's tilt or two keys' diagonal),
// the look's yaw it is along, sprint held, jump pressed on this tick.
struct WalkInput {
  Vec2 move{};
  f32 yaw = 0.0f;
  bool sprint = false;
  bool jump = false;
};

// The ground as the frames draw it (renderer.md, "The dunes in time-lapse"): the lattice of the
// scene's terrain grid, and a still ground or the pair of fields and the blend the frames draw.
// Set once a frame, before its ticks. Where the frames draw the world's tiles (renderer.md, "The
// ground from the world's tiles"), `tiles` is their source and `lattice` the finest tile level's —
// the world's lattice at that level's spacing, without an edge — and the ground as drawn is read
// from the tiles.
struct DrawnGround {
  scene_gen::Lattice lattice;
  const scene_gen::TileSource* tiles = nullptr;
  bool moving = false;
  f64 time_a = 0.0;
  f64 time_b = 0.0;
  f64 blend = 0.0;
};

// **Wading** (docs/subsystems/water.md, "Wading"; roadmap S63): the depth of water at the walker's
// feet scales its speed by a declared curve — full speed to the ankle (0.3 m), halving by the waist
// (1 m), a slow wade to the chest (1.4 m) at a quarter — and **at the chest the water is a wall**:
// a step whose heading leads deeper (the depth a quarter metre ahead along it is greater than at
// the feet) is not taken, so the walker stops there and wades back the way it came. No swimming
// this stage (roadmap S86). The depth comes from whoever owns the water (engine-view asks the water
// capability, `water::depth_at`), as a plain function of a position, so a walk through water
// replays bit for bit; a walker with no depth function, or at a depth of 0, takes the speeds it
// always did, to the bit (`x * 1.0f` is `x`).
inline constexpr f32 k_wade_ankle_m = 0.3f;
inline constexpr f32 k_wade_waist_m = 1.0f;
inline constexpr f32 k_wade_chest_m = 1.4f;
inline constexpr f32 k_wade_probe_m = 0.25f;  // how far ahead "deeper" is asked
// The curve: 1 to the ankle, falling linearly to 0.5 at the waist and to 0.25 at the chest, and
// 0.25 beyond (where only a step back is taken).
f32 wade_scale(f32 depth) noexcept;
// The depth of water at a world position, metres; 0 where there is none.
using DepthFn = f32 (*)(const void* context, WorldPos at) noexcept;

struct WalkStats {
  u64 ticks = 0;         // ticks walked
  f64 distance_m = 0.0;  // horizontal distance walked
  f64 climb_m = 0.0;     // height gained, summed over every tick that gained any
  u32 drops = 0;         // times put on the ground (a switch to walking, or a teleport)
  u32 teleports = 0;     // of which a game's `walker_teleport`
  // The largest distance, at the walker's feet, between the ground it stands on — the collision
  // heightfield, or the provider's height when it follows the ground — and the ground as drawn (the
  // scene's grid, or the finest level of the world's tiles, as the renderer triangulates it, at the
  // drawn time), metres.
  f32 max_ground_error_m = 0.0f;
  u64 hash = 0;  // the walker's own state chain: the character's, or the ground-follow's
  // Wading (`Walker::set_water`): the deepest water the feet stood in, the ticks they stood in any,
  // and the ticks the chest's wall stopped a step.
  f32 max_depth_m = 0.0f;
  u64 wading_ticks = 0;
  u64 stopped_ticks = 0;
};

// **Where a walker's ticks spend wall time** (apps.md, "The frame record's walk"), over the walk:
// the steps, the ground asked for outside the collision (the drawn ground under the feet, the
// ground-follow's height, a game's `ground_height`) and the water's depth, the ring's update and
// the collision's refresh before each step (`ring_ns`, every activation inside it), and the
// collision's activations by stage. Counters only: nothing the walker computes reads them, so a
// replay walks the same whatever they say.
struct WalkTiming {
  u64 steps = 0;
  i64 step_ns = 0;
  i64 max_step_ns = 0;
  u64 ground_queries = 0;
  i64 ground_query_ns = 0;
  i64 max_ground_query_ns = 0;
  u64 water_queries = 0;
  i64 water_ns = 0;
  i64 ring_ns = 0;
  u64 activations = 0;
  i64 activate_ns = 0;
  i64 max_activate_ns = 0;
  i64 ground_sample_ns = 0;
  i64 placement_ns = 0;
  i64 body_ns = 0;
  // The ground built ahead of the ring (scene_collision.h, `prefetch`): activations that found
  // their tile's build finished, waited for it running, or built it themselves.
  u64 prefetched = 0;
  u64 prefetch_waits = 0;
  i64 prefetch_wait_ns = 0;
  u64 prefetch_inline = 0;
  // The most one step and one ground query took since `Walker::take_max_ns` last asked.
  i64 recent_max_step_ns = 0;
  i64 recent_max_query_ns = 0;
};

class Walker {
 public:
  Walker();
  ~Walker();
  Walker(const Walker&) = delete;
  Walker& operator=(const Walker&) = delete;

  // Over `scene`'s terrain, and its placements where the build collides with them, stepping at
  // `tick_hz`. `scene` must outlive the walker. A scene with no terrain cannot be walked:
  // `available()` is false and says why, and nothing else here is called. False with `error` only
  // for a collision that cannot be made (a generator the build does not carry). `tiles`: the tile
  // source the renderer draws the world's tiles from, when it does, which the collision's ground is
  // then made of (renderer.md, "The ground from the world's tiles"): what is walked on is what is
  // drawn. It must outlive the walker.
  bool start(const WalkParams& params, u32 tick_hz, const renderer::SceneData& scene,
             std::string* error, const scene_gen::TileSource* tiles = nullptr);
  bool available() const noexcept;
  // "physics" or "ground-follow" (or "none" before `start` or without a terrain), and the sentence
  // saying why a walker is not the physical one.
  const char* collision() const noexcept;
  const std::string& why() const noexcept;
  const WalkParams& params() const noexcept;

  // Before a frame's ticks: the ground as that frame draws it.
  void set_drawn(const DrawnGround& drawn) noexcept;
  // **A game's scales on the speeds** (game_runner.h, `walker_set`): the header's `speed` and
  // `sprint` times these, each held to [0, 4], from the next step on. 1 is the header's number to
  // the bit — `x * 1.0f` is `x`, and contraction is off tree-wide (ADR-0035) — so a walk with no
  // game and a walk whose game never set them are the same floats, and every walk recorded before
  // the scales existed replays as it did.
  void set_speed_scale(f32 walk, f32 sprint) noexcept;
  f32 speed_scale() const noexcept;
  f32 sprint_scale() const noexcept;
  // **The water the walker wades in** (above, "Wading"): asked at the feet before every step, and a
  // quarter metre ahead along the heading at the chest. `context` must outlive the walker. Null:
  // no water, the walker as it always was.
  void set_water(DepthFn depth, const void* context) noexcept;
  // **A teleport** (game_runner.h, `walker_teleport`): the feet put at `at`'s x and z on the ground
  // under it — the highest surface below, or the ground's height — as a `drop` is, counted as one
  // and as a teleport; the eye returned.
  WorldPos teleport(WorldPos at);
  // The most one step and one ground query took since the last call, nanoseconds, and those
  // maxima started again: a frame record's own (`WalkProbe`).
  void take_max_ns(i64& step, i64& query) noexcept;
  // A switch to walking: the feet on the ground under `camera` — the highest surface below it, or
  // the ground's own height where there is none — and the eye returned.
  WorldPos drop(WorldPos camera);
  // A tick of flying: the collision tiles round the camera built ahead on the walker's worker
  // (scene_collision.md, "The placements built ahead"), so a switch to walking here finds them
  // ready instead of building them on its tick. It makes no body and changes no step, so a
  // recorded session replays the same with it.
  void look_ahead(WorldPos camera);
  // One tick of walking; the eye.
  WorldPos step(const WalkInput& input);
  WorldPos eye() const noexcept;
  WorldPos feet() const noexcept;
  // Whether the feet stand on something after the last step: the character's `OnGround`, or the
  // ground-follow's not being in a jump. False before the first drop. Read only: a game asks it
  // (game_runner.h), and nothing the walker computes depends on it being asked.
  bool on_ground() const noexcept;
  // **Whether the ground under the feet is ready** (apps.md, "--startup-trace"): placed, and, for
  // the physical walker, a collision tile held under the feet (`SceneCollision::ground_height`
  // answers there); the ground-follow stands on the provider's heights, ready once placed. Read
  // only, like `on_ground`.
  bool ground_ready() const noexcept;
  // **The ground's height at (x, z)** (game_runner.h `ground_height`, module_abi.h ABI 5): the
  // provider's height at the nearest millimetre, the function the ground-follow stands on — at the
  // terrain's own time on still ground, the drawn pair blended as the frame draws it on moving
  // ground. False without a ground (no terrain). Asking changes nothing the walker computes.
  bool ground_height(f64 x, f64 z, f64& out);

  const WalkStats& stats() const noexcept;
  // The walk's wall time so far, by where it went (`WalkTiming`).
  WalkTiming timing() const noexcept;
  // The ground the walker follows and stands the placements on; null before `start` or without a
  // terrain.
  const scene_gen::GroundProvider* ground() const noexcept;
  // The summary's block (apps.md, "Walking"): the collision and its counts beside the stats.
  JsonValue summary_json() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// **The walker's capsule, handed to a frame** (renderer.md, "The flora stage"): while `walking`,
// the feet and the header's radius and height go into the frame's `flora` block, where the
// renderer bends aside the fronds, leaves and twigs the capsule reaches; otherwise the block says
// no walker. A frame's walker is the last tick's feet, so a replay hands every frame the capsule
// its recording did, and the trees' poses follow it bit for bit.
void set_frame_walker(const Walker& walker, bool walking, renderer::FrameFlora& out) noexcept;

// **A frame record's walk** (FrameRecord's `walk`, apps.md, "The frame record's walk"): the walk's
// counters, the frame thread's share of an authored ground's reader and every thread's, taken
// before a frame's ticks and after them, so a record says where a stalled frame's wall time went.
// Reads counters only; the walk is the same with or without one.
class WalkProbe {
 public:
  // `drawn`: the renderer's tile source, whose reader the terrain's workers share (null: none).
  void start(Walker& walker, const scene_gen::TileSource* drawn) noexcept;
  // Before the frame's ticks; `terrain_stage` is the terrain's re-centre then.
  void begin(u32 terrain_stage) noexcept;
  // After them: the frame's numbers.
  scene::FrameWalk end() noexcept;

 private:
  struct Reader {
    u64 locks = 0, wait_ns = 0, max_wait_ns = 0, hold_ns = 0, hits = 0, misses = 0, io_bytes = 0,
        io_ns = 0, load_waits = 0;
  };
  Reader thread_reader() const noexcept;
  Reader all_readers() const noexcept;
  Walker* walker_ = nullptr;
  const void* sets_[2] = {nullptr, nullptr};
  i64 started_ns_ = 0;
  i64 ticks_ns_ = 0;
  u32 stage_ = 0;
  WalkTiming timing_;
  Reader thread_;
  Reader all_;
};

}  // namespace engine::view

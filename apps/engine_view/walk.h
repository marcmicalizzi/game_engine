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

#include <core/base/types.h>
#include <core/json/json_value.h>
#include <core/math/math.h>
#include <domain/scene_gen/scene_gen.h>

#include <memory>
#include <string>

namespace engine::renderer {
struct SceneData;
}

namespace engine::view {

// **The walk integration's version.** Everything a walk is a function of besides the events, the
// header's numbers and the scene — the heading's arithmetic, the order of the ring, the refresh and
// the step, the ground-follow's jump — is this number. A session header records it, and a replay
// under another is refused rather than walked somewhere else.
inline constexpr u32 k_walk_version = 1;

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
// Set once a frame, before its ticks.
struct DrawnGround {
  scene_gen::Lattice lattice;
  bool moving = false;
  f64 time_a = 0.0;
  f64 time_b = 0.0;
  f64 blend = 0.0;
};

struct WalkStats {
  u64 ticks = 0;         // ticks walked
  f64 distance_m = 0.0;  // horizontal distance walked
  f64 climb_m = 0.0;     // height gained, summed over every tick that gained any
  u32 drops = 0;         // times put on the ground (a switch to walking)
  // The largest distance, at the walker's feet, between the ground it stands on — the collision
  // heightfield, or the provider's height when it follows the ground — and the ground as drawn (the
  // scene's grid, as the renderer triangulates it, at the drawn time), metres.
  f32 max_ground_error_m = 0.0f;
  u64 hash = 0;  // the walker's own state chain: the character's, or the ground-follow's
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
  // for a collision that cannot be made (a generator the build does not carry).
  bool start(const WalkParams& params, u32 tick_hz, const renderer::SceneData& scene,
             std::string* error);
  bool available() const noexcept;
  // "physics" or "ground-follow" (or "none" before `start` or without a terrain), and the sentence
  // saying why a walker is not the physical one.
  const char* collision() const noexcept;
  const std::string& why() const noexcept;
  const WalkParams& params() const noexcept;

  // Before a frame's ticks: the ground as that frame draws it.
  void set_drawn(const DrawnGround& drawn) noexcept;
  // A switch to walking: the feet on the ground under `camera` — the highest surface below it, or
  // the ground's own height where there is none — and the eye returned.
  Vec3 drop(Vec3 camera);
  // One tick of walking; the eye.
  Vec3 step(const WalkInput& input);
  Vec3 eye() const noexcept;

  const WalkStats& stats() const noexcept;
  // The summary's block (apps.md, "Walking"): the collision and its counts beside the stats.
  JsonValue summary_json() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace engine::view

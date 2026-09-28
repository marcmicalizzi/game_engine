#pragma once

// physics::CharacterBody: a walker (docs/subsystems/physics.md, "The character"; plan 05 §5.11,
// "Character controllers via Jolt's").
//
// **A capsule that moves by collision checks, not by the solver.** The backend's virtual character
// (Jolt's `CharacterVirtual`) is not a body the step integrates: it is swept through the world each
// tick by queries against the bodies that are there, slides along what it touches, climbs what is
// low enough to be a step, and stands on what is flat enough to stand on. So it moves exactly when
// the caller says, on the caller's thread, and a rigid body's contact solver never pushes a player
// through a wall because the step happened to be long. That is why plan 05 wants it for players and
// NPCs at LOD0/1, and why it is a class of its own rather than a `BodyDesc`: it has no body.
//
// **A fixed step and a plain state.** `step()` advances one tick of `1 / step_hz` — the
// simulation's tick, never a frame's duration — from a `CharacterInput`, so the same inputs over
// the same bodies land on the same floats, whatever the frame rate. `state()` is a plain struct
// (`CharacterState`) and `hash()` the chain of its bits over every step since `create`, which is
// what a replay compares. The backend is built with cross-platform determinism
// (physics.md, "Determinism"), and the character's own arithmetic is `+ - * /` and `sqrt` and the
// backend's own trigonometry, so the chain is the same number on every toolchain the tree builds
// for; the tests pin it.
//
// **What the caller decides, and what it does not.** The caller says which way to go (a horizontal
// direction in world space, at most unit length: a half-tilted stick walks at half speed), whether
// to sprint, and when jump is pressed; it does not turn a heading into a direction here, because
// the trigonometry that turns a yaw into one is the caller's (a camera's `fly_sin_cos`, a game's
// AI), and a physics module that called the C library's `sin` would give a different walk on
// another compiler. The rules — a capsule, a slope limit past which it slides, a step it climbs,
// gravity, a jump — are the config's, which a host fills from its tunables.
//
// Nothing from the backend is public (physics.md, "Why nothing from Jolt is public"): the character
// lives behind `CharacterBody::Impl` in src/.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/math/math.h>
#include <domain/physics/types.h>

namespace engine::physics {

class World;

// Where a character stands after a step: on something it can stand on, on something too steep to
// stand on (it slides), touching something that does not hold it up, or in the air. The backend's
// ground states, in the engine's words and order.
enum class Ground : u8 { OnGround, OnSteepGround, NotSupported, InAir };
const char* ground_name(Ground ground) noexcept;

// The rules of one character. Every length is metres, every speed metres a second; nothing here is
// read from a tunable — a host fills it from its own (engine-view's `view.walk.*`).
struct CharacterConfig {
  f32 height = 1.8f;       // the capsule, feet to crown
  f32 radius = 0.3f;       // at most half the height
  f32 eye_height = 1.65f;  // feet to eyes: where a first-person camera sits
  f32 walk_speed = 1.5f;   // a brisk walk
  f32 sprint_speed = 5.0f;
  // Past this a surface is too steep to stand on: the character slides down it and cannot walk up
  // it. Degrees from the horizontal.
  f32 max_slope_deg = 40.0f;
  f32 step_height = 0.35f;  // a ledge up to this high is climbed by walking into it
  f32 gravity = 9.81f;      // metres a second squared, down
  f32 jump_speed = 4.0f;    // straight up at the start of a jump: v^2 / 2g = 0.82 m high
  f32 mass = 80.0f;         // kg, what the character presses on what it stands on
  u32 step_hz = 60;         // the fixed step: 1 / step_hz seconds a `step()`
  // The backend sorts a character's contacts with other characters by an id, and its own default
  // is a process-wide counter, which would make a replay's result depend on how many characters the
  // process had made before. A character is given one here instead.
  u32 id = 1;
};

// One tick's intent. `move` is a direction in world space, horizontal (its y is ignored), at most
// unit length — a direction scaled by how far a stick is pushed; the character goes at
// `walk_speed` (or `sprint_speed`) times its length.
struct CharacterInput {
  Vec3 move{};
  bool sprint = false;
  bool jump = false;  // pressed on this tick: a jump starts only from ground it can stand on
};

// A character between two steps. `position` is the feet: the bottom of the capsule, which on flat
// ground is on the ground (a slope holds the capsule's round bottom a little above the point under
// its centre, radius * (1 / cos(slope) - 1)). 48 bytes, compared and hashed bit for bit.
struct CharacterState {
  Vec3 position{};
  Vec3 velocity{};       // metres a second, what the last step moved at
  Vec3 ground_normal{};  // of what it stands on; zero in the air
  Ground ground = Ground::InAir;
  u8 pad[3] = {};  // named and zeroed, so the struct has no byte nobody wrote
  u64 tick = 0;    // steps since `create`
};

// One state folded into a hash chain: every field by its bits, -0 as +0.
u64 hash_character_state(u64 seed, const CharacterState& state) noexcept;

class CharacterBody {
 public:
  CharacterBody() noexcept;
  ~CharacterBody();
  ENGINE_NON_COPYABLE(CharacterBody);

  // A character in `world` with its feet at `feet`. The world must outlive it (or `destroy` must
  // come first). InvalidArgument for a config that describes no capsule (a radius over half the
  // height, an eye above the crown, a step taller than the capsule, a speed or rate not above
  // zero), and for a second `create` without a `destroy`.
  Status create(World& world, const CharacterConfig& config, Vec3 feet);
  void destroy() noexcept;
  bool valid() const noexcept { return impl_ != nullptr; }
  const CharacterConfig& config() const noexcept;

  // One fixed step of `1 / step_hz`: the velocity the input asks for on the ground it stands on,
  // gravity, a jump when asked for from ground it can stand on, then the sweep — sliding along what
  // it hits, stepping up what is under `step_height`, sticking to the floor as it walks down a
  // slope, and holding still on a slope under the limit when it is asked to go nowhere.
  Status step(const CharacterInput& input);

  // Moves the feet to `feet` with no velocity and finds what it stands on there: a camera's
  // "walk from here", and a respawn. Not a step; the hash chain notes it.
  Status teleport(Vec3 feet);

  CharacterState state() const noexcept;
  Vec3 feet() const noexcept;
  Vec3 eye() const noexcept;  // the feet plus `eye_height` straight up
  // The chain over the state after `create`, every `step` and every `teleport`, in order.
  u64 hash() const noexcept;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace engine::physics

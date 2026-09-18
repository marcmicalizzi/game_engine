#pragma once

// physics: the engine's simulated world (plan 05 §5.11, §5.13, ADR-0026).
//
// Jolt 5.6 is the backend and is invisible from here: the surface is core/math (Vec3, Quat,
// Transform3, Aabb3), core/containers handles, core/time's fixed step, and `Status`. That is
// not politeness. ADR-0026 requires that replacing the solver not touch the asset format, the
// LOD policy, or the renderer path, and a header that leaked JPH types would make every
// consumer a Jolt consumer; it also keeps Jolt's build flags (which must match the ones its
// library was compiled with, or it aborts at startup) inside one translation-unit group.
//
// The step is fixed. `step(dt, sub_steps)` advances one SimTick; the sim owns the clock
// (core/time's FixedStepClock) and physics never reads a wall clock. Contacts are recorded
// into a buffer the caller drains after the step, because Jolt's listener callbacks run on
// worker threads and calling game code from there would make the tick order depend on the
// scheduler.
//
// Not wrapped yet, deliberately: constraints and motors, character controllers (plan 05 §5.11
// wants Jolt's, which is its own wrapper), ragdolls, vehicles, sensors and triggers, state
// save/restore for rollback (ADR-0016), and Jolt's GPU hair solver.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/math/math.h>
#include <core/time/time.h>
#include <domain/physics/soft_body.h>
#include <domain/physics/types.h>

#include <span>

namespace engine::jobs {
class JobSystem;
}

namespace engine::physics {

// --- world ----------------------------------------------------------------------------------

struct WorldOptions {
  // Hard caps, allocated up front. The body count is the real limit; the pair and constraint
  // counts bound the per-step scratch, and running out of either drops contacts rather than
  // failing, so they are sized generously (ADR-0017: no hidden limits, but the ones that
  // exist are visible here).
  u32 max_bodies = 10240;
  u32 max_body_pairs = 65536;
  u32 max_contact_constraints = 20480;
  u32 body_mutex_count = 0;  // 0 lets the backend choose

  Vec3 gravity{0.0f, -9.81f, 0.0f};
  u32 step_hz = 60;  // the fixed step; a game may choose 30 (plan 05 §5.2)

  // Sorts contacts and islands so the result does not depend on the order jobs happen to
  // finish in. Cross-platform determinism is a *build* property of the backend and is always
  // on (see docs/subsystems/physics.md); this flag is the per-world part of it and turning it
  // off buys a few percent in exchange for replayability.
  bool deterministic = true;

  // The debris pool (plan 05 §5.11). Spawning past the cap recycles the oldest piece.
  u32 debris_cap = 256;

  // Jolt's jobs run on the engine's performance pool. A null job system runs them inline on
  // the calling thread, which is the right answer for tools and for tests that want one
  // thread; `worker_count` of 0 means the pool's own worker count.
  jobs::JobSystem* job_system = nullptr;
  u32 worker_count = 0;

  // How many backend job objects the world keeps, allocated up front (ADR-0017: the limits
  // that exist are visible here). 0 takes the backend's own worst-case figure, which is what
  // every caller should use; the option exists so that a test can make the pool small enough
  // for exhaustion to be reachable. Rounded up to a power of two. A step that needs more than
  // this fails hard rather than stalling, so a value below roughly 10 + 8 * worker_count is
  // not a small pool but a broken one.
  u32 max_backend_jobs = 0;

  // Scratch for one step, allocated once. 0 sizes it from the limits above, which is what the
  // caller wants unless they have measured otherwise: the dominant term is one contact
  // constraint per `max_contact_constraints`, and getting it wrong is not a failure — the
  // arena falls back to malloc and the step shows up as a spike — but it is a spike.
  u32 temp_allocator_bytes = 0;
};

// The arena size `init` will use for these limits. Exposed so a caller can budget for it or
// print it; `WorldOptions::temp_allocator_bytes` overrides it.
u32 temp_allocator_size_for(const WorldOptions& options) noexcept;

struct WorldStats {
  u64 steps = 0;
  u64 backend_jobs = 0;             // jobs the backend handed to the job system
  u64 backend_jobs_on_workers = 0;  // of those, the ones that ran on a jobs::JobSystem worker
  // Backend jobs handed to the job system that have not run yet. Each one is holding a job out
  // of the world's fixed pool, so **this is zero every time `step()` returns** — the step
  // drains them, and the count existing at all is what makes that a checkable invariant
  // rather than a race that only a slow machine loses (docs/subsystems/physics.md).
  u32 backend_jobs_pending = 0;
  // Of `backend_jobs`, the ones that ran the soft-body constraint solve, and how many distinct
  // performance workers have executed one since the world was created. The second is the
  // answer to "does the soft-body solve actually spread?", which the cost of a cage makes
  // worth asking (ADR-0026's 1.5 ms tick budget); see docs/subsystems/physics.md. Workers past
  // 64 are folded into the last bit, because the set is one word.
  u64 soft_body_solve_jobs = 0;
  u32 soft_body_solve_workers = 0;
  // What the *last* step spent on deformable volumes, and whether that fits (ADR-0029 decision
  // 1). Wall clock, not a CPU-time sum, and per step rather than averaged: smoothing is the tier
  // logic's decision and a number that has already been smoothed cannot be un-smoothed. See
  // docs/subsystems/physics.md, "What a tick spends on deformables".
  SoftBodyBudget soft_body_budget;
  u32 body_count = 0;
  u32 active_body_count = 0;
  u32 soft_body_count = 0;
  u32 debris_count = 0;
  u32 shape_count = 0;
};

// --- shapes ---------------------------------------------------------------------------------

// One child of a compound shape, in the compound's local space.
struct CompoundChild {
  ShapeId shape{};
  Transform3 transform;
};

// A regular grid of heights. The surface is `offset + scale * (x, heights[z * sample_count +
// x], z)` for integer x and z, which is the terrain layout engine-view builds.
// `sample_count` must be at least 4 and a multiple of 2; a power of two is cheapest.
struct HeightfieldDesc {
  std::span<const f32> heights;
  u32 sample_count = 0;
  Vec3 offset{};
  Vec3 scale{1.0f, 1.0f, 1.0f};
};

// --- bodies ---------------------------------------------------------------------------------

struct BodyDesc {
  ShapeId shape{};
  Transform3 transform;  // `scale` is ignored: shapes carry their own size
  Vec3 linear_velocity{};
  Vec3 angular_velocity{};
  MotionType motion = MotionType::Dynamic;
  Layer layer = Layer::Moving;
  f32 mass = 0.0f;  // 0 computes the mass from the shape's volume and density
  f32 friction = 0.2f;
  f32 restitution = 0.0f;
  f32 linear_damping = 0.05f;
  f32 angular_damping = 0.05f;
  f32 gravity_factor = 1.0f;
  u64 user_data = 0;  // the caller's entity id; handed back on every contact event
  bool allow_sleeping = true;
  bool start_active = true;
};

// --- queries --------------------------------------------------------------------------------

struct RayHit {
  BodyId body{};
  Vec3 position{};      // world space
  Vec3 normal{};        // the surface normal at `position`, pointing out of the hit body
  f32 fraction = 0.0f;  // along the ray's direction vector, so position = origin + dir*fraction
};

struct ShapeHit {
  BodyId body{};
  Vec3 position{};
  Vec3 normal{};        // points from the hit body towards the cast shape
  f32 fraction = 0.0f;  // along `sweep`
  f32 penetration = 0.0f;
};

// --- contacts -------------------------------------------------------------------------------

enum class ContactPhase : u8 { Begin, Persist, End };

// One contact event from the last step. `a` and `b` are ordered so that a < b, which makes
// "one event per pair" a property of the buffer rather than of the caller's bookkeeping.
// Position and normal are zero for an End event: by the time a pair separates there is no
// contact left to describe.
struct ContactEvent {
  BodyId a{};
  BodyId b{};
  u64 user_data_a = 0;
  u64 user_data_b = 0;
  Vec3 position{};
  Vec3 normal{};  // points from b towards a
  f32 penetration = 0.0f;
  ContactPhase phase = ContactPhase::Begin;
};

// --- the world ------------------------------------------------------------------------------

class World {
 public:
  World() noexcept;
  ~World();
  ENGINE_NON_COPYABLE(World);

  // Allocates everything the world will ever use. Failure leaves the world unusable and says
  // why; the only recoverable cause is an option that makes no sense.
  Status init(const WorldOptions& options);
  void shutdown() noexcept;
  bool initialized() const noexcept { return impl_ != nullptr; }
  const WorldOptions& options() const noexcept;

  // --- shapes. A shape is immutable and shared; bodies reference it. Destroying a shape that
  // bodies still use is refused (Status::LimitReached would be a lie, so it is InvalidArgument).
  Status create_box(Vec3 half_extent, ShapeId& out);
  Status create_sphere(f32 radius, ShapeId& out);
  Status create_capsule(f32 half_height, f32 radius, ShapeId& out);
  Status create_convex_hull(std::span<const Vec3> points, ShapeId& out);
  // The static collision form of a cluster mesh: positions plus triangle indices, no LOD.
  Status create_mesh(std::span<const Vec3> positions, std::span<const u32> indices, ShapeId& out);
  Status create_heightfield(const HeightfieldDesc& desc, ShapeId& out);
  Status create_compound(std::span<const CompoundChild> children, ShapeId& out);
  bool destroy_shape(ShapeId shape);
  bool shape_bounds(ShapeId shape, Aabb3& out) const;
  u32 shape_count() const noexcept;

  // --- bodies
  Status create_body(const BodyDesc& desc, BodyId& out);
  bool destroy_body(BodyId body);
  bool contains(BodyId body) const noexcept;
  u32 body_count() const noexcept;

  bool body_transform(BodyId body, Transform3& out) const;
  bool set_body_transform(BodyId body, const Transform3& transform, bool activate = true);
  bool body_linear_velocity(BodyId body, Vec3& out) const;
  bool body_angular_velocity(BodyId body, Vec3& out) const;
  bool set_body_velocities(BodyId body, Vec3 linear, Vec3 angular);
  bool add_impulse(BodyId body, Vec3 impulse);
  // Sets the velocity that reaches `target` in one step of `dt_seconds`, which is how a
  // kinematic body pushes dynamic ones instead of teleporting through them.
  bool move_kinematic(BodyId body, const Transform3& target, f32 dt_seconds);
  bool body_layer(BodyId body, Layer& out) const;
  bool body_user_data(BodyId body, u64& out) const;

  bool body_active(BodyId body) const;
  u32 active_body_count() const noexcept;
  bool activate_body(BodyId body);
  bool deactivate_body(BodyId body);

  // The renderer's read path: one lock of the body table, `ids.size()` reads, no per-body
  // call. `out` must be at least as long as `ids`; a stale id leaves its slot at identity.
  void read_transforms(std::span<const BodyId> ids, std::span<Transform3> out) const;

  // --- debris (plan 05 §5.11). A dynamic body from a dedicated pool with a hard cap: past the
  // cap the oldest piece is destroyed to make room, so destruction cannot grow without bound.
  // Debris is in Layer::Debris, so pieces never collide with each other.
  Status spawn_debris(ShapeId shape, const Transform3& transform, Vec3 linear_velocity,
                      BodyId& out);
  u32 debris_count() const noexcept;
  u32 debris_cap() const noexcept;
  void clear_debris();

  // --- soft bodies (ADR-0026)
  Status create_soft_body(const SoftBodyDesc& desc, SoftBodyId& out);
  bool destroy_soft_body(SoftBodyId body);
  bool contains(SoftBodyId body) const noexcept;
  u32 soft_body_count() const noexcept;
  u32 soft_body_vertex_count(SoftBodyId body) const;
  // Writes world-space particle positions; returns how many were written (0 for a stale id).
  u32 read_soft_body_vertices(SoftBodyId body, std::span<Vec3> out) const;
  bool soft_body_bounds(SoftBodyId body, Aabb3& out) const;
  bool soft_body_active(SoftBodyId body) const;

  // --- queries
  bool cast_ray(Vec3 origin, Vec3 direction, RayHit& out, LayerMask mask = LayerMask::all()) const;
  bool cast_shape(ShapeId shape, const Transform3& start, Vec3 sweep, ShapeHit& out,
                  LayerMask mask = LayerMask::all()) const;

  // --- stepping
  // Advances the world by one fixed step. `dt_seconds` is the step length (use
  // `step_seconds()`), `sub_steps` the collision sub-steps within it; 1 is right at 60 Hz for
  // everything that is not very fast. Contact events from the previous step are dropped here.
  Status step(f32 dt_seconds, u32 sub_steps = 1);
  Status step() { return step(step_seconds(), 1); }
  f32 step_seconds() const noexcept;
  SimTick tick() const noexcept;

  std::span<const ContactEvent> contact_events() const noexcept;
  void clear_contact_events() noexcept;

  WorldStats stats() const noexcept;

  // Rebuilds the broadphase after a large batch of additions. Not needed in steady state.
  void optimize_broad_phase();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace engine::physics

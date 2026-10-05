// The world: creation, bodies, the step, contacts, debris, and queries.

#include "world_impl.h"

#include <core/base/assert.h>
#include <core/jobs/job_system.h>

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine::physics {

// --- contact recorder ------------------------------------------------------------------------

void ContactRecorder::reset(u32 bucket_count) {
  buckets_.clear();
  buckets_.resize(bucket_count == 0 ? 1 : bucket_count);
}

void ContactRecorder::clear() noexcept {
  for (Vector<RawContact>& b : buckets_)
    b.clear();
}

void ContactRecorder::collect(Vector<RawContact>& out) const {
  for (const Vector<RawContact>& b : buckets_)
    out.append(std::span<const RawContact>(b));
}

Vector<RawContact>& ContactRecorder::bucket() noexcept {
  const u32 fallback = buckets_.size() - 1;
  const jobs::WorkerInfo* info = jobs::JobSystem::current_worker();
  if (info != nullptr && info->pool == jobs::Pool::Performance && info->index < fallback)
    return buckets_[info->index];
  return buckets_[fallback];
}

void ContactRecorder::record(const JPH::Body& body1, const JPH::Body& body2,
                             const JPH::ContactManifold& manifold, ContactPhase phase) {
  RawContact contact;
  contact.jph_a = body1.GetID().GetIndex();
  contact.jph_b = body2.GetID().GetIndex();
  contact.sub_a = manifold.mSubShapeID1.GetValue();
  contact.sub_b = manifold.mSubShapeID2.GetValue();
  // Jolt's normal points from shape 1 to shape 2; the event's points from b towards a, and
  // (a, b) is (body1, body2) here, so it is the other way round.
  contact.normal = from_jph(-manifold.mWorldSpaceNormal);
  contact.penetration = manifold.mPenetrationDepth;
  // The manifold's points are floats relative to its base offset, which is near the bodies; the
  // sum is formed in double, so a contact 10,000 km out is where it is (ADR-0053).
  contact.position =
      manifold.mRelativeContactPointsOn1.empty()
          ? from_jph_world(manifold.mBaseOffset)
          : from_jph_world(manifold.mBaseOffset + manifold.mRelativeContactPointsOn1[0]);
  contact.phase = phase;
  bucket().push_back(contact);
}

void ContactRecorder::OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
                                     const JPH::ContactManifold& manifold,
                                     JPH::ContactSettings& settings) {
  (void)settings;
  record(body1, body2, manifold, ContactPhase::Begin);
}

void ContactRecorder::OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2,
                                         const JPH::ContactManifold& manifold,
                                         JPH::ContactSettings& settings) {
  (void)settings;
  record(body1, body2, manifold, ContactPhase::Persist);
}

void ContactRecorder::OnContactRemoved(const JPH::SubShapeIDPair& pair) {
  RawContact contact;
  contact.jph_a = pair.GetBody1ID().GetIndex();
  contact.jph_b = pair.GetBody2ID().GetIndex();
  contact.sub_a = pair.GetSubShapeID1().GetValue();
  contact.sub_b = pair.GetSubShapeID2().GetValue();
  contact.phase = ContactPhase::End;
  bucket().push_back(contact);
}

// --- world -------------------------------------------------------------------------------------

namespace {

JPH::EMotionType to_jph_motion(MotionType motion) noexcept {
  switch (motion) {
    case MotionType::Static: return JPH::EMotionType::Static;
    case MotionType::Kinematic: return JPH::EMotionType::Kinematic;
    case MotionType::Dynamic: break;
  }
  return JPH::EMotionType::Dynamic;
}

u32 round_up_pow2(u32 v) noexcept {
  u32 r = 1;
  while (r < v)
    r <<= 1;
  return r;
}

}  // namespace

u32 temp_allocator_size_for(const WorldOptions& options) noexcept {
  // Measured against Jolt 5.6 in Debug, where the step's largest single allocation is one
  // ContactConstraint (480 bytes) per configured contact constraint; the island builder and
  // the large-island splitter add the rest. The multipliers carry headroom because being a
  // little over costs address space and being under costs a malloc in the middle of a step.
  const u64 bytes = 640ull * options.max_contact_constraints + 64ull * options.max_body_pairs +
                    256ull * options.max_bodies;
  const u64 floor = 4ull << 20;
  const u64 ceiling = 512ull << 20;
  const u64 clamped = bytes < floor ? floor : (bytes > ceiling ? ceiling : bytes);
  return static_cast<u32>(clamped);
}

World::World() noexcept = default;

World::~World() { shutdown(); }

Status World::init(const WorldOptions& world_options) {
  if (impl_ != nullptr) return Status::InvalidArgument;
  if (world_options.max_bodies == 0 || world_options.step_hz == 0) return Status::InvalidArgument;
  if (world_options.max_body_pairs == 0 || world_options.max_contact_constraints == 0)
    return Status::InvalidArgument;

  backend_acquire();
  Impl* impl = new Impl();
  impl->options = world_options;
  impl->step_seconds = 1.0f / static_cast<f32>(world_options.step_hz);

  impl->system.Init(world_options.max_bodies, world_options.body_mutex_count,
                    world_options.max_body_pairs, world_options.max_contact_constraints,
                    impl->broad_phase_layers, impl->object_vs_broad_phase, impl->layer_pairs);
  impl->system.SetGravity(to_jph(world_options.gravity));

  JPH::PhysicsSettings settings = impl->system.GetPhysicsSettings();
  settings.mDeterministicSimulation = world_options.deterministic;
  impl->system.SetPhysicsSettings(settings);

  impl->system.SetContactListener(&impl->contacts);

  u32 concurrency = world_options.worker_count;
  u32 buckets = 1;
  // A pool with no performance workers is the same thing as no pool: nothing would ever run a
  // queued job, so the step would have to execute all of them on the stepping thread anyway
  // and then wait forever for the queue to drain. Saying so here beats hanging there.
  jobs::JobSystem* job_system = world_options.job_system;
  if (job_system != nullptr && job_system->worker_count(jobs::Pool::Performance) == 0)
    job_system = nullptr;
  if (job_system != nullptr) {
    const u32 pool_workers = job_system->worker_count(jobs::Pool::Performance);
    if (concurrency == 0) concurrency = pool_workers;
    buckets = pool_workers + 1;  // + the stepping thread, which has no worker info
  } else {
    concurrency = 1;
  }
  impl->contacts.reset(buckets);

  // The malloc-fallback arena, not the plain one: a step that outgrows its scratch should cost
  // an allocation, not abort the process.
  const u32 arena_bytes = world_options.temp_allocator_bytes != 0
                              ? world_options.temp_allocator_bytes
                              : temp_allocator_size_for(world_options);
  impl->options.temp_allocator_bytes = arena_bytes;
  impl->temp_allocator = new JPH::TempAllocatorImplWithMallocFallback(arena_bytes);
  // The backend's own worst-case job count unless the caller asked for a different one. It is
  // a fixed pool and it must stay a function of the backend and the caller's option alone:
  // sizing it for "however many jobs happen to be in flight" is exactly the bug that made a
  // slow machine run out of them while a fast one never did (see the drain in `step`).
  const u32 configured_jobs = world_options.max_backend_jobs != 0
                                  ? world_options.max_backend_jobs
                                  : static_cast<u32>(JPH::cMaxPhysicsJobs);
  impl->options.max_backend_jobs = configured_jobs;
  impl->job_adapter = new JoltJobAdapter(job_system, concurrency, round_up_pow2(configured_jobs),
                                         static_cast<u32>(JPH::cMaxPhysicsBarriers));

  impl->body_of_jph.resize(world_options.max_bodies);
  impl->user_data_of_jph.resize(world_options.max_bodies, 0);
  impl->debris_ring.resize(world_options.debris_cap);

  impl_ = impl;
  return Status::Ok;
}

void World::shutdown() noexcept {
  if (impl_ == nullptr) return;
  Impl* impl = impl_;
  impl_ = nullptr;

  JPH::BodyInterface& bodies = impl->system.GetBodyInterface();
  for (const Impl::BodyEntry& entry : impl->bodies.values()) {
    bodies.RemoveBody(entry.id);
    bodies.DestroyBody(entry.id);
  }
  for (const Impl::SoftEntry& entry : impl->soft_bodies.values()) {
    bodies.RemoveBody(entry.id);
    bodies.DestroyBody(entry.id);
  }
  delete impl->job_adapter;
  delete impl->temp_allocator;
  delete impl;
  backend_release();
}

const WorldOptions& World::options() const noexcept {
  static const WorldOptions k_empty;
  return impl_ != nullptr ? impl_->options : k_empty;
}

f32 World::step_seconds() const noexcept {
  return impl_ != nullptr ? impl_->step_seconds : 1.0f / 60.0f;
}

SimTick World::tick() const noexcept { return impl_ != nullptr ? impl_->tick : SimTick{}; }

u32 World::shape_count() const noexcept { return impl_ != nullptr ? impl_->shapes.size() : 0; }
u32 World::body_count() const noexcept { return impl_ != nullptr ? impl_->bodies.size() : 0; }
u32 World::soft_body_count() const noexcept {
  return impl_ != nullptr ? impl_->soft_bodies.size() : 0;
}
u32 World::debris_cap() const noexcept { return impl_ != nullptr ? impl_->options.debris_cap : 0; }

u32 World::active_body_count() const noexcept {
  if (impl_ == nullptr) return 0;
  return impl_->system.GetNumActiveBodies(JPH::EBodyType::RigidBody);
}

WorldStats World::stats() const noexcept {
  WorldStats out;
  if (impl_ == nullptr) return out;
  out.steps = impl_->steps;
  out.backend_jobs = impl_->job_adapter->queued_count();
  out.backend_jobs_on_workers = impl_->job_adapter->worker_count();
  out.backend_jobs_pending = impl_->job_adapter->pending_count();
  out.soft_body_solve_jobs = impl_->job_adapter->soft_body_count();
  out.soft_body_solve_workers = impl_->job_adapter->soft_body_worker_count();
  out.soft_body_budget = impl_->soft_body_budget;
  out.body_count = impl_->bodies.size();
  out.active_body_count = active_body_count();
  out.soft_body_count = impl_->soft_bodies.size();
  out.debris_count = debris_count();
  out.shape_count = impl_->shapes.size();
  return out;
}

void World::optimize_broad_phase() {
  if (impl_ != nullptr) impl_->system.OptimizeBroadPhase();
}

// ADR-0029 decision 1. The measured number is one wall-clock figure for the whole soft-body
// phase, because that is what the backend's arrangement produces: its solve jobs take the next
// available constraint group from *any* active cage, which is exactly why eight cages cost less
// together than one costs alone (E19), and the same interleaving is why there is no per-cage
// wall clock to be had. The split between the hero and the ambient set is therefore by work —
// elements times iterations, E19's cost model, which held to within 5% across its whole grid —
// and the total is measured. Saying which half of that sentence applies to which number is the
// honest way to report it, and it is why the two are named separately rather than summed.
//
// A sleeping cage is left out of the weights: the backend does no work for it, so giving it a
// share of the phase would make a settled scene look busy.
void World::Impl::refresh_soft_body_budget(i64 soft_body_ns) noexcept {
  // The clamp filled these in on its way past and this runs afterwards, so they survive the reset.
  const u32 sweeps = soft_body_budget.strain_clamp_sweeps;
  const bool saturated = soft_body_budget.strain_clamp_saturated;
  soft_body_budget = SoftBodyBudget{};
  soft_body_budget.strain_clamp_sweeps = sweeps;
  soft_body_budget.strain_clamp_saturated = saturated;
  if (soft_bodies.size() == 0) return;

  f64 ambient_work = 0.0;
  f64 hero_work = 0.0;
  const JPH::BodyInterface& body_interface = system.GetBodyInterfaceNoLock();
  for (const SoftEntry& entry : soft_bodies.values()) {
    if (entry.hero) ++soft_body_budget.hero_count;
    if (!body_interface.IsActive(entry.id)) continue;
    const f64 work = static_cast<f64>(entry.vertex_count) * static_cast<f64>(entry.iterations);
    if (entry.hero) {
      hero_work += work;
    } else {
      ambient_work += work;
    }
  }

  const f64 total_ms = static_cast<f64>(soft_body_ns) / 1.0e6;
  const f64 total_work = ambient_work + hero_work;
  if (total_work > 0.0) {
    soft_body_budget.hero_ms = static_cast<f32>(total_ms * hero_work / total_work);
    soft_body_budget.ambient_ms = static_cast<f32>(total_ms * ambient_work / total_work);
  } else {
    // Every cage asleep: whatever the phase cost was, it was not deformable work.
    soft_body_budget.ambient_ms = 0.0f;
  }
  if (soft_body_budget.ambient_ms > k_deformable_ambient_budget_ms) {
    soft_body_budget.ambient_over_ms = soft_body_budget.ambient_ms - k_deformable_ambient_budget_ms;
  }
  if (soft_body_budget.hero_ms > k_deformable_hero_budget_ms)
    soft_body_budget.hero_over_ms = soft_body_budget.hero_ms - k_deformable_hero_budget_ms;
  soft_body_budget.over_budget =
      soft_body_budget.ambient_over_ms > 0.0f || soft_body_budget.hero_over_ms > 0.0f;
}

// --- bodies --------------------------------------------------------------------------------

Status World::create_body(const BodyDesc& desc, BodyId& out) {
  if (impl_ == nullptr) return Status::InvalidArgument;
  const JPH::Shape* shape = impl_->shape_ptr(desc.shape);
  if (shape == nullptr) return Status::NotFound;
  if (desc.layer >= Layer::Count) return Status::InvalidArgument;
  // A mesh or heightfield has no inertia tensor and no inside, so Jolt only moves it as a
  // static body. Saying so here beats an assert inside the backend.
  if (shape->MustBeStatic() && desc.motion != MotionType::Static) return Status::Unsupported;
  if (impl_->bodies.size() >= impl_->options.max_bodies) return Status::LimitReached;

  JPH::BodyCreationSettings settings(shape, to_jph(desc.transform.position),
                                     to_jph(desc.transform.rotation), to_jph_motion(desc.motion),
                                     to_object_layer(desc.layer));
  if (desc.motion != MotionType::Static) {
    settings.mLinearVelocity = to_jph(desc.linear_velocity);
    settings.mAngularVelocity = to_jph(desc.angular_velocity);
  }
  settings.mFriction = desc.friction;
  settings.mRestitution = desc.restitution;
  settings.mLinearDamping = desc.linear_damping;
  settings.mAngularDamping = desc.angular_damping;
  settings.mGravityFactor = desc.gravity_factor;
  settings.mAllowSleeping = desc.allow_sleeping;
  settings.mUserData = desc.user_data;
  if (desc.mass > 0.0f) {
    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    settings.mMassPropertiesOverride.mMass = desc.mass;
  }

  const bool activate = desc.start_active && desc.motion != MotionType::Static;
  const JPH::BodyID id = impl_->system.GetBodyInterface().CreateAndAddBody(
      settings, activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
  if (id.IsInvalid()) return Status::LimitReached;

  Impl::BodyEntry entry;
  entry.id = id;
  entry.shape = desc.shape;
  entry.user_data = desc.user_data;
  entry.layer = desc.layer;
  const BodyId handle{impl_->bodies.insert(entry)};
  impl_->body_of_jph[id.GetIndex()] = handle;
  impl_->user_data_of_jph[id.GetIndex()] = desc.user_data;
  ++impl_->shapes.get(desc.shape.handle)->body_refs;
  out = handle;
  return Status::Ok;
}

bool World::destroy_body(BodyId body) {
  if (impl_ == nullptr) return false;
  Impl::BodyEntry* entry = impl_->body_entry(body);
  if (entry == nullptr) return false;
  JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
  bodies.RemoveBody(entry->id);
  bodies.DestroyBody(entry->id);
  impl_->body_of_jph[entry->id.GetIndex()] = BodyId{};
  impl_->user_data_of_jph[entry->id.GetIndex()] = 0;
  Impl::ShapeEntry* shape = impl_->shapes.get(entry->shape.handle);
  if (shape != nullptr && shape->body_refs > 0) --shape->body_refs;
  impl_->bodies.erase(body.handle);
  return true;
}

bool World::contains(BodyId body) const noexcept {
  return impl_ != nullptr && impl_->bodies.contains(body.handle);
}

bool World::body_transform(BodyId body, BodyTransform& out) const {
  if (impl_ == nullptr) return false;
  const Impl::BodyEntry* entry = impl_->body_entry(body);
  if (entry == nullptr) return false;
  const JPH::BodyInterface& bodies = impl_->system.GetBodyInterfaceNoLock();
  out.position = from_jph_world(bodies.GetPosition(entry->id));
  out.rotation = from_jph(bodies.GetRotation(entry->id));
  return true;
}

bool World::set_body_transform(BodyId body, const BodyTransform& transform, bool activate) {
  if (impl_ == nullptr) return false;
  const Impl::BodyEntry* entry = impl_->body_entry(body);
  if (entry == nullptr) return false;
  impl_->system.GetBodyInterface().SetPositionAndRotation(
      entry->id, to_jph(transform.position), to_jph(transform.rotation),
      activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
  return true;
}

bool World::body_linear_velocity(BodyId body, Vec3& out) const {
  if (impl_ == nullptr) return false;
  const Impl::BodyEntry* entry = impl_->body_entry(body);
  if (entry == nullptr) return false;
  out = from_jph(impl_->system.GetBodyInterfaceNoLock().GetLinearVelocity(entry->id));
  return true;
}

bool World::body_angular_velocity(BodyId body, Vec3& out) const {
  if (impl_ == nullptr) return false;
  const Impl::BodyEntry* entry = impl_->body_entry(body);
  if (entry == nullptr) return false;
  out = from_jph(impl_->system.GetBodyInterfaceNoLock().GetAngularVelocity(entry->id));
  return true;
}

bool World::set_body_velocities(BodyId body, Vec3 linear, Vec3 angular) {
  if (impl_ == nullptr) return false;
  const Impl::BodyEntry* entry = impl_->body_entry(body);
  if (entry == nullptr) return false;
  impl_->system.GetBodyInterface().SetLinearAndAngularVelocity(entry->id, to_jph(linear),
                                                               to_jph(angular));
  return true;
}

bool World::add_impulse(BodyId body, Vec3 impulse) {
  if (impl_ == nullptr) return false;
  const Impl::BodyEntry* entry = impl_->body_entry(body);
  if (entry == nullptr) return false;
  impl_->system.GetBodyInterface().AddImpulse(entry->id, to_jph(impulse));
  return true;
}

bool World::move_kinematic(BodyId body, const BodyTransform& target, f32 dt_seconds) {
  if (impl_ == nullptr || dt_seconds <= 0.0f) return false;
  const Impl::BodyEntry* entry = impl_->body_entry(body);
  if (entry == nullptr) return false;
  impl_->system.GetBodyInterface().MoveKinematic(entry->id, to_jph(target.position),
                                                 to_jph(target.rotation), dt_seconds);
  return true;
}

bool World::body_layer(BodyId body, Layer& out) const {
  if (impl_ == nullptr) return false;
  const Impl::BodyEntry* entry = impl_->body_entry(body);
  if (entry == nullptr) return false;
  out = entry->layer;
  return true;
}

bool World::body_user_data(BodyId body, u64& out) const {
  if (impl_ == nullptr) return false;
  const Impl::BodyEntry* entry = impl_->body_entry(body);
  if (entry == nullptr) return false;
  out = entry->user_data;
  return true;
}

bool World::body_active(BodyId body) const {
  if (impl_ == nullptr) return false;
  const Impl::BodyEntry* entry = impl_->body_entry(body);
  if (entry == nullptr) return false;
  return impl_->system.GetBodyInterfaceNoLock().IsActive(entry->id);
}

bool World::activate_body(BodyId body) {
  if (impl_ == nullptr) return false;
  const Impl::BodyEntry* entry = impl_->body_entry(body);
  if (entry == nullptr) return false;
  impl_->system.GetBodyInterface().ActivateBody(entry->id);
  return true;
}

bool World::deactivate_body(BodyId body) {
  if (impl_ == nullptr) return false;
  const Impl::BodyEntry* entry = impl_->body_entry(body);
  if (entry == nullptr) return false;
  impl_->system.GetBodyInterface().DeactivateBody(entry->id);
  return true;
}

void World::read_transforms(std::span<const BodyId> ids, std::span<BodyTransform> out) const {
  if (impl_ == nullptr) return;
  ENGINE_ASSERT(out.size() >= ids.size(), "physics: read_transforms output is too short");
  const JPH::BodyInterface& bodies = impl_->system.GetBodyInterfaceNoLock();
  const usize count = ids.size() < out.size() ? ids.size() : out.size();
  for (usize i = 0; i < count; ++i) {
    const Impl::BodyEntry* entry = impl_->body_entry(ids[i]);
    if (entry == nullptr) {
      out[i] = BodyTransform::identity();
      continue;
    }
    out[i].position = from_jph_world(bodies.GetPosition(entry->id));
    out[i].rotation = from_jph(bodies.GetRotation(entry->id));
  }
}

// --- debris --------------------------------------------------------------------------------

u32 World::debris_count() const noexcept {
  if (impl_ == nullptr) return 0;
  u32 live = 0;
  for (u32 i = 0; i < impl_->debris_size; ++i) {
    const u32 slot = (impl_->debris_head + i) % impl_->debris_ring.size();
    if (impl_->bodies.contains(impl_->debris_ring[slot].handle)) ++live;
  }
  return live;
}

void World::clear_debris() {
  if (impl_ == nullptr) return;
  for (u32 i = 0; i < impl_->debris_size; ++i) {
    const u32 slot = (impl_->debris_head + i) % impl_->debris_ring.size();
    destroy_body(impl_->debris_ring[slot]);
    impl_->debris_ring[slot] = BodyId{};
  }
  impl_->debris_head = 0;
  impl_->debris_size = 0;
}

Status World::spawn_debris(ShapeId shape, const BodyTransform& transform, Vec3 linear_velocity,
                           BodyId& out) {
  if (impl_ == nullptr) return Status::InvalidArgument;
  const u32 cap = impl_->debris_ring.size();
  if (cap == 0) return Status::LimitReached;

  // Oldest first: the ring is in spawn order, so recycling is one destroy at the head.
  if (impl_->debris_size == cap) {
    destroy_body(impl_->debris_ring[impl_->debris_head]);
    impl_->debris_ring[impl_->debris_head] = BodyId{};
    impl_->debris_head = (impl_->debris_head + 1) % cap;
    --impl_->debris_size;
  }

  BodyDesc desc;
  desc.shape = shape;
  desc.transform = transform;
  desc.linear_velocity = linear_velocity;
  desc.motion = MotionType::Dynamic;
  desc.layer = Layer::Debris;
  const Status status = create_body(desc, out);
  if (status != Status::Ok) return status;

  Impl::BodyEntry* entry = impl_->body_entry(out);
  entry->debris = true;
  const u32 slot = (impl_->debris_head + impl_->debris_size) % cap;
  impl_->debris_ring[slot] = out;
  ++impl_->debris_size;
  return Status::Ok;
}

// --- queries -------------------------------------------------------------------------------

bool World::cast_ray(WorldPos origin, Vec3 direction, RayHit& out, LayerMask mask) const {
  if (impl_ == nullptr) return false;
  const JPH::RRayCast ray(to_jph(origin), to_jph(direction));
  JPH::RayCastResult result;
  const MaskBroadPhaseLayerFilter bp_filter(mask);
  const MaskObjectLayerFilter object_filter(mask);
  if (!impl_->system.GetNarrowPhaseQuery().CastRay(ray, result, bp_filter, object_filter))
    return false;

  // origin + direction * fraction in double: the point is where the ray met the surface wherever
  // in the world that is.
  const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
  const JPH::BodyLockRead lock(impl_->system.GetBodyLockInterfaceNoLock(), result.mBodyID);
  if (!lock.Succeeded()) return false;
  out.body = impl_->handle_of(result.mBodyID);
  out.position = from_jph_world(point);
  out.normal = from_jph(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
  out.fraction = result.mFraction;
  return true;
}

bool World::cast_shape(ShapeId shape, const BodyTransform& start, Vec3 sweep, ShapeHit& out,
                       LayerMask mask) const {
  if (impl_ == nullptr) return false;
  const JPH::Shape* jph_shape = impl_->shape_ptr(shape);
  if (jph_shape == nullptr) return false;

  const JPH::RVec3 base = to_jph(start.position);
  const JPH::RMat44 from = JPH::RMat44::sRotationTranslation(to_jph(start.rotation), base);
  const JPH::RShapeCast cast =
      JPH::RShapeCast::sFromWorldTransform(jph_shape, JPH::Vec3::sOne(), from, to_jph(sweep));
  JPH::ShapeCastSettings settings;
  settings.mUseShrunkenShapeAndConvexRadius = true;
  settings.mReturnDeepestPoint = true;
  JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
  const MaskBroadPhaseLayerFilter bp_filter(mask);
  const MaskObjectLayerFilter object_filter(mask);
  // The hit comes back in floats relative to a base the caller of the backend chooses; the cast's
  // own start is the one that keeps them small (the backend's own advice for far from the origin),
  // and the base is added back in double.
  impl_->system.GetNarrowPhaseQuery().CastShape(cast, settings, base, collector, bp_filter,
                                                object_filter);
  if (!collector.HadHit()) return false;

  out.body = impl_->handle_of(collector.mHit.mBodyID2);
  out.position = from_jph_world(base + collector.mHit.mContactPointOn2);
  // mPenetrationAxis moves the hit body out of the cast shape, so the outward surface normal
  // is the other way.
  const JPH::Vec3 axis = collector.mHit.mPenetrationAxis;
  out.normal = axis.IsNearZero() ? Vec3::zero() : from_jph(-axis.Normalized());
  out.fraction = collector.mHit.mFraction;
  out.penetration = collector.mHit.mPenetrationDepth;
  return true;
}

// --- the step ------------------------------------------------------------------------------

std::span<const ContactEvent> World::contact_events() const noexcept {
  if (impl_ == nullptr) return {};
  return std::span<const ContactEvent>(impl_->events);
}

void World::clear_contact_events() noexcept {
  if (impl_ != nullptr) impl_->events.clear();
}

Status World::step(f32 dt_seconds, u32 sub_steps) {
  if (impl_ == nullptr) return Status::InvalidArgument;
  if (!(dt_seconds > 0.0f) || sub_steps == 0) return Status::InvalidArgument;

  impl_->events.clear();
  impl_->raw_contacts.clear();
  impl_->contacts.clear();

  // ADR-0029's budget is per step, so the window accumulator starts empty every step. The three
  // things that go into it are this pre-pass, the backend's own soft-body phase (measured by the
  // job adapter), and the strain clamp at the end.
  impl_->job_adapter->reset_soft_body_ns();
  const bool has_soft_bodies = impl_->soft_bodies.size() != 0;
  const i64 pre_pass_start = has_soft_bodies ? time::monotonic_ns() : 0;

  // Drive every attached particle from the body it hangs on. Both kinds write a velocity and
  // nothing else, which is what lets "bound to a rigid body" of ADR-0026 work without a second
  // backend constraint kind:
  //
  //   Rigid   zero inverse mass, so the solver integrates the particle from its velocity
  //           alone; setting that velocity to cover the whole gap is the particle form of
  //           MoveKinematic.
  //   Spring  the particle keeps its mass, and its velocity is steered a fraction `alpha` of
  //           the way towards the gap-covering velocity. At alpha = 1 that is the Rigid
  //           formula with mass kept; below 1 it is a critically damped position servo, which
  //           cannot ring the way an accumulating spring force would because the velocity is
  //           rewritten each step rather than added to.
  const f32 inv_dt = 1.0f / dt_seconds;
  const JPH::BodyInterface& anchors = impl_->system.GetBodyInterfaceNoLock();
  for (Impl::SoftEntry& entry : impl_->soft_bodies.values()) {
    if (entry.attachments.empty()) continue;
    JPH::BodyLockWrite lock(impl_->system.GetBodyLockInterfaceNoLock(), entry.id);
    if (!lock.Succeeded()) continue;
    JPH::Body& soft = lock.GetBody();
    auto* motion = static_cast<JPH::SoftBodyMotionProperties*>(soft.GetMotionProperties());
    const JPH::RMat44 to_local = soft.GetCenterOfMassTransform().InversedRotationTranslation();
    for (const SoftAttachment& attachment : entry.attachments) {
      if (attachment.vertex >= motion->GetVertices().size()) continue;
      // The anchor in the world, in double: the anchoring body's frame, or for a null body the
      // frame the soft body was placed in (soft_body.h). Brought into the particle's frame in
      // double too, and only the result — a gap of centimetres — is a float.
      const JPH::Vec3 local = to_jph(attachment.local_point);
      JPH::RVec3 target = entry.placement * local;
      if (!attachment.body.is_null()) {
        const Impl::BodyEntry* anchor = impl_->body_entry(attachment.body);
        if (anchor == nullptr) continue;
        target = anchors.GetWorldTransform(anchor->id) * local;
      }
      JPH::SoftBodyVertex& vertex = motion->GetVertex(attachment.vertex);
      const JPH::Vec3 gap_velocity = (JPH::Vec3(to_local * target) - vertex.mPosition) * inv_dt;
      if (attachment.kind == AttachmentKind::Rigid) {
        vertex.mInvMass = 0.0f;
        vertex.mVelocity = gap_velocity;
      } else {
        const f32 alpha = clamp(attachment.follow_rate * dt_seconds, 0.0f, 1.0f);
        vertex.mVelocity = vertex.mVelocity + (gap_velocity - vertex.mVelocity) * alpha;
      }
    }
  }

  if (has_soft_bodies) impl_->job_adapter->add_soft_body_ns(time::monotonic_ns() - pre_pass_start);

  const JPH::EPhysicsUpdateError error = impl_->system.Update(
      dt_seconds, static_cast<int>(sub_steps), impl_->temp_allocator, impl_->job_adapter);

  // The last sub-step's soft-body phase has no next-step job to close its window, so the end of
  // the update closes it. Every earlier sub-step closed itself when the backend queued the job
  // that starts the next one (src/job_adapter.h).
  impl_->job_adapter->close_soft_body_window();

  // Update returns when the backend's barrier is satisfied, which is not the same as "the job
  // system has nothing of ours left". A job the barrier executed on this thread is still
  // sitting in a pool queue holding the last reference to its Job object, and the Job objects
  // come from a fixed-size free list. Leaving those entries for the pool to pick up whenever
  // it gets round to it makes the number of live jobs a function of how fast the workers drain
  // rather than of the step, which is why a four-core machine exhausted the list in a fraction
  // of a second while a thirty-six-thread one never did. Draining here bounds it to one step.
  impl_->job_adapter->drain();

  // The authored strain limit, enforced (ADR-0029 decision 4, plan 07 §7.10 `limits.max_strain`).
  // Here rather than inside the solve because a limit is a property of the pose the step leaves
  // behind, not a constraint the solver negotiates; the reasoning in full is in soft_body.cpp.
  if (has_soft_bodies) {
    impl_->soft_body_budget.strain_clamp_sweeps = 0;
    impl_->soft_body_budget.strain_clamp_saturated = false;
    const i64 clamp_start = time::monotonic_ns();
    impl_->clamp_soft_body_strain(dt_seconds);
    impl_->job_adapter->add_soft_body_ns(time::monotonic_ns() - clamp_start);
  }
  impl_->refresh_soft_body_budget(impl_->job_adapter->soft_body_ns());

  ++impl_->tick;
  ++impl_->steps;

  // Turn the worker buckets into one ordered, deduplicated list. Sorting by the backend's own
  // body and sub-shape indices is what makes the result independent of which worker solved
  // which island; keeping the first of each (pair, phase) run is what makes "begin is reported
  // once per pair" a property of this buffer rather than of the caller.
  impl_->contacts.collect(impl_->raw_contacts);
  std::sort(impl_->raw_contacts.begin(), impl_->raw_contacts.end(),
            [](const RawContact& l, const RawContact& r) {
              if (l.jph_a != r.jph_a) return l.jph_a < r.jph_a;
              if (l.jph_b != r.jph_b) return l.jph_b < r.jph_b;
              if (l.phase != r.phase) return l.phase < r.phase;
              if (l.sub_a != r.sub_a) return l.sub_a < r.sub_a;
              return l.sub_b < r.sub_b;
            });

  const RawContact* previous = nullptr;
  for (const RawContact& raw : impl_->raw_contacts) {
    if (previous != nullptr && previous->jph_a == raw.jph_a && previous->jph_b == raw.jph_b &&
        previous->phase == raw.phase) {
      continue;
    }
    previous = &raw;
    ContactEvent event;
    event.a = impl_->handle_of(JPH::BodyID(raw.jph_a));
    event.b = impl_->handle_of(JPH::BodyID(raw.jph_b));
    // A soft body's contacts are not recorded, and a body destroyed inside the step leaves a
    // null handle behind; either way there is nothing for a caller to act on.
    if (event.a.is_null() || event.b.is_null()) continue;
    event.user_data_a = impl_->user_data_of_jph[raw.jph_a];
    event.user_data_b = impl_->user_data_of_jph[raw.jph_b];
    event.position = raw.position;
    event.normal = raw.normal;
    event.penetration = raw.penetration;
    event.phase = raw.phase;
    if (event.b < event.a) {
      std::swap(event.a, event.b);
      std::swap(event.user_data_a, event.user_data_b);
      event.normal = -event.normal;
    }
    impl_->events.push_back(event);
  }

  if (error != JPH::EPhysicsUpdateError::None) {
    ENGINE_LOG_WARN(log_physics, "backend buffers overflowed during a step",
                    log::field("error", static_cast<u64>(error)),
                    log::field("tick", impl_->tick.value));
    return Status::LimitReached;
  }
  return Status::Ok;
}

}  // namespace engine::physics

#pragma once

// World::Impl: every piece of backend state the world owns. Shared by world.cpp, shapes.cpp
// and soft_body.cpp, and by nothing else.

#include "backend.h"
#include "job_adapter.h"

#include <core/containers/slot_map.h>
#include <core/containers/vector.h>
#include <domain/physics/physics.h>

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/PhysicsSystem.h>

namespace engine::physics {

// A contact as the listener sees it, before it is turned into a public event. The JPH body
// indices and sub-shape ids are what make the buffer sortable into a deterministic order: the
// order the callbacks arrive in depends on which worker got which island.
struct RawContact {
  u32 jph_a = 0;  // JPH::BodyID index of Jolt's body 1
  u32 jph_b = 0;
  u32 sub_a = 0;
  u32 sub_b = 0;
  Vec3 position{};
  Vec3 normal{};  // from Jolt's body 2 towards its body 1
  f32 penetration = 0.0f;
  ContactPhase phase = ContactPhase::Begin;
};

// Jolt calls this from whichever worker is solving the island. Recording into a per-worker
// bucket keeps the callback lock-free; the buckets are merged and sorted on the stepping
// thread, so no game code ever runs on a physics worker (plan 05 §5.11).
class ContactRecorder final : public JPH::ContactListener {
 public:
  void reset(u32 bucket_count);
  void clear() noexcept;
  // Appends every bucket's contents to `out`, in bucket order.
  void collect(Vector<RawContact>& out) const;

  void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
                      const JPH::ContactManifold& manifold,
                      JPH::ContactSettings& settings) override;
  void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2,
                          const JPH::ContactManifold& manifold,
                          JPH::ContactSettings& settings) override;
  void OnContactRemoved(const JPH::SubShapeIDPair& pair) override;

 private:
  Vector<RawContact>& bucket() noexcept;
  void record(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
              ContactPhase phase);

  // One bucket per performance worker plus one for every other thread (the stepping thread
  // helps out inside the barrier wait, where current_worker() is null).
  Vector<Vector<RawContact>> buckets_;
};

struct World::Impl {
  WorldOptions options;

  // Jolt keeps references to these three, so they outlive the system by living beside it.
  LayerInterface broad_phase_layers;
  ObjectVsBroadPhaseFilter object_vs_broad_phase;
  LayerPairFilter layer_pairs;

  JPH::PhysicsSystem system;
  JPH::TempAllocator* temp_allocator = nullptr;
  JoltJobAdapter* job_adapter = nullptr;
  ContactRecorder contacts;

  struct ShapeEntry {
    JPH::RefConst<JPH::Shape> shape;
    u32 body_refs = 0;
  };
  struct BodyEntry {
    JPH::BodyID id;
    ShapeId shape;
    u64 user_data = 0;
    Layer layer = Layer::Moving;
    bool debris = false;
  };
  struct SoftEntry {
    JPH::BodyID id;
    Vector<SoftAttachment> attachments;
    u32 vertex_count = 0;
    // What the budget report needs to split the measured phase between the ambient set and the
    // hero, and what the strain clamp needs to run (ADR-0029). `iterations` is kept because a
    // cage's share of the phase is proportional to its elements times its iterations — E19's
    // cost model, linear to within 5% across the whole grid — and the backend does not hand back
    // per-cage timings to split it any other way.
    u32 iterations = 1;
    f32 max_strain = 0.0f;
    bool hero = false;
  };

  SlotMap<ShapeEntry> shapes;
  SlotMap<BodyEntry> bodies;
  SlotMap<SoftEntry> soft_bodies;

  // JPH::BodyID index -> our handle, for the contact listener and the query results. Sized to
  // max_bodies once, so a lookup is one load.
  Vector<BodyId> body_of_jph;
  Vector<u64> user_data_of_jph;

  // The debris pool: a ring of handles in spawn order, oldest first (plan 05 §5.11).
  Vector<BodyId> debris_ring;
  u32 debris_head = 0;
  u32 debris_size = 0;

  Vector<RawContact> raw_contacts;
  Vector<ContactEvent> events;

  SimTick tick;
  u64 steps = 0;
  f32 step_seconds = 1.0f / 60.0f;
  SoftBodyBudget soft_body_budget;

  // Turns the nanoseconds the last step spent on deformables into ADR-0029's report. Called once
  // per step, after the clamp, so that everything the tick spent on cages is inside the number.
  void refresh_soft_body_budget(i64 soft_body_ns) noexcept;

  // ADR-0029 decision 4: enforce `SoftBodyDesc::max_strain`. Runs on the stepping thread once the
  // backend's update has returned, over every cage that asked for a limit. Defined in
  // soft_body.cpp, beside the rest of the cage code; called from `World::step`.
  void clamp_soft_body_strain(f32 dt_seconds);

  const JPH::Shape* shape_ptr(ShapeId id) const noexcept {
    const ShapeEntry* entry = shapes.get(id.handle);
    return entry != nullptr ? entry->shape.GetPtr() : nullptr;
  }
  const BodyEntry* body_entry(BodyId id) const noexcept { return bodies.get(id.handle); }
  BodyEntry* body_entry(BodyId id) noexcept { return bodies.get(id.handle); }
  BodyId handle_of(JPH::BodyID id) const noexcept {
    const u32 index = id.GetIndex();
    return index < body_of_jph.size() ? body_of_jph[index] : BodyId{};
  }
  // Takes ownership of a freshly built shape and returns its handle. The reference is what
  // keeps the shape alive: the ShapeResult it came out of is a temporary.
  Status adopt_shape(JPH::RefConst<JPH::Shape> shape, ShapeId& out);
};

}  // namespace engine::physics

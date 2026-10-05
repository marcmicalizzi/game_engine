#pragma once

// Everything that knows Jolt exists. This header is under src/, so nothing outside the module
// can include it and no public header ever names a JPH type (ADR-0026 decision 3).
//
// Jolt's headers wrap themselves in warning suppression (JPH_NAMESPACE_BEGIN pushes it), and
// the module marks the include directory SYSTEM on top of that, so the engine's -Werror policy
// applies to our code and not to Jolt's.

#include "jolt.h"

#include <core/base/types.h>
#include <core/log/log.h>
#include <core/math/math.h>
#include <domain/physics/types.h>

#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

namespace engine::physics {

ENGINE_LOG_CATEGORY_DECLARE(log_physics);

// --- process-wide backend state ---------------------------------------------------------
//
// Jolt keeps three pieces of global state: the allocation hooks, the RTTI factory, and the
// collision dispatch table. They are installed once per process and torn down when the last
// world goes away, so a test binary that creates and destroys worlds does not leak the factory
// and a host that never creates a world pays nothing.
void backend_acquire();
void backend_release() noexcept;

// --- conversions --------------------------------------------------------------------------

inline JPH::Vec3 to_jph(Vec3 v) noexcept { return JPH::Vec3(v.x, v.y, v.z); }
inline Vec3 from_jph(JPH::Vec3Arg v) noexcept { return Vec3(v.GetX(), v.GetY(), v.GetZ()); }
// A world position and the backend's: both three doubles (jolt.h refuses a float build), so these
// copy and never round (ADR-0053).
inline JPH::RVec3 to_jph(WorldPos p) noexcept { return JPH::RVec3(p.x, p.y, p.z); }
inline WorldPos from_jph_world(JPH::RVec3Arg p) noexcept {
  return WorldPos(p.GetX(), p.GetY(), p.GetZ());
}
inline JPH::Quat to_jph(Quat q) noexcept { return JPH::Quat(q.x, q.y, q.z, q.w); }
inline Quat from_jph(JPH::QuatArg q) noexcept {
  return Quat(q.GetX(), q.GetY(), q.GetZ(), q.GetW());
}

// --- layers ---------------------------------------------------------------------------------
//
// An engine Layer is its own object layer, one to one, so the filter is exactly the table in
// types.h and there is nothing to keep in sync. Broadphase layers are coarser: each is one
// bounding-volume tree, and the point of having more than one is that the tree holding the
// world's static geometry is not rebuilt because a crate moved. Kinematic and Debris get their
// own trees because they move in bulk and are queried by different sets of layers.
namespace bp_layers {
constexpr JPH::BroadPhaseLayer k_static(0);     // Layer::Static, Layer::Query
constexpr JPH::BroadPhaseLayer k_moving(1);     // Layer::Moving
constexpr JPH::BroadPhaseLayer k_debris(2);     // Layer::Debris
constexpr JPH::BroadPhaseLayer k_kinematic(3);  // Layer::Kinematic
constexpr u32 k_count = 4;
}  // namespace bp_layers

constexpr JPH::ObjectLayer to_object_layer(Layer layer) noexcept {
  return static_cast<JPH::ObjectLayer>(layer);
}
constexpr Layer from_object_layer(JPH::ObjectLayer layer) noexcept {
  return static_cast<Layer>(layer);
}
constexpr JPH::BroadPhaseLayer broad_phase_layer_of(Layer layer) noexcept {
  switch (layer) {
    case Layer::Static:
    case Layer::Query: return bp_layers::k_static;
    case Layer::Moving: return bp_layers::k_moving;
    case Layer::Debris: return bp_layers::k_debris;
    case Layer::Kinematic: return bp_layers::k_kinematic;
    case Layer::Count: break;
  }
  return bp_layers::k_static;
}

class LayerInterface final : public JPH::BroadPhaseLayerInterface {
 public:
  JPH::uint GetNumBroadPhaseLayers() const override { return bp_layers::k_count; }
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
    return broad_phase_layer_of(from_object_layer(layer));
  }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
    switch (static_cast<JPH::BroadPhaseLayer::Type>(layer)) {
      case 0: return "static";
      case 1: return "moving";
      case 2: return "debris";
      default: return "kinematic";
    }
  }
#endif
};

// True when an object layer has to be tested against a broadphase tree at all: the union of
// layers_collide() over the object layers that live in that tree.
constexpr bool object_vs_broad_phase(Layer object, JPH::BroadPhaseLayer::Type tree) noexcept {
  for (u32 i = 0; i < k_layer_count; ++i) {
    const Layer other = static_cast<Layer>(i);
    if (static_cast<JPH::BroadPhaseLayer::Type>(broad_phase_layer_of(other)) != tree) continue;
    if (layers_collide(object, other)) return true;
  }
  return false;
}

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer object, JPH::BroadPhaseLayer tree) const override {
    return object_vs_broad_phase(from_object_layer(object),
                                 static_cast<JPH::BroadPhaseLayer::Type>(tree));
  }
};

class LayerPairFilter final : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
    return layers_collide(from_object_layer(a), from_object_layer(b));
  }
};

// Query-side filters built from a LayerMask. Queries ignore the collision table entirely: a
// raycast against Layer::Query geometry is the whole point of that layer.
class MaskObjectLayerFilter final : public JPH::ObjectLayerFilter {
 public:
  explicit MaskObjectLayerFilter(LayerMask mask) noexcept : mask_(mask) {}
  bool ShouldCollide(JPH::ObjectLayer layer) const override {
    return mask_.test(from_object_layer(layer));
  }

 private:
  LayerMask mask_;
};

class MaskBroadPhaseLayerFilter final : public JPH::BroadPhaseLayerFilter {
 public:
  explicit MaskBroadPhaseLayerFilter(LayerMask mask) noexcept : mask_(mask) {}
  bool ShouldCollide(JPH::BroadPhaseLayer tree) const override {
    for (u32 i = 0; i < k_layer_count; ++i) {
      const Layer layer = static_cast<Layer>(i);
      if (!mask_.test(layer)) continue;
      if (broad_phase_layer_of(layer) == tree) return true;
    }
    return false;
  }

 private:
  LayerMask mask_;
};

}  // namespace engine::physics

#pragma once

// Shared vocabulary of domain/physics: handles, layers, and the error enum. Split out of
// physics.h so that soft_body.h can name a BodyId without including the whole world.
//
// Nothing here (or anywhere else under include/) mentions Jolt. The backend is an
// implementation detail of src/, and swapping it must not touch a caller (ADR-0026 decision 3).

#include <core/base/types.h>
#include <core/containers/slot_map.h>

namespace engine::physics {

// How a call failed. Ok is the only success value; every creation function returns one of
// these and leaves its out-parameter untouched on failure.
enum class Status : u8 {
  Ok,
  InvalidArgument,  // the caller's data is not a body, shape, or mesh we can build
  NotFound,         // a stale or null handle
  LimitReached,     // a world limit (max bodies, debris cap, backend job count) is full
  Unsupported,      // a combination the backend does not implement
  BackendError,     // the backend refused and said why in the log
};

const char* status_name(Status status) noexcept;

// Collision layers (plan 05 §5.11). The layer decides what a body is tested against, and the
// broadphase layer it maps to decides which tree it lives in, so that the trees of things that
// never move are not rebuilt every step.
//
//   Static     the world: terrain, buildings, static meshes. Never moves, never tested
//              against another Static.
//   Moving     ordinary dynamic bodies and soft bodies. Tested against everything.
//   Debris     short-lived dynamic pieces from destruction. Tested against Static, Kinematic,
//              and Moving, but never against other Debris: a thousand pieces of rubble that
//              collide with each other is a quadratic bill for a visual effect.
//   Kinematic  animation- or code-driven bodies (platforms, doors, a character's capsule).
//              Tested against Moving and Debris; two kinematic bodies pass through each other
//              because neither would respond.
//   Query      collision geometry that exists only to be raycast or overlapped (triggers,
//              cover volumes, aim probes). Never simulated against anything.
enum class Layer : u8 { Static, Moving, Debris, Kinematic, Query, Count };

constexpr u32 k_layer_count = static_cast<u32>(Layer::Count);

const char* layer_name(Layer layer) noexcept;

// True when a body in `a` is simulated against a body in `b`. Symmetric by construction; the
// table is asserted symmetric in the tests.
constexpr bool layers_collide(Layer a, Layer b) noexcept {
  if (a == Layer::Query || b == Layer::Query) return false;
  if (a == Layer::Static && b == Layer::Static) return false;
  if (a == Layer::Kinematic && b == Layer::Kinematic) return false;
  if (a == Layer::Debris && b == Layer::Debris) return false;
  if (a == Layer::Static && b == Layer::Kinematic) return false;
  if (a == Layer::Kinematic && b == Layer::Static) return false;
  return true;
}

// A set of layers, for filtering queries. Default-constructed means "no layers"; use
// LayerMask::all() for "everything".
struct LayerMask {
  u8 bits = 0;

  static constexpr LayerMask all() noexcept {
    return LayerMask{static_cast<u8>((1u << k_layer_count) - 1u)};
  }
  static constexpr LayerMask of(Layer layer) noexcept {
    return LayerMask{static_cast<u8>(1u << static_cast<u32>(layer))};
  }
  constexpr bool test(Layer layer) const noexcept {
    return (bits & (1u << static_cast<u32>(layer))) != 0;
  }
  constexpr LayerMask with(Layer layer) const noexcept {
    return LayerMask{static_cast<u8>(bits | of(layer).bits)};
  }
  constexpr LayerMask without(Layer layer) const noexcept {
    return LayerMask{static_cast<u8>(bits & ~of(layer).bits)};
  }
  constexpr bool operator==(const LayerMask&) const noexcept = default;
};

// How a body is driven. Static never moves; Kinematic moves because something told it to and
// is infinitely heavy to everything it hits; Dynamic is solved.
enum class MotionType : u8 { Static, Kinematic, Dynamic };

// Handles are SlotMap handles (plan 11 §11.9) in a strong wrapper, so a BodyId cannot be
// passed where a ShapeId belongs. A default-constructed handle is null and every lookup of it
// fails rather than aliasing slot 0. The ordering is only a total order for sorting; it says
// nothing about creation time.
struct ShapeId {
  SlotHandle handle{};
  constexpr bool is_null() const noexcept { return handle.is_null(); }
  constexpr explicit operator bool() const noexcept { return !is_null(); }
  constexpr bool operator==(const ShapeId&) const noexcept = default;
  constexpr auto operator<=>(const ShapeId&) const noexcept = default;
};

struct BodyId {
  SlotHandle handle{};
  constexpr bool is_null() const noexcept { return handle.is_null(); }
  constexpr explicit operator bool() const noexcept { return !is_null(); }
  constexpr bool operator==(const BodyId&) const noexcept = default;
  constexpr auto operator<=>(const BodyId&) const noexcept = default;
};

struct SoftBodyId {
  SlotHandle handle{};
  constexpr bool is_null() const noexcept { return handle.is_null(); }
  constexpr explicit operator bool() const noexcept { return !is_null(); }
  constexpr bool operator==(const SoftBodyId&) const noexcept = default;
  constexpr auto operator<=>(const SoftBodyId&) const noexcept = default;
};

}  // namespace engine::physics

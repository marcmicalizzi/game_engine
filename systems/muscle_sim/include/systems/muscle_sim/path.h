#pragma once

// A line actuator's path from a posed skeleton (docs/subsystems/muscle_sim.md, "The path").
//
// The kernel reads a path as a cubic Hermite over each tick (muscle::HermitePath: the length and
// its rate at the tick's two boundaries). Here those come from the skeleton: the routed length at a
// boundary is the polyline through the actuator's attachments and via points, each a joint and an
// offset in its frame, carried to model space by the joint's posed transform; the rate at a
// boundary is the backward difference of the lengths at it and at the boundary before. That is
// first order: the rate a Hermite is given lags the true one by (dt/2) L'' + O(dt^2), so a path
// whose second derivative is large over a tick is followed with an error the tests measure. A
// velocity-aware rate (the joints' angular velocities through the route's Jacobian, which the
// animation capability does not publish today) is the later improvement.
//
// Nothing here touches the entity store: the system (muscle_sim.h) gathers the transforms and calls
// these, and the tests call them on a synthetic skeleton.

#include <core/base/types.h>
#include <core/math/math.h>
#include <systems/muscle/muscle.h>

#include <span>

namespace engine::muscle_sim {

// One point of a routed path: a joint of the skeleton and an offset in the joint's frame, metres.
struct RoutePoint {
  u32 joint = 0;
  Vec3 offset{};
};

// The most points a route has: the origin, the insertion and up to six fixed via points. A record
// with more is refused at materialization with that said (ADR-0017: stated, not hidden).
inline constexpr u32 k_max_route_points = 8;

// The polyline length through `route`, each point carried to model space by its joint's transform
// in `model`, in f64 from the f32 matrices and offsets. An entity's world transform is rigid
// (`world::Transform` has no scale), so the model-space length is the world-space length and no
// world coordinate is ever formed (ADR-0053). Every joint must be in range.
double route_length(std::span<const Mat4> model, std::span<const RoutePoint> route) noexcept;

// The path's history at the last tick boundary: the routed length there and the rate the last tick
// ended with. Not valid before the first tick, or after a boundary whose length was not a number.
struct PathHistory {
  double length = 0.0;  // m
  double rate = 0.0;    // m/s
  bool valid = false;
};

// Tick k's path, from the history at boundary k-1 and the routed `length` at boundary k: a Hermite
// from (history.length, history.rate) to (length, (length - history.length) / dt). With no valid
// history the tick's path is constant at `length`. Advances the history to boundary k.
muscle::HermitePath next_path(PathHistory& history, double length) noexcept;

}  // namespace engine::muscle_sim

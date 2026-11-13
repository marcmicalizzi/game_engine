#pragma once

// **An obstacle in the swash, as closed forms** (roadmap S102; the design is docs/experiments/
// shore-mechanics.md, "Stage 3: a shore of rock, and a boulder in the swash"). A boulder that is an
// instance is not in a body's depth map, and until a later pass the sheet passes through it. What
// the sheet does at one is a function of the sheet's speed there and the obstacle's size against
// it; these are those functions in double, held by cases (`water_tests.cpp`), **and nothing draws
// them yet**: no list of discs is made at load and no shader reads one.
//
// The obstacle is a disc: its centre, its radius, the height of its top over the body's level.
// The sheet's front reaches it travelling along a unit direction `way` (the swash's arrival's
// gradient) at the speed a projectile has left: sqrt(2 g E) where the front still has E to climb.

#include <core/base/types.h>
#include <domain/gfx/water_swash.h>

#include <algorithm>
#include <cmath>

namespace engine::gfx {

struct WaterDisc {
  f64 x = 0.0;  // the centre, body-local
  f64 z = 0.0;
  f64 radius = 0.0;
  f64 top_m = 0.0;   // its top over the body's level
  f64 foot_m = 0.0;  // the ground at its foot over the level (under 0: it stands in the water)
};

// The front's speed where it has `climb_m` still to climb: a projectile's (Shen and Meyer 1963).
inline f64 swash_front_speed(f64 climb_m) noexcept {
  return std::sqrt(2.0 * k_swash_g * std::max(climb_m, 0.0));
}

// **The detour**: how much further the front travels to reach the point (px, pz) than it would
// over an open beach, metres — 0 anywhere the front reaches straight on. Behind the disc (within
// its width, past its middle) the shortest way goes to the disc's side, round its back along the
// arc, and off along the tangent through the point: with rho the point's distance from the centre
// and psi its angle from the axis behind the disc,
//
//   detour = r (pi/2 - psi - acos(r / rho)) + sqrt(rho^2 - r^2) - a,     a = rho cos(psi),
//
// which is (pi/2 - 1) r where the point touches the disc's back, r^2 / (2 rho) far behind it (the
// lee closes), and 0 on the shadow's edge. Divided by the front's speed it is the delay the
// swash's phase takes there, so the front bends round the rock and its two arms close behind it.
inline f64 water_disc_detour(const WaterDisc& disc, f64 way_x, f64 way_z, f64 px, f64 pz) noexcept {
  const f64 dx = px - disc.x;
  const f64 dz = pz - disc.z;
  const f64 along = dx * way_x + dz * way_z;
  const f64 across = std::fabs(dz * way_x - dx * way_z);
  const f64 r = disc.radius;
  if (!(r > 0.0) || !(along > 0.0) || !(across < r)) return 0.0;
  const f64 rho = std::max(std::sqrt(along * along + across * across), r);
  const f64 psi = std::atan2(across, along);
  const f64 arc = 1.5707963267948966 - psi - std::acos(r / rho);
  if (!(arc > 0.0)) return 0.0;
  return r * arc + std::sqrt(std::max(rho * rho - r * r, 0.0)) - along;
}

// **The pile**: how far the sheet's surface rises against the disc's seaward face, metres — the
// speed head u^2 / 2g where the sheet meets the face head on, falling round the sides as the
// square of the cosine of the angle from the way (the normal part of the speed is what stops).
// `cos_face` is that cosine: -way . (the outward normal of the face at the place).
inline f64 water_disc_pile(f64 speed, f64 cos_face) noexcept {
  const f64 c = std::clamp(cos_face, 0.0, 1.0);
  return speed * speed / (2.0 * k_swash_g) * c * c;
}

// What the sheet does at the face: it **overtops** a rock whose top is under the pile (the rock
// pours for the rest of the period), it is **thrown** by a face that stands over the pile and is
// too steep to climb — past the surging limit of the wave there, Battjes's xi of 3.3 — and it
// **runs up** a face it can climb.
enum class WaterDiscMeeting : u8 { RunsUp, Thrown, Overtops };
inline WaterDiscMeeting water_disc_meeting(const WaterDisc& disc, f64 speed, f64 face_slope,
                                           f64 wave_height_m, f64 period_s) noexcept {
  const f64 pile = water_disc_pile(speed, 1.0);
  if (disc.top_m - disc.foot_m <= pile) return WaterDiscMeeting::Overtops;
  const f64 l0 = k_swash_g * period_s * period_s / (2.0 * 3.14159265358979323846);
  const f64 xi = face_slope / std::sqrt(std::max(wave_height_m, 1e-4) / std::max(l0, 1e-3));
  return xi > 3.3 ? WaterDiscMeeting::Thrown : WaterDiscMeeting::RunsUp;
}

}  // namespace engine::gfx

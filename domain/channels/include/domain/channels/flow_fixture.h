#pragma once

// **The flow fixture** (roadmap W120, stage 0; channels.md, "The fixture"): a ground and three
// solved channels whose every number comes from the closed forms of steady flow in a trapezoid,
// computed here by direct evaluation and by the direct step method — and **not** by the solver, so
// the fixture is what the solver is held to and what a draw is built against before the solver is
// whole. `engine-content flow fixture <dir>` writes it as a scene.
//
//   "uniform"  a reach at normal depth from end to end;
//   "step"     a mild approach drawn down to critical depth at a step's crest (M2), a fall, the
//              supercritical run below it (H3), a hydraulic jump, and the pool's backwater (H2) to
//              a mouth in a body;
//   "mouth"    a mild reach backed up by a lake (M1).
//
// Every decision is + - x /, a square root, the falls' cube root and `core/math/deterministic.h`,
// so the fixture is the same bytes from every toolchain.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <schemas/scene.h>
#include <string>

namespace engine::channels {

// ---- The closed forms of a trapezoid ------------------------------------------------------------

// A symmetric trapezoid: a flat bed `bed_m` wide and two banks that run `side` metres out for
// every metre they rise. `y` below is the depth over the bed.
struct Trapezoid {
  f64 bed_m = 0;
  f64 side = 0;
  f64 area(f64 y) const noexcept { return (bed_m + side * y) * y; }
  f64 top(f64 y) const noexcept { return bed_m + 2.0 * side * y; }
  f64 perimeter(f64 y) const noexcept;
  // The first moment of the area about the surface, A times the centroid's depth.
  f64 moment(f64 y) const noexcept { return bed_m * y * y / 2.0 + side * y * y * y / 3.0; }
  // The depth whose area is `a`: the quadratic's positive root.
  f64 depth_of_area(f64 a) const noexcept;
};

// Manning's discharge at depth y: Q = (1 / n) A R^(2/3) S^(1/2), R = A / P.
f64 trapezoid_discharge(const Trapezoid& t, f64 y, f64 n, f64 slope) noexcept;
// The friction slope that carries `q` at depth y: S_f = n^2 Q^2 / (A^2 R^(4/3)).
f64 trapezoid_friction(const Trapezoid& t, f64 y, f64 q, f64 n) noexcept;
// Specific energy over the bed, E = y + Q^2 / (2 g A^2), and specific force,
// M = Q^2 / (g A) + A ybar.
f64 trapezoid_energy(const Trapezoid& t, f64 y, f64 q) noexcept;
f64 trapezoid_force(const Trapezoid& t, f64 y, f64 q) noexcept;
// The Froude number, V / sqrt(g A / T).
f64 trapezoid_froude(const Trapezoid& t, f64 y, f64 q) noexcept;
// Critical depth: Q^2 T = g A^3. Normal depth: Manning's discharge equal to `q`. The sequent
// depth of y: the depth on the other side of critical with the same specific force. Each a
// bisection of a monotone function run until its bracket is one float wide.
f64 trapezoid_critical(const Trapezoid& t, f64 q) noexcept;
f64 trapezoid_normal(const Trapezoid& t, f64 q, f64 n, f64 slope) noexcept;
f64 trapezoid_sequent(const Trapezoid& t, f64 y, f64 q) noexcept;

// **The direct step method** (Chow 1959, section 10-3): the distance between two depths of a
// gradually varied profile, dx = dE / (S_0 - S_f), summed over `steps` equal steps of depth from
// `y0` (at x = 0) to `y1`, the friction slope the mean of each step's two ends. `x` is metres
// downstream of where the depth is `y0` (negative upstream).
struct DepthTable {
  Vector<f64> y;
  Vector<f64> x;
};
void direct_step(const Trapezoid& t, f64 q, f64 n, f64 slope, f64 y0, f64 y1, u32 steps,
                 DepthTable& out);
// The table's depth at `x` (linear between its rows; the end's past either end).
f64 depth_at(const DepthTable& table, f64 x) noexcept;

// ---- The fixture --------------------------------------------------------------------------------

// What the fixture was derived to: the page's numbers (channels.md, "The fixture").
struct FlowFixtureNumbers {
  f64 discharge_m3s = 0;
  f64 manning_n = 0;
  f64 slope = 0;
  f64 normal_m = 0;
  f64 critical_m = 0;
  f64 normal_froude = 0;
  // The step: where its crest is, its height, the depth at the head of the approach (M2), the
  // fall's foot (Rand 1955) and the depth there, the jump's head and foot with their depths and
  // its length, the depth at the mouth and the pool's level.
  f64 crest_x_m = 0;
  f64 drop_m = 0;
  f64 approach_head_m = 0;
  f64 foot_x_m = 0;
  f64 foot_m = 0;
  f64 foot_froude = 0;
  f64 jump_head_x_m = 0;
  f64 jump_head_m = 0;
  f64 jump_head_froude = 0;
  f64 jump_foot_x_m = 0;
  f64 jump_foot_m = 0;
  f64 jump_length_m = 0;
  f64 pool_mouth_m = 0;
  f64 pool_level_m = 0;
  // The mouth: the lake's level, the depth at the mouth and at the reach's head (M1).
  f64 lake_level_m = 0;
  f64 lake_mouth_m = 0;
  f64 lake_head_m = 0;
  // Stations each channel carries, and the nodes they were chosen from.
  u32 stations[3] = {};
  u32 nodes[3] = {};
};

// The ground as a heightfield (row-major, z then x, metres; point (0, 0) at the origin given),
// the two bodies and the three channels.
struct FlowFixture {
  u32 width = 0;
  u32 height = 0;
  i64 spacing_mm = 0;
  i64 origin_x_mm = 0;
  i64 origin_z_mm = 0;
  Vector<f32> heights;
  Vector<scene::WaterBody> bodies;
  Vector<scene::WaterChannel> channels;
  FlowFixtureNumbers numbers;
};
// The fixture's ground at a point, metres: the function the heightfield samples (linear between
// lattice rows and columns, so the lattice's bilinear surface is this function).
f64 flow_fixture_ground(f64 x_m, f64 z_m) noexcept;
// False with a sentence if the derivation does not close (a jump that finds no place).
bool make_flow_fixture(FlowFixture& out, std::string& error);

}  // namespace engine::channels

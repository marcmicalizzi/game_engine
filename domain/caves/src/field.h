#pragma once

// **The void's field** (docs/subsystems/caves.md, "The field"): one scalar over the world, positive
// in rock and negative in air, that a cave network's surface is the zero of — the union of every
// segment's section swept along its centreline and every hall, cut by the sediment floors, the
// water table and, under a mouth, the ground; with the walls' own detail (bedding ledges from the
// formation's beds, scallops along the flow) added before the union. The plan reads it at points
// (where the void meets the ground is a mouth), a cell samples it on the world's lattice and
// meshes it, and the dressing marches it for a roof and a floor.
//
// Every number is a function of whole millimetres of the world, evaluated in f64 from the plan's
// anchor with products, sums, divisions and correctly rounded square roots (`det::sin` for the
// formation's fold), so two cells that sample one lattice point compute one value on every
// toolchain, and the candidates' order never matters: a tie goes to the smaller id.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/hash/hash.h>
#include <domain/gfx/formation.h>

#include <cmath>
#include <schemas/caves.h>

namespace engine::caves::detail {

struct V3 {
  f64 x = 0.0, y = 0.0, z = 0.0;
};
constexpr V3 operator+(V3 a, V3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr V3 operator-(V3 a, V3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr V3 operator*(V3 a, f64 k) noexcept { return {a.x * k, a.y * k, a.z * k}; }
constexpr f64 dot(V3 a, V3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr V3 cross(V3 a, V3 b) noexcept {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline f64 length(V3 a) noexcept { return std::sqrt(dot(a, a)); }
inline V3 normalized(V3 a, V3 fallback = {0.0, 1.0, 0.0}) noexcept {
  const f64 l = length(a);
  return l > 1e-12 ? a * (1.0 / l) : fallback;
}
constexpr V3 lerp(V3 a, V3 b, f64 t) noexcept { return a + (b - a) * t; }
constexpr f64 lerp(f64 a, f64 b, f64 t) noexcept { return a + (b - a) * t; }
constexpr f64 clamp01(f64 t) noexcept { return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t); }
constexpr f64 smooth01(f64 t) noexcept {
  const f64 c = clamp01(t);
  return c * c * (3.0 - 2.0 * c);
}

constexpr u64 hash3(u64 seed, u64 a, u64 b = 0, u64 c = 0) noexcept {
  return mix64(hash_combine(hash_combine(hash_combine(seed, a), b), c));
}
// [0, 1) from a hash: 53 bits.
constexpr f64 unit(u64 h) noexcept {
  return static_cast<f64>(h >> 11) * (1.0 / 9007199254740992.0);
}

inline constexpr f64 k_q20 = 1.0 / 1048576.0;
inline constexpr f64 k_far = 1.0e9;
inline constexpr u32 k_no_cell = 0xFFFFFFFFu;

// What a piece of the surface is: which material it draws with.
enum SurfaceClass : u8 {
  k_surface_rock = 0,
  k_surface_sediment = 1,
  k_surface_breakdown = 2,
  k_surface_water = 3,
  k_surface_apron = 4,
  k_surface_classes = 5
};

struct FieldStation {
  V3 p;  // metres from the anchor
  V3 tangent;
  V3 across, up;  // the tube's section axes
  V3 level;       // the canyon's: level, across the passage
  f64 a = 0.0, b = 0.0;
  f64 hw = 0.0, top = 0.0, bot = 0.0;
  f64 floor_y = 0.0;  // metres from the anchor's height
  f64 s = 0.0;        // metres along the edge
  u32 cell = 0, edge = 0, index = 0;
  bool scallops = false;
  bool shaft = false;
};

struct FieldSegment {
  u32 i0 = 0, i1 = 0;
  V3 d;
  f64 length = 0.0;
  V3 lo, hi;
  u64 id = 0;
};

struct FieldHall {
  V3 c;
  f64 ra = 0.0, rb = 0.0, rh = 0.0;
  V3 axis, perp;
  f64 floor_y = 0.0, pile = 0.0;
  u32 cell = 0;
  u64 seed = 0;
  V3 lo, hi;
  u64 id = 0;
};

// One point's answer.
struct FieldSample {
  f64 phi = k_far;     // rock positive; the void, and the air over the ground, negative
  f64 raw = k_far;     // the nearest void's distance with no floor and no detail (rock positive)
  f64 void_f = k_far;  // the void with its floors, water and detail, the ground left out
  u32 cell = k_no_cell;
  u32 prim = 0xFFFFFFFFu;  // the owning segment, or a hall with the top bit set
  f32 along = 0.0f;        // where on the owning segment
  u8 cls = k_surface_rock;
  bool ground = false;  // the ground is the nearer surface here
};

struct Field {
  Vector<FieldStation> stations;
  Vector<FieldSegment> segments;
  Vector<FieldHall> halls;
  bool has_water = false;
  f64 water_y = 0.0;
  // Detail.
  gfx::FormationFrame frame{};
  gfx::FormationSequence beds{};
  // The integral of the beds' resistance from the sequence's base to each bed's base, and over a
  // period: what the ledges' box filter differences (`set_beds`).
  f64 bed_prefix[gfx::k_formation_max_beds] = {};
  f64 bed_total = 0.0;
  void set_beds(const gfx::FormationSequence& sequence) noexcept;
  V3 anchor_m;  // the anchor in metres of the world: what the formation is asked at
  f64 ledge = 0.0;
  f64 scallop = 0.0;  // the scallops' length; 0: none
  u64 seed = 0;
  // The ground under a mouth.
  i64 patch_i = 0, patch_j = 0;
  u32 patch_nx = 0, patch_nz = 0;
  const i32* patch = nullptr;
  i64 voxel_mm = 250;
  i64 ax = 0, ay = 0, az = 0;  // the anchor, whole millimetres of the world
  f64 apron = 0.0;
  // How far from a primitive's surface it is still asked.
  f64 margin = 1.0;
  // Bins over the primitives' bounds.
  V3 bin_lo;
  f64 bin_size = 4.0;
  i32 bins_x = 0, bins_y = 0, bins_z = 0;
  Vector<u32> bin_first;
  Vector<u32> bin_items;

  // The anchor (`ax`, `ay`, `az`) is set before the first `add_`.
  void add_station(const CellStation& s, V3 bed_normal, V3 strike);
  void add_hall(const CellHall& h);
  // Segments from the stations, bounds, bins: after the last `add_`.
  void finish();
  FieldSample sample(V3 p) const noexcept;
  // The ground's height (metres from the anchor's) at a point in metres from the anchor; false
  // outside the patch.
  bool ground_at(f64 x, f64 z, f64& out) const noexcept;
};

// The no-ground sentinel a patch column holds where no mouth's window covered it.
inline constexpr i32 k_no_ground = -2147483647 - 1;

// The scallops' length for a flow, metres (Curl 1966: v L / nu = 22,500 at 10 C).
f64 scallop_length(f64 flow_speed_m_s, f64 voxel_m) noexcept;

}  // namespace engine::caves::detail

namespace engine::caves {
struct Plan;
// The whole plan's field, no ground under any mouth: what the mouths are found in and the tests
// and the dressing ask (network_plan.cpp).
void plan_field(const Plan& plan, detail::Field& field);
}  // namespace engine::caves

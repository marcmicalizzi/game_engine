#pragma once

// The scatter capability (docs/subsystems/scatter.md; ADR-0027, ADR-0046; plan 07 §7.6): a
// placement generator that puts instances of a few meshes over the ground **by rule** — a seeded,
// tiled Poisson-disc of each kind at its spacing, thinned to a density field, then the kinds'
// slope, height and region rules, then the layer's exclusions — as a function of the layer, the
// seed and the tile alone, so a tile streamed in never moves a placement already visible and two
// runs on any toolchain give the same bytes.
//
// It registers as the placement generator "scatter" (`src/scene_generator.cpp`), which is how a
// scene names a layer (`{"generator": "scatter", "params": <engine.scatter.Layer>}`) and how the
// renderer's scene reader and the world ring reach it without naming it. Its output is
// `scene_gen::Placements`: instances of the kinds' meshes, which the reader loads once through the
// derived-data cache and every tile shares, and which cull and LOD as everything else does. Nothing
// here touches a document, the GPU or another capability; the ground is `scene_gen::Ground`, the
// view a scene's ground provider hands every placement generator.
//
// ADR-0027's checklist, as this capability answers it:
//
//   [x] schema types      schemas/scatter.schema (`engine.scatter.Layer`), engine_schema_library
//   [-] scheduler entry   none: it runs when a scene is read or a tile is streamed, never per tick
//   [-] render passes     none: its output is instances the renderer already draws
//   [-] derived data      the procedural rock's GLB, written once under a name that is its hash
//   [-] protocol methods  none: a layer reaches the host as a scene's placements entry
//   [-] tunables          none: a layer's budget is its own data
//   [x] LOD policy        the budget: a kind's `far` (rings past it place none of it) and the
//                         layer's `per_tile` cap; far tiers place fewer kinds, not smaller meshes
//   [x] determinism       "derived": integer millimetres and a pure hash, no C-library rounding
//   [x] zero cost unused  a scene that names no layer never calls it; OFF removes it
//   [x] docs, tests, size table

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/scene_gen/scene_gen.h>

#include <memory>
#include <schemas/scatter.h>
#include <span>
#include <string>
#include <string_view>

namespace engine::scatter {

// The name the generator registers under, which a scene's placements entry gives.
inline constexpr const char* k_placement_generator = "scatter";

// Determinism stance (ADR-0010): "derived" — a layer's output is a pure function of the layer, the
// seed, the tile and the ground, rebuilt wherever it is asked for, never part of the sim's state.
inline constexpr const char* k_determinism = "derived";

// **The packing of the Poisson-disc** (docs/subsystems/scatter.md, "Density"): the points a square
// metre the two-round process leaves at spacing 1 m, measured over 4 km² (`scatter_tests.cpp`,
// "the packing is what the density divides by"). A kind of spacing r keeps a point with
// probability `density / (k_packing / r²)`, so the density is honoured in expectation up to that
// cap, and past it the kind is as dense as its spacing lets it be.
inline constexpr f64 k_packing = 0.4805;

// The step the slope is measured over, either side of the point: half a metre, millimetres.
inline constexpr i64 k_slope_step_mm = 500;

// The most kinds a layer has: a far tier is named by how many kinds it leaves out, a `u8`.
inline constexpr u32 k_max_kinds = 32;

// The procedural rock's version: part of its file's name, bumped when its shape changes.
inline constexpr u32 k_rock_version = 1;

// One placement as the layer decided it: where (integer millimetres of the world in x and z, the
// ground's height less the sink in y), how it is turned and scaled, which kind, and the priority
// the budget drops by. Sorted by (kind, z, x), which no generation order changes. 56 bytes.
struct Point {
  i64 x_mm = 0;
  i64 z_mm = 0;
  f64 y = 0.0;
  Quat rotation = Quat::identity();
  f32 yaw = 0.0f;  // radians about +y, before the alignment
  f32 scale = 1.0f;
  u32 kind = 0;
  u32 rank = 0;  // the budget's order: lower is kept first
};

// Where a point stands in the world: its millimetres divided once in f64.
inline WorldPos position(const Point& p) noexcept {
  return WorldPos{static_cast<f64>(p.x_mm) / 1000.0, p.y, static_cast<f64>(p.z_mm) / 1000.0};
}

// A rectangle of the world in millimetres, [x0, x1) x [z0, z1).
struct RectMm {
  i64 x0 = 0;
  i64 z0 = 0;
  i64 x1 = 0;
  i64 z1 = 0;
  bool contains(i64 x, i64 z) const noexcept { return x >= x0 && x < x1 && z >= z0 && z < z1; }
  RectMm grown(i64 by) const noexcept { return RectMm{x0 - by, z0 - by, x1 + by, z1 + by}; }
};

// A mask read from a `.tex` file: its level 0's red channel over a world rectangle.
struct Mask {
  u32 width = 0;
  u32 height = 0;
  Vector<u8> red;
  f64 min_x = 0.0, min_z = 0.0, max_x = 0.0, max_z = 0.0;
  bool present() const noexcept { return width != 0; }
  // Bilinear, at texel centres, in [0, 1]; 0 outside the rectangle.
  f64 sample(f64 x_m, f64 z_m) const noexcept;
};

// The layer read, checked and turned into the integers it decides in: what every query runs
// against. Immutable once opened, so `place` may run on as many threads as there are tiles.
class Placer {
 public:
  Placer();
  ~Placer();
  Placer(Placer&&) noexcept;
  Placer& operator=(Placer&&) noexcept;
  Placer(const Placer&) = delete;
  Placer& operator=(const Placer&) = delete;

  // Reads `params` as an `engine.scatter.Layer` and checks it. `dir` is what relative paths
  // resolve against; `world_seed` stands in for a seed of 0. `excluded_ok` is false for a layer an
  // exclusion names, which may not exclude by layers itself.
  bool open(const Layer& desc, std::string_view dir, u64 world_seed, std::string* error,
            bool layers_allowed = true);

  const Layer& layer() const noexcept;
  u64 seed() const noexcept;
  i64 tile_mm() const noexcept;
  u32 kind_count() const noexcept;

  // Whether the region reaches the tile at all.
  bool occupies(scene_gen::TileCoord tile) const noexcept;
  // The tiles a whole read places on, rows of x in order of z: refused for a region of the whole
  // scene, which has no whole.
  bool region_tiles(Vector<scene_gen::TileCoord>& out, std::string* error) const;

  // Every placement of the kinds in `kind_mask` (bit k, kind k) on `tile`, standing on `ground`,
  // after the budget's cap, sorted by (kind, z, x). The cap and every rule are decided over every
  // kind, so the mask only leaves some out: a far ring draws a subset of a near ring's placements.
  void place_tile(scene_gen::TileCoord tile, const scene_gen::Ground& ground, u32 kind_mask,
                  Vector<Point>& out) const;
  // The density field at a point, placements a square metre before the kinds share it.
  f64 density_at(i64 x_mm, i64 z_mm) const noexcept;
  // Every placement of every kind in `rect`, before the budget (the budget is per tile), sorted.
  void place_rect(const RectMm& rect, const scene_gen::Ground& ground, Vector<Point>& out) const;

  // The kinds a ring places: those whose `far` is 0 or past the ring's nearest tile.
  u32 ring_kind_mask(const scene_gen::Context& context) const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The kinds' meshes: a named file resolved against `dir`, or the procedural rock, written into the
// layer's mesh directory under its hash when it is not there yet. One per kind, in kind order.
bool kind_meshes(const Layer& desc, std::string_view dir, Vector<scene_gen::PlacementMesh>& out,
                 std::string* error);

// The procedural rock as positions, normals and indices; and as a GLB.
struct RockMesh {
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<u32> indices;
};
void make_rock(const scatter::Rock& rock, RockMesh& out);
Vector<u8> rock_glb(const scatter::Rock& rock);
// The rock's file name: "rock-<16 hex of its fields' hash and k_rock_version>.glb".
std::string rock_file_name(const scatter::Rock& rock);

// The hash of placements' bytes, in the order given: what the determinism test pins.
u64 hash_points(std::span<const Point> points) noexcept;

// sin and cos with no C library: an argument reduction and a fixed polynomial, + and * alone, so
// every toolchain rounds every placement's rotation the same (AGENTS.md: no `std::` function whose
// rounding the C library decides inside a decision). Within 2e-15 of the C library over ±30 rad.
void sin_cos(f64 radians, f64& s, f64& c) noexcept;

}  // namespace engine::scatter

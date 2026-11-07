#pragma once

// **A terrain's holes** (`engine.scene.Terrain`'s `holes`; docs/subsystems/terrain.md, "Holes";
// roadmap W87): regions of the world's ground plane — circles and simple polygons — where the
// ground has no surface: a cave's mouth, a sinkhole, a shaft. Every consumer of the ground asks the
// same question of them: the renderer's meshes leave a cell's triangles out where it is a hole
// (the world's tiles, the far levels, the scene's grid), and the collision leaves the heightfield's
// samples out where every cell round them is one.
//
// **Why here, in the registry's module**, like the ridges and basins (terrain_features.h): a hole
// is part of the terrain *entry*, not of any one provider — an authored tile set and the dunes
// alike — and the renderer and the collision, which must not depend on a ground capability, both
// read it. One copy of the arithmetic, so a cell is a hole for the picture exactly where it is one
// for the walker's mesh-level answer.
//
// **The cell rule.** A cell of a lattice is a hole when its four corners lie strictly inside one
// region: for a circle or a convex polygon that is the cell inside it. So a cell on the rim keeps
// its triangles, the hole a level draws is the union of its cells wholly inside, and **a level
// whose cells are larger than the hole closes it** (none of its cells fits). Coordinates are
// millimetres in f64, the regions' and the lattice's alike — a lattice point is `i * spacing_mm`
// exactly — and the tests are products and sums, never libm, so a cell is a hole on every
// toolchain or on none.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <schemas/scene.h>
#include <span>
#include <string>

namespace engine::scene_gen {

class TerrainHoles {
 public:
  // Compiles `holes`. False, with a sentence naming the hole, for one with neither a polygon (three
  // points at least) nor a positive radius, or with both, or a coordinate that is not finite.
  bool build(std::span<const scene::TerrainHole> holes, std::string* error = nullptr);
  bool empty() const noexcept { return regions_.empty(); }
  u32 count() const noexcept { return regions_.size(); }
  // `hash_bytes` over every region's numbers in order: 0 for none, so a terrain without holes
  // hashes as it always did.
  u64 hash() const noexcept { return hash_; }

  // Whether the world point (millimetres) lies strictly inside some hole.
  bool contains_mm(f64 x_mm, f64 z_mm) const noexcept;
  // Whether the cell with corners (x0, z0) and (x1, z1), millimetres, is a hole: all four corners
  // strictly inside one region.
  bool cell_mm(f64 x0, f64 z0, f64 x1, f64 z1) const noexcept;
  // Cell (i, j) of the world's lattice at `spacing_mm`: corners `(i, j) * spacing` and
  // `(i + 1, j + 1) * spacing`.
  bool cell(i64 spacing_mm, i64 i, i64 j) const noexcept;
  // Whether any hole's bounds reach the rectangle [x0, x1] x [z0, z1], millimetres: the test a
  // tile makes before it looks at its cells, so a tile no hole reaches pays one comparison a hole.
  bool touches_mm(f64 x0, f64 z0, f64 x1, f64 z1) const noexcept;
  // `hash_combine` over the hole cells of the window [i0, i0 + nx) x [j0, j0 + nz) of the lattice
  // at `spacing_mm`, in order of z then x, and the count of them; 0 and 0 when none is. What a
  // tile's key adds, so a tile no hole reaches keeps the key it always had.
  u64 window_hash(i64 spacing_mm, i64 i0, i64 j0, u32 nx, u32 nz, u32* cells = nullptr) const;

 private:
  struct Region {
    bool circle = false;
    f64 cx = 0.0, cz = 0.0, r2 = 0.0;
    Vector<f64> xs, zs;                          // the polygon, millimetres
    f64 x0 = 0.0, z0 = 0.0, x1 = 0.0, z1 = 0.0;  // bounds
  };
  static bool inside(const Region& r, f64 x, f64 z) noexcept;
  Vector<Region> regions_;
  u64 hash_ = 0;
};

}  // namespace engine::scene_gen

#pragma once

// A tile of the dune field at a time (docs/subsystems/terrain.md, "Tiles, seams and the sampler"):
// the grid of heights, normals and materials a tile's mesh is built from, the crest lines, wind and
// flux a renderer's blowing-sand pass needs, and the sampler that answers a height and a normal at
// any point with exactly the numbers the mesh has.
//
// **Seamless by construction.** A tile is not a thing the field is made of: the field is a function
// of the world point, and a tile is where it is sampled. A tile gathers every primitive that can
// reach it — its neighbours' included, and the lattice cells upwind whose primitives the wind has
// carried in (dunes.h) — so the vertex two tiles share is the same point evaluated against the same
// primitives, and has the same height to the micrometre. Normals come from central differences over
// a one-vertex apron outside the tile, so the shared edge has the same normals too, and the
// material is a function of the point. The tests compare shared edges bit for bit at several times.
//
// **The deformed surface** is the base plus the declared drifts' steady state plus the overlay's
// deviation (overlay.h). A tile owns its overlay vertices [0, cells) and reads its neighbours' for
// its far edge and its apron, when they are loaded (`Deformation::overlays`); an unloaded
// neighbour's deviation reads as zero, which only matters for a tile beside one nobody is near.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/terrain/dunes.h>
#include <domain/terrain/feedback.h>

#include <span>

namespace engine::jobs {
class JobSystem;
}

namespace engine::terrain {

class Overlay;

inline constexpr u32 k_crest_points = 5;
// The four materials the renderer's terrain already has: sand, ridge rock, basin sand, the basin's
// floor, chosen as `renderer::terrain_material` chooses them.
inline constexpr u32 k_materials = 4;
// The wind speed the record's mean strength stands for, m/s: what `TileWind::speed` is scaled by.
inline constexpr f32 k_mean_wind_mps = 8.0f;

struct TileOptions {
  i64 tile_mm = 32'000;
  // Quads a side; the tile size must be a multiple of it. 128 is the overlay's 25 cm grid.
  u32 cells = 128;
  Detail detail = Detail::dunes;
  bool normals = true;
  bool crests = true;
};

// A crest line as a renderer's blowing-sand pass wants it (plan 05 §5.13; terrain.md, "What the
// effect needs"): the crest (a barchan's brink) as five points on the surface, which way its slip
// face falls, how high and how sharp it is and how fast it moves. World metres. 88 bytes.
struct CrestLine {
  Vec3 points[k_crest_points];
  Vec2 lee;              // unit, horizontal: down the slip face
  f32 height_m = 0.0f;   // the primitive's crest height above the floor
  f32 sharpness = 0.0f;  // 0 rounded, 1 a slip face at the angle of repose
  f32 celerity_m_per_day = 0.0f;
  u16 cell_hash = 0;
  u8 band = 0;
  u8 kind = 0;  // PrimitiveKind
};

// The tile's wind at its time: the day's direction and strength, and the sand it moves.
// The wind over the tile in the hour holding the tile's time (`WindRecord::wind_at`): a storm's
// when one is blowing, the day's otherwise.
struct TileWind {
  Vec2 direction;              // unit, where the sand moves to
  f32 speed_mps = 0.0f;        // the hour's wind, `k_mean_wind_mps` at the record's mean
  f32 flux_m2_per_day = 0.0f;  // the hour's sand flux per metre of width, as a daily rate
  // The saltation flux over this tile: that flux times the share of the tile that is loose sand
  // (rock sheds no grains), m^2 a day. What drives a crest's plume.
  f32 saltation_m2_per_day = 0.0f;
  bool storm = false;  // a storm is blowing this hour
};

// Everything a tile adds to its base: the drifts that can reach it (its neighbours' included) and
// the overlays of the 3 x 3 tiles round it, [dz + 1][dx + 1], null where one is not loaded.
struct Deformation {
  std::span<const DriftDecl> drifts;
  const Overlay* overlays[3][3] = {};
};

struct TileOutput {
  TileCoord tile;
  i64 time_us = 0;
  i64 tile_mm = 0;
  u32 cells = 0;
  Detail detail = Detail::dunes;
  // (cells + 1)^2 vertices, row by row along +x, rows along +z: µm, the vertex's normal, and its
  // material.
  Vector<i32> height_um;
  Vector<Vec3> normals;
  Vector<u8> material;
  Vector<CrestLine> crests;
  TileWind wind;
  i32 min_um = 0;
  i32 max_um = 0;
  i64 mean_um = 0;
  u32 primitives = 0;  // gathered
  u32 sand_vertices = 0;
  // The tile's content hash: every height, material and normal's bits, little-endian. The number
  // the golden tests pin and `engine-content terrain` prints.
  u64 hash() const noexcept;
};

// One tile at one time. `lag` and `deformation` may be null: the base field alone.
void evaluate_tile(const DuneField& field, TileCoord tile, i64 time_us, const TileOptions& options,
                   const LagField* lag, const Deformation* deformation, TileOutput& out);

// Many tiles, on the job system's performance pool when `jobs` is given. Each tile is independent
// and written to its own slot, so the result does not depend on the thread count (a test asserts
// it).
void evaluate_tiles(const DuneField& field, std::span<const TileCoord> tiles, i64 time_us,
                    const TileOptions& options, const LagField* lag, jobs::JobSystem* jobs,
                    Vector<TileOutput>& out);

// The tile's mesh: (cells + 1)^2 positions in world metres, their normals, and two triangles a
// quad, counter-clockwise seen from +y (the renderer's heightfield winding).
void build_tile_mesh(const TileOutput& tile, Vector<Vec3>& positions, Vector<Vec3>& normals,
                     Vector<u32>& indices);

// The sampler contract (renderer.md, "The terrain"): a height and a normal at any point, the same
// numbers the mesh of the tile under the point has at its vertices — the height is the field
// evaluated at the point, and the normal the central difference over the mesh's spacing that the
// mesh's normals are. It keeps the last tile's gather, so a caller walking a tile pays the gather
// once; not thread-safe, one per thread.
class TileSampler {
 public:
  TileSampler(const DuneField& field, i64 time_us, const TileOptions& options = {},
              const LagField* lag = nullptr);
  i64 height_um(i64 x_mm, i64 z_mm);
  f32 height(f32 x, f32 z) { return height_m(height_um(to_mm(x), to_mm(z))); }
  Vec3 normal(f32 x, f32 z);
  TileCoord tile_at(i64 x_mm, i64 z_mm) const noexcept;

 private:
  const Gather& gather_for(TileCoord tile);
  const DuneField* field_;
  i64 time_us_;
  TileOptions options_;
  const LagField* lag_;
  Gather gather_;
  TileCoord cached_;
  bool has_cache_ = false;
};

// The normal of a height grid by central differences, as the mesh and the sampler both compute it:
// (h(x - s) - h(x + s), 2 s, h(z - s) - h(z + s)) normalized, heights in µm and s in mm.
Vec3 grid_normal(i64 west_um, i64 east_um, i64 south_um, i64 north_um, i64 spacing_mm) noexcept;
// Which of `k_materials` a point is (the renderer's rule, from the field's ridge and basin
// weights).
u8 material_at(const DuneField& field, i64 x_mm, i64 z_mm) noexcept;

}  // namespace engine::terrain

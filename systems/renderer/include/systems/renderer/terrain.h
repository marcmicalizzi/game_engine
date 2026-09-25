#pragma once

// The procedural terrain a scene file may carry (docs/subsystems/renderer.md, "Scenes, camera
// paths and flythroughs"; schema `engine.scene.Terrain`). Seeded dunes, plus ridges and basins
// placed by hand so that a camera path can be laid out against them: the flythrough corpus of
// plan 09 §9.4 needs assets that sit fully occluded behind a ridge at a known frame, and a height
// function nobody can predict cannot promise that.
//
// **Why this is not the classic heightfield.** `Procedural::heightfield` is a fixed 20-unit
// fixture whose cluster ids and cuts the renderer's own tests and the reference corpus pin; this
// is a superset in metres with a seed and features, built beside it rather than into it so that
// nothing about the fixture moves. Both go down the same cluster LOD builder and the same
// streaming page layout, and the terrain's derived-data cache entry is keyed by `terrain_hash`,
// so it is content-addressed exactly like a file.
//
// The height is an analytic function of (x, z), which is what lets a scene put an instance on the
// ground and a camera path hold a height above it without the mesh existing yet.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>

#include <span>
#include <string>

namespace engine::renderer {

struct TerrainRidge {
  Vec2 from{};  // x, z
  Vec2 to{};
  f32 height = 50.0f;
  f32 width = 100.0f;  // where the profile reaches zero, metres from the segment
  f32 roughness = 0.15f;
};

struct TerrainBasin {
  Vec2 center{};  // x, z
  f32 radius = 100.0f;
  f32 depth = 5.0f;
};

struct TerrainDesc {
  bool enabled = false;
  u32 size = 1025;       // vertices a side
  f32 extent = 1000.0f;  // half the side: the grid spans [-extent, extent] in x and z
  u32 seed = 1;
  f32 dune_height = 3.0f;
  f32 dune_wavelength = 90.0f;
  Vector<TerrainRidge> ridges;
  Vector<TerrainBasin> basins;
};

// Bumped when `terrain_height` or the mesh built from it changes, so a cache entry built by an
// older generator is never mistaken for this one's. 5: one material over baked maps instead of
// four materials voted per cluster (`terrain_surface`).
inline constexpr u32 k_terrain_version = 5;
inline constexpr u32 k_terrain_max_size = 4097;

// The surface height at (x, z), metres. Defined everywhere, including outside the grid, so a
// camera or an instance off the edge still has a ground. It draws the dune field from the seed on
// every call, which costs more than the height itself (six waves' worth of trigonometry and a
// `pow` each); a caller that asks more than a handful of times holds a `TerrainSampler` instead.
f32 terrain_height(const TerrainDesc& desc, f32 x, f32 z) noexcept;

// The terrain's height function with its dune field drawn once: what a scene read asks for every
// instance it stands on the ground, every building of a ruins scatter asks some forty times (and a
// block-by-block ruin some hundreds), and a camera path asks for every key. It is the very object
// `terrain_height` builds per call and `build_terrain_mesh` builds per grid, so a height sampled
// here is **the same float, bit for bit**, as the direct function and as the mesh's vertex at that
// point — no cache, no interpolation, nothing that could disagree (the renderer's flythrough test
// compares them on a grid). It keeps a pointer to `desc`, whose ridges and basins it reads per
// call, so the description must outlive it and must not change under it.
class TerrainSampler {
 public:
  explicit TerrainSampler(const TerrainDesc& desc) noexcept;
  f32 height(f32 x, f32 z) const noexcept;
  f32 ridge_weight(f32 x, f32 z) const noexcept;
  f32 basin_weight(f32 x, f32 z) const noexcept;
  const TerrainDesc& desc() const noexcept { return *desc_; }

 private:
  static constexpr u32 k_waves = 6;
  struct Wave {
    f32 kx = 0.0f;
    f32 kz = 0.0f;
    f32 phase = 0.0f;
    f32 mx = 0.0f;
    f32 mz = 0.0f;
    f32 meander = 0.0f;
    f32 meander_phase = 0.0f;
    f32 amplitude = 0.0f;
  };
  f32 dunes(f32 x, f32 z) const noexcept;

  const TerrainDesc* desc_;
  Wave waves_[k_waves];
  f32 roll_kx_ = 0.0f;
  f32 roll_kz_ = 0.0f;
  f32 roll_phase_ = 0.0f;
};

// How much of each feature is under (x, z), in [0, 1]: the largest ridge profile and the largest
// basin weight. The terrain's surface (`terrain_surface`) is chosen from these.
f32 terrain_ridge_weight(const TerrainDesc& desc, f32 x, f32 z) noexcept;
f32 terrain_basin_weight(const TerrainDesc& desc, f32 x, f32 z) noexcept;

// Which of `k_terrain_materials` a point is: 0 sand, 1 rock (a ridge), 2 basin sand, 3 the
// basin's floor — the hard classification `terrain_surface` blends across a band around each
// threshold.
inline constexpr u32 k_terrain_materials = 4;
u32 terrain_material(const TerrainDesc& desc, f32 x, f32 z) noexcept;

// What the ground is at (x, z): its base colour (linear) and its perceptual roughness. The four
// materials' own values, blended across a band around each of `terrain_material`'s thresholds
// (0.1 of the feature's weight wide, which is ten to twenty metres on the desert's ridges and
// basin), so the ground changes from sand to rock over a stretch of ground rather than at a line.
//
// **Why a function of the point and not of a cluster.** Until 2026-09-25 the terrain carried the
// four materials and drew each cluster with the one most of its vertices were. A cluster of the
// LOD DAG is a patch whose size doubles with each level, so a coarse cluster's vote is over a
// larger patch than its children's and comes out differently near every boundary: as the camera
// moved and the cut changed, patches of rock and of the basin's green floor grew, shrank and
// jumped by whole clusters — the "green and grey ground textures that shift at position
// thresholds" of the second owner session
// (docs/experiments/second-interactive-session-2026-09-25.md). The colour now travels in a
// texture over the terrain's UVs (`bake_terrain_maps`), which every level interpolates from the
// same source vertices, so the ground under a pixel is the same colour whichever cut drew it,
// up to the cut's own geometric error.
struct TerrainSurface {
  Vec3 albedo{};
  f32 roughness = 1.0f;
};
TerrainSurface terrain_surface(const TerrainSampler& field, f32 x, f32 z) noexcept;

// The side of the terrain's material maps in texels: one texel per grid cell (`size - 1`),
// at least one. A cell is 2.5 m on the desert's 2049 grid over 5,120 m, and the colour changes
// over ten metres or more, so a finer map would store the same picture four times over.
u32 terrain_map_side(const TerrainDesc& desc) noexcept;

// The terrain's two material maps over its UVs (`build_terrain_mesh`: u across x, v across z,
// both [0, 1] over the grid), `side * side` RGBA8 texels each, rows in v order and texel centres
// at the cell centres: `base_color` is `terrain_surface`'s albedo encoded sRGB, alpha 255;
// `metallic_roughness` is glTF's packing, R 255 (no occlusion), G the roughness, B 0 (no metal).
// The terrain's one material samples them with factors of one and clamped edges.
void bake_terrain_maps(const TerrainDesc& desc, u32 side, Vector<u8>& base_color,
                       Vector<u8>& metallic_roughness);

// A content hash over every field and `k_terrain_version`: the "source hash" of the terrain's
// derived-data cache key, and what a run's summary names the terrain by.
u64 terrain_hash(const TerrainDesc& desc) noexcept;

// The grid: `size * size` positions, two counter-clockwise (seen from +y) triangles a quad, and
// UVs over [0, 1] across the whole terrain. False with a reason when the description is out of
// range.
bool build_terrain_mesh(const TerrainDesc& desc, Vector<Vec3>& positions, Vector<u32>& indices,
                        Vector<Vec2>& uvs, std::string* error = nullptr);

}  // namespace engine::renderer

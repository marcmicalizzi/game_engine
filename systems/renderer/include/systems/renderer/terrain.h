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
// older generator is never mistaken for this one's.
inline constexpr u32 k_terrain_version = 4;
inline constexpr u32 k_terrain_max_size = 4097;

// The surface height at (x, z), metres. Defined everywhere, including outside the grid, so a
// camera or an instance off the edge still has a ground.
f32 terrain_height(const TerrainDesc& desc, f32 x, f32 z) noexcept;

// How much of each feature is under (x, z), in [0, 1]: the largest ridge profile and the largest
// basin weight. The terrain's per-cluster materials are chosen from these.
f32 terrain_ridge_weight(const TerrainDesc& desc, f32 x, f32 z) noexcept;
f32 terrain_basin_weight(const TerrainDesc& desc, f32 x, f32 z) noexcept;

// Which of `k_terrain_materials` a point is: 0 sand, 1 rock (a ridge), 2 basin sand, 3 the
// basin's floor.
inline constexpr u32 k_terrain_materials = 4;
u32 terrain_material(const TerrainDesc& desc, f32 x, f32 z) noexcept;
// The material most of `points` (their x and z) are: what one cluster is drawn with. By vote
// rather than at the cluster's centre, because a coarse cluster can span a ridge's foot and a
// quarter of a kilometre of plain, and its centre then paints the plain with rock.
u32 terrain_majority_material(const TerrainDesc& desc, std::span<const Vec3> points) noexcept;

// A content hash over every field and `k_terrain_version`: the "source hash" of the terrain's
// derived-data cache key, and what a run's summary names the terrain by.
u64 terrain_hash(const TerrainDesc& desc) noexcept;

// The grid: `size * size` positions, two counter-clockwise (seen from +y) triangles a quad, and
// UVs over [0, 1] across the whole terrain. False with a reason when the description is out of
// range.
bool build_terrain_mesh(const TerrainDesc& desc, Vector<Vec3>& positions, Vector<u32>& indices,
                        Vector<Vec2>& uvs, std::string* error = nullptr);

}  // namespace engine::renderer

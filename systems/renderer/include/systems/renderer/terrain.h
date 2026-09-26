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
//
// **Two dune fields.** `TerrainGenerator::waves` is the field this file has always drawn: a handful
// of seeded transverse waves, and still the default. `TerrainGenerator::dunes` hands the dunes to
// the terrain capability's generator (docs/subsystems/terrain.md, ADR-0043): parametric dune
// primitives the seeded wind has carried to the description's game time, `time_s`, integer and the
// same bits on every toolchain. Ridges and basins are this file's in both, added the same way, and
// the generator flows its sand round them. A build without the terrain capability builds
// everything here and the scene reader refuses a terrain that names the generator, with a sentence;
// `terrain_generator_available` says which build this is.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>

#include <memory>
#include <span>
#include <string>

namespace engine::renderer {

enum class TerrainGenerator : u8 { waves = 0, dunes = 1 };
// Whether this build links the terrain capability's dune generator.
bool terrain_generator_available() noexcept;

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

// One band of the dune generator's table (`engine.scene.TerrainBand`; terrain.md, "The band
// table"), in metres and fractions of its cell, as the scene says it. The renderer hands it to the
// terrain capability unchanged; it knows nothing of what it means.
struct TerrainBand {
  std::string name;
  u8 kind = 0;  // 0 transverse, 1 barchan
  f32 height_min = 1.0f, height_max = 2.0f;
  f32 cell = 90.0f;
  f32 share = 1.0f;
  f32 length_min = 0.45f, length_max = 0.85f;
  f32 stoss = 0.4f, bend = 0.25f, sinuosity = 0.0f;
  f32 spread_deg = 20.0f;
  f32 sharpness = 1.0f;
  u32 side_days = 120, sharp_days = 30;
  u8 couple = 0;  // 0 none, 1 flanks, 2 floors
  f32 couple_width = 0.0f;
  bool far = true;
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
  // Which dune field (above). With `dunes`: the game time the field is drawn at, seconds since the
  // world's epoch, and the wind's mean sand flux, m^2 a year (terrain.md, "The wind record").
  TerrainGenerator generator = TerrainGenerator::waves;
  f64 time_s = 0.0;
  f32 sand_flux = 200.0f;
  // With `dunes`: the band table, tallest first; `has_bands` false is the default's three bands
  // (derived from the dune height and wavelength).
  bool has_bands = false;
  Vector<TerrainBand> bands;
  // With `dunes`: sandstorms a year (0..31) and their peak wind as a multiple of the mean
  // (terrain.md, "Storms"). None by default, and none leaves the hash as it was.
  u32 storms_per_year = 0;
  f32 storm_strength = 2.5f;
};

// With `dunes`: false, with a sentence, when the band table is one the generator cannot be built
// from (terrain.md, `validate_bands`); true without the capability, which refuses the generator
// on its own.
bool terrain_bands_valid(const TerrainDesc& desc, std::string* error) noexcept;

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
//
// With the dune generator the sampler holds the generator's field, and `height` asks it for the
// point: a small gather per call, a few microseconds, and thread-safe (the scene reader assembles
// ruins on a job pool through one sampler). `build_terrain_mesh` gathers once per block of vertices
// instead, and the answer is the same bits (the generator's gather rule, terrain.md, "Tiles, seams
// and the sampler").
class TerrainSampler {
 public:
  explicit TerrainSampler(const TerrainDesc& desc) noexcept;
  ~TerrainSampler();
  TerrainSampler(const TerrainSampler&) = delete;
  TerrainSampler& operator=(const TerrainSampler&) = delete;
  f32 height(f32 x, f32 z) const noexcept;
  // What a building stands on: `height` with the waves; with the dune generator, the interdune
  // floor plus the ridges and basins, which never moves — the dunes migrate over it and bury what
  // stands there (terrain.md, "The ruins' ground"), so a tile's ruin is the same building at any
  // time.
  f32 ground(f32 x, f32 z) const noexcept;
  f32 ridge_weight(f32 x, f32 z) const noexcept;
  f32 basin_weight(f32 x, f32 z) const noexcept;
  const TerrainDesc& desc() const noexcept { return *desc_; }

  // The generator's field, when the description names it and this build has it; else null.
  struct Dunes;
  const Dunes* dunes_field() const noexcept { return generator_.get(); }
  // The ridges and basins alone, and the masks the waves are multiplied by: what both fields add.
  f32 features(f32 x, f32 z, f32& ridge_mask, f32& flatten) const noexcept;

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
  std::unique_ptr<const Dunes> generator_;
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

// The sand's base colour, linear: `terrain_surface`'s albedo away from every ridge and basin, and
// the ground a scene with a terrain lights its hemisphere ambient's lower half with
// (`SceneData::ground_albedo`) — most of a desert is this sand, so a slip face in shade is lit by
// the colour it stands in.
Vec3 terrain_sand_albedo() noexcept;

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
// derived-data cache key, and what a run's summary names the terrain by. The generator's fields
// (and the generator's own version) enter it only when the description names the generator, so
// every waves terrain keeps the hash, and the cache entry, it had.
u64 terrain_hash(const TerrainDesc& desc) noexcept;

// The grid: `size * size` positions, two counter-clockwise (seen from +y) triangles a quad, and
// UVs over [0, 1] across the whole terrain. False with a reason when the description is out of
// range.
bool build_terrain_mesh(const TerrainDesc& desc, Vector<Vec3>& positions, Vector<u32>& indices,
                        Vector<Vec2>& uvs, std::string* error = nullptr);

// **The dune generator's grid at another game time** (terrain.md, "Re-evaluation"): the heights
// `build_terrain_mesh` puts in its positions' y, `size * size` of them in the same order, for the
// field the sampler holds at `time_s` instead of the description's own `time`. The grid is cut into
// the same 64 x 64 blocks the mesh is built in, `terrain_height_blocks` of them; this fills blocks
// [begin, end) of `heights` (sized `size * size` by the caller), so a caller can hand the blocks to
// as many jobs as it likes and get the same bytes. False, filling nothing, when the sampler holds
// no generator (the waves have no time) or this build has no terrain capability.
u32 terrain_height_blocks(const TerrainDesc& desc) noexcept;
bool evaluate_terrain_heights(const TerrainSampler& sampler, f64 time_s, u32 block_begin,
                              u32 block_end, std::span<f32> heights) noexcept;

}  // namespace engine::renderer

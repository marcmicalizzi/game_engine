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
// **Ground providers** (docs/subsystems/scene_gen.md, ADR-0046). What the terrain's height is comes
// from the ground provider the description names (`TerrainDesc::provider`, or `generator`), found
// in the scene-generator registry and made from the description: `waves` is the field this file
// has always drawn — a handful of seeded transverse waves, still the default, and registered by
// this file itself, so the one path serves the default too — and `dunes` is the terrain
// capability's generator (docs/subsystems/terrain.md, ADR-0043): parametric dune primitives the
// seeded wind has carried to the description's game time, `time_s`, integer and the same bits on
// every toolchain. Ridges and basins are the scene's in both (scene_gen/terrain_features.h), added
// the same way, and the generator flows its sand round them. The renderer links no provider but its
// own: a build without the terrain capability builds everything here, and the scene reader refuses
// a terrain that names a provider the executable does not carry, with a sentence naming it.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/gfx/ground_detail.h>
#include <domain/scene_gen/scene_gen.h>

#include <schemas/scene.h>
#include <span>
#include <string>
#include <string_view>

namespace engine::renderer {

// Which dune field a scene's `generator` enum names (`engine.scene.TerrainGenerator`), read as the
// provider `waves` or `dunes` when the description names none.
enum class TerrainGenerator : u8 { waves = 0, dunes = 1 };

// A ridge and a basin are the scene's own records (`engine.scene.Ridge`, `engine.scene.Basin`: a
// segment's ends and its height, width and roughness; a centre, radius and depth), which every
// provider adds the same way.
using TerrainRidge = scene::Ridge;
using TerrainBasin = scene::Basin;

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
  f32 celerity_scale = 1.0f;  // a stylization: the band's travel times this (terrain.md)
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
  // With `dunes`: the wind's day and the storms' transport gains (terrain.md, "The day's wind",
  // "A storm scales transport"; `engine.scene.Terrain` version 5). The defaults are the record as
  // it was and leave the hash as it was.
  f32 diurnal_strength = 0.0f;
  f32 diurnal_peak_hour = 15.0f;
  f32 diurnal_veer_deg = 0.0f;
  f32 diurnal_veer_hour = 12.0f;
  f32 storm_gain = 1.0f;
  struct StormGain {
    u32 storm = 0;
    f32 gain = 1.0f;
  };
  Vector<StormGain> storm_gains;
  // The ground provider's name (`engine.scene.Terrain.provider`; scene_gen.md). Empty: the one
  // `generator` names.
  std::string provider;
  // The sand's detail, close up (`engine.scene.TerrainDetail`, `Terrain` version 6; renderer.md,
  // "The sand close up"): wind ripples and grain the resolve draws as a function of position.
  // `has_detail` false — a scene without the block — draws the terrain exactly as it was.
  bool has_detail = false;
  scene::TerrainDetail detail;
};

// The detail's numbers as the mechanism takes them (`gfx::GroundDetailDesc`, domain/gfx/
// ground_detail.h): the scene's block, field for field.
gfx::GroundDetailDesc terrain_detail_desc(const TerrainDesc& desc) noexcept;
// False, with a sentence naming the field, for a detail block the mechanism cannot draw: a
// wavelength outside [0.01, 1] m, a height outside [0, 0.1] m or more than half the wavelength, an
// asymmetry outside [0.5, 0.95], defects outside [0, 1], a slope fade that does not rise within
// [0, 90] degrees, a grain cell outside (0, 0.2] m, grain strengths outside [0, 0.5].
bool validate_terrain_detail(const scene::TerrainDetail& detail, std::string* error);

// The name of the ground provider a description is drawn by: `provider`, or the one `generator`
// names ("waves", "dunes").
std::string_view terrain_provider(const TerrainDesc& desc) noexcept;
// The description as the scene's terrain entry (`engine.scene.Terrain`), what a provider is made
// from: every field as it is, and the provider's name.
scene::Terrain terrain_entry(const TerrainDesc& desc);
// Whether this executable carries the description's provider, and whether that provider's grounds
// move with game time (`scene_gen::k_ground_moves`) and have rings round a camera
// (`scene_gen::k_ground_rings`): what a host decides a time-lapse and the rings by, without making
// a ground.
bool terrain_provider_known(const TerrainDesc& desc) noexcept;
bool terrain_moves(const TerrainDesc& desc) noexcept;
bool terrain_has_rings(const TerrainDesc& desc) noexcept;

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

// The terrain's height function with its ground made once: what a scene read asks for every
// instance it stands on the ground, every building of a ruins scatter asks some forty times (and a
// block-by-block ruin some hundreds), and a camera path asks for every key. It is **the reader's
// view of a ground provider** (scene_gen.md): the provider the description names, made from it
// through the registry, and the very object `terrain_height` builds per call and
// `build_terrain_mesh` builds per grid, so a height sampled here is **the same float, bit for
// bit**, as the direct function and as the mesh's vertex at that point — no cache, no
// interpolation, nothing that could disagree (the renderer's flythrough test compares them on a
// grid). It keeps a pointer to `desc`, whose ridges and basins its surface functions read, so the
// description must outlive it and must not change under it.
//
// With the dunes the sampler holds the generator's field, and `height` asks it for the point: a
// small gather per call, a few microseconds, and thread-safe (the scene reader assembles ruins on a
// job pool through one sampler). `build_terrain_mesh` gathers once per block of vertices instead,
// and the answer is the same bits (the generator's gather rule, terrain.md, "Tiles, seams and the
// sampler").
//
// **A provider it cannot make** — a name this executable does not carry, or a description the
// provider refuses (a dune time out of range, a band table the field cannot be built from) — leaves
// the sampler drawing the waves over the same description, as a build without the terrain
// capability always did, with `ok()` false and the sentence in `error()`. The scene reader refuses
// such a file with that sentence; everything else gets a ground.
class TerrainSampler {
 public:
  explicit TerrainSampler(const TerrainDesc& desc) noexcept;
  ~TerrainSampler();
  TerrainSampler(const TerrainSampler&) = delete;
  TerrainSampler& operator=(const TerrainSampler&) = delete;
  f32 height(f32 x, f32 z) const noexcept { return provider_.height(x, z); }
  // What a building stands on: `height` with the waves; with the dunes, the interdune floor plus
  // the ridges and basins, which never moves — the dunes migrate over it and bury what stands there
  // (terrain.md, "The ruins' ground"), so a tile's ruin is the same building at any time.
  f32 ground(f32 x, f32 z) const noexcept { return provider_.floor(x, z); }
  f32 ridge_weight(f32 x, f32 z) const noexcept;
  f32 basin_weight(f32 x, f32 z) const noexcept;
  const TerrainDesc& desc() const noexcept { return *desc_; }

  // The ground the description names, as made: what the time-lapse re-evaluates, the rings are
  // built from and a placement generator stands on (`provider().view()`).
  const scene_gen::GroundProvider& provider() const noexcept { return provider_; }
  // Whether the ground moves with game time: the dunes do, the waves do not.
  bool moves() const noexcept { return provider_.moves(); }
  // The wind the ground's surface detail lies across at game time `time_s`, a unit vector over
  // (x, z) (`scene_gen::GroundOps::wind`): the dunes' ripple rule, the waves' prevailing wind.
  // +x for a ground that says none.
  // The ripples' transport at `time_s` (`scene_gen::GroundOps::transport`): the sand moved across a
  // metre of width since the ground's epoch, m^2, and the wind's strength over its mean; false for
  // a ground that says none.
  bool transport(f64 time_s, f64& moved_m2, f32& strength) const noexcept;
  Vec2 wind(f64 time_s) const noexcept;
  // False, with the sentence, when the named provider could not be made (above).
  bool ok() const noexcept { return error_.empty(); }
  const std::string& error() const noexcept { return error_; }
  // The ridges and basins alone, and the masks the waves are multiplied by: what every ground adds.
  f32 features(f32 x, f32 z, f32& ridge_mask, f32& flatten) const noexcept;

 private:
  const TerrainDesc* desc_;
  scene_gen::GroundProvider provider_;
  std::string error_;
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
  // How much of the ground here is sand — the plain's and the basin's, 1; the ridge rock and the
  // basin's floor, 0 — blended across the same bands as the colour: what the sand's detail is
  // weighted by (renderer.md, "The sand close up").
  f32 sand = 1.0f;
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
// `metallic_roughness` is glTF's packing, R 255 (no occlusion), G the roughness, B 0 (no metal),
// and alpha 255 — or, when the description asks for the sand's detail, **the sand share**
// (`TerrainSurface::sand`), which the resolve weights the detail by (`gfx::k_material_ground_
// detail`). Without a ridge or a basin that is 255 everywhere too, so the bytes, and the cache
// entry, are the ones a terrain without the detail has (`terrain_hash`).
// The terrain's one material samples them with factors of one and clamped edges.
void bake_terrain_maps(const TerrainDesc& desc, u32 side, Vector<u8>& base_color,
                       Vector<u8>& metallic_roughness);

// A content hash over every field and `k_terrain_version`: the "source hash" of the terrain's
// derived-data cache key, and what a run's summary names the terrain by. The generator's fields
// (and the generator's own version) enter it only when the description names the dunes, so every
// waves terrain keeps the hash, and the cache entry, it had; and a provider other than those two
// enters it by its name, with every field, so two providers never share an entry. Naming the dunes
// by `provider` or by `generator` is the same terrain and the same hash. **The detail's numbers
// never enter it**: they are shading, drawn per frame from the block, and the mesh and its maps do
// not depend on them. What the detail changes in the maps — the sand share in the
// metallic-roughness map's alpha — enters it only where it changes their bytes, which is a terrain
// with a ridge or a basin; the erg has neither, and keeps its hash and its cache entry.
u64 terrain_hash(const TerrainDesc& desc) noexcept;

// The grid: `size * size` positions, two counter-clockwise (seen from +y) triangles a quad, and
// UVs over [0, 1] across the whole terrain. False with a reason when the description is out of
// range.
bool build_terrain_mesh(const TerrainDesc& desc, Vector<Vec3>& positions, Vector<u32>& indices,
                        Vector<Vec2>& uvs, std::string* error = nullptr);

// **The moving ground's grid at another game time** (terrain.md, "Re-evaluation"): the heights
// `build_terrain_mesh` puts in its positions' y, `size * size` of them in the same order, for the
// ground the sampler holds at `time_s` instead of the description's own `time` — the provider's
// re-evaluation entry (`scene_gen::GroundOps::evaluate`). The grid is cut into the same 64 x 64
// blocks the mesh is built in, `terrain_height_blocks` of them; this fills blocks [begin, end) of
// `heights` (sized `size * size` by the caller), so a caller can hand the blocks to as many jobs as
// it likes and get the same bytes. False, filling nothing, when the ground does not move (the
// waves have no time).
u32 terrain_height_blocks(const TerrainDesc& desc) noexcept;
bool evaluate_terrain_heights(const TerrainSampler& sampler, f64 time_s, u32 block_begin,
                              u32 block_end, std::span<f32> heights) noexcept;

// **A terrain level's lattice** (renderer.md, "The dunes in time-lapse"): the points a level's
// vertices sit on and its height fields are sampled at. The scene's own grid is one — its
// coordinates computed exactly as `build_terrain_mesh` computes them, so a field on it is the
// mesh's heights to the bit — and a ring's is the world's grid at the ring's spacing, counted from
// the world's origin (terrain.md, "Rings"), so a ring that moves samples the points it did. The
// lattice is the registry's (`scene_gen::Lattice`), since a provider samples on it.
using TerrainLattice = scene_gen::Lattice;
TerrainLattice terrain_scene_lattice(const TerrainDesc& desc) noexcept;
TerrainLattice terrain_ring_lattice(i64 spacing_mm, i64 filter_mm = 0) noexcept;

// The moving ground's heights on a window of a lattice at `time_s`: `nx * nz` of them, rows of x
// in order of z, sample (i, j) at lattice point (i0 + i, j0 + j) — the ridges and basins added as
// `height` adds them. Cut into 64 x 64 blocks exactly as `evaluate_terrain_heights` is (one gather
// a block), so a caller hands blocks [begin, end) to as many jobs as it likes and gets the same
// bytes; over the scene lattice's whole window it is `evaluate_terrain_heights`. False, filling
// nothing, for a ground that does not move.
u32 terrain_window_blocks(u32 nx, u32 nz) noexcept;
bool evaluate_terrain_window(const TerrainSampler& sampler, f64 time_s,
                             const TerrainLattice& lattice, i32 i0, i32 j0, u32 nx, u32 nz,
                             u32 block_begin, u32 block_end, std::span<f32> heights) noexcept;

// **How far the ground's fastest feature travels between two game times**, metres — for the
// dunes, the largest over the bands of the wind's flux path length over the band's height
// (Bagnold's rule, the closed form `terrain::DuneField::displacement` moves the band's lattice by,
// taken along the path rather than between its ends so a reversal inside the interval is not a
// short move). Storms are in the record, so a storm's hours travel further. 0 for a ground that
// does not move.
f64 terrain_band_travel_m(const TerrainSampler& sampler, f64 from_s, f64 to_s) noexcept;

// The dunes' own field behind a sampler — a band's primitives and displacement, where a crest
// stands (terrain.md, "How far the big dunes move") — is the terrain capability's to hand out, not
// the renderer's, which links no generator: 	errain::dune_field(sampler.provider()), null for
// another ground (domain/terrain/scene_ground.h).

}  // namespace engine::renderer

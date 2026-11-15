#pragma once

// **The mask level** (docs/subsystems/tileset.md, "The mask level"; renderer.md, "Ground materials
// by place"): a terrain's `masks` block built into per-tile material weights beside its tile set —
// the generated priors (slope, elevation, aspect), under the painted weight maps, under the index
// map, under the regions — one file a tile in the layout `scene_gen::GroundMasks` hands the
// renderer
// (`domain/scene_gen/ground_masks.h`), keyed as the heights are.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/scene_gen/ground_masks.h>
#include <domain/tileset/tileset.h>

#include <schemas/scene.h>
#include <schemas/tileset.h>
#include <span>
#include <string>

namespace engine::tileset {

inline constexpr std::string_view k_mask_format = "engine.tileset.masks";
// Bumped whenever a build's output for the same set and inputs changes. 2 (2026-10-24, roadmap
// W76): the level is one archive (`<spacing_mm>.masks`) instead of a file a tile; every tile's
// bytes are what its file held.
// 3 (2026-11-08, roadmap T74): a layer whose peak weight over a tile is under one quantization step
// no longer takes a channel (see `k_mask_step`), so a tile that stored a layer at a weight of one
// 255th by rounding up now drops it.
// 4 (2026-11-11, roadmap T109): a region's `base` with no `priors` lays its share of the ground
// (the whole ground is the soil it stands in for), where it laid nothing.
inline constexpr u32 k_mask_version = 4;
// **One quantization step of a layer's weight as stored** (a mask texel's channel is a u8 of
// `round(weight * 255)`). A layer counts toward a tile's palette budget, and takes a channel, only
// when its peak weight over the tile's points is at least one step; under it the layer is dropped
// from that tile and named in a warning (`DroppedLayer`) when its peak is over half a step, which
// the old rounding stored as 1. A tile still over the budget with every layer at or over a step is
// refused exactly as before: the budget never widens.
inline constexpr f64 k_mask_step = 1.0 / 255.0;

// **The authored inputs**, as a build takes them: the block with its image paths resolved and its
// water bodies' outlines copied in (`scene_gen::resolve_mask_water`), the palette's names in order,
// the per-tile budget, and the terrain's map frame — the square the images span, millimetres.
struct MaskInputs {
  scene::TerrainMasks masks;
  Vector<std::string> palette;
  u32 budget = 4;
  i64 map_x0_mm = 0, map_z0_mm = 0, map_size_mm = 0;
};

// The inputs from a scene's terrain entry: its `masks` and `materials` (the palette and the budget)
// and its extent (the map frame, [-extent, extent]), the paths resolved against `dir`. False, with
// a sentence, for a terrain with masks and no materials.
bool mask_inputs(const scene::Terrain& entry, std::string_view dir, MaskInputs& out,
                 std::string& error);
// **The tile set a load composes, for a load and a check alike** (T74): the set the terrain's
// `tiles` block names, or its heightfield built with the block's options and `cuts` into the
// derived-data cache on a miss. `ddc_in` (may be empty) overrides the block's `ddc`; `ddc` is the
// root used and `manifest` the set's manifest. The `tiles` ground provider and `engine-content
// masks check` both call this, so the check cannot compose the uncut ground a load does not.
bool resolve_tile_set(const scene::Terrain& entry, std::string_view dir, const std::string& ddc_in,
                      std::string& ddc, std::string& manifest, std::string& error);
// False, with a sentence naming the field, for inputs a build cannot take: a spacing outside 250 to
// 4000 mm, tile cells not a power of two in 4..256, a budget outside 1..4, a palette over 254 or
// with a name empty or twice, a name the palette does not have, a `priors` block naming no role
// (T109), a region with no outline or two, a region giving both `material` and `base`, numbers out
// of range, an image that does not read.
bool validate_mask_inputs(const MaskInputs& in, std::string& error);
// The level's key: the set's hash, every input (each image by its bytes) and `k_mask_version`.
bool mask_key(const TileSetManifest& set, const MaskInputs& in, u64& out, std::string& error);
// "<ddc>/tilemasks/<key>".
std::string mask_cache_dir(std::string_view ddc, u64 key);

// A layer dropped from a tile for weighing under one step: the tile, the layer and its peak weight.
struct DroppedLayer {
  i32 tx = 0, tz = 0;
  u32 layer = 0;
  std::string name;
  f64 peak = 0;
};

struct MaskBuildResult {
  std::string manifest;
  u64 key = 0;
  bool hit = false;
  u32 tiles = 0;
  u64 bytes = 0;
  u64 hash = 0;
  // Filled by a build that ran (empty on a hit: the warning was given when the level was built).
  Vector<DroppedLayer> dropped;
};

// **The palette budget, checked without a cook** (roadmap T72): one layer a tile names, with its
// mean and peak weight over the tile's points.
struct MaskTileLayer {
  u32 layer = 0;  // index into the palette
  std::string name;
  f64 mean = 0, peak = 0;
  // **What the format would store** (W113): the points of the tile's mip 0 at or over one step
  // (`k_mask_step`), and the largest stored value, `round(peak * 255)`, in 255ths. A counted layer
  // has at least one such point; a mean that prints as 0.00 is not a layer of no weight.
  u32 points = 0;
  u32 stored = 0;
};
// A tile whose points name more layers than the budget. `layers` is heaviest first by mean weight;
// `weakest` indexes the last, the layer a blend-out would drop.
struct OverBudgetTile {
  i32 tx = 0, tz = 0;
  i64 x0_mm = 0, x1_mm = 0, z0_mm = 0, z1_mm = 0;
  Vector<MaskTileLayer> layers;
  u32 weakest = 0;
  u32 points = 0;  // the tile's mip-0 points, (cells + 1) squared
};
struct MaskBudgetReport {
  Vector<OverBudgetTile> over;   // in tile order, row by row
  Vector<DroppedLayer> dropped;  // layers under one step, in tile order, with a peak over half one
  u32 tiles = 0;                 // tiles scanned
  u32 largest = 0;               // the most layers any tile names
  u32 budget = 0;
  // **The level's resident size** (T121), as the renderer will hold it: the tiles a build writes
  // (any layer at a step or more, or any wetness; a tile over the budget counts, since it is one
  // once the scene is fixed), `scene_gen::mask_resident_bytes` over the range, the scene's figure,
  // and `scene_gen::mask_resident_refusal`'s sentence, empty when the level fits.
  u64 present = 0;
  u64 resident_bytes = 0;
  u32 resident_mib = 0;
  std::string resident_refusal;
};
// **Where a cook's tiles are composed** (T128; tileset.md, "The mask level's cook"): the job
// system's performance pool `jobs()` returns — asked only when there are tiles to compose, so a
// hit starts no pool — or the calling thread alone when it returns null. `lanes` is how many of
// the pool's threads (the caller's counted) work at once: 0 all of them, 1 the calling thread
// alone, more a cap. **The level is the same bytes at any value**: tiles are composed in any
// order and handed to the archive, the hash and the refusal in tile order.
struct MaskCookJobs {
  jobs::JobSystem* (*get)(void* context) = nullptr;
  void* context = nullptr;
  u32 lanes = 0;
  jobs::JobSystem* jobs() const { return get != nullptr ? get(context) : nullptr; }
};

// The composition a build runs, scanned for the budget and the resident size: no archive is
// written. Every tile over is listed, not the first. False, with a sentence, only when the inputs
// cannot be composed; a level over either is in the report (`over`, `resident_refusal`). On the
// pool as a build is (`cook`), the report the same at any number of threads.
bool check_mask_budget(const TileSet& set, const MaskInputs& in, MaskBudgetReport& out,
                       std::string& error, const MaskCookJobs& cook = {});

// **A level's footprint before anything is built** (T122): the mask tiles its tile set's lattice
// spans, read from the set's manifest when the terrain names one, else from its heightfield's
// header moved by the block's origin — no heights read, no set built. Every tile a build writes
// lies in it; `range` is what the renderer's page table covers.
struct MaskLevelSpan {
  i32 tx0 = 0, tz0 = 0, tx1 = -1, tz1 = -1;
  u64 range() const noexcept {
    return tx1 < tx0 || tz1 < tz0
               ? 0u
               : static_cast<u64>(tx1 - tx0 + 1) * static_cast<u64>(tz1 - tz0 + 1);
  }
};
bool mask_level_span(const scene::Terrain& entry, std::string_view dir, const MaskInputs& in,
                     MaskLevelSpan& out, std::string& error);
// **Whether every tile of the span is present, known from the inputs alone**: when the priors name
// soil, scree and bedrock, every point's weights sum to one over at most 254 layers, so some layer
// is over one step (1/254 > 1/255) at every point and every tile is written (or refused for its
// layers). Otherwise which tiles hold anything is the composition's to say.
bool mask_level_full(const MaskInputs& in) noexcept;
// **What a load refuses of the masks before it builds anything** (T122): the inputs
// (`validate_mask_inputs`), and the level's resident size against `in.masks.resident_mib` where
// the span alone gives it (`mask_level_full`), with the one sentence the upload and the check give.
// `known` says whether the size was known; `bytes` is then exact. The `tiles` provider calls it
// before it finds or builds its set, and `engine-content masks check` reaches it through the load.
struct MaskEarlyCheck {
  MaskLevelSpan span;
  bool known = false;
  u64 bytes = 0;
};
bool check_masks_early(const scene::Terrain& entry, std::string_view dir, const MaskInputs& in,
                       MaskEarlyCheck& out, std::string& error);
// How many over-budget tiles the build's refusal names before "and N more".
inline constexpr u32 k_over_budget_listed = 32;
// The refusal's sentence: the count, then up to `cap` tiles with place, layers and weakest.
std::string describe_over_budget(const MaskBudgetReport& report, u32 cap);

// Builds the level of `in` over `set` into `dir`, the manifest last. **Refused, with a sentence
// naming every tile over (up to `k_over_budget_listed`), its place and its layers, when a tile's
// points name more layers than the budget**: a dropped layer would show on one side of a seam and
// not the other. Its tiles are composed on `cook`'s pool and written in tile order (T128).
bool build_mask_level(const TileSet& set, const MaskInputs& in, const std::string& dir,
                      MaskBuildResult& out, std::string& error, const MaskCookJobs& cook = {});
class MaskLevel;
// The content step as a library call: into `dir`, or with it empty into `mask_cache_dir(ddc, key)`,
// unless the manifest there records this key (a hit; `force` builds anyway). `opened`, when given,
// is the level opened: on a hit the one the key's check opened, so a caller that reads the level
// does not parse its manifest a second time (T128).
bool ensure_mask_level(const TileSet& set, const MaskInputs& in, const std::string& dir,
                       const std::string& ddc, bool force, MaskBuildResult& out, std::string& error,
                       const MaskCookJobs& cook = {}, MaskLevel* opened = nullptr);

// **The priors as images an author keeps or edits**: one greyscale PNG per role named (soil,
// scree, bedrock, snow), its R the role's weight over the map frame at the mask's spacing, which a
// `paint` entry takes back. False, with a sentence, when they cannot be written.
bool write_mask_priors(const TileSet& set, const MaskInputs& in, const std::string& dir,
                       Vector<std::string>& written, std::string& error);

// The palette weights at one world point (millimetres) before quantization, in palette order, and
// the wetness: the field every tile's point is (the composite of the inputs). For tests and tools.
bool mask_point(const TileSet& set, const MaskInputs& in, i64 x_mm, i64 z_mm, Vector<f64>& weights,
                f64& wetness, std::string& error);

// **A built level, read back.** Its archive is opened once and its table read; a tile is copied
// out of a **read-ahead window** — the tiles from the one asked onwards in file order, up to 2 MiB,
// read as one queue of large ranges — because the renderer asks every tile in that order at load
// once (presence is counted from the manifest), so a level costs a few dozen reads, not one a tile.
// Safe to call from several threads (the window is behind a lock).
class MaskLevel {
 public:
  MaskLevel();
  ~MaskLevel();
  MaskLevel(MaskLevel&&) noexcept;
  MaskLevel& operator=(MaskLevel&&) noexcept;
  // `unbuffered`: the archive read past the OS's file cache (a cold measurement).
  bool open(const std::string& manifest_path, std::string& error, bool unbuffered = false);
  bool is_open() const noexcept { return info_.cells != 0; }
  const MaskLevelManifest& manifest() const noexcept { return manifest_; }
  const scene_gen::GroundMaskInfo& info() const noexcept { return info_; }
  bool present(i32 tx, i32 tz) const noexcept;
  // Tile (tx, tz)'s words (`scene_gen::mask_tile_words`); false for a tile not present or a file
  // that is not the manifest's.
  bool tile(i32 tx, i32 tz, std::span<u32> words) const noexcept;
  // The palette weights at a point (`scene_gen::mask_sample` of the tile holding it, its channels
  // put back in palette order), into `weights` (palette-sized), and the wetness. False past the
  // level or where no tile is.
  bool sample(i64 x_mm, f64 x_frac, i64 z_mm, f64 z_frac, f32 level, Vector<f32>& weights,
              f32& wetness) const;
  scene_gen::GroundMasks masks() const noexcept;

  // Reads of the archive so far: windows filled and the bytes they read.
  struct ReadStats {
    u64 windows = 0;
    u64 bytes = 0;
  };
  ReadStats read_stats() const noexcept;

 private:
  MaskLevelManifest manifest_;
  scene_gen::GroundMaskInfo info_;
  std::string dir_;
  Vector<u64> present_;  // sorted keys of the tiles written
  struct Reading;
  std::unique_ptr<Reading> reading_;
};

// "<spacing_mm>.masks" inside a level's directory: its archive.
std::string mask_archive(i64 spacing_mm);

}  // namespace engine::tileset

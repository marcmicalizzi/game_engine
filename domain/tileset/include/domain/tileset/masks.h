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
inline constexpr u32 k_mask_version = 2;

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
// False, with a sentence naming the field, for inputs a build cannot take: a spacing outside 250 to
// 4000 mm, tile cells not a power of two in 4..256, a budget outside 1..4, a palette over 254 or
// with a name empty or twice, a name the palette does not have, a region with no outline or two, a
// region giving both `material` and `base`, numbers out of range, an image that does not read.
bool validate_mask_inputs(const MaskInputs& in, std::string& error);
// The level's key: the set's hash, every input (each image by its bytes) and `k_mask_version`.
bool mask_key(const TileSetManifest& set, const MaskInputs& in, u64& out, std::string& error);
// "<ddc>/tilemasks/<key>".
std::string mask_cache_dir(std::string_view ddc, u64 key);

struct MaskBuildResult {
  std::string manifest;
  u64 key = 0;
  bool hit = false;
  u32 tiles = 0;
  u64 bytes = 0;
  u64 hash = 0;
};

// Builds the level of `in` over `set` into `dir`, the manifest last. **Refused, with a sentence
// naming the tile, its place and its layers, when a tile's points name more layers than the
// budget**: a dropped layer would show on one side of a seam and not the other.
bool build_mask_level(const TileSet& set, const MaskInputs& in, const std::string& dir,
                      MaskBuildResult& out, std::string& error);
// The content step as a library call: into `dir`, or with it empty into `mask_cache_dir(ddc, key)`,
// unless the manifest there records this key (a hit; `force` builds anyway).
bool ensure_mask_level(const TileSet& set, const MaskInputs& in, const std::string& dir,
                       const std::string& ddc, bool force, MaskBuildResult& out,
                       std::string& error);

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

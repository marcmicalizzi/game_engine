#pragma once

// **An authored terrain** (docs/subsystems/tileset.md): an authored heightfield
// (`engine.terrain.heightfield/1`, a JSON header beside a raw float32 grid), the tile set built
// from it — its finest tiles at the heightfield's spacing with a point of apron, and its far mip
// chain, each level the surface box-filtered over each point's own cell — and the reader that
// answers the renderer's and the collision's tile source (`scene_gen::TileSource`) from it. The
// `tiles` ground provider (`k_ground_provider`) opens a set from a scene's terrain entry.
//
// **Integer millimetres from the build on.** The float32 source becomes whole millimetres once,
// when the set is built (`quantize_heights`: the unit times `vertical_mm_per_unit`, the scale and
// the offset in f64, rounded half up), and everything after is integers: the tiles hold i32
// millimetres, the box filter sums them exactly in i64 and rounds once, and the reader turns a
// millimetre into a float32 metre by one correctly rounded division. So a set is the same bytes on
// every toolchain, and two readers of it the same floats.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/scene_gen/tile_source.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <schemas/tileset.h>
#include <span>
#include <string>
#include <string_view>

namespace engine::jobs {
class JobSystem;
}

namespace engine::scene_gen {
class GroundProvider;
}

namespace engine::tileset {

inline constexpr std::string_view k_heightfield_format = "engine.terrain.heightfield";
inline constexpr u32 k_heightfield_version = 1;
inline constexpr std::string_view k_tileset_format = "engine.tileset";
// Bumped whenever a build's output for the same source and options changes, so a set built by an
// older build is never mistaken for this one's.
inline constexpr u32 k_tileset_version = 1;
// The name the ground provider registers under (scene_gen.md, "The registrar pattern").
inline constexpr std::string_view k_ground_provider = "tiles";
// The file a set's manifest is, in its directory.
inline constexpr std::string_view k_manifest_name = "manifest.json";

inline constexpr u32 k_max_heightfield_side = 65536;
inline constexpr i64 k_max_spacing_mm = 1'000'000;

// ---- the source ---------------------------------------------------------------------------------

// An authored heightfield in memory: `width * height` grid units, rows of x in order of z.
struct Heightfield {
  u32 width = 0;
  u32 height = 0;
  i64 spacing_mm = 1000;
  i64 origin_x_mm = 0;
  i64 origin_z_mm = 0;
  f64 vertical_mm_per_unit = 1000.0;
  Vector<f32> data;
};

// Writes `field` as `json_path` (the header) and its grid beside it, `<stem>.f32`, with the grid's
// hash in the header. False, with a sentence, for a field that is not one or a file not written.
bool write_heightfield(const std::string& json_path, const Heightfield& field, std::string& error);
// Reads a header and its grid, checking the format, the sizes, the spacing, the origin's alignment
// and the grid's hash. `source_hash` is `hash_bytes` over the header's bytes as read — which hold
// the grid's hash — the key every build of it starts from.
bool read_heightfield(const std::string& json_path, Heightfield& out, u64& source_hash,
                      std::string& error);

// The analytic shapes `engine-content terrain-synth` writes, for tests and for a person who wants a
// terrain without a DEM. Polynomials and integer hashes only (no libm), so a seed is the same grid
// on every toolchain.
enum class SynthKind : u8 { ridge, basin, steps, flat };
bool parse_synth_kind(std::string_view name, SynthKind& out) noexcept;
// `size` points a side, `spacing_mm` apart, point (0, 0) at the origin given, metres (units of
// 1000 mm). A non-zero `seed` adds a quarter-metre of seeded roughness.
Heightfield synth_heightfield(SynthKind kind, u32 size, i64 spacing_mm, u64 seed = 0,
                              i64 origin_x_mm = 0, i64 origin_z_mm = 0);

// ---- the build ----------------------------------------------------------------------------------

struct BuildOptions {
  // Points a tile side less one: a tile file holds `cells + 3` points a side.
  u32 cells = 64;
  // The far levels a renderer will ask (renderer.md, "Ground to the horizon"): the first far
  // level's spacing is `far_ratio` times the outermost ring's, and each one after doubles it.
  i64 ring_spacing_mm = 2000;
  u32 far_ratio = 4;
  u32 far_levels = 6;
  // Applied to every height before it becomes whole millimetres; the offset moves the grid's
  // origin (x, z: whole spacings) and adds to every height (y).
  f64 vertical_scale = 1.0;
  i64 offset_x_mm = 0;
  i64 offset_y_mm = 0;
  i64 offset_z_mm = 0;
};

// The chain's spacings, finest first. False, with a sentence, for options with no chain to build.
bool chain_spacings(const BuildOptions& options, Vector<i64>& out, std::string* error = nullptr);
// The build key: the source's hash, every option and `k_tileset_version`.
u64 tileset_key(u64 source_hash, const BuildOptions& options) noexcept;
// "<ddc>/tilesets/<key as 16 lower-case hex digits>": where a set built into the cache lives.
std::string tileset_cache_dir(std::string_view ddc, u64 key);

// The source's heights as whole millimetres after the options' scale and offset, rows of x in
// order of z: what the finest level holds. False, with a sentence naming the point, for a height
// that is not finite or does not fit an i32 millimetre (2,147 km).
bool quantize_heights(const Heightfield& field, const BuildOptions& options, Vector<i32>& out,
                      std::string& error);

struct BuildResult {
  std::string manifest;  // the manifest's path
  u64 key = 0;
  bool hit = false;  // the set was already there: nothing was written
  u32 levels = 0;
  u32 tiles = 0;
  u64 bytes = 0;  // of the tiles written (or, on a hit, recorded)
  u64 hash = 0;   // every level's hash combined
};

// Builds `field` (whose header hashed to `source_hash`) into `dir`: a directory a level of tile
// files and `manifest.json` written last, so a set whose manifest is there is whole.
bool build_tileset(const Heightfield& field, u64 source_hash, std::string_view source_name,
                   const BuildOptions& options, const std::string& dir, BuildResult& out,
                   std::string& error);

// **The content step as a library call**: reads `heightfield_json`, and builds its set into `dir`
// — or, with `dir` empty, into the derived-data cache under `ddc` at `tileset_cache_dir` — unless
// the manifest there already records this build's key (a hit; `force` builds anyway).
bool ensure_tileset(const std::string& heightfield_json, const BuildOptions& options,
                    const std::string& dir, const std::string& ddc, bool force, BuildResult& out,
                    std::string& error);

// ---- the reader ---------------------------------------------------------------------------------

// **A built tile set, read back** (docs/subsystems/tileset.md, "The reader"). Tiles are read on
// first ask and kept; a reader is asked from several jobs at once (tile_source.h), so a tile's
// read is behind a lock and, once read, never moves. Not movable: `source()` points at it.
class TileSet {
 public:
  TileSet();
  ~TileSet();
  TileSet(const TileSet&) = delete;
  TileSet& operator=(const TileSet&) = delete;

  // Opens `manifest_path`: its format, version and levels checked, no tile read yet.
  bool open(const std::string& manifest_path, std::string& error);
  bool is_open() const noexcept { return !levels_.empty(); }
  const TileSetManifest& manifest() const noexcept { return manifest_; }

  // **The set as a tile source**: at `filter_mm` 0, the ground at each point of any spacing — the
  // finest's own points where the spacing is a multiple of its own, its bilinear surface between
  // them; at a filter, the chain level of that spacing, point for point, and **a filter it was
  // not built with is refused** (false). Past the data the edge's heights are held: flat beyond
  // it, at every level. No time: every time is the one surface.
  scene_gen::TileSource source() const noexcept;
  bool heights(i64 spacing_mm, i64 filter_mm, i32 i0, i32 j0, u32 nx, u32 nz, u32 block_begin,
               u32 block_end, std::span<f32> out) const noexcept;
  // Whether a lattice of `spacing_mm` filtered to it is one of the chain's.
  bool has_filter(i64 spacing_mm) const noexcept;

  // **The ground at a point in whole millimetres**: the finest heights' bilinear surface, the edge
  // held past them — what placement and a camera key stand on. `point_mm` is the millimetres, in
  // f64; `height_mm` the float32 metres, one division of it.
  f64 point_mm(i64 x_mm, i64 z_mm) const noexcept;
  f32 height_mm(i64 x_mm, i64 z_mm) const noexcept;

  // Tiles read so far, and reads that failed (a tile missing or not the manifest's).
  u32 reads() const noexcept { return reads_.load(std::memory_order_relaxed); }
  u32 failures() const noexcept { return failures_.load(std::memory_order_relaxed); }

  struct Level;

 private:
  const i32* tile(const Level& level, i32 tx, i32 tz) const noexcept;
  i32 value(const Level& level, i64 i, i64 j) const noexcept;

  TileSetManifest manifest_;
  std::string dir_;
  Vector<std::unique_ptr<Level>> levels_;
  mutable std::mutex mutex_;
  mutable std::atomic<u32> reads_{0};
  mutable std::atomic<u32> failures_{0};
};

// The tile file's name inside a set's directory: "<spacing_mm>/<x>_<z>.tile".
std::string tile_file(i64 spacing_mm, i32 tx, i32 tz);

// **The set behind a `tiles` ground** (the capability's typed accessor, scene_gen.md): null for a
// ground another provider made. A host hands `tile_set_of(provider)->source()` to the renderer
// and the collision, which refuses a filter the set lacks — the provider's own `tiles()` goes
// through its `grid`, which cannot refuse (docs/subsystems/tileset.md, "What the seam lacks").
const TileSet* tile_set_of(const scene_gen::GroundProvider& provider) noexcept;

}  // namespace engine::tileset

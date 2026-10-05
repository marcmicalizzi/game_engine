#pragma once

// Scene generators (docs/subsystems/scene_gen.md; ADR-0046): the registry the scene reader and the
// world ring look generators up in by the name a scene gives, and the two kinds a generator is.
//
// **Why a registry, and why here.** A scene names what generates part of it — its terrain's ground
// (`engine.scene.Terrain.provider`: the renderer's waves, the terrain capability's dunes) and its
// placements (`engine.scene.Scene.ruins`, `placements`: the ruins capability, the city) — and two
// modules that are not capabilities act on those names: the renderer's scene reader expands a
// scene, and the world capability's ring streams it a tile at a time. Before this module each
// generator reached them through an `#if` and a dependency declared only where the capability was
// configured (ADR-0037), which is the surface ADR-0027 decision 1 forbids and which grew by one per
// generator. So a generator registers a constant-initialized descriptor from its own source, a
// host links the capabilities its configuration has, and nothing below the host names one: the
// reader and the ring find a generator by name here, and refuse a name this executable does not
// carry with a sentence (`unknown_ground`, `unknown_placement`).
//
// **Two kinds, one order.** A *ground provider* is a function of position and time — a height, the
// floor a building stands on, a grid of itself at a spacing and the same grid at another time — and
// optionally the rings round a camera and the per-tile state a world keeps. A *placement generator*
// is a function of the scene entry, the world seed and a tile — `expand` the whole entry, `tile`
// one tile of it, which must agree — and its output is instances of named meshes with a tag. Ground
// is what placements stand on, so it is built first, and placements follow in the scene's order,
// each seeing the ground and nothing of another generator (the `Context`): every generator stays a
// function of (entry, seed, tile, ground), which is the per-tile determinism the ruins and the city
// rest on.
//
// **What it is not.** It carries no state beyond the table of descriptors, adds no seam for one
// capability to reach another, and costs nothing per frame: a lookup by name and a call through a
// function pointer per scene read or tile activation. A provider's per-point height is a call
// through a pointer too, which is why the grids — the mesh's, the time-lapse's, a ring's — are one
// call a block rather than one a point.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/math/math.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/scene_gen/tile_source.h>

#include <schemas/scene.h>
#include <span>
#include <string>
#include <string_view>

namespace engine::jobs {
class JobSystem;
}

namespace engine::scene_gen {

// A tile of the world's grid: tile (x, z) covers [x, x + 1) * tile_size in world x and z (y up) —
// the cell the world ring, `ruins::TileCoord` and `city::TileCoord` name at the same size.
struct TileCoord {
  i32 x = 0;
  i32 z = 0;
  constexpr bool operator==(const TileCoord&) const = default;
};

// ---- the ground a placement stands on -----------------------------------------------------------

// A height in metres at (x, z): a plain function and a context, because a placement generator asks
// it a few dozen times a building and is in the domain layer, below whoever made the ground.
using HeightFn = f32 (*)(const void* context, f32 x, f32 z) noexcept;

// The view of the scene's ground a placement generator is handed (`Context::ground`): the surface
// at the scene's time, and the **floor** — what a building stands on, the ground that does not move
// while the surface does (the dunes migrate over the interdune floor and bury what stands there;
// with the waves the two are the same). No surface is flat ground at 0: a scene with no terrain.
struct Ground {
  HeightFn surface_fn = nullptr;
  HeightFn floor_fn = nullptr;  // null: the surface
  const void* context = nullptr;
  bool present() const noexcept { return surface_fn != nullptr; }
  f32 surface(f32 x, f32 z) const noexcept {
    return surface_fn != nullptr ? surface_fn(context, x, z) : 0.0f;
  }
  f32 floor(f32 x, f32 z) const noexcept {
    return floor_fn != nullptr ? floor_fn(context, x, z) : surface(x, z);
  }
};

// ---- what a generator sees ----------------------------------------------------------------------

// Everything a generator is handed besides its entry, and nothing of any other generator (ADR-0046
// decision 4). A generator that wants another's output asks the ground it is handed, never the
// registry.
struct Context {
  // The scene's seed: its terrain's `seed`, 0 for a scene with none. An entry with a seed of its
  // own (a ruins scatter's) uses that; this is the one a generator with none of its own draws from.
  u64 world_seed = 0;
  // The world's tile edge, metres: a streamed world's ring tiles (`engine.scene.WorldRings`), which
  // a `tile` call's `TileCoord` is on and an entry's own grid must match. 32 outside a world.
  f32 tile_size = 32.0f;
  // What an entry's relative paths resolve against: the scene file's directory, or empty.
  std::string_view dir;
  // Where the entry is, for a sentence: "scene.json: ruins 0".
  std::string_view where;
  Ground ground;
  // A streamed world's block (its rings, innermost first, and what each draws) and the ring a
  // `tile` call is for: what a generator with a representation per ring (the ruins' blocks near,
  // sections far) decides it by. Null and `k_no_ring` outside a streamed world, and for `expand`.
  const scene::WorldRings* world = nullptr;
  u8 ring = 0xFFu;
  // The job system's performance pool, made on first ask (a read of a few tiles never starts one):
  // `jobs()` is null when the caller offers none. The output of every generator is the same on any
  // number of threads.
  jobs::JobSystem* (*jobs_fn)(void* context) = nullptr;
  void* jobs_context = nullptr;
  jobs::JobSystem* jobs() const { return jobs_fn != nullptr ? jobs_fn(jobs_context) : nullptr; }
};
inline constexpr u8 k_no_ring = 0xFFu;

// ---- placements ---------------------------------------------------------------------------------

// A mesh a placement instances: a path the reader resolves through the derived-data cache as it
// does any scene mesh (glTF, GLB or `.clusters`), the name a summary knows it by, and the content
// hash its bytes must have (0: unchecked).
struct PlacementMesh {
  std::string path;
  std::string name;
  u64 hash = 0;
};

// One instance: a mesh, its world transform, and a tag the generator gives it and the host may read
// (the ruins' 1 is rubble: a debris member or a fallen block, what engine-view's handover pass
// splits a pop by). A mesh's materials are its own, as every scene instance's are. 48 bytes.
struct Placement {
  u32 mesh = 0;
  Transform3 transform{};
  u8 tag = 0;
};

// What a generator made. `expand` fills `meshes` with what its instances index; `tile` leaves it
// empty and indexes the list its `meshes` entry gave, which a streamed scene holds resident.
struct Placements {
  Vector<PlacementMesh> meshes;
  Vector<Placement> instances;
  // The things made — buildings — for a host's summary.
  u32 things = 0;
  // For `tile`: which representation it drew the tile in, the generator's own numbering (the
  // ruins': 0 blocks, 1 sections, 2 walls), what a streaming host counts its work by.
  u8 kind = 0;
  void clear() noexcept {
    meshes.clear();
    instances.clear();
    things = 0;
    kind = 0;
  }
};

// A placement generator (ADR-0046 decision 1): a function of its scene entry, the world seed, a
// tile and the ground. Every entry point but `name`, `open`, `close` and `expand` may be null — a
// generator with no `tile` cannot be streamed, and a scene that streams it is refused.
struct PlacementGeneratorDesc {
  const char* name = nullptr;
  // Reads what the entry names (its kits, its plan) and checks its parameters, once: the state
  // every other call runs against. `params` is the entry's JSON
  // (`engine.scene.PlacementEntry.params`, or a legacy field's object). False, with a sentence and
  // nothing to close, for an entry the generator cannot run.
  bool (*open)(const JsonValue& params, const Context& context, void** state,
               std::string* error) = nullptr;
  void (*close)(void* state) noexcept = nullptr;
  // The whole entry, standing on `context.ground`: a scene read whole.
  bool (*expand)(void* state, const Context& context, Placements& out,
                 std::string* error) = nullptr;
  // What a streamed scene holds resident for `tile` to name, in the order `tile`'s placements index
  // it. Read once, when the scene is.
  bool (*meshes)(void* state, const Context& context, Vector<PlacementMesh>& out,
                 std::string* error) = nullptr;
  // One tile of it in `context.ring`: the whole expansion restricted to the tile, in that ring's
  // representation. Empty for a tile the entry puts nothing on.
  bool (*tile)(void* state, TileCoord tile, const Context& context, Placements& out,
               std::string* error) = nullptr;
  // Whether the entry puts anything on `tile`: what a host asks before it treats a ring change as a
  // handover. Null: every tile might.
  bool (*occupies)(const void* state, TileCoord tile) noexcept = nullptr;
  // The representation `ring` draws, the generator's own numbering: a tile moving between two
  // rings of one representation is not built again. Null: every ring its own.
  u8 (*representation)(const void* state, const Context& context) noexcept = nullptr;
};

// ---- ground -------------------------------------------------------------------------------------

// **A lattice** a grid of the ground is sampled on (renderer.md, "The dunes in time-lapse"): the
// scene's own terrain grid, its coordinates computed exactly as the renderer's mesh computes them —
// `-extent + 2 extent (i / (size - 1))` in f32 — so a grid on it is the mesh's heights to the bit;
// or the world's grid at a spacing in millimetres counted from the world's origin (terrain.md,
// "Rings"), so a ring that moves samples the points it did.
struct Lattice {
  f64 origin_x = 0.0;  // metres of lattice point (0, 0)
  f64 origin_z = 0.0;
  f64 spacing = 1.0;  // metres between points
  bool scene_grid = false;
  f32 extent = 0.0f;
  u32 size = 0;
  i64 spacing_mm = 0;
  // **The width a point stands for**, mm (renderer.md, "Ground to the horizon"): 0, the ground at
  // the point; otherwise the ground as a lattice this coarse can carry it — what of it varies over
  // much more than this width at the point, and what varies faster than that as its mean, so a
  // coarse lattice neither aliases nor shimmers as the ground moves under it. A provider with
  // nothing finer than its lattices can carry answers the point either way.
  i64 filter_mm = 0;
  // A point's coordinate in metres, as float32: what a provider's float arithmetic (the fixed
  // features, a height function of metres) is handed. Exact for the world's lattice only while a
  // float holds the millimetre — to 16 km for any spacing, and to 2^24 spacings for a spacing that
  // is a power-of-two fraction of a metre (4,000 km at 25 cm) — so it is never the way to a
  // lattice point's place.
  f32 x(i32 i) const noexcept;
  f32 z(i32 j) const noexcept;
  // **A point's place in whole millimetres from the world's origin** (ADR-0053): for the world's
  // lattice `i * spacing_mm`, exact for every index; for a scene grid the nearest millimetre of
  // its float coordinate, which is what that coordinate always meant. What a provider samples its
  // field at, so a lattice index never reaches the field through a float32 metre.
  i64 x_mm(i64 i) const noexcept;
  i64 z_mm(i64 j) const noexcept;
};
Lattice scene_lattice(f32 extent, u32 size) noexcept;
Lattice ring_lattice(i64 spacing_mm, i64 filter_mm = 0) noexcept;

// The nearest whole millimetre to a coordinate in metres, halves up: the world's lattice is
// integer millimetres, and a position reaches it in f64 (ADR-0053). The terrain's own `to_mm`
// rounds a float32 metre the same way.
i64 nearest_mm(f64 metres) noexcept;

// A grid is cut into 64 x 64 blocks of lattice points, and a provider evaluates a range of them per
// call, so a caller hands the blocks to as many jobs as it likes and gets the same bytes: blocks
// run in rows of x, in order of z.
inline constexpr u32 k_height_block = 64;
u32 window_blocks(u32 nx, u32 nz) noexcept;

// ---- the rings round a camera -------------------------------------------------------------------

// The most rings a ground's ring set has, the scene's own grid counted as its outer ring.
inline constexpr u32 k_max_rings = 4;

// One ring, millimetres: half the square's side, its grid's spacing, how deep its skirts hang, and
// the side of the world-aligned chunks it is built in.
struct RingSpec {
  i64 half_mm = 0;
  i64 spacing_mm = 0;
  i64 skirt_mm = 0;
  i64 chunk_mm = 0;
};
// Where each ring is, innermost first, millimetres.
struct RingPlace {
  i64 cx = 0;
  i64 cz = 0;
  i64 half = 0;
};
struct RingsLayout {
  u32 count = 0;
  RingPlace ring[k_max_rings];
};
// One chunk of one ring as last built: its world chunk indices, the key that decides its mesh
// besides the heights (equal keys, equal meshes), where its skirt vertices start, and its DAG,
// which the caller may move out of.
struct RingChunkRef {
  i32 i = 0;
  i32 j = 0;
  u64 key = 0;
  u32 grid_vertices = 0;
  geometry::ClusterLodMesh* lod = nullptr;
};
// Where a ring's heights come from: an `nx` x `nz` grid from (x0, z0) `spacing_mm` apart, µm, rows
// of x in order of z. The caller's — the renderer reads them from its newest field where it covers
// the chunk and evaluates them where it does not.
struct RingHeights {
  void (*fn)(const void* context, i64 x0, i64 z0, u32 nx, u32 nz, i64 spacing_mm,
             jobs::JobSystem* jobs, Vector<i64>& out_um) = nullptr;
  const void* context = nullptr;
};
struct GroundRingsOps {
  void (*destroy)(void* rings) noexcept = nullptr;
  u32 (*count)(const void* rings) noexcept = nullptr;  // the scene's grid included, as the last
  RingSpec (*spec)(const void* rings, u32 ring) noexcept = nullptr;  // innermost first
  // Lays the rings out round the camera and builds those in `build_mask` (bit k, ring k).
  bool (*reset)(void* rings, i64 camera_x, i64 camera_z, const RingHeights& heights,
                const geometry::ClusterLodOptions& options, jobs::JobSystem* jobs, u32 build_mask,
                std::string* error) = nullptr;
  // The re-centre rule for the camera's new position: `rebuilt` is a mask of the rings rebuilt.
  bool (*update)(void* rings, i64 camera_x, i64 camera_z, jobs::JobSystem* jobs, u32& rebuilt,
                 std::string* error) = nullptr;
  void (*layout)(const void* rings, RingsLayout& out) noexcept = nullptr;
  // Where the re-centre rule puts the rings for a camera at (x, z) when they stand at `from`: it
  // reads only the ring parameters, so it may be asked while `update` runs on another thread.
  void (*next_layout)(const void* rings, i64 camera_x, i64 camera_z, const RingsLayout& from,
                      RingsLayout& out) noexcept = nullptr;
  u32 (*chunk_count)(const void* rings, u32 ring) noexcept = nullptr;
  RingChunkRef (*chunk)(void* rings, u32 ring, u32 index) noexcept = nullptr;
  u32 (*last_built)(const void* rings) noexcept = nullptr;
  u32 (*last_reused)(const void* rings) noexcept = nullptr;
};

// A ground's rings, owned: the provider's state behind a table of plain functions.
class GroundRings {
 public:
  GroundRings() noexcept = default;
  GroundRings(const GroundRingsOps* ops, void* state) noexcept : ops_(ops), state_(state) {}
  ~GroundRings() { reset(); }
  GroundRings(GroundRings&& other) noexcept : ops_(other.ops_), state_(other.state_) {
    other.ops_ = nullptr;
    other.state_ = nullptr;
  }
  GroundRings& operator=(GroundRings&& other) noexcept;
  GroundRings(const GroundRings&) = delete;
  GroundRings& operator=(const GroundRings&) = delete;
  void reset() noexcept;

  bool valid() const noexcept { return ops_ != nullptr; }
  u32 count() const noexcept { return ops_->count(state_); }
  RingSpec spec(u32 ring) const noexcept { return ops_->spec(state_, ring); }
  bool build(i64 camera_x, i64 camera_z, const RingHeights& heights,
             const geometry::ClusterLodOptions& options, jobs::JobSystem* jobs, u32 build_mask,
             std::string* error) {
    return ops_->reset(state_, camera_x, camera_z, heights, options, jobs, build_mask, error);
  }
  bool update(i64 camera_x, i64 camera_z, jobs::JobSystem* jobs, u32& rebuilt, std::string* error) {
    return ops_->update(state_, camera_x, camera_z, jobs, rebuilt, error);
  }
  void layout(RingsLayout& out) const noexcept { ops_->layout(state_, out); }
  void next_layout(i64 camera_x, i64 camera_z, const RingsLayout& from,
                   RingsLayout& out) const noexcept {
    ops_->next_layout(state_, camera_x, camera_z, from, out);
  }
  u32 chunk_count(u32 ring) const noexcept { return ops_->chunk_count(state_, ring); }
  RingChunkRef chunk(u32 ring, u32 index) noexcept { return ops_->chunk(state_, ring, index); }
  u32 last_built() const noexcept { return ops_->last_built(state_); }
  u32 last_reused() const noexcept { return ops_->last_reused(state_); }

 private:
  const GroundRingsOps* ops_ = nullptr;
  void* state_ = nullptr;
};

// ---- the ground's tiles in a world --------------------------------------------------------------

// Where a ground keeps a tile's record between activations (the world's store, when it has one):
// bytes under the tile, whatever the ground makes of them.
struct TileRecords {
  void* context = nullptr;
  bool (*load)(void* context, TileCoord tile, Vector<u8>& out) = nullptr;  // false: none
  bool (*save)(void* context, TileCoord tile, std::span<const u8> bytes) = nullptr;
  void (*erase)(void* context, TileCoord tile) = nullptr;
};

// The tiles of a ground a world ring holds (world.md, "The consumers"): a tile built when the ring
// takes it in at the ring's resolution, kept while it is held, its record written when it goes.
struct GroundTilesOps {
  void (*destroy)(void* tiles) noexcept = nullptr;
  bool (*activate)(void* tiles, TileCoord tile, u8 ring) = nullptr;
  bool (*change_ring)(void* tiles, TileCoord tile, u8 ring) = nullptr;
  void (*deactivate)(void* tiles, TileCoord tile) = nullptr;
  // The tiles built or rebuilt since the last call, in the order they were.
  void (*take_built)(void* tiles, Vector<TileCoord>& out) = nullptr;
  // The game time the next activations are built at, µs. Never backwards.
  void (*set_time)(void* tiles, i64 time_us) = nullptr;
};

class GroundTiles {
 public:
  GroundTiles() noexcept = default;
  GroundTiles(const GroundTilesOps* ops, void* state) noexcept : ops_(ops), state_(state) {}
  ~GroundTiles() { reset(); }
  GroundTiles(GroundTiles&& other) noexcept : ops_(other.ops_), state_(other.state_) {
    other.ops_ = nullptr;
    other.state_ = nullptr;
  }
  GroundTiles& operator=(GroundTiles&& other) noexcept;
  GroundTiles(const GroundTiles&) = delete;
  GroundTiles& operator=(const GroundTiles&) = delete;
  void reset() noexcept;

  bool valid() const noexcept { return ops_ != nullptr; }
  bool activate(TileCoord tile, u8 ring) { return ops_->activate(state_, tile, ring); }
  bool change_ring(TileCoord tile, u8 ring) { return ops_->change_ring(state_, tile, ring); }
  void deactivate(TileCoord tile) { ops_->deactivate(state_, tile); }
  void take_built(Vector<TileCoord>& out) { ops_->take_built(state_, out); }
  void set_time(i64 time_us) { ops_->set_time(state_, time_us); }
  // For the capability that made it, which alone knows what the state is (its typed accessor).
  const GroundTilesOps* ops() const noexcept { return ops_; }
  void* state() const noexcept { return state_; }

 private:
  const GroundTilesOps* ops_ = nullptr;
  void* state_ = nullptr;
};

// ---- a ground provider --------------------------------------------------------------------------

// What one ground provider is, made for one scene's terrain: a table of plain functions over the
// state its `make` allocated. `height` is required; everything else may be null.
struct GroundOps {
  void (*destroy)(void* state) noexcept = nullptr;
  // The surface at (x, z) at the ground's own time (the entry's), metres. Thread-safe.
  f32 (*height)(const void* state, f32 x, f32 z) noexcept = nullptr;
  // What a building stands on (`Ground::floor`). Null: the surface.
  f32 (*floor)(const void* state, f32 x, f32 z) noexcept = nullptr;
  // `nx * nz` heights at its own time on lattice points (i0 + i, j0 + j), rows of x in order of z:
  // what the renderer builds the scene's mesh from. Null: `height` at each point.
  void (*grid)(const void* state, const Lattice& lattice, i32 i0, i32 j0, u32 nx, u32 nz,
               std::span<f32> heights) noexcept = nullptr;
  // The same window at another game time, blocks [begin, end) of it (`k_height_block`): the
  // time-lapse's re-evaluation. Null for a ground with no time.
  bool (*evaluate)(const void* state, f64 time_s, const Lattice& lattice, i32 i0, i32 j0, u32 nx,
                   u32 nz, u32 block_begin, u32 block_end,
                   std::span<f32> heights) noexcept = nullptr;
  // How far the ground's fastest feature travels between two game times, metres: what a moving
  // ground's cadence is timed by — of the features a lattice of `filter_mm` carries (`Lattice::
  // filter_mm`; 0: every feature), since the mean a coarser one stands in for the rest with does
  // not move. Null: 0.
  f64 (*travel_m)(const void* state, f64 from_s, f64 to_s, i64 filter_mm) noexcept = nullptr;
  // The wind the ground's surface detail lies across at game time `time_s`: the direction the sand
  // moves over (x, z), a unit vector, continuous in time — what a renderer turns the ripples it
  // draws by (docs/subsystems/renderer.md, "The sand close up"). False, and nothing written, for a
  // ground with no wind to say. Thread-safe.
  bool (*wind)(const void* state, f64 time_s, f32& x, f32& z) noexcept = nullptr;
  // What moves the ripples at game time `time_s`: `moved_m2`, the sand the ground's wind has
  // carried across a metre of width since its epoch, m^2 (a path length, so it only grows; and
  // continuous in time, never rounded to a unit, since the ripples are drawn from it every frame
  // and a rounded one holds them still and then jumps them), and
  // `strength`, the wind's strength over its record's mean. The renderer turns the first into the
  // ripples' travel and flattens them by the second (renderer.md, "Ripples that move"). Null, or
  // false, for a ground with no transport to say. Thread-safe.
  bool (*transport)(const void* state, f64 time_s, f64& moved_m2, f32& strength) noexcept = nullptr;
  // The rings round a camera over a scene grid `extent_mm` either side at `spacing_mm` (terrain.md,
  // "Rings"). Null: this ground has none.
  bool (*make_rings)(const void* state, i64 extent_mm, i64 spacing_mm, GroundRings& out,
                     std::string* error) = nullptr;
  // Its tiles in a world whose tiles are `tile_mm`, their records kept in `records` (whose context
  // must outlive them). Null: a world streams nothing of this ground.
  bool (*open_tiles)(const void* state, const TileRecords& records, i64 tile_mm, GroundTiles& out,
                     std::string* error) = nullptr;
  // What the store files a tile's record under (a schema type's name), with `open_tiles`.
  const char* record = nullptr;
  // **The surface at a point given in whole millimetres** from the world's origin, at game time
  // `time_s` — a ground that does not move answers its one surface whatever the time (ADR-0053;
  // scene_gen.md, "Far from the origin"). What a walker asks between lattice points, anywhere: the
  // place is integers, so it never passes through a float32 metre, and it is not a lattice index,
  // so it does not overflow one (a 1 mm lattice's i32 index ends at 2,147 km). The same heights
  // as `evaluate` on a 1 mm lattice at that point. Null: `GroundProvider::height_mm` falls back
  // (below).
  f32 (*height_mm)(const void* state, f64 time_s, i64 x_mm, i64 z_mm) noexcept = nullptr;
};

class GroundProvider {
 public:
  GroundProvider() noexcept = default;
  GroundProvider(const GroundOps* ops, void* state) noexcept : ops_(ops), state_(state) {}
  ~GroundProvider() { reset(); }
  GroundProvider(GroundProvider&& other) noexcept : ops_(other.ops_), state_(other.state_) {
    other.ops_ = nullptr;
    other.state_ = nullptr;
  }
  GroundProvider& operator=(GroundProvider&& other) noexcept;
  GroundProvider(const GroundProvider&) = delete;
  GroundProvider& operator=(const GroundProvider&) = delete;
  void reset() noexcept;

  bool valid() const noexcept { return ops_ != nullptr; }
  f32 height(f32 x, f32 z) const noexcept { return ops_->height(state_, x, z); }
  // The surface at (x_mm, z_mm) at `time_s` (`GroundOps::height_mm`). A provider without the entry
  // answers through `evaluate` on a 1 mm lattice where its i32 index reaches (2,147 km) and the
  // ground moves, and through `height` at the point's float32 metres otherwise — exact only while a
  // float holds the millimetre, which is why the dunes have the entry.
  f32 height_mm(f64 time_s, i64 x_mm, i64 z_mm) const noexcept;
  f32 floor(f32 x, f32 z) const noexcept {
    return ops_->floor != nullptr ? ops_->floor(state_, x, z) : ops_->height(state_, x, z);
  }
  void grid(const Lattice& lattice, i32 i0, i32 j0, u32 nx, u32 nz,
            std::span<f32> heights) const noexcept;
  bool moves() const noexcept { return ops_ != nullptr && ops_->evaluate != nullptr; }
  bool evaluate(f64 time_s, const Lattice& lattice, i32 i0, i32 j0, u32 nx, u32 nz, u32 block_begin,
                u32 block_end, std::span<f32> heights) const noexcept {
    return ops_->evaluate != nullptr &&
           ops_->evaluate(state_, time_s, lattice, i0, j0, nx, nz, block_begin, block_end, heights);
  }
  f64 travel_m(f64 from_s, f64 to_s, i64 filter_mm = 0) const noexcept {
    return ops_->travel_m != nullptr ? ops_->travel_m(state_, from_s, to_s, filter_mm) : 0.0;
  }
  // The wind the surface detail lies across at `time_s` (`GroundOps::wind`); false for a ground
  // that says none.
  bool wind(f64 time_s, f32& x, f32& z) const noexcept {
    return ops_ != nullptr && ops_->wind != nullptr && ops_->wind(state_, time_s, x, z);
  }
  // The ripples' transport at `time_s` (`GroundOps::transport`).
  bool transport(f64 time_s, f64& moved_m2, f32& strength) const noexcept {
    return ops_ != nullptr && ops_->transport != nullptr &&
           ops_->transport(state_, time_s, moved_m2, strength);
  }
  bool has_rings() const noexcept { return ops_ != nullptr && ops_->make_rings != nullptr; }
  bool make_rings(i64 extent_mm, i64 spacing_mm, GroundRings& out, std::string* error) const;
  bool has_tiles() const noexcept { return ops_ != nullptr && ops_->open_tiles != nullptr; }
  bool open_tiles(const TileRecords& records, i64 tile_mm, GroundTiles& out,
                  std::string* error) const;
  const char* record() const noexcept { return ops_ != nullptr ? ops_->record : nullptr; }
  // The view a placement generator stands on (`Context::ground`), valid while this lives.
  Ground view() const noexcept;
  // **This ground as a tile source** (tile_source.h; scene_gen.md, "The tile source"): the heights
  // of a window of the world's lattice through `evaluate` at the time asked for, or, for a ground
  // that does not move, through `grid` at its own time whatever the time; and `travel_m` when it
  // moves. The provider's heights to the bit. Valid while this lives and is not moved.
  TileSource tiles() const noexcept;
  // For the capability that made it, which alone knows what the state is (its typed accessor).
  const GroundOps* ops() const noexcept { return ops_; }
  const void* state() const noexcept { return state_; }

 private:
  const GroundOps* ops_ = nullptr;
  void* state_ = nullptr;
};

// A ground provider (ADR-0046 decision 1): made from a scene's terrain entry. `make` checks the
// entry and says why it cannot be drawn with a sentence (no path: the caller adds where it is).
struct GroundProviderDesc {
  const char* name = nullptr;
  bool (*make)(const scene::Terrain& entry, const Context& context, GroundProvider& out,
               std::string* error) = nullptr;
  // What every ground this provider makes can do, without making one (`k_ground_*`): what a host
  // decides a setting by before it has a scene's sampler (a time-lapse needs a ground that moves).
  u32 flags = 0;
};
inline constexpr u32 k_ground_moves = 1u << 0;  // `evaluate` and `travel_m`
inline constexpr u32 k_ground_rings = 1u << 1;  // `make_rings`
inline constexpr u32 k_ground_tiles = 1u << 2;  // `open_tiles`

// ---- the registry -------------------------------------------------------------------------------

// The third kind, a sky provider (sky.h; ADR-0048): what a scene's sky is made of and where its
// lights are at a game time. Declared there, registered here like the other two.
struct SkyProviderDesc;

// Every generator this executable carries, by kind and name: the descriptors themselves, which are
// constant-initialized in their capabilities' sources and never copied. Filled before `main` by
// `Registrar`s; read from `main` on.
class GeneratorRegistry {
 public:
  GeneratorRegistry() = default;
  GeneratorRegistry(const GeneratorRegistry&) = delete;
  GeneratorRegistry& operator=(const GeneratorRegistry&) = delete;

  static GeneratorRegistry& global();

  // False, and nothing changes, when another descriptor of the kind has the name — a second
  // generator under one name would make what a scene means depend on link order, which is what a
  // registry exists to rule out — or the descriptor is incomplete. The same descriptor again is
  // true: a library linked into two images of one process registers once per image.
  bool add(const GroundProviderDesc& desc) noexcept;
  bool add(const PlacementGeneratorDesc& desc) noexcept;
  bool add(const SkyProviderDesc& desc) noexcept;
  const GroundProviderDesc* find_ground(std::string_view name) const noexcept;
  const PlacementGeneratorDesc* find_placement(std::string_view name) const noexcept;
  const SkyProviderDesc* find_sky(std::string_view name) const noexcept;
  // The names of a kind, sorted: the same list whatever order the registrars ran in.
  Vector<std::string_view> ground_names() const;
  Vector<std::string_view> placement_names() const;
  Vector<std::string_view> sky_names() const;

  // The sentences a reader refuses a name this executable does not carry with, naming it and the
  // ones it does carry.
  std::string unknown_ground(std::string_view name) const;
  std::string unknown_placement(std::string_view name) const;
  std::string unknown_sky(std::string_view name) const;

 private:
  Vector<const GroundProviderDesc*> grounds_;
  Vector<const PlacementGeneratorDesc*> placements_;
  Vector<const SkyProviderDesc*> skies_;
};

// A capability's registration, at namespace scope in its own source beside the descriptor it adds:
//
//   constexpr scene_gen::PlacementGeneratorDesc k_ruins{.name = "ruins", .open = ..., ...};
//   const scene_gen::Registrar k_ruins_registrar{k_ruins};
//
// The descriptor is constant-initialized; the registrar adds it to `GeneratorRegistry::global()`
// during static initialization, and a descriptor whose name another holds stops the program with a
// message, because that is a build that links two generators under one name. A module holding a
// registrar is `WHOLE_ARCHIVE`, or the linker drops the object nothing references and the
// registration with it.
struct Registrar {
  explicit Registrar(const GroundProviderDesc& desc) noexcept;
  explicit Registrar(const PlacementGeneratorDesc& desc) noexcept;
  explicit Registrar(const SkyProviderDesc& desc) noexcept;
};

}  // namespace engine::scene_gen

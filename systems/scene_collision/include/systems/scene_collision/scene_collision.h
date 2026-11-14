#pragma once

// scene_collision capability (ADR-0027; docs/subsystems/scene_collision.md; plan 05 §5.11):
// **static collision bodies from the scene, round a walker.** A world consumer (world.h,
// `TileConsumer`) that, for every tile its ring holds, keeps two static bodies in a
// `physics::World`: the **ground** as a heightfield sampled from the scene's ground provider
// (`scene_gen::GroundProvider`) at a collision spacing, and the **placements** — what the scene's
// placement generators put on the tile, the ruins' walls and rubble — as one static compound of
// each piece's proxy. A `physics::CharacterBody` then walks on them. It builds nothing the
// renderer draws (collision_draw.h draws what it holds, for a look); it links no generator, finding
// each in the scene-generator registry by the name the scene gives, as the world's placements
// consumer does.
//
// **Where the placements come from.** A streamed scene (`renderer::SceneData::streamed`) keeps its
// placement entries for the world, and this consumer opens each entry's generator again and asks it
// for the tile (`scene_gen::PlacementGeneratorDesc::tile`) in the representation the scene's inner
// ring draws — what is drawn round the walker. A scene read whole has its placements among its
// instances already, so there a tile's placements are the instances whose bounds reach into it (the
// generator's `tile` is the whole read restricted to the tile, so this is the same set, and it also
// gives the scene's own meshes a body).
//
// **A proxy per mesh, never the full-detail mesh.** Every instance of one mesh at one scale shares
// one shape: the mesh's own cluster LOD DAG cut at `mesh_error_m` (5 cm) — the coarsest level that
// stays within that of the surface. It keeps a doorway a doorway, which a convex hull or a box
// would fill; it is a handful of triangles for a block or a kit-of-boxes wall and a few thousand
// for a scanned ruin section, where the leaves would be half a million.
//
// **Determinism.** The bodies a walker meets are a function of the ring's events, and the ring's
// events of the observer sequence (world.md): a host that updates this consumer's ring at fixed
// ticks from the walker's own position creates and destroys the same bodies in the same order and
// the physics world hands out the same body ids, which the character's contact order depends on.
// So a recorded walk replays bit for bit on still ground. A ground the time-lapse moves is
// refreshed on the renderer's schedule (`set_ground_time`), which a live window's display drives,
// and a walk over moving sand replays against the sand the replay draws.
//
// **Far from the origin** (ADR-0053; docs: "Far from the origin"). A tile's bodies stand at the
// tile's corner, a `WorldPos`, and hold their shapes in the tile's own frame: the ground a field of
// heights from the corner, the placements a compound of pieces placed from it. A height is asked
// for at a `WorldPos` and found on the lattice in integer millimetres, so neither the walker's feet
// nor a lattice point passes through a float32 metre, and a tile 10,000 km out collides as one by
// the origin does.
//
//   [-] schema types      none: nothing here is persistent; the bodies are rebuilt from the scene
//   [-] scheduler entry   none: the ring updates between ticks, as every world consumer's does
//   [x] render passes     the collision debug draw, a frame pass a host registers
//   (collision_draw.h)
//   [-] derived data      none: a proxy is cut from the DAG the scene already loaded, in
//   microseconds
//   [-] protocol methods  none yet: nothing over `render.*` walks
//   [x] tunables          walk.collision.* (below)
//   [x] LOD policy        the ring: bodies exist only for the tiles round the walker (one ring)
//   [x] determinism       k_determinism below
//   [x] zero cost unused  no linked code without it; a host that never walks makes no consumer
//   [x] docs, tests, size table, bench

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/flat_map.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/physics/physics.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/world/tile_ring.h>
#include <systems/world/world.h>

#include <memory>
#include <mutex>
#include <schemas/scene.h>
#include <span>
#include <string>

namespace engine::geometry {
struct ClusterLodMesh;
struct ClusterMeshPart;
}  // namespace engine::geometry
namespace engine::renderer {
struct SceneData;
struct SourceMesh;
struct PlacementMeshData;
class UncutGround;
}  // namespace engine::renderer
namespace engine::physics {
class CharacterBody;
}
namespace engine::jobs {
class JobSystem;
}

namespace engine::scene_collision {

// Determinism stance (ADR-0010): `hashed`. The bodies are what a walker's state is a function of,
// and they are a function of the ring's events (above).
inline constexpr const char* k_determinism = "hashed";

// ---- the tunables (`walk.collision.*`), read once by the host that makes the consumer -----------

// The ground heightfield's sample spacing, metres: 1 (a 32 m tile is 34 x 34 samples).
f32 spacing_tunable() noexcept;
// The ring's radius in tiles: 1.5 (the 3 x 3 tiles round the walker's own at a tile's centre).
f32 radius_tunable() noexcept;
// Tiles activated per update, the budget: 2; deactivations twice that.
u32 max_tiles_tunable() noexcept;
// How far a placement's proxy may stand off its mesh, metres: 0.05.
f32 mesh_error_tunable() noexcept;
// How far the collision ground may stand off the drawn one under a time-lapse before its tile is
// refreshed, metres: 0.05.
f32 ground_error_tunable() noexcept;
// Ground tiles refreshed per update under a time-lapse: 2.
u32 max_refreshes_tunable() noexcept;
// How far past the collision ring a tile's ground is built ahead on the job system, in tiles: 1
// (docs: "The ground built ahead of the ring"). 0 builds nothing ahead.
f32 prefetch_tunable() noexcept;

// How far the ground under the walker may stand off the drawn ground under a time-lapse before its
// tile is rebuilt, metres (docs: "The ground moves"). A millimetre: the step a standing walker's
// eye then takes is a pixel at most on the owner's 11520 x 2160, where `ground_error_m`'s 5 cm was
// the jump the owner saw about once a second at 600 game seconds a real second (2026-09-29), and
// the sand at the game's own rate still rebuilds nothing but that tile, a few times in ten minutes.
// Not a tunable: a smaller one buys nothing the eye can see and rebuilds the tile every frame.
inline constexpr f32 k_underfoot_error_m = 0.001f;

struct Config {
  f32 tile_size = 32.0f;
  f32 spacing_m = 1.0f;
  f32 mesh_error_m = 0.05f;
  f32 ground_error_m = 0.05f;
  u32 max_refreshes = 2;
  // **The ground built ahead of the ring** (docs): tiles whose centres are within this many tiles
  // of the walker have their heights built on the job system `create` was handed, before the ring
  // activates them; 0 (or no job system) builds nothing ahead. A host sets it to its ring's radius
  // plus `prefetch_tunable()`. Changes no body: the heights are the same call's either way.
  f32 prefetch_radius_tiles = 0.0f;
  // **Keep what is held for a debug draw** (collision_draw.h; docs: "Seeing the collision"): every
  // proxy's triangles (with each triangle's flora part) and every held tile's pieces stay on the
  // CPU beside the backend's shapes, for `held_tile` and `held_piece`. Off, nothing is kept and
  // those report the ground alone. Changes no body.
  bool keep_shapes = false;
};
Config config_from_tunables(f32 tile_size) noexcept;
// The consumer's ring: one ring of `radius_tunable()` tiles, the world's hysteresis, the budget.
world::RingParams ring_params_from_tunables(f32 tile_size) noexcept;

// **The ground as it is drawn now** (renderer.md, "The dunes in time-lapse"): a still ground, or a
// pair of fields at two game times and the blend between them — the finest terrain level's, which
// is what is drawn round the walker. The collision ground is the same pair and the same blend on
// its own lattice, so it follows the drawn sand, cross-fade and all, rather than the true surface
// the drawing approximates.
struct GroundTime {
  bool moving = false;
  f64 time_a = 0.0;  // absolute game seconds
  f64 time_b = 0.0;
  f64 blend = 0.0;  // 0 at a, 1 at b
};

struct Stats {
  // Now.
  u32 tiles = 0;       // held
  u32 bodies = 0;      // live: a heightfield a tile, a compound a tile with placements
  u32 placements = 0;  // pieces in the compounds
  u32 proxies = 0;     // shapes made from meshes, each shared by every instance of its mesh
  // Of those, the proxies of meshes a tile handed (docs: "A tile's meshes"): resident while a
  // held tile or a build ahead names them, so this falls when the walker leaves.
  u32 tile_proxies = 0;
  u32 proxy_triangles = 0;
  // A tree's triangles a proxy left out because a corner is of a frond, a leaf, a curtain or a twig
  // (scene_collision.md, "What gets no body").
  u32 flora_triangles_left_out = 0;
  // The scene's far-forest instances left out of the bins (docs: "What gets no body"): one a world
  // tile the load merged its streamed trees' coarsest forms into, which is drawn only where the
  // ring's tail does not draw the tile's trees (renderer.md, "The far forest").
  u32 far_forest_left_out = 0;
  u64 ground_samples = 0;  // heights held on the CPU, beside the backend's own copy
  // What the backend holds (`physics::World::shape_memory`): the live tiles' heightfields and
  // compounds (a compound's own tree and child table; its pieces are the proxies), and every proxy.
  u64 ground_bytes = 0;
  u64 compound_bytes = 0;
  u64 proxy_bytes = 0;
  // Over the consumer's life.
  u32 max_bodies = 0;
  u32 max_placements = 0;
  u64 activated = 0;
  u64 deactivated = 0;
  u64 bodies_created = 0;
  u64 bodies_destroyed = 0;
  u64 refreshes = 0;          // ground bodies rebuilt for the time-lapse
  u64 field_evaluations = 0;  // tile fields evaluated at another time
  u64 failures = 0;           // a generator or the backend refused a tile
  // How far the collision heights under the walker (the tile holding the point `refresh` is
  // given) stood off the drawn heights at its samples, metres: the largest a rebuild found (just
  // past `k_underfoot_error_m`, by at most what the drawn sand moved since the last update), and
  // the largest the rule left standing (under it).
  f64 max_stale_m = 0.0;
  f64 max_kept_m = 0.0;
  // What `follow` did to the walker (docs: "The walker goes with the ground"): the ticks it was
  // carried with the ground it stood on and the most in one, and the times the ground had risen
  // past its feet and it was lifted onto it, and the most.
  u64 carries = 0;
  f64 max_carry_m = 0.0;
  u64 lifts = 0;
  f64 max_lift_m = 0.0;
  i64 activate_ns = 0;
  i64 max_activate_ns = 0;
  // Where an activation's wall time goes (docs: "What an activation costs"): the ground's heights
  // asked of the tile source (`ground_sample_ns`, the most once), the placements gathered and their
  // proxies and compound made (`placement_ns`), and the shapes and bodies the physics world made
  // for them (`body_ns`).
  i64 ground_sample_ns = 0;
  i64 max_ground_sample_ns = 0;
  i64 placement_ns = 0;
  i64 body_ns = 0;
  // The ground built ahead (docs: "The ground built ahead of the ring"): builds submitted, and of
  // the activations that found one, those it had finished (`prefetched`), those whose build was
  // running so the tick waited for that tile alone (`prefetch_waits`, the wait's total and most),
  // and those whose build had not started so the tick built it itself (`prefetch_inline`); builds
  // let go unused when the walker turned away.
  u64 prefetch_submitted = 0;
  u64 prefetched = 0;
  u64 prefetch_waits = 0;
  i64 prefetch_wait_ns = 0;
  i64 max_prefetch_wait_ns = 0;
  u64 prefetch_inline = 0;
  u64 prefetch_dropped = 0;
  // What the worker spent on the builds ahead (docs: "The placements built ahead"), off every
  // tick: the ground's heights, and the placements — generators' tiles, proxies cut and built, the
  // compound built. An activation that took a finished build spends none of it.
  i64 ahead_ground_ns = 0;
  i64 ahead_placement_ns = 0;
  // Activations whose placements came from a build ahead (finished or waited for).
  u64 placements_ahead = 0;
  i64 deactivate_ns = 0;
  i64 refresh_ns = 0;
  i64 max_refresh_ns = 0;
  i64 proxy_ns = 0;
};

// **What a held tile holds, for a debug draw** (collision_draw.h). Spans are the consumer's and
// stay valid until its next update.
inline constexpr u32 k_no_index = 0xFFFFFFFFu;
struct HeldTile {
  world::TileCoord coord;
  WorldPos corner{};  // where its compound stands: the tile's corner at y 0
  // The ground's lattice: `n` x `n` heights from the point (i0, j0) at `spacing_m`, row-major (z
  // rows of x), what its heightfield was built with; n 0 for a tile with no ground body. A sample a
  // terrain's hole took out is `lattice_hole(i0 + i, j0 + j)`.
  i64 i0 = 0;
  i64 j0 = 0;
  u32 n = 0;
  f64 spacing_m = 1.0;
  std::span<const f32> heights;
  u32 pieces = 0;  // `held_piece`'s count: 0 unless `Config::keep_shapes`
};
// **What a cast met** (`describe_hit`; docs: "What a module can ask"): the ground of a held tile,
// or one piece of its placements — a scene mesh's instance (`instance`) or a streamed entry's
// placement (`entry`; `mesh` its scene mesh, or none for a mesh the tile handed).
enum class HitKind : u8 { none = 0, ground = 1, piece = 2 };
struct HitInfo {
  HitKind kind = HitKind::none;
  u32 mesh = k_no_index;
  u32 instance = k_no_index;
  u32 entry = k_no_index;
};

// **A place a small animal can stand** (`supports_near`; docs: "Supports"). `kind` is 1 a perch the
// mesh declares, 2 a limb or branch of its structural table, 3 the top of its bounds — the
// order a piece's supports come from — and `part` the limb's id in the mesh's motion table (the
// perch's index among the mesh's perches; `k_no_index` for a top).
inline constexpr u8 k_support_perch = 1;
inline constexpr u8 k_support_limb = 2;
inline constexpr u8 k_support_top = 3;
struct Support {
  WorldPos position{};
  Vec3 up{0.0f, 1.0f, 0.0f};
  f32 size_m = 0.0f;
  f32 distance = 0.0f;  // from the point asked
  u8 kind = 0;
  HitInfo piece;
  u32 part = k_no_index;
};
struct SupportAnswer {
  u32 count = 0;         // written, nearest first
  u32 found = 0;         // within the radius in all
  bool covered = false;  // the whole disc lay over held tiles
};

struct HeldPiece {
  u32 mesh = 0;               // the scene mesh
  u32 instance = k_no_index;  // the loaded instance it is (a bin's), or none (a streamed placement)
  u32 entry = k_no_index;     // the streamed entry that placed it, or none (a bin's)
  Vec3 offset{};              // from the tile's corner, metres
  Quat rotation{};
  // The proxy, in the mesh's frame at the instance's scale; `parts` the flora part of each
  // triangle's first corner (0 for a mesh with no motion contract).
  std::span<const Vec3> vertices;
  std::span<const u32> indices;
  std::span<const u32> parts;
};

class SceneCollision {
 public:
  SceneCollision();
  ~SceneCollision();
  ENGINE_NON_COPYABLE(SceneCollision);

  // Over `scene`'s placements and `ground`'s surface (null: no ground bodies — a scene with no
  // terrain), into `physics`. Every argument must outlive the consumer. For a streamed scene it
  // opens each streamed entry's generator (and refuses one this executable does not carry, or one
  // with no tiles, with the registry's sentence); for a scene read whole it bins the instances by
  // the tiles their bounds reach. False, with `error`, when it cannot.
  //
  // **The ground's heights come from a tile source** (scene_gen/tile_source.h; ADR-0050): `tiles`
  // when given — the one the renderer draws the world's tiles from, so what is walked on is what is
  // drawn — or `ground` seen as tiles (`GroundProvider::tiles()`), which is its own heights to the
  // bit. `ground` still stands the placements: a generator is handed its floor.
  //
  // `jobs`, when given with `Config::prefetch_radius_tiles`, builds tiles' ground ahead of the
  // ring (`prefetch`); it must outlive the consumer.
  bool create(physics::World& physics, const renderer::SceneData& scene,
              const scene_gen::GroundProvider* ground, const Config& config,
              std::string* error = nullptr, const scene_gen::TileSource* tiles = nullptr,
              jobs::JobSystem* jobs = nullptr);
  // The row to register: after the scene's ground and placements consumers where the world has
  // them (world.md, "The order"), acting in ring 0.
  world::TileConsumer consumer() noexcept;
  const Config& config() const noexcept { return config_; }

  // The drawn ground's time, before an update: tiles activated after it are evaluated there.
  void set_ground_time(const GroundTime& time) noexcept { time_ = time; }
  const GroundTime& ground_time() const noexcept { return time_; }
  // Keeps the held ground tiles on the drawn ground (docs: "The ground moves"): a tile whose drawn
  // pair of fields changed has the new field evaluated, and a tile whose heights stand off the
  // drawn ones at some sample — the pair blended as the renderer blends it — has its body rebuilt
  // at the drawn blend. **The tiles under the walker** — the one holding (x, z) and any within
  // `reach` of it, which a capsule of that radius can touch — are rebuilt once they stand more
  // than `k_underfoot_error_m` off, at every update and outside the budget, so the ground a walker
  // stands on follows the drawn sand a frame at a time; the others once they stand more than
  // `ground_error_m` off, at most `max_refreshes` of them an update, nearest first. Returns the
  // tiles worked. Nothing moves on still ground, and this returns 0 there without looking. The
  // walker is at `at`, whose y is not read.
  u32 refresh(WorldPos at, f32 reach = 0.0f);
  // **The walker goes with the ground** (docs: "The walker goes with the ground"): `refresh` round
  // the walker's feet, within its capsule's radius, and then the walker moved with what that did
  // under it, before its step. Standing on the ground — on a held tile's heightfield, not on a
  // placement — it is carried straight up or down by the change of the collision ground under its
  // feet, so it rises and sinks with the drawn sand and keeps the offset it stood at; and wherever
  // it is, **a walker is never below the collision ground after a refresh**: feet the ground has
  // risen past are lifted onto it. Both put it there with `physics::CharacterBody::teleport`, which
  // leaves it standing with no velocity: the vertical speed of a fall or a jump the sand caught is
  // dropped, as a landing drops it, and the horizontal speed is the input's, which every step sets
  // anew. On still ground this is `refresh` and nothing else. Returns the tiles worked.
  u32 follow(physics::CharacterBody& walker);
  // **The tile built ahead of the ring** (docs: "The ground built ahead of the ring", "The
  // placements built ahead"): every tile whose centre is within `Config::prefetch_radius_tiles` of
  // `at` and is neither held nor already built has its ground's heights and its placements — the
  // generators' tile or the scene's bins, the proxies' shapes, the compound's shape — built on the
  // job system, so the tick that activates it takes them ready. Every body is still made in the
  // activation, on the tick and in the ring's order, so body ids — and a recorded walk — are what
  // they were. A build of a tile the ring activates before it is done is waited for (running) or
  // built on the tick (not started): one tile's cost at most, never the others'. Only where the
  // ground is still and read through a tile source, or there is none; under a time-lapse nothing
  // is built ahead. Builds left behind by the walker are let go, and every shape a build made that
  // no tile took is released with it. Call before the ring's update, once a tick.
  void prefetch(WorldPos at);

  // The collision ground's height under `at` (its y is not read), from the heights the tile holding
  // it was built with, interpolated across the cell as the backend's heightfield triangulates it.
  // The cell and the fractions across it are found in millimetres from the tile's lattice window,
  // so on a lattice point the answer is the tile source's height to the bit, anywhere. False where
  // no held tile covers the point, and over a terrain's hole (W87): a cell with a corner the hole
  // took out of the heightfield.
  bool ground_height(WorldPos at, f32& out) const noexcept;

  // ---- what a module can ask (docs: "What a module can ask"; roadmap A43) ----
  // **What a cast met**: the held tile whose ground or placements `body` is, and for placements the
  // piece the compound's `child` is (`physics::RayHit::child`). `none` for a body this consumer
  // does not hold.
  HitInfo describe_hit(physics::BodyId body, u32 child) const noexcept;
  // Whether a held tile covers (x, z), and whether held tiles cover every point of the segment from
  // `a` to `b` in x and z (walked a tile at a time).
  bool covers(WorldPos at) const noexcept;
  bool covers_segment(WorldPos a, WorldPos b) const noexcept;
  // **The supports within `radius` of `at`** over the held tiles' pieces, at least `min_size`
  // large, the nearest `out.size()` of them into `out`, nearest first, ties by piece order (docs:
  // "Supports"). A piece's are the perches its mesh declares, else the parts of its structural
  // table (a tree's limbs and branches, each an axis with a radius) that lie within 60 degrees of
  // level, at the point of each nearest the asker, else the top of its
  // bounds — found by a ray down the bounds' top centre onto the piece itself, kept where that
  // surface is within 40 degrees of level. Reads the held tiles and casts into `physics`; changes
  // nothing and allocates nothing.
  SupportAnswer supports_near(WorldPos at, f32 radius, f32 min_size,
                              std::span<Support> out) const noexcept;
  const Stats& stats() const noexcept { return stats_; }
  // **What is held** (docs: "Seeing the collision"): the held tiles, in the consumer's order, and a
  // tile's pieces where `Config::keep_shapes` kept them. False past the count.
  u32 held_tiles() const noexcept { return tiles_.size(); }
  bool held_tile(u32 index, HeldTile& out) const noexcept;
  bool held_piece(u32 tile, u32 piece, HeldPiece& out) const noexcept;
  // Whether a terrain's hole took lattice sample (i, j) out of the ground (W87).
  bool lattice_hole(i64 i, i64 j) const noexcept { return hole_sample(i, j); }
  // Whether a streamed scene's generators, rather than the scene's own instances, give the tiles
  // their placements.
  bool streamed() const noexcept { return !entries_.empty(); }

 private:
  struct Entry;
  struct Build;
  // A proxy is built once, on whichever thread first asks for it, and shared for the consumer's
  // life by every compound of every tile (each holds the backend's reference): its address is
  // stable, so a compound being built on a worker can name it while the tick adds others.
  // A support in the proxy's frame (the mesh's, at the proxy's scale): a perch (`a` its point,
  // `up`), or a limb's axis from `a` to `b` with its radius `r0` to `r1`. A top is the bounds'.
  struct LocalSupport {
    u8 kind = 0;
    u32 part = k_no_index;
    Vec3 a{};
    Vec3 b{};
    Vec3 up{0.0f, 1.0f, 0.0f};
    f32 r0 = 0.0f;
    f32 r1 = 0.0f;
  };
  struct Proxy {
    physics::BuiltShape shape;
    u32 triangles = 0;
    // With `Config::keep_shapes`: the triangles the shape was built from, and each one's part.
    Vector<Vec3> vertices;
    Vector<u32> indices;
    Vector<u32> parts;
    // **Its supports** (docs: "Supports"), always: the bounds of the triangles the shape was built
    // from, and the perches or the limbs, in the proxy's frame. Empty supports: its top.
    Vec3 lo{};
    Vec3 hi{};
    Vector<LocalSupport> supports;
  };
  // One piece of a held tile, with `Config::keep_shapes`.
  struct PieceRecord {
    u32 mesh = 0;
    u32 instance = k_no_index;
    u32 entry = k_no_index;
    Vec3 offset{};
    Quat rotation{};
    const Proxy* proxy = nullptr;
  };
  // The proxy of a mesh a tile handed, at a scale: resident by reference (`refs`: the held tiles
  // and the builds ahead that name it), gone with the last.
  struct TileProxy {
    Proxy proxy;
    u32 refs = 0;
    u64 bytes = 0;
  };
  // The proxies' counters, kept under the proxies' lock and copied into `Stats` on the tick.
  struct ProxyStats {
    u32 proxies = 0;
    u32 triangles = 0;
    u32 flora_left_out = 0;
    u64 bytes = 0;
    i64 ns = 0;
  };
  // What one thread's placements build uses, reused: the tick has one, a build ahead its own.
  struct PlacementScratch {
    Vector<physics::BuiltChild> children;
    Vector<Vec3> vertices;
    Vector<u32> indices;
    Vector<u32> parts;
    FlatMap<u32, u32> remap;
    scene_gen::Placements placed;
    Vector<PieceRecord> records;  // beside `children`, with `Config::keep_shapes`
    // The last mesh a tile handed that this thread built (`renderer::load_placement_mesh`), kept
    // so a tile's two placements of one mesh at two scales build it once.
    std::unique_ptr<renderer::PlacementMeshData> tile_mesh;
    u64 tile_mesh_identity = 0;
    bool tile_mesh_ok = false;
  };
  // A tile's placements as built (`gather_placements`): its compound's shape, not yet in the
  // physics world's table, and what it holds.
  struct TilePlacements {
    physics::BuiltShape compound;
    u32 pieces = 0;
    u64 compound_bytes = 0;
    bool ok = true;               // false: a generator refused the tile or the backend a shape
    Vector<PieceRecord> records;  // with `Config::keep_shapes`
    // The tile-handed proxies this gather took a reference to (`tile_proxies_`' keys), each once:
    // moved to the tile with its body, released by `release_tile_proxies` when either goes.
    Vector<u64> tile_proxies;
  };
  struct Tile {
    world::TileCoord coord;
    physics::BodyId ground;
    physics::ShapeId ground_shape;
    physics::BodyId placements;
    physics::ShapeId placements_shape;
    u32 pieces = 0;
    u64 ground_bytes = 0;
    u64 compound_bytes = 0;
    // The ground's lattice window and its heights: what the body was built with (`heights`), and,
    // under a time-lapse, the two fields it blends and the blend it was built at. The window's
    // first point is i64: the tile source takes an i32 index, which at the collision's spacing s
    // reaches 2^31 s (2.1e9 m at a metre), and a tile past that is refused rather than wrapped.
    i64 i0 = 0;
    i64 j0 = 0;
    u32 n = 0;
    Vector<f32> heights;
    Vector<f32> field_a;
    Vector<f32> field_b;
    f64 time_a = 0.0;
    f64 time_b = 0.0;
    f64 blend = 0.0;
    bool moving = false;
    // Its compound's pieces in child order, always (a cast's `child` and the supports read them;
    // `held_piece` too, with `Config::keep_shapes`).
    Vector<PieceRecord> pieces_held;
    Vector<u64> tile_proxies;  // the references its compound's pieces hold
  };
  // The supports a proxy carries, from its source mesh at its scale, and its bounds from the
  // triangles in `scratch` (`proxy_for`, `tile_proxy_for`).
  static void fill_supports(const renderer::SourceMesh* source, Vec3 scale,
                            const PlacementScratch& scratch, Proxy& proxy);
  // One piece's supports into the answer (`supports_near`).
  void piece_supports(const Tile& tile, u32 piece, WorldPos at, f32 radius, f32 min_size,
                      std::span<Support> out, SupportAnswer& answer) const noexcept;

  static bool activate(void* context, const world::TileEvent& event);
  static void deactivate(void* context, const world::TileEvent& event);
  static void commit(void* context);
  // The tile's ground body, from `build`'s heights when it built them.
  bool build_ground(Tile& tile, Build* build);
  // The tile's lattice window (`build_ground`); false for one past the source's i32 index.
  bool ground_window(world::TileCoord coord, i64& i0, i64& j0, u32& n) const noexcept;
  bool can_prefetch() const noexcept;
  // Whether a tile can hold placements at all: a streamed scene's entries or a whole read's bins.
  bool has_placements() const noexcept { return !entries_.empty() || bins_.size() != 0; }
  // The tile's build ahead, when there is one, finished: taken ready, waited for while a worker
  // runs it, or run here when it had not started. Null when the tile has none.
  std::unique_ptr<Build> take_build(world::TileCoord coord);
  void retire(std::unique_ptr<Build>&& build);
  bool ground_body(physics::BodyId body) const noexcept;
  bool pair_fields(Tile& tile);
  f64 stale_of(const Tile& tile) const noexcept;
  bool evaluate_field(const Tile& tile, f64 time, Vector<f32>& out);
  void blend_heights(Tile& tile, f64 blend);
  bool make_ground_body(Tile& tile);
  // The terrain's holes on the collision lattice (W87): sample (i, j) is none, and whether any hole
  // reaches the tile's window.
  bool hole_sample(i64 i, i64 j) const noexcept;
  bool holes_reach(const Tile& tile) const noexcept;
  void drop_ground_body(Tile& tile);
  // The tile's compound body, from `build`'s placements when it built them; on the tick.
  bool build_placements(Tile& tile, Build* build);
  // **Thread-safe** (docs: "The placements built ahead"): the tile's placements gathered — each
  // streamed entry's generator asked for the tile under the generators' lock, or the whole read's
  // bin — their proxies found or built under the proxies' lock, and their compound's shape built,
  // all into `out` and `scratch`; nothing of the physics world's table or of `Stats` is touched.
  void gather_placements(world::TileCoord coord, PlacementScratch& scratch, TilePlacements& out);
  // Where the tile's bodies stand: its corner at y 0 (`coord` times the tile's millimetres).
  WorldPos tile_corner(world::TileCoord coord) const noexcept;
  // One piece of the tile's compound: `linear`'s rotation and scale (its translation column is not
  // read) standing at `at`, placed in the compound relative to the tile's `corner`.
  bool add_piece(u32 mesh, const Mat4& linear, WorldPos at, WorldPos corner,
                 PlacementScratch& scratch, u32 instance, u32 entry);
  // A far-forest mesh (`renderer::SceneData::far_forest`): drawn only where the tail is not.
  bool far_mesh(u32 mesh) const noexcept {
    return mesh < far_meshes_.size() && far_meshes_[mesh] != 0;
  }
  // The proxy of a mesh at a scale, built on first ask; null for a mesh with no surface to stand
  // on. Thread-safe.
  const Proxy* proxy_for(u32 mesh, Vec3 scale, PlacementScratch& scratch);
  // The cut both kinds of proxy are made of: `part`'s clusters of `lod` where each cluster's own
  // error is within the threshold and its parent's is not, welded by source vertex and scaled,
  // into `scratch.vertices`, `.indices` and `.parts`. `tree`: a mesh with the motion contract,
  // which keeps its wood. Returns the triangles left out for it.
  u32 cut_proxy(const geometry::ClusterLodMesh& lod, const geometry::ClusterMeshPart& part,
                const renderer::SourceMesh* tree, Vec3 scale, PlacementScratch& scratch) const;
  // **A tile-handed mesh's proxy** (docs: "A tile's meshes"): keyed by the mesh's identity
  // (`renderer::placement_mesh_identity`) and the scale, built from the recipe through
  // `renderer::load_placement_mesh` — no device, the cache entry the renderer reads — outside the
  // proxies' lock, and referenced once per gather in `held`. Null for a mesh that will not build
  // or has no surface. Thread-safe.
  const Proxy* tile_proxy_for(const scene_gen::PlacementMesh& mesh, Vec3 scale,
                              PlacementScratch& scratch, Vector<u64>& held);
  // Gives back what a gather took: a proxy nobody else names goes, with its shape. Thread-safe.
  void release_tile_proxies(Vector<u64>& held) noexcept;
  // One piece of a tile-handed mesh, as `add_piece` for a scene mesh.
  bool add_tile_piece(const scene_gen::PlacementMesh& mesh, const Mat4& linear, WorldPos at,
                      WorldPos corner, PlacementScratch& scratch, Vector<u64>& held, u32 entry);
  scene_gen::Context context_for(const Entry& entry) const noexcept;
  void note_bodies() noexcept;

  physics::World* physics_ = nullptr;
  const renderer::SceneData* scene_ = nullptr;
  const scene_gen::GroundProvider* ground_ = nullptr;
  // Where the ground's heights come from (`create`), and the time a still one stands at.
  scene_gen::TileSource source_;
  bool explicit_source_ = false;  // a host's source, rather than the ground seen as tiles
  f64 own_time_s_ = 0.0;
  Config config_;
  i64 spacing_mm_ = 1000;
  i64 tile_mm_ = 32000;
  GroundTime time_;
  scene::WorldRings world_rings_;
  // The ground before its cuts, for a generator that plans them (`scene_gen::Context::uncut`).
  std::unique_ptr<renderer::UncutGround> uncut_;
  scene_gen::Context::UncutGround uncut_hook_;
  Vector<std::unique_ptr<Entry>> entries_;
  // A scene read whole: for every tile its instances reach, their indices.
  FlatMap<u64, Vector<u32>> bins_;
  Vector<u8> far_meshes_;  // by scene mesh, 1 for a far-forest tile's mesh
  // By (mesh, scale), under `proxies_mutex_`: a worker building a tile ahead and the tick building
  // one itself may ask at once.
  FlatMap<u64, std::unique_ptr<Proxy>> proxies_;
  // By (identity, scale), under the same lock: the proxies of meshes that arrived with a tile.
  FlatMap<u64, std::unique_ptr<TileProxy>> tile_proxies_;
  ProxyStats proxy_stats_;
  std::mutex proxies_mutex_;
  // A generator's state is its own and may be scratch it writes (the ruins' assemblers), so its
  // `tile` is asked by one thread at a time.
  std::mutex generators_mutex_;
  FlatMap<u64, Tile> tiles_;
  Stats stats_;
  // Scratch, reused: the tick's placements, and the refresh's order.
  PlacementScratch tick_scratch_;
  Vector<u64> order_;
  // The ground built ahead: a build a tile, and builds let go whose job has yet to run.
  jobs::JobSystem* jobs_ = nullptr;
  Vector<std::unique_ptr<Build>> builds_;
  Vector<std::unique_ptr<Build>> retired_;
  WorldPos prefetched_at_{};
  bool prefetched_once_ = false;
};

}  // namespace engine::scene_collision

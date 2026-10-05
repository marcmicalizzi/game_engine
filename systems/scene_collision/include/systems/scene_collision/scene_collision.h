#pragma once

// scene_collision capability (ADR-0027; docs/subsystems/scene_collision.md; plan 05 §5.11):
// **static collision bodies from the scene, round a walker.** A world consumer (world.h,
// `TileConsumer`) that, for every tile its ring holds, keeps two static bodies in a
// `physics::World`: the **ground** as a heightfield sampled from the scene's ground provider
// (`scene_gen::GroundProvider`) at a collision spacing, and the **placements** — what the scene's
// placement generators put on the tile, the ruins' walls and rubble — as one static compound of
// each piece's proxy. A `physics::CharacterBody` then walks on them. It builds nothing the
// renderer draws and draws nothing; it links no generator, finding each in the scene-generator
// registry by the name the scene gives, as the world's placements consumer does.
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
//   [-] render passes     none: collision is not drawn
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
#include <schemas/scene.h>
#include <span>
#include <string>

namespace engine::renderer {
struct SceneData;
}
namespace engine::physics {
class CharacterBody;
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
  u32 proxy_triangles = 0;
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
  i64 deactivate_ns = 0;
  i64 refresh_ns = 0;
  i64 max_refresh_ns = 0;
  i64 proxy_ns = 0;
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
  bool create(physics::World& physics, const renderer::SceneData& scene,
              const scene_gen::GroundProvider* ground, const Config& config,
              std::string* error = nullptr, const scene_gen::TileSource* tiles = nullptr);
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

  // The collision ground's height under `at` (its y is not read), from the heights the tile holding
  // it was built with, interpolated across the cell as the backend's heightfield triangulates it.
  // The cell and the fractions across it are found in millimetres from the tile's lattice window,
  // so on a lattice point the answer is the tile source's height to the bit, anywhere. False where
  // no held tile covers the point.
  bool ground_height(WorldPos at, f32& out) const noexcept;
  const Stats& stats() const noexcept { return stats_; }
  // Whether a streamed scene's generators, rather than the scene's own instances, give the tiles
  // their placements.
  bool streamed() const noexcept { return !entries_.empty(); }

 private:
  struct Entry;
  struct Proxy {
    physics::ShapeId shape;
    u32 triangles = 0;
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
  };

  static bool activate(void* context, const world::TileEvent& event);
  static void deactivate(void* context, const world::TileEvent& event);
  static void commit(void* context);
  bool build_ground(Tile& tile);
  bool ground_body(physics::BodyId body) const noexcept;
  bool pair_fields(Tile& tile);
  f64 stale_of(const Tile& tile) const noexcept;
  bool evaluate_field(const Tile& tile, f64 time, Vector<f32>& out);
  void blend_heights(Tile& tile, f64 blend);
  bool make_ground_body(Tile& tile);
  void drop_ground_body(Tile& tile);
  bool build_placements(Tile& tile);
  // Where the tile's bodies stand: its corner at y 0 (`Tile::coord` times the tile's millimetres).
  WorldPos tile_corner(const Tile& tile) const noexcept;
  bool add_piece(u32 mesh, const Mat4& world, WorldPos corner,
                 Vector<physics::CompoundChild>& children);
  bool proxy_for(u32 mesh, Vec3 scale, physics::ShapeId& out);
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
  Vector<std::unique_ptr<Entry>> entries_;
  // A scene read whole: for every tile its instances reach, their indices.
  FlatMap<u64, Vector<u32>> bins_;
  FlatMap<u64, Proxy> proxies_;  // by (mesh, scale)
  FlatMap<u64, Tile> tiles_;
  Stats stats_;
  // Scratch, reused.
  Vector<physics::CompoundChild> children_;
  Vector<Vec3> proxy_vertices_;
  Vector<u32> proxy_indices_;
  FlatMap<u32, u32> proxy_remap_;
  scene_gen::Placements placed_;
  Vector<u64> order_;
};

}  // namespace engine::scene_collision

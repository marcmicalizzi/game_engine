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
#include <domain/geometry/cluster_file.h>
#include <domain/physics/physics.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/scene.h>
#include <systems/scene_collision/proxy_source.h>
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
// **Built ahead of a running walker** (T166; docs: "Ahead of a running walker"): the workers a host
// gives the builds ahead (4), how many seconds of the walker's own velocity the builds look ahead
// along (4 s, at most `k_lead_max_tiles`), and the bytes of tile-handed meshes' copies kept between
// builds (64 MiB; 0 reads a tile's meshes for every build, as before T166). None changes a body.
u32 prefetch_workers_tunable() noexcept;
f32 lead_tunable() noexcept;
u64 mesh_copies_tunable() noexcept;
// The farthest the lead reaches along the walker's velocity, in tiles: past it the builds of a
// fast flight are let go before any worker reaches them (docs: "Ahead of a running walker").
inline constexpr f32 k_lead_max_tiles = 4.0f;

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
  // **Ahead of a running walker** (T166; docs): the ring's own radius in tiles, which says when the
  // ring will want a tile (0: `prefetch_radius_tiles` less one); the seconds of the walker's
  // velocity (`prefetch`'s) the builds also look along; and how many builds are handed to the job
  // system at once — the most urgent first, by when the ring will want each — beyond the tiles the
  // ring wants now, which are always handed over (0: every build as it is found, the rule before
  // T166, in tile order). A host sets the slots to a small multiple of its workers. None changes a
  // body: they say only when a build is made and on which thread.
  f32 ring_radius_tiles = 0.0f;
  f32 lead_s = 0.0f;
  u32 prefetch_slots = 0;
  // **The copies of tile-handed meshes kept between builds** (T166; docs): a build ahead reads a
  // tile's meshes through `renderer::load_placement_mesh` and keeps of each only what a proxy is
  // cut from (proxy_source.h), at a floor `k_copy_scale_headroom` finer than the scale it was
  // asked at, so the next tile's placement of the same recipe at a scale up to that much larger is
  // cut from the copy, word for word the cut a fresh read gives. At most this many bytes, the least
  // recently used let go first; 0 keeps none.
  u64 mesh_copy_bytes = 0;
  // **Keep what is held for a debug draw** (collision_draw.h; docs: "Seeing the collision"): every
  // proxy's triangles (with each triangle's flora part) and every held tile's pieces stay on the
  // CPU beside the backend's shapes, for `held_tile` and `held_piece`. Off, nothing is kept and
  // those report the ground alone. Changes no body.
  bool keep_shapes = false;
  // **A prop's proxy is built off the tick** (docs: "A prop's proxy"; P45): a mesh prop whose
  // proxy is not resident gets its body at the `prop_build_updates`-th `update_props` after the
  // one that first wanted it, the proxy built meanwhile on the job system `create` was handed —
  // waited for at that update if a worker is still on it, built there if none started it. A count
  // of updates, never a clock, so a replay makes the body at the same tick whatever the workers
  // did. 0: built at the update that wants it, on the tick (a host with no job system, the tests).
  // A declared prop's proxy at scale 1 is built at `create` and needs no wait.
  u32 prop_build_updates = 0;
};
// engine-view's walk (apps/engine_view/walk.cpp): twelve of its 240 Hz ticks, 50 ms. A constant of
// the walk, like its tick rate, so a recorded walk and its replay make every prop's body at the
// same tick.
inline constexpr u32 k_walk_prop_build_updates = 12;
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
  // Proxies cut at their mesh's floor in the copy rather than at `mesh_error_m`, because the
  // placement was larger than the copy was built for (proxy_source.h, "The floor").
  u32 proxies_coarsened = 0;
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
  // T172: ticks on which no build was handed over because the walker (or the flying camera) passes
  // the builds' reach faster than a build takes (docs: "Ahead of a running walker"); and that
  // estimate of a build's time, seconds, from the builds that ran.
  u64 prefetch_outrun = 0;
  f64 build_estimate_s = 0.0;
  // T166: `prefetch_submitted` counts the builds handed to the job system (with slots, a build
  // waits its turn first, and one the tick takes before its turn was never handed over); the
  // tile-handed meshes cut from a kept copy, against those read for want of one.
  u64 mesh_copy_hits = 0;
  u64 mesh_copy_reads = 0;
  u64 mesh_copy_bytes = 0;  // held now
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
  // A game's props (`set_props`; docs: "Props"): handed now, with a body now (their tile held, a
  // shape that collides), and the bodies made and let go over the consumer's life.
  // `prop_moves`: bodies put at a prop's new place, which is what a moved prop costs once its body
  // is a kinematic one.
  u32 props = 0;
  u32 prop_bodies = 0;
  u64 prop_bodies_created = 0;
  u64 prop_bodies_destroyed = 0;
  u64 prop_moves = 0;
  // Props `set_props` was handed under a key it had already been handed in the same call, and left
  // out (the first handed of a key is kept).
  u64 prop_duplicates = 0;
  // **Props' proxies** (docs: "A prop's proxy"): resident now (one a mesh and quantized scale some
  // held prop names, and every declared prop's at scale 1); built, and let go once no prop named
  // them, over the consumer's life; and of the builds, those the tick ran itself (`inline`: no
  // job system, or none had started it by its update) and those it waited for a worker to finish
  // (`waits`, the waits' total). `prop_proxy_bytes`: what the resident ones hold, each one's
  // physics shape (the library's own count) and its entry, so a walk can say what props keep.
  u32 prop_proxies = 0;
  u64 prop_proxy_bytes = 0;
  u64 prop_proxy_builds = 0;
  u64 prop_proxies_freed = 0;
  u64 prop_proxy_inline = 0;
  u64 prop_proxy_waits = 0;
  i64 prop_proxy_wait_ns = 0;
};

// **What the walker and a module's casts meet** (docs: "What a moving prop costs"): every body the
// collision makes — the static ground, placements and standing props, and the kinematic ones,
// a prop's box once it has moved (and a door) — and nothing else in the world. One mask for every
// query that asks what stops or carries the walker or what a module's cast hits: a body that
// moves into `Layer::Kinematic` must not drop out of what those queries see.
inline constexpr physics::LayerMask k_scene_surfaces =
    physics::LayerMask::of(physics::Layer::Static).with(physics::Layer::Kinematic);

// **A prop a host places** (`SceneCollision::set_props`; docs: "Props"; roadmap P45): a scene mesh
// standing at `origin`, turned by `rotation` and scaled uniformly by `scale` — the renderer's
// `SceneInstance` of a game's prop, the mesh's fit applied as the renderer applies it — and the
// body it gets (renderer::PropCollider). `key` is the host's name for it (a game prop's id),
// unique among those handed, and the order bodies are made in.
struct PropBody {
  u32 key = 0;
  u32 mesh = 0;
  WorldPos origin{};
  Quat rotation{};
  f32 scale = 1.0f;
  renderer::PropCollider collider{};
};

// **A door** (`SceneCollision::set_doors`, or a placed piece whose mesh declares a leaf; docs:
// "Doors"; roadmap S91; ADR-0070): a leaf turning about an upright hinge. `hinge` is the hinge
// line's foot; closed, the leaf runs from it along +x of the frame `base` turns to, turned further
// `closed_mdeg` millidegrees about +y, `half_extents` its half width along the leaf, half height
// and half thickness, its middle `face_z` from the hinge line through its thickness (0: centred on
// it; a declared leaf hangs from its face, so half its thickness to one side). Open is `open_mdeg`
// from closed, signed: the side it swings to (a negative swing turns the leaf's +x toward +z).
// Each tick the angle moves toward its state's target by `speed_mdeg`, an integer, so a replay
// reaches the same angle at the same tick. `key` is the host's name for it, or for a declared
// leaf a function of its piece's place and the leaf's index (`declared_door_key`).
inline constexpr i32 k_door_open_mdeg = 90000;
inline constexpr i32 k_door_speed_mdeg = 375;  // 90 degrees in a second at 240 Hz
struct DoorBody {
  u64 key = 0;
  WorldPos hinge{};
  Quat base{};
  i32 closed_mdeg = 0;
  i32 open_mdeg = k_door_open_mdeg;
  i32 speed_mdeg = k_door_speed_mdeg;
  Vec3 half_extents{0.45f, 1.0f, 0.025f};
  f32 face_z = 0.0f;
  bool open = false;  // its state: where it is heading
  // **A door that stands** (docs: "Doors"): a streamed tile's declared leaf, drawn at its declared
  // angle in the tile's block, which nothing turns yet. It collides there and `toggle_door` refuses
  // it, so what collides is what is drawn; it goes with its tile, having no state of its own.
  bool fixed = false;
  i32 angle_mdeg = 0;  // reached, from closed
};
struct DoorStats {
  u32 doors = 0;
  u32 bodies = 0;
  u32 declared = 0;  // of `doors`, the ones a placed piece's mesh declared
  u64 toggles = 0;
  u64 moves = 0;    // ticks a door's angle changed
  u64 holds = 0;    // ticks a door held its angle because its leaf would have entered the walker
  u64 clears = 0;   // ticks a door already in the walker moved away from it (docs: "Doors")
  u32 refused = 0;  // doors the last `set_doors` refused: a key given before
};
// A leaf's turn about +y at `mdeg` millidegrees, a function of the integer's bits on every
// compiler: core/math's deterministic sine and cosine of the half angle, never the C library's
// (docs: "Doors"). What a door's body and its hold on the walker both turn by.
Quat door_rotation(i32 mdeg) noexcept;
// **Where a door's leaf stands** at its angle now (docs: "Doors"): its hinge's foot and its whole
// turn (`base`, then `closed_mdeg` plus the angle about +y). The collision's box and a drawn leaf
// mesh — whose frame is the hinge's foot at the origin with the leaf shut along +x, as the
// structures capability meshes one — both stand here, so what is drawn swings with what collides.
struct DoorLeafPose {
  WorldPos origin{};
  Quat rotation{};
};
DoorLeafPose door_leaf_pose(const DoorBody& door) noexcept;
// **A door from a declared leaf** (the container's section 53, `geometry::ClusterFileDoor`; docs:
// "Doors"): the record in its piece's frame (`offset_scale` the piece's uniform scale) placed at
// `origin` turned by `rotation`, standing at the angle the record declares (open when it is not
// shut), keyed `key`. The turn from the record's shut and swing directions is made with square
// roots alone, so it is the same bits on every toolchain.
DoorBody declared_door(const geometry::ClusterFileDoor& record, WorldPos origin, Quat rotation,
                       f32 scale, u64 key) noexcept;
// The key of a placed piece's `leaf`-th declared door: the piece's name (`support_placement`) and
// the leaf's index, never the order tiles came in. Its top bit is set, which keeps it apart from
// the keys a host hands below 2^63.
u64 declared_door_key(u64 placement, u32 leaf) noexcept;

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
// A game's prop (`set_props`) is `prop`, its `instance` the prop's key and `mesh` its scene mesh;
// a door's leaf (`set_doors`) is `door`, its `instance` the door's key.
enum class HitKind : u8 { none = 0, ground = 1, piece = 2, prop = 3, door = 4 };
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
// `along` (A45) is the limb's axis at the point, unit, base to tip, so an animal can sit along it
// (or across it); zero for a perch and a top, which hold a foot any way round. `placement` names
// the piece whatever tile holds it (`support_placement`): a streamed placement has no scene
// instance, so without it two trees of one entry gave one name to their limbs of the same part.
struct Support {
  WorldPos position{};
  Vec3 up{0.0f, 1.0f, 0.0f};
  f32 size_m = 0.0f;
  f32 distance = 0.0f;  // from the point asked
  u8 kind = 0;
  HitInfo piece;
  u32 part = k_no_index;
  Vec3 along{};
  u64 placement = 0;
};
// **A piece's name among every piece of the world** (A45; docs: "Supports"): a scene instance's
// index; a streamed placement's entry with the name its generator gives it (`Placement::id`, the
// `authored` entry's place in the file), or, from a generator that names none, its entry, its tile
// and its place among the entry's placements there — the same whenever and in whatever order its
// tile comes in, since a generator's tile is a function of the tile. The top bit tells the third
// form from the second; `what` (an instance or a placement) tells the first from the others.
u64 support_placement(u32 instance, u32 entry, u32 id, i32 tile_x, i32 tile_z, u32 index) noexcept;
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
  //
  // **The proxies are cut from `proxies`** (proxy_source.h; docs: "The collision's own copy"),
  // which a host built from `scene` before it released the scene's float geometry and which must
  // outlive the consumer; null, the consumer builds its own from `scene` now, whose floats must
  // still be there. Nothing here reads the scene's float positions, triangles or source indices
  // after `create`.
  bool create(physics::World& physics, const renderer::SceneData& scene,
              const scene_gen::GroundProvider* ground, const Config& config,
              std::string* error = nullptr, const scene_gen::TileSource* tiles = nullptr,
              jobs::JobSystem* jobs = nullptr, const ProxySource* proxies = nullptr);
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
  //
  // **`velocity`** (metres a second, T166; docs: "Ahead of a running walker"): with
  // `Config::lead_s`, the tiles round the point that far along it (at most `k_lead_max_tiles`) are
  // built ahead too, and with `Config::prefetch_slots` the builds are handed to the job system in
  // the order the ring will want them — the time until the walker, going on as it goes, comes
  // within the ring's radius of each — so the tile a runner reaches next is never queued behind one
  // beside it. Changes no body.
  void prefetch(WorldPos at, Vec3 velocity = Vec3{});

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
  // ---- a game's props (docs: "Props"; roadmap P45) ----
  // **The props a host places**, copied: each is a body of its own **while the tile it stands in
  // is held** — made with the tile, let go with it — of its mesh's proxy (the cut every
  // placement's is, at the prop's scale) or its declared box; a prop declared `none` is not kept
  // and costs nothing past this call; a key handed twice keeps the first handed
  // (`Stats::prop_duplicates`). A host calls it when its props changed, on the tick they
  // changed; nothing moves until `update_props`.
  void set_props(std::span<const PropBody> props);
  // **On the tick, after the ring's update and before the step**: every prop whose tile came in
  // or which `set_props` added gets its body, every one whose tile went, or which went or changed
  // its shape, loses it, and **one that only moved is moved**: put at its new place with nothing
  // rebuilt — a box's body, made static, made once again as a kinematic one at its first move; a
  // mesh's, which the library moves only as a static body, moved where it is (docs: "What a moving
  // prop costs"). In key order, destroyed before made, so a replay
  // makes the same bodies with the same ids. Returns the bodies made, let go and moved; 0 at once
  // when nothing changed, and a scene with no props pays one comparison. Allocates nothing once
  // the props were handed.
  // **`at_once`: every pending proxy in reach is taken at this update** (docs: "A prop's proxy"): a
  // mesh prop whose tile is held and whose proxy is not ready has it built here, or waited for if a
  // worker is on it, and its body made now instead of at its promised update. A host passes it at
  // the updates that look for every body at once — a drop, a teleport, a walk's first tick
  // (engine-view's walk.h, "Determinism") — so it is a step of the tick sequence and a replay takes
  // the same proxies at the same tick.
  u32 update_props(bool at_once = false);
  // ---- doors (docs: "Doors"; roadmap S91; ADR-0070) ----
  // The doors, copied with their states and angles as given (a restore hands them back where they
  // were, declared ones with them); each a box on the kinematic layer while the tile its hinge
  // stands in is held. **A placed piece whose mesh declares leaves** (section 53) adds a door a
  // leaf the first time a held tile holds it, keyed by its place (`declared_door_key`), at the
  // angle it was declared at, unless a door of that key is already held — so a restore wins, and
  // a door keeps its state while its tile is away and comes back as it was left.
  void set_doors(std::span<const DoorBody> doors);
  // At the tick: the door heads for its other state. False for a key not held, and for a door that
  // stands (`DoorBody::fixed`, a streamed tile's).
  bool toggle_door(u64 key);
  // **On the tick, after the ring's update and before the walker's step**: a held tile's declared
  // doors taken in, bodies made and let go with their tiles (in key order), and every door turned
  // toward its target by its speed — held where the leaf at the next angle would reach into
  // `walker`'s capsule (null: none), except that one already in it moves when moving takes it
  // away — its box put there. Returns the doors that moved; nothing to do costs a scan of the
  // doors.
  u32 step_doors(const physics::CharacterBody* walker);
  // A door's state now: its angle and where it is heading, what a host writes back.
  bool door(u64 key, DoorBody& out) const noexcept;
  // Every door, in key order (what a host saves, draws a leaf for, or searches for an interact).
  std::span<const DoorBody> doors() const noexcept { return {doors_.data(), doors_.size()}; }
  // **The door a walker's interact reaches** (docs: "Doors"): of the doors whose leaf's middle, at
  // its angle now, is within `reach` metres of `feet` on the ground and less than 2.5 m above or
  // below them, the nearest whose bearing from the feet is within 60 degrees of `facing` (on the
  // ground; unit or not) — its index in `doors()`, the smaller key on a tie; `k_no_index` for
  // none. And the door whose hinge is nearest `point` within `radius` (a module's
  // `door_nearest`), the same way.
  u32 door_to_reach(WorldPos feet, Vec3 facing, f32 reach) const noexcept;
  u32 door_nearest(WorldPos point, f32 radius) const noexcept;
  const DoorStats& door_stats() const noexcept { return door_stats_; }
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
    // **The leaves its mesh declares** (section 53; docs: "Doors"), in the proxy's frame at its
    // scale: a door each wherever a tile holds the piece. Empty for most.
    Vector<geometry::ClusterFileDoor> doors;
    f32 door_scale = 1.0f;
    // A tree (a motion contract of the parts kind) that declares no perch and has no structural
    // table: it offers only its top. Said once, the first time a module asks near it (`told`, the
    // tick's alone: `supports_near` runs on the tick).
    bool bare_tree = false;
    mutable bool told = false;
  };
  // Which scene meshes `supports_near` has said are bare trees (`Proxy::bare_tree`), by mesh.
  mutable Vector<u8> told_;
  // One piece of a held tile, with `Config::keep_shapes`.
  struct PieceRecord {
    u32 mesh = 0;
    u32 instance = k_no_index;
    u32 entry = k_no_index;
    Vec3 offset{};
    Quat rotation{};
    const Proxy* proxy = nullptr;
    // A streamed placement's name within its entry (`Placement::id`), else 0; and its place among
    // the entry's placements on its tile (`support_placement`).
    u32 id = 0;
    u32 index = 0;
    // **Whether the renderer hung its leaves** (docs: "Doors"): what collides is what is drawn, so
    // a piece's declared leaves are doors only where they are drawn — `k_leaves_turn` for a scene
    // instance the scene hung them on (`SceneData::leaves`), `k_leaves_stand` for a tile's own mesh
    // the drawn tile hangs them for (`renderer::placement_mesh_hangs_leaves`), drawn at their
    // declared angle; none for any other piece, whose doorway stands open.
    u8 leaves = 0;
  };
  static constexpr u8 k_leaves_turn = 1;
  static constexpr u8 k_leaves_stand = 2;
  // Scene instances whose leaves the scene hung (`SceneData::leaves`' houses), by instance.
  Vector<u8> hung_houses_;
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
    u32 coarsened = 0;
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
    ProxySource tile_source;  // that mesh's copy, at the one threshold asked (`tile_proxy_for`)
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
  // What `fill_supports` reads of a source mesh: its perches, its species, its structural table
  // and its motion parts — a view, so a kept copy (`MeshCopy`) holds those and not the mesh.
  struct SupportSource {
    std::span<const geometry::ClusterFilePerch> perches;
    std::span<const geometry::ClusterFileStructuralPart> structural_parts;
    std::span<const geometry::ClusterFileMotionPart> motion_parts;
    std::span<const geometry::ClusterFileDoor> doors;
    const std::string* species = nullptr;
    bool has_motion = false;
  };
  static SupportSource support_source(const renderer::SourceMesh& source) noexcept;
  static void fill_supports(const SupportSource* source, Vec3 scale,
                            const PlacementScratch& scratch, Proxy& proxy);
  // **A tile-handed mesh's kept copy** (T166; `Config::mesh_copy_bytes`): what its proxies are cut
  // from (`copy`, mesh `index`, cut exactly at every threshold at or above `floor`) and what its
  // supports are read from, by the mesh's identity. `users` counts the cuts reading it now, under
  // `copies_mutex_`: a copy is let go only when none is.
  struct MeshCopy {
    ProxySource copy;
    u32 index = 0;
    f32 floor = 0.0f;
    bool ok = false;
    Vector<geometry::ClusterFilePerch> perches;
    Vector<geometry::ClusterFileStructuralPart> structural_parts;
    Vector<geometry::ClusterFileMotionPart> motion_parts;
    Vector<geometry::ClusterFileDoor> doors;
    std::string species;
    bool has_motion = false;
    u64 bytes = 0;
    u64 used = 0;
    u32 users = 0;
  };
  // The copy of `mesh` that cuts `threshold` exactly, read and kept when there is none; null when
  // copies are off. `release_copy` gives it back. Thread-safe.
  MeshCopy* acquire_copy(const scene_gen::PlacementMesh& mesh, u64 identity, f32 threshold,
                         PlacementScratch& scratch);
  void release_copy(MeshCopy* copy) noexcept;
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
  // The cut both kinds of proxy are made of: mesh `mesh` of the copy `source`, its clusters where
  // each cluster's own error is within the threshold and its parent's is not, welded by source
  // vertex and scaled, into `scratch.vertices`, `.indices` and `.parts` — the copy holds a tree's
  // wood alone. Returns the triangles left out for a tree; `coarsened` says the threshold was under
  // the mesh's floor and the floor was cut instead (proxy_source.h, "The floor").
  u32 cut_proxy(const ProxySource& source, u32 mesh, Vec3 scale, PlacementScratch& scratch,
                bool& coarsened) const;
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
  // What the scene's proxies are cut from (`create`): the host's copy, or `own_source_`.
  const ProxySource* proxy_source_ = nullptr;
  std::unique_ptr<ProxySource> own_source_;
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
  // A generator's state is its own and may be scratch it writes, so the `tile` of one that does not
  // say it is reentrant (`scene_gen::PlacementGeneratorDesc::tile_reentrant`, T169) is asked by one
  // thread at a time, under this; a reentrant one is asked beside the others.
  std::mutex generators_mutex_;
  FlatMap<u64, Tile> tiles_;
  Stats stats_;
  // A game's props (`set_props`, P45): the ones that collide, held in key order with their bodies,
  // the next set handed waiting beside them, and the tiles' activations and deactivations at the
  // last update. `kinematic`: a box that has moved, so its body is a kinematic one (docs: "What a
  // moving prop costs"); `pending`: its body is to be made at this update if its tile is held.
  // A mesh prop's `proxy` is its key in `prop_proxies_` and `quantized` the module's scale its body
  // is built and placed at (`prop_scale`); both 0 for a box.
  struct HeldProp {
    PropBody prop;
    physics::BodyId body{};
    physics::ShapeId shape{};
    u64 proxy = 0;
    f32 quantized = 0.0f;
    bool kinematic = false;
    bool pending = true;
  };
  Vector<HeldProp> props_;
  Vector<HeldProp> props_next_;
  bool props_handed_ = false;
  u64 props_ring_ = ~u64{0};
  bool prop_pose(const HeldProp& held, physics::BodyTransform& out) const;
  bool make_prop_body(HeldProp& held);
  void drop_prop_body(HeldProp& held) noexcept;
  // **What a mesh's props share** (docs: "A prop's proxy"), by scene mesh, found once: the mesh's
  // fit as a turn and a scale (`decompose`), so a prop's proxy scale is its own scale times the
  // fit's and never a function of its yaw; and the step its scale is rounded to, which moves no
  // point of the mesh by more than a quarter of `Config::mesh_error_m` (`reach_m`: how far the
  // fitted mesh reaches from its origin). `ok` false: a mesh with no prop body.
  struct PropMesh {
    bool known = false;
    bool ok = false;
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    f32 reach_m = 0.0f;
    f32 step = 0.0f;
  };
  Vector<PropMesh> prop_meshes_;
  const PropMesh& prop_mesh(u32 mesh);
  // The module scale `scale` rounded to the mesh's step, anchored at 1 (so 1 is 1), and the key of
  // the proxy at it; false for a mesh with no prop body or a scale of no size.
  bool prop_scale(u32 mesh, f32 scale, f32& quantized, u64& key);
  u32 last_scale_mesh_ = k_no_index;  // `prop_scale`'s last question and answer
  f32 last_scale_ = 0.0f;
  f32 last_quantized_ = 0.0f;
  u64 last_key_ = 0;
  // **A prop's proxy, built off the tick** (`Config::prop_build_updates`): by (mesh, quantized
  // scale), named by `refs` held props whose tile is held, gone with the last unless `declared`. A
  // job builds it into `proxy`; `state` is claimed by whoever runs it first (the job or the tick at
  // `due`), and `ran` signalled when the job returned. Defined in the source.
  struct PropProxy;
  PropProxy* want_prop_proxy(const HeldProp& held, bool now);
  bool finish_prop_proxy(PropProxy& entry);
  void count_prop_proxies() noexcept;
  void build_proxy(u32 mesh, Vec3 scale, PlacementScratch& scratch, Proxy& out, u32& left_out,
                   bool& coarsened, u64& bytes) const;
  FlatMap<u64, std::unique_ptr<PropProxy>> prop_proxies_;
  Vector<std::unique_ptr<PropProxy>> prop_retired_;  // let go while a job may still touch them
  Vector<u64> prop_unused_;                          // scratch: the keys no prop names
  u64 prop_updates_ = 0;                             // `update_props` calls, the deadline's clock
  u32 props_waiting_ = 0;                            // held props whose proxy is building
  // The doors (`set_doors` and the pieces' declared leaves, S91), in key order, and beside each its
  // kinematic box while its tile is held (`door_bodies_[i]` is `doors_[i]`'s).
  struct HeldDoor {
    physics::BodyId body{};
    physics::ShapeId shape{};
  };
  Vector<DoorBody> doors_;
  Vector<HeldDoor> door_bodies_;
  Vector<DoorBody> doors_found_;  // scratch: a ring change's declared doors not held yet
  u64 doors_ring_ = ~u64{0};
  DoorStats door_stats_;
  void drop_door_body(HeldDoor& held) noexcept;
  // The held tiles' declared doors not held yet, merged in by key (`step_doors`, on a ring change).
  void take_declared_doors();
  u32 find_door(u64 key) const noexcept;
  // Scratch, reused: the tick's placements, and the refresh's order.
  PlacementScratch tick_scratch_;
  Vector<u64> order_;
  // The ground built ahead: a build a tile, and builds let go whose job has yet to run.
  jobs::JobSystem* jobs_ = nullptr;
  Vector<std::unique_ptr<Build>> builds_;
  Vector<std::unique_ptr<Build>> retired_;
  WorldPos prefetched_at_{};
  bool prefetched_once_ = false;
  // The builds not yet handed to the job system, handed over most urgent first up to the slots
  // (`Config::prefetch_slots`; T166): every tick, from where the walker is and how it moves.
  void submit_builds(WorldPos at, Vec3 velocity);
  // A build that ran, folded into `Stats::build_estimate_s` (T172): what one takes on this machine
  // and this world, which the rule above reads against how long the walker stays in reach.
  void count_build(const Build& build) noexcept;
  // The kept copies of tile-handed meshes, by identity (T166), and the order they were last used.
  FlatMap<u64, std::unique_ptr<MeshCopy>> mesh_copies_;
  Vector<std::unique_ptr<MeshCopy>> copies_orphans_;  // replaced while a cut still read them
  std::mutex copies_mutex_;
  u64 copies_clock_ = 0;
  // Under the lock; the tick folds them into `stats_` (`submit_builds`), since a worker never
  // touches `Stats`.
  u64 copies_hits_ = 0;
  u64 copies_reads_ = 0;
  u64 copies_bytes_ = 0;
};

}  // namespace engine::scene_collision

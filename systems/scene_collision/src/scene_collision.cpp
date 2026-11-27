// The scene's collision round a walker (scene_collision.h; docs/subsystems/scene_collision.md).

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/log/log.h>
#include <core/math/deterministic.h>
#include <core/profiling/startup_trace.h>
#include <core/time/time.h>
#include <domain/physics/character.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/flora_motion.h>
#include <systems/renderer/scene.h>
#include <systems/scene_collision/scene_collision.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace engine::scene_collision {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_collision, "scene_collision");

// **A water body's surface is not something to stand on** (docs/subsystems/water.md, "Wading"):
// the walker stands on the ground under it and wades by the depth, so a mesh the scene's `water`
// block made (`renderer::SceneMeshInfo::water`) gets no body, as the terrain's does not.
bool water_mesh(const renderer::SceneData& scene, u32 mesh) noexcept {
  return mesh < scene.mesh_info.size() && scene.mesh_info[mesh].water.has_value();
}

// Read once by the host that makes the consumer (docs/subsystems/scene_collision.md, "The
// tunables", where the costs they were chosen by are).
tunables::Float collision_spacing{"walk.collision.spacing_m", 1.0, 0.125, 8.0,
                                  "The collision ground's sample spacing, metres: a heightfield a "
                                  "tile on this lattice of the scene's ground"};
tunables::Float collision_radius{"walk.collision.radius_tiles", 1.5, 0.5, 8.0,
                                 "How far from the walker, in tiles, the collision ring holds "
                                 "tiles' bodies (to a tile's centre)"};
tunables::Int collision_max_tiles{"walk.collision.max_tiles", 2, 0, 1 << 16,
                                  "Tiles whose bodies the collision ring makes per update, nearest "
                                  "first (twice as many are let go); 0 is unlimited"};
tunables::Float collision_mesh_error{"walk.collision.mesh_error_m", 0.05, 0.0, 16.0,
                                     "How far a placement's collision proxy may stand off its "
                                     "mesh: the coarsest cut of its LOD DAG within this, metres"};
tunables::Float collision_ground_error{"walk.collision.ground_error_m", 0.05, 0.001, 16.0,
                                       "How far the collision ground may stand off the drawn "
                                       "ground under a time-lapse before its tile is rebuilt, "
                                       "metres"};
tunables::Int collision_max_refreshes{"walk.collision.max_refreshes", 2, 1, 1 << 16,
                                      "Ground tiles the collision rebuilds per update under a "
                                      "time-lapse, nearest the walker first"};
tunables::Float collision_prefetch{"walk.collision.prefetch_tiles", 1.0, 0.0, 8.0,
                                   "How far past the collision ring, in tiles, a tile's ground "
                                   "is built ahead on the job system; 0 builds nothing ahead"};
tunables::Int collision_workers{"walk.collision.prefetch_workers", 4, 1, 32,
                                "Workers the collision's builds ahead run on (T166): a drop's "
                                "ring is built on them at once, not one tile after another"};
tunables::Float collision_lead{"walk.collision.lead_s", 4.0, 0.0, 30.0,
                               "Seconds of the walker's velocity the collision's builds ahead look "
                               "along, at most four tiles (T166); 0 builds round the walker alone"};
tunables::Int collision_mesh_copies{
    "walk.collision.mesh_copies_mib", 64, 0, 4096,
    "MiB of tile-handed meshes' proxy copies kept between the "
    "collision's builds (T166); 0 reads a tile's meshes every build"};
tunables::Float collision_proxy_ahead{
    "walk.collision.proxy_ahead_m", 256.0, 0.0, 100000.0,
    "Metres round the walker within which the scene's own meshes' collision proxies are built "
    "ahead on the job system, nearest first (W200); 0 builds none ahead"};

i64 floor_div(i64 a, i64 b) noexcept {
  i64 q = a / b;
  if ((a % b) != 0 && ((a < 0) != (b < 0))) --q;
  return q;
}
i64 ceil_div(i64 a, i64 b) noexcept { return -floor_div(-a, b); }

// The millimetre a world coordinate is in: the world's lattice and its tiles are integer
// millimetres from the origin (ADR-0050), and a coordinate reaches them in f64, never through a
// float32 metre (ADR-0053).
i64 floor_mm(f64 metres) noexcept { return static_cast<i64>(std::floor(metres * 1000.0)); }

// Whether a lattice index fits the tile source's i32 window (`scene_gen::TileSourceOps`).
bool fits_i32(i64 v) noexcept {
  return v >= static_cast<i64>(std::numeric_limits<i32>::min()) &&
         v <= static_cast<i64>(std::numeric_limits<i32>::max());
}

// **An instance's place, as the GPU stores it** (`gfx::InstanceDesc`; ADR-0053): its 3x4 in its
// cell's frame — the cell's corner at the origin, so the translation column is the cell's local,
// under 64 m — and the cell. The frame's matrix is `gfx::instance_matrix` at an eye on the cell's
// corner, which is the local exactly; the place is the cell and the local summed in f64. Nothing
// here forms the float32 world matrix, which 420 km out rounded a piece by up to 1.6 cm.
Mat4 cell_frame_matrix(const gfx::InstanceDesc& instance) noexcept {
  return gfx::instance_matrix(instance, WorldEye{instance.cell, Vec3{}, Vec3{}});
}
WorldPos cell_corner(const gfx::InstanceDesc& instance) noexcept {
  return WorldPos{static_cast<f64>(instance.cell.x) * k_world_cell_m,
                  static_cast<f64>(instance.cell.y) * k_world_cell_m,
                  static_cast<f64>(instance.cell.z) * k_world_cell_m};
}

u32 bits(f32 v) noexcept {
  if (v == 0.0f) v = 0.0f;
  return std::bit_cast<u32>(v);
}

// The height of a lattice cell at fractions (u along x, v along z), as the backend's heightfield
// triangulates it: split along the (0, 0)-(1, 1) diagonal (physics.md; Jolt's
// `HeightFieldShape::ProjectOntoSurface`).
f32 cell_height(f32 h00, f32 h10, f32 h01, f32 h11, f32 u, f32 v) noexcept {
  if (v >= u) return h00 + v * (h01 - h00) + u * (h11 - h01);
  return h00 + v * (h11 - h10) + u * (h10 - h00);
}

// A matrix's linear part (its first three columns; the translation is the caller's, in f64) as a
// rotation and a scale the proxy is built at: the backend places a body by a position and a
// rotation, and a shape carries its own size (physics.md, `BodyDesc`). A mirror goes into the
// scale's x.
struct Placement {
  Quat rotation{};
  Vec3 scale{1.0f, 1.0f, 1.0f};
};
bool decompose(const Mat4& m, Placement& out) noexcept {
  const Vec3 c0 = m.c[0].xyz();
  const Vec3 c1 = m.c[1].xyz();
  const Vec3 c2 = m.c[2].xyz();
  f32 sx = length(c0);
  const f32 sy = length(c1);
  const f32 sz = length(c2);
  if (!(sx > 0.0f) || !(sy > 0.0f) || !(sz > 0.0f)) return false;
  if (determinant(Mat3(c0, c1, c2)) < 0.0f) sx = -sx;
  out.rotation = quat_from_mat3(Mat3(c0 / sx, c1 / sy, c2 / sz));
  out.scale = Vec3{sx, sy, sz};
  return true;
}

// A proxy's key: the mesh alone at unit scale (every ruin piece), the mesh and the scale's bits
// otherwise, in a space the first cannot reach.
u64 proxy_key(u32 mesh, Vec3 s) noexcept {
  if (s.x == 1.0f && s.y == 1.0f && s.z == 1.0f) return mesh;
  u64 h = hash_combine(hash_combine(k_hash_seed, mesh), bits(s.x));
  h = hash_combine(hash_combine(h, bits(s.y)), bits(s.z));
  return h | (u64{1} << 63);
}

// A proxy's threshold, in the mesh's own units: the instance's scale (its fit included, since the
// scale was taken from the whole world matrix) multiplies every error by up to its largest axis.
f32 proxy_threshold(f32 mesh_error_m, Vec3 scale) noexcept {
  const f32 largest =
      std::max(std::fabs(scale.x), std::max(std::fabs(scale.y), std::fabs(scale.z)));
  return mesh_error_m / (largest > 0.0f ? largest : 1.0f);
}

}  // namespace

f32 spacing_tunable() noexcept { return static_cast<f32>(collision_spacing.get()); }
f32 radius_tunable() noexcept { return static_cast<f32>(collision_radius.get()); }
u32 max_tiles_tunable() noexcept { return static_cast<u32>(collision_max_tiles.get()); }
f32 mesh_error_tunable() noexcept { return static_cast<f32>(collision_mesh_error.get()); }
f32 ground_error_tunable() noexcept { return static_cast<f32>(collision_ground_error.get()); }
u32 max_refreshes_tunable() noexcept { return static_cast<u32>(collision_max_refreshes.get()); }
f32 prefetch_tunable() noexcept { return static_cast<f32>(collision_prefetch.get()); }
u32 prefetch_workers_tunable() noexcept { return static_cast<u32>(collision_workers.get()); }
f32 lead_tunable() noexcept { return static_cast<f32>(collision_lead.get()); }
u64 mesh_copies_tunable() noexcept {
  return static_cast<u64>(collision_mesh_copies.get()) * 1024u * 1024u;
}
f32 proxy_ahead_tunable() noexcept { return static_cast<f32>(collision_proxy_ahead.get()); }

Config config_from_tunables(f32 tile_size) noexcept {
  Config c;
  c.tile_size = tile_size;
  c.spacing_m = spacing_tunable();
  c.mesh_error_m = mesh_error_tunable();
  c.ground_error_m = ground_error_tunable();
  c.max_refreshes = max_refreshes_tunable();
  return c;
}

world::RingParams ring_params_from_tunables(f32 tile_size) noexcept {
  world::RingParams p;
  p.tile_size = tile_size;
  p.ring_count = 1;
  for (f32& r : p.radius)
    r = 0.0f;
  p.radius[0] = radius_tunable();
  p.max_activations = max_tiles_tunable();
  p.max_deactivations = 2 * max_tiles_tunable();
  return p;
}

// One streamed entry: its generator, the state it opened, and the scene meshes its resident list
// names (`renderer::StreamedPlacements`).
struct SceneCollision::Entry {
  const scene_gen::PlacementGeneratorDesc* generator = nullptr;
  void* state = nullptr;
  const renderer::StreamedPlacements* source = nullptr;
  ~Entry() {
    if (generator != nullptr) generator->close(state);
  }
};

// **A tile built ahead of the ring** (docs: "The ground built ahead of the ring", "The placements
// built ahead"): its lattice window and the heights the activation would ask the tile source for —
// the same call with the same arguments, so the floats are the same whichever thread asked — and
// its placements as the activation would gather them, the compound's shape built and not yet in
// the physics world's table. `state` is claimed by whoever runs it first — the job, or the tick
// that activates the tile before the job started — and `ran` is signalled by the job system when
// the job has returned, after which nothing touches the build but its owner. A build dropped
// before its tile was activated releases its compound with it.
struct SceneCollision::Build {
  SceneCollision* owner = nullptr;
  world::TileCoord coord;
  // The ground's heights, when `ground`.
  bool ground = false;
  i64 i0 = 0;
  i64 j0 = 0;
  u32 n = 0;
  scene_gen::TileSource source;
  f64 time_s = 0.0;
  i64 spacing_mm = 1000;
  Vector<f32> heights;
  bool heights_ok = false;
  // The placements, when `placements`.
  bool placements = false;
  PlacementScratch scratch;
  TilePlacements placed;
  // Where its time went, on whichever thread ran it.
  i64 ground_ns = 0;
  i64 placement_ns = 0;
  // Handed to the job system yet (T166: with slots a build waits its turn; `ran` counts a job only
  // once it has one), and when the ring will want it, seconds (`submit_builds`).
  bool submitted = false;
  f64 need_s = 0.0;
  f64 distance = 0.0;
  static constexpr u32 k_queued = 0, k_running = 1, k_done = 2;
  std::atomic<u32> state{k_queued};
  jobs::Counter ran;

  // A build let go before its tile was activated gives back the tile-handed proxies it named.
  ~Build() {
    if (owner != nullptr) owner->release_tile_proxies(placed.tile_proxies);
  }

  void compute() noexcept {
    // The startup trace's word for a tile built, on whichever thread built it (T166): the tick's
    // own are inside its "collision tile", a worker's stand alone.
    profiling::TraceStage stage("collision build");
    const i64 start = time::monotonic_ns();
    if (ground) {
      heights_ok = source.heights(time_s, spacing_mm, static_cast<i32>(i0), static_cast<i32>(j0), n,
                                  n, std::span<f32>(heights.data(), heights.size()));
    }
    const i64 sampled = time::monotonic_ns();
    if (placements) owner->gather_placements(coord, scratch, placed);
    ground_ns = sampled - start;
    placement_ns = time::monotonic_ns() - sampled;
    state.store(k_done, std::memory_order_release);
  }
  static void run(void* data) noexcept {
    auto* build = static_cast<Build*>(data);
    u32 expected = k_queued;
    if (build->state.compare_exchange_strong(expected, k_running, std::memory_order_acq_rel))
      build->compute();
  }
};

// **A prop's proxy** (docs: "A prop's proxy"): built on the job system, or by the tick at `due`
// when no job ran it first; after `ran` (or `state` done with no job), only the tick touches it.
struct SceneCollision::PropProxy {
  const SceneCollision* owner = nullptr;
  u32 mesh = 0;
  Vec3 scale{};
  Proxy proxy;
  std::unique_ptr<PlacementScratch> scratch;  // the build's, let go once it is done
  u32 left_out = 0;
  bool coarsened = false;
  u64 bytes = 0;
  i64 ns = 0;
  u64 due = 0;  // the `update_props` (its count) whose body waits no longer
  u32 refs = 0;
  bool declared = false;
  bool submitted = false;
  bool counted = false;  // folded into the stats
  static constexpr u32 k_queued = 0, k_running = 1, k_done = 2;
  std::atomic<u32> state{k_queued};
  jobs::Counter ran;

  void compute() noexcept {
    const i64 start = time::monotonic_ns();
    owner->build_proxy(mesh, scale, *scratch, proxy, left_out, coarsened, bytes);
    ns = time::monotonic_ns() - start;
    state.store(k_done, std::memory_order_release);
  }
  static void run(void* data) noexcept {
    auto* entry = static_cast<PropProxy*>(data);
    u32 expected = k_queued;
    if (entry->state.compare_exchange_strong(expected, k_running, std::memory_order_acq_rel))
      entry->compute();
  }
};

// **A scene mesh's proxy built ahead** (W200; docs: "A scene of authored meshes"): one job a
// proxy, into the proxies' cache (`cached_proxy_for`), which is what any asker would have built.
// `state` says who builds it: the job, once it has moved it from queued to running; or an asker
// that found it still queued and moved it to done, and so builds it itself (`proxy_for`); or the
// tick, withdrawing it unbuilt (`withdraw_proxies`). Kept for the consumer's life: an asker may be
// waiting on `ran`. An asker moves it from queued only under the proxies' lock, and a withdrawn
// one is out of `ahead_by_key_` under the same lock, so one is made again for another proxy
// (`ahead_free_`) once its job has returned and no asker can be holding it.
struct SceneCollision::ProxyAhead {
  SceneCollision* owner = nullptr;
  u32 mesh = 0;
  Vec3 scale{};
  u32 row = 0;  // its `AheadRow`, for the withdrawal's distance
  static constexpr u32 k_queued = 0, k_running = 1, k_done = 2;
  std::atomic<u32> state{k_queued};
  jobs::Counter ran;

  static void run(void* data) noexcept {
    auto* entry = static_cast<ProxyAhead*>(data);
    u32 expected = k_queued;
    if (!entry->state.compare_exchange_strong(expected, k_running, std::memory_order_acq_rel))
      return;
    {
      PlacementScratch scratch;  // the job's own, gone with it
      (void)entry->owner->cached_proxy_for(entry->mesh, entry->scale, scratch);
      std::lock_guard<std::mutex> lock(entry->owner->proxies_mutex_);
      ++entry->owner->proxy_stats_.ahead_built;
    }
    entry->state.store(k_done, std::memory_order_release);
  }
};

SceneCollision::SceneCollision() = default;

SceneCollision::~SceneCollision() {
  // The proxies handed ahead that no job has started are taken back first (W200), so what is waited
  // for below is the jobs already running, never a queue of builds nobody will use.
  withdraw_proxies(WorldPos{}, -1.0);
  // Every job that may still touch a build has run before the builds go, and the builds' and the
  // proxies' shapes are released before the physics world they were built in goes.
  for (const std::unique_ptr<Build>& build : builds_)
    build->ran.wait_blocking();
  for (const std::unique_ptr<Build>& build : retired_)
    build->ran.wait_blocking();
  builds_.clear();
  retired_.clear();
  for (const std::unique_ptr<ProxyAhead>& entry : ahead_)  // a proxy built ahead (W200)
    entry->ran.wait_blocking();
  for (u32 i = 0; i < prop_proxies_.size(); ++i)  // a prop's proxy a job may still build (P45)
    if (prop_proxies_.value_at(i)->submitted) prop_proxies_.value_at(i)->ran.wait_blocking();
  for (const std::unique_ptr<PropProxy>& entry : prop_retired_)
    entry->ran.wait_blocking();
  prop_retired_.clear();
  if (physics_ != nullptr) {
    for (u32 i = 0; i < tiles_.size(); ++i) {
      Tile& tile = tiles_.value_at(i);
      drop_ground_body(tile);
      if (tile.placements) physics_->destroy_body(tile.placements);
      if (tile.placements_shape) physics_->destroy_shape(tile.placements_shape);
    }
    for (HeldProp& held : props_)
      drop_prop_body(held);  // P45
    for (HeldDoor& held : door_bodies_)
      drop_door_body(held);  // S91
  }
  tiles_.clear();
  tick_scratch_.children.clear();
  proxies_.clear();
  tile_proxies_.clear();
  prop_proxies_.clear();
  entries_.clear();
}

scene_gen::Context SceneCollision::context_for(const Entry& entry) const noexcept {
  scene_gen::Context context;
  context.world_seed = scene_->terrain.enabled ? scene_->terrain.seed : 0;
  context.tile_size = config_.tile_size;
  context.dir = entry.source->dir;
  context.where = entry.source->where;
  context.ddc = entry.source->ddc;  // the whole read's root (T119; scene_gen.md)
  if (ground_ != nullptr) context.ground = ground_->view();
  if (!scene_->terrain.exclusion.empty()) context.exclusion = &scene_->terrain.exclusion;
  context.site = &scene_->terrain.site;
  if (scene_->terrain.formation.has_value()) context.formation = &*scene_->terrain.formation;
  context.uncut_ground = uncut_hook_;
  // The inner ring's representation: what is drawn round the walker (the ruins' blocks near).
  context.world = &world_rings_;
  context.ring = 0;
  return context;
}

bool SceneCollision::create(physics::World& physics, const renderer::SceneData& scene,
                            const scene_gen::GroundProvider* ground, const Config& config,
                            std::string* error, const scene_gen::TileSource* tiles,
                            jobs::JobSystem* jobs, const ProxySource* proxies) {
  auto fail = [&](const std::string& why) {
    if (error != nullptr) *error = why;
    return false;
  };
  if (!physics.initialized()) return fail("scene collision needs an initialized physics world");
  if (!(config.tile_size > 0.0f) || !(config.spacing_m > 0.0f)) {
    return fail("scene collision needs a positive tile size and ground spacing");
  }
  physics_ = &physics;
  jobs_ = jobs;
  scene_ = &scene;
  ground_ = ground != nullptr && ground->valid() ? ground : nullptr;
  uncut_ = std::make_unique<renderer::UncutGround>(scene.terrain);
  uncut_hook_ = uncut_->hook();
  explicit_source_ = tiles != nullptr && tiles->valid();
  source_ = explicit_source_     ? *tiles
            : ground_ != nullptr ? ground_->tiles()
                                 : scene_gen::TileSource{};
  own_time_s_ = scene.terrain.time_s;
  config_ = config;
  // The collision's own copy (T138; docs: "The collision's own copy"): the host's, built before it
  // released the scene's floats, or one of this consumer's from them now.
  own_source_.reset();
  proxy_source_ = proxies;
  if (proxy_source_ == nullptr) {
    own_source_ = std::make_unique<ProxySource>();
    own_source_->build(scene, config.mesh_error_m);
    proxy_source_ = own_source_.get();
  }
  spacing_mm_ = static_cast<i64>(std::llround(static_cast<f64>(config.spacing_m) * 1000.0));
  if (spacing_mm_ < 1) spacing_mm_ = 1;
  tile_mm_ = static_cast<i64>(std::llround(static_cast<f64>(config.tile_size) * 1000.0));
  stats_ = Stats{};
  told_.clear();
  told_.resize(scene.parts.size(), u8{0});

  // The scene's world block, which a generator reads a ring's representation from: its rings'
  // words and radii, or the world's defaults for a scene that streams with none.
  world_rings_ = scene::WorldRings{};
  world_rings_.tile_size = config.tile_size;
  const world::RingParams defaults;
  const u32 rings = scene.world.ring_count > 0 ? scene.world.ring_count : defaults.ring_count;
  for (u32 r = 0; r < rings && r < world::k_max_rings; ++r) {
    scene::WorldRing ring;
    ring.radius = scene.world.ring_count > r ? scene.world.radius[r] : defaults.radius[r];
    ring.ruins = scene.world.ring_count > r
                     ? static_cast<scene::RingRuins>(scene.world.ruins[r])
                     : (r == 0 ? scene::RingRuins::Blocks
                               : (r == 1 ? scene::RingRuins::Sections : scene::RingRuins::Walls));
    world_rings_.rings.push_back(ring);
  }

  const scene_gen::GeneratorRegistry& registry = scene_gen::GeneratorRegistry::global();
  for (const renderer::StreamedPlacements& source : scene.streamed) {
    auto entry = std::make_unique<Entry>();
    entry->source = &source;
    const scene_gen::PlacementGeneratorDesc* generator = registry.find_placement(source.generator);
    if (generator == nullptr)
      return fail(source.where + " " + registry.unknown_placement(source.generator));
    if (generator->tile == nullptr) {
      return fail(source.where + ": the placement generator \"" + source.generator +
                  "\" has no tiles, so nothing can collide with it tile by tile");
    }
    std::string why;
    void* state = nullptr;
    if (!generator->open(source.params, context_for(*entry), &state, &why)) return fail(why);
    entry->generator = generator;
    entry->state = state;
    entries_.push_back(std::move(entry));
  }

  // The scene's own instances — a scene read whole, or a streamed scene's prefix, which is what
  // the load kept out of the streamed sources (an actor, a deformed instance, or a mesh reaching
  // past a tile, `kept_reach`; W88) — binned by the tiles they reach by their bounds. The streamed
  // sources place their own. The terrain is the ground's, and a skinned instance moves, so neither
  // has a static body.
  bins_.clear();
  ahead_rows_.clear();
  ahead_candidates_.clear();
  ahead_next_ = 0;
  ahead_once_ = false;
  const bool ahead = jobs_ != nullptr && config_.proxy_ahead_m > 0.0f;
  // **The far forest gets no body** (P62, 2026-11-08): a far tile's mesh is the coarsest forms of
  // every streamed tree on a world tile merged into one rigid instance of the prefix, drawn only
  // where the ring's tail does not draw that tile's trees — so never round a walker, whose tile the
  // tail always holds. Binned as a prefix instance (W88) it was a wall of impostor cards a crown
  // wide round every trunk of the tile, invisible, which the owner walked into in the v35 forest.
  far_meshes_.clear();
  for (const renderer::FarForestTile& far : scene.far_forest.tiles) {
    if (far.mesh >= far_meshes_.size()) far_meshes_.resize(far.mesh + 1, u8{0});
    far_meshes_[far.mesh] = 1;
  }
  stats_.far_forest_left_out = 0;
  {
    // **A drawn leaf is its door's box, never a static piece** (S91; docs: "Doors"): its instance
    // stands where the door does, and a door is made from the house's records.
    // And **what collides is what is drawn**: a house's declared leaves are doors only where the
    // scene hung them (`hung_houses_`, `take_declared_doors`), and each only where its own leaf
    // was hung (`hung_leaves_`): a load that kept one leaf's instance and not its neighbour's makes
    // the one door.
    Vector<u8> leaf_instance;
    hung_houses_.clear();
    hung_leaves_.clear();
    if (!scene.leaves.empty()) {
      leaf_instance.resize(scene.instances.size(), u8{0});
      hung_houses_.resize(scene.instances.size(), u8{0});
      hung_leaves_.reserve(static_cast<u32>(scene.leaves.size()));
    }
    for (const renderer::SceneLeaf& leaf : scene.leaves) {
      if (leaf.instance < leaf_instance.size()) leaf_instance[leaf.instance] = 1;
      if (leaf.house < hung_houses_.size()) {
        hung_houses_[leaf.house] = 1;
        hung_leaves_.push_back(hung_leaf(leaf.house, leaf.leaf));
      }
    }
    std::sort(hung_leaves_.begin(), hung_leaves_.end());
    for (u32 i = 0; i < scene.instances.size(); ++i) {
      const gfx::InstanceDesc& instance = scene.instances[i];
      if (instance.mesh == scene.terrain_mesh || instance.mesh >= scene.parts.size()) continue;
      if (i < leaf_instance.size() && leaf_instance[i] != 0) continue;
      if (water_mesh(scene, instance.mesh)) continue;
      if (far_mesh(instance.mesh)) {
        ++stats_.far_forest_left_out;
        continue;
      }
      if (i < scene.instance_joints.size() && scene.instance_joints[i] != 0) continue;
      const geometry::ClusterMeshPart& part = scene.parts[instance.mesh];
      Aabb3 local = Aabb3::empty();
      for (u32 c = 0; c < part.cluster_count; ++c) {
        const geometry::ClusterDesc& cluster = scene.lod.mesh.clusters[part.first_cluster + c];
        local.expand(cluster.center - Vec3(cluster.radius));
        local.expand(cluster.center + Vec3(cluster.radius));
      }
      if (local.is_empty()) continue;
      // The bounds in the instance's cell's frame (float32 the size of the cell and the mesh), then
      // to millimetres of the world from the cell's corner in f64 (`cell_frame_matrix`).
      const Aabb3 bounds = transform_aabb(cell_frame_matrix(instance), local);
      const WorldPos corner = cell_corner(instance);
      const i64 x0 = floor_div(floor_mm(corner.x + static_cast<f64>(bounds.min.x)), tile_mm_);
      const i64 x1 = floor_div(floor_mm(corner.x + static_cast<f64>(bounds.max.x)), tile_mm_);
      const i64 z0 = floor_div(floor_mm(corner.z + static_cast<f64>(bounds.min.z)), tile_mm_);
      const i64 z1 = floor_div(floor_mm(corner.z + static_cast<f64>(bounds.max.z)), tile_mm_);
      // A piece reaching past the world's last tile is on no tile a key can name: its indices are
      // never narrowed to i32 (F11), and no tile of the stream is there to bin it into.
      if (!fits_i32(x0) || !fits_i32(x1) || !fits_i32(z0) || !fits_i32(z1)) continue;
      if ((x1 - x0 + 1) * (z1 - z0 + 1) > 4096)
        continue;  // a scene-sized mesh is ground, not a piece
      // Its proxy, which a tile's build will ask for by this mesh and scale (`add_piece`), built
      // ahead from the first `prefetch` on (W200), nearest first by where it stands: the centre of
      // its bounds, not its instance's origin, which an import that keeps its vertices in its
      // tile's coordinates under identity instances puts at one point for every building.
      if (ahead) {
        Placement placed;
        if (decompose(cell_frame_matrix(instance), placed)) {
          AheadRow row;
          const auto mid = [](f32 lo, f32 hi) {
            return 0.5 * (static_cast<f64>(lo) + static_cast<f64>(hi));
          };
          row.at = WorldPos{corner.x + mid(bounds.min.x, bounds.max.x),
                            corner.y + mid(bounds.min.y, bounds.max.y),
                            corner.z + mid(bounds.min.z, bounds.max.z)};
          row.mesh = instance.mesh;
          row.scale = placed.scale;
          row.key = proxy_key(instance.mesh, placed.scale);
          ahead_rows_.push_back(row);
        }
      }
      for (i64 z = z0; z <= z1; ++z) {
        for (i64 x = x0; x <= x1; ++x) {
          const u64 key =
              world::tile_key(world::TileCoord{static_cast<i32>(x), static_cast<i32>(z)});
          Vector<u32>* bin = bins_.find_value(key);
          if (bin == nullptr) {
            bins_.insert(key, Vector<u32>{});
            bin = bins_.find_value(key);
          }
          bin->push_back(i);
        }
      }
    }
  }
  // **A declared prop's proxy at scale 1, built at the load** (docs: "A prop's proxy"; P45): the
  // scene names the meshes its game may place as props, so the first placement of one at its own
  // size, the common case, costs the tick no build and waits no update.
  prop_meshes_.clear();
  prop_meshes_.resize(scene.parts.size());
  for (const renderer::SceneProp& declared : scene.props) {
    if (declared.collider.shape != renderer::PropShape::mesh) continue;
    HeldProp held;
    if (!prop_scale(declared.mesh, 1.0f, held.quantized, held.proxy)) continue;
    held.prop.mesh = declared.mesh;
    held.prop.collider = declared.collider;
    PropProxy* entry = want_prop_proxy(held, true);
    if (entry != nullptr) entry->declared = true;
  }
  count_prop_proxies();
  ENGINE_LOG_INFO(log_collision, "scene collision", log::field("streamed", entries_.size()),
                  log::field("binned_tiles", bins_.size()),
                  log::field("ground", ground_ != nullptr), log::field("spacing_mm", spacing_mm_),
                  log::field("mesh_error_m", config_.mesh_error_m));
  return true;
}

world::TileConsumer SceneCollision::consumer() noexcept {
  world::TileConsumer out;
  out.name = "collision";
  out.context = this;
  out.rings = 0x01u;
  out.activate = &SceneCollision::activate;
  out.deactivate = &SceneCollision::deactivate;
  out.commit = &SceneCollision::commit;
  return out;
}

// **The broadphase is rebuilt once an update that changed bodies.** The backend's broadphase trees
// free the nodes of the bodies taken out of them only when a tree is rebuilt, which the world's
// `step` does and this world — static bodies and a character, never stepped — never would; a
// walker crossing tiles, or a time-lapse rebuilding the ground under it a thousand times, then runs
// the trees out of nodes (the bench found it: "QuadTree: Out of nodes!" after a few hundred ring
// changes). A rebuild of a handful of bodies — two a tile, nine tiles — is microseconds.
void SceneCollision::commit(void* context) {
  static_cast<SceneCollision*>(context)->physics_->optimize_broad_phase();
}

void SceneCollision::note_bodies() noexcept {
  stats_.tiles = tiles_.size();
  if (config_.mesh_copy_bytes > 0) {
    // The kept copies' counts (T166), which a worker keeps under the copies' lock.
    std::lock_guard<std::mutex> lock(copies_mutex_);
    stats_.mesh_copy_hits = copies_hits_;
    stats_.mesh_copy_reads = copies_reads_;
    stats_.mesh_copy_bytes = copies_bytes_;
  }
  u32 bodies = 0;
  u32 pieces = 0;
  u64 samples = 0;
  u64 ground_bytes = 0;
  u64 compound_bytes = 0;
  for (u32 i = 0; i < tiles_.size(); ++i) {
    const Tile& t = tiles_.value_at(i);
    bodies += (t.ground ? 1u : 0u) + (t.placements ? 1u : 0u);
    pieces += t.pieces;
    samples += t.heights.size() + t.field_a.size() + t.field_b.size();
    ground_bytes += t.ground_bytes;
    compound_bytes += t.compound_bytes;
  }
  stats_.ground_bytes = ground_bytes;
  stats_.compound_bytes = compound_bytes;
  stats_.bodies = bodies;
  stats_.placements = pieces;
  stats_.ground_samples = samples;
  stats_.max_bodies = bodies > stats_.max_bodies ? bodies : stats_.max_bodies;
  stats_.max_placements = pieces > stats_.max_placements ? pieces : stats_.max_placements;
  note_proxies();
}

const Stats& SceneCollision::stats() const noexcept {
  note_proxies();
  return stats_;
}

// The proxies are built on whichever thread asks first, a job ahead among them; their counters are
// copied here, under their lock, at an activation's end and whenever `stats()` is read (W200: the
// builds ahead complete between activations, and a summary read after a flight saw the counts of
// the last activation instead).
void SceneCollision::note_proxies() const noexcept {
  std::lock_guard<std::mutex> lock(proxies_mutex_);
  stats_.proxies = proxies_.size() + tile_proxies_.size();
  stats_.tile_proxies = tile_proxies_.size();
  stats_.proxy_triangles = proxy_stats_.triangles;
  stats_.flora_triangles_left_out = proxy_stats_.flora_left_out;
  stats_.proxies_coarsened = proxy_stats_.coarsened;
  stats_.proxy_bytes = proxy_stats_.bytes;
  stats_.proxy_ns = proxy_stats_.ns;
  stats_.proxy_cut_ns = proxy_stats_.cut_ns;
  stats_.proxy_shape_ns = proxy_stats_.shape_ns;
  stats_.proxy_ahead_built = proxy_stats_.ahead_built;
  stats_.proxy_ahead_waits = proxy_stats_.ahead_waits;
  stats_.proxy_ahead_wait_ns = proxy_stats_.ahead_wait_ns;
  stats_.max_proxy_ahead_wait_ns = proxy_stats_.max_ahead_wait_ns;
  stats_.proxy_ahead_claimed = proxy_stats_.ahead_claimed;
}

// ---- the ground
// ----------------------------------------------------------------------------------

bool SceneCollision::evaluate_field(const Tile& tile, f64 time, Vector<f32>& out) {
  out.resize(tile.n * tile.n);
  ++stats_.field_evaluations;
  // `build_ground` refused a window whose index does not fit the source's i32.
  return source_.heights(time, spacing_mm_, static_cast<i32>(tile.i0), static_cast<i32>(tile.j0),
                         tile.n, tile.n, std::span<f32>(out.data(), out.size()));
}

// `a (1 - t) + b t`, as the renderer's pool pass blends a terrain level: exactly a at 0 and b at 1.
void SceneCollision::blend_heights(Tile& tile, f64 blend) {
  const f32 t = static_cast<f32>(blend);
  const f32 s = 1.0f - t;
  tile.heights.resize(tile.field_a.size());
  for (u32 k = 0; k < tile.heights.size(); ++k)
    tile.heights[k] = tile.field_a[k] * s + tile.field_b[k] * t;
  tile.blend = blend;
}

// **A terrain's holes** (W87; terrain.md, "Holes"): a collision sample is no sample where every
// cell of the collision lattice round it is a hole by the terrain's cell rule, so the triangles
// that use it have no collision and the hole the walker falls through lies inside the one drawn
// (which is the same rule at the drawn levels' spacing). Only a tile whose window a hole reaches
// pays.
bool SceneCollision::hole_sample(i64 i, i64 j) const noexcept {
  const scene_gen::TerrainHoles& holes = scene_->terrain.holes;
  return holes.cell(spacing_mm_, i - 1, j - 1) && holes.cell(spacing_mm_, i, j - 1) &&
         holes.cell(spacing_mm_, i - 1, j) && holes.cell(spacing_mm_, i, j);
}

bool SceneCollision::holes_reach(const Tile& tile) const noexcept {
  const scene_gen::TerrainHoles& holes = scene_->terrain.holes;
  const f64 s = static_cast<f64>(spacing_mm_);
  return !holes.empty() &&
         holes.touches_mm(static_cast<f64>(tile.i0) * s, static_cast<f64>(tile.j0) * s,
                          static_cast<f64>(tile.i0 + tile.n) * s,
                          static_cast<f64>(tile.j0 + tile.n) * s);
}

bool SceneCollision::make_ground_body(Tile& tile) {
  const i64 start = time::monotonic_ns();
  struct Count {
    i64 start;
    i64& into;
    ~Count() { into += time::monotonic_ns() - start; }
  } count{start, stats_.body_ns};
  physics::HeightfieldDesc desc;
  desc.heights = std::span<const f32>(tile.heights.data(), tile.heights.size());
  Vector<f32> holed;
  if (holes_reach(tile)) {
    holed = tile.heights;
    for (u32 j = 0; j < tile.n; ++j) {
      for (u32 i = 0; i < tile.n; ++i) {
        if (hole_sample(tile.i0 + i, tile.j0 + j))
          holed[static_cast<usize>(j) * tile.n + i] = physics::k_heightfield_hole;
      }
    }
    desc.heights = std::span<const f32>(holed.data(), holed.size());
  }
  desc.sample_count = tile.n;
  const f32 spacing = static_cast<f32>(static_cast<f64>(spacing_mm_) / 1000.0);
  // The field in its own frame, from its first lattice point, and the body at that point in f64
  // (ADR-0053): a field offset by its corner in floats would put a tile 420 km out on a 3 cm grid.
  desc.local_offset = Vec3{};
  desc.scale = Vec3{spacing, 1.0f, spacing};
  if (physics_->create_heightfield(desc, tile.ground_shape) != physics::Status::Ok) return false;
  u32 triangles = 0;
  (void)physics_->shape_memory(tile.ground_shape, false, tile.ground_bytes, triangles);
  physics::BodyDesc body;
  body.shape = tile.ground_shape;
  body.transform.position = WorldPos{static_cast<f64>(tile.i0 * spacing_mm_) / 1000.0, 0.0,
                                     static_cast<f64>(tile.j0 * spacing_mm_) / 1000.0};
  body.motion = physics::MotionType::Static;
  body.layer = physics::Layer::Static;
  body.friction = 0.8f;
  if (physics_->create_body(body, tile.ground) != physics::Status::Ok) {
    physics_->destroy_shape(tile.ground_shape);
    tile.ground_shape = {};
    return false;
  }
  ++stats_.bodies_created;
  return true;
}

void SceneCollision::drop_ground_body(Tile& tile) {
  if (tile.ground) {
    physics_->destroy_body(tile.ground);
    ++stats_.bodies_destroyed;
  }
  if (tile.ground_shape) physics_->destroy_shape(tile.ground_shape);
  tile.ground = {};
  tile.ground_shape = {};
}

// The tile's heights on the collision lattice — the lattice points from the one at or west of its
// west edge to the one at or east of its east edge, an even count as the backend's blocks want, so
// neighbouring tiles share their edge samples bit for bit — at the ground's own time, or, under a
// time-lapse, the drawn pair of fields blended as it is drawn.
bool SceneCollision::ground_window(world::TileCoord coord, i64& i0, i64& j0,
                                   u32& n) const noexcept {
  const i64 x0 = i64{coord.x} * tile_mm_;
  const i64 z0 = i64{coord.z} * tile_mm_;
  i0 = floor_div(x0, spacing_mm_);
  j0 = floor_div(z0, spacing_mm_);
  const i64 xi = ceil_div(x0 + tile_mm_, spacing_mm_);
  const i64 zj = ceil_div(z0 + tile_mm_, spacing_mm_);
  n = static_cast<u32>(std::max(xi - i0, zj - j0)) + 1u;
  n = n < 4 ? 4 : n + (n & 1u);
  // The tile source's window is indexed in i32 (scene_gen/tile_source.h): past 2^31 lattice points
  // from the origin — 2.1e9 m at a metre, 2.7e8 m at the tunable's finest 12.5 cm — a tile has no
  // index to ask with, and is refused rather than asked for the heights of a wrapped one.
  return fits_i32(i0) && fits_i32(j0) && fits_i32(i0 + n) && fits_i32(j0 + n);
}

bool SceneCollision::build_ground(Tile& tile, Build* build) {
  u32 n = 0;
  if (!ground_window(tile.coord, tile.i0, tile.j0, n)) {
    tile.n = n;
    ENGINE_LOG_WARN(log_collision, "a tile is past the tile source's lattice index",
                    log::field("x", tile.coord.x), log::field("z", tile.coord.z),
                    log::field("spacing_mm", spacing_mm_));
    return false;
  }
  tile.n = n;
  const i32 i0 = static_cast<i32>(tile.i0);
  const i32 j0 = static_cast<i32>(tile.j0);
  tile.moving = time_.moving && source_.moves();
  const i64 sample_start = time::monotonic_ns();
  auto sampled = [&] {
    const i64 dt = time::monotonic_ns() - sample_start;
    stats_.ground_sample_ns += dt;
    stats_.max_ground_sample_ns = std::max(stats_.max_ground_sample_ns, dt);
  };
  if (!tile.moving) {
    tile.heights.resize(n * n);
    // The ground at its own time: the provider's `grid`, or a tile source's heights at the scene's
    // time (a still source's one surface), which for the scene's own ground is the same bits.
    if (explicit_source_ || (ground_ != nullptr && ground_->ops()->tile_source != nullptr)) {
      // Built ahead, when it was (`prefetch`): the same call's heights, taken as they are.
      bool ok = false;
      if (build != nullptr && build->ground) {
        ok = build->heights_ok;
        if (ok) tile.heights = std::move(build->heights);
      } else {
        ok = source_.heights(own_time_s_, spacing_mm_, i0, j0, n, n,
                             std::span<f32>(tile.heights.data(), tile.heights.size()));
      }
      sampled();
      if (!ok) return false;
    } else {
      ground_->grid(scene_gen::ring_lattice(spacing_mm_), i0, j0, n, n,
                    std::span<f32>(tile.heights.data(), tile.heights.size()));
      sampled();
    }
    tile.field_a.clear();
    tile.field_b.clear();
    return make_ground_body(tile);
  }
  const bool paired = pair_fields(tile);
  sampled();
  if (!paired) return false;
  blend_heights(tile, time_.blend);
  return make_ground_body(tile);
}

// The drawn pair's two fields on the tile's lattice window. When the drawn level handed over — the
// surface reached b and b became the next pair's a, the same heights to the bit — the tile's b is
// its new a and only the new b is evaluated.
bool SceneCollision::pair_fields(Tile& tile) {
  if (tile.moving && !tile.field_b.empty() && tile.time_b == time_.time_a) {
    tile.field_a = std::move(tile.field_b);
    tile.field_b.clear();
  } else if (!evaluate_field(tile, time_.time_a, tile.field_a)) {
    return false;
  }
  if (time_.time_b == time_.time_a) {
    tile.field_b = tile.field_a;
  } else if (!evaluate_field(tile, time_.time_b, tile.field_b)) {
    return false;
  }
  tile.time_a = time_.time_a;
  tile.time_b = time_.time_b;
  tile.moving = true;
  return true;
}

// How far the heights the tile's body was built with stand off the drawn ground now, at worst over
// its samples: the drawn heights are the tile's pair blended as the renderer blends it.
f64 SceneCollision::stale_of(const Tile& tile) const noexcept {
  if (tile.field_a.size() != tile.heights.size() || tile.field_b.size() != tile.heights.size())
    return 0.0;
  const f32 t = static_cast<f32>(time_.blend);
  const f32 s = 1.0f - t;
  f32 worst = 0.0f;
  for (u32 k = 0; k < tile.heights.size(); ++k) {
    const f32 d = std::fabs(tile.heights[k] - (tile.field_a[k] * s + tile.field_b[k] * t));
    worst = d > worst ? d : worst;
  }
  return static_cast<f64>(worst);
}

u32 SceneCollision::refresh(WorldPos at, f32 reach) {
  if (ground_ == nullptr || !time_.moving || !source_.moves() || tiles_.empty()) return 0;
  // The tiles under the walker: every tile the square `reach` either side of (x, z) touches, which
  // is the one holding the point unless a capsule of that radius stands near a tile's edge.
  const f64 r = reach > 0.0f ? static_cast<f64>(reach) : 0.0;
  const i64 ux0 = floor_div(floor_mm(at.x - r), tile_mm_);
  const i64 ux1 = floor_div(floor_mm(at.x + r), tile_mm_);
  const i64 uz0 = floor_div(floor_mm(at.z - r), tile_mm_);
  const i64 uz1 = floor_div(floor_mm(at.z + r), tile_mm_);
  const auto under_walker = [&](u64 key) {
    const world::TileCoord c = world::tile_of_key(key);
    return c.x >= ux0 && c.x <= ux1 && c.z >= uz0 && c.z <= uz1;
  };
  // The tiles with work: a new pair of fields to evaluate (which is not yet a rebuild — at a slow
  // rate the new b is the old heights to the millimetre), or heights further than their error
  // from the drawn ones under the pair they have: a millimetre under the walker, where the ground
  // it stands on must follow the drawn sand a frame at a time, `ground_error_m` elsewhere.
  const f64 limit = static_cast<f64>(config_.ground_error_m);
  const f64 underfoot_limit = static_cast<f64>(k_underfoot_error_m);
  order_.clear();
  for (u32 i = 0; i < tiles_.size(); ++i) {
    const Tile& t = tiles_.value_at(i);
    if (t.heights.empty()) continue;
    const bool pair = !t.moving || t.time_a != time_.time_a || t.time_b != time_.time_b;
    const bool under = under_walker(tiles_.key_at(i));
    if (!pair && !(stale_of(t) > (under ? underfoot_limit : limit))) continue;
    order_.push_back(tiles_.key_at(i));
  }
  if (order_.empty()) return 0;
  // In f64 from the walker: a tile's centre and the feet are both far from the origin, and their
  // difference is what orders the tiles.
  const f64 tile = static_cast<f64>(tile_mm_) / 1000.0;
  auto distance2 = [&](u64 key) {
    const world::TileCoord c = world::tile_of_key(key);
    const f64 dx = (static_cast<f64>(c.x) + 0.5) * tile - at.x;
    const f64 dz = (static_cast<f64>(c.z) + 0.5) * tile - at.z;
    return dx * dx + dz * dz;
  };
  // The tiles under the walker first, then nearest first; the budget is the others'.
  std::sort(order_.begin(), order_.end(), [&](u64 a, u64 b) {
    const bool ua = under_walker(a);
    const bool ub = under_walker(b);
    if (ua != ub) return ua;
    const f64 da = distance2(a);
    const f64 db = distance2(b);
    return da != db ? da < db : a < b;
  });
  u32 under_count = 0;
  while (under_count < order_.size() && under_walker(order_[under_count]))
    ++under_count;
  const u32 count = under_count + std::min<u32>(static_cast<u32>(order_.size()) - under_count,
                                                config_.max_refreshes);
  const i64 xm = floor_mm(at.x);
  const i64 zm = floor_mm(at.z);
  const i64 txm = floor_div(xm, tile_mm_);
  const i64 tzm = floor_div(zm, tile_mm_);
  // A walker past the world's last tile stands on none (F11): its index is not narrowed to i32,
  // where it would name some other tile.
  const bool on_tile = fits_i32(txm) && fits_i32(tzm);
  const u64 underfoot_key =
      on_tile ? world::tile_key(world::TileCoord{static_cast<i32>(txm), static_cast<i32>(tzm)}) : 0;
  u32 done = 0;
  bool rebuilt = false;
  for (u32 k = 0; k < count; ++k) {
    Tile* t = tiles_.find_value(order_[k]);
    if (t == nullptr) continue;
    const i64 start = time::monotonic_ns();
    const bool pair = !t->moving || t->time_a != time_.time_a || t->time_b != time_.time_b;
    if (pair && !pair_fields(*t)) {
      ++stats_.failures;
      continue;
    }
    // Rebuilt only when the heights it has are too far from the drawn ones: what they stood off
    // by, on the tile the walker stands on, is the error the walker was held at until now. A
    // tile further off may wait behind nearer ones for several updates, and nobody stands on it.
    const f64 stale = stale_of(*t);
    const bool underfoot = on_tile && order_[k] == underfoot_key;
    if (stale > (k < under_count ? underfoot_limit : limit)) {
      if (underfoot) stats_.max_stale_m = stale > stats_.max_stale_m ? stale : stats_.max_stale_m;
      drop_ground_body(*t);
      blend_heights(*t, time_.blend);
      if (!make_ground_body(*t)) ++stats_.failures;
      ++stats_.refreshes;
      rebuilt = true;
    } else if (underfoot) {
      stats_.max_kept_m = stale > stats_.max_kept_m ? stale : stats_.max_kept_m;
    }
    const i64 dt = time::monotonic_ns() - start;
    stats_.refresh_ns += dt;
    stats_.max_refresh_ns = dt > stats_.max_refresh_ns ? dt : stats_.max_refresh_ns;
    ++done;
  }
  if (rebuilt) physics_->optimize_broad_phase();  // `commit`'s reason
  note_bodies();
  return done;
}

bool SceneCollision::ground_height(WorldPos at, f32& out) const noexcept {
  const i64 xm = floor_mm(at.x);
  const i64 zm = floor_mm(at.z);
  const i64 tx = floor_div(xm, tile_mm_);
  const i64 tz = floor_div(zm, tile_mm_);
  if (!fits_i32(tx) || !fits_i32(tz)) return false;
  const world::TileCoord coord{static_cast<i32>(tx), static_cast<i32>(tz)};
  const Tile* tile = tiles_.find_value(world::tile_key(coord));
  if (tile == nullptr || tile->heights.empty()) return false;
  // Where the point is in the tile's window, in lattice steps from its first point: the point's
  // millimetres less the first point's, which are an integer — a difference of two numbers within a
  // tile of each other, so f64 holds it to a nanometre anywhere, and a lattice point lands on a
  // whole number exactly (ADR-0053). It was `x / spacing - i0` in f32 x until 2026-10-05, which at
  // 420 km asked for the height 1.6 cm from the feet.
  const f64 spacing = static_cast<f64>(spacing_mm_);
  const f64 fx = (at.x * 1000.0 - static_cast<f64>(tile->i0 * spacing_mm_)) / spacing;
  const f64 fz = (at.z * 1000.0 - static_cast<f64>(tile->j0 * spacing_mm_)) / spacing;
  const i64 i = static_cast<i64>(std::floor(fx));
  const i64 j = static_cast<i64>(std::floor(fz));
  if (i < 0 || j < 0 || i + 1 >= tile->n || j + 1 >= tile->n) return false;
  // Over a hole there is no ground: a cell with a corner that is no sample (`hole_sample`).
  if (holes_reach(*tile)) {
    for (i64 dj = 0; dj < 2; ++dj) {
      for (i64 di = 0; di < 2; ++di) {
        if (hole_sample(tile->i0 + i + di, tile->j0 + j + dj)) return false;
      }
    }
  }
  const u32 n = tile->n;
  const auto sample = [&](i64 a, i64 b) {
    return tile->heights[static_cast<u32>(b) * n + static_cast<u32>(a)];
  };
  out = cell_height(sample(i, j), sample(i + 1, j), sample(i, j + 1), sample(i + 1, j + 1),
                    static_cast<f32>(fx - static_cast<f64>(i)),
                    static_cast<f32>(fz - static_cast<f64>(j)));
  return true;
}

bool SceneCollision::held_tile(u32 index, HeldTile& out) const noexcept {
  if (index >= tiles_.size()) return false;
  const Tile& tile = tiles_.value_at(index);
  out = HeldTile{};
  out.coord = tile.coord;
  out.corner = tile_corner(tile.coord);
  if (tile.ground) {
    out.i0 = tile.i0;
    out.j0 = tile.j0;
    out.n = tile.n;
    out.heights = std::span<const f32>(tile.heights.data(), tile.heights.size());
  }
  out.spacing_m = static_cast<f64>(spacing_mm_) / 1000.0;
  // The records are kept always now (the casts and the supports read them, A43); a debug draw's
  // count is still "0 unless kept", as the header says.
  out.pieces = config_.keep_shapes ? tile.pieces_held.size() : 0u;
  return true;
}

bool SceneCollision::held_piece(u32 tile_index, u32 piece, HeldPiece& out) const noexcept {
  if (tile_index >= tiles_.size()) return false;
  const Tile& tile = tiles_.value_at(tile_index);
  if (piece >= tile.pieces_held.size()) return false;
  const PieceRecord& record = tile.pieces_held[piece];
  out = HeldPiece{};
  out.mesh = record.mesh;
  out.instance = record.instance;
  out.entry = record.entry;
  out.offset = record.offset;
  out.rotation = record.rotation;
  if (record.proxy != nullptr) {
    out.vertices =
        std::span<const Vec3>(record.proxy->vertices.data(), record.proxy->vertices.size());
    out.indices = std::span<const u32>(record.proxy->indices.data(), record.proxy->indices.size());
    out.parts = std::span<const u32>(record.proxy->parts.data(), record.proxy->parts.size());
  }
  return true;
}

// ---- what a module can ask (docs: "What a module can ask"; roadmap A43) ----------------------

namespace {
// A limb steeper than this from level is a trunk or a riser, not something to land on (sin 60°).
constexpr f32 k_limb_max_rise = 0.8660254f;
// A top whose surface leans further than this from level is not one to stand on (cos 40°, the
// walker's own slope limit, `WalkParams::max_slope_deg`).
constexpr f32 k_top_min_up = 0.7660444f;
// How far above a piece's bounds the ray down onto its top starts, metres.
constexpr f32 k_top_ray_lift_m = 0.25f;

i32 floor_tile(f64 metres, i64 tile_mm) noexcept {
  const f64 mm = std::floor(metres * 1000.0 + 0.5);
  const i64 v = static_cast<i64>(mm);
  i64 q = v / tile_mm;
  if (v % tile_mm != 0 && v < 0) --q;
  return static_cast<i32>(q);
}
}  // namespace

// A streamed piece with an id carries bit 62, so its name never equals a scene instance's index
// (below 2^32) nor the hashed third form (bit 63); without it entry 0's piece n was scene instance
// n, one door key for two doors. An entry has bits 32 to 61, so one at `k_placement_entry_limit` or
// past it (a scene of a billion entries) takes the third form instead.
u64 support_placement(u32 instance, u32 entry, u32 id, i32 tile_x, i32 tile_z, u32 index) noexcept {
  static_assert(k_placement_entry_limit <= u64{1} << 30, "an entry would reach the tag at bit 62");
  if (instance != k_no_index) return instance;
  if (id != 0 && entry < k_placement_entry_limit)
    return k_placement_streamed_tag | (u64{entry} << 32) | id;
  u64 h = hash_combine(k_hash_seed, entry);
  h = hash_combine(h, static_cast<u32>(tile_x));
  h = hash_combine(h, static_cast<u32>(tile_z));
  return hash_combine(h, index) | (u64{1} << 63);
}

// The supports a proxy carries (docs: "Supports"), in its own frame — the mesh's at the proxy's
// scale, as its triangles are: the bounds of those triangles, and the perches the mesh declares,
// else the parts of its structural table. A limb's radius is scaled by the smallest of the scale's
// axes, the size a foot can trust.
SceneCollision::SupportSource SceneCollision::support_source(
    const renderer::SourceMesh& source) noexcept {
  SupportSource out;
  out.perches =
      std::span<const geometry::ClusterFilePerch>(source.perches.data(), source.perches.size());
  out.structural_parts = std::span<const geometry::ClusterFileStructuralPart>(
      source.structural_parts.data(), source.structural_parts.size());
  out.motion_parts = std::span<const geometry::ClusterFileMotionPart>(source.motion_parts.data(),
                                                                      source.motion_parts.size());
  out.doors = std::span<const geometry::ClusterFileDoor>(source.doors.data(), source.doors.size());
  out.species = &source.species;
  out.has_motion = source.has_motion();
  return out;
}

void SceneCollision::fill_supports(const SupportSource* source, Vec3 scale,
                                   const PlacementScratch& scratch, Proxy& proxy) {
  proxy.supports.clear();
  proxy.bare_tree = false;
  proxy.lo = Vec3{};
  proxy.hi = Vec3{};
  if (!scratch.vertices.empty()) {
    proxy.lo = scratch.vertices[0];
    proxy.hi = scratch.vertices[0];
    for (const Vec3& v : scratch.vertices) {
      proxy.lo =
          Vec3{std::min(proxy.lo.x, v.x), std::min(proxy.lo.y, v.y), std::min(proxy.lo.z, v.z)};
      proxy.hi =
          Vec3{std::max(proxy.hi.x, v.x), std::max(proxy.hi.y, v.y), std::max(proxy.hi.z, v.z)};
    }
  }
  proxy.doors.clear();
  proxy.door_scale = 1.0f;
  if (source == nullptr) return;
  // The leaves its mesh declares (section 53; docs: "Doors"), kept as recorded with the scale a
  // door is placed at: a door each wherever a held tile holds the piece (`take_declared_doors`).
  for (const geometry::ClusterFileDoor& d : source->doors)
    proxy.doors.push_back(d);
  proxy.door_scale = scale.x;
  const auto scaled = [&](Vec3 p) { return Vec3{p.x * scale.x, p.y * scale.y, p.z * scale.z}; };
  const f32 smallest =
      std::min(std::fabs(scale.x), std::min(std::fabs(scale.y), std::fabs(scale.z)));
  if (!source->perches.empty()) {
    for (u32 i = 0; i < source->perches.size(); ++i) {
      const geometry::ClusterFilePerch& p = source->perches[i];
      LocalSupport s;
      s.kind = k_support_perch;
      s.part = i;
      s.a = scaled(p.position);
      // A direction under a non-uniform scale goes by the inverse scale, as a normal does.
      const Vec3 up{p.up.x / (scale.x != 0.0f ? scale.x : 1.0f),
                    p.up.y / (scale.y != 0.0f ? scale.y : 1.0f),
                    p.up.z / (scale.z != 0.0f ? scale.z : 1.0f)};
      s.up = length(up) > 0.0f ? normalize(up) : Vec3{0.0f, 1.0f, 0.0f};
      s.r0 = 0.5f * p.size_m * smallest;
      s.r1 = s.r0;
      proxy.supports.push_back(s);
    }
    return;
  }
  if (source->species == nullptr) return;
  const geometry::FloraSpeciesInfo* species = geometry::flora_species_info(*source->species);
  if (species == nullptr || species->motion != geometry::FloraMotionKind::parts) return;
  // A tree with neither (an imported tree, a solver that writes no table): its top alone, and a
  // line the first time a module asks near it (`piece_supports`).
  proxy.bare_tree = source->structural_parts.empty() && source->has_motion;
  // Every part the table gives a radius, load-bearing or not: a bird perches on a branch no walker
  // could stand on. The size a caller asks for (`min_size`) is what leaves the twigs out.
  for (const geometry::ClusterFileStructuralPart& record : source->structural_parts) {
    if (record.part >= source->motion_parts.size() || !(record.base_radius > 0.0f)) continue;
    const geometry::ClusterFileMotionPart& part = source->motion_parts[record.part];
    LocalSupport s;
    s.kind = k_support_limb;
    s.part = record.part;
    s.a = scaled(part.pivot);
    s.b = scaled(part.pivot + part.direction * part.length);
    s.r0 = record.base_radius * smallest;
    s.r1 = record.tip_radius * smallest;
    proxy.supports.push_back(s);
  }
}

HitInfo SceneCollision::describe_hit(physics::BodyId body, u32 child) const noexcept {
  HitInfo out;
  if (!body) return out;
  for (u32 i = 0; i < tiles_.size(); ++i) {
    const Tile& tile = tiles_.value_at(i);
    if (tile.ground == body) {
      out.kind = HitKind::ground;
      return out;
    }
    if (tile.placements == body) {
      out.kind = HitKind::piece;
      if (child < tile.pieces_held.size()) {
        const PieceRecord& record = tile.pieces_held[child];
        out.mesh = record.mesh;
        out.instance = record.instance;
        out.entry = record.entry;
      }
      return out;
    }
  }
  for (const HeldProp& held : props_) {  // a game's prop (P45)
    if (!(held.body == body)) continue;
    out.kind = HitKind::prop;
    out.mesh = held.prop.mesh;
    out.instance = held.prop.key;
    return out;
  }
  for (u32 i = 0; i < door_bodies_.size(); ++i) {  // a door's leaf (S91)
    if (!(door_bodies_[i].body == body)) continue;
    out.kind = HitKind::door;
    // The key's low half: a host's key whole, a declared door's as its low 32 bits (`door(key)`
    // and `doors()` give the whole key).
    out.instance = static_cast<u32>(doors_[i].key);
    return out;
  }
  return out;
}

bool SceneCollision::covers(WorldPos at) const noexcept {
  const world::TileCoord coord{floor_tile(at.x, tile_mm_), floor_tile(at.z, tile_mm_)};
  return tiles_.find_value(world::tile_key(coord)) != nullptr;
}

// The segment's (x, z) sampled every eighth of a tile, its two ends included, each point's tile
// asked whether it is held: a tile the segment only grazes for less than an eighth of a tile may be
// missed, which errs towards "covered" by at most that corner. Bounded: a 256 m cast over 32 m
// tiles is 65 looks.
bool SceneCollision::covers_segment(WorldPos a, WorldPos b) const noexcept {
  const f64 step = static_cast<f64>(tile_mm_) / 8000.0;
  const f64 dx = b.x - a.x;
  const f64 dz = b.z - a.z;
  const f64 run = std::sqrt(dx * dx + dz * dz);
  const u32 n = static_cast<u32>(std::min(4096.0, std::ceil(run / step)));
  for (u32 i = 0; i <= n; ++i) {
    const f64 t = n == 0 ? 0.0 : static_cast<f64>(i) / static_cast<f64>(n);
    if (!covers(WorldPos{a.x + dx * t, 0.0, a.z + dz * t})) return false;
  }
  return true;
}

void SceneCollision::piece_supports(const Tile& tile, u32 piece, WorldPos at, f32 radius,
                                    f32 min_size, std::span<Support> out,
                                    SupportAnswer& answer) const noexcept {
  const PieceRecord& record = tile.pieces_held[piece];
  const Proxy* proxy = record.proxy;
  if (proxy == nullptr) return;
  const WorldPos origin = tile_corner(tile.coord) + DVec3{record.offset};
  // A scene's instance is a piece of every tile its bounds reach (the bins), and answers once: from
  // the tile that holds its origin. A streamed placement is its own tile's alone.
  if (record.instance != k_no_index && (floor_tile(origin.x, tile_mm_) != tile.coord.x ||
                                        floor_tile(origin.z, tile_mm_) != tile.coord.z)) {
    return;
  }
  // The piece's bounds as a sphere, for the rejection: its centre and half diagonal.
  const Vec3 centre_local = (proxy->lo + proxy->hi) * 0.5f;
  const f32 reach = 0.5f * length(proxy->hi - proxy->lo);
  const WorldPos centre = origin + DVec3{rotate(record.rotation, centre_local)};
  if (length(centre - at) > static_cast<f64>(radius + reach)) return;
  HitInfo owner;
  owner.kind = HitKind::piece;
  owner.mesh = record.mesh;
  owner.instance = record.instance;
  owner.entry = record.entry;
  const u64 placement = support_placement(record.instance, record.entry, record.id, tile.coord.x,
                                          tile.coord.z, record.index);
  // Once a scene mesh (its proxies at every scale), once a proxy for a mesh a tile handed.
  u8* told = record.mesh < told_.size() ? &told_[record.mesh] : nullptr;
  if (proxy->bare_tree && !proxy->told && (told == nullptr || *told == 0)) {
    proxy->told = true;
    if (told != nullptr) *told = 1;
    ENGINE_LOG_INFO(log_collision,
                    "a tree with no structural table and no perch offers a module its top alone",
                    log::field("mesh", record.mesh), log::field("entry", record.entry),
                    log::field("instance", record.instance));
  }
  const auto offer = [&](u8 kind, u32 part, WorldPos position, Vec3 up, f32 size, Vec3 along) {
    if (size < min_size) return;
    const f64 d = length(position - at);
    if (d > static_cast<f64>(radius)) return;
    ++answer.found;
    // Insertion into the nearest so far: ties keep the earlier (the piece order, then the
    // support's own order), so the answer is the same for the same held tiles.
    u32 slot = answer.count;
    while (slot > 0 && static_cast<f64>(out[slot - 1].distance) > d)
      --slot;
    if (slot >= out.size()) return;
    const u32 last = answer.count < out.size() ? answer.count : static_cast<u32>(out.size()) - 1u;
    for (u32 k = last; k > slot; --k)
      out[k] = out[k - 1];
    Support& s = out[slot];
    s.position = position;
    s.up = up;
    s.size_m = size;
    s.distance = static_cast<f32>(d);
    s.kind = kind;
    s.piece = owner;
    s.part = part;
    s.along = along;
    s.placement = placement;
    if (answer.count < out.size()) ++answer.count;
  };
  if (proxy->supports.empty()) {
    // The top of its bounds: a ray down the top's centre onto the piece itself.
    const Vec3 top_local{centre_local.x, proxy->hi.y, centre_local.z};
    const WorldPos top = origin + DVec3{rotate(record.rotation, top_local)};
    const f32 height = proxy->hi.y - proxy->lo.y;
    physics::RayHit hit;
    const WorldPos from = top + DVec3{0.0, static_cast<f64>(k_top_ray_lift_m), 0.0};
    // What the collision made (`k_scene_surfaces`): a prop standing on the piece, static or moved,
    // covers its top alike.
    if (!physics_->cast_ray(from, Vec3{0.0f, -(height + 2.0f * k_top_ray_lift_m), 0.0f}, hit,
                            k_scene_surfaces)) {
      return;
    }
    // A triangle's normal is its winding's, which an asset may have either way round: the top's
    // faces the ray, which came from above.
    const Vec3 up = hit.normal.y < 0.0f ? hit.normal * -1.0f : hit.normal;
    if (up.y < k_top_min_up) return;
    // On this piece: this tile's child, or the same instance in a neighbouring tile's compound,
    // which stands in the same place.
    if (hit.body != tile.placements || hit.child != piece) {
      const HitInfo met = describe_hit(hit.body, hit.child);
      if (met.kind != HitKind::piece || record.instance == k_no_index ||
          met.instance != record.instance) {
        return;
      }
    }
    // Its size is the width of the foot it holds, as a limb's is its diameter: the bounds' smaller
    // extent across (A45; until then half of it, so `min_size` meant half as much for a top).
    const f32 width_x = proxy->hi.x - proxy->lo.x;
    const f32 width_z = proxy->hi.z - proxy->lo.z;
    offer(k_support_top, k_no_index, hit.position, up, std::min(width_x, width_z), Vec3{});
    return;
  }
  for (const LocalSupport& s : proxy->supports) {
    if (s.kind == k_support_perch) {
      offer(k_support_perch, s.part, origin + DVec3{rotate(record.rotation, s.a)},
            rotate(record.rotation, s.up), 2.0f * s.r0, Vec3{});
      continue;
    }
    // A limb: the point of its axis nearest the asker, on the axis's top, where the radius is.
    const Vec3 a = rotate(record.rotation, s.a);
    const Vec3 b = rotate(record.rotation, s.b);
    const Vec3 axis = b - a;
    const f32 len = length(axis);
    if (!(len > 0.0f) || std::fabs(axis.y) > k_limb_max_rise * len) continue;
    const Vec3 dir = axis * (1.0f / len);
    const Vec3 to = relative(at, origin) - a;
    f32 t = dot(to, dir) / len;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    // Toward the base where the tip is thinner than the foot asked for.
    const f32 r_t = s.r0 + (s.r1 - s.r0) * t;
    if (2.0f * r_t < min_size && s.r1 < s.r0 && 2.0f * s.r0 >= min_size) {
      t = (s.r0 - 0.5f * min_size) / (s.r0 - s.r1);
    }
    const f32 r = s.r0 + (s.r1 - s.r0) * t;
    // The limb's top: world up less its share along the axis.
    const Vec3 world_up{0.0f, 1.0f, 0.0f};
    const Vec3 lift = normalize(world_up - dir * dot(world_up, dir));
    offer(k_support_limb, s.part, origin + DVec3{a + dir * (t * len) + lift * r}, lift, 2.0f * r,
          dir);
  }
}

SupportAnswer SceneCollision::supports_near(WorldPos at, f32 radius, f32 min_size,
                                            std::span<Support> out) const noexcept {
  SupportAnswer answer;
  const f64 r = static_cast<f64>(radius);
  // Covered when the disc's bounding square lies over held tiles (its four corners' tiles and
  // the ones between them).
  const i32 x0 = floor_tile(at.x - r, tile_mm_);
  const i32 x1 = floor_tile(at.x + r, tile_mm_);
  const i32 z0 = floor_tile(at.z - r, tile_mm_);
  const i32 z1 = floor_tile(at.z + r, tile_mm_);
  answer.covered = true;
  for (i32 z = z0; z <= z1 && answer.covered; ++z) {
    for (i32 x = x0; x <= x1; ++x) {
      if (tiles_.find_value(world::tile_key(world::TileCoord{x, z})) == nullptr) {
        answer.covered = false;
        break;
      }
    }
  }
  // The held tiles in the consumer's order (sorted by key), each piece in its child order: the
  // same answer for the same held tiles, whatever order they came in.
  for (u32 i = 0; i < tiles_.size(); ++i) {
    const Tile& tile = tiles_.value_at(i);
    if (tile.coord.x < x0 || tile.coord.x > x1 || tile.coord.z < z0 || tile.coord.z > z1) continue;
    for (u32 p = 0; p < tile.pieces_held.size(); ++p)
      piece_supports(tile, p, at, radius, min_size, out, answer);
  }
  return answer;
}

bool SceneCollision::ground_body(physics::BodyId body) const noexcept {
  if (!body) return false;
  for (u32 i = 0; i < tiles_.size(); ++i) {
    if (tiles_.value_at(i).ground == body) return true;
  }
  return false;
}

// **The walker goes with the ground** (scene_collision.h). A rebuilt heightfield is a new static
// body under a capsule the backend moves only by its own sweep, so the ground's motion reaches the
// walker through nothing but this: the sand that drops away left a standing walker hovering on the
// contact it had until the next rebuild snapped it down (the owner's jump, 2026-09-29), and the
// sand that rose closed over its feet, where a heightfield's one-sided contact never pushes back,
// and at a day a second went over its head (tests/time_lapse_tests.cpp has both, before and after).
u32 SceneCollision::follow(physics::CharacterBody& walker) {
  const WorldPos feet = walker.feet();
  if (ground_ == nullptr || !time_.moving || !source_.moves()) {
    return refresh(feet, walker.config().radius);
  }
  // Standing on the ground, not on a placement: what is straight under the capsule's centre, from
  // just above its feet down past the round bottom's rise on the steepest ground it stands on
  // (0.3 (1 / cos 40 - 1) = 9 cm, and the backend's padding), is a held tile's heightfield.
  f32 before = 0.0f;
  bool standing = false;
  if (walker.state().ground == physics::Ground::OnGround && ground_height(feet, before)) {
    physics::RayHit hit;
    // Every body the collision made (`k_scene_surfaces`): a walker on a moved box or a door is on
    // that, not on the ground.
    standing = physics_->cast_ray(feet + DVec3{0.0, 0.05, 0.0},
                                  Vec3{0.0f, -(0.05f + walker.config().radius), 0.0f}, hit,
                                  k_scene_surfaces) &&
               ground_body(hit.body);
  }
  const u32 done = refresh(feet, walker.config().radius);
  f32 after_f32 = 0.0f;
  if (!ground_height(feet, after_f32)) return done;
  const f64 after = static_cast<f64>(after_f32);
  WorldPos to = feet;
  if (standing && after_f32 != before) {
    // Carried: the offset it stood at over the ground (a slope's rise under its round bottom, the
    // backend's padding) is kept, so a standing walker neither hovers nor sinks. Added to the new
    // height rather than the change to the feet, so feet that stood at or over the ground are at
    // or over it after, to the bit.
    to.y = after + (feet.y - static_cast<f64>(before));
    ++stats_.carries;
    stats_.max_carry_m = std::max(stats_.max_carry_m, std::fabs(after - static_cast<f64>(before)));
  }
  if (to.y < after) {
    // Under the collision ground: lifted onto it. Counted from a tenth of a millimetre: below that
    // it is the character settling a step's worth of gravity into a surface it stands on.
    const f64 lift = after - to.y;
    if (lift > 1.0e-4) ++stats_.lifts;
    stats_.max_lift_m = std::max(stats_.max_lift_m, lift);
    to.y = after;
  }
  if (to.y != feet.y) (void)walker.teleport(to);
  return done;
}

// ---- the placements
// -------------------------------------------------------------------------------

// The cut a proxy is made of (`proxy_for`, `tile_proxy_for`): see the header.
u32 SceneCollision::cut_proxy(const ProxySource& source, u32 mesh, Vec3 scale,
                              PlacementScratch& scratch, bool& coarsened) const {
  Vector<Vec3>& vertices = scratch.vertices;
  Vector<u32>& indices = scratch.indices;
  HashMap<u32, u32>& remap = scratch.remap;
  u32 left_out = 0;
  vertices.clear();
  indices.clear();
  scratch.parts.clear();
  coarsened = false;
  const ProxySource::Mesh* copied = source.mesh(mesh);
  if (copied == nullptr) return 0;
  // The threshold is in the mesh's own units (`proxy_threshold`). Under the copy's floor it has
  // clusters the copy did not keep, so the floor is cut instead (proxy_source.h, "The floor").
  f32 threshold = proxy_threshold(config_.mesh_error_m, scale);
  if (threshold < copied->floor) {
    threshold = copied->floor;
    coarsened = true;
  }
  // **The weld's table is sized for this cut** (W200): `clear` zeroes every bucket the table has,
  // and the scratch is reused, so after one large proxy every small cut paid for the large one's
  // table. The cut's corners bound its vertices; a table with more than four buckets a corner is
  // let go and grown again from empty. A run of cuts of one size keeps its table and allocates
  // nothing.
  u64 corners = 0;
  for (const ProxySource::Cluster& cluster : source.clusters(*copied))
    if (cluster.own_error <= threshold && threshold < cluster.parent_error)
      corners += 3u * static_cast<u64>(cluster.triangle_count);
  if (static_cast<u64>(remap.bucket_count()) > 4u * corners + 64u)
    remap = HashMap<u32, u32>{};
  else
    remap.clear();
  // **A tree collides by its wood** (scene_collision.md, "What gets no body"): a mesh that carries
  // the motion contract keeps a triangle only when all three corners are of a part that stays a
  // collider — the trunk, and a load-bearing limb where the structural table says so — so the
  // walker walks into a palm's crown, where the renderer's push bends the fronds aside, and is
  // still stopped by the trunk. The copy decided it once (`ProxySource::add`), holds the wood
  // alone, and counts per cluster what it left out.
  const std::span<const Vec3> points = source.points();
  const std::span<const u32> sources = source.sources();
  const std::span<const u32> triangles = source.triangles();
  const std::span<const u16> parts = source.parts();
  // A mirror's scale — an odd number of negative axes — reverses the winding; put it back, so the
  // faces face out.
  const bool mirrored = ((scale.x < 0.0f) != (scale.y < 0.0f)) != (scale.z < 0.0f);
  for (const ProxySource::Emptied& cluster : source.emptied(*copied))
    if (cluster.own_error <= threshold && threshold < cluster.parent_error)
      left_out += cluster.left_out;
  for (const ProxySource::Cluster& cluster : source.clusters(*copied)) {
    if (!(cluster.own_error <= threshold && threshold < cluster.parent_error)) continue;
    left_out += cluster.left_out;
    for (u32 t = 0; t < cluster.triangle_count; ++t) {
      const u32 packed = triangles[cluster.first_triangle + t];
      const u32 local[3] = {packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu};
      u32 corner[3];
      for (u32 k = 0; k < 3; ++k) {
        const u32 v = cluster.first_point + local[k];
        if (const u32* seen = remap.find_value(sources[v])) {
          corner[k] = *seen;
          continue;
        }
        const Vec3 p = points[v];
        corner[k] = vertices.size();
        vertices.push_back(Vec3{p.x * scale.x, p.y * scale.y, p.z * scale.z});
        remap.insert(sources[v], corner[k]);
      }
      if (corner[0] == corner[1] || corner[1] == corner[2] || corner[0] == corner[2]) continue;
      indices.push_back(corner[0]);
      indices.push_back(mirrored ? corner[2] : corner[1]);
      indices.push_back(mirrored ? corner[1] : corner[2]);
      if (config_.keep_shapes) scratch.parts.push_back(u32{parts[cluster.first_triangle + t]});
    }
  }
  return left_out;
}

// A mesh's proxy at a scale: the scene's cluster LOD DAG of the mesh cut where each cluster's own
// error is within `mesh_error_m` and its parent's is not (`geometry::lod_selects`' rule with the
// threshold in metres rather than pixels) — the coarsest level that stays that close to the
// surface — welded by source vertex so the backend sees shared edges, scaled into the instance's
// size. One shape per (mesh, scale), shared by every instance. **Thread-safe**: a build ahead on a
// worker and the tick may both ask, so the cache is looked in under its lock and every asker of a
// key gets the one shape the cache keeps. **The cut and the shape are built outside the lock**
// (T166), as a tile-handed mesh's are: with several workers building a drop's ring at once, a
// forest's trees — each at its own scale, so each its own proxy — were built one at a time behind
// it. Two threads may then build one key at once; the second finds the first's and drops its own,
// and nothing in a shape depends on the thread that built it. The scene's DAG is read only, and
// the cut is made in the asker's scratch.
// The cut, its supports and its backend shape, into `out`, with what the stats count: `proxy_for`'s
// and a prop's (`PropProxy`) alike. Touches nothing shared: the scene's DAG and the copy are read
// only, and the backend's `build_mesh` is thread-safe.
void SceneCollision::build_proxy(u32 mesh, Vec3 scale, PlacementScratch& scratch, Proxy& out,
                                 u32& left_out, bool& coarsened, u64& bytes) const {
  profiling::TraceStage proxy_stage("collision proxy");  // a scene mesh's, cut and built (T172)
  coarsened = false;
  bytes = 0;
  const i64 cut_start = time::monotonic_ns();
  left_out = cut_proxy(*proxy_source_, mesh, scale, scratch, coarsened);
  scratch.cut_ns = time::monotonic_ns() - cut_start;
  const Vector<Vec3>& vertices = scratch.vertices;
  const Vector<u32>& indices = scratch.indices;
  if (config_.keep_shapes) {
    out.vertices = vertices;
    out.indices = indices;
    out.parts = scratch.parts;
  }
  SupportSource supports;
  if (mesh < scene_->sources.size()) supports = support_source(scene_->sources[mesh]);
  fill_supports(mesh < scene_->sources.size() ? &supports : nullptr, scale, scratch, out);
  const i64 shape_start = time::monotonic_ns();
  if (!indices.empty() &&
      physics_->build_mesh(std::span<const Vec3>(vertices.data(), vertices.size()),
                           std::span<const u32>(indices.data(), indices.size()),
                           out.shape) == physics::Status::Ok) {
    out.triangles = indices.size() / 3;
    u32 triangles = 0;
    if (!physics_->built_memory(out.shape, false, bytes, triangles)) bytes = 0;
  }
  scratch.shape_ns = time::monotonic_ns() - shape_start;
}

// **A proxy a job may be building** (W200; docs: "A scene of authored meshes"): in the cache, it is
// taken; handed to a job that has not started, the asker takes it from the job and builds it here,
// as it did before; running on a job, the asker waits for that one proxy alone — never for the
// queue behind it — and takes what the job put in the cache.
const SceneCollision::Proxy* SceneCollision::proxy_for(u32 mesh, Vec3 scale,
                                                       PlacementScratch& scratch) {
  const u64 key = proxy_key(mesh, scale);
  // Taken from the queue under the lock, so the tick's withdrawal (`withdraw_proxies`) and an
  // asker never both think they moved it; one running is never withdrawn or made again, so it may
  // be waited for outside the lock.
  ProxyAhead* ahead = nullptr;
  {
    std::lock_guard<std::mutex> lock(proxies_mutex_);
    if (const std::unique_ptr<Proxy>* proxy = proxies_.find_value(key))
      return (*proxy)->shape ? proxy->get() : nullptr;
    if (ProxyAhead* const* found = ahead_by_key_.find_value(key)) {
      u32 expected = ProxyAhead::k_queued;
      if ((*found)->state.compare_exchange_strong(expected, ProxyAhead::k_done,
                                                  std::memory_order_acq_rel))
        ++proxy_stats_.ahead_claimed;
      else if (expected == ProxyAhead::k_running)
        ahead = *found;
    }
  }
  if (ahead != nullptr) {
    {
      const i64 start = time::monotonic_ns();
      ahead->ran.wait_blocking();
      const i64 waited = time::monotonic_ns() - start;
      std::lock_guard<std::mutex> lock(proxies_mutex_);
      ++proxy_stats_.ahead_waits;
      proxy_stats_.ahead_wait_ns += waited;
      proxy_stats_.max_ahead_wait_ns = std::max(proxy_stats_.max_ahead_wait_ns, waited);
    }
  }
  return cached_proxy_for(mesh, scale, scratch);
}

const SceneCollision::Proxy* SceneCollision::cached_proxy_for(u32 mesh, Vec3 scale,
                                                              PlacementScratch& scratch) {
  const u64 key = proxy_key(mesh, scale);
  {
    std::lock_guard<std::mutex> lock(proxies_mutex_);
    if (const std::unique_ptr<Proxy>* proxy = proxies_.find_value(key))
      return (*proxy)->shape ? proxy->get() : nullptr;
  }
  const i64 start = time::monotonic_ns();
  auto proxy = std::make_unique<Proxy>();
  u32 left_out = 0;
  bool coarsened = false;
  u64 bytes = 0;
  build_proxy(mesh, scale, scratch, *proxy, left_out, coarsened, bytes);
  std::lock_guard<std::mutex> lock(proxies_mutex_);
  proxy_stats_.ns += time::monotonic_ns() - start;
  proxy_stats_.cut_ns += scratch.cut_ns;
  proxy_stats_.shape_ns += scratch.shape_ns;
  if (const std::unique_ptr<Proxy>* found = proxies_.find_value(key))
    return (*found)->shape ? found->get() : nullptr;  // another thread's: this one goes
  proxy_stats_.flora_left_out += left_out;
  proxy_stats_.coarsened += coarsened ? 1u : 0u;
  proxy_stats_.triangles += proxy->triangles;
  proxy_stats_.bytes += bytes;
  const Proxy* out = proxy->shape ? proxy.get() : nullptr;
  proxies_.try_emplace(key, std::move(proxy));
  return out;
}

// **The proxy of a mesh a tile handed** (scene_collision.md, "A tile's meshes"). The mesh has no
// place in the scene's merged DAG — it arrives with its tile — so the proxy is cut from the mesh as
// `renderer::load_placement_mesh` builds it: the load's own per-mesh pass, no device, into the
// cache entry the renderer's residency reads or wrote, so on a warm cache this is a read. Keyed by
// the mesh's identity (the generator and the recipe's bytes), never by where a tile listed it, so
// two tiles handing one recipe share one shape. **The build is outside the proxies' lock** — a
// recipe on a cold cache is a solve, and the tick must not wait behind a worker's — so two threads
// may build one key at once; the second finds the first's and drops its own.
const SceneCollision::Proxy* SceneCollision::tile_proxy_for(const scene_gen::PlacementMesh& mesh,
                                                            Vec3 scale, PlacementScratch& scratch,
                                                            Vector<u64>& held) {
  const u64 identity = renderer::placement_mesh_identity(mesh);
  u64 key = hash_combine(hash_combine(identity, bits(scale.x)), bits(scale.y));
  key = hash_combine(key, bits(scale.z));
  const auto take = [&](TileProxy& found) -> const Proxy* {
    bool counted = false;
    for (const u64 k : held)
      counted = counted || k == key;
    if (!counted) {
      ++found.refs;
      held.push_back(key);
    }
    return found.proxy.shape ? &found.proxy : nullptr;
  };
  {
    std::lock_guard<std::mutex> lock(proxies_mutex_);
    if (const std::unique_ptr<TileProxy>* found = tile_proxies_.find_value(key))
      return take(**found);
  }
  const i64 start = time::monotonic_ns();
  profiling::TraceStage proxy_stage("collision tile proxy");  // a tile-handed mesh's (T172)
  // **From a kept copy** (T166; docs: "Ahead of a running walker"): the cut a fresh read gives,
  // word for word, without reading the mesh's container again for every tile that places it.
  if (MeshCopy* copy =
          acquire_copy(mesh, identity, proxy_threshold(config_.mesh_error_m, scale), scratch)) {
    auto made = std::make_unique<TileProxy>();
    u32 left_out = 0;
    if (copy->ok) {
      bool coarsened = false;
      left_out = cut_proxy(copy->copy, copy->index, scale, scratch, coarsened);
      if (config_.keep_shapes) {
        made->proxy.vertices = scratch.vertices;
        made->proxy.indices = scratch.indices;
        made->proxy.parts = scratch.parts;
      }
      SupportSource supports;
      supports.perches =
          std::span<const geometry::ClusterFilePerch>(copy->perches.data(), copy->perches.size());
      supports.structural_parts = std::span<const geometry::ClusterFileStructuralPart>(
          copy->structural_parts.data(), copy->structural_parts.size());
      supports.motion_parts = std::span<const geometry::ClusterFileMotionPart>(
          copy->motion_parts.data(), copy->motion_parts.size());
      supports.doors =
          std::span<const geometry::ClusterFileDoor>(copy->doors.data(), copy->doors.size());
      supports.species = &copy->species;
      supports.has_motion = copy->has_motion;
      fill_supports(&supports, scale, scratch, made->proxy);
    }
    const bool cut = copy->ok;
    release_copy(copy);
    if (cut && !scratch.indices.empty() &&
        physics_->build_mesh(
            std::span<const Vec3>(scratch.vertices.data(), scratch.vertices.size()),
            std::span<const u32>(scratch.indices.data(), scratch.indices.size()),
            made->proxy.shape) == physics::Status::Ok) {
      made->proxy.triangles = scratch.indices.size() / 3;
      u32 triangles = 0;
      (void)physics_->built_memory(made->proxy.shape, false, made->bytes, triangles);
    }
    std::lock_guard<std::mutex> lock(proxies_mutex_);
    proxy_stats_.ns += time::monotonic_ns() - start;
    if (const std::unique_ptr<TileProxy>* found = tile_proxies_.find_value(key))
      return take(**found);  // another thread's, made meanwhile: this one goes with `made`
    proxy_stats_.triangles += made->proxy.triangles;
    proxy_stats_.bytes += made->bytes;
    proxy_stats_.flora_left_out += left_out;
    TileProxy& kept = *made;
    tile_proxies_.try_emplace(key, std::move(made));
    return take(kept);
  }
  if (scratch.tile_mesh == nullptr || scratch.tile_mesh_identity != identity) {
    if (scratch.tile_mesh == nullptr)
      scratch.tile_mesh = std::make_unique<renderer::PlacementMeshData>();
    std::string why;
    scratch.tile_mesh_identity = identity;
    scratch.tile_mesh_ok =
        renderer::load_placement_mesh(mesh, scene_->mesh_options, *scratch.tile_mesh, why);
    if (!scratch.tile_mesh_ok) {
      ENGINE_LOG_WARN(log_collision, "a tile's mesh has no collision: it was not built",
                      log::field("mesh", mesh.path), log::field("error", why));
    }
  }
  auto made = std::make_unique<TileProxy>();
  u32 left_out = 0;
  if (scratch.tile_mesh_ok) {
    const renderer::PlacementMeshData& data = *scratch.tile_mesh;
    // Copied at the one threshold this scale asks (proxy_source.h), so the cut is exact.
    scratch.tile_source.clear();
    const u32 copied = scratch.tile_source.add(data.lod, data.part,
                                               data.source.has_motion() ? &data.source : nullptr,
                                               proxy_threshold(config_.mesh_error_m, scale));
    bool coarsened = false;
    left_out = cut_proxy(scratch.tile_source, copied, scale, scratch, coarsened);
    if (config_.keep_shapes) {
      made->proxy.vertices = scratch.vertices;
      made->proxy.indices = scratch.indices;
      made->proxy.parts = scratch.parts;
    }
    const SupportSource supports = support_source(data.source);
    fill_supports(&supports, scale, scratch, made->proxy);
    if (!scratch.indices.empty() &&
        physics_->build_mesh(
            std::span<const Vec3>(scratch.vertices.data(), scratch.vertices.size()),
            std::span<const u32>(scratch.indices.data(), scratch.indices.size()),
            made->proxy.shape) == physics::Status::Ok) {
      made->proxy.triangles = scratch.indices.size() / 3;
      u32 triangles = 0;
      (void)physics_->built_memory(made->proxy.shape, false, made->bytes, triangles);
    }
  }
  std::lock_guard<std::mutex> lock(proxies_mutex_);
  proxy_stats_.ns += time::monotonic_ns() - start;
  if (const std::unique_ptr<TileProxy>* found = tile_proxies_.find_value(key))
    return take(**found);  // another thread's, made meanwhile: this one goes with `made`
  proxy_stats_.triangles += made->proxy.triangles;
  proxy_stats_.bytes += made->bytes;
  proxy_stats_.flora_left_out += left_out;
  TileProxy& kept = *made;
  tile_proxies_.try_emplace(key, std::move(made));
  return take(kept);
}

void SceneCollision::release_tile_proxies(Vector<u64>& held) noexcept {
  if (held.empty()) return;
  std::lock_guard<std::mutex> lock(proxies_mutex_);
  for (const u64 key : held) {
    const std::unique_ptr<TileProxy>* found = tile_proxies_.find_value(key);
    if (found == nullptr || (*found)->refs == 0) continue;
    if (--(*found)->refs != 0) continue;
    proxy_stats_.triangles -= (*found)->proxy.triangles;
    proxy_stats_.bytes -= (*found)->bytes;
    tile_proxies_.erase(key);
  }
  held.clear();
}

// **A tile-handed mesh's kept copy** (T166; docs: "Ahead of a running walker"). Until T166 every
// build read each tile-handed mesh's whole container again (its scratch kept the last one alone):
// on the forest's staging about 100 MB and 34 to 135 ms of a tile's 170 to 500 ms. A copy keeps
// what a proxy is cut from (proxy_source.h) at a floor `k_copy_scale_headroom` finer than the scale
// first asked, so a placement up to that much larger is cut from it exactly; a larger one reads the
// mesh again and replaces the copy. The read is outside the lock, as `tile_proxy_for`'s is: two
// threads may read one mesh at once, and the second's copy replaces the first's (or is used once).
SceneCollision::MeshCopy* SceneCollision::acquire_copy(const scene_gen::PlacementMesh& mesh,
                                                       u64 identity, f32 threshold,
                                                       PlacementScratch& scratch) {
  if (config_.mesh_copy_bytes == 0) return nullptr;
  {
    std::lock_guard<std::mutex> lock(copies_mutex_);
    if (const std::unique_ptr<MeshCopy>* found = mesh_copies_.find_value(identity)) {
      MeshCopy& copy = **found;
      if (!copy.ok || threshold >= copy.floor) {
        ++copy.users;
        copy.used = ++copies_clock_;
        ++copies_hits_;
        return &copy;
      }
    }
  }
  auto made = std::make_unique<MeshCopy>();
  if (scratch.tile_mesh == nullptr)
    scratch.tile_mesh = std::make_unique<renderer::PlacementMeshData>();
  std::string why;
  scratch.tile_mesh_identity = identity;
  scratch.tile_mesh_ok =
      renderer::load_placement_mesh(mesh, scene_->mesh_options, *scratch.tile_mesh, why);
  if (!scratch.tile_mesh_ok) {
    ENGINE_LOG_WARN(log_collision, "a tile's mesh has no collision: it was not built",
                    log::field("mesh", mesh.path), log::field("error", why));
  } else {
    const renderer::PlacementMeshData& data = *scratch.tile_mesh;
    made->ok = true;
    made->floor = threshold / k_copy_scale_headroom;
    made->index = made->copy.add(data.lod, data.part,
                                 data.source.has_motion() ? &data.source : nullptr, made->floor);
    for (const geometry::ClusterFilePerch& p : data.source.perches)
      made->perches.push_back(p);
    for (const geometry::ClusterFileStructuralPart& p : data.source.structural_parts)
      made->structural_parts.push_back(p);
    for (const geometry::ClusterFileMotionPart& p : data.source.motion_parts)
      made->motion_parts.push_back(p);
    for (const geometry::ClusterFileDoor& d : data.source.doors)
      made->doors.push_back(d);
    made->species = data.source.species;
    made->has_motion = data.source.has_motion();
  }
  made->bytes = sizeof(MeshCopy) + made->copy.bytes() +
                made->perches.capacity() * sizeof(geometry::ClusterFilePerch) +
                made->structural_parts.capacity() * sizeof(geometry::ClusterFileStructuralPart) +
                made->motion_parts.capacity() * sizeof(geometry::ClusterFileMotionPart) +
                made->doors.capacity() * sizeof(geometry::ClusterFileDoor) +
                made->species.capacity();
  std::lock_guard<std::mutex> lock(copies_mutex_);
  ++copies_reads_;
  made->users = 1;
  made->used = ++copies_clock_;
  MeshCopy* out = made.get();
  if (std::unique_ptr<MeshCopy>* found = mesh_copies_.find_value(identity)) {
    copies_bytes_ -= (*found)->bytes;
    // One in use now goes when its last cut gives it back (`release_copy`).
    if ((*found)->users != 0) copies_orphans_.push_back(std::move(*found));
    *found = std::move(made);
  } else {
    mesh_copies_.try_emplace(identity, std::move(made));
  }
  copies_bytes_ += out->bytes;
  // Over the budget: the least recently used copies nobody is reading go, oldest first.
  while (copies_bytes_ > config_.mesh_copy_bytes) {
    u32 oldest = mesh_copies_.size();
    for (u32 i = 0; i < mesh_copies_.size(); ++i) {
      const MeshCopy& c = *mesh_copies_.value_at(i);
      if (c.users == 0 &&
          (oldest == mesh_copies_.size() || c.used < mesh_copies_.value_at(oldest)->used))
        oldest = i;
    }
    if (oldest == mesh_copies_.size()) break;
    copies_bytes_ -= mesh_copies_.value_at(oldest)->bytes;
    const u64 gone = mesh_copies_.key_at(oldest);
    mesh_copies_.erase(gone);
  }
  return out;
}

void SceneCollision::release_copy(MeshCopy* copy) noexcept {
  std::lock_guard<std::mutex> lock(copies_mutex_);
  if (--copy->users != 0) return;
  for (u32 k = 0; k < copies_orphans_.size(); ++k) {
    if (copies_orphans_[k].get() != copy) continue;
    copies_orphans_[k] = std::move(copies_orphans_[copies_orphans_.size() - 1]);
    copies_orphans_.pop_back();
    return;
  }
}

bool SceneCollision::add_tile_piece(const scene_gen::PlacementMesh& mesh, const Mat4& linear,
                                    WorldPos at, WorldPos corner, PlacementScratch& scratch,
                                    Vector<u64>& held, u32 entry) {
  Placement placed;
  if (!decompose(linear, placed)) return false;
  const Proxy* proxy = tile_proxy_for(mesh, placed.scale, scratch, held);
  if (proxy == nullptr) return false;
  physics::BuiltChild child;
  child.shape = &proxy->shape;
  child.transform.position = relative(at, corner);
  child.transform.rotation = placed.rotation;
  scratch.children.push_back(child);
  // The piece's record, always, in child order (a cast's `child` names it; A43).
  PieceRecord record;
  record.mesh = k_no_index;  // no scene mesh: it arrived with the tile
  record.entry = entry;
  record.offset = child.transform.position;
  record.rotation = placed.rotation;
  record.proxy = proxy;  // resident while the tile holds its reference
  scratch.records.push_back(record);
  return true;
}

WorldPos SceneCollision::tile_corner(world::TileCoord coord) const noexcept {
  return WorldPos{static_cast<f64>(i64{coord.x} * tile_mm_) / 1000.0, 0.0,
                  static_cast<f64>(i64{coord.z} * tile_mm_) / 1000.0};
}

bool SceneCollision::add_piece(u32 mesh, const Mat4& linear, WorldPos at, WorldPos corner,
                               PlacementScratch& scratch, u32 instance, u32 entry) {
  if (mesh >= scene_->parts.size() || mesh == scene_->terrain_mesh) return false;
  if (water_mesh(*scene_, mesh) || far_mesh(mesh)) return false;
  Placement placed;
  if (!decompose(linear, placed)) return false;
  const Proxy* proxy = proxy_for(mesh, placed.scale, scratch);
  if (proxy == nullptr) return false;
  physics::BuiltChild child;
  child.shape = &proxy->shape;
  // In the compound's frame, which is the tile's corner: a few tens of metres at most, the
  // difference taken in f64 and rounded once.
  child.transform.position = relative(at, corner);
  child.transform.rotation = placed.rotation;
  scratch.children.push_back(child);
  // The piece's record, always, in child order (a cast's `child` names it; A43).
  PieceRecord record;
  record.mesh = mesh;
  record.instance = instance;
  record.entry = entry;
  record.offset = child.transform.position;
  record.rotation = placed.rotation;
  record.proxy = proxy;  // the cache's: its address is stable for the consumer's life
  scratch.records.push_back(record);
  return true;
}

// Every piece on the tile as one static compound of shared proxies: one body a tile whatever the
// tile holds (a ruin laid in blocks is hundreds of pieces), with the compound's own tree doing the
// culling the broadphase would otherwise do per piece. Its shape is built here, on whichever thread
// asks, and the body made of it on the tick (`build_placements`).
void SceneCollision::gather_placements(world::TileCoord coord, PlacementScratch& scratch,
                                       TilePlacements& out) {
  out.compound.reset();
  out.pieces = 0;
  out.compound_bytes = 0;
  out.ok = true;
  out.records.clear();
  release_tile_proxies(out.tile_proxies);
  scratch.children.clear();
  scratch.records.clear();
  const u64 key = world::tile_key(coord);
  const WorldPos corner = tile_corner(coord);
  if (!entries_.empty()) {
    const scene_gen::TileCoord at{coord.x, coord.z};
    // **Each entry is handed what the entries before it put on the tile, as the drawn tile's build
    // hands it** (T114; docs: "The entries before this one"): their placements' discs and then the
    // shapes they said they occupy in `earlier`, and those shapes in the tile's exclusion, made the
    // first time an entry of this tile hands one — `world::PlacementTiles::build`'s rule, read and
    // followed, so the collision's tile is the drawn tile's. Without them a scatter layer that kept
    // off a tread on the drawn tile made bodies on it here: rocks nobody could see, in the way. Per
    // gather and on this thread, since a build ahead gathers on a worker; a polygon's points are
    // the list's own (`FootprintList`), because `scratch.placed` is the next entry's by then.
    scene_gen::FootprintList earlier;
    FlatMap<u32, f32> reach_of;  // by scene mesh
    scene_gen::Exclusion shaped;
    bool has_shaped = false;
    for (u32 e = 0; e < entries_.size(); ++e) {
      const Entry& entry = *entries_[e];
      if (entry.generator->occupies != nullptr && !entry.generator->occupies(entry.state, at))
        continue;
      scratch.placed.clear();
      std::string why;
      bool made = false;
      {
        scene_gen::Context context = context_for(entry);
        context.earlier = earlier.span();
        if (has_shaped) context.exclusion = &shaped;
        // **Side by side where the generator says so** (T169; docs: "Ahead of a running walker"):
        // a reentrant `tile` is asked on whichever thread builds the tile, beside the others; any
        // other under the one lock, one thread at a time, as every one was before. The startup
        // trace names each ask by its generator and the wait for the lock apart (T172).
        if (entry.generator->tile_reentrant) {
          profiling::TraceStage ask("collision generator", entry.generator->name);
          made = entry.generator->tile(entry.state, at, context, scratch.placed, &why);
        } else {
          profiling::TraceStage wait("collision generator lock", entry.generator->name);
          std::lock_guard<std::mutex> lock(generators_mutex_);
          wait.end();
          profiling::TraceStage ask("collision generator", entry.generator->name);
          made = entry.generator->tile(entry.state, at, context, scratch.placed, &why);
        }
      }
      if (!made) {
        out.ok = false;
        ENGINE_LOG_WARN(log_collision, "a tile's placements were not made",
                        log::field("entry", entry.source->where), log::field("x", coord.x),
                        log::field("z", coord.z), log::field("error", why));
        continue;
      }
      const bool later = e + 1 < entries_.size();  // an entry after it reads what it placed
      if (later && !scratch.placed.shapes.empty()) {
        profiling::TraceStage shaped_stage("collision exclusion");  // T172
        if (!has_shaped) shaped = scene_->terrain.exclusion;
        has_shaped = true;
        if (!shaped.add_footprints(scratch.placed.shapes.span(), &why)) {
          out.ok = false;
          ENGINE_LOG_WARN(log_collision, "a tile's shape was refused",
                          log::field("entry", entry.source->where), log::field("x", coord.x),
                          log::field("z", coord.z), log::field("error", why));
        }
      }
      // A placement's disc, as the drawn build makes it: at the placement's millimetre, its mesh's
      // reach on the ground (`renderer::ground_radius`, the tile's word for a mesh it handed) times
      // its larger ground scale.
      const auto disc = [&](const scene_gen::Placement& p, f32 reach) {
        if (!later) return;
        const f32 scale = std::max(std::fabs(p.scale.x), std::fabs(p.scale.z));
        earlier.add(scene_gen::Footprint{scene_gen::nearest_mm(p.position.x),
                                         scene_gen::nearest_mm(p.position.z),
                                         static_cast<f64>(reach * scale)});
      };
      const u32 resident = entry.source->meshes.size();
      // The piece's name for a module's supports (`support_placement`): the generator's, and its
      // place among the entry's placements on the tile, counted over every placement handed.
      u32 nth = 0;
      const auto name_piece = [&](bool added, const scene_gen::Placement& p) {
        if (added) {
          scratch.records.back().id = p.id;
          scratch.records.back().index = nth;
        }
        ++nth;
      };
      for (const scene_gen::Placement& p : scratch.placed.instances) {
        if (p.mesh >= resident) {
          // A mesh the tile handed (scene_gen.md, "A tile's meshes"): index `resident + k` is the
          // tile's own list. It stands where the renderer stands it — its fit is the identity.
          const u32 k = p.mesh - resident;
          if (k >= scratch.placed.meshes.size()) {
            ++nth;
            continue;
          }
          disc(p, scratch.placed.meshes[k].reach);
          const bool added = add_tile_piece(scratch.placed.meshes[k], mat4_from_transform(p.turn()),
                                            p.position, corner, scratch, out.tile_proxies, e);
          // Its leaves are drawn where the drawn tile hangs them, at their declared angle.
          if (added && renderer::placement_mesh_hangs_leaves(scratch.placed.meshes[k]))
            scratch.records.back().leaves = k_leaves_stand;
          name_piece(added, p);
          continue;
        }
        const u32 mesh = entry.source->meshes[p.mesh];
        if (later) {
          // Measured once a mesh a gather: a tile holds many placements of a few meshes.
          f32* reach = reach_of.find_value(mesh);
          if (reach == nullptr) {
            const geometry::ClusterMeshPart* part =
                mesh < scene_->parts.size() ? &scene_->parts[mesh] : nullptr;
            reach = &(reach_of[mesh] = part != nullptr ? renderer::ground_radius(
                                                             scene_->lod, part->first_cluster,
                                                             part->cluster_count)
                                                       : 0.0f);
          }
          disc(p, *reach);
        }
        const Mat4 fit = mesh < scene_->mesh_fit.size() ? scene_->mesh_fit[mesh] : Mat4::identity();
        // Where the renderer stands the same placement (`renderer::instance_translation`): the
        // fit's offset through the rotation and scale in float32, the size of the mesh, and the
        // sum with the placement's `WorldPos` in f64 — where its generator put it, anywhere.
        const Mat4 linear = mat4_from_transform(p.turn()) * fit;
        const WorldPos stands = p.position + DVec3{linear.c[3].xyz()};
        name_piece(add_piece(mesh, linear, stands, corner, scratch, k_no_index, e), p);
      }
      if (later)
        for (const scene_gen::Footprint& shape : scratch.placed.shapes.span())
          earlier.add(shape);
    }
  }
  if (const Vector<u32>* bin = bins_.find_value(key)) {
    for (const u32 i : *bin) {
      const gfx::InstanceDesc& instance = scene_->instances[i];
      if (add_piece(instance.mesh, cell_frame_matrix(instance),
                    to_world(gfx::instance_cell(instance)), corner, scratch, i, k_no_index) &&
          i < hung_houses_.size() && hung_houses_[i] != 0)
        scratch.records.back().leaves = k_leaves_turn;
    }
  }
  if (scratch.children.empty()) return;
  out.records = scratch.records;
  profiling::TraceStage compound_stage("collision compound");
  if (physics_->build_compound(
          std::span<const physics::BuiltChild>(scratch.children.data(), scratch.children.size()),
          out.compound) != physics::Status::Ok) {
    out.ok = false;
    return;
  }
  out.pieces = scratch.children.size();
  u32 triangles = 0;
  (void)physics_->built_memory(out.compound, false, out.compound_bytes, triangles);
}

// The tile's compound body, from what a build ahead gathered or, without one, gathered here: the
// shape adopted into the physics world's table and the body made, on the tick, after the ground's
// — the order and the ids every activation has always had.
bool SceneCollision::build_placements(Tile& tile, Build* build) {
  TilePlacements here;
  TilePlacements* placed = &here;
  if (build != nullptr && build->placements) {
    placed = &build->placed;
    ++stats_.placements_ahead;
  } else {
    const i64 start = time::monotonic_ns();
    gather_placements(tile.coord, tick_scratch_, here);
    stats_.placement_ns += time::monotonic_ns() - start;
  }
  tile.pieces = 0;
  // The references the gather took go with the tile, and are given back when it is let go
  // (`deactivate`) — or here, when the tile gets no body to hold them.
  tile.tile_proxies = std::move(placed->tile_proxies);
  placed->tile_proxies.clear();
  struct GiveBack {
    SceneCollision* self;
    Tile* tile;
    bool keep = false;
    ~GiveBack() {
      if (!keep) self->release_tile_proxies(tile->tile_proxies);
    }
  } give_back{this, &tile};
  if (!placed->compound) return placed->ok;
  const i64 body_start = time::monotonic_ns();
  struct BodyCount {
    i64 start;
    i64& into;
    ~BodyCount() { into += time::monotonic_ns() - start; }
  } body_count{body_start, stats_.body_ns};
  if (physics_->adopt_shape(placed->compound, tile.placements_shape) != physics::Status::Ok)
    return false;
  // The table holds the shape now; the build's own reference goes with it.
  placed->compound.reset();
  tile.compound_bytes = placed->compound_bytes;
  physics::BodyDesc body;
  body.shape = tile.placements_shape;
  body.transform.position = tile_corner(tile.coord);
  body.motion = physics::MotionType::Static;
  body.layer = physics::Layer::Static;
  body.friction = 0.8f;
  if (physics_->create_body(body, tile.placements) != physics::Status::Ok) {
    physics_->destroy_shape(tile.placements_shape);
    tile.placements_shape = {};
    return false;
  }
  tile.pieces = placed->pieces;
  give_back.keep = true;
  tile.pieces_held = std::move(placed->records);
  ++stats_.bodies_created;
  return placed->ok;
}

// ---- the tile built ahead of the ring
// --------------------------------------------------------------

bool SceneCollision::can_prefetch() const noexcept {
  if (jobs_ == nullptr || !(config_.prefetch_radius_tiles > 0.0f)) return false;
  // A moving ground is evaluated at the drawn time, which the activation's tick sets; and a
  // generator is handed the ground to stand its placements on, which a worker may read only where
  // the ground is a tile source's (a provider's own `grid` is not one the job system may share).
  if (ground_ == nullptr) return has_placements();
  return source_.valid() && !(time_.moving && source_.moves()) &&
         (explicit_source_ || ground_->ops()->tile_source != nullptr);
}

void SceneCollision::count_build(const Build& build) noexcept {
  // Only a build that ran: one let go before a worker reached it measured nothing.
  const i64 ns = build.ground_ns + build.placement_ns;
  if (ns <= 0) return;
  const f64 s = static_cast<f64>(ns) / 1.0e9;
  f64& estimate = stats_.build_estimate_s;
  estimate = estimate > 0.0 ? 0.75 * estimate + 0.25 * s : s;
}

void SceneCollision::retire(std::unique_ptr<Build>&& build) {
  if (build->ran.done()) {  // its job has returned: nothing else touches it
    count_build(*build);
    return;
  }
  retired_.push_back(std::move(build));
}

std::unique_ptr<SceneCollision::Build> SceneCollision::take_build(world::TileCoord coord) {
  for (u32 k = 0; k < builds_.size(); ++k) {
    Build& build = *builds_[k];
    if (build.coord.x != coord.x || build.coord.z != coord.z) continue;
    u32 expected = Build::k_queued;
    bool here = false;
    if (build.state.compare_exchange_strong(expected, Build::k_running,
                                            std::memory_order_acq_rel)) {
      // Not started: built here, on the tick — its own cost, as without a build ahead.
      build.compute();
      ++stats_.prefetch_inline;
      here = true;
    } else if (expected == Build::k_running) {
      // Running on a worker: this tile's build alone is waited for.
      const i64 from = time::monotonic_ns();
      build.ran.wait_blocking();
      const i64 waited = time::monotonic_ns() - from;
      ++stats_.prefetch_waits;
      stats_.prefetch_wait_ns += waited;
      stats_.max_prefetch_wait_ns = std::max(stats_.max_prefetch_wait_ns, waited);
    } else {
      ++stats_.prefetched;
    }
    count_build(build);
    // Where its time went: the activation's own split when the tick ran it, the worker's otherwise.
    if (here) {
      stats_.ground_sample_ns += build.ground_ns;
      stats_.max_ground_sample_ns = std::max(stats_.max_ground_sample_ns, build.ground_ns);
      stats_.placement_ns += build.placement_ns;
    } else {
      stats_.ahead_ground_ns += build.ground_ns;
      stats_.ahead_placement_ns += build.placement_ns;
    }
    std::unique_ptr<Build> taken = std::move(builds_[k]);
    builds_[k] = std::move(builds_[builds_.size() - 1]);
    builds_.pop_back();
    return taken;
  }
  return nullptr;
}

void SceneCollision::prefetch(WorldPos at, Vec3 velocity) {
  // Builds let go whose jobs have returned are freed, and with them any shape they built.
  for (u32 k = 0; k < retired_.size();) {
    if (retired_[k]->ran.done()) {
      count_build(*retired_[k]);
      retired_[k] = std::move(retired_[retired_.size() - 1]);
      retired_.pop_back();
    } else {
      ++k;
    }
  }
  // The scene's own meshes' proxies (W200): whatever the ground is and however fast the walker
  // goes, since a proxy depends on neither.
  submit_proxies(at);
  if (!can_prefetch()) return;
  // Every tick, the waiting builds' turn (T166): the walker has moved, and a worker may be free.
  struct Submit {
    SceneCollision* self;
    WorldPos at;
    Vec3 velocity;
    ~Submit() { self->submit_builds(at, velocity); }
  } submit{this, at, velocity};
  // Once a metre walked, not every tick: a tile's centre is 16 m from its edge.
  if (prefetched_once_) {
    const DVec3 moved = at - prefetched_at_;
    if (moved.x * moved.x + moved.z * moved.z < 1.0) return;
  }
  prefetched_once_ = true;
  prefetched_at_ = at;
  const f64 tile_m = static_cast<f64>(tile_mm_) / 1000.0;
  const f64 r = static_cast<f64>(config_.prefetch_radius_tiles);
  const f64 fx = at.x / tile_m;
  const f64 fz = at.z / tile_m;
  // **The lead** (T166; docs: "Ahead of a running walker"): the point `lead_s` of the walker's own
  // velocity on, at most `k_lead_max_tiles` away, whose tiles are built ahead too — so a runner's
  // next tiles are on their way before the radius round its feet reaches them.
  f64 lx = fx;
  f64 lz = fz;
  if (config_.lead_s > 0.0f) {
    const f64 vx = static_cast<f64>(velocity.x) / tile_m;
    const f64 vz = static_cast<f64>(velocity.z) / tile_m;
    const f64 speed = std::sqrt(vx * vx + vz * vz);
    if (speed > 0.0) {
      const f64 reach_tiles =
          std::min(speed * static_cast<f64>(config_.lead_s), static_cast<f64>(k_lead_max_tiles));
      lx = fx + vx / speed * reach_tiles;
      lz = fz + vz / speed * reach_tiles;
    }
  }
  const i64 cx = static_cast<i64>(std::floor(std::min(fx, lx)));
  const i64 cz = static_cast<i64>(std::floor(std::min(fz, lz)));
  const i64 cx1 = static_cast<i64>(std::floor(std::max(fx, lx)));
  const i64 cz1 = static_cast<i64>(std::floor(std::max(fz, lz)));
  const i64 reach = static_cast<i64>(std::ceil(r)) + 1;
  auto within_of = [](i64 x, i64 z, f64 px, f64 pz, f64 radius) {
    const f64 dx = static_cast<f64>(x) + 0.5 - px;
    const f64 dz = static_cast<f64>(z) + 0.5 - pz;
    return dx * dx + dz * dz <= radius * radius;
  };
  auto within = [&](i64 x, i64 z, f64 radius) {
    return within_of(x, z, fx, fz, radius) || within_of(x, z, lx, lz, radius);
  };
  // Builds the walker has left behind — past the radius and a tile of both its place and its
  // lead — are let go; one a worker has not started never will be (T166: a fast flight's builds
  // were each built when their turn came, long after the flight had left them, in front of the
  // ones it wanted).
  for (u32 k = 0; k < builds_.size();) {
    if (!within(builds_[k]->coord.x, builds_[k]->coord.z, r + 1.0)) {
      u32 queued = Build::k_queued;
      (void)builds_[k]->state.compare_exchange_strong(queued, Build::k_done,
                                                      std::memory_order_acq_rel);
      std::unique_ptr<Build> gone = std::move(builds_[k]);
      builds_[k] = std::move(builds_[builds_.size() - 1]);
      builds_.pop_back();
      ++stats_.prefetch_dropped;
      retire(std::move(gone));
    } else {
      ++k;
    }
  }
  // The ground's heights where the activation would read them from a tile source, and the
  // placements wherever a tile can hold some.
  const bool ground = ground_ != nullptr;
  const bool placements = has_placements();
  for (i64 z = cz - reach; z <= cz1 + reach; ++z) {
    for (i64 x = cx - reach; x <= cx1 + reach; ++x) {
      if (!within(x, z, r) || !fits_i32(x) || !fits_i32(z)) continue;
      const world::TileCoord coord{static_cast<i32>(x), static_cast<i32>(z)};
      if (tiles_.find_value(world::tile_key(coord)) != nullptr) continue;
      bool pending = false;
      for (const std::unique_ptr<Build>& build : builds_)
        pending = pending || (build->coord.x == coord.x && build->coord.z == coord.z);
      if (pending) continue;
      auto build = std::make_unique<Build>();
      build->owner = this;
      build->coord = coord;
      if (!ground_window(coord, build->i0, build->j0, build->n)) continue;
      build->ground = ground;
      if (ground) {
        build->source = source_;
        build->time_s = own_time_s_;
        build->spacing_mm = spacing_mm_;
        build->heights.resize(build->n * build->n);
      }
      build->placements = placements;
      builds_.push_back(std::move(build));
    }
  }
}

// **The scene's own meshes' proxies built ahead** (W200; docs: "A scene of authored meshes"). The
// rows within `Config::proxy_ahead_m` of the walker are ranked nearest first — again once the
// walker is 8 m from where they were last ranked, or the ranking is used up — and their proxies
// handed to the job system from the head of the ranking while fewer than the slots are running:
// one job a proxy, so a landing's nearest are built first on every worker, and a fast flight
// holds at most the slots' worth of workers, each for one proxy's build. A row whose proxy is in
// the cache or handed over already is passed by. The ranking is the walker's alone, so it decides
// only which proxy is built when and on which thread, never what a proxy is.
void SceneCollision::submit_proxies(WorldPos at) {
  if (jobs_ == nullptr || ahead_rows_.empty()) return;
  for (u32 k = 0; k < ahead_pending_.size();) {
    if (ahead_pending_[k]->ran.done()) {
      ahead_pending_[k] = ahead_pending_[ahead_pending_.size() - 1];
      ahead_pending_.pop_back();
    } else {
      ++k;
    }
  }
  const u32 slots = config_.prefetch_slots > 0 ? config_.prefetch_slots : 4u;
  const f64 radius = static_cast<f64>(config_.proxy_ahead_m);
  const auto rank = [&] {
    // What was handed over for a place the walker has left and no job has started is taken back
    // first (W200): the workers take the oldest first, so at 1,400 m/s the queue held proxies
    // ranked 400 m back ahead of the landing's own, and the destructor built every one of them.
    withdraw_proxies(at, radius);
    ahead_once_ = true;
    ahead_at_ = at;
    ahead_next_ = 0;
    ahead_candidates_.clear();
    for (u32 r = 0; r < ahead_rows_.size(); ++r) {
      const DVec3 d = ahead_rows_[r].at - at;
      const f64 distance = std::sqrt(d.x * d.x + d.z * d.z);
      if (distance <= radius) ahead_candidates_.push_back(AheadCandidate{distance, r});
    }
    // Nearest first, then by row: the same ranking from the same place.
    std::sort(ahead_candidates_.begin(), ahead_candidates_.end(),
              [](const AheadCandidate& a, const AheadCandidate& b) {
                return a.distance != b.distance ? a.distance < b.distance : a.row < b.row;
              });
  };
  if (!ahead_once_) {
    rank();
  } else {
    const DVec3 moved = at - ahead_at_;
    if (moved.x * moved.x + moved.z * moved.z >= 64.0) rank();
  }
  if (ahead_pending_.size() >= slots) return;
  if (ahead_next_ >= ahead_candidates_.size()) return;  // used up until the walker moves on
  // The entries are made and named under the lock, and handed to the job system after it is let
  // go (W200): a job system whose queues are full runs a job on the caller (`JobSystem::schedule`),
  // and the job takes this lock, which a `std::mutex` held here would deadlock.
  ahead_submit_.clear();
  {
    std::lock_guard<std::mutex> lock(proxies_mutex_);
    while (ahead_pending_.size() < slots && ahead_next_ < ahead_candidates_.size()) {
      const u32 r = ahead_candidates_[ahead_next_++].row;
      const AheadRow& row = ahead_rows_[r];
      if (proxies_.find_value(row.key) != nullptr || ahead_by_key_.find_value(row.key) != nullptr)
        continue;
      ProxyAhead* job = nullptr;
      for (u32 k = 0; k < ahead_free_.size(); ++k) {
        if (!ahead_free_[k]->ran.done()) continue;  // its job not popped yet
        job = ahead_free_[k];
        ahead_free_[k] = ahead_free_[ahead_free_.size() - 1];
        ahead_free_.pop_back();
        break;
      }
      if (job == nullptr) {
        ahead_.push_back(std::make_unique<ProxyAhead>());
        job = ahead_.back().get();
        job->owner = this;
      }
      job->mesh = row.mesh;
      job->scale = row.scale;
      job->row = r;
      job->state.store(ProxyAhead::k_queued, std::memory_order_relaxed);
      ahead_by_key_.insert(row.key, job);
      ahead_pending_.push_back(job);
      job->ran.add(1);
      ahead_submit_.push_back(job);
    }
  }
  for (ProxyAhead* job : ahead_submit_) {
    jobs_->schedule(jobs::Pool::Performance, jobs::Job{&ProxyAhead::run, job, &job->ran});
    ++stats_.proxy_ahead_submitted;
  }
}

void SceneCollision::withdraw_proxies(WorldPos at, f64 radius) noexcept {
  if (ahead_pending_.empty()) return;
  std::lock_guard<std::mutex> lock(proxies_mutex_);
  for (u32 k = 0; k < ahead_pending_.size();) {
    ProxyAhead* entry = ahead_pending_[k];
    bool out = radius < 0.0;
    if (!out) {
      const DVec3 d = ahead_rows_[entry->row].at - at;
      out = d.x * d.x + d.z * d.z > radius * radius;
    }
    u32 expected = ProxyAhead::k_queued;
    if (out && entry->state.compare_exchange_strong(expected, ProxyAhead::k_done,
                                                    std::memory_order_acq_rel)) {
      (void)ahead_by_key_.erase(ahead_rows_[entry->row].key);
      ahead_free_.push_back(entry);
      ahead_pending_[k] = ahead_pending_[ahead_pending_.size() - 1];
      ahead_pending_.pop_back();
      ++stats_.proxy_ahead_withdrawn;
    } else {
      ++k;
    }
  }
}

// **The builds handed over by need** (T166; docs: "Ahead of a running walker"). Without slots every
// waiting build is handed to the job system as found, in tile order — the rule before T166, which
// after a drop queued twenty tiles of a forest behind each other on one worker, so the one a runner
// reached next was still waiting seconds later. With slots, each build's need is the time until the
// walker, going on at `velocity`, comes within the ring's radius of its tile's centre (zero for one
// the ring wants now; a tile off the walker's line by its distance at the walker's pace), and the
// waiting builds are handed over most urgent first while fewer than the slots are waiting in the
// job system or running there — and every one the ring wants now at once, so a drop's ring is built
// on every worker together, the tick building one of them itself (`take_build`).
void SceneCollision::submit_builds(WorldPos at, Vec3 velocity) {
  if (jobs_ == nullptr) return;
  auto hand_over = [&](Build& build) {
    build.submitted = true;
    build.ran.add(1);
    jobs_->schedule(jobs::Pool::Performance, jobs::Job{&Build::run, &build, &build.ran});
    ++stats_.prefetch_submitted;
  };
  if (config_.prefetch_slots == 0) {
    for (const std::unique_ptr<Build>& build : builds_)
      if (!build->submitted) hand_over(*build);
    return;
  }
  const f64 tile_m = static_cast<f64>(tile_mm_) / 1000.0;
  const f64 ring_m =
      tile_m * static_cast<f64>(config_.ring_radius_tiles > 0.0f
                                    ? config_.ring_radius_tiles
                                    : std::max(config_.prefetch_radius_tiles - 1.0f, 0.5f));
  const f64 vx = static_cast<f64>(velocity.x);
  const f64 vz = static_cast<f64>(velocity.z);
  const f64 vv = vx * vx + vz * vz;
  // A walker standing still is given a walker's pace, so the tiles round it still go nearest first.
  const f64 pace = std::max(std::sqrt(vv), 1.0);
  // **Outrun** (T172; docs: "Ahead of a running walker"): a build is of use only if the walker is
  // still within the builds' reach when it is done, and a flying camera crosses the reach's
  // diameter in `2 r / speed`. When that is less than a build takes here (the estimate from the
  // builds that ran), nothing is handed over this tick, the tiles the ring wants now included:
  // the workers stay free for the place the camera slows down at, where a drop's ring is wanted.
  // A drop and a walker ask with no velocity or a walker's, and are never outrun.
  if (vv > 0.0 && stats_.build_estimate_s > 0.0) {
    const f64 reach_m = tile_m * static_cast<f64>(config_.prefetch_radius_tiles);
    if (2.0 * reach_m < stats_.build_estimate_s * std::sqrt(vv)) {
      ++stats_.prefetch_outrun;
      return;
    }
  }
  u32 busy = 0;
  u32 waiting = 0;
  for (const std::unique_ptr<Build>& b : builds_) {
    Build& build = *b;
    if (build.submitted) {
      if (!build.ran.done()) ++busy;
      continue;
    }
    ++waiting;
    const WorldPos centre = tile_corner(build.coord) + DVec3{0.5 * tile_m, 0.0, 0.5 * tile_m};
    const f64 dx = centre.x - at.x;
    const f64 dz = centre.z - at.z;
    build.distance = std::sqrt(dx * dx + dz * dz);
    if (build.distance <= ring_m) {
      build.need_s = 0.0;
      continue;
    }
    // When |at + v t - c| first reaches the ring's radius, for t >= 0: the smaller root of
    // vv t^2 - 2 (d.v) t + (|d|^2 - R^2) = 0, where the walker's line passes that close at all.
    build.need_s = (build.distance - ring_m) / pace;
    if (vv > 0.0) {
      const f64 dv = dx * vx + dz * vz;
      const f64 disc = dv * dv - vv * (build.distance * build.distance - ring_m * ring_m);
      if (dv > 0.0 && disc >= 0.0)
        build.need_s = std::min(build.need_s, (dv - std::sqrt(disc)) / vv);
    }
  }
  if (waiting == 0) return;
  // Most urgent first, then nearest, then in tile order: the same order from the same walk.
  std::sort(builds_.begin(), builds_.end(),
            [](const std::unique_ptr<Build>& a, const std::unique_ptr<Build>& b) {
              if (a->submitted != b->submitted) return a->submitted;
              if (a->need_s != b->need_s) return a->need_s < b->need_s;
              if (a->distance != b->distance) return a->distance < b->distance;
              return world::tile_key(a->coord) < world::tile_key(b->coord);
            });
  for (const std::unique_ptr<Build>& b : builds_) {
    Build& build = *b;
    if (build.submitted) continue;
    if (build.need_s > 0.0 && busy >= config_.prefetch_slots) break;
    hand_over(build);
    ++busy;
  }
}

// ---- the consumer
// ---------------------------------------------------------------------------------

bool SceneCollision::activate(void* context, const world::TileEvent& event) {
  ENGINE_STARTUP_STAGE("collision tile");  // the startup trace: the ring's first tiles
  auto* self = static_cast<SceneCollision*>(context);
  const i64 start = time::monotonic_ns();
  Tile tile;
  tile.coord = event.tile;
  bool ok = true;
  // Its build ahead, finished (taken, waited for, or run here); the bodies are made below, in the
  // order they always were: the ground's, then the compound's.
  std::unique_ptr<Build> build = self->take_build(event.tile);
  if (self->ground_ != nullptr && !self->build_ground(tile, build.get())) ok = false;
  if (!self->build_placements(tile, build.get())) ok = false;
  if (build != nullptr) self->retire(std::move(build));
  if (!ok) ++self->stats_.failures;
  self->tiles_.insert_or_assign(world::tile_key(event.tile), std::move(tile));
  ++self->stats_.activated;
  self->note_bodies();
  const i64 dt = time::monotonic_ns() - start;
  self->stats_.activate_ns += dt;
  self->stats_.max_activate_ns =
      dt > self->stats_.max_activate_ns ? dt : self->stats_.max_activate_ns;
  return ok;
}

void SceneCollision::deactivate(void* context, const world::TileEvent& event) {
  auto* self = static_cast<SceneCollision*>(context);
  const i64 start = time::monotonic_ns();
  const u64 key = world::tile_key(event.tile);
  Tile* tile = self->tiles_.find_value(key);
  if (tile == nullptr) return;
  self->drop_ground_body(*tile);
  if (tile->placements) {
    self->physics_->destroy_body(tile->placements);
    ++self->stats_.bodies_destroyed;
  }
  if (tile->placements_shape) self->physics_->destroy_shape(tile->placements_shape);
  self->release_tile_proxies(tile->tile_proxies);  // after the compound that named them
  self->tiles_.erase(key);
  ++self->stats_.deactivated;
  self->note_bodies();
  self->stats_.deactivate_ns += time::monotonic_ns() - start;
}

// ---- a game's props (scene_collision.md, "Props"; roadmap P45)
// ---------------------------------------------------------------------------------------------
//
// A body a prop, not a piece of its tile's compound: a module moves one prop at a tick, and a
// compound would be built again whole for it, with every other piece of the tile. The bodies are
// made on the tick and in key order, after the ring's update, so their ids — and a walk that
// touches one — are the same in a replay. **A move is a move** (docs: "What a moving prop costs"):
// a prop whose shape is the same is put at its new place, never made again, and nothing about the
// broadphase is redone for it — the trees are rebuilt only for a body made or let go, which leaves
// a node behind until they are. A box that has moved once is made again, once, as a kinematic body
// in `Layer::Kinematic`, the tree the physics library keeps for things that move, so the static
// tree the tiles' ground and placements live in is not the one it widens; a mesh's proxy has no
// inside, the library moves it only as a static body, and it is moved in the static tree.

namespace {
// Whether two props of one key have the same body but for its place and turn: a box by its size,
// a mesh by its proxy (the mesh at its quantized scale), so a scale that rounds to the same step
// keeps the body.
bool same_shape(const PropBody& a, u64 a_proxy, const PropBody& b, u64 b_proxy) noexcept {
  if (a.mesh != b.mesh || a.collider.shape != b.collider.shape) return false;
  if (a.collider.shape == renderer::PropShape::mesh) return a_proxy == b_proxy;
  return a.scale == b.scale && a.collider.half_extents == b.collider.half_extents &&
         a.collider.center == b.collider.center;
}
bool same_pose(const PropBody& a, const PropBody& b) noexcept {
  return a.origin.x == b.origin.x && a.origin.y == b.origin.y && a.origin.z == b.origin.z &&
         a.rotation.x == b.rotation.x && a.rotation.y == b.rotation.y &&
         a.rotation.z == b.rotation.z && a.rotation.w == b.rotation.w;
}
// A half turn about x: with the x axis's mirror, a uniform negative scale (−1 = Rx(π) · diag(−1,
// 1, 1)).
constexpr Quat k_half_turn_x{1.0f, 0.0f, 0.0f, 0.0f};
}  // namespace

// **What a mesh's props share** (docs: "A prop's proxy"), found the first time a prop asks: its fit
// as a turn and a scale, and the step a prop's scale is rounded to. The reach is the mesh's 16-bit
// grid (`ClusterMeshPart::quant_origin`, `quant_scale`), a cube holding every vertex, through the
// fit: an upper bound, so the step errs small. A prop at scale s built at q stands every point
// |q − s| times its distance from the origin off the drawn mesh, so a step of
// `mesh_error_m / (2 reach)`, rounded to the nearest, moves none by more than a quarter of the
// walk's error.
const SceneCollision::PropMesh& SceneCollision::prop_mesh(u32 mesh) {
  static const PropMesh k_none{};
  if (mesh >= prop_meshes_.size()) return k_none;
  PropMesh& pm = prop_meshes_[mesh];
  if (pm.known) return pm;
  pm.known = true;
  if (mesh >= scene_->parts.size() || mesh == scene_->terrain_mesh) return pm;
  if (water_mesh(*scene_, mesh) || far_mesh(mesh)) return pm;
  const Mat4 fit = mesh < scene_->mesh_fit.size() ? scene_->mesh_fit[mesh] : Mat4::identity();
  Placement turned;
  if (!decompose(fit, turned)) return pm;
  const geometry::ClusterMeshPart& part = scene_->parts[mesh];
  const f32 side = part.quant_scale * 65535.0f;
  f32 reach = 0.0f;
  for (u32 c = 0; c < 8; ++c) {
    const Vec3 corner{part.quant_origin.x + ((c & 1u) ? side : 0.0f),
                      part.quant_origin.y + ((c & 2u) ? side : 0.0f),
                      part.quant_origin.z + ((c & 4u) ? side : 0.0f)};
    reach = std::max(reach, length(transform_point(fit, corner)));
  }
  pm.rotation = turned.rotation;
  pm.scale = turned.scale;
  pm.reach_m = reach;
  pm.step =
      reach > 0.0f && config_.mesh_error_m > 0.0f ? config_.mesh_error_m / (2.0f * reach) : 0.0f;
  pm.ok = true;
  return pm;
}

bool SceneCollision::prop_scale(u32 mesh, f32 scale, f32& quantized, u64& key) {
  // The last answer again (a game's props are mostly many of one mesh at one size): the hand-over
  // asks once a prop a tick it hands them.
  if (mesh == last_scale_mesh_ && std::bit_cast<u32>(scale) == std::bit_cast<u32>(last_scale_)) {
    quantized = last_quantized_;
    key = last_key_;
    return key != 0;
  }
  last_scale_mesh_ = mesh;
  last_scale_ = scale;
  quantized = 0.0f;
  key = 0;
  last_quantized_ = 0.0f;
  last_key_ = 0;
  const PropMesh& pm = prop_mesh(mesh);
  const f32 size = std::fabs(scale);
  if (!pm.ok || !(size > 0.0f) || !(size < std::numeric_limits<f32>::infinity())) return false;
  f32 q = size;
  if (pm.step > 0.0f) {
    // Anchored at 1, so a prop at its own size is the declared proxy (`create`) exactly.
    const f32 steps = std::floor((size - 1.0f) / pm.step + 0.5f);
    const f32 rounded = 1.0f + steps * pm.step;
    if (rounded > 0.0f) q = rounded;
  }
  quantized = scale < 0.0f ? -q : q;
  key = hash_combine(hash_combine(k_hash_seed, mesh), bits(quantized)) | (u64{1} << 63);
  last_quantized_ = quantized;
  last_key_ = key;
  return true;
}

void SceneCollision::set_props(std::span<const PropBody> props) {
  props_next_.clear();
  props_next_.reserve(static_cast<u32>(props.size()));
  bool sorted = true;
  for (const PropBody& p : props) {
    if (p.collider.shape == renderer::PropShape::none) continue;  // no body, nothing kept
    sorted = sorted && (props_next_.empty() || props_next_.back().prop.key < p.key);
    HeldProp held;
    held.prop = p;
    if (p.collider.shape == renderer::PropShape::mesh)
      (void)prop_scale(p.mesh, p.scale, held.quantized, held.proxy);  // none: no body
    props_next_.push_back(held);
  }
  if (!sorted) {
    // In key order, and **one prop a key**: a key handed twice keeps the first handed, since two
    // held entries of one key would both claim the one body the merge keeps and leak the other.
    std::stable_sort(props_next_.begin(), props_next_.end(),
                     [](const HeldProp& a, const HeldProp& b) { return a.prop.key < b.prop.key; });
    const auto last =
        std::unique(props_next_.begin(), props_next_.end(),
                    [](const HeldProp& a, const HeldProp& b) { return a.prop.key == b.prop.key; });
    const u32 kept = static_cast<u32>(last - props_next_.begin());
    stats_.prop_duplicates += props_next_.size() - kept;
    props_next_.resize(kept);
  }
  stats_.props = static_cast<u32>(props.size());
  props_handed_ = true;
}

bool SceneCollision::prop_pose(const HeldProp& held, physics::BodyTransform& out) const {
  const PropBody& p = held.prop;
  if (p.collider.shape == renderer::PropShape::box) {
    // The box's centre through the prop's turn and scale, added to its place in f64.
    out.position = p.origin + DVec3{rotate(p.rotation, p.collider.center * p.scale)};
    out.rotation = p.rotation;
    return true;
  }
  if (held.proxy == 0 || p.mesh >= prop_meshes_.size() || !prop_meshes_[p.mesh].ok) return false;
  // Where the renderer stands the instance (`renderer::instance_translation`) at the quantized
  // scale: its fit through the turn and scale, the offset added to the origin in f64 — as a
  // placement's piece is. The turn is the prop's and the fit's, never one taken back out of a
  // matrix the yaw went into.
  Transform3 turn;
  turn.rotation = p.rotation;
  turn.scale = Vec3{held.quantized, held.quantized, held.quantized};
  const Mat4 fit = p.mesh < scene_->mesh_fit.size() ? scene_->mesh_fit[p.mesh] : Mat4::identity();
  const Mat4 linear = mat4_from_transform(turn) * fit;
  out.position = p.origin + DVec3{linear.c[3].xyz()};
  out.rotation = p.rotation * prop_meshes_[p.mesh].rotation;
  if (held.quantized < 0.0f) out.rotation = out.rotation * k_half_turn_x;
  return true;
}

// The proxy `held` names, found or begun: a new one is built now when its update has come (`now`,
// or `Config::prop_build_updates` 0), else handed to the job system to be ready by its update.
SceneCollision::PropProxy* SceneCollision::want_prop_proxy(const HeldProp& held, bool now) {
  if (held.proxy == 0) return nullptr;
  if (std::unique_ptr<PropProxy>* found = prop_proxies_.find_value(held.proxy)) return found->get();
  const PropMesh& pm = prop_mesh(held.prop.mesh);
  if (!pm.ok) return nullptr;
  auto entry = std::make_unique<PropProxy>();
  entry->owner = this;
  entry->mesh = held.prop.mesh;
  entry->scale = pm.scale * std::fabs(held.quantized);
  if (held.quantized < 0.0f) entry->scale.x = -entry->scale.x;
  entry->scratch = std::make_unique<PlacementScratch>();
  entry->due = prop_updates_ + (now ? 0u : config_.prop_build_updates);
  PropProxy* out = entry.get();
  prop_proxies_.try_emplace(held.proxy, std::move(entry));
  if (out->due > prop_updates_ && jobs_ != nullptr) {
    out->submitted = true;
    out->ran.add(1);
    jobs_->schedule(jobs::Pool::Performance, jobs::Job{&PropProxy::run, out, &out->ran});
  } else if (now) {
    out->state.store(PropProxy::k_running, std::memory_order_relaxed);
    out->compute();  // at the load (`create`), or an update that takes every proxy `now`
    (void)finish_prop_proxy(*out);
  } else if (out->due <= prop_updates_) {
    (void)finish_prop_proxy(*out);
  }
  count_prop_proxies();
  return out;
}

// Whether the proxy is ready for a body at this update: its update come (`due`, which `at_once`
// brings forward), and then built — here if no worker started it, or waited for while one runs.
// **Never before its update, built or not**, so the body's tick, its id and every id after it are
// the hand-over's plus the updates, whatever the workers did: until 2026-11-20 the update was asked
// only while the proxy was unbuilt, so one a worker had finished got its body at the next update.
bool SceneCollision::finish_prop_proxy(PropProxy& entry) {
  if (prop_updates_ < entry.due) return false;
  if (entry.state.load(std::memory_order_acquire) != PropProxy::k_done) {
    u32 expected = PropProxy::k_queued;
    if (entry.state.compare_exchange_strong(expected, PropProxy::k_running,
                                            std::memory_order_acq_rel)) {
      entry.compute();
      ++stats_.prop_proxy_inline;
    } else {
      const i64 start = time::monotonic_ns();
      if (entry.submitted) entry.ran.wait_blocking();
      ++stats_.prop_proxy_waits;
      stats_.prop_proxy_wait_ns += time::monotonic_ns() - start;
    }
  }
  if (!entry.counted) {
    entry.counted = true;
    ++stats_.prop_proxy_builds;
    stats_.proxy_ns += entry.ns;
    entry.scratch.reset();  // the build's alone; it stored `k_done` after its last use
    count_prop_proxies();
  }
  return true;
}

void SceneCollision::count_prop_proxies() noexcept {
  stats_.prop_proxies = static_cast<u32>(prop_proxies_.size());
  u64 bytes = 0;  // a walk over the table, which changes only at a build or a letting go
  for (u32 i = 0; i < prop_proxies_.size(); ++i) {
    const PropProxy& entry = *prop_proxies_.value_at(i);
    bytes += sizeof(PropProxy) + (entry.counted ? entry.bytes : 0u);
  }
  stats_.prop_proxy_bytes = bytes;
}

bool SceneCollision::make_prop_body(HeldProp& held) {
  const PropBody& p = held.prop;
  physics::BodyDesc body;
  body.motion = held.kinematic ? physics::MotionType::Kinematic : physics::MotionType::Static;
  body.layer = held.kinematic ? physics::Layer::Kinematic : physics::Layer::Static;
  body.friction = 0.8f;
  if (!prop_pose(held, body.transform)) return false;
  if (p.collider.shape == renderer::PropShape::box) {
    if (physics_->create_box(p.collider.half_extents * p.scale, held.shape) != physics::Status::Ok)
      return false;
  } else {
    const std::unique_ptr<PropProxy>* entry = prop_proxies_.find_value(held.proxy);
    if (entry == nullptr || !(*entry)->counted || !(*entry)->proxy.shape) return false;
    if (physics_->adopt_shape((*entry)->proxy.shape, held.shape) != physics::Status::Ok)
      return false;
  }
  body.shape = held.shape;
  if (physics_->create_body(body, held.body) != physics::Status::Ok) {
    physics_->destroy_shape(held.shape);
    held.shape = {};
    return false;
  }
  ++stats_.prop_bodies_created;
  return true;
}

void SceneCollision::drop_prop_body(HeldProp& held) noexcept {
  if (held.body) {
    physics_->destroy_body(held.body);
    ++stats_.prop_bodies_destroyed;
  }
  if (held.shape) physics_->destroy_shape(held.shape);
  held.body = {};
  held.shape = {};
}

u32 SceneCollision::update_props(bool at_once) {
  ++prop_updates_;
  const u64 ring = stats_.activated + stats_.deactivated;
  const bool ring_moved = ring != props_ring_;
  if (!props_handed_ && !ring_moved && props_waiting_ == 0 && prop_retired_.empty()) return 0;
  props_ring_ = ring;
  u32 worked = 0;      // bodies made or let go, which the broadphase is rebuilt for
  u32 moved = 0;       // bodies put at a new place, which it is not
  bool named = false;  // which proxies the held props name may have changed
  if (props_handed_) {
    // Destroyed before made, in key order: a held prop the new set does not hold, or holds with
    // another shape, loses its body here; one held with the same shape keeps it, and is moved if
    // it moved — at its first move made again as a kinematic body. A key new to the set, or one
    // with no body, stays pending.
    u32 n = 0;
    u32 matched = 0;
    for (HeldProp& old : props_) {
      while (n < props_next_.size() && props_next_[n].prop.key < old.prop.key)
        ++n;
      HeldProp* now = n < props_next_.size() && props_next_[n].prop.key == old.prop.key
                          ? &props_next_[n]
                          : nullptr;
      if (now == nullptr || !same_shape(now->prop, now->proxy, old.prop, old.proxy)) {
        named = true;
        if (old.body) {
          drop_prop_body(old);
          ++worked;
        }
        continue;
      }
      ++matched;
      now->kinematic = old.kinematic;
      if (!old.body) continue;
      now->body = old.body;
      now->shape = old.shape;
      now->pending = false;
      if (same_pose(now->prop, old.prop)) continue;
      // A box's first move: let go, to be made a kinematic body. A mesh's proxy has no inside,
      // so the physics library moves it only as a static body: it is moved where it is.
      const bool promote = !now->kinematic && now->prop.collider.shape == renderer::PropShape::box;
      if (promote || !covers(now->prop.origin)) {  // or off the held tiles: let go
        named = named || !promote;                 // and its proxy perhaps with it
        now->kinematic = now->kinematic || promote;
        now->pending = true;
        drop_prop_body(*now);
        ++worked;
        continue;
      }
      physics::BodyTransform pose;
      if (prop_pose(*now, pose) && physics_->set_body_transform(now->body, pose, false)) {
        ++moved;
        ++stats_.prop_moves;
      }
    }
    named = named || matched != props_next_.size();
    props_.swap(props_next_);
    props_next_.clear();
    props_handed_ = false;
  }
  if (ring_moved) {
    // A body while the tile it stands in is held: let go with it here, and every prop with none
    // asked again below; and the proxies counted again, since one only props off the held tiles
    // name goes with them.
    named = true;
    for (HeldProp& held : props_) {
      if (held.body && !covers(held.prop.origin)) {
        drop_prop_body(held);
        ++worked;
      }
      held.pending = held.pending || !held.body;
    }
  }
  props_waiting_ = 0;
  for (HeldProp& held : props_) {  // made, in key order
    if (!held.pending) continue;
    held.pending = false;
    if (held.body || !covers(held.prop.origin)) continue;
    if (held.prop.collider.shape == renderer::PropShape::mesh) {
      // Its proxy: resident, building, or begun now; a body only once it is ready (above).
      // Taken `at_once` (a drop, a teleport, a walk's first tick): a new one built here, one a job
      // has brought forward to this update, so `finish_prop_proxy` builds or waits for it.
      const bool resident = prop_proxies_.find_value(held.proxy) != nullptr;
      PropProxy* entry = want_prop_proxy(held, at_once);
      named = named || !resident;
      if (entry == nullptr) continue;  // a mesh with no body
      if (at_once && !resident) ++stats_.prop_proxy_inline;
      if (at_once && entry->due > prop_updates_) entry->due = prop_updates_;
      if (!finish_prop_proxy(*entry)) {
        held.pending = true;
        ++props_waiting_;
        continue;
      }
    }
    if (make_prop_body(held)) ++worked;
  }
  if (named) {
    // **What no prop in reach names is let go, with the tiles** (docs: "A prop's proxy"), but a
    // declared one's: counted again over the held props whose tile is held, and one a job may
    // still be building kept aside until it ran. A prop handed but off the held tiles names
    // nothing, so what a walk leaves behind is freed as it leaves and the table is bounded by the
    // ring, not by the ground walked; coming back builds it again, at its promised update.
    for (u32 i = 0; i < prop_proxies_.size(); ++i)
      prop_proxies_.value_at(i)->refs = 0;
    for (const HeldProp& held : props_)
      if (held.proxy != 0 && covers(held.prop.origin))
        if (std::unique_ptr<PropProxy>* entry = prop_proxies_.find_value(held.proxy))
          ++(*entry)->refs;
    prop_unused_.clear();
    for (u32 i = 0; i < prop_proxies_.size(); ++i) {
      const PropProxy& entry = *prop_proxies_.value_at(i);
      if (entry.refs == 0 && !entry.declared) prop_unused_.push_back(prop_proxies_.key_at(i));
    }
    for (const u64 key : prop_unused_) {
      std::unique_ptr<PropProxy>* entry = prop_proxies_.find_value(key);
      if (!(*entry)->ran.done()) prop_retired_.push_back(std::move(*entry));
      prop_proxies_.erase(key);
      ++stats_.prop_proxies_freed;
    }
    count_prop_proxies();
  }
  for (u32 k = 0; k < prop_retired_.size();) {
    if (prop_retired_[k]->ran.done()) {
      prop_retired_[k] = std::move(prop_retired_[prop_retired_.size() - 1]);
      prop_retired_.pop_back();
    } else {
      ++k;
    }
  }
  if (worked > 0) {
    stats_.prop_bodies = 0;
    for (const HeldProp& held : props_)
      stats_.prop_bodies += held.body ? 1u : 0u;
    // The trees give back a removed body's node only when rebuilt (docs: "Why this shape"):
    // rebuilt once an update that made or let go a prop's body, never for a move.
    physics_->optimize_broad_phase();
  }
  return worked + moved;
}

// ---- doors, kinematic boxes about a hinge (scene_collision.md, "Doors"; roadmap S91; ADR-0070)
// ---------------------------------------------------------------------------------------------
//
// A kinematic box a door, made while the tile its hinge stands in is held, put at its angle on the
// tick with `set_body_transform` — the walker's world is never stepped, so `move_kinematic`, which
// sets a velocity for a step, would move nothing (ADR-0070 decision 3).

// The leaf's turn about +y at `mdeg` millidegrees, from the integer alone: the angle is reduced to
// (-180, 180] degrees in integers, and the half angle's sine and cosine are `core/math`'s
// deterministic ones (`det::sin`, `det::cos`, fdlibm's kernels in IEEE arithmetic), never the C
// library's, whose last bit differs between MSVC's UCRT and glibc — which would make a walk
// through a door replay differently on another compiler, since the body's pose and the walker's
// hold both turn on it (scene_collision.md, "Doors").
// The renderer's `hinge_turn`, which a drawn leaf stands by too: one function, so what is drawn and
// what collides turn by the same bits.
Quat door_rotation(i32 mdeg) noexcept { return renderer::hinge_turn(mdeg); }

DoorLeafPose door_leaf_pose(const DoorBody& door) noexcept {
  return DoorLeafPose{door.hinge, door.base * door_rotation(door.closed_mdeg + door.angle_mdeg)};
}

DoorBody declared_door(const geometry::ClusterFileDoor& record, WorldPos origin, Quat rotation,
                       f32 scale, u64 key) noexcept {
  DoorBody d;
  d.key = key;
  const f32 s = scale > 0.0f ? scale : 1.0f;
  d.hinge = origin + DVec3{rotate(rotation, record.pivot * s)};
  // The shut leaf's heading (the turn about +y that takes +x to `shut`, square roots alone) and the
  // sense of its opening, by the renderer's functions, which its drawn leaf is stood by too.
  d.base = rotation * renderer::leaf_heading(record);
  const i32 side = renderer::leaf_side(record);
  d.closed_mdeg = 0;
  d.open_mdeg = side * k_door_open_mdeg;
  d.half_extents =
      Vec3{0.5f * record.width_m * s, 0.5f * record.height_m * s, 0.5f * record.thickness_m * s};
  d.face_z = static_cast<f32>(side) * d.half_extents.z;
  // Where its description stood it, and open when that was not shut: a door hung ajar finishes
  // opening the first time a tick turns it, and a toggle shuts it.
  d.angle_mdeg = renderer::leaf_declared_mdeg(record);
  d.open = d.angle_mdeg != 0;
  return d;
}

u64 declared_door_key(u64 placement, u32 leaf) noexcept {
  return hash_combine(hash_combine(k_hash_seed, placement), leaf) | (u64{1} << 63);
}

namespace {
// How far short of the walker's capsule a door holds: past the character's contact padding, so a
// leaf that stops beside it never presses it, which the character would answer by stepping aside
// and the door by creeping after it.
constexpr f32 k_door_hold_margin_m = 0.05f;
// How finely a tick's swing is tested against the walker: a degree, through which a leaf's edge
// at 17 m from its hinge moves the 0.3 m of the walker's radius, so no leaf a building hangs can
// step over the capsule between two tests.
constexpr i32 k_door_sweep_mdeg = 1000;
i32 door_target(const DoorBody& d) noexcept { return d.open ? d.open_mdeg : 0; }
// The leaf box's centre at swing `mdeg`: half its width along the leaf from the hinge, half its
// height up, and `face_z` through its thickness.
physics::BodyTransform door_pose(const DoorBody& d, i32 mdeg) noexcept {
  physics::BodyTransform t;
  t.rotation = d.base * door_rotation(d.closed_mdeg + mdeg);
  t.position =
      d.hinge + DVec3{rotate(t.rotation, Vec3{d.half_extents.x, d.half_extents.y, d.face_z})};
  return t;
}
// How far the leaf at swing `mdeg` reaches into an upright capsule whose feet stand at `feet`
// (`radius`, `height` feet to crown): the radius less the distance in x and z from the capsule's
// axis to the leaf's rectangle in the leaf's own frame, the heights overlapping; -1 where they do
// not. Positive is in. Exact for a capsule standing up, as the walker always does, and a function
// of the bits alone (a correctly rounded square root).
f32 leaf_depth(const DoorBody& d, i32 mdeg, WorldPos feet, f32 radius, f32 height) noexcept {
  const DVec3 rel = feet - d.hinge;
  if (rel.y > 2.0 * static_cast<f64>(d.half_extents.y) || rel.y + static_cast<f64>(height) < 0.0)
    return -1.0f;
  const Quat back = conjugate(d.base * door_rotation(d.closed_mdeg + mdeg));
  const Vec3 local = rotate(back, Vec3{static_cast<f32>(rel.x), 0.0f, static_cast<f32>(rel.z)});
  const f32 dx = std::max(0.0f, std::fabs(local.x - d.half_extents.x) - d.half_extents.x);
  const f32 dz = std::max(0.0f, std::fabs(local.z - d.face_z) - d.half_extents.z);
  return radius - std::sqrt(dx * dx + dz * dz);
}
}  // namespace

u32 SceneCollision::find_door(u64 key) const noexcept {
  u32 lo = 0, hi = doors_.size();
  while (lo < hi) {
    const u32 mid = lo + (hi - lo) / 2;
    if (doors_[mid].key < key)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < doors_.size() && doors_[lo].key == key ? lo : k_no_index;
}

void SceneCollision::set_doors(std::span<const DoorBody> doors) {
  for (HeldDoor& held : door_bodies_)
    drop_door_body(held);
  doors_.clear();
  door_bodies_.clear();
  doors_.reserve(static_cast<u32>(doors.size()));
  // Every number a tick adds to is brought into range here, so the integer swing never overflows:
  // a speed of 1 to 180 degrees a tick (none or a negative one the default), a swing within a whole
  // turn either way, the closed heading within one turn, and the angle reached between closed and
  // open.
  for (const DoorBody& d : doors) {
    DoorBody h = d;
    if (h.speed_mdeg <= 0) h.speed_mdeg = k_door_speed_mdeg;
    h.speed_mdeg = std::min(h.speed_mdeg, 180000);
    h.open_mdeg = std::clamp(h.open_mdeg, -360000, 360000);
    h.closed_mdeg %= 360000;
    h.angle_mdeg = std::clamp(h.angle_mdeg, std::min(0, h.open_mdeg), std::max(0, h.open_mdeg));
    doors_.push_back(h);
  }
  // In key order, the first given of a key kept and every later one refused (`DoorStats::refused`):
  // a key names one door, and two with one key would make `toggle_door` and `door` answer for
  // whichever the sort left first.
  std::stable_sort(doors_.begin(), doors_.end(),
                   [](const DoorBody& a, const DoorBody& b) { return a.key < b.key; });
  u32 kept = 0;
  for (u32 i = 0; i < doors_.size(); ++i) {
    if (kept > 0 && doors_[kept - 1].key == doors_[i].key) continue;
    if (kept != i) doors_[kept] = doors_[i];
    ++kept;
  }
  door_stats_.refused = doors_.size() - kept;
  doors_.resize(kept);
  door_bodies_.resize(kept);
  door_stats_.doors = doors_.size();
  door_stats_.declared = 0;
  for (const DoorBody& d : doors_)
    door_stats_.declared += (d.key >> 63) != 0 ? 1u : 0u;
  doors_ring_ = ~u64{0};  // the held tiles' declared doors are looked for again at the next step
}

u32 SceneCollision::door_to_reach(WorldPos feet, Vec3 facing, f32 reach) const noexcept {
  const f64 fx = static_cast<f64>(facing.x), fz = static_cast<f64>(facing.z);
  const f64 flen = std::sqrt(fx * fx + fz * fz);
  u32 best = k_no_index;
  f64 best_d2 = static_cast<f64>(reach) * static_cast<f64>(reach);
  for (u32 i = 0; i < doors_.size(); ++i) {
    // A door that stands turns for no one, so an interact passes it by for one that turns behind
    // it (`door_nearest`, a module's question, still finds it).
    if (doors_[i].fixed) continue;
    const WorldPos middle = door_pose(doors_[i], doors_[i].angle_mdeg).position;
    const DVec3 to = middle - feet;
    if (to.y < -2.5 || to.y > 2.5) continue;
    const f64 d2 = to.x * to.x + to.z * to.z;
    if (d2 >= best_d2) continue;  // in key order: the first of two at one distance stays
    // In front: the bearing within 60 degrees of the facing, cos 60 = 1/2, without a division.
    if (flen > 0.0 && 2.0 * (to.x * fx + to.z * fz) < std::sqrt(d2) * flen) continue;
    best = i;
    best_d2 = d2;
  }
  return best;
}

u32 SceneCollision::door_nearest(WorldPos point, f32 radius) const noexcept {
  u32 best = k_no_index;
  f64 best_d2 = static_cast<f64>(radius) * static_cast<f64>(radius);
  for (u32 i = 0; i < doors_.size(); ++i) {
    const DVec3 to = doors_[i].hinge - point;
    const f64 d2 = to.x * to.x + to.y * to.y + to.z * to.z;
    if (d2 > best_d2 || (d2 == best_d2 && best != k_no_index)) continue;
    best = i;
    best_d2 = d2;
  }
  return best;
}

bool SceneCollision::toggle_door(u64 key) {
  const u32 i = find_door(key);
  if (i == k_no_index || doors_[i].fixed) return false;  // a door that stands turns for no one
  doors_[i].open = !doors_[i].open;
  ++door_stats_.toggles;
  return true;
}

bool SceneCollision::door(u64 key, DoorBody& out) const noexcept {
  const u32 i = find_door(key);
  if (i == k_no_index) return false;
  out = doors_[i];
  return true;
}

void SceneCollision::drop_door_body(HeldDoor& held) noexcept {
  if (held.body) physics_->destroy_body(held.body);
  if (held.shape) physics_->destroy_shape(held.shape);
  held.body = {};
  held.shape = {};
}

bool SceneCollision::leaf_hung(u32 house, u32 leaf) const noexcept {
  return std::binary_search(hung_leaves_.begin(), hung_leaves_.end(), hung_leaf(house, leaf));
}

// **A placed piece's declared leaves become doors** (docs: "Doors"): every held tile's pieces whose
// proxy carries section 53's records, each record a door keyed by the piece's name and the leaf's
// index, taken in when no door of that key is held, and a held one told whether it stands.
// Collected, sorted and merged by key, so neither the tiles' table order nor the order they came
// in reaches the doors. Runs on a ring change only.
bool SceneCollision::take_declared_doors() {
  doors_found_.clear();
  doors_unmade_.clear();
  for (u32 t = 0; t < tiles_.size(); ++t) {
    const Tile& tile = tiles_.value_at(t);
    for (const PieceRecord& record : tile.pieces_held) {
      const Proxy* proxy = record.proxy;
      if (proxy == nullptr || proxy->doors.empty()) continue;
      const WorldPos origin = tile_corner(tile.coord) + DVec3{record.offset};
      const u64 placement = support_placement(record.instance, record.entry, record.id,
                                              tile.coord.x, tile.coord.z, record.index);
      const bool stands = record.leaves == k_leaves_stand;
      for (u32 k = 0; k < proxy->doors.size(); ++k) {
        const u64 key = declared_door_key(placement, k);
        // **What collides is what is drawn**: a piece whose leaves the renderer hung none of (a
        // mesh the scene fits, a `.clusters` file, a streamed world's authored run, a tile's
        // resident mesh) makes no door, and its doorway stands open as it is drawn; nor does a
        // scene's house where its own leaf was not hung, whichever of its others were. A door of
        // that key held already (a restore of a save that made one) is dropped with its body.
        if (record.leaves == 0 ||
            (record.leaves == k_leaves_turn && !leaf_hung(record.instance, k))) {
          if (const u32 held = find_door(key); held != k_no_index) doors_unmade_.push_back(held);
          continue;
        }
        if (const u32 held = find_door(key); held != k_no_index) {
          // **Whether it stands is the scene's, not a restore's**: a door held already (a restore,
          // or the same door from another tile) takes `fixed` from the piece that declares it. One
          // handed back turning that stands is made again as declared, where its leaf is drawn;
          // one handed back standing that turns keeps its angle and turns from there.
          DoorBody& door = doors_[held];
          if (door.fixed == stands) continue;
          if (stands) {
            door = declared_door(proxy->doors[k], origin, record.rotation, proxy->door_scale, key);
            drop_door_body(door_bodies_[held]);  // made again at its declared pose below
          }
          door.fixed = stands;
          continue;
        }
        doors_found_.push_back(
            declared_door(proxy->doors[k], origin, record.rotation, proxy->door_scale, key));
        doors_found_.back().fixed = stands;
      }
    }
  }
  // The unmade doors out, in place and in key order (a piece in several tiles names one twice).
  bool dropped = false;
  if (!doors_unmade_.empty()) {
    std::sort(doors_unmade_.begin(), doors_unmade_.end());
    u32 kept = 0, u = 0;
    for (u32 i = 0; i < doors_.size(); ++i) {
      while (u < doors_unmade_.size() && doors_unmade_[u] < i)
        ++u;
      if (u < doors_unmade_.size() && doors_unmade_[u] == i) {
        if (door_bodies_[i].body) dropped = true;
        drop_door_body(door_bodies_[i]);
        --door_stats_.declared;  // a declared leaf's key
        continue;
      }
      if (kept != i) {
        doors_[kept] = doors_[i];
        door_bodies_[kept] = door_bodies_[i];
      }
      ++kept;
    }
    doors_.resize(kept);
    door_bodies_.resize(kept);
    door_stats_.doors = doors_.size();
  }
  if (doors_found_.empty()) return dropped;
  std::sort(doors_found_.begin(), doors_found_.end(),
            [](const DoorBody& a, const DoorBody& b) { return a.key < b.key; });
  Vector<DoorBody> merged;
  Vector<HeldDoor> bodies;
  merged.reserve(doors_.size() + doors_found_.size());
  bodies.reserve(doors_.size() + doors_found_.size());
  u32 i = 0, j = 0;
  while (i < doors_.size() || j < doors_found_.size()) {
    // A scene's instance is a piece of every tile its bounds reach: one door a key.
    if (j < doors_found_.size() && !merged.empty() && merged.back().key == doors_found_[j].key) {
      ++j;
      continue;
    }
    if (j >= doors_found_.size() || (i < doors_.size() && doors_[i].key < doors_found_[j].key)) {
      merged.push_back(doors_[i]);
      bodies.push_back(door_bodies_[i]);
      ++i;
    } else {
      merged.push_back(doors_found_[j]);
      bodies.push_back(HeldDoor{});
      ++door_stats_.declared;
      ++j;
    }
  }
  doors_ = std::move(merged);
  door_bodies_ = std::move(bodies);
  door_stats_.doors = doors_.size();
  return dropped;
}

u32 SceneCollision::step_doors(const physics::CharacterBody* walker) {
  const u64 ring = stats_.activated + stats_.deactivated;
  bool changed = false;
  if (ring != doors_ring_) {
    doors_ring_ = ring;
    if (take_declared_doors()) changed = true;
    // **A door that stands goes with its tile** (docs: "Doors"): it has no state of its own, so it
    // is made again as declared when the tile comes back, and the doors a walk keeps are bounded
    // by the ring for a streamed world. A door that turns outlives its tile, its state the world's.
    u32 kept = 0;
    for (u32 i = 0; i < doors_.size(); ++i) {
      if (doors_[i].fixed && (doors_[i].key >> 63) != 0 && !covers(doors_[i].hinge)) {
        if (door_bodies_[i].body) changed = true;
        drop_door_body(door_bodies_[i]);
        --door_stats_.declared;
        continue;
      }
      if (kept != i) {
        doors_[kept] = doors_[i];
        door_bodies_[kept] = door_bodies_[i];
      }
      ++kept;
    }
    doors_.resize(kept);
    door_bodies_.resize(kept);
    door_stats_.doors = doors_.size();
    for (u32 i = 0; i < doors_.size();
         ++i) {  // let go with the tile first, then made, in key order
      if (door_bodies_[i].body && !covers(doors_[i].hinge)) {
        drop_door_body(door_bodies_[i]);
        changed = true;
      }
    }
    for (u32 i = 0; i < doors_.size(); ++i) {
      HeldDoor& held = door_bodies_[i];
      if (held.body || !covers(doors_[i].hinge)) continue;
      if (physics_->create_box(doors_[i].half_extents, held.shape) != physics::Status::Ok) continue;
      physics::BodyDesc body;
      body.shape = held.shape;
      body.transform = door_pose(doors_[i], doors_[i].angle_mdeg);
      body.motion = physics::MotionType::Kinematic;
      body.layer = physics::Layer::Kinematic;
      body.friction = 0.8f;
      if (physics_->create_body(body, held.body) != physics::Status::Ok) {
        physics_->destroy_shape(held.shape);
        held.shape = {};
        continue;
      }
      changed = true;
    }
  }
  if (doors_.empty()) return 0;
  // Each door toward its target, whether or not its tile is held (its state is the world's), held
  // where the leaf at the next angle would reach into the walker.
  const physics::CharacterConfig* c = walker != nullptr ? &walker->config() : nullptr;
  const WorldPos feet = walker != nullptr ? walker->feet() : WorldPos{};
  const f32 reach = c != nullptr ? c->radius + k_door_hold_margin_m : 0.0f;
  u32 moved = 0;
  u32 bodies = 0;
  for (u32 i = 0; i < doors_.size(); ++i) {
    DoorBody& d = doors_[i];
    HeldDoor& held = door_bodies_[i];
    bodies += held.body ? 1u : 0u;
    const i32 target = door_target(d);
    // A door that stands stays at the angle it is drawn at, a door hung ajar among them.
    if (d.fixed || d.angle_mdeg == target) continue;
    const i32 step = d.speed_mdeg > 0 ? d.speed_mdeg : k_door_speed_mdeg;
    const i32 dir = target > d.angle_mdeg ? 1 : -1;
    const i32 travel = std::min(step, dir > 0 ? target - d.angle_mdeg : d.angle_mdeg - target);
    // **The arc, not only its end**: the leaf is tested every `k_door_sweep_mdeg` along this tick's
    // travel and goes as far as the last angle clear of the walker, so a fast door (a game's speed,
    // up to half a turn a tick) never passes through it in one tick. At the default speed the
    // travel is one test, at its end, as it always was. **A leaf already in the walker** (a
    // restore, a drop or a teleport into its arc) moves while each angle reaches less far into it
    // than the one before — away from it — and holds the moment one would reach further, so it
    // clears the walker the way it can and never closes on it (docs: "Doors").
    // The depth at the angle it stands at is asked only when the first angle tested is in: a leaf
    // clear there goes either way (from in, it is less far; from clear, it is clear), so a door
    // swinging clear of the walker pays one test a degree, as before the rule.
    i32 reached = travel;
    bool was_in = false;
    if (c != nullptr) {
      reached = 0;
      f32 before = 0.0f;
      const i32 first = std::min(travel, k_door_sweep_mdeg);
      for (i32 s = first;; s = std::min(travel, s + k_door_sweep_mdeg)) {
        const f32 depth = leaf_depth(d, d.angle_mdeg + dir * s, feet, reach, c->height);
        if (depth > 0.0f) {
          if (s == first) {
            before = leaf_depth(d, d.angle_mdeg, feet, reach, c->height);
            was_in = before > 0.0f;
          }
          if (!(before > 0.0f && depth < before)) break;
        }
        reached = s;
        before = depth;
        if (s == travel) break;
      }
    }
    if (reached < travel) ++door_stats_.holds;
    if (reached == 0) continue;
    if (was_in) ++door_stats_.clears;
    const i32 next = d.angle_mdeg + dir * reached;
    d.angle_mdeg = next;
    ++door_stats_.moves;
    ++moved;
    // **A swing is a move** (docs: "Doors"), as a prop's is: the body is put at its angle, and the
    // kinematic tree's node follows it there; nothing is rebuilt for it. Until 2026-11-18 every
    // tick a door swung rebuilt the broadphase over every body the world holds.
    if (held.body) (void)physics_->set_body_transform(held.body, door_pose(d, next), false);
  }
  door_stats_.bodies = bodies;
  // Rebuilt once a step that made or let go a body (a removed body's node comes back only so).
  if (changed) physics_->optimize_broad_phase();
  return moved;
}

}  // namespace engine::scene_collision

// The scene's collision round a walker (scene_collision.h; docs/subsystems/scene_collision.md).

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/log/log.h>
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

}  // namespace

f32 spacing_tunable() noexcept { return static_cast<f32>(collision_spacing.get()); }
f32 radius_tunable() noexcept { return static_cast<f32>(collision_radius.get()); }
u32 max_tiles_tunable() noexcept { return static_cast<u32>(collision_max_tiles.get()); }
f32 mesh_error_tunable() noexcept { return static_cast<f32>(collision_mesh_error.get()); }
f32 ground_error_tunable() noexcept { return static_cast<f32>(collision_ground_error.get()); }
u32 max_refreshes_tunable() noexcept { return static_cast<u32>(collision_max_refreshes.get()); }
f32 prefetch_tunable() noexcept { return static_cast<f32>(collision_prefetch.get()); }

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
  static constexpr u32 k_queued = 0, k_running = 1, k_done = 2;
  std::atomic<u32> state{k_queued};
  jobs::Counter ran;

  void compute() noexcept {
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

SceneCollision::SceneCollision() = default;

SceneCollision::~SceneCollision() {
  // Every job that may still touch a build has run before the builds go, and the builds' and the
  // proxies' shapes are released before the physics world they were built in goes.
  for (const std::unique_ptr<Build>& build : builds_)
    build->ran.wait_blocking();
  for (const std::unique_ptr<Build>& build : retired_)
    build->ran.wait_blocking();
  builds_.clear();
  retired_.clear();
  if (physics_ != nullptr) {
    for (u32 i = 0; i < tiles_.size(); ++i) {
      Tile& tile = tiles_.value_at(i);
      drop_ground_body(tile);
      if (tile.placements) physics_->destroy_body(tile.placements);
      if (tile.placements_shape) physics_->destroy_shape(tile.placements_shape);
    }
  }
  tiles_.clear();
  tick_scratch_.children.clear();
  proxies_.clear();
  entries_.clear();
}

scene_gen::Context SceneCollision::context_for(const Entry& entry) const noexcept {
  scene_gen::Context context;
  context.world_seed = scene_->terrain.enabled ? scene_->terrain.seed : 0;
  context.tile_size = config_.tile_size;
  context.dir = entry.source->dir;
  context.where = entry.source->where;
  if (ground_ != nullptr) context.ground = ground_->view();
  // The inner ring's representation: what is drawn round the walker (the ruins' blocks near).
  context.world = &world_rings_;
  context.ring = 0;
  return context;
}

bool SceneCollision::create(physics::World& physics, const renderer::SceneData& scene,
                            const scene_gen::GroundProvider* ground, const Config& config,
                            std::string* error, const scene_gen::TileSource* tiles,
                            jobs::JobSystem* jobs) {
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
  explicit_source_ = tiles != nullptr && tiles->valid();
  source_ = explicit_source_     ? *tiles
            : ground_ != nullptr ? ground_->tiles()
                                 : scene_gen::TileSource{};
  own_time_s_ = scene.terrain.time_s;
  config_ = config;
  spacing_mm_ = static_cast<i64>(std::llround(static_cast<f64>(config.spacing_m) * 1000.0));
  if (spacing_mm_ < 1) spacing_mm_ = 1;
  tile_mm_ = static_cast<i64>(std::llround(static_cast<f64>(config.tile_size) * 1000.0));
  stats_ = Stats{};

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

  // A scene read whole: every instance's bounds, binned by the tiles they reach. The terrain is
  // the ground's, and a skinned instance moves, so neither has a static body.
  bins_.clear();
  if (scene.streamed.empty()) {
    for (u32 i = 0; i < scene.instances.size(); ++i) {
      const gfx::InstanceDesc& instance = scene.instances[i];
      if (instance.mesh == scene.terrain_mesh || instance.mesh >= scene.parts.size()) continue;
      if (water_mesh(scene, instance.mesh)) continue;
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
      if ((x1 - x0 + 1) * (z1 - z0 + 1) > 4096)
        continue;  // a scene-sized mesh is ground, not a piece
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
  // The proxies are built on whichever thread asks first; their counters are copied here, on the
  // tick, under their lock.
  std::lock_guard<std::mutex> lock(proxies_mutex_);
  stats_.proxies = proxies_.size();
  stats_.proxy_triangles = proxy_stats_.triangles;
  stats_.flora_triangles_left_out = proxy_stats_.flora_left_out;
  stats_.proxy_bytes = proxy_stats_.bytes;
  stats_.proxy_ns = proxy_stats_.ns;
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

bool SceneCollision::make_ground_body(Tile& tile) {
  const i64 start = time::monotonic_ns();
  struct Count {
    i64 start;
    i64& into;
    ~Count() { into += time::monotonic_ns() - start; }
  } count{start, stats_.body_ns};
  physics::HeightfieldDesc desc;
  desc.heights = std::span<const f32>(tile.heights.data(), tile.heights.size());
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
  const u64 underfoot_key = world::tile_key(world::TileCoord{
      static_cast<i32>(floor_div(xm, tile_mm_)), static_cast<i32>(floor_div(zm, tile_mm_))});
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
    const bool underfoot = order_[k] == underfoot_key;
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
  const u32 n = tile->n;
  const auto sample = [&](i64 a, i64 b) {
    return tile->heights[static_cast<u32>(b) * n + static_cast<u32>(a)];
  };
  out = cell_height(sample(i, j), sample(i + 1, j), sample(i, j + 1), sample(i + 1, j + 1),
                    static_cast<f32>(fx - static_cast<f64>(i)),
                    static_cast<f32>(fz - static_cast<f64>(j)));
  return true;
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
    const physics::LayerMask statics = physics::LayerMask::of(physics::Layer::Static);
    standing =
        physics_->cast_ray(feet + DVec3{0.0, 0.05, 0.0},
                           Vec3{0.0f, -(0.05f + walker.config().radius), 0.0f}, hit, statics) &&
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

// A mesh's proxy at a scale: the scene's cluster LOD DAG of the mesh cut where each cluster's own
// error is within `mesh_error_m` and its parent's is not (`geometry::lod_selects`' rule with the
// threshold in metres rather than pixels) — the coarsest level that stays that close to the
// surface — welded by source vertex so the backend sees shared edges, scaled into the instance's
// size. One shape per (mesh, scale), shared by every instance. **Thread-safe**: a build ahead on a
// worker and the tick may both ask, so the cache is looked in and a proxy built under its lock —
// one proxy is built at a time, whoever asks, and every asker of a key gets the one shape. The
// scene's DAG is read only, and the cut is made in the asker's scratch.
const physics::BuiltShape* SceneCollision::proxy_for(u32 mesh, Vec3 scale,
                                                     PlacementScratch& scratch) {
  const u64 key = proxy_key(mesh, scale);
  std::lock_guard<std::mutex> lock(proxies_mutex_);
  if (const std::unique_ptr<Proxy>* proxy = proxies_.find_value(key))
    return (*proxy)->shape ? &(*proxy)->shape : nullptr;
  Vector<Vec3>& vertices = scratch.vertices;
  Vector<u32>& indices = scratch.indices;
  FlatMap<u32, u32>& remap = scratch.remap;
  const i64 start = time::monotonic_ns();
  const geometry::ClusterMeshPart& part = scene_->parts[mesh];
  const geometry::ClusterLodMesh& lod = scene_->lod;
  const f32 largest =
      std::max(std::fabs(scale.x), std::max(std::fabs(scale.y), std::fabs(scale.z)));
  // The threshold is in the mesh's own units: the instance's scale (its fit included, since the
  // scale was taken from the whole world matrix) multiplies every error by up to `largest`.
  const f32 threshold = config_.mesh_error_m / (largest > 0.0f ? largest : 1.0f);
  vertices.clear();
  indices.clear();
  remap.clear();
  const bool has_lod = lod.lod.size() == lod.mesh.clusters.size();
  // **A tree collides by its wood** (scene_collision.md, "What gets no body"): a mesh that carries
  // the motion contract keeps a triangle only when all three corners are of a part that stays a
  // collider — the trunk, and a load-bearing limb where the structural table says so — so the
  // walker walks into a palm's crown, where the renderer's push bends the fronds aside, and is
  // still stopped by the trunk.
  const renderer::SourceMesh* tree =
      mesh < scene_->sources.size() && scene_->sources[mesh].has_motion() ? &scene_->sources[mesh]
                                                                          : nullptr;
  const std::span<const geometry::ClusterFileStructuralPart> structure =
      tree != nullptr ? std::span<const geometry::ClusterFileStructuralPart>(
                            tree->structural_parts.data(), tree->structural_parts.size())
                      : std::span<const geometry::ClusterFileStructuralPart>();
  const auto* species = tree != nullptr ? geometry::flora_species_info(tree->species) : nullptr;
  const auto kind = species != nullptr ? species->motion : geometry::FloraMotionKind::none;
  auto wooden = [&](u32 v) {
    const u32 at = v - part.first_vertex;
    return at < tree->motion_vertices.size() &&
           renderer::flora_part_collides(structure, tree->motion_vertices[at].part, kind);
  };
  for (u32 c = 0; c < part.cluster_count; ++c) {
    const u32 index = part.first_cluster + c;
    if (has_lod) {
      const geometry::ClusterLodDesc& d = lod.lod[index];
      if (!(d.own_error <= threshold && threshold < d.parent_error)) continue;
    } else if (c < part.first_leaf_cluster ||
               c >= part.first_leaf_cluster + part.leaf_cluster_count) {
      continue;
    }
    const geometry::ClusterDesc& cluster = lod.mesh.clusters[index];
    for (u32 t = 0; t < cluster.triangle_count; ++t) {
      const u32 packed = lod.mesh.triangles[cluster.triangle_offset + t];
      const u32 local[3] = {packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu};
      if (tree != nullptr &&
          !(wooden(cluster.vertex_offset + local[0]) && wooden(cluster.vertex_offset + local[1]) &&
            wooden(cluster.vertex_offset + local[2]))) {
        ++proxy_stats_.flora_left_out;
        continue;
      }
      u32 corner[3];
      for (u32 k = 0; k < 3; ++k) {
        const u32 v = cluster.vertex_offset + local[k];
        const u32 source = v < lod.mesh.vertex_source.size() ? lod.mesh.vertex_source[v] : v;
        if (const u32* seen = remap.find_value(source)) {
          corner[k] = *seen;
          continue;
        }
        const Vec3 p = lod.mesh.vertices[v];
        corner[k] = vertices.size();
        vertices.push_back(Vec3{p.x * scale.x, p.y * scale.y, p.z * scale.z});
        remap.insert(source, corner[k]);
      }
      if (corner[0] == corner[1] || corner[1] == corner[2] || corner[0] == corner[2]) continue;
      // A mirror's scale — an odd number of negative axes — reverses the winding; put it back, so
      // the faces face out.
      const bool mirrored = ((scale.x < 0.0f) != (scale.y < 0.0f)) != (scale.z < 0.0f);
      indices.push_back(corner[0]);
      indices.push_back(mirrored ? corner[2] : corner[1]);
      indices.push_back(mirrored ? corner[1] : corner[2]);
    }
  }
  auto proxy = std::make_unique<Proxy>();
  if (!indices.empty() &&
      physics_->build_mesh(std::span<const Vec3>(vertices.data(), vertices.size()),
                           std::span<const u32>(indices.data(), indices.size()),
                           proxy->shape) == physics::Status::Ok) {
    proxy->triangles = indices.size() / 3;
    proxy_stats_.triangles += proxy->triangles;
    u64 bytes = 0;
    u32 triangles = 0;
    if (physics_->built_memory(proxy->shape, false, bytes, triangles)) proxy_stats_.bytes += bytes;
  }
  const physics::BuiltShape* out = proxy->shape ? &proxy->shape : nullptr;
  proxies_.try_emplace(key, std::move(proxy));
  proxy_stats_.ns += time::monotonic_ns() - start;
  return out;
}

WorldPos SceneCollision::tile_corner(world::TileCoord coord) const noexcept {
  return WorldPos{static_cast<f64>(i64{coord.x} * tile_mm_) / 1000.0, 0.0,
                  static_cast<f64>(i64{coord.z} * tile_mm_) / 1000.0};
}

bool SceneCollision::add_piece(u32 mesh, const Mat4& linear, WorldPos at, WorldPos corner,
                               PlacementScratch& scratch) {
  if (mesh >= scene_->parts.size() || mesh == scene_->terrain_mesh) return false;
  if (water_mesh(*scene_, mesh)) return false;
  Placement placed;
  if (!decompose(linear, placed)) return false;
  const physics::BuiltShape* shape = proxy_for(mesh, placed.scale, scratch);
  if (shape == nullptr) return false;
  physics::BuiltChild child;
  child.shape = shape;
  // In the compound's frame, which is the tile's corner: a few tens of metres at most, the
  // difference taken in f64 and rounded once.
  child.transform.position = relative(at, corner);
  child.transform.rotation = placed.rotation;
  scratch.children.push_back(child);
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
  scratch.children.clear();
  const u64 key = world::tile_key(coord);
  const WorldPos corner = tile_corner(coord);
  if (!entries_.empty()) {
    const scene_gen::TileCoord at{coord.x, coord.z};
    for (const std::unique_ptr<Entry>& entry_ptr : entries_) {
      const Entry& entry = *entry_ptr;
      if (entry.generator->occupies != nullptr && !entry.generator->occupies(entry.state, at))
        continue;
      scratch.placed.clear();
      std::string why;
      bool made = false;
      {
        std::lock_guard<std::mutex> lock(generators_mutex_);
        made = entry.generator->tile(entry.state, at, context_for(entry), scratch.placed, &why);
      }
      if (!made) {
        out.ok = false;
        ENGINE_LOG_WARN(log_collision, "a tile's placements were not made",
                        log::field("entry", entry.source->where), log::field("x", coord.x),
                        log::field("z", coord.z), log::field("error", why));
        continue;
      }
      for (const scene_gen::Placement& p : scratch.placed.instances) {
        if (p.mesh >= entry.source->meshes.size()) continue;
        const u32 mesh = entry.source->meshes[p.mesh];
        const Mat4 fit = mesh < scene_->mesh_fit.size() ? scene_->mesh_fit[mesh] : Mat4::identity();
        // Where the renderer stands the same placement (`renderer::instance_translation`): the
        // fit's offset through the rotation and scale in float32, the size of the mesh, and the
        // sum with the placement's `WorldPos` in f64 — where its generator put it, anywhere.
        const Mat4 linear = mat4_from_transform(p.turn()) * fit;
        const WorldPos stands = p.position + DVec3{linear.c[3].xyz()};
        (void)add_piece(mesh, linear, stands, corner, scratch);
      }
    }
  } else if (const Vector<u32>* bin = bins_.find_value(key)) {
    for (const u32 i : *bin) {
      const gfx::InstanceDesc& instance = scene_->instances[i];
      (void)add_piece(instance.mesh, cell_frame_matrix(instance),
                      to_world(gfx::instance_cell(instance)), corner, scratch);
    }
  }
  if (scratch.children.empty()) return;
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

void SceneCollision::retire(std::unique_ptr<Build>&& build) {
  if (build->ran.done()) return;  // its job has returned: nothing else touches it
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

void SceneCollision::prefetch(WorldPos at) {
  // Builds let go whose jobs have returned are freed, and with them any shape they built.
  for (u32 k = 0; k < retired_.size();) {
    if (retired_[k]->ran.done()) {
      retired_[k] = std::move(retired_[retired_.size() - 1]);
      retired_.pop_back();
    } else {
      ++k;
    }
  }
  if (!can_prefetch()) return;
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
  const i64 cx = static_cast<i64>(std::floor(fx));
  const i64 cz = static_cast<i64>(std::floor(fz));
  const i64 reach = static_cast<i64>(std::ceil(r)) + 1;
  auto within = [&](i64 x, i64 z, f64 radius) {
    const f64 dx = static_cast<f64>(x) + 0.5 - fx;
    const f64 dz = static_cast<f64>(z) + 0.5 - fz;
    return dx * dx + dz * dz <= radius * radius;
  };
  // Builds the walker has left behind — past the radius and a tile — are let go.
  for (u32 k = 0; k < builds_.size();) {
    if (!within(builds_[k]->coord.x, builds_[k]->coord.z, r + 1.0)) {
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
  for (i64 z = cz - reach; z <= cz + reach; ++z) {
    for (i64 x = cx - reach; x <= cx + reach; ++x) {
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
      build->ran.add(1);
      Build* raw = build.get();
      builds_.push_back(std::move(build));
      jobs_->schedule(jobs::Pool::Performance, jobs::Job{&Build::run, raw, &raw->ran});
      ++stats_.prefetch_submitted;
    }
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
  self->tiles_.erase(key);
  ++self->stats_.deactivated;
  self->note_bodies();
  self->stats_.deactivate_ns += time::monotonic_ns() - start;
}

}  // namespace engine::scene_collision

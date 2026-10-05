// The scene's collision round a walker (scene_collision.h; docs/subsystems/scene_collision.md).

#include <core/hash/hash.h>
#include <core/log/log.h>
#include <core/time/time.h>
#include <domain/physics/character.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/scene.h>
#include <systems/scene_collision/scene_collision.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace engine::scene_collision {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_collision, "scene_collision");

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

i64 floor_div(i64 a, i64 b) noexcept {
  i64 q = a / b;
  if ((a % b) != 0 && ((a < 0) != (b < 0))) --q;
  return q;
}
i64 ceil_div(i64 a, i64 b) noexcept { return -floor_div(-a, b); }

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

// A world matrix as a rigid placement and a scale the proxy is built at: the backend places a body
// by a position and a rotation, and a shape carries its own size (physics.md, `BodyDesc`). A mirror
// goes into the scale's x.
struct Placement {
  Vec3 position{};
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
  out.position = m.c[3].xyz();
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

SceneCollision::SceneCollision() = default;

SceneCollision::~SceneCollision() {
  if (physics_ == nullptr) return;
  for (u32 i = 0; i < tiles_.size(); ++i) {
    Tile& tile = tiles_.value_at(i);
    drop_ground_body(tile);
    if (tile.placements) physics_->destroy_body(tile.placements);
    if (tile.placements_shape) physics_->destroy_shape(tile.placements_shape);
  }
  tiles_.clear();
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
                            std::string* error, const scene_gen::TileSource* tiles) {
  auto fail = [&](const std::string& why) {
    if (error != nullptr) *error = why;
    return false;
  };
  if (!physics.initialized()) return fail("scene collision needs an initialized physics world");
  if (!(config.tile_size > 0.0f) || !(config.spacing_m > 0.0f)) {
    return fail("scene collision needs a positive tile size and ground spacing");
  }
  physics_ = &physics;
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
      if (i < scene.instance_joints.size() && scene.instance_joints[i] != 0) continue;
      const geometry::ClusterMeshPart& part = scene.parts[instance.mesh];
      Aabb3 local = Aabb3::empty();
      for (u32 c = 0; c < part.cluster_count; ++c) {
        const geometry::ClusterDesc& cluster = scene.lod.mesh.clusters[part.first_cluster + c];
        local.expand(cluster.center - Vec3(cluster.radius));
        local.expand(cluster.center + Vec3(cluster.radius));
      }
      if (local.is_empty()) continue;
      const Aabb3 bounds = transform_aabb(renderer::instance_world_matrix(instance), local);
      const i64 x0 = floor_div(static_cast<i64>(std::floor(bounds.min.x * 1000.0f)), tile_mm_);
      const i64 x1 = floor_div(static_cast<i64>(std::floor(bounds.max.x * 1000.0f)), tile_mm_);
      const i64 z0 = floor_div(static_cast<i64>(std::floor(bounds.min.z * 1000.0f)), tile_mm_);
      const i64 z1 = floor_div(static_cast<i64>(std::floor(bounds.max.z * 1000.0f)), tile_mm_);
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
  stats_.proxies = proxies_.size();
}

// ---- the ground
// ----------------------------------------------------------------------------------

bool SceneCollision::evaluate_field(const Tile& tile, f64 time, Vector<f32>& out) {
  out.resize(tile.n * tile.n);
  ++stats_.field_evaluations;
  return source_.heights(time, spacing_mm_, tile.i0, tile.j0, tile.n, tile.n,
                         std::span<f32>(out.data(), out.size()));
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
  physics::HeightfieldDesc desc;
  desc.heights = std::span<const f32>(tile.heights.data(), tile.heights.size());
  desc.sample_count = tile.n;
  const f32 spacing = static_cast<f32>(static_cast<f64>(spacing_mm_) / 1000.0);
  desc.offset = Vec3{static_cast<f32>(static_cast<f64>(i64{tile.i0} * spacing_mm_) / 1000.0), 0.0f,
                     static_cast<f32>(static_cast<f64>(i64{tile.j0} * spacing_mm_) / 1000.0)};
  desc.scale = Vec3{spacing, 1.0f, spacing};
  if (physics_->create_heightfield(desc, tile.ground_shape) != physics::Status::Ok) return false;
  u32 triangles = 0;
  (void)physics_->shape_memory(tile.ground_shape, false, tile.ground_bytes, triangles);
  physics::BodyDesc body;
  body.shape = tile.ground_shape;
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
bool SceneCollision::build_ground(Tile& tile) {
  const i64 x0 = i64{tile.coord.x} * tile_mm_;
  const i64 z0 = i64{tile.coord.z} * tile_mm_;
  tile.i0 = static_cast<i32>(floor_div(x0, spacing_mm_));
  tile.j0 = static_cast<i32>(floor_div(z0, spacing_mm_));
  const i64 xi = ceil_div(x0 + tile_mm_, spacing_mm_);
  const i64 zj = ceil_div(z0 + tile_mm_, spacing_mm_);
  u32 n = static_cast<u32>(std::max(xi - tile.i0, zj - tile.j0)) + 1u;
  n = n < 4 ? 4 : n + (n & 1u);
  tile.n = n;
  tile.moving = time_.moving && source_.moves();
  if (!tile.moving) {
    tile.heights.resize(n * n);
    // The ground at its own time: the provider's `grid`, or a tile source's heights at the scene's
    // time (a still source's one surface), which for the scene's own ground is the same bits.
    if (explicit_source_) {
      (void)source_.heights(own_time_s_, spacing_mm_, tile.i0, tile.j0, n, n,
                            std::span<f32>(tile.heights.data(), tile.heights.size()));
    } else {
      ground_->grid(scene_gen::ring_lattice(spacing_mm_), tile.i0, tile.j0, n, n,
                    std::span<f32>(tile.heights.data(), tile.heights.size()));
    }
    tile.field_a.clear();
    tile.field_b.clear();
    return make_ground_body(tile);
  }
  if (!pair_fields(tile)) return false;
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

u32 SceneCollision::refresh(f32 x, f32 z, f32 reach) {
  if (ground_ == nullptr || !time_.moving || !source_.moves() || tiles_.empty()) return 0;
  // The tiles under the walker: every tile the square `reach` either side of (x, z) touches, which
  // is the one holding the point unless a capsule of that radius stands near a tile's edge.
  const f64 r = reach > 0.0f ? static_cast<f64>(reach) : 0.0;
  const i64 ux0 =
      floor_div(static_cast<i64>(std::floor((static_cast<f64>(x) - r) * 1000.0)), tile_mm_);
  const i64 ux1 =
      floor_div(static_cast<i64>(std::floor((static_cast<f64>(x) + r) * 1000.0)), tile_mm_);
  const i64 uz0 =
      floor_div(static_cast<i64>(std::floor((static_cast<f64>(z) - r) * 1000.0)), tile_mm_);
  const i64 uz1 =
      floor_div(static_cast<i64>(std::floor((static_cast<f64>(z) + r) * 1000.0)), tile_mm_);
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
  const f32 tile = config_.tile_size;
  auto distance2 = [&](u64 key) {
    const world::TileCoord c = world::tile_of_key(key);
    const f32 dx = (static_cast<f32>(c.x) + 0.5f) * tile - x;
    const f32 dz = (static_cast<f32>(c.z) + 0.5f) * tile - z;
    return dx * dx + dz * dz;
  };
  // The tiles under the walker first, then nearest first; the budget is the others'.
  std::sort(order_.begin(), order_.end(), [&](u64 a, u64 b) {
    const bool ua = under_walker(a);
    const bool ub = under_walker(b);
    if (ua != ub) return ua;
    const f32 da = distance2(a);
    const f32 db = distance2(b);
    return da != db ? da < db : a < b;
  });
  u32 under_count = 0;
  while (under_count < order_.size() && under_walker(order_[under_count]))
    ++under_count;
  const u32 count = under_count + std::min<u32>(static_cast<u32>(order_.size()) - under_count,
                                                config_.max_refreshes);
  const i64 xm = static_cast<i64>(std::floor(static_cast<f64>(x) * 1000.0));
  const i64 zm = static_cast<i64>(std::floor(static_cast<f64>(z) * 1000.0));
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

bool SceneCollision::ground_height(f32 x, f32 z, f32& out) const noexcept {
  const i64 xm = static_cast<i64>(std::floor(static_cast<f64>(x) * 1000.0));
  const i64 zm = static_cast<i64>(std::floor(static_cast<f64>(z) * 1000.0));
  const world::TileCoord coord{static_cast<i32>(floor_div(xm, tile_mm_)),
                               static_cast<i32>(floor_div(zm, tile_mm_))};
  const Tile* tile = tiles_.find_value(world::tile_key(coord));
  if (tile == nullptr || tile->heights.empty()) return false;
  const f64 spacing = static_cast<f64>(spacing_mm_) / 1000.0;
  const f64 fx = static_cast<f64>(x) / spacing - static_cast<f64>(tile->i0);
  const f64 fz = static_cast<f64>(z) / spacing - static_cast<f64>(tile->j0);
  const i64 i = static_cast<i64>(std::floor(fx));
  const i64 j = static_cast<i64>(std::floor(fz));
  if (i < 0 || j < 0 || i + 1 >= tile->n || j + 1 >= tile->n) return false;
  const u32 n = tile->n;
  const auto at = [&](i64 a, i64 b) {
    return tile->heights[static_cast<u32>(b) * n + static_cast<u32>(a)];
  };
  out = cell_height(at(i, j), at(i + 1, j), at(i, j + 1), at(i + 1, j + 1),
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
  const Vec3 feet = walker.feet();
  if (ground_ == nullptr || !time_.moving || !source_.moves()) {
    return refresh(feet.x, feet.z, walker.config().radius);
  }
  // Standing on the ground, not on a placement: what is straight under the capsule's centre, from
  // just above its feet down past the round bottom's rise on the steepest ground it stands on
  // (0.3 (1 / cos 40 - 1) = 9 cm, and the backend's padding), is a held tile's heightfield.
  f32 before = 0.0f;
  bool standing = false;
  if (walker.state().ground == physics::Ground::OnGround && ground_height(feet.x, feet.z, before)) {
    physics::RayHit hit;
    const physics::LayerMask statics = physics::LayerMask::of(physics::Layer::Static);
    standing =
        physics_->cast_ray(Vec3{feet.x, feet.y + 0.05f, feet.z},
                           Vec3{0.0f, -(0.05f + walker.config().radius), 0.0f}, hit, statics) &&
        ground_body(hit.body);
  }
  const u32 done = refresh(feet.x, feet.z, walker.config().radius);
  f32 after = 0.0f;
  if (!ground_height(feet.x, feet.z, after)) return done;
  Vec3 to = feet;
  if (standing && after != before) {
    // Carried: the offset it stood at over the ground (a slope's rise under its round bottom, the
    // backend's padding) is kept, so a standing walker neither hovers nor sinks. Added to the new
    // height rather than the change to the feet, so feet that stood at or over the ground are at
    // or over it after, to the bit.
    to.y = after + (feet.y - before);
    ++stats_.carries;
    stats_.max_carry_m = std::max(stats_.max_carry_m, std::fabs(static_cast<f64>(after - before)));
  }
  if (to.y < after) {
    // Under the collision ground: lifted onto it. Counted from a tenth of a millimetre: below that
    // it is the character settling a step's worth of gravity into a surface it stands on.
    const f64 lift = static_cast<f64>(after - to.y);
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
// size. One shape per (mesh, scale), shared by every instance.
bool SceneCollision::proxy_for(u32 mesh, Vec3 scale, physics::ShapeId& out) {
  const u64 key = proxy_key(mesh, scale);
  if (const Proxy* proxy = proxies_.find_value(key)) {
    out = proxy->shape;
    return static_cast<bool>(out);
  }
  const i64 start = time::monotonic_ns();
  const geometry::ClusterMeshPart& part = scene_->parts[mesh];
  const geometry::ClusterLodMesh& lod = scene_->lod;
  const f32 largest =
      std::max(std::fabs(scale.x), std::max(std::fabs(scale.y), std::fabs(scale.z)));
  // The threshold is in the mesh's own units: the instance's scale (its fit included, since the
  // scale was taken from the whole world matrix) multiplies every error by up to `largest`.
  const f32 threshold = config_.mesh_error_m / (largest > 0.0f ? largest : 1.0f);
  proxy_vertices_.clear();
  proxy_indices_.clear();
  proxy_remap_.clear();
  const bool has_lod = lod.lod.size() == lod.mesh.clusters.size();
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
      u32 corner[3];
      for (u32 k = 0; k < 3; ++k) {
        const u32 v = cluster.vertex_offset + local[k];
        const u32 source = v < lod.mesh.vertex_source.size() ? lod.mesh.vertex_source[v] : v;
        if (const u32* seen = proxy_remap_.find_value(source)) {
          corner[k] = *seen;
          continue;
        }
        const Vec3 p = lod.mesh.vertices[v];
        corner[k] = proxy_vertices_.size();
        proxy_vertices_.push_back(Vec3{p.x * scale.x, p.y * scale.y, p.z * scale.z});
        proxy_remap_.insert(source, corner[k]);
      }
      if (corner[0] == corner[1] || corner[1] == corner[2] || corner[0] == corner[2]) continue;
      // A mirror's scale — an odd number of negative axes — reverses the winding; put it back, so
      // the faces face out.
      const bool mirrored = ((scale.x < 0.0f) != (scale.y < 0.0f)) != (scale.z < 0.0f);
      proxy_indices_.push_back(corner[0]);
      proxy_indices_.push_back(mirrored ? corner[2] : corner[1]);
      proxy_indices_.push_back(mirrored ? corner[1] : corner[2]);
    }
  }
  Proxy proxy;
  if (!proxy_indices_.empty() &&
      physics_->create_mesh(std::span<const Vec3>(proxy_vertices_.data(), proxy_vertices_.size()),
                            std::span<const u32>(proxy_indices_.data(), proxy_indices_.size()),
                            proxy.shape) == physics::Status::Ok) {
    proxy.triangles = proxy_indices_.size() / 3;
    stats_.proxy_triangles += proxy.triangles;
    u64 bytes = 0;
    u32 triangles = 0;
    if (physics_->shape_memory(proxy.shape, false, bytes, triangles)) stats_.proxy_bytes += bytes;
  }
  proxies_.insert(key, proxy);
  stats_.proxy_ns += time::monotonic_ns() - start;
  out = proxy.shape;
  return static_cast<bool>(out);
}

bool SceneCollision::add_piece(u32 mesh, const Mat4& world,
                               Vector<physics::CompoundChild>& children) {
  if (mesh >= scene_->parts.size() || mesh == scene_->terrain_mesh) return false;
  Placement placed;
  if (!decompose(world, placed)) return false;
  physics::ShapeId shape;
  if (!proxy_for(mesh, placed.scale, shape)) return false;
  physics::CompoundChild child;
  child.shape = shape;
  child.transform.position = placed.position;
  child.transform.rotation = placed.rotation;
  children.push_back(child);
  return true;
}

// Every piece on the tile as one static compound of shared proxies: one body a tile whatever the
// tile holds (a ruin laid in blocks is hundreds of pieces), with the compound's own tree doing the
// culling the broadphase would otherwise do per piece.
bool SceneCollision::build_placements(Tile& tile) {
  children_.clear();
  bool ok = true;
  const u64 key = world::tile_key(tile.coord);
  if (!entries_.empty()) {
    const scene_gen::TileCoord at{tile.coord.x, tile.coord.z};
    for (const std::unique_ptr<Entry>& entry_ptr : entries_) {
      const Entry& entry = *entry_ptr;
      if (entry.generator->occupies != nullptr && !entry.generator->occupies(entry.state, at))
        continue;
      placed_.clear();
      std::string why;
      if (!entry.generator->tile(entry.state, at, context_for(entry), placed_, &why)) {
        ok = false;
        ENGINE_LOG_WARN(log_collision, "a tile's placements were not made",
                        log::field("entry", entry.source->where), log::field("x", tile.coord.x),
                        log::field("z", tile.coord.z), log::field("error", why));
        continue;
      }
      for (const scene_gen::Placement& p : placed_.instances) {
        if (p.mesh >= entry.source->meshes.size()) continue;
        const u32 mesh = entry.source->meshes[p.mesh];
        const Mat4 fit = mesh < scene_->mesh_fit.size() ? scene_->mesh_fit[mesh] : Mat4::identity();
        (void)add_piece(mesh, mat4_from_transform(p.transform) * fit, children_);
      }
    }
  } else if (const Vector<u32>* bin = bins_.find_value(key)) {
    for (const u32 i : *bin) {
      const gfx::InstanceDesc& instance = scene_->instances[i];
      (void)add_piece(instance.mesh, renderer::instance_world_matrix(instance), children_);
    }
  }
  tile.pieces = children_.size();
  if (children_.empty()) return ok;
  if (physics_->create_compound(
          std::span<const physics::CompoundChild>(children_.data(), children_.size()),
          tile.placements_shape) != physics::Status::Ok) {
    tile.pieces = 0;
    return false;
  }
  u32 triangles = 0;
  (void)physics_->shape_memory(tile.placements_shape, false, tile.compound_bytes, triangles);
  physics::BodyDesc body;
  body.shape = tile.placements_shape;
  body.motion = physics::MotionType::Static;
  body.layer = physics::Layer::Static;
  body.friction = 0.8f;
  if (physics_->create_body(body, tile.placements) != physics::Status::Ok) {
    physics_->destroy_shape(tile.placements_shape);
    tile.placements_shape = {};
    tile.pieces = 0;
    return false;
  }
  ++stats_.bodies_created;
  return ok;
}

// ---- the consumer
// ---------------------------------------------------------------------------------

bool SceneCollision::activate(void* context, const world::TileEvent& event) {
  auto* self = static_cast<SceneCollision*>(context);
  const i64 start = time::monotonic_ns();
  Tile tile;
  tile.coord = event.tile;
  bool ok = true;
  if (self->ground_ != nullptr && !self->build_ground(tile)) ok = false;
  if (!self->build_placements(tile)) ok = false;
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

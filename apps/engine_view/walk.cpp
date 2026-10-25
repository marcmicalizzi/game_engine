// The walk mode (walk.h; docs/subsystems/apps.md, "Walking").
#include "walk.h"

#include "fly_camera.h"

#include <core/hash/hash.h>
#include <core/log/log.h>
#include <core/math/world.h>
#include <core/time/time.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/flora_motion.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>

#if ENGINE_VIEW_WALK_PHYSICS
#include <core/jobs/job_system.h>
#include <domain/physics/character.h>
#include <domain/physics/physics.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>
#endif
#if ENGINE_VIEW_TILESET
#include <domain/tileset/tileset.h>
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>

namespace engine::view {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_walk, "view.walk");

// Read once, when a live session starts, into its header (apps.md, "Walking").
tunables::Float walk_height{"view.walk.height", 1.8, 0.5, 5.0,
                            "The walker's capsule, feet to crown, metres"};
tunables::Float walk_radius{"view.walk.radius", 0.3, 0.05, 2.0, "The walker's capsule's radius"};
tunables::Float walk_eye{"view.walk.eye_height", 1.65, 0.1, 5.0,
                         "The walking camera's height over the walker's feet, metres"};
tunables::Float walk_speed{"view.walk.speed", 1.5, 0.01, 100.0, "Walking speed, metres a second"};
tunables::Float walk_sprint{"view.walk.sprint", 5.0, 0.01, 100.0,
                            "Speed with the fast action (Shift) held while walking"};
tunables::Float walk_slope{"view.walk.max_slope_deg", 40.0, 1.0, 89.0,
                           "Ground steeper than this, degrees, cannot be walked up and is slid "
                           "down"};
tunables::Float walk_step{"view.walk.step_height", 0.35, 0.0, 2.0,
                          "A ledge up to this high, metres, is climbed by walking into it"};
tunables::Float walk_gravity{"view.walk.gravity", 9.81, 0.0, 100.0,
                             "Metres a second squared, down, on the walker"};
tunables::Float walk_jump{"view.walk.jump_speed", 4.0, 0.0, 50.0,
                          "The walker's upward speed at the start of a jump (Space)"};

u32 bits(f32 v) noexcept {
  if (v == 0.0f) v = 0.0f;
  return std::bit_cast<u32>(v);
}

u64 bits64(f64 v) noexcept {
  if (v == 0.0) v = 0.0;
  return std::bit_cast<u64>(v);
}

bool read_f32(const JsonValue& object, const char* key, f32& out) {
  const JsonValue* v = object.find(key);
  f64 value = 0.0;
  if (v == nullptr || !v->get_f64(value)) return false;
  out = static_cast<f32>(value);
  return true;
}

bool read_u32(const JsonValue& object, const char* key, u32& out) {
  const JsonValue* v = object.find(key);
  u64 value = 0;
  if (v == nullptr || !v->get_u64(value) || value > 0xFFFFFFFFull) return false;
  out = static_cast<u32>(value);
  return true;
}

std::string hex16(u64 value) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return text;
}

// The ground's direction of travel from the look's yaw: forward along the heading (the camera's
// forward with its pitch taken out) and right, from `fly_sin_cos`, and the stick or the keys'
// diagonal brought back into the unit disc.
Vec3 heading_direction(const WalkInput& in) noexcept {
  f32 s = 0.0f;
  f32 c = 0.0f;
  fly_sin_cos(in.yaw, s, c);
  f32 mx = in.move.x;
  f32 my = in.move.y;
  const f32 length2 = mx * mx + my * my;
  if (length2 > 1.0f) {
    const f32 inverse = 1.0f / std::sqrt(length2);
    mx = mx * inverse;
    my = my * inverse;
  }
  // forward (-sin, 0, -cos) times y, plus right (cos, 0, -sin) times x
  return Vec3{-(s * my) + c * mx, 0.0f, -(c * my) - s * mx};
}

}  // namespace

WalkParams walk_params_from_tunables() {
  WalkParams p;
  p.height = static_cast<f32>(walk_height.get());
  p.radius = static_cast<f32>(walk_radius.get());
  p.eye_height = static_cast<f32>(walk_eye.get());
  p.speed = static_cast<f32>(walk_speed.get());
  p.sprint = static_cast<f32>(walk_sprint.get());
  p.max_slope_deg = static_cast<f32>(walk_slope.get());
  p.step_height = static_cast<f32>(walk_step.get());
  p.gravity = static_cast<f32>(walk_gravity.get());
  p.jump_speed = static_cast<f32>(walk_jump.get());
#if ENGINE_VIEW_WALK_PHYSICS
  p.spacing_m = scene_collision::spacing_tunable();
  p.radius_tiles = scene_collision::radius_tunable();
  p.max_tiles = scene_collision::max_tiles_tunable();
  p.mesh_error_m = scene_collision::mesh_error_tunable();
  p.ground_error_m = scene_collision::ground_error_tunable();
  p.max_refreshes = scene_collision::max_refreshes_tunable();
#endif
  return p;
}

JsonValue walk_params_to_json(const WalkParams& p) {
  JsonValue out = JsonValue::object();
  out.set("version", static_cast<u64>(k_walk_version));
  out.set("height", JsonValue(p.height));
  out.set("radius", JsonValue(p.radius));
  out.set("eye_height", JsonValue(p.eye_height));
  out.set("speed", JsonValue(p.speed));
  out.set("sprint", JsonValue(p.sprint));
  out.set("max_slope_deg", JsonValue(p.max_slope_deg));
  out.set("step_height", JsonValue(p.step_height));
  out.set("gravity", JsonValue(p.gravity));
  out.set("jump_speed", JsonValue(p.jump_speed));
  JsonValue c = JsonValue::object();
  c.set("spacing_m", JsonValue(p.spacing_m));
  c.set("radius_tiles", JsonValue(p.radius_tiles));
  c.set("max_tiles", static_cast<u64>(p.max_tiles));
  c.set("mesh_error_m", JsonValue(p.mesh_error_m));
  c.set("ground_error_m", JsonValue(p.ground_error_m));
  c.set("max_refreshes", static_cast<u64>(p.max_refreshes));
  out.set("collision", std::move(c));
  return out;
}

bool walk_params_from_json(const JsonValue& value, WalkParams& out, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error != nullptr) *error = "session header's walk block: " + why;
    return false;
  };
  if (!value.is_object()) return fail("not an object");
  u32 version = 0;
  if (!read_u32(value, "version", version)) return fail("no version");
  // Version 1's numbers mean what version 2's do; its walk is walked as version 2 (walk.h).
  if (version != k_walk_version && version != k_walk_version_f32) {
    return fail("recorded with walk integration version " + std::to_string(version) +
                ", and this engine-view walks version " + std::to_string(k_walk_version) +
                "; a walk from another version would go somewhere else without saying so, so the "
                "log is refused rather than misread");
  }
  WalkParams p;
  const JsonValue* c = value.find("collision");
  if (!read_f32(value, "height", p.height) || !read_f32(value, "radius", p.radius) ||
      !read_f32(value, "eye_height", p.eye_height) || !read_f32(value, "speed", p.speed) ||
      !read_f32(value, "sprint", p.sprint) || !read_f32(value, "max_slope_deg", p.max_slope_deg) ||
      !read_f32(value, "step_height", p.step_height) || !read_f32(value, "gravity", p.gravity) ||
      !read_f32(value, "jump_speed", p.jump_speed) || c == nullptr || !c->is_object() ||
      !read_f32(*c, "spacing_m", p.spacing_m) || !read_f32(*c, "radius_tiles", p.radius_tiles) ||
      !read_u32(*c, "max_tiles", p.max_tiles) || !read_f32(*c, "mesh_error_m", p.mesh_error_m) ||
      !read_f32(*c, "ground_error_m", p.ground_error_m) ||
      !read_u32(*c, "max_refreshes", p.max_refreshes)) {
    return fail("a field is missing or not a number");
  }
  out = p;
  return true;
}

struct Walker::Impl {
  WalkParams params;
  u32 tick_hz = 240;
  f32 dt = 1.0f / 240.0f;
  const renderer::SceneData* scene = nullptr;
  std::unique_ptr<renderer::TerrainSampler> terrain;
  const scene_gen::GroundProvider* ground = nullptr;
  const char* collision = "none";
  std::string why = "the walker has not started";
  DrawnGround drawn;
  WalkStats stats;
  WalkTiming timing;
  // A ground query's wall time, into `timing` (walk.h, `WalkTiming`).
  struct Query {
    WalkTiming& into;
    i64 start = time::monotonic_ns();
    ~Query() {
      const i64 took = time::monotonic_ns() - start;
      ++into.ground_queries;
      into.ground_query_ns += took;
      into.max_ground_query_ns = std::max(into.max_ground_query_ns, took);
      into.recent_max_query_ns = std::max(into.recent_max_query_ns, took);
    }
  };
  WorldPos feet{};
  bool placed = false;
  // A game's scales on the speeds (walk.h, `set_speed_scale`); 1 leaves every float as it was.
  f32 speed_scale = 1.0f;
  f32 sprint_scale = 1.0f;
  // The water (walk.h, "Wading"): null, none.
  DepthFn depth = nullptr;
  const void* depth_context = nullptr;
  // This step's scale for the depth at the feet, and whether the chest's wall stops it: 1 and false
  // with no water, which multiplies nothing.
  f32 wade(WorldPos at, Vec3 direction) {
    if (depth == nullptr) return 1.0f;
    const i64 asked = time::monotonic_ns();
    const f32 d = depth(depth_context, at);
    ++timing.water_queries;
    timing.water_ns += time::monotonic_ns() - asked;
    if (!(d > 0.0f)) return 1.0f;
    ++stats.wading_ticks;
    stats.max_depth_m = std::max(stats.max_depth_m, d);
    if (d >= k_wade_chest_m && (direction.x != 0.0f || direction.z != 0.0f)) {
      const WorldPos ahead{at.x + static_cast<f64>(direction.x * k_wade_probe_m), at.y,
                           at.z + static_cast<f64>(direction.z * k_wade_probe_m)};
      if (depth(depth_context, ahead) > d) {
        ++stats.stopped_ticks;
        return 0.0f;
      }
    }
    return wade_scale(d);
  }

  // Following the ground.
  f32 vy = 0.0f;
  bool airborne = false;
  // The drawn pair's heights at a point, on a millimetre lattice, and at a cell of the drawn grid:
  // the last asked, because a walker asks the same point or cell many ticks running.
  struct Cache {
    i64 i = std::numeric_limits<i64>::min();
    i64 j = 0;
    f64 time_a = -1.0;
    f64 time_b = -1.0;
    bool moving = false;
    f32 a[4] = {};
    f32 b[4] = {};
  };
  Cache point;
  Cache cell;

#if ENGINE_VIEW_WALK_PHYSICS
  bool physical = false;
  // The worker the collision builds tiles' ground ahead of the ring on (scene_collision.h,
  // `prefetch`): before the consumer, so it outlives the builds the consumer waits for.
  std::unique_ptr<jobs::JobSystem> jobs;
  physics::World world;
  world::World ring;
  scene_collision::SceneCollision consumer;
  physics::CharacterBody body;
  sim::ObserverSet observers;
  u64 ring_tick = 0;

  physics::CharacterConfig character_config() const {
    physics::CharacterConfig c;
    c.height = params.height;
    c.radius = params.radius;
    c.eye_height = params.eye_height;
    c.walk_speed = params.speed;
    c.sprint_speed = params.sprint;
    c.max_slope_deg = params.max_slope_deg;
    c.step_height = params.step_height;
    c.gravity = params.gravity;
    c.jump_speed = params.jump_speed;
    c.step_hz = tick_hz;
    return c;
  }
  scene_collision::GroundTime ground_time() const {
    scene_collision::GroundTime t;
    t.moving = drawn.moving;
    t.time_a = drawn.time_a;
    t.time_b = drawn.time_b;
    t.blend = drawn.blend;
    return t;
  }
  // The ring round (x, z), before a step or a drop: at a fixed tick and from the walker's own
  // position, so a replay makes and lets go of the same bodies at the same ticks (walk.h,
  // "Determinism"). The ground's time goes to the consumer first, so a tile the ring makes now is
  // made at the sand this frame draws; until 2026-09-29 it was made at the last tick's, which after
  // a flight over moving sand was wherever the sand had been when the walker last walked.
  void move_ring(WorldPos at, bool unlimited) {
    observers.clear();        // kept, so a tick allocates nothing
    observers.add(at, 1.0f);  // the walker's own f64 place (ADR-0053)
    consumer.set_ground_time(ground_time());
    // The ground of the tiles about to come in, built ahead on the worker; the bodies are still
    // made by the update below, in its order, so a replay makes the same ones.
    consumer.prefetch(at);
    ring.update(observers, ring_tick++, unlimited);
  }
#endif

  // The provider's height where the walker follows the ground: its own time on still ground, the
  // drawn pair blended as drawn on moving ground. Asked at the nearest millimetre of the f64 place,
  // an i64 that is never a lattice index and never a float32 metre (ADR-0053;
  // `scene_gen::GroundProvider::height_mm`): until 2026-10-05 it was an i32 index on a 1 mm
  // lattice, which overflows past 2,147 km, and the provider's float32 `height` on still ground.
  f32 ground_at(f64 x, f64 z) {
    const Query query{timing};
    const i64 i = scene_gen::nearest_mm(x);
    const i64 j = scene_gen::nearest_mm(z);
    if (!drawn.moving || !ground->moves()) return ground->height_mm(scene->terrain.time_s, i, j);
    if (point.i != i || point.j != j || point.time_a != drawn.time_a ||
        point.time_b != drawn.time_b) {
      point.a[0] = ground->height_mm(drawn.time_a, i, j);
      point.b[0] = ground->height_mm(drawn.time_b, i, j);
      point.i = i;
      point.j = j;
      point.time_a = drawn.time_a;
      point.time_b = drawn.time_b;
    }
    const f32 t = static_cast<f32>(drawn.blend);
    return point.a[0] * (1.0f - t) + point.b[0] * t;
  }

  // The ground as drawn at (x, z): the scene's grid cell under it, as the renderer triangulates a
  // quad (the (x + 1, z)-(x, z + 1) diagonal), at the drawn time. NaN off the grid. On the world's
  // tiles, the finest tile level's cell, which has no edge, from the tiles' own source (a tile is
  // triangulated with the grid's diagonal).
  f32 drawn_at(f64 x, f64 z) {
    const scene_gen::Lattice& l = drawn.lattice;
    const bool tiled = drawn.tiles != nullptr && l.spacing_mm > 0;
    if (!tiled && l.size < 2) return std::numeric_limits<f32>::quiet_NaN();
    // On the world's tiles, the cell and the fractions from the point's millimetres and the
    // lattice's integer spacing, so a lattice point is a whole number anywhere (ADR-0053).
    const f64 fx =
        tiled ? (x * 1000.0) / static_cast<f64>(l.spacing_mm) : (x - l.origin_x) / l.spacing;
    const f64 fz =
        tiled ? (z * 1000.0) / static_cast<f64>(l.spacing_mm) : (z - l.origin_z) / l.spacing;
    const i64 i = static_cast<i64>(std::floor(fx));
    const i64 j = static_cast<i64>(std::floor(fz));
    if (!tiled && (i < 0 || j < 0 || i + 1 >= l.size || j + 1 >= l.size)) {
      return std::numeric_limits<f32>::quiet_NaN();
    }
    // The tile source's window is indexed in i32 (2.1e9 m at a metre, 5.4e8 m at 25 cm): past
    // that the drawn ground has no cell to ask for.
    constexpr i64 k_i32_max = static_cast<i64>(std::numeric_limits<i32>::max());
    if (i < -k_i32_max || j < -k_i32_max || i >= k_i32_max || j >= k_i32_max) {
      return std::numeric_limits<f32>::quiet_NaN();
    }
    const bool moving = drawn.moving && ground->moves();
    if (cell.i != i || cell.j != j || cell.moving != moving ||
        (moving && (cell.time_a != drawn.time_a || cell.time_b != drawn.time_b))) {
      const Query query{timing};
      const i32 ci = static_cast<i32>(i);
      const i32 cj = static_cast<i32>(j);
      if (tiled) {
        const f64 still = scene->terrain.time_s;
        const std::span<f32> a(cell.a, 4);
        const std::span<f32> b(cell.b, 4);
        if (!drawn.tiles->heights(moving ? drawn.time_a : still, l.spacing_mm, ci, cj, 2, 2, a) ||
            !drawn.tiles->heights(moving ? drawn.time_b : still, l.spacing_mm, ci, cj, 2, 2, b)) {
          return std::numeric_limits<f32>::quiet_NaN();
        }
      } else if (moving) {
        (void)ground->evaluate(drawn.time_a, l, ci, cj, 2, 2, 0, 1, std::span<f32>(cell.a, 4));
        (void)ground->evaluate(drawn.time_b, l, ci, cj, 2, 2, 0, 1, std::span<f32>(cell.b, 4));
      } else {
        ground->grid(l, ci, cj, 2, 2, std::span<f32>(cell.a, 4));
        for (u32 k = 0; k < 4; ++k)
          cell.b[k] = cell.a[k];
      }
      cell.i = i;
      cell.j = j;
      cell.moving = moving;
      cell.time_a = drawn.time_a;
      cell.time_b = drawn.time_b;
    }
    const f32 t = moving ? static_cast<f32>(drawn.blend) : 0.0f;
    f32 h[4];
    for (u32 k = 0; k < 4; ++k)
      h[k] = cell.a[k] * (1.0f - t) + cell.b[k] * t;
    // h[0] (i, j), h[1] (i + 1, j), h[2] (i, j + 1), h[3] (i + 1, j + 1)
    const f32 u = static_cast<f32>(fx - static_cast<f64>(i));
    const f32 v = static_cast<f32>(fz - static_cast<f64>(j));
    if (u + v <= 1.0f) return h[0] + u * (h[1] - h[0]) + v * (h[2] - h[0]);
    return h[3] + (1.0f - u) * (h[2] - h[3]) + (1.0f - v) * (h[1] - h[3]);
  }

  void note(WorldPos before, WorldPos after, f32 held) {
    ++stats.ticks;
    const DVec3 d = after - before;
    stats.distance_m += std::sqrt(d.x * d.x + d.z * d.z);
    if (d.y > 0.0) stats.climb_m += d.y;
    const f32 drawn_h = drawn_at(after.x, after.z);
    if (drawn_h == drawn_h && held == held) {
      const f32 e = std::fabs(held - drawn_h);
      stats.max_ground_error_m = e > stats.max_ground_error_m ? e : stats.max_ground_error_m;
    }
  }
};

Walker::Walker() : impl_(std::make_unique<Impl>()) {}
Walker::~Walker() = default;

bool Walker::start(const WalkParams& params, u32 tick_hz, const renderer::SceneData& scene,
                   std::string* error, const scene_gen::TileSource* tiles) {
  (void)error;
  (void)tiles;  // the collision's, where the build has it
  Impl& w = *impl_;
  w.params = params;
  w.tick_hz = tick_hz > 0 ? tick_hz : 240u;
  w.dt = 1.0f / static_cast<f32>(w.tick_hz);
  w.scene = &scene;
  if (!scene.terrain.enabled) {
    w.collision = "none";
    w.why = "the scene has no terrain to walk on";
    return true;
  }
  w.terrain = std::make_unique<renderer::TerrainSampler>(scene.terrain);
  if (!w.terrain->ok()) {
    w.collision = "none";
    w.why = w.terrain->error();
    w.terrain.reset();
    return true;
  }
  w.ground = &w.terrain->provider();
  w.drawn.lattice = renderer::terrain_scene_lattice(scene.terrain);
  w.stats.hash = hash_combine(k_hash_seed, k_walk_version);
  w.collision = "ground-follow";
#if ENGINE_VIEW_WALK_PHYSICS
  physics::WorldOptions options;
  options.max_bodies = 4096;
  options.max_body_pairs = 16384;
  options.max_contact_constraints = 8192;
  const f32 tile = scene.world.enabled ? scene.world.tile_size : 32.0f;
  scene_collision::Config config;
  config.tile_size = tile;
  config.spacing_m = params.spacing_m;
  config.mesh_error_m = params.mesh_error_m;
  config.ground_error_m = params.ground_error_m;
  config.max_refreshes = params.max_refreshes;
  // Built ahead a tile past the ring (`walk.collision.prefetch_tiles`): not in the walk's header,
  // since it changes no body and no step.
  const f32 ahead = scene_collision::prefetch_tunable();
  config.prefetch_radius_tiles = ahead > 0.0f ? params.radius_tiles + ahead : 0.0f;
  if (ahead > 0.0f) {
    jobs::JobSystemConfig workers;
    workers.performance_workers = 1;
    workers.pin_threads = false;
    w.jobs = std::make_unique<jobs::JobSystem>(workers);
  }
  world::RingParams ring;
  ring.tile_size = tile;
  ring.ring_count = 1;
  for (f32& r : ring.radius)
    r = 0.0f;
  ring.radius[0] = params.radius_tiles;
  ring.max_activations = params.max_tiles;
  ring.max_deactivations = 2 * params.max_tiles;
  std::string why;
  const char* ring_error = nullptr;
  if (w.world.init(options) != physics::Status::Ok) {
    why = "the physics world could not be made";
  } else if (!w.ring.configure(ring, &ring_error)) {
    why = std::string("the collision ring: ") + ring_error;
  } else if (!w.consumer.create(w.world, scene, w.ground, config, &why, tiles, w.jobs.get())) {
    // `why` says what
  } else {
    w.ring.add_consumer(w.consumer.consumer());
    w.physical = true;
    w.collision = "physics";
    w.why.clear();
    w.stats.hash = hash_combine(w.stats.hash, 1);
    ENGINE_LOG_INFO(log_walk, "walker", log::field("collision", "physics"),
                    log::field("tile", tile), log::field("spacing_m", params.spacing_m),
                    log::field("radius_tiles", params.radius_tiles));
    return true;
  }
  w.why = "the scene's collision could not be made (" + why + "): the walker follows the ground";
  ENGINE_LOG_WARN(log_walk, "the walker follows the ground", log::field("why", why));
#else
  w.why =
      "this build has no physics (the physics or scene_collision capability is off): the walker "
      "follows the ground and walks through what stands on it";
#endif
  return true;
}

bool Walker::available() const noexcept { return impl_->ground != nullptr; }
const char* Walker::collision() const noexcept { return impl_->collision; }
const std::string& Walker::why() const noexcept { return impl_->why; }
const WalkParams& Walker::params() const noexcept { return impl_->params; }
const WalkStats& Walker::stats() const noexcept { return impl_->stats; }

WalkTiming Walker::timing() const noexcept {
  WalkTiming out = impl_->timing;
#if ENGINE_VIEW_WALK_PHYSICS
  if (impl_->physical) {
    const scene_collision::Stats& c = impl_->consumer.stats();
    out.activations = c.activated;
    out.activate_ns = c.activate_ns;
    out.max_activate_ns = c.max_activate_ns;
    out.ground_sample_ns = c.ground_sample_ns;
    out.placement_ns = c.placement_ns;
    out.body_ns = c.body_ns;
    out.prefetched = c.prefetched;
    out.prefetch_waits = c.prefetch_waits;
    out.prefetch_wait_ns = c.prefetch_wait_ns;
    out.prefetch_inline = c.prefetch_inline;
  }
#endif
  return out;
}

void Walker::set_drawn(const DrawnGround& drawn) noexcept {
  const scene_gen::Lattice lattice = impl_->drawn.lattice;
  impl_->drawn = drawn;
  // The caller's may be empty; a tile level's has no edge, so no size.
  if (drawn.tiles == nullptr && drawn.lattice.size < 2) impl_->drawn.lattice = lattice;
}

WorldPos Walker::eye() const noexcept {
  return impl_->feet + DVec3{0.0, static_cast<f64>(impl_->params.eye_height), 0.0};
}

WorldPos Walker::feet() const noexcept { return impl_->feet; }

// The ground-follow's own `ground_at`, so a game's answer is the height the walker would stand on
// there. Its one-point cache is a cache: asked elsewhere, the walker's next look recomputes the
// same numbers.
bool Walker::ground_height(f64 x, f64 z, f64& out) {
  Impl& w = *impl_;
  if (w.ground == nullptr) return false;
  out = static_cast<f64>(w.ground_at(x, z));
  return true;
}

bool Walker::on_ground() const noexcept {
  const Impl& w = *impl_;
  if (w.ground == nullptr || !w.placed) return false;
#if ENGINE_VIEW_WALK_PHYSICS
  if (w.physical) return w.body.state().ground == physics::Ground::OnGround;
#endif
  return !w.airborne;
}

bool Walker::ground_ready() const noexcept {
  const Impl& w = *impl_;
  if (w.ground == nullptr || !w.placed) return false;
#if ENGINE_VIEW_WALK_PHYSICS
  if (w.physical) {
    f32 height = 0.0f;
    return w.consumer.ground_height(w.feet, height);
  }
#endif
  return true;
}

void Walker::set_speed_scale(f32 walk, f32 sprint) noexcept {
  const auto held = [](f32 s) { return s < 0.0f ? 0.0f : s > 4.0f ? 4.0f : s; };
  impl_->speed_scale = held(walk);
  impl_->sprint_scale = held(sprint);
}

f32 Walker::speed_scale() const noexcept { return impl_->speed_scale; }
f32 Walker::sprint_scale() const noexcept { return impl_->sprint_scale; }

void Walker::set_water(DepthFn depth, const void* context) noexcept {
  impl_->depth = depth;
  impl_->depth_context = context;
}

f32 wade_scale(f32 depth) noexcept {
  if (depth <= k_wade_ankle_m) return 1.0f;
  if (depth <= k_wade_waist_m)
    return 1.0f - 0.5f * (depth - k_wade_ankle_m) / (k_wade_waist_m - k_wade_ankle_m);
  if (depth <= k_wade_chest_m)
    return 0.5f - 0.25f * (depth - k_wade_waist_m) / (k_wade_chest_m - k_wade_waist_m);
  return 0.25f;
}

WorldPos Walker::teleport(WorldPos at) {
  if (impl_->ground != nullptr) ++impl_->stats.teleports;
  return drop(at);
}

void Walker::look_ahead(WorldPos camera) {
#if ENGINE_VIEW_WALK_PHYSICS
  Impl& w = *impl_;
  if (w.ground == nullptr || !w.physical) return;
  // The drawn ground's time first, as before the ring's update: under a time-lapse nothing is
  // built ahead, and the consumer must know.
  w.consumer.set_ground_time(w.ground_time());
  w.consumer.prefetch(camera);
#else
  (void)camera;
#endif
}

WorldPos Walker::drop(WorldPos camera) {
  Impl& w = *impl_;
  if (w.ground == nullptr) return camera;
  ++w.stats.drops;
#if ENGINE_VIEW_WALK_PHYSICS
  if (w.physical) {
    // Every tile round the camera, and its ground as drawn, before anything is looked for.
    w.move_ring(camera, true);
    while (w.consumer.refresh(camera, w.params.radius) > 0) {
    }
    // The highest surface below the camera; a camera under the ground (a flight below the sand)
    // comes up to the surface above it; nothing at all, the ground's own height.
    const physics::LayerMask statics = physics::LayerMask::of(physics::Layer::Static);
    physics::RayHit hit;
    WorldPos feet{camera.x, 0.0, camera.z};
    if (w.world.cast_ray(camera + DVec3{0.0, 0.01, 0.0}, Vec3{0.0f, -20000.0f, 0.0f}, hit,
                         statics) ||
        w.world.cast_ray(camera + DVec3{0.0, 20000.0, 0.0}, Vec3{0.0f, -40000.0f, 0.0f}, hit,
                         statics)) {
      feet.y = hit.position.y;
    } else {
      feet.y = static_cast<f64>(w.ground_at(camera.x, camera.z));
    }
    feet.y = feet.y + 0.02;
    if (!w.body.valid()) {
      if (w.body.create(w.world, w.character_config(), feet) != physics::Status::Ok) {
        w.physical = false;
        w.collision = "ground-follow";
        w.why =
            "the walker's capsule could not be made from the header's numbers: the walker "
            "follows the ground";
      }
    } else {
      (void)w.body.teleport(feet);
    }
    if (w.physical) {
      w.feet = w.body.feet();
      w.placed = true;
      w.stats.hash = hash_combine(w.stats.hash, w.body.hash());
      return eye();
    }
  }
#endif
  w.feet = WorldPos{camera.x, static_cast<f64>(w.ground_at(camera.x, camera.z)), camera.z};
  w.vy = 0.0f;
  w.airborne = false;
  w.placed = true;
  w.stats.hash = hash_combine(hash_combine(w.stats.hash, bits64(w.feet.x)), bits64(w.feet.z));
  return eye();
}

WorldPos Walker::step(const WalkInput& input) {
  Impl& w = *impl_;
  if (w.ground == nullptr || !w.placed) return eye();
  struct Step {
    WalkTiming& into;
    i64 start = time::monotonic_ns();
    ~Step() {
      const i64 took = time::monotonic_ns() - start;
      ++into.steps;
      into.step_ns += took;
      into.max_step_ns = std::max(into.max_step_ns, took);
      into.recent_max_step_ns = std::max(into.recent_max_step_ns, took);
    }
  } const timed{w.timing};
  const Vec3 direction = heading_direction(input);
  const WorldPos before = w.feet;
  // Wading (walk.h): the depth at the feet before the step scales it, and at the chest a step that
  // leads deeper is not taken. 1 with no water, which leaves every float below as it was.
  const f32 wade = w.wade(before, direction);
#if ENGINE_VIEW_WALK_PHYSICS
  if (w.physical) {
    // The ground round the feet as this frame draws it, and the walker with it: carried by what a
    // refresh changed under it, never left under it (scene_collision.h, `follow`).
    const i64 ring_start = time::monotonic_ns();
    w.move_ring(before, false);
    (void)w.consumer.follow(w.body);
    w.timing.ring_ns += time::monotonic_ns() - ring_start;
    // What the step did, from where the sand left it: being carried up a rising dune is not
    // climbing it.
    const WorldPos from = w.body.feet();
    physics::CharacterInput in;
    in.move = direction;
    in.sprint = input.sprint;
    in.jump = input.jump;
    // A game's scale (walk.h): the character's speed times it; 1, the same floats.
    in.speed_scale = (input.sprint ? w.sprint_scale : w.speed_scale) * wade;
    (void)w.body.step(in);
    w.feet = w.body.feet();
    f32 held = std::numeric_limits<f32>::quiet_NaN();
    if (w.body.state().ground == physics::Ground::OnGround) {
      f32 h = 0.0f;
      if (w.consumer.ground_height(w.feet, h)) held = h;
    }
    w.note(from, w.feet, held);
    w.stats.hash = w.body.hash();
    return eye();
  }
#endif
  // Following the ground: along the heading at the speed, the feet on the ground's height, and a
  // jump an arc of the header's gravity back down onto it. A tick's step is float32, as it always
  // was, and is added to the feet in f64 (ADR-0053).
  // A game's scale on the speed (walk.h): at 1 the product is the header's number to the bit.
  const f32 speed =
      (input.sprint ? w.params.sprint * w.sprint_scale : w.params.speed * w.speed_scale) * wade;
  w.feet.x = w.feet.x + static_cast<f64>(direction.x * speed * w.dt);
  w.feet.z = w.feet.z + static_cast<f64>(direction.z * speed * w.dt);
  const f32 ground = w.ground_at(w.feet.x, w.feet.z);
  if (!w.airborne && input.jump) {
    w.airborne = true;
    w.vy = w.params.jump_speed;
  }
  if (w.airborne) {
    w.vy = w.vy - w.params.gravity * w.dt;
    w.feet.y = w.feet.y + static_cast<f64>(w.vy * w.dt);
    if (w.feet.y <= static_cast<f64>(ground) && w.vy <= 0.0f) {
      w.feet.y = static_cast<f64>(ground);
      w.vy = 0.0f;
      w.airborne = false;
    }
  } else {
    w.feet.y = static_cast<f64>(ground);
  }
  w.note(before, w.feet, w.airborne ? std::numeric_limits<f32>::quiet_NaN() : ground);
  u64 h = hash_combine(w.stats.hash, bits64(w.feet.x));
  h = hash_combine(h, bits64(w.feet.y));
  h = hash_combine(h, bits64(w.feet.z));
  w.stats.hash = hash_combine(h, bits(w.vy));
  return eye();
}

JsonValue Walker::summary_json() const {
  const Impl& w = *impl_;
  JsonValue out = JsonValue::object();
  out.set("collision", w.collision);
  out.set("why", w.why);
  out.set("version", static_cast<u64>(k_walk_version));
  out.set("params", walk_params_to_json(w.params));
  out.set("ticks", w.stats.ticks);
  out.set("distance_m", w.stats.distance_m);
  out.set("climb_m", w.stats.climb_m);
  out.set("drops", static_cast<u64>(w.stats.drops));
  out.set("teleports", static_cast<u64>(w.stats.teleports));
  out.set("speed_scale", JsonValue(w.speed_scale));
  out.set("sprint_scale", JsonValue(w.sprint_scale));
  out.set("max_ground_error_m", JsonValue(w.stats.max_ground_error_m));
  out.set("hash", hex16(w.stats.hash));
  // Wading (walk.h), only where there is water to wade in: a summary of a scene without any is the
  // one it always was.
  if (w.depth != nullptr) {
    JsonValue wading = JsonValue::object();
    wading.set("max_depth_m", JsonValue(w.stats.max_depth_m));
    wading.set("ticks", w.stats.wading_ticks);
    wading.set("stopped_ticks", w.stats.stopped_ticks);
    out.set("wading", std::move(wading));
  }
  JsonValue bodies = JsonValue::object();
  JsonValue ground = JsonValue::object();
  JsonValue ms = JsonValue::object();
  u64 live = 0;
#if ENGINE_VIEW_WALK_PHYSICS
  if (w.physical) {
    const scene_collision::Stats& s = w.consumer.stats();
    live = s.bodies;
    bodies.set("max", static_cast<u64>(s.max_bodies));
    bodies.set("created", s.bodies_created);
    bodies.set("destroyed", s.bodies_destroyed);
    bodies.set("tiles", static_cast<u64>(s.tiles));
    bodies.set("activated", s.activated);
    bodies.set("deactivated", s.deactivated);
    bodies.set("placements", static_cast<u64>(s.placements));
    bodies.set("max_placements", static_cast<u64>(s.max_placements));
    bodies.set("proxies", static_cast<u64>(s.proxies));
    bodies.set("proxy_triangles", static_cast<u64>(s.proxy_triangles));
    bodies.set("streamed", w.consumer.streamed());
    bodies.set("ground_bytes", s.ground_bytes);
    bodies.set("compound_bytes", s.compound_bytes);
    bodies.set("proxy_bytes", s.proxy_bytes);
    ground.set("samples", s.ground_samples);
    ground.set("refreshes", s.refreshes);
    ground.set("field_evaluations", s.field_evaluations);
    ground.set("max_stale_m", s.max_stale_m);
    ground.set("max_kept_m", s.max_kept_m);
    ground.set("carries", s.carries);
    ground.set("max_carry_m", s.max_carry_m);
    ground.set("lifts", s.lifts);
    ground.set("max_lift_m", s.max_lift_m);
    ms.set("activate_total", static_cast<f64>(s.activate_ns) / 1.0e6);
    ms.set("activate_max", static_cast<f64>(s.max_activate_ns) / 1.0e6);
    ms.set("deactivate_total", static_cast<f64>(s.deactivate_ns) / 1.0e6);
    ms.set("refresh_total", static_cast<f64>(s.refresh_ns) / 1.0e6);
    ms.set("refresh_max", static_cast<f64>(s.max_refresh_ns) / 1.0e6);
    ms.set("proxy_total", static_cast<f64>(s.proxy_ns) / 1.0e6);
    ms.set("ground_sample_total", static_cast<f64>(s.ground_sample_ns) / 1.0e6);
    ms.set("ground_sample_max", static_cast<f64>(s.max_ground_sample_ns) / 1.0e6);
    ms.set("placement_total", static_cast<f64>(s.placement_ns) / 1.0e6);
    ms.set("body_total", static_cast<f64>(s.body_ns) / 1.0e6);
    ms.set("prefetch_wait_total", static_cast<f64>(s.prefetch_wait_ns) / 1.0e6);
    ms.set("prefetch_wait_max", static_cast<f64>(s.max_prefetch_wait_ns) / 1.0e6);
    // The worker's share of the builds ahead, off every tick (scene_collision.md, "The placements
    // built ahead").
    ms.set("ahead_ground_total", static_cast<f64>(s.ahead_ground_ns) / 1.0e6);
    ms.set("ahead_placement_total", static_cast<f64>(s.ahead_placement_ns) / 1.0e6);
    ground.set("placements_ahead", s.placements_ahead);
    ground.set("prefetch_submitted", s.prefetch_submitted);
    ground.set("prefetched", s.prefetched);
    ground.set("prefetch_waits", s.prefetch_waits);
    ground.set("prefetch_inline", s.prefetch_inline);
    ground.set("prefetch_dropped", s.prefetch_dropped);
  }
#endif
  bodies.set("live", live);
  out.set("bodies", std::move(bodies));
  out.set("ground", std::move(ground));
  out.set("ms", std::move(ms));
  return out;
}

void set_frame_walker(const Walker& walker, bool walking, renderer::FrameFlora& out) noexcept {
  out.walker = walking && walker.available();
  if (!out.walker) return;
  out.walker_feet = walker.feet();
  out.walker_radius = walker.params().radius;
  out.walker_height = walker.params().height;
}

const scene_gen::GroundProvider* Walker::ground() const noexcept { return impl_->ground; }

void Walker::take_max_ns(i64& step, i64& query) noexcept {
  step = impl_->timing.recent_max_step_ns;
  query = impl_->timing.recent_max_query_ns;
  impl_->timing.recent_max_step_ns = 0;
  impl_->timing.recent_max_query_ns = 0;
}

namespace {

f64 ms_of(i64 ns) noexcept { return static_cast<f64>(ns) / 1.0e6; }
f64 ms_of(u64 ns) noexcept { return static_cast<f64>(ns) / 1.0e6; }
u32 count_of(u64 n) noexcept { return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(n); }

}  // namespace

void WalkProbe::start(Walker& walker, const scene_gen::TileSource* drawn) noexcept {
  walker_ = &walker;
  started_ns_ = time::monotonic_ns();
  sets_[0] = sets_[1] = nullptr;
#if ENGINE_VIEW_TILESET
  // The renderer's set, which the terrain's workers read, and the walker's own provider's when it
  // is another; the same set once.
  if (drawn != nullptr) sets_[0] = tileset::tile_set_of(*drawn);
  if (walker.ground() != nullptr) {
    const tileset::TileSet* own = tileset::tile_set_of(*walker.ground());
    if (own != sets_[0]) sets_[1] = own;
  }
#else
  (void)drawn;
#endif
  timing_ = walker.timing();
  thread_ = thread_reader();
  all_ = all_readers();
}

WalkProbe::Reader WalkProbe::thread_reader() const noexcept {
  Reader out;
#if ENGINE_VIEW_TILESET
  const tileset::TileSet::ReaderStats t = tileset::TileSet::thread_reader_stats(true);
  out = Reader{t.locks,    t.lock_wait_ns, t.max_lock_wait_ns, t.lock_hold_ns, t.hits, t.misses,
               t.io_bytes, t.io_ns,        t.load_waits};
#endif
  return out;
}

WalkProbe::Reader WalkProbe::all_readers() const noexcept {
  Reader out;
#if ENGINE_VIEW_TILESET
  for (const void* set : sets_) {
    if (set == nullptr) continue;
    const tileset::TileSet::ReaderStats t =
        static_cast<const tileset::TileSet*>(set)->reader_stats();
    out.locks += t.locks;
    out.wait_ns += t.lock_wait_ns;
    out.hold_ns += t.lock_hold_ns;
    out.hits += t.hits;
    out.misses += t.misses;
    out.io_bytes += t.io_bytes;
    out.io_ns += t.io_ns;
    out.load_waits += t.load_waits;
  }
#endif
  return out;
}

void WalkProbe::begin(u32 terrain_stage) noexcept {
  if (walker_ == nullptr) return;
  stage_ = terrain_stage;
  timing_ = walker_->timing();
  i64 step = 0;
  i64 query = 0;
  walker_->take_max_ns(step, query);
  thread_ = thread_reader();
  all_ = all_readers();
  ticks_ns_ = time::monotonic_ns();
}

scene::FrameWalk WalkProbe::end() noexcept {
  scene::FrameWalk out;
  if (walker_ == nullptr) return out;
  const i64 now = time::monotonic_ns();
  const WalkTiming t = walker_->timing();
  i64 max_step = 0;
  i64 max_query = 0;
  walker_->take_max_ns(max_step, max_query);
  const Reader thread = thread_reader();
  const Reader all = all_readers();
  out.wall_s = static_cast<f64>(now - started_ns_) / 1.0e9;
  out.ticks_ms = ms_of(now - ticks_ns_);
  out.steps = count_of(t.steps - timing_.steps);
  out.max_step_ms = ms_of(max_step);
  out.ground_queries = count_of(t.ground_queries - timing_.ground_queries);
  out.ground_query_ms = ms_of(t.ground_query_ns - timing_.ground_query_ns);
  out.max_ground_query_ms = ms_of(max_query);
  out.water_queries = count_of(t.water_queries - timing_.water_queries);
  out.water_ms = ms_of(t.water_ns - timing_.water_ns);
  out.ring_ms = ms_of(t.ring_ns - timing_.ring_ns);
  out.activations = count_of(t.activations - timing_.activations);
  out.activate_ms = ms_of(t.activate_ns - timing_.activate_ns);
  out.activate_ground_ms = ms_of(t.ground_sample_ns - timing_.ground_sample_ns);
  out.activate_placements_ms = ms_of(t.placement_ns - timing_.placement_ns);
  out.activate_bodies_ms = ms_of(t.body_ns - timing_.body_ns);
  out.prefetched = count_of(t.prefetched - timing_.prefetched);
  out.prefetch_waits = count_of(t.prefetch_waits - timing_.prefetch_waits);
  out.prefetch_wait_ms = ms_of(t.prefetch_wait_ns - timing_.prefetch_wait_ns);
  out.prefetch_inline = count_of(t.prefetch_inline - timing_.prefetch_inline);
  out.reader_locks = count_of(thread.locks - thread_.locks);
  out.reader_wait_ms = ms_of(thread.wait_ns - thread_.wait_ns);
  out.reader_max_wait_ms = ms_of(thread.max_wait_ns);
  out.reader_hold_ms = ms_of(thread.hold_ns - thread_.hold_ns);
  out.reader_misses = count_of(thread.misses - thread_.misses);
  out.all_reader_locks = all.locks - all_.locks;
  out.all_reader_wait_ms = ms_of(all.wait_ns - all_.wait_ns);
  out.all_reader_hold_ms = ms_of(all.hold_ns - all_.hold_ns);
  out.all_reader_hits = all.hits - all_.hits;
  out.all_reader_misses = count_of(all.misses - all_.misses);
  out.all_reader_io_bytes = all.io_bytes - all_.io_bytes;
  out.all_reader_io_ms = ms_of(all.io_ns - all_.io_ns);
  out.all_reader_load_waits = count_of(all.load_waits - all_.load_waits);
  out.terrain_stage = stage_;
  timing_ = t;
  thread_ = thread;
  all_ = all;
  return out;
}

}  // namespace engine::view

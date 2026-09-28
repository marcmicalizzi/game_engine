// The walk mode (walk.h; docs/subsystems/apps.md, "Walking").
#include "walk.h"

#include "fly_camera.h"

#include <core/hash/hash.h>
#include <core/log/log.h>
#include <core/time/time.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>

#if ENGINE_VIEW_WALK_PHYSICS
#include <domain/physics/character.h>
#include <domain/physics/physics.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>
#endif

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
  if (version != k_walk_version) {
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
  Vec3 feet{};
  bool placed = false;

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
  // The ring and the ground's refresh round (x, z), before a step or a drop: at a fixed tick and
  // from the walker's own position, so a replay makes and lets go of the same bodies at the same
  // ticks (walk.h, "Determinism").
  void follow(Vec3 at, bool unlimited) {
    observers.clear();  // kept, so a tick allocates nothing
    observers.add(at, 1.0f);
    ring.update(observers, ring_tick++, unlimited);
    consumer.set_ground_time(ground_time());
    if (unlimited) {
      while (consumer.refresh(at.x, at.z) > 0) {
      }
    } else {
      (void)consumer.refresh(at.x, at.z);
    }
  }
#endif

  // The provider's height where the walker follows the ground: its own time on still ground, the
  // drawn pair blended as drawn on moving ground.
  f32 ground_at(f32 x, f32 z) {
    if (!drawn.moving || !ground->moves()) return ground->height(x, z);
    const i64 i = static_cast<i64>(std::llround(static_cast<f64>(x) * 1000.0));
    const i64 j = static_cast<i64>(std::llround(static_cast<f64>(z) * 1000.0));
    if (point.i != i || point.j != j || point.time_a != drawn.time_a ||
        point.time_b != drawn.time_b) {
      const scene_gen::Lattice lattice = scene_gen::ring_lattice(1);
      if (!ground->evaluate(drawn.time_a, lattice, static_cast<i32>(i), static_cast<i32>(j), 1, 1,
                            0, 1, std::span<f32>(point.a, 1)) ||
          !ground->evaluate(drawn.time_b, lattice, static_cast<i32>(i), static_cast<i32>(j), 1, 1,
                            0, 1, std::span<f32>(point.b, 1))) {
        return ground->height(x, z);
      }
      point.i = i;
      point.j = j;
      point.time_a = drawn.time_a;
      point.time_b = drawn.time_b;
    }
    const f32 t = static_cast<f32>(drawn.blend);
    return point.a[0] * (1.0f - t) + point.b[0] * t;
  }

  // The ground as drawn at (x, z): the scene's grid cell under it, as the renderer triangulates a
  // quad (the (x + 1, z)-(x, z + 1) diagonal), at the drawn time. NaN off the grid.
  f32 drawn_at(f32 x, f32 z) {
    const scene_gen::Lattice& l = drawn.lattice;
    if (l.size < 2) return std::numeric_limits<f32>::quiet_NaN();
    const f64 fx = (static_cast<f64>(x) - l.origin_x) / l.spacing;
    const f64 fz = (static_cast<f64>(z) - l.origin_z) / l.spacing;
    const i64 i = static_cast<i64>(std::floor(fx));
    const i64 j = static_cast<i64>(std::floor(fz));
    if (i < 0 || j < 0 || i + 1 >= l.size || j + 1 >= l.size) {
      return std::numeric_limits<f32>::quiet_NaN();
    }
    const bool moving = drawn.moving && ground->moves();
    if (cell.i != i || cell.j != j || cell.moving != moving ||
        (moving && (cell.time_a != drawn.time_a || cell.time_b != drawn.time_b))) {
      const i32 ci = static_cast<i32>(i);
      const i32 cj = static_cast<i32>(j);
      if (moving) {
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

  void note(Vec3 before, Vec3 after, f32 held) {
    ++stats.ticks;
    const f32 dx = after.x - before.x;
    const f32 dz = after.z - before.z;
    stats.distance_m += static_cast<f64>(std::sqrt(dx * dx + dz * dz));
    if (after.y > before.y) stats.climb_m += static_cast<f64>(after.y - before.y);
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
                   std::string* error) {
  (void)error;
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
  } else if (!w.consumer.create(w.world, scene, w.ground, config, &why)) {
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

void Walker::set_drawn(const DrawnGround& drawn) noexcept {
  const scene_gen::Lattice lattice = impl_->drawn.lattice;
  impl_->drawn = drawn;
  if (drawn.lattice.size < 2) impl_->drawn.lattice = lattice;  // the caller's may be empty
}

Vec3 Walker::eye() const noexcept {
  return Vec3{impl_->feet.x, impl_->feet.y + impl_->params.eye_height, impl_->feet.z};
}

Vec3 Walker::drop(Vec3 camera) {
  Impl& w = *impl_;
  if (w.ground == nullptr) return camera;
  ++w.stats.drops;
#if ENGINE_VIEW_WALK_PHYSICS
  if (w.physical) {
    // Every tile round the camera, and its ground as drawn, before anything is looked for.
    w.follow(camera, true);
    // The highest surface below the camera; a camera under the ground (a flight below the sand)
    // comes up to the surface above it; nothing at all, the ground's own height.
    const physics::LayerMask statics = physics::LayerMask::of(physics::Layer::Static);
    physics::RayHit hit;
    Vec3 feet{camera.x, 0.0f, camera.z};
    if (w.world.cast_ray(Vec3{camera.x, camera.y + 0.01f, camera.z}, Vec3{0.0f, -20000.0f, 0.0f},
                         hit, statics) ||
        w.world.cast_ray(Vec3{camera.x, camera.y + 20000.0f, camera.z}, Vec3{0.0f, -40000.0f, 0.0f},
                         hit, statics)) {
      feet.y = hit.position.y;
    } else {
      feet.y = w.ground_at(camera.x, camera.z);
    }
    feet.y = feet.y + 0.02f;
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
  w.feet = Vec3{camera.x, w.ground_at(camera.x, camera.z), camera.z};
  w.vy = 0.0f;
  w.airborne = false;
  w.placed = true;
  w.stats.hash = hash_combine(hash_combine(w.stats.hash, bits(w.feet.x)), bits(w.feet.z));
  return eye();
}

Vec3 Walker::step(const WalkInput& input) {
  Impl& w = *impl_;
  if (w.ground == nullptr || !w.placed) return eye();
  const Vec3 direction = heading_direction(input);
  const Vec3 before = w.feet;
#if ENGINE_VIEW_WALK_PHYSICS
  if (w.physical) {
    w.follow(before, false);
    physics::CharacterInput in;
    in.move = direction;
    in.sprint = input.sprint;
    in.jump = input.jump;
    (void)w.body.step(in);
    w.feet = w.body.feet();
    f32 held = std::numeric_limits<f32>::quiet_NaN();
    if (w.body.state().ground == physics::Ground::OnGround) {
      f32 h = 0.0f;
      if (w.consumer.ground_height(w.feet.x, w.feet.z, h)) held = h;
    }
    w.note(before, w.feet, held);
    w.stats.hash = w.body.hash();
    return eye();
  }
#endif
  // Following the ground: along the heading at the speed, the feet on the ground's height, and a
  // jump an arc of the header's gravity back down onto it.
  const f32 speed = input.sprint ? w.params.sprint : w.params.speed;
  w.feet.x = w.feet.x + direction.x * speed * w.dt;
  w.feet.z = w.feet.z + direction.z * speed * w.dt;
  const f32 ground = w.ground_at(w.feet.x, w.feet.z);
  if (!w.airborne && input.jump) {
    w.airborne = true;
    w.vy = w.params.jump_speed;
  }
  if (w.airborne) {
    w.vy = w.vy - w.params.gravity * w.dt;
    w.feet.y = w.feet.y + w.vy * w.dt;
    if (w.feet.y <= ground && w.vy <= 0.0f) {
      w.feet.y = ground;
      w.vy = 0.0f;
      w.airborne = false;
    }
  } else {
    w.feet.y = ground;
  }
  w.note(before, w.feet, w.airborne ? std::numeric_limits<f32>::quiet_NaN() : ground);
  u64 h = hash_combine(w.stats.hash, bits(w.feet.x));
  h = hash_combine(h, bits(w.feet.y));
  h = hash_combine(h, bits(w.feet.z));
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
  out.set("max_ground_error_m", JsonValue(w.stats.max_ground_error_m));
  out.set("hash", hex16(w.stats.hash));
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
    ms.set("activate_total", static_cast<f64>(s.activate_ns) / 1.0e6);
    ms.set("activate_max", static_cast<f64>(s.max_activate_ns) / 1.0e6);
    ms.set("deactivate_total", static_cast<f64>(s.deactivate_ns) / 1.0e6);
    ms.set("refresh_total", static_cast<f64>(s.refresh_ns) / 1.0e6);
    ms.set("refresh_max", static_cast<f64>(s.max_refresh_ns) / 1.0e6);
    ms.set("proxy_total", static_cast<f64>(s.proxy_ns) / 1.0e6);
  }
#endif
  bodies.set("live", live);
  out.set("bodies", std::move(bodies));
  out.set("ground", std::move(ground));
  out.set("ms", std::move(ms));
  return out;
}

}  // namespace engine::view

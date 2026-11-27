// A walk onto a scene of authored meshes (W200; docs/subsystems/scene_collision.md, "A scene of
// authored meshes"): a city block of facades read whole (facade_fixture.h), a still ground, and a
// drop at its middle, with no device and no window. The Toronto proof of concept's landing froze
// its window for 38.9 s building the proxies of the city's imported buildings (its gap G-T37);
// these cases hold, by counts, what the consumer does about it. The full-size block's times are the
// bench rows' (`scene_collision.landing`, `scene_collision.proxy_build.*`), in `msvc-release`.

#include <core/jobs/job_system.h>
#include <core/time/time.h>
#include <domain/physics/character.h>
#include <domain/physics/physics.h>
#include <systems/renderer/scene.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>

#include <chrono>
#include <facade_fixture.h>
#include <string>
#include <thread>

using namespace engine;
using namespace engine::scene_collision;

namespace {

world::RingParams block_ring(u32 budget) {
  world::RingParams p;
  p.tile_size = 32.0f;
  p.ring_count = 1;
  for (f32& r : p.radius)
    r = 0.0f;
  p.radius[0] = 1.5f;
  p.max_activations = budget;
  p.max_deactivations = 2 * budget;
  return p;
}

physics::WorldOptions block_physics() {
  physics::WorldOptions o;
  o.max_bodies = 1024;
  o.max_body_pairs = 4096;
  o.max_contact_constraints = 4096;
  return o;
}

f64 ms(i64 ns) { return static_cast<f64>(ns) / 1.0e6; }

// The walker's host's config with the builds ahead, as engine-view's walk sets it (walk.cpp): four
// workers' slots, the ring's radius, a tile past it, a lead, and the proxies built ahead.
Config ahead_config() {
  Config c;
  c.prefetch_radius_tiles = 2.5f;
  c.ring_radius_tiles = 1.5f;
  c.lead_s = 4.0f;
  c.prefetch_slots = 8;
  c.proxy_ahead_m = 256.0f;
  return c;
}

jobs::JobSystemConfig four_workers() {
  jobs::JobSystemConfig c;
  c.performance_workers = 4;
  c.pin_threads = false;
  return c;
}

// `prefetch` at a hover over (x, z) until every proxy handed over has been built, a minute at most.
void hover(SceneCollision& collision, WorldPos at) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (std::chrono::steady_clock::now() < until) {
    collision.prefetch(at);
    if (collision.proxies_ahead_pending() == 0) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

}  // namespace

// **The block's landing, counted** (W200). Eight facades, each placed eight times at a scale of its
// own, so 64 proxies none of which is shared; a drop at the street in the middle fills the ring of
// nine tiles at once with no job system, the tick building every proxy itself, which was the
// landing's worst case. The cut keeps every triangle (the reveals are 0.1 m deep and more), and the
// stats split the proxies' time into the cut and weld and the backend's tree (printed, asserted by
// nothing: the times are the bench rows').
TEST_CASE("scene_collision: a drop onto a block of authored facades builds every proxy it holds") {
  renderer::SceneData scene;
  std::string error;
  REQUIRE_MESSAGE(facade_fixture::facade_scene(8, 8, true, scene, error), error);
  u64 triangles = 0;  // the facades' own, every placement's
  for (u32 k = 0; k < 8; ++k)
    triangles +=
        8 * (facade_fixture::facade_mesh(facade_fixture::facade_for(k, true)).indices.size() / 3);
  physics::World physics;
  REQUIRE(physics.init(block_physics()) == physics::Status::Ok);
  SceneCollision collision;
  REQUIRE_MESSAGE(collision.create(physics, scene, nullptr, Config{}, &error), error);
  world::World world(block_ring(0));
  world.add_consumer(collision.consumer());
  sim::ObserverSet at;
  at.add(WorldPos{48.0, 0.0, 48.0}, 1.0f);
  world.update(at, 0, true);
  const Stats& s = collision.stats();
  MESSAGE("the drop: " << s.tiles << " tiles, " << s.proxies << " proxies of " << s.proxy_triangles
                       << " triangles of " << triangles << "; activation most "
                       << ms(s.max_activate_ns) << " ms; proxies " << ms(s.proxy_ns)
                       << " ms, the cut " << ms(s.proxy_cut_ns) << " ms, the shape "
                       << ms(s.proxy_shape_ns) << " ms");
  CHECK(s.tiles == 9);
  CHECK(s.proxies == 64);
  CHECK(s.proxy_triangles == triangles);
  CHECK(s.proxy_ahead_submitted == 0);  // no job system: nothing ahead
  CHECK(s.proxy_cut_ns > 0);
  CHECK(s.proxy_shape_ns > 0);
}

// **A landing finds the scene's proxies built ahead** (W200; docs: "A scene of authored
// meshes"). The same block with four workers: a camera hovering over the street for as long as the
// builds take hands every proxy within 256 m to the workers, one job a proxy, and the drop that
// follows builds none of them on the tick — it takes them from the cache. Built ahead or on the
// tick they are the same proxies: the same count and the same triangles.
TEST_CASE(
    "scene_collision: a drop onto a block of authored facades finds its proxies built ahead") {
  renderer::SceneData scene;
  std::string error;
  REQUIRE_MESSAGE(facade_fixture::facade_scene(8, 8, true, scene, error), error);
  physics::World physics;
  REQUIRE(physics.init(block_physics()) == physics::Status::Ok);
  jobs::JobSystem workers(four_workers());
  SceneCollision collision;
  // The proxies alone ahead, no tile's build: so what the drop's tick takes from the cache is the
  // proxies' work and nothing a tile's build ahead did for it.
  Config config = ahead_config();
  config.prefetch_radius_tiles = 0.0f;
  REQUIRE_MESSAGE(collision.create(physics, scene, nullptr, config, &error, nullptr, &workers),
                  error);
  const WorldPos middle{48.0, 0.0, 48.0};
  hover(collision, middle);
  world::World world(block_ring(0));
  world.add_consumer(collision.consumer());
  sim::ObserverSet at;
  at.add(middle, 1.0f);
  REQUIRE(collision.proxies_ahead_pending() == 0);
  world.update(at, 0, true);
  const Stats& s = collision.stats();
  MESSAGE("built ahead " << s.proxy_ahead_built << " of " << s.proxy_ahead_submitted
                         << " handed over; the drop: " << s.tiles << " tiles, " << s.proxies
                         << " proxies of " << s.proxy_triangles << " triangles; activation most "
                         << ms(s.max_activate_ns) << " ms; " << s.proxy_ahead_waits << " waits, "
                         << s.proxy_ahead_claimed << " taken from a job");
  CHECK(s.proxy_ahead_submitted == 64);
  CHECK(s.proxy_ahead_built == 64);
  CHECK(s.proxy_ahead_claimed == 0);
  CHECK(s.proxy_ahead_waits == 0);
  CHECK(s.tiles == 9);
  CHECK(s.proxies == 64);
  u64 triangles = 0;
  for (u32 k = 0; k < 8; ++k)
    triangles +=
        8 * (facade_fixture::facade_mesh(facade_fixture::facade_for(k, true)).indices.size() / 3);
  CHECK(s.proxy_triangles == triangles);
}

namespace {

struct BlockWalk {
  Vector<u64> hashes;  // the character's state after every step
  Stats stats;
  WorldPos feet{};  // where it ended
};

// A walker's host over the block, as engine-view's: dropped in the street at (42.3, 47), the ring
// filled at once, then a step a tick with `prefetch` and the ring's update before it, walking south
// into the fourth row's facade (its sills, reveals and panes) and held there. With `ahead`, four
// workers and every builds-ahead setting a walker has, the proxies built ahead from the first tick;
// without, no job system, every proxy built on the tick.
BlockWalk walk_block(bool ahead) {
  renderer::SceneData scene;
  std::string error;
  REQUIRE_MESSAGE(facade_fixture::facade_scene(8, 8, true, scene, error), error);
  physics::World physics;
  REQUIRE(physics.init(block_physics()) == physics::Status::Ok);
  // The still ground: the street's floor as a box under the block, as the read-whole case's is.
  physics::ShapeId floor_shape;
  REQUIRE(physics.create_box(Vec3{100.0f, 0.5f, 100.0f}, floor_shape) == physics::Status::Ok);
  physics::BodyDesc floor;
  floor.shape = floor_shape;
  floor.transform.position = WorldPos{48.0, -0.5, 48.0};
  floor.motion = physics::MotionType::Static;
  floor.layer = physics::Layer::Static;
  physics::BodyId floor_body;
  REQUIRE(physics.create_body(floor, floor_body) == physics::Status::Ok);
  jobs::JobSystem workers(four_workers());
  SceneCollision collision;
  REQUIRE_MESSAGE(collision.create(physics, scene, nullptr, ahead ? ahead_config() : Config{},
                                   &error, nullptr, ahead ? &workers : nullptr),
                  error);
  world::World world(block_ring(2));
  world.add_consumer(collision.consumer());
  const WorldPos start{42.3, 0.02, 47.0};
  sim::ObserverSet observers;
  collision.prefetch(start);
  observers.add(start, 1.0f);
  world.update(observers, 0, true);
  physics::CharacterBody body;
  physics::CharacterConfig c;
  c.step_hz = 240;
  c.walk_speed = 3.0f;
  REQUIRE(body.create(physics, c, start) == physics::Status::Ok);
  physics::CharacterInput input;
  input.move = Vec3{0.0f, 0.0f, -1.0f};
  BlockWalk out;
  WorldPos last = body.feet();
  for (u64 tick = 1; tick <= 2400; ++tick) {
    const DVec3 moved = body.feet() - last;
    last = body.feet();
    collision.prefetch(body.feet(), Vec3{static_cast<f32>(moved.x * 240.0), 0.0f,
                                         static_cast<f32>(moved.z * 240.0)});
    observers.clear();
    observers.add(body.feet(), 1.0f);
    world.update(observers, tick, false);
    (void)collision.follow(body);
    REQUIRE(body.step(input) == physics::Status::Ok);
    out.hashes.push_back(body.hash());
  }
  out.stats = collision.stats();
  out.feet = body.feet();
  return out;
}

}  // namespace

// **The proxies built ahead change no body** (W200). The same walk over the block with the proxies
// built on four workers from the first tick, and with every proxy built on the tick, has the same
// states, tick for tick: the bodies are made on the tick in the ring's order either way, and a
// proxy is a function of its mesh, scale and error whoever builds it.
TEST_CASE("scene_collision: the block's proxies built ahead change no body") {
  const BlockWalk plain = walk_block(false);
  const BlockWalk ahead = walk_block(true);
  REQUIRE(plain.hashes.size() == ahead.hashes.size());
  u32 differ = 0;
  for (u32 k = 0; k < plain.hashes.size(); ++k)
    differ += plain.hashes[k] == ahead.hashes[k] ? 0u : 1u;
  MESSAGE("2,400 ticks; " << plain.stats.activated
                          << " tiles activated; ahead: " << ahead.stats.proxy_ahead_submitted
                          << " proxies handed over, " << ahead.stats.proxy_ahead_built
                          << " built ahead, " << ahead.stats.proxy_ahead_waits << " waited for, "
                          << ahead.stats.proxy_ahead_claimed << " taken from a job; " << differ
                          << " states differ");
  CHECK(differ == 0);
  CHECK(plain.feet.z == ahead.feet.z);
  CHECK(plain.feet.x == ahead.feet.x);
  CHECK(plain.stats.bodies_created == ahead.stats.bodies_created);
  CHECK(plain.stats.activated == ahead.stats.activated);
  // Ahead, the proxies within 256 m are built whether a held tile names them or not.
  CHECK(ahead.stats.proxies >= plain.stats.proxies);
  CHECK(plain.stats.proxy_ahead_submitted == 0);
  CHECK(ahead.stats.proxy_ahead_submitted > 0);
  // The walk met a facade: walking south from z = 47 it is held at the fourth row's face (z = 42,
  // its sills 0.12 m proud of it) less its radius.
  MESSAGE("the walker ends at (" << plain.feet.x << ", " << plain.feet.z << ")");
  CHECK(plain.feet.z > 42.0);
  CHECK(plain.feet.z < 42.0 + 0.12 + 0.3 + 0.1);
}

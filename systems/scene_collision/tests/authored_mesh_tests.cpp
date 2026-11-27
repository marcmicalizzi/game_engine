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
#include <systems/scene_collision/proxy_source.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <facade_fixture.h>
#include <map>
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

// A job system of one performance worker the test holds busy with a job of its own until
// `release`, so what is handed over stays queued (or, with a small `queue_capacity`, fills every
// queue and is run on the caller): the ways a host's shared, busy job system treats the collision's
// jobs, made deterministic. `release` before the consumer goes: its destructor waits for its jobs.
struct HeldWorker {
  std::atomic<bool> started{false};
  std::atomic<bool> go{false};
  jobs::Counter done;
  jobs::JobSystem system;
  static jobs::JobSystemConfig config(u32 queue_capacity) {
    jobs::JobSystemConfig c;
    c.performance_workers = 1;
    c.efficiency_workers = 1;
    c.pin_threads = false;
    c.queue_capacity = queue_capacity;
    return c;
  }
  static void hold(void* data) noexcept {
    auto* h = static_cast<HeldWorker*>(data);
    h->started.store(true);
    while (!h->go.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  explicit HeldWorker(u32 queue_capacity) : system(config(queue_capacity)) {
    done.add(1);
    system.schedule(jobs::Pool::Performance, jobs::Job{&hold, this, &done});
    while (!started.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  void release() {
    go.store(true);
    done.wait_blocking();
  }
  ~HeldWorker() { release(); }
};

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
  // The jobs ahead let finish where the walk ended (it makes no body: bodies are made by the ring's
  // update), then the counts read.
  if (ahead) hover(collision, body.feet());
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
  // Read fresh (`stats()` copies the proxies' counters in) after the jobs have returned, every
  // proxy handed over is counted as what became of it; the counts of the walk's last activation,
  // which is what the summary read before the mend, are short of it.
  CHECK(ahead.stats.proxy_ahead_submitted == ahead.stats.proxy_ahead_built +
                                                 ahead.stats.proxy_ahead_claimed +
                                                 ahead.stats.proxy_ahead_withdrawn);
}

// **A job system that runs a proxy's job on the tick does not deadlock it** (W200's mend). A
// host's job system whose queues are full runs a job on the thread that schedules it; the
// proxies were handed over under the proxies' lock and the job takes it, so a shared, busy job
// system deadlocked the tick on a `std::mutex` locked twice. A worker held busy and queues of two:
// of sixteen proxies handed over, the queues take a few and the rest run on the tick, which
// returns, having built them.
TEST_CASE("scene_collision: a proxy's job run on the tick by a full job system returns") {
  renderer::SceneData scene;
  std::string error;
  REQUIRE_MESSAGE(facade_fixture::facade_scene(8, 8, true, scene, error), error);
  physics::World physics;
  REQUIRE(physics.init(block_physics()) == physics::Status::Ok);
  HeldWorker held(2);
  {
    SceneCollision collision;
    Config config = ahead_config();
    config.prefetch_radius_tiles = 0.0f;
    config.prefetch_slots = 16;
    REQUIRE_MESSAGE(
        collision.create(physics, scene, nullptr, config, &error, nullptr, &held.system), error);
    collision.prefetch(WorldPos{48.0, 0.0, 48.0});
    const u64 inline_runs = held.system.stats().inline_runs;
    const Stats& s = collision.stats();
    MESSAGE(s.proxy_ahead_submitted << " handed over, " << inline_runs << " run on the tick, "
                                    << s.proxy_ahead_built << " built");
    CHECK(s.proxy_ahead_submitted == 16);
    CHECK(inline_runs > 0);
    CHECK(s.proxy_ahead_built == inline_runs);  // the held worker built none
    held.release();
  }
}

// **A tick takes a queued proxy and builds it itself** (W200). The worker held busy, the eight
// nearest proxies handed over stay queued; the drop's tick that wants them finds no job started on
// them, takes all eight from their jobs and builds them, and waits for none. Released, the jobs
// find their proxies taken and build nothing.
TEST_CASE("scene_collision: a drop takes the proxies no job has started and builds them") {
  renderer::SceneData scene;
  std::string error;
  REQUIRE_MESSAGE(facade_fixture::facade_scene(8, 8, true, scene, error), error);
  physics::World physics;
  REQUIRE(physics.init(block_physics()) == physics::Status::Ok);
  HeldWorker held(4096);
  {
    SceneCollision collision;
    Config config = ahead_config();
    config.prefetch_radius_tiles = 0.0f;
    REQUIRE_MESSAGE(
        collision.create(physics, scene, nullptr, config, &error, nullptr, &held.system), error);
    const WorldPos middle{48.0, 0.0, 48.0};
    collision.prefetch(middle);
    REQUIRE(collision.proxies_ahead_pending() == 8);
    world::World world(block_ring(0));
    world.add_consumer(collision.consumer());
    sim::ObserverSet at;
    at.add(middle, 1.0f);
    world.update(at, 0, true);
    {
      const Stats& s = collision.stats();
      MESSAGE(s.proxy_ahead_claimed << " taken from a job, " << s.proxy_ahead_waits
                                    << " waited for, " << s.proxy_ahead_built << " built ahead, "
                                    << s.proxies << " proxies");
      CHECK(s.proxy_ahead_claimed == 8);
      CHECK(s.proxy_ahead_waits == 0);
      CHECK(s.proxy_ahead_built == 0);
      CHECK(s.proxies == 64);
    }
    held.release();
    hover(collision, middle);
    CHECK(collision.stats().proxy_ahead_built == 0);
    CHECK(collision.stats().proxy_ahead_submitted == 8);
  }
}

// **What a fast flight handed over and left behind is taken back** (W200's mend). Sixteen facades
// 300 m apart in a strip 4.5 km long, the worker held busy, and a flight along it at 1,400 m/s, a
// `prefetch` a 240 Hz tick: each facade is handed over as the flight comes within 256 m of it.
// The workers take the oldest first, so before the mend the first eight stood queued ahead of the
// landing's own and were built after it (and by the destructor); now each is withdrawn unbuilt once
// a ranking finds it out of range. At the strip's far end a drop takes the last facade's proxy
// from its job; released, the worker builds none, since none the landing's ring does not name is
// left queued, and every proxy handed over is accounted for: built, taken, or withdrawn.
TEST_CASE("scene_collision: a fast flight's proxies left behind are withdrawn, not built") {
  renderer::SceneData scene;
  std::string error;
  REQUIRE_MESSAGE(facade_fixture::facade_scene(8, 2, true, scene, error, false, 300.0f, 16), error);
  physics::World physics;
  REQUIRE(physics.init(block_physics()) == physics::Status::Ok);
  HeldWorker held(4096);
  {
    SceneCollision collision;
    Config config = ahead_config();
    config.prefetch_radius_tiles = 0.0f;
    REQUIRE_MESSAGE(
        collision.create(physics, scene, nullptr, config, &error, nullptr, &held.system), error);
    const f64 end = 6.0 + 300.0 * 15.0;
    const f64 step = 1400.0 / 240.0;
    for (f64 x = 6.0; x < end; x += step)
      collision.prefetch(WorldPos{x, 0.0, 6.0}, Vec3{1400.0f, 0.0f, 0.0f});
    const WorldPos landing{end, 0.0, 6.0};
    collision.prefetch(landing);
    world::World world(block_ring(0));
    world.add_consumer(collision.consumer());
    sim::ObserverSet at;
    at.add(landing, 1.0f);
    world.update(at, 0, true);
    held.release();
    hover(collision, landing);
    const Stats& s = collision.stats();
    MESSAGE(s.proxy_ahead_submitted << " handed over, " << s.proxy_ahead_withdrawn << " withdrawn, "
                                    << s.proxy_ahead_claimed << " taken, " << s.proxy_ahead_built
                                    << " built ahead");
    CHECK(s.proxy_ahead_submitted == 16);
    CHECK(s.proxy_ahead_withdrawn == 15);
    CHECK(s.proxy_ahead_claimed == 1);  // the landing's facade, by the drop's tick
    CHECK(s.proxy_ahead_built == 0);
    CHECK(s.proxy_ahead_submitted ==
          s.proxy_ahead_withdrawn + s.proxy_ahead_claimed + s.proxy_ahead_built);
    CHECK(collision.proxies_ahead_pending() == 0);
  }
}

// **Ranked by where the mesh stands, not by its instance's origin** (W200's mend). The block with
// every facade's vertices moved and scaled to its place and its instance at the origin, as an
// import in its tile's coordinates arrives, and the block as placed: a hover over the street with
// the proxies built ahead within 22 m hands over the same twelve facades either way (before the
// mend the moved form's rows all stood at the origin, 68 m away, and none was built).
TEST_CASE("scene_collision: a mesh in its tile's coordinates is ranked where it stands") {
  const WorldPos middle{48.0, 0.0, 48.0};
  u32 within = 0;  // the placed form's facades within 22 m of the middle
  for (u32 n = 0; n < 64; ++n) {
    const f64 dx = 6.0 + 12.0 * (n % 8) - middle.x;
    const f64 dz = 6.0 + 12.0 * (n / 8) - middle.z;
    within += std::sqrt(dx * dx + dz * dz) <= 22.0 ? 1u : 0u;
  }
  u64 built[2] = {0, 0};
  for (u32 baked = 0; baked < 2; ++baked) {
    CAPTURE(baked);
    renderer::SceneData scene;
    std::string error;
    REQUIRE_MESSAGE(facade_fixture::facade_scene(8, 8, true, scene, error, baked != 0), error);
    physics::World physics;
    REQUIRE(physics.init(block_physics()) == physics::Status::Ok);
    jobs::JobSystem workers(four_workers());
    SceneCollision collision;
    Config config = ahead_config();
    config.prefetch_radius_tiles = 0.0f;
    config.proxy_ahead_m = 22.0f;
    REQUIRE_MESSAGE(collision.create(physics, scene, nullptr, config, &error, nullptr, &workers),
                    error);
    hover(collision, middle);
    built[baked] = collision.stats().proxy_ahead_built;
  }
  MESSAGE(within << " facades within 22 m; built ahead " << built[0] << " placed, " << built[1]
                 << " in their tile's coordinates");
  CHECK(within == 12);
  CHECK(built[0] == within);
  CHECK(built[1] == built[0]);
}

// **The weld's hash map makes the proxy the sorted map made** (W200's mend 1): a proxy of each of
// the block's first row of facades, held with `Config::keep_shapes`, against the cut welded here by
// the rule the sorted map kept — the same clusters, the first vertex met for a source index wins —
// in a `std::map` of the test's own, vertex for vertex and index for index, word for word. The
// facades are in their tile's coordinates at scale 1, so the reference's vertices are the copy's.
TEST_CASE("scene_collision: the weld makes the proxy the sorted map made, word for word") {
  renderer::SceneData scene;
  std::string error;
  REQUIRE_MESSAGE(facade_fixture::facade_scene(8, 1, true, scene, error, true), error);
  const Config config = [] {
    Config c;
    c.keep_shapes = true;
    return c;
  }();
  ProxySource source;
  source.build(scene, config.mesh_error_m);
  physics::World physics;
  REQUIRE(physics.init(block_physics()) == physics::Status::Ok);
  SceneCollision collision;
  REQUIRE_MESSAGE(
      collision.create(physics, scene, nullptr, config, &error, nullptr, nullptr, &source), error);
  world::World world(block_ring(0));
  world.add_consumer(collision.consumer());
  sim::ObserverSet at;
  at.add(WorldPos{48.0, 0.0, 6.0}, 1.0f);
  world.update(at, 0, true);
  u32 pieces = 0;
  u64 words = 0;
  u64 differ = 0;
  for (u32 t = 0; t < collision.held_tiles(); ++t) {
    HeldPiece piece;
    for (u32 p = 0; collision.held_piece(t, p, piece); ++p) {
      const ProxySource::Mesh* mesh = source.mesh(piece.mesh);
      REQUIRE(mesh != nullptr);
      const f32 threshold = std::max(config.mesh_error_m, mesh->floor);
      std::map<u32, u32> remap;
      Vector<Vec3> vertices;
      Vector<u32> indices;
      for (const ProxySource::Cluster& cluster : source.clusters(*mesh)) {
        if (!(cluster.own_error <= threshold && threshold < cluster.parent_error)) continue;
        for (u32 k = 0; k < cluster.triangle_count; ++k) {
          const u32 packed = source.triangles()[cluster.first_triangle + k];
          u32 corner[3];
          for (u32 c = 0; c < 3; ++c) {
            const u32 v = cluster.first_point + ((packed >> (8 * c)) & 0xFFu);
            const auto [it, fresh] = remap.try_emplace(source.sources()[v], vertices.size());
            if (fresh) vertices.push_back(source.points()[v]);
            corner[c] = it->second;
          }
          if (corner[0] == corner[1] || corner[1] == corner[2] || corner[0] == corner[2]) continue;
          for (u32 c = 0; c < 3; ++c)
            indices.push_back(corner[c]);
        }
      }
      ++pieces;
      CHECK(piece.vertices.size() == vertices.size());
      CHECK(piece.indices.size() == indices.size());
      if (piece.vertices.size() != vertices.size() || piece.indices.size() != indices.size())
        continue;
      for (u32 v = 0; v < vertices.size(); ++v) {
        const f32 a[3] = {piece.vertices[v].x, piece.vertices[v].y, piece.vertices[v].z};
        const f32 b[3] = {vertices[v].x, vertices[v].y, vertices[v].z};
        for (u32 c = 0; c < 3; ++c)
          differ += std::bit_cast<u32>(a[c]) == std::bit_cast<u32>(b[c]) ? 0u : 1u;
        words += 3;
      }
      for (u32 i = 0; i < indices.size(); ++i)
        differ += piece.indices[i] == indices[i] ? 0u : 1u;
      words += indices.size();
    }
  }
  MESSAGE(pieces << " pieces, " << words << " words compared, " << differ << " differ");
  CHECK(pieces >= 8);
  CHECK(words > 0);
  CHECK(differ == 0);
}

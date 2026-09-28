// The collision ground under a time-lapse, on the committed erg (content/test-scenes/desert-erg)
// and the renderer's own CPU model of the drawn sand (terrain_time.h: the cadence rule timing the
// fields, `terrain_blend_frame` crossing them a frame at a time within the per-frame bound, the
// fields waited for as an offscreen run waits). No device: the ground is the terrain capability's
// dune generator through the registry, linked as a host links it, and the drawn heights are the
// same two fields and blend the pool pass draws, evaluated on the collision's lattice.
// docs/subsystems/scene_collision.md, "The ground moves".
#include <domain/physics/character.h>
#include <domain/physics/physics.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_time.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::scene_collision;

namespace {

struct Erg {
  renderer::SceneDesc desc;
  std::unique_ptr<renderer::TerrainSampler> sampler;
  bool ok = false;
};

void load_erg(Erg& erg) {
  const std::string path =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/desert-erg/scene.json",
                      "content/test-scenes/desert-erg/scene.json");
  if (!test::path_exists(path)) {
    MESSAGE("not in this bundle: " << path);
    return;
  }
  std::string error;
  REQUIRE_MESSAGE(renderer::read_scene_file(path, erg.desc, error), error);
  erg.sampler = std::make_unique<renderer::TerrainSampler>(erg.desc.terrain);
  REQUIRE(erg.sampler->ok());
  REQUIRE(erg.sampler->moves());
  erg.ok = true;
}

// A drawn field cached by its time: the collision lattice's window over the walker's tile.
struct Field {
  f64 time = -1.0;
  Vector<f32> heights;
};

constexpr i32 k_i0 = 96;  // the walker's tile (3, 0) on the 1 m lattice: x 96..129, z 0..33
constexpr i32 k_j0 = 0;
constexpr u32 k_n = 34;

const Vector<f32>& drawn_field(const scene_gen::GroundProvider& ground, f64 time, Field& cache) {
  if (cache.time != time) {
    cache.heights.resize(k_n * k_n);
    REQUIRE(ground.evaluate(time, scene_gen::ring_lattice(1000), k_i0, k_j0, k_n, k_n, 0,
                            scene_gen::window_blocks(k_n, k_n),
                            std::span<f32>(cache.heights.data(), cache.heights.size())));
    cache.time = time;
  }
  return cache.heights;
}

// The largest difference of two fields over the drawn level's own lattice round the walker: what
// the renderer's per-frame bound divides the blend's step by (`TerrainNextField::delta_m`).
f64 pair_delta(const scene_gen::GroundProvider& ground, const renderer::TerrainDesc& terrain, f64 a,
               f64 b) {
  const scene_gen::Lattice lattice = scene_gen::scene_lattice(terrain.extent, terrain.size);
  const f64 extent = static_cast<f64>(terrain.extent);
  const i32 i0 = static_cast<i32>(std::floor((96.0 + extent) / lattice.spacing)) - 16;
  const i32 j0 = static_cast<i32>(std::floor((0.0 + extent) / lattice.spacing)) - 16;
  Vector<f32> fa(64 * 64);
  Vector<f32> fb(64 * 64);
  REQUIRE(ground.evaluate(a, lattice, i0, j0, 64, 64, 0, 1, std::span<f32>(fa.data(), fa.size())));
  REQUIRE(ground.evaluate(b, lattice, i0, j0, 64, 64, 0, 1, std::span<f32>(fb.data(), fb.size())));
  f64 d = 0.0;
  for (u32 k = 0; k < fa.size(); ++k)
    d = std::max(d, static_cast<f64>(std::fabs(fb[k] - fa[k])));
  return d;
}

struct Run {
  u32 frames = 0;
  u32 pairs = 0;          // fields the drawn level took as b
  f32 worst = 0.0f;       // the walker's tile against the drawn heights, after each update
  f32 worst_feet = 0.0f;  // a standing walker's feet against the collision ground
  f32 slope = 0.0f;       // the steepest ground it stood on, rise over run
  f32 frame_move = 0.0f;  // the most the drawn sand moved at a sample of the tile in one frame
  Stats stats;
};

// `frames` frames at `rate` game seconds a real second, 60 a second, the walker standing on the
// interdune floor at the path's `toe` (x = 100), among the waves, the band that moves most.
void run(const Erg& erg, f64 rate, u32 frames, bool walker, Run& out) {
  const scene_gen::GroundProvider& ground = erg.sampler->provider();
  const renderer::TerrainDesc& terrain = erg.desc.terrain;
  renderer::SceneData scene;
  scene.terrain = terrain;
  physics::WorldOptions options;
  options.max_bodies = 1024;
  physics::World physics;
  REQUIRE(physics.init(options) == physics::Status::Ok);
  SceneCollision collision;
  Config config;  // the tunables' defaults: 1 m, 5 cm, two refreshes an update
  std::string error;
  REQUIRE_MESSAGE(collision.create(physics, scene, &ground, config, &error), error);

  // The drawn level: the scene's grid (1.5 m), a quarter of a sample a field.
  const f64 spacing = 2.0 * static_cast<f64>(terrain.extent) / static_cast<f64>(terrain.size - 1);
  const f64 fraction = 0.25;
  renderer::TerrainBlend blend;
  blend.time_a = terrain.time_s;
  blend.surface_s = terrain.time_s;
  renderer::TerrainNextField next;
  auto ask = [&]() {
    const f64 from = blend.has_b ? blend.time_b : blend.time_a;
    next.time_s =
        renderer::terrain_next_time(*erg.sampler, from, spacing, fraction, 60.0, 2592000.0);
    next.delta_m = pair_delta(ground, terrain, from, next.time_s);
    next.ready = true;
  };
  auto ground_time = [&]() {
    GroundTime t;
    t.moving = true;
    t.time_a = blend.time_a;
    t.time_b = blend.has_b ? blend.time_b : blend.time_a;
    t.blend = blend.blend();
    return t;
  };

  const f32 wx = 100.0f;
  const f32 wz = 10.0f;
  collision.set_ground_time(ground_time());
  world::World ring(ring_params_from_tunables(32.0f));
  ring.add_consumer(collision.consumer());
  sim::ObserverSet at;
  at.add(Vec3{wx, 0.0f, wz}, 1.0f);
  ring.update(at, 0, true);
  physics::CharacterBody body;
  if (walker) {
    f32 y = 0.0f;
    REQUIRE(collision.ground_height(wx, wz, y));
    physics::CharacterConfig c;
    c.step_hz = 240;
    REQUIRE(body.create(physics, c, Vec3{wx, y + 0.05f, wz}) == physics::Status::Ok);
  }

  Field fa;
  Field fb;
  Vector<f32> previous;
  f64 game = terrain.time_s;
  for (u32 frame = 0; frame < frames; ++frame) {
    game += rate / 60.0;
    if (!next.ready) ask();
    const renderer::TerrainFrameResult result =
        renderer::terrain_blend_frame(blend, game, fraction * spacing, next);
    out.pairs += result.installed;
    // A frame is four of engine-view's 240 Hz ticks, and a walking host refreshes once a tick.
    collision.set_ground_time(ground_time());
    for (u32 tick = 0; tick < 4; ++tick) {
      collision.refresh(wx, wz);
      if (walker) REQUIRE(body.step(physics::CharacterInput{}) == physics::Status::Ok);
    }
    // The walker's tile, sample by sample, against the drawn heights there.
    const GroundTime t = ground_time();
    const Vector<f32>& a = drawn_field(ground, t.time_a, fa);
    const Vector<f32>& b = drawn_field(ground, t.time_b, fb);
    const f32 w = static_cast<f32>(t.blend);
    previous.resize(32 * 32);
    for (u32 j = 0; j < 32; ++j) {
      for (u32 i = 0; i < 32; ++i) {
        f32 held = 0.0f;
        REQUIRE(collision.ground_height(static_cast<f32>(k_i0 + static_cast<i32>(i)),
                                        static_cast<f32>(k_j0 + static_cast<i32>(j)), held));
        const f32 drawn = a[j * k_n + i] * (1.0f - w) + b[j * k_n + i] * w;
        out.worst = std::max(out.worst, std::fabs(held - drawn));
        // How far the drawn sand moved since the last frame: what a host a frame behind the
        // picture can be off by before its first tick catches up.
        if (frame > 0)
          out.frame_move = std::max(out.frame_move, std::fabs(drawn - previous[j * 32 + i]));
        previous[j * 32 + i] = drawn;
      }
    }
    if (walker) {
      const physics::CharacterState st = body.state();
      f32 held = 0.0f;
      if (st.ground == physics::Ground::OnGround &&
          collision.ground_height(st.position.x, st.position.z, held)) {
        out.worst_feet = std::max(out.worst_feet, std::fabs(st.position.y - held));
        // The slope under it: a capsule's round bottom rests r (1 / cos - 1) above the point
        // under its centre, which on a slip face is centimetres.
        out.slope = std::max(out.slope, std::sqrt(st.ground_normal.x * st.ground_normal.x +
                                                  st.ground_normal.z * st.ground_normal.z) /
                                            st.ground_normal.y);
      }
    }
    ++out.frames;
  }
  out.stats = collision.stats();
}

}  // namespace

TEST_CASE(
    "scene_collision: on the erg, the collision ground follows the drawn sand in time-lapse") {
  Erg erg;
  load_erg(erg);
  if (!erg.ok) return;
  // A game day a real second, ten seconds of it: the waves and barchans are what move.
  Run lapse;
  run(erg, 86400.0, 600, true, lapse);
  MESSAGE("the erg at 86,400: "
          << lapse.frames << " frames, " << lapse.pairs << " fields drawn, "
          << lapse.stats.refreshes << " tile refreshes (" << lapse.stats.field_evaluations
          << " tile fields evaluated), the walker's tile at most " << lapse.worst
          << " m off the drawn sand after an update, the stalest a refresh under "
          << "it found " << lapse.stats.max_stale_m << " m; a standing walker's feet at most "
          << lapse.worst_feet << " m off the collision ground, on slopes up to " << lapse.slope
          << " rise over run; the drawn sand moved up to " << lapse.frame_move << " m in a frame");
  CHECK(lapse.pairs > 5);
  CHECK(lapse.stats.refreshes > 10);
  // The rule's bound, after every update, on every sample of the walker's tile.
  CHECK(lapse.worst <= 0.05f + 1.0e-4f);
  // Before the first tick of a frame catches up, the collision is the last frame's sand: off by the
  // bound and at most one frame's move of the drawn sand (the renderer's per-frame bound, a quarter
  // of its spacing: 0.375 m on the erg's grid).
  CHECK(lapse.stats.max_stale_m <= 0.05 + static_cast<f64>(lapse.frame_move) + 1.0e-4);
  CHECK(lapse.frame_move <= 0.375f);
  // The feet: the capsule's round bottom on the slope (0.3 (1 / cos - 1)), the backend's 2 cm
  // padding, and the ground under it moving by up to that much between two rebuilds — sand that
  // drops away leaves a standing walker to fall after it.
  const f32 cosine = 1.0f / std::sqrt(1.0f + lapse.slope * lapse.slope);
  CHECK(lapse.worst_feet <= 0.3f * (1.0f / cosine - 1.0f) + 0.02f +
                                static_cast<f32>(lapse.stats.max_stale_m) + 1.0e-3f);
}

TEST_CASE("scene_collision: on the erg at the game's own rate, the collision ground is still") {
  Erg erg;
  load_erg(erg);
  if (!erg.ok) return;
  // Ten real minutes at a game second a real second: the drawn sand moves by millimetres, and no
  // tile is rebuilt.
  Run still;
  run(erg, 1.0, 36000, false, still);
  MESSAGE("the erg at 1: " << still.frames << " frames, " << still.pairs << " fields drawn, "
                           << still.stats.refreshes << " tile refreshes, the walker's tile at most "
                           << still.worst << " m off the drawn sand");
  CHECK(still.stats.refreshes == 0);
  CHECK(still.worst <= 0.05f);
}

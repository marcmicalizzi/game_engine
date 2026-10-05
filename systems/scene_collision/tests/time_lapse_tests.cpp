// The collision ground under a time-lapse, on the committed erg (content/test-scenes/desert-erg)
// and the renderer's own CPU model of the drawn sand (terrain_time.h: the cadence rule timing the
// fields, `terrain_blend_frame` crossing them a frame at a time within the per-frame bound, the
// fields waited for as an offscreen run waits). No device: the ground is the terrain capability's
// dune generator through the registry, linked as a host links it, and the drawn heights are the
// same two fields and blend the pool pass draws, evaluated on the collision's lattice.
// docs/subsystems/scene_collision.md, "The ground moves" and "The walker goes with the ground".
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

// The cases below stand round the origin, whose frame is the world's (ADR-0053): a point (x, z) on
// the ground, a point in the world from one in that frame, and back.
WorldPos wp(f32 x, f32 z) { return WorldPos{static_cast<f64>(x), 0.0, static_cast<f64>(z)}; }
WorldPos place(Vec3 local) { return absolute(WorldPos::origin(), local); }
Vec3 local_of(WorldPos p) { return relative(p, WorldPos::origin()); }

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
    REQUIRE(collision.ground_height(wp(wx, wz), y));
    physics::CharacterConfig c;
    c.step_hz = 240;
    REQUIRE(body.create(physics, c, place(Vec3{wx, y + 0.05f, wz})) == physics::Status::Ok);
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
    // A frame is four of engine-view's 240 Hz ticks, and a walking host refreshes once a tick —
    // round the walker, which then goes with the ground, before its step (walk.cpp).
    collision.set_ground_time(ground_time());
    for (u32 tick = 0; tick < 4; ++tick) {
      if (walker) {
        collision.follow(body);
        REQUIRE(body.step(physics::CharacterInput{}) == physics::Status::Ok);
      } else {
        collision.refresh(wp(wx, wz));
      }
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
        REQUIRE(collision.ground_height(wp(static_cast<f32>(k_i0 + static_cast<i32>(i)),
                                           static_cast<f32>(k_j0 + static_cast<i32>(j))),
                                        held));
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
      if (st.ground == physics::Ground::OnGround && collision.ground_height(st.position, held)) {
        out.worst_feet = std::max(out.worst_feet, std::fabs(local_of(st.position).y - held));
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
  // The rule's bound under the walker, after every update, on every sample of the walker's tile: a
  // millimetre (it was `ground_error_m`, 5 cm, until 2026-09-29).
  CHECK(lapse.worst <= k_underfoot_error_m + 1.0e-4f);
  // Before the first tick of a frame catches up, the collision is the last frame's sand: off by the
  // bound and at most one frame's move of the drawn sand (the renderer's per-frame bound, a quarter
  // of its spacing: 0.375 m on the erg's grid).
  CHECK(lapse.stats.max_stale_m <=
        static_cast<f64>(k_underfoot_error_m) + static_cast<f64>(lapse.frame_move) + 1.0e-4);
  CHECK(lapse.frame_move <= 0.375f);
  // The feet go with the ground (`follow`): the capsule's round bottom on the slope
  // (0.3 (1 / cos - 1)) and the backend's 2 cm padding over the collision ground, and no more —
  // until 2026-09-29 a standing walker was left on its contact until a rebuild over sand that had
  // dropped away, and so off by the stalest rebuild besides.
  const f32 cosine = 1.0f / std::sqrt(1.0f + lapse.slope * lapse.slope);
  CHECK(lapse.worst_feet <= 0.3f * (1.0f / cosine - 1.0f) + 0.02f + 1.0e-3f);
}

namespace {

// The drawn ground at (x, z): the pair of fields blended as the pool pass blends them, on the
// collision's own 1 m lattice and triangulated as the backend's heightfield triangulates a cell —
// exactly what a tile rebuilt now would hold there. The last cell asked is kept.
struct DrawnCell {
  i32 i = 0;
  i32 j = 0;
  f64 time_a = -1.0;
  f64 time_b = -1.0;
  f32 a[4] = {};
  f32 b[4] = {};
};

f32 drawn_at(const scene_gen::GroundProvider& ground, const GroundTime& t, f32 x, f32 z,
             DrawnCell& cell) {
  const i32 i = static_cast<i32>(std::floor(x));
  const i32 j = static_cast<i32>(std::floor(z));
  if (cell.i != i || cell.j != j || cell.time_a != t.time_a || cell.time_b != t.time_b) {
    const scene_gen::Lattice lattice = scene_gen::ring_lattice(1000);
    REQUIRE(ground.evaluate(t.time_a, lattice, i, j, 2, 2, 0, 1, std::span<f32>(cell.a, 4)));
    REQUIRE(ground.evaluate(t.time_b, lattice, i, j, 2, 2, 0, 1, std::span<f32>(cell.b, 4)));
    cell.i = i;
    cell.j = j;
    cell.time_a = t.time_a;
    cell.time_b = t.time_b;
  }
  const f32 w = static_cast<f32>(t.blend);
  f32 h[4];
  for (u32 k = 0; k < 4; ++k)
    h[k] = cell.a[k] * (1.0f - w) + cell.b[k] * w;
  // h[0] (i, j), h[1] (i + 1, j), h[2] (i, j + 1), h[3] (i + 1, j + 1); split along the
  // (0, 0)-(1, 1) diagonal, as scene_collision.cpp's `cell_height` and the backend do.
  const f32 u = x - static_cast<f32>(i);
  const f32 v = z - static_cast<f32>(j);
  if (v >= u) return h[0] + v * (h[2] - h[0]) + u * (h[3] - h[2]);
  return h[0] + v * (h[3] - h[1]) + u * (h[1] - h[0]);
}

// The largest difference of two fields over the drawn level's own lattice round (x, z).
f64 pair_delta_at(const scene_gen::GroundProvider& ground, const renderer::TerrainDesc& terrain,
                  f64 a, f64 b, f32 x, f32 z) {
  const scene_gen::Lattice lattice = scene_gen::scene_lattice(terrain.extent, terrain.size);
  const f64 extent = static_cast<f64>(terrain.extent);
  const i32 i0 =
      static_cast<i32>(std::floor((static_cast<f64>(x) + extent) / lattice.spacing)) - 32;
  const i32 j0 =
      static_cast<i32>(std::floor((static_cast<f64>(z) + extent) / lattice.spacing)) - 32;
  Vector<f32> fa(64 * 64);
  Vector<f32> fb(64 * 64);
  REQUIRE(ground.evaluate(a, lattice, i0, j0, 64, 64, 0, 1, std::span<f32>(fa.data(), fa.size())));
  REQUIRE(ground.evaluate(b, lattice, i0, j0, 64, 64, 0, 1, std::span<f32>(fb.data(), fb.size())));
  f64 d = 0.0;
  for (u32 k = 0; k < fa.size(); ++k)
    d = std::max(d, static_cast<f64>(std::fabs(fb[k] - fa[k])));
  return d;
}

// How the pairs are timed. Offscreen (`turnaround_s` 0) by the displacement rule alone, each field
// waited for; in a window also by the keep-up rule, `lead` times the level's turnaround ahead at
// the rate (renderer.md, "A clock that never stops") — for the scene's grid twice
// `renderer.terrain.lead`, since it has room for one field after b — which at a day or a week a
// second makes a pair a cross-fade days apart, crossed at up to the per-frame bound.
struct Cadence {
  f64 turnaround_s = 0.0;
  f64 lead = 3.0;
};

// A walker standing still on the erg under a time-lapse: what its feet did, frame by frame, against
// the drawn ground under them.
struct Stand {
  u32 frames = 0;
  u32 pairs = 0;
  f32 drawn_change = 0.0f;    // the drawn ground under the feet, the last frame's minus the first's
  f32 max_drawn_step = 0.0f;  // the most it moved in one frame
  f32 max_feet_step = 0.0f;   // the most the feet moved in one frame
  u32 jumps = 0;              // frames the feet moved over a centimetre and 4 times the ground
  f32 max_below = 0.0f;       // before a step: how far the feet were under the collision ground
  f32 max_sunk = 0.0f;        // after a frame: how far the feet were under the drawn ground
  f32 max_above = 0.0f;       // and over it (standing on collision the sand dropped away from)
  bool fell = false;          // a frame ended with the feet a capsule's radius under the drawn sand
  u32 lifts = 0;
  f32 max_lift = 0.0f;
  Stats stats;
};

// `frames` frames at `rate` of the walker standing at (wx, wz): 60 a second, four 240 Hz ticks
// each, every tick the collision's refresh round the feet and then the character's step, as
// engine-view's walker does (walk.cpp).
void stand(const Erg& erg, f64 rate, u32 frames, f32 wx, f32 wz, const Cadence& cadence,
           Stand& out) {
  const scene_gen::GroundProvider& ground = erg.sampler->provider();
  const renderer::TerrainDesc& terrain = erg.desc.terrain;
  renderer::SceneData scene;
  scene.terrain = terrain;
  physics::WorldOptions options;
  options.max_bodies = 1024;
  physics::World physics;
  REQUIRE(physics.init(options) == physics::Status::Ok);
  SceneCollision collision;
  const Config config;  // the tunables' defaults
  std::string error;
  REQUIRE_MESSAGE(collision.create(physics, scene, &ground, config, &error), error);

  const f64 spacing = 2.0 * static_cast<f64>(terrain.extent) / static_cast<f64>(terrain.size - 1);
  const renderer::TimeLapseConfig lapse;  // the tunables' defaults: a quarter, a second, 30 days
  const f64 bound = lapse.fraction * spacing;
  renderer::TerrainBlend blend;
  blend.time_a = terrain.time_s;
  blend.surface_s = terrain.time_s;
  renderer::TerrainNextField next;
  f64 last_delta = 0.0;
  auto ask = [&]() {
    const f64 from = blend.has_b ? blend.time_b : blend.time_a;
    f64 at = renderer::terrain_next_time(*erg.sampler, from, spacing, lapse.fraction,
                                         lapse.min_step_s, lapse.max_step_s);
    if (cadence.turnaround_s > 0.0) {
      renderer::TerrainKeepUp keep;
      keep.rate = rate;
      keep.turnaround_s = cadence.turnaround_s;
      keep.lead = cadence.lead;
      keep.frame_s = 1.0 / 60.0;
      keep.delta_m = last_delta;
      keep.budget_m = bound;
      keep.longest_step_s = lapse.max_step_s;
      at = renderer::terrain_keep_up_time(from, at, keep);
    }
    next.time_s = at;
    next.delta_m = pair_delta_at(ground, terrain, from, at, wx, wz);
    last_delta = next.delta_m;
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

  collision.set_ground_time(ground_time());
  world::World ring(ring_params_from_tunables(32.0f));
  ring.add_consumer(collision.consumer());
  sim::ObserverSet at;
  at.add(Vec3{wx, 0.0f, wz}, 1.0f);
  ring.update(at, 0, true);
  f32 y = 0.0f;
  REQUIRE(collision.ground_height(wp(wx, wz), y));
  physics::CharacterConfig c;
  c.step_hz = 240;
  physics::CharacterBody body;
  REQUIRE(body.create(physics, c, place(Vec3{wx, y + 0.02f, wz})) == physics::Status::Ok);
  // Settled on the still ground first: half a second.
  for (u32 tick = 0; tick < 120; ++tick)
    REQUIRE(body.step(physics::CharacterInput{}) == physics::Status::Ok);

  DrawnCell cell;
  f64 game = terrain.time_s;
  f32 first_drawn = 0.0f;
  f32 last_drawn = 0.0f;
  f32 last_feet = local_of(body.feet()).y;
  f32 last_step = 0.0f;
  for (u32 frame = 0; frame < frames; ++frame) {
    game += rate / 60.0;
    if (!next.ready) ask();
    const renderer::TerrainFrameResult result =
        renderer::terrain_blend_frame(blend, game, bound, next);
    out.pairs += result.installed;
    collision.set_ground_time(ground_time());
    for (u32 tick = 0; tick < 4; ++tick) {
      collision.follow(body);
      const Vec3 feet = local_of(body.feet());
      f32 held = 0.0f;
      if (collision.ground_height(wp(feet.x, feet.z), held)) {
        out.max_below = std::max(out.max_below, held - feet.y);
      }
      REQUIRE(body.step(physics::CharacterInput{}) == physics::Status::Ok);
    }
    const Vec3 feet = local_of(body.feet());
    const f32 drawn = drawn_at(ground, ground_time(), feet.x, feet.z, cell);
    if (frame == 0) {
      first_drawn = drawn;
    } else {
      const f32 ground_step = std::fabs(drawn - last_drawn);
      const f32 feet_step = std::fabs(feet.y - last_feet);
      out.max_drawn_step = std::max(out.max_drawn_step, ground_step);
      out.max_feet_step = std::max(out.max_feet_step, feet_step);
      if (feet_step > 0.01f && feet_step > 4.0f * std::max(ground_step, last_step)) ++out.jumps;
      last_step = ground_step;
    }
    out.max_sunk = std::max(out.max_sunk, drawn - feet.y);
    out.max_above = std::max(out.max_above, feet.y - drawn);
    if (feet.y < drawn - c.radius) out.fell = true;
    last_drawn = drawn;
    last_feet = feet.y;
    ++out.frames;
  }
  out.drawn_change = last_drawn - first_drawn;
  out.stats = collision.stats();
  out.lifts = static_cast<u32>(out.stats.lifts);
  out.max_lift = static_cast<f32>(out.stats.max_lift_m);
}

void report(const std::string& what, f64 rate, const Stand& s) {
  MESSAGE(what << " at " << rate << ": " << s.frames << " frames, " << s.pairs
               << " fields; the drawn ground under the feet moved " << s.drawn_change
               << " m, at most " << s.max_drawn_step << " m a frame; the feet at most "
               << s.max_feet_step << " m a frame, " << s.jumps << " jumps; before a step at most "
               << s.max_below << " m under the collision ground; after a frame at most "
               << s.max_sunk << " m under the drawn ground and " << s.max_above
               << " m over it; fell through: " << std::string(s.fell ? "yes" : "no") << "; "
               << s.stats.carries << " carries (at most " << s.stats.max_carry_m << " m), "
               << s.lifts << " lifts (at most " << s.max_lift << " m); " << s.stats.refreshes
               << " tile rebuilds");
}

}  // namespace

TEST_CASE("scene_collision: a walker standing on the erg moves with the drawn sand, in no steps") {
  // The owner's start at the owner's rate (2026-09-29): x = 140, z = -60 on the interdune floor at
  // the mega-draa's toe, where at 600 game seconds a real second the sand sinks under a standing
  // walker by 0.2-1.3 mm a frame (docs/experiments/walk-in-time-lapse-2026-09-29.md). Until the
  // walker went with the ground (`follow`) and the ground under it kept to a millimetre, it stood
  // on its contact while the collision ground stayed 5 cm behind and then dropped 4.65 cm in one
  // frame: three jumps in these twenty seconds, with the sand under it moving 0.15 mm a frame.
  Erg erg;
  load_erg(erg);
  if (!erg.ok) return;
  Stand s;
  stand(erg, 600.0, 1200, 140.0f, -60.0f, Cadence{}, s);
  report("standing at the owner's start", 600.0, s);
  CHECK(s.drawn_change < -0.1f);  // it sinks, as it did under the owner
  CHECK(s.jumps == 0);
  CHECK(s.max_feet_step <= s.max_drawn_step + k_underfoot_error_m + 1.0e-4f);
  CHECK(s.max_above <= k_underfoot_error_m + 1.0e-4f);
  CHECK(s.max_below <= 1.0e-5f);
}

TEST_CASE("scene_collision: a walker is never below the collision ground, on rising sand") {
  // Where the erg's sand rises most over ten seconds at each rung of the dune ladder (a scan every
  // 25 m over a kilometre either side of the origin, from the committed time; a minute at 600,
  // where it rises slowest), a walker standing there, with the pairs timed as a window times them.
  // A walker is never below the collision ground after a refresh (`follow`). Until 2026-09-29 the
  // sand closed over its feet — one-sided contact, and the character's slope hold zeroing the
  // backend's penetration recovery — burying it 15 and 18 cm at the two slow rungs and dropping it
  // through the dune at the three fast ones, 0.9, 95 and 132 m under, as the owner fell at a week a
  // real second.
  Erg erg;
  load_erg(erg);
  if (!erg.ok) return;
  struct Case {
    f64 rate;
    f32 x;
    f32 z;
    u32 frames;
  };
  const Case cases[] = {{600.0, -250.0f, 975.0f, 3600},
                        {3600.0, -500.0f, 900.0f, 600},
                        {8640.0, -750.0f, -575.0f, 600},
                        {86400.0, -500.0f, -825.0f, 600},
                        {604800.0, -400.0f, -1000.0f, 600}};
  for (const Case& k : cases) {
    CAPTURE(k.rate);
    Cadence window;
    window.turnaround_s = 0.8;  // the owner's grid: an evaluation of 750-775 ms and its copy
    Stand s;
    stand(erg, k.rate, k.frames, k.x, k.z, window, s);
    report("rising", k.rate, s);
    CHECK(s.drawn_change > 0.1f);  // it rose under the walker
    CHECK_FALSE(s.fell);
    CHECK(s.max_below <= 1.0e-5f);
    CHECK(s.max_sunk <= k_underfoot_error_m + 1.0e-4f);
    CHECK(s.jumps == 0);
  }
}

TEST_CASE("scene_collision: on the erg at the game's own rate, the collision ground is still") {
  Erg erg;
  load_erg(erg);
  if (!erg.ok) return;
  // Ten real minutes at a game second a real second: the drawn sand moves by millimetres, and only
  // the tile under the walker is rebuilt, a millimetre at a time (none was until 2026-09-29, when
  // the ground under the walker kept to `ground_error_m` too).
  Run still;
  run(erg, 1.0, 36000, false, still);
  MESSAGE("the erg at 1: " << still.frames << " frames, " << still.pairs << " fields drawn, "
                           << still.stats.refreshes << " tile refreshes, the walker's tile at most "
                           << still.worst << " m off the drawn sand");
  CHECK(still.stats.refreshes <= 10);
  CHECK(still.worst <= k_underfoot_error_m + 1.0e-4f);
}

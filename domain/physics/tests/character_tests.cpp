// The character (character.h; docs/subsystems/physics.md, "The character"): a capsule swept
// through a world's static bodies at a fixed tick. Every case here is a world of a few static
// bodies and no job system, and none of them needs a device.

#include "physics_test_support.h"

#include <core/containers/vector.h>
#include <domain/physics/character.h>
#include <domain/physics/physics.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::physics;
using namespace engine::physics::testing;

namespace {

// A static box, axis-aligned unless a rotation is given, `center` in `site`'s frame.
BodyId add_box(World& world, Vec3 center, Vec3 half_extent, Quat rotation = Quat{},
               WorldPos site = WorldPos::origin()) {
  ShapeId shape;
  REQUIRE(world.create_box(half_extent, shape) == Status::Ok);
  BodyDesc desc;
  desc.shape = shape;
  desc.transform.position = place(center, site);
  desc.transform.rotation = rotation;
  desc.motion = MotionType::Static;
  desc.layer = Layer::Static;
  BodyId body;
  REQUIRE(world.create_body(desc, body) == Status::Ok);
  return body;
}

// A heightfield of `n` x `n` samples a metre apart, its sample (0, 0) at (origin, origin) and its
// heights a function of the sample indices made of products and sums only, so the field is the
// same bits on every compiler (the pinned hash below walks on one).
struct Field {
  u32 n = 64;
  f32 origin = -32.0f;
  Vector<f32> heights;
  f32 at(u32 i, u32 j) const { return heights[j * n + i]; }
  // The surface between the samples as the backend triangulates it: the cell split along its
  // (i, j)-(i + 1, j + 1) diagonal.
  f32 surface(f32 x, f32 z) const {
    const f32 fx = x - origin;
    const f32 fz = z - origin;
    const u32 i = static_cast<u32>(std::floor(fx));
    const u32 j = static_cast<u32>(std::floor(fz));
    const f32 u = fx - static_cast<f32>(i);
    const f32 v = fz - static_cast<f32>(j);
    const f32 h00 = at(i, j);
    const f32 h10 = at(i + 1, j);
    const f32 h01 = at(i, j + 1);
    const f32 h11 = at(i + 1, j + 1);
    if (v >= u) return h00 + v * (h01 - h00) + u * (h11 - h01);
    return h00 + v * (h11 - h10) + u * (h10 - h00);
  }
};

Field make_field() {
  Field field;
  field.heights.resize(field.n * field.n);
  for (u32 j = 0; j < field.n; ++j) {
    for (u32 i = 0; i < field.n; ++i) {
      const f32 x = static_cast<f32>(i) - 32.0f;
      const f32 z = static_cast<f32>(j) - 32.0f;
      // A bowl with a tilt and a saddle in it: slopes up to about 15 degrees.
      field.heights[j * field.n + i] =
          2.0f + 0.004f * x * x + 0.05f * z - 0.0015f * x * z + 0.002f * z * z;
    }
  }
  return field;
}

// The field in `site`'s frame: the body at the site, the samples offset in its frame.
void add_field(World& world, const Field& field, WorldPos site = WorldPos::origin()) {
  HeightfieldDesc desc;
  desc.heights = std::span<const f32>(field.heights.data(), field.heights.size());
  desc.sample_count = field.n;
  desc.local_offset = Vec3{field.origin, 0.0f, field.origin};
  desc.scale = Vec3{1.0f, 1.0f, 1.0f};
  ShapeId shape;
  REQUIRE(world.create_heightfield(desc, shape) == Status::Ok);
  BodyDesc body;
  body.transform.position = site;
  body.shape = shape;
  body.motion = MotionType::Static;
  body.layer = Layer::Static;
  BodyId id;
  REQUIRE(world.create_body(body, id) == Status::Ok);
}

CharacterConfig config_at(u32 step_hz) {
  CharacterConfig config;
  config.step_hz = step_hz;
  return config;
}

// Steps with the same input `count` times.
void walk(CharacterBody& body, const CharacterInput& input, u32 count) {
  for (u32 i = 0; i < count; ++i)
    REQUIRE(body.step(input) == Status::Ok);
}

}  // namespace

TEST_CASE("character: a config that describes no capsule is refused") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);
  CharacterBody body;
  CharacterConfig c;
  c.radius = 1.0f;  // wider than half the height
  CHECK(body.create(world, c, place(Vec3{})) == Status::InvalidArgument);
  c = CharacterConfig{};
  c.eye_height = 2.0f;  // above the crown
  CHECK(body.create(world, c, place(Vec3{})) == Status::InvalidArgument);
  c = CharacterConfig{};
  c.max_slope_deg = 90.0f;
  CHECK(body.create(world, c, place(Vec3{})) == Status::InvalidArgument);
  c = CharacterConfig{};
  c.step_hz = 0;
  CHECK(body.create(world, c, place(Vec3{})) == Status::InvalidArgument);
  CHECK_FALSE(body.valid());
  CHECK(body.step(CharacterInput{}) == Status::InvalidArgument);
  World empty;  // not initialized
  CHECK(body.create(empty, CharacterConfig{}, place(Vec3{})) == Status::InvalidArgument);
  REQUIRE(body.create(world, CharacterConfig{}, place(Vec3{})) == Status::Ok);
  CHECK(body.create(world, CharacterConfig{}, place(Vec3{})) == Status::InvalidArgument);  // twice
  CHECK(std::strcmp(ground_name(Ground::OnSteepGround), "on_steep_ground") == 0);
}

TEST_CASE("character: stands on flat ground at its feet, and its eye is eye height above them") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);
  add_ground(world);
  CharacterBody body;
  REQUIRE(body.create(world, config_at(60), place(Vec3{0.0f, 0.5f, 0.0f})) == Status::Ok);
  walk(body, CharacterInput{}, 120);  // falls half a metre and settles
  const CharacterState s = body.state();
  CHECK(s.ground == Ground::OnGround);
  CHECK(std::fabs(local_of(s.position).y) <
        0.03f);  // the backend keeps a 2 cm padding off the ground
  CHECK(std::fabs(local_of(s.position).x) < 1.0e-4f);
  CHECK(s.tick == 120);
  CHECK(local_of(body.eye()).y == doctest::Approx(local_of(s.position).y + 1.65f));
  CHECK(s.ground_normal.y == doctest::Approx(1.0f));
}

TEST_CASE("character: walking follows a heightfield's surface") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);
  const Field field = make_field();
  add_field(world, field);
  CharacterBody body;
  const CharacterConfig config = config_at(240);
  REQUIRE(body.create(world, config,
                      place(Vec3{-20.0f, field.surface(-20.0f, -3.0f) + 0.05f, -3.0f})) ==
          Status::Ok);
  walk(body, CharacterInput{}, 60);
  // East and a little north across the bowl and its saddle, eight seconds at a walk: every step on
  // the ground, the feet on the surface. A capsule's round bottom rests above the point under its
  // centre by r (1 / cos(slope) - 1) on a slope (1 cm at 15 degrees) plus the backend's 2 cm
  // padding; the heightfield itself is quantized to a few millimetres. 5 cm covers all of it with
  // room, and a walker 5 cm off the sand is not one anybody can see.
  CharacterInput east;
  east.move = Vec3{0.96f, 0.0f, 0.28f};
  f32 worst = 0.0f;
  f32 lowest = 0.0f;
  u32 grounded = 0;
  for (u32 i = 0; i < 8 * 240; ++i) {
    REQUIRE(body.step(east) == Status::Ok);
    const CharacterState s = body.state();
    if (s.ground != Ground::OnGround) continue;
    ++grounded;
    const f32 gap =
        local_of(s.position).y - field.surface(local_of(s.position).x, local_of(s.position).z);
    worst = gap > worst ? gap : worst;
    lowest = gap < lowest ? gap : lowest;
  }
  const CharacterState end = body.state();
  MESSAGE("heightfield walk: " << grounded << " of 1920 steps grounded, feet from " << lowest
                               << " to " << worst << " m off the surface, ended at x "
                               << local_of(end.position).x);
  CHECK(grounded == 8 * 240);
  CHECK(worst < 0.05f);
  CHECK(lowest > -0.01f);
  CHECK(local_of(end.position).x >
        -20.0f + 8.0f * 1.5f * 0.96f * 0.9f);  // it walked, near full speed
}

TEST_CASE("character: a wall stops it") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);
  add_ground(world);
  // A wall half a metre thick and three high, its near face at x = 4.75.
  add_box(world, Vec3{5.0f, 1.5f, 0.0f}, Vec3{0.25f, 1.5f, 5.0f});
  CharacterBody body;
  const CharacterConfig config = config_at(60);
  REQUIRE(body.create(world, config, place(Vec3{0.0f, 0.02f, 0.0f})) == Status::Ok);
  CharacterInput east;
  east.move = Vec3{1.0f, 0.0f, 0.0f};
  walk(body, east, 5 * 60);  // 7.5 m of walking at 1.5 m/s: far past the wall, were it not there
  const CharacterState s = body.state();
  MESSAGE("stopped at x " << local_of(s.position).x << " against a face at 4.75");
  CHECK(local_of(s.position).x < 4.75f - config.radius + 0.03f);
  CHECK(local_of(s.position).x > 4.75f - config.radius - 0.1f);  // it got there
  CHECK(s.ground == Ground::OnGround);
  CHECK(std::fabs(local_of(s.position).y) < 0.03f);
  // Sprinting and jumping into it does not get it through either: the wall is higher than a jump.
  CharacterInput run = east;
  run.sprint = true;
  run.jump = true;
  walk(body, run, 3 * 60);
  CHECK(local_of(body.state().position).x < 4.75f - config.radius + 0.03f);
}

TEST_CASE("character: it climbs a step under its step height and not one over it") {
  auto run = [](f32 step_top, CharacterState& out) {
    World world;
    REQUIRE(world.init(small_world_options()) == Status::Ok);
    add_ground(world);
    // A raised floor from x = 2 onwards, `step_top` high.
    add_box(world, Vec3{7.0f, 0.5f * step_top, 0.0f}, Vec3{5.0f, 0.5f * step_top, 5.0f});
    CharacterBody body;
    REQUIRE(body.create(world, config_at(60), place(Vec3{0.0f, 0.02f, 0.0f})) == Status::Ok);
    CharacterInput east;
    east.move = Vec3{1.0f, 0.0f, 0.0f};
    walk(body, east, 4 * 60);
    out = body.state();
  };
  CharacterState low;
  run(0.25f, low);  // under the default 0.35 m
  MESSAGE("a 0.25 m step: ended at x " << local_of(low.position).x << ", y "
                                       << local_of(low.position).y);
  CHECK(local_of(low.position).x > 4.0f);
  CHECK(std::fabs(local_of(low.position).y - 0.25f) < 0.03f);
  CHECK(low.ground == Ground::OnGround);

  CharacterState high;
  run(0.5f, high);  // over it
  MESSAGE("a 0.5 m step: ended at x " << local_of(high.position).x << ", y "
                                      << local_of(high.position).y);
  CHECK(local_of(high.position).x < 2.0f - 0.3f + 0.03f);
  CHECK(std::fabs(local_of(high.position).y) < 0.03f);
}

TEST_CASE("character: it slides down a slope past the limit and stands on one under it") {
  // A ramp rising towards +x at `degrees`, its top face through the origin, and the character put
  // on it there and given no input for a second.
  auto on_ramp = [](f32 degrees, const CharacterInput& input, CharacterState& start,
                    CharacterState& end) {
    World world;
    REQUIRE(world.init(small_world_options()) == Status::Ok);
    const Quat tilt = quat_from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, radians(degrees));
    add_box(world, rotate(tilt, Vec3{0.0f, -0.5f, 0.0f}), Vec3{20.0f, 0.5f, 5.0f}, tilt);
    CharacterBody body;
    REQUIRE(body.create(world, config_at(60), place(Vec3{0.0f, 0.05f, 0.0f})) == Status::Ok);
    walk(body, CharacterInput{}, 10);  // settle onto it
    start = body.state();
    walk(body, input, 60);
    end = body.state();
  };
  CharacterState start;
  CharacterState end;
  on_ramp(25.0f, CharacterInput{}, start, end);  // under the 40 degree limit
  MESSAGE("25 degrees, a second of nothing: moved "
          << length(local_of(end.position) - local_of(start.position)) << " m, "
          << std::string(ground_name(end.ground)));
  CHECK(start.ground == Ground::OnGround);
  CHECK(end.ground == Ground::OnGround);
  CHECK(length(local_of(end.position) - local_of(start.position)) < 0.01f);

  on_ramp(55.0f, CharacterInput{}, start, end);  // past it
  MESSAGE("55 degrees, a second of nothing: moved "
          << local_of(end.position).x - local_of(start.position).x << " m in x, "
          << std::string(ground_name(end.ground)));
  CHECK(start.ground == Ground::OnSteepGround);
  CHECK(local_of(end.position).x <
        local_of(start.position).x - 1.0f);  // down the slope, at gravity's pace
  CHECK(local_of(end.position).y < local_of(start.position).y - 1.0f);

  // And walking up it gets nowhere: the face past the limit is a wall to a walker.
  CharacterInput up;
  up.move = Vec3{1.0f, 0.0f, 0.0f};
  on_ramp(55.0f, up, start, end);
  CHECK(local_of(end.position).y < local_of(start.position).y + 0.02f);
  // Up one under the limit it goes.
  on_ramp(25.0f, up, start, end);
  CHECK(local_of(end.position).y > local_of(start.position).y + 0.5f);
}

TEST_CASE("character: a jump rises by v^2 / 2g and lands") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);
  add_ground(world);
  CharacterBody body;
  const CharacterConfig config = config_at(240);
  REQUIRE(body.create(world, config, place(Vec3{0.0f, 0.02f, 0.0f})) == Status::Ok);
  walk(body, CharacterInput{}, 24);
  const f32 floor = local_of(body.state().position).y;
  CharacterInput jump;
  jump.jump = true;
  REQUIRE(body.step(jump) == Status::Ok);
  f32 top = floor;
  u32 landed = 0;
  for (u32 i = 1; i < 2 * 240; ++i) {
    REQUIRE(body.step(CharacterInput{}) == Status::Ok);
    const CharacterState s = body.state();
    top = local_of(s.position).y > top ? local_of(s.position).y : top;
    if (landed == 0 && i > 10 && s.ground == Ground::OnGround) landed = i;
  }
  const f32 expected = config.jump_speed * config.jump_speed / (2.0f * config.gravity);
  MESSAGE("jumped " << top - floor << " m (v^2/2g " << expected << "), landed after " << landed
                    << " steps");
  CHECK(std::fabs((top - floor) - expected) < 0.03f);
  // 2v/g = 0.82 s, 196 steps at 240 Hz.
  CHECK(landed > 180);
  CHECK(landed < 215);
  // Back on the ground a jump starts at once; held on the next step, in the air, it does nothing.
  REQUIRE(body.step(jump) == Status::Ok);
  const f32 rising = body.state().velocity.y;
  CHECK(rising > 3.5f);
  REQUIRE(body.step(jump) == Status::Ok);
  CHECK(body.state().velocity.y < rising);
}

namespace {

// One scripted walk over a heightfield with two walls and a step in it: walking, turning,
// sprinting, jumping and running into things, 2,400 steps at 240 Hz. Only products, sums and the
// backend's own arithmetic, so the result is the same bits wherever it runs.
struct Walked {
  u64 hash = 0;
  CharacterState end;
  u32 airborne = 0;
  // The feet after every step, from the site, in f64: what the far case compares.
  Vector<DVec3> feet;
};

constexpr u32 k_walk_steps = 2400;
// How far the scripted walk at a far site may stand off the origin's at any step (the far case
// below): measured, then given a margin.
constexpr f64 k_far_walk_tolerance_m = 1.0e-4;

// The walk with its whole world — field, walls, slab and the feet it starts at — placed at `site`.
Walked scripted_walk(WorldPos site = WorldPos::origin()) {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);
  const Field field = make_field();
  add_field(world, field, site);
  add_box(world, Vec3{-4.0f, 3.0f, 2.0f}, Vec3{0.3f, 3.0f, 6.0f}, Quat{}, site);
  add_box(world, Vec3{6.0f, 3.0f, -8.0f}, Vec3{5.0f, 3.0f, 0.3f}, Quat{}, site);
  add_box(world, Vec3{12.0f, 2.2f, 6.0f}, Vec3{3.0f, 0.2f, 3.0f}, Quat{}, site);
  CharacterBody body;
  REQUIRE(body.create(world, config_at(240), place(Vec3{-14.0f, 4.0f, 0.0f}, site)) == Status::Ok);
  const Vec3 directions[] = {{1.0f, 0.0f, 0.0f},  {0.6f, 0.0f, 0.8f},    {-0.8f, 0.0f, 0.6f},
                             {0.0f, 0.0f, -1.0f}, {0.28f, 0.0f, -0.96f}, {0.5f, 0.0f, 0.0f}};
  Walked out;
  out.feet.reserve(k_walk_steps);
  for (u32 i = 0; i < k_walk_steps; ++i) {
    CharacterInput input;
    const u32 phase = i / 200;
    input.move = directions[phase % 6];
    if (phase == 7) input.move = Vec3{};
    input.sprint = (phase % 3) == 1;
    input.jump = (i % 450) == 100;
    REQUIRE(body.step(input) == Status::Ok);
    if (body.state().ground == Ground::InAir) ++out.airborne;
    out.feet.push_back(body.feet() - site);
  }
  out.hash = body.hash();
  out.end = body.state();
  return out;
}

}  // namespace

TEST_CASE("character: the same inputs give the same state, bit for bit, on every toolchain") {
  const Walked a = scripted_walk();
  const Walked b = scripted_walk();
  CHECK(a.hash == b.hash);
  CHECK(std::memcmp(&a.end, &b.end, sizeof(CharacterState)) == 0);
  CHECK(a.airborne > 0);  // the jumps left the ground
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(a.hash));
  MESSAGE("scripted walk: hash " << hex << ", ended at (" << local_of(a.end.position).x << ", "
                                 << local_of(a.end.position).y << ", " << local_of(a.end.position).z
                                 << "), " << a.airborne << " steps in the air");
  // Pinned on MSVC and checked by GCC and Clang at both CPU baselines (physics.md, "The
  // character"): the backend is built cross-platform deterministic and contraction is off
  // (ADR-0035). A change that moves it changes every recorded walk; say why in the commit.
  CHECK(a.hash == 0x6b70c6f467ae6192ull);
}

// **Far from the origin, the walk is the walk** (ADR-0053; physics.md, "Far from the origin"). The
// scripted walk's whole world moved to each far site walks the same steps as by the origin: the
// backend keeps the feet and every body in double and sweeps the capsule in floats relative to the
// feet, so what changes with the site is only where f64 rounds the absolute positions — its step is
// 2 nm at 10,000 km and 15 nm at 1e8 m — and how far that rounding carries through 2,400 swept
// steps that slide along walls and land jumps. Before the backend was double, the same walk at
// 419 km moved on a 3.1 cm grid and at 1e8 m could not walk at all.
TEST_CASE("character: the scripted walk at 419 km, 10,000 km and 1e8 m is the origin's") {
  const Walked home = scripted_walk();
  REQUIRE(home.feet.size() == k_walk_steps);
  for (u32 s = 1; s < 4; ++s) {
    const Walked far = scripted_walk(k_sites[s]);
    REQUIRE(far.feet.size() == k_walk_steps);
    f64 worst = 0.0;
    u32 worst_step = 0;
    for (u32 i = 0; i < k_walk_steps; ++i) {
      const f64 d = length(far.feet[i] - home.feet[i]);
      if (d > worst) {
        worst = d;
        worst_step = i;
      }
    }
    MESSAGE(site_name(s) << ": the feet stood off the origin's walk by at most " << worst
                         << " m (step " << worst_step << "), " << far.airborne
                         << " steps in the air against " << home.airborne);
    CHECK(far.airborne == home.airborne);
    CHECK(worst < k_far_walk_tolerance_m);
  }
}

// The fly and walk controllers far from the origin (ADR-0053; docs/subsystems/apps.md, "Far from
// the origin"), with no GPU and no window: the same recorded input flown and walked from the origin
// and from 419 km, 10,000 km and 1e8 m out, a walk along x at 10,000 km, the walker's ground
// against the drawn ground at 420 km, and the owner's own sprint of 2026-10-04 at x = -419,055 m.
//
// **Why these and not a tolerance on the old tests.** Until 2026-10-05 both controllers added each
// 240 Hz tick to an absolute float32. 420 km out a float steps by 3.1 cm, so a walk along x at
// 1.5 m/s — a 6 mm tick — rounded back to where it was and did not move, and the owner's sprint 22
// degrees off +z went straight along +z at 3.75 m/s
// (docs/experiments/far-from-origin-2026-10-04.md). A test by the origin cannot see that; these
// can, and each says what it held to.
//
// **The ground** is the test's own provider, registered under its own name: a gentle swell
// periodic every 16 m, asked for in whole millimetres, so a site a whole number of 16 m (and of
// the collision's 32 m tiles) out stands on exactly the ground the origin does, and any difference
// in a walk is the controller's. It is a ground provider in every configuration, so the walk cases
// hold for the physical walker and for the minimal build's ground follower alike.

#include "../fly_camera.h"
#include "../walk.h"

#include <core/math/world.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/tile_source.h>
#include <foundation/input/input.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace engine;

namespace {

constexpr u32 k_key_w = 26;
constexpr u32 k_key_a = 4;
constexpr u32 k_key_s = 22;
constexpr u32 k_key_d = 7;
constexpr u32 k_key_e = 8;
constexpr u32 k_key_q = 20;
constexpr u32 k_key_space = 44;
constexpr u32 k_key_lshift = 225;
constexpr u32 k_key_lalt = 226;

input::RawEvent key(u64 tick, u32 code, bool down) {
  return input::RawEvent{SimTick{tick}, input::Source::Key, code, down ? 1.0f : 0.0f, 0};
}
input::RawEvent pointer(u64 tick, input::MouseAxisCode axis, f32 pixels) {
  return input::RawEvent{SimTick{tick}, input::Source::MouseAxis, static_cast<u32>(axis), pixels,
                         0};
}

// ---- the ground: a swell periodic every 16 m, in whole millimetres ---------------------------

constexpr i64 k_period_mm = 16000;
constexpr const char* k_ground_name = "engine-view-far-test";

f32 swell(i64 x_mm, i64 z_mm) noexcept {
  const i64 a = ((x_mm % k_period_mm) + k_period_mm) % k_period_mm;
  const i64 b = ((z_mm % k_period_mm) + k_period_mm) % k_period_mm;
  constexpr f64 k_turn = 6.283185307179586 / static_cast<f64>(k_period_mm);
  return static_cast<f32>(80.0 + 0.05 * std::sin(k_turn * static_cast<f64>(a)) +
                          0.03 * std::cos(k_turn * static_cast<f64>(b)));
}

void swell_destroy(void*) noexcept {}
f32 swell_height(const void*, f32 x, f32 z) noexcept {
  return swell(scene_gen::nearest_mm(static_cast<f64>(x)),
               scene_gen::nearest_mm(static_cast<f64>(z)));
}
void swell_grid(const void*, const scene_gen::Lattice& lattice, i32 i0, i32 j0, u32 nx, u32 nz,
                std::span<f32> heights) noexcept {
  for (u32 j = 0; j < nz; ++j) {
    for (u32 i = 0; i < nx; ++i) {
      heights[static_cast<usize>(j) * nx + i] =
          swell(lattice.x_mm(i64{i0} + i), lattice.z_mm(i64{j0} + j));
    }
  }
}
f32 swell_height_mm(const void*, f64, i64 x_mm, i64 z_mm) noexcept { return swell(x_mm, z_mm); }

constexpr scene_gen::GroundOps k_swell_ops{.destroy = &swell_destroy,
                                           .height = &swell_height,
                                           .grid = &swell_grid,
                                           .height_mm = &swell_height_mm};
bool swell_make(const scene::Terrain&, const scene_gen::Context&, scene_gen::GroundProvider& out,
                std::string*) {
  out = scene_gen::GroundProvider(&k_swell_ops, nullptr);
  return true;
}
constexpr scene_gen::GroundProviderDesc k_swell{.name = k_ground_name, .make = &swell_make};
const scene_gen::Registrar k_swell_registrar{k_swell};

renderer::SceneData swell_scene() {
  renderer::SceneData scene;
  scene.terrain.enabled = true;
  scene.terrain.size = 65;
  scene.terrain.extent = 60.0f;
  scene.terrain.provider = k_ground_name;
  return scene;
}

// ---- the sites -------------------------------------------------------------------------------

// The ADR-0053 sites, each a whole number of 32 m (the collision's tiles, and so the swell's 16 m).
struct Site {
  const char* name;
  WorldPos at;
};
constexpr Site k_sites[] = {
    {"origin", WorldPos{0.0, 0.0, 0.0}},
    {"419 km", WorldPos{419072.0, 0.0, -419072.0}},
    {"10,000 km", WorldPos{10000000.0, 0.0, 10000000.0}},
    {"1e8 m", WorldPos{100000000.0, 0.0, -100000000.0}},
};
constexpr u32 k_site_count = sizeof(k_sites) / sizeof(k_sites[0]);
// And for the flight, a site whose start, (0, 8, 22) from it, is a metre short of a 64 m cell's
// corner in x and in z (419,072 m is cell 6,548's edge), so the flight crosses both edges.
constexpr Site k_edge{"across a cell's corner at 419 km", WorldPos{419071.0, 0.0, -419093.0}};

// One f64 step at a site's largest coordinate: what a sum there rounds to.
f64 f64_step(WorldPos at) noexcept {
  const f64 m = std::max(std::fabs(at.x), std::max(std::fabs(at.y), std::fabs(at.z)));
  return m > 0.0 ? std::ldexp(1.0, std::ilogb(m) - 52) : 0.0;
}

// What a session did, a tick at a time, from its site.
struct Path {
  Vector<DVec3> eye;  // the camera's position from the site, every tick
  Vector<f32> yaw;
  Vector<f32> pitch;
  f32 ground_error = 0.0f;
  const char* collision = "";
};

struct Compared {
  f64 worst = 0.0;  // the largest distance between the two at one tick
  u32 worst_tick = 0;
  f64 end = 0.0;  // between where they ended
  u32 angles_off = 0;
};
Compared compare(const Path& home, const Path& far) {
  Compared out;
  REQUIRE(home.eye.size() == far.eye.size());
  for (u32 k = 0; k < home.eye.size(); ++k) {
    const f64 off = length(far.eye[k] - home.eye[k]);
    if (off > out.worst) {
      out.worst = off;
      out.worst_tick = k + 1;
    }
    out.angles_off += far.yaw[k] == home.yaw[k] && far.pitch[k] == home.pitch[k] ? 0u : 1u;
  }
  out.end = length(far.eye.back() - home.eye.back());
  return out;
}

// ---- the recorded input ------------------------------------------------------------------------

// Two seconds of flight: forward with a turn, a strafe with fast held, a climb looking up, a slow
// reverse, a descent while strafing and looking down, a diagonal of two keys.
Vector<input::RawEvent> flight_events() {
  Vector<input::RawEvent> e;
  for (u64 t = 1; t <= 480; ++t) {
    if (t == 10) e.push_back(key(t, k_key_w, true));
    if (t == 130) e.push_back(key(t, k_key_w, false));
    if (t >= 20 && t < 80) e.push_back(pointer(t, input::MouseAxisCode::X, 4.0f));
    if (t == 140) e.push_back(key(t, k_key_d, true));
    if (t == 150) e.push_back(key(t, k_key_lshift, true));
    if (t == 190) e.push_back(key(t, k_key_lshift, false));
    if (t == 200) e.push_back(key(t, k_key_d, false));
    if (t == 210) e.push_back(key(t, k_key_e, true));
    if (t >= 210 && t < 250) e.push_back(pointer(t, input::MouseAxisCode::Y, -3.0f));
    if (t == 260) e.push_back(key(t, k_key_e, false));
    if (t == 270) e.push_back(key(t, k_key_s, true));
    if (t == 275) e.push_back(key(t, k_key_lalt, true));
    if (t == 320) e.push_back(key(t, k_key_lalt, false));
    if (t == 330) e.push_back(key(t, k_key_s, false));
    if (t == 340) {
      e.push_back(key(t, k_key_q, true));
      e.push_back(key(t, k_key_a, true));
    }
    if (t >= 350 && t < 380) {
      e.push_back(pointer(t, input::MouseAxisCode::X, -1.5f));
      e.push_back(pointer(t, input::MouseAxisCode::Y, 5.0f));
    }
    if (t == 400) {
      e.push_back(key(t, k_key_q, false));
      e.push_back(key(t, k_key_a, false));
    }
    if (t == 440) {
      e.push_back(key(t, k_key_w, true));
      e.push_back(key(t, k_key_d, true));
    }
    if (t == 470) {
      e.push_back(key(t, k_key_w, false));
      e.push_back(key(t, k_key_d, false));
    }
  }
  return e;
}

// Two and a half seconds of walking: a second north with a turn, a sprint, a jump, a strafe, and
// back.
Vector<input::RawEvent> walk_events() {
  Vector<input::RawEvent> e;
  for (u64 t = 1; t <= 600; ++t) {
    if (t == 10) e.push_back(key(t, k_key_w, true));
    if (t >= 40 && t < 100) e.push_back(pointer(t, input::MouseAxisCode::X, 3.0f));
    if (t == 250) e.push_back(key(t, k_key_lshift, true));
    if (t == 300) e.push_back(key(t, k_key_space, true));
    if (t == 302) e.push_back(key(t, k_key_space, false));
    if (t == 370) {
      e.push_back(key(t, k_key_lshift, false));
      e.push_back(key(t, k_key_w, false));
      e.push_back(key(t, k_key_d, true));
    }
    if (t == 450) {
      e.push_back(key(t, k_key_d, false));
      e.push_back(key(t, k_key_s, true));
    }
    if (t == 580) e.push_back(key(t, k_key_s, false));
  }
  return e;
}

view::SessionHeader header_at(WorldPos start, bool walking) {
  view::SessionHeader h;
  h.start.position = start;
  h.start.yaw = 0.0f;
  h.start.pitch = -0.3f;
  h.params.tick_hz = 240;
  h.params.speed = 4.0f;
  h.has_walk = true;
  h.walking = walking;
  h.walk = view::WalkParams{};
  h.ticks = walking ? 600 : 480;
  return h;
}

// The ground as the frames would draw it from the world's tiles: the finest level's 25 cm lattice
// of the same source the collision is made of (walk.h, `DrawnGround`).
view::DrawnGround drawn_from(const scene_gen::TileSource* tiles) {
  view::DrawnGround d;
  d.lattice = scene_gen::ring_lattice(250);
  d.tiles = tiles;
  return d;
}

Path fly_from(WorldPos site) {
  const Vector<input::RawEvent> events = flight_events();
  const view::SessionHeader header = header_at(site + DVec3{0.0, 8.0, 22.0}, false);
  const input::ActionMap map = view::default_fly_map();  // outlives the session
  view::FlySession session;
  std::string error;
  REQUIRE_MESSAGE(session.start(map, header, &error), error);
  Path out;
  u32 cursor = 0;
  for (u64 t = 1; t <= header.ticks; ++t) {
    cursor = session.run(std::span<const input::RawEvent>(events.data(), events.size()), cursor,
                         SimTick{t});
    out.eye.push_back(session.state().position - site);
    out.yaw.push_back(session.state().yaw);
    out.pitch.push_back(session.state().pitch);
  }
  return out;
}

Path walk_from(WorldPos site) {
  const renderer::SceneData scene = swell_scene();
  const renderer::TerrainSampler sampler(scene.terrain);
  REQUIRE_MESSAGE(sampler.ok(), sampler.error());
  const scene_gen::TileSource tiles = sampler.provider().tiles();
  const Vector<input::RawEvent> events = walk_events();
  // Mid-tile, 10 m over the swell, walking from the first tick.
  const view::SessionHeader header = header_at(site + DVec3{16.25, 90.0, 16.5}, true);
  view::Walker walker;
  std::string error;
  REQUIRE_MESSAGE(walker.start(header.walk, 240, scene, &error, &tiles), error);
  REQUIRE(walker.available());
  walker.set_drawn(drawn_from(&tiles));
  const input::ActionMap map = view::default_fly_map();
  view::FlySession session;
  session.set_walker(&walker);
  REQUIRE_MESSAGE(session.start(map, header, &error), error);
  REQUIRE(session.walking());
  Path out;
  u32 cursor = 0;
  for (u64 t = 1; t <= header.ticks; ++t) {
    cursor = session.run(std::span<const input::RawEvent>(events.data(), events.size()), cursor,
                         SimTick{t});
    out.eye.push_back(session.state().position - site);
    out.yaw.push_back(session.state().yaw);
    out.pitch.push_back(session.state().pitch);
  }
  out.ground_error = walker.stats().max_ground_error_m;
  out.collision = walker.collision();
  return out;
}

}  // namespace

// **The same flight from every site, displaced by the site and nothing else, to a micrometre.** The
// tick's displacement is float32 and the sum f64 (fly_camera.h), which rounds at the coordinate's
// own step: 58 pm at 419 km, 1.9 nm at 10,000 km, 15 nm at 1e8 m. 480 ticks of that could add to
// 3.6 um at 1e8 m at the very worst; measured (2026-10-05, MSVC debug; the arithmetic is IEEE's and
// unfused, so every compiler's), the flight ends 44 pm, 81 nm and 0.50 um from the origin's, and
// 44 pm across a cell's corner. A float32 position missed by a float's step (3.1 cm, 1 m, 8 m).
TEST_CASE("far: the same input flown from the origin and from 419 km, 10,000 km and 1e8 m") {
  const Path home = fly_from(k_sites[0].at);
  REQUIRE(home.eye.size() == 480);
  f64 reach = 0.0;  // it flew somewhere: the farthest it got from where it started
  for (const DVec3& at : home.eye)
    reach = std::max(reach, length(at - home.eye.front()));
  MESSAGE("the flight reached " << reach << " m from its start");
  CHECK(reach > 2.0);
  const Site sites[] = {k_sites[1], k_sites[2], k_sites[3], k_edge};
  for (const Site& site : sites) {
    const Path far = fly_from(site.at);
    const Compared c = compare(home, far);
    const f64 bound = 1.0e-6;
    MESSAGE(std::string(site.name)
            << ": the flight at most " << c.worst << " m off the origin's at any tick, " << c.end
            << " m at its end (held to " << bound << " m; the worst 480 roundings at the site "
            << "could make " << 480.0 * 0.5 * f64_step(site.at) << " m); " << c.angles_off
            << " ticks' angles off");
    CHECK(c.worst <= bound);
    CHECK(c.end <= bound);
    CHECK(c.angles_off == 0);
  }
}

// **The same walk from every site.** The physical walker is the character's (Jolt in double
// precision) on the collision's ground; the ground follower adds a float32 tick in f64, as the
// flight does. Both start mid-tile on the same swell. **Where the walk ends is held to a
// micrometre at every site** (measured 2026-10-05, MSVC debug: 0.2 nm at 419 km, 0.48 um at
// 10,000 km, 0.55 um at 1e8 m). Along the way the physical walker strays further at the two
// farther sites — 7.5 um at worst, mid-walk (tick 543 at 10,000 km, tick 53 at 1e8 m) — which is
// the character backend's own arithmetic in double mode: its own far walk measured 0.88 um and
// 6.6 um over ten seconds there (docs/experiments/world-positions-physics-2026-10-05.md). Each
// tick is held to a micrometre at 419 km and to 20 um beyond. And the walker's ground against the
// ground as drawn — the collision's 1 m
// heightfield against the 25 cm tile lattice — is under a millimetre at every site and the same
// as by the origin, not a float32 step (3.1 cm at 419 km).
TEST_CASE("far: the same input walked from the origin and from 419 km, 10,000 km and 1e8 m") {
  const Path home = walk_from(k_sites[0].at);
  REQUIRE(home.eye.size() == 600);
  const f64 walked = length(home.eye.back() - home.eye.front());
  MESSAGE("by the origin (" << std::string(home.collision) << "): walked " << walked
                            << " m from start to end; the ground under the feet at most "
                            << home.ground_error << " m off the drawn ground");
  CHECK(walked > 1.0);
  CHECK(home.ground_error < 5.0e-3f);
  for (u32 s = 1; s < k_site_count; ++s) {
    const Path far = walk_from(k_sites[s].at);
    const Compared c = compare(home, far);
    const f64 bound = std::fabs(k_sites[s].at.x) >= 1.0e7 ? 2.0e-5 : 1.0e-6;
    MESSAGE(std::string(k_sites[s].name)
            << ": the walk at most " << c.worst << " m off the origin's (tick " << c.worst_tick
            << ", held to " << bound << " m), " << c.end << " m at its end (held to 1e-6 m); "
            << c.angles_off << " ticks' angles off; the ground under the feet at most "
            << far.ground_error << " m off the drawn ground");
    CHECK(c.worst <= bound);
    CHECK(c.end <= 1.0e-6);
    CHECK(c.angles_off == 0);
    CHECK(far.ground_error < 5.0e-3f);
    CHECK(std::fabs(far.ground_error - home.ground_error) < 1.0e-4f);
  }
}

// **A second's walk along x at 1.5 m/s moves 1.5 m at 10,000 km.** There a float32 steps by a
// metre and the 6 mm tick rounded away: until 2026-10-05 the ground follower stood still, and the
// physical walker's eye, narrowed after each step, stood on a metre's grid.
TEST_CASE("far: a second's walk along x at 1.5 m/s moves 1.5 m 10,000 km out") {
  // On the swell, whose slope takes a millimetre or so off the horizontal: the same by the origin.
  DVec3 by_origin{};
  for (const WorldPos site : {k_sites[0].at, k_sites[2].at}) {
    const renderer::SceneData scene = swell_scene();
    const renderer::TerrainSampler sampler(scene.terrain);
    const scene_gen::TileSource tiles = sampler.provider().tiles();
    view::Walker walker;
    std::string error;
    REQUIRE_MESSAGE(walker.start(view::WalkParams{}, 240, scene, &error, &tiles), error);
    walker.set_drawn(drawn_from(&tiles));
    (void)walker.drop(site + DVec3{16.0, 90.0, 16.0});
    view::WalkInput settle;
    for (u32 t = 0; t < 24; ++t)
      (void)walker.step(settle);
    const WorldPos from = walker.feet();
    view::WalkInput east;
    east.move = Vec2{0.0f, 1.0f};
    east.yaw = -1.5707964f;  // forward along +x
    for (u32 t = 0; t < 240; ++t)
      (void)walker.step(east);
    const DVec3 moved = walker.feet() - from;
    MESSAGE((site.x == 0.0 ? "by the origin" : "10,000 km out")
            << " (" << std::string(walker.collision()) << "): a second east moved " << moved.x
            << " m along x, " << moved.z << " m along z");
    CHECK(std::fabs(moved.x - 1.5) < 0.005);
    CHECK(std::fabs(moved.z) < 0.001);
    if (site.x == 0.0) by_origin = moved;
    CHECK(length(moved - by_origin) < 1.0e-6);
  }
}

// **The owner's sprint, 2026-10-04** (his frame log, game_engine_local/flythrough/
// endless-2026-10-04T1710-frames.jsonl, frames 5256 to 5266): walking on the endless desert at
// (-419,055.125, 81.7, -66,781.67), sprinting at yaw -2.7478 — 22 degrees off +z, towards +x — for
// the 0.1224 s those eleven frames span, his eye's x stayed at -419,055.125 on every frame and its
// z advanced 0.461 m, 3.77 m/s: the float32 sum rounded every tick's 8 mm along x away and every
// 19 mm along z to two of z's 7.8 mm steps. In f64 the same 29 ticks move the walker 0.232 m along
// x and 0.558 m along z, the sprint's 5 m/s in the direction he faced. The ground is the swell, not
// the dunes he walked on: what is checked is the walker's arithmetic, which does not depend on it.
TEST_CASE("far: the owner's sprint at x = -419,055 m goes where he faced, x and z") {
  const renderer::SceneData scene = swell_scene();
  const renderer::TerrainSampler sampler(scene.terrain);
  const scene_gen::TileSource tiles = sampler.provider().tiles();
  view::Walker walker;
  std::string error;
  REQUIRE_MESSAGE(walker.start(view::WalkParams{}, 240, scene, &error, &tiles), error);
  walker.set_drawn(drawn_from(&tiles));
  view::SessionHeader header = header_at(WorldPos{-419055.125, 90.0, -66781.671875}, true);
  header.start.yaw = -2.7478187084198f;  // the record's pose_yaw, frames 5252 to 5265
  header.start.pitch = -0.08433321f;
  header.ticks = 24 + 29;
  const input::ActionMap map = view::default_fly_map();
  view::FlySession session;
  session.set_walker(&walker);
  REQUIRE_MESSAGE(session.start(map, header, &error), error);
  // A tenth of a second to settle on the ground, then W and Shift held for the frames' 29 ticks.
  const input::RawEvent events[] = {key(25, k_key_w, true), key(25, k_key_lshift, true)};
  (void)session.run(std::span<const input::RawEvent>(events), 0, SimTick{24});
  const WorldPos from = session.state().position;
  (void)session.run(std::span<const input::RawEvent>(events), 0, SimTick{24 + 29});
  const DVec3 moved = session.state().position - from;
  f32 s = 0.0f;
  f32 c = 0.0f;
  view::fly_sin_cos(header.start.yaw, s, c);
  const f64 run = 5.0 * 29.0 / 240.0;  // the sprint's 5 m/s over the 29 ticks
  const f64 want_x = -static_cast<f64>(s) * run;
  const f64 want_z = -static_cast<f64>(c) * run;
  MESSAGE("the owner's sprint (" << std::string(walker.collision()) << "): x moved " << moved.x
                                 << " m (wanted " << want_x << ", the record 0), z " << moved.z
                                 << " m (wanted " << want_z << ", the record 0.461)");
  CHECK(std::fabs(moved.x - want_x) < 1.0e-3);
  CHECK(std::fabs(moved.z - want_z) < 1.0e-3);
  CHECK(moved.x > 0.2);  // the record's frames: 0
}

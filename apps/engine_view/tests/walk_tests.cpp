// The walk mode with no GPU and no window (walk.h; docs/subsystems/apps.md, "Walking"): a fly
// session over a scene with a small terrain of the renderer's own waves — a ground provider in
// every configuration — switched onto a walker and off it with F, stepping it with WASD, Shift and
// Space, the same events twice to the same trajectory, the walk block of a session header, the
// title. Where the build carries physics and scene collision the walker is a character on the
// collision's heightfields; in the minimal build it follows the ground; the cases hold either.

#include "../fly_camera.h"
#include "../time_controls.h"
#include "../walk.h"

#include <core/json/json_value.h>
#include <foundation/input/input.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>

using namespace engine;

namespace {

constexpr u32 k_key_w = 26;
constexpr u32 k_key_s = 22;
constexpr u32 k_key_f = 9;
constexpr u32 k_key_space = 44;
constexpr u32 k_key_lshift = 225;
#if ENGINE_VIEW_WALK_PHYSICS
constexpr const char* k_collision = "physics";
// The character keeps the backend's 2 cm padding under its feet and sits a capsule's round bottom
// above the point under its centre on a slope; the ground follower is on the ground to the bit.
constexpr f32 k_feet_slack = 0.05f;
#else
constexpr const char* k_collision = "ground-follow";
constexpr f32 k_feet_slack = 1.0e-4f;
#endif

input::RawEvent key(u64 tick, u32 code, bool down) {
  return input::RawEvent{SimTick{tick}, input::Source::Key, code, down ? 1.0f : 0.0f, 0};
}

// A 120 m terrain of the renderer's own waves (the default ground, in every configuration), 1 m
// high: gentle slopes a walker stands on and follows.
renderer::SceneData waves_scene() {
  renderer::SceneData scene;
  scene.terrain.enabled = true;
  scene.terrain.size = 65;
  scene.terrain.extent = 60.0f;
  scene.terrain.seed = 4;
  scene.terrain.dune_height = 1.0f;
  return scene;
}

view::SessionHeader walk_header() {
  view::SessionHeader h;
  h.start.position = Vec3{2.0f, 20.0f, 3.0f};
  h.start.yaw = 0.0f;
  h.start.pitch = -0.3f;
  h.params.tick_hz = 240;
  h.params.speed = 4.0f;
  h.has_walk = true;
  h.walk = view::WalkParams{};
  return h;
}

struct Walked {
  u64 hash = 0;
  u64 walker_hash = 0;
  view::FlyState last;
  u32 mode_changes = 0;
  bool walking = false;
  f32 max_error = 0.0f;
};

// The session over `events` to `ticks`, a tick at a time, calling `each` after every tick.
template <typename Each>
Walked walk_session(const view::SessionHeader& header, const input::ActionMap& map,
                    std::span<const input::RawEvent> events, u64 ticks, Each&& each) {
  const renderer::SceneData scene = waves_scene();
  view::Walker walker;
  std::string error;
  REQUIRE_MESSAGE(walker.start(header.walk, header.params.tick_hz, scene, &error), error);
  REQUIRE(walker.available());
  CHECK(std::string(walker.collision()) == k_collision);
  view::FlySession session;
  session.set_walker(&walker);
  REQUIRE_MESSAGE(session.start(map, header, &error), error);
  u32 cursor = 0;
  for (u64 t = 1; t <= ticks; ++t) {
    cursor = session.run(events, cursor, SimTick{t});
    each(session, walker);
  }
  Walked out;
  out.hash = session.trajectory().hash;
  out.walker_hash = walker.stats().hash;
  out.last = session.state();
  out.mode_changes = session.mode_changes();
  out.walking = session.walking();
  out.max_error = walker.stats().max_ground_error_m;
  return out;
}

f32 ground_under(f32 x, f32 z) {
  const renderer::SceneData scene = waves_scene();
  const renderer::TerrainSampler sampler(scene.terrain);
  return sampler.height(x, z);
}

}  // namespace

TEST_CASE("walk: F walks onto the ground under the camera, and F again flies from where it is") {
  const input::ActionMap map = view::default_fly_map();
  const input::RawEvent events[] = {
      key(5, k_key_f, true),    key(7, k_key_f, false),    // onto the ground
      key(20, k_key_w, true),   key(260, k_key_w, false),  // a second north at a walk
      key(270, k_key_w, true),  key(271, k_key_lshift, true),
      key(390, k_key_w, false), key(390, k_key_lshift, false),  // half a second's sprint
      key(400, k_key_f, true),  key(402, k_key_f, false),       // back into the air
      key(420, k_key_w, true),  key(540, k_key_w, false),       // and a half second's flight
  };
  const view::SessionHeader header = walk_header();
  f32 worst_feet = 0.0f;
  Vec3 at_walk_end{};
  f32 walked_z = 0.0f;
  const Walked a =
      walk_session(header, map, events, 560, [&](const view::FlySession& s, const view::Walker& w) {
        const u64 t = s.tick().value;
        if (t >= 5 && t < 400) {
          REQUIRE(s.walking());
          const Vec3 eye = s.state().position;
          const f32 feet = eye.y - w.params().eye_height;
          worst_feet = std::max(worst_feet, std::fabs(feet - ground_under(eye.x, eye.z)));
        }
        if (t == 260) walked_z = s.state().position.z;
        if (t == 399) at_walk_end = s.state().position;
      });
  MESSAGE("walked: feet at most " << worst_feet << " m off the ground, a second's walk to z "
                                  << walked_z << ", the sprint to z " << at_walk_end.z
                                  << "; the walker's ground error " << a.max_error << " m");
  CHECK(worst_feet <= k_feet_slack);
  // North is -z at yaw 0: a second at 1.5 m/s, and then half a second at 5 m/s.
  CHECK(walked_z < 3.0f - 1.3f);
  CHECK(walked_z > 3.0f - 1.7f);
  CHECK(at_walk_end.z < walked_z - 2.0f);
  // Flying again: the camera flew on from where the walker stood, along its pitched look (down).
  CHECK_FALSE(a.walking);
  CHECK(a.mode_changes == 2);
  CHECK(a.last.position.y < at_walk_end.y);
  CHECK(a.last.position.z < at_walk_end.z - 1.0f);
  // The ground the walker stood on against the ground as drawn (the scene's grid): centimetres.
  CHECK(a.max_error < 0.1f);

  // The same events again: the same camera at every tick, and the same walker.
  const Walked b = walk_session(header, map, events, 560, [](const auto&, const auto&) {});
  CHECK(a.hash == b.hash);
  CHECK(a.walker_hash == b.walker_hash);
}

TEST_CASE("walk: --walk starts on the ground, and a jump rises by v^2 / 2g and lands") {
  view::SessionHeader header = walk_header();
  header.walking = true;
  const input::ActionMap map = view::default_fly_map();
  const input::RawEvent events[] = {key(30, k_key_space, true), key(32, k_key_space, false)};
  f32 standing = 0.0f;
  f32 top = -1.0e9f;
  u64 landed = 0;
  const Walked w =
      walk_session(header, map, events, 300, [&](const view::FlySession& s, const view::Walker&) {
        const u64 t = s.tick().value;
        if (t == 29) standing = s.state().position.y;
        if (t >= 30) top = std::max(top, s.state().position.y);
        if (t > 60 && landed == 0 && std::fabs(s.state().position.y - standing) < 0.03f) landed = t;
      });
  CHECK(w.walking);
  CHECK(w.mode_changes == 0);  // it started walking; nothing switched
  const f32 feet = standing - header.walk.eye_height;
  CHECK(std::fabs(feet - ground_under(2.0f, 3.0f)) <= k_feet_slack);
  const f32 expected =
      header.walk.jump_speed * header.walk.jump_speed / (2.0f * header.walk.gravity);
  MESSAGE("jumped " << top - standing << " m (v^2/2g " << expected << "), back at tick " << landed);
  CHECK(std::fabs((top - standing) - expected) < 0.05f);
  CHECK(landed > 30 + 180);  // 2v/g = 0.82 s, 196 ticks
  CHECK(landed < 30 + 215);
}

TEST_CASE("walk: a session whose map has no walk action flies as it always did") {
  // Revision 2 of the map has no `walk`: F presses nothing, so a session recorded against it
  // replays to exactly the trajectory it had before the walk mode existed.
  const input::ActionMap second = view::default_fly_map(2);
  const input::RawEvent with_f[] = {key(5, k_key_f, true), key(7, k_key_f, false),
                                    key(20, k_key_w, true), key(200, k_key_w, false)};
  const input::RawEvent without_f[] = {key(20, k_key_w, true), key(200, k_key_w, false)};
  const Walked a =
      walk_session(walk_header(), second, with_f, 240, [](const auto&, const auto&) {});
  const Walked b =
      walk_session(walk_header(), second, without_f, 240, [](const auto&, const auto&) {});
  CHECK_FALSE(a.walking);
  CHECK(a.mode_changes == 0);
  CHECK(a.hash == b.hash);

  // And with no walker, or one over a scene with no terrain, F switches nothing either.
  const input::ActionMap map = view::default_fly_map();
  view::FlySession session;
  std::string error;
  REQUIRE(session.start(map, walk_header(), &error));
  (void)session.run(std::span<const input::RawEvent>(with_f), 0, SimTick{240});
  CHECK_FALSE(session.walking());
  renderer::SceneData bare;
  view::Walker walker;
  REQUIRE(walker.start(view::WalkParams{}, 240, bare, &error));
  CHECK_FALSE(walker.available());
  CHECK(walker.why().find("no terrain") != std::string::npos);
  view::FlySession grounded;
  grounded.set_walker(&walker);
  REQUIRE(grounded.start(map, walk_header(), &error));
  (void)grounded.run(std::span<const input::RawEvent>(with_f), 0, SimTick{240});
  CHECK_FALSE(grounded.walking());
  CHECK(grounded.mode_changes() == 0);
}

TEST_CASE("walk: a session header's walk block reads back, and another walk version is refused") {
  view::SessionHeader h = walk_header();
  h.walking = true;
  h.walk.speed = 2.25f;
  h.walk.max_slope_deg = 33.0f;
  h.walk.spacing_m = 0.5f;
  const JsonValue json = view::session_to_json(h);
  view::SessionHeader back;
  std::string error;
  REQUIRE_MESSAGE(view::session_from_json(json, back, &error), error);
  CHECK(back.has_walk);
  CHECK(back.walking);
  CHECK(back.walk.speed == 2.25f);
  CHECK(back.walk.max_slope_deg == 33.0f);
  CHECK(back.walk.spacing_m == 0.5f);
  CHECK(back.walk.eye_height == h.walk.eye_height);

  // A header from before the walk mode has no block, and reads as a session that starts flying.
  view::SessionHeader old = walk_header();
  old.has_walk = false;
  const JsonValue old_json = view::session_to_json(old);
  CHECK(old_json.find("walk") == nullptr);
  REQUIRE(view::session_from_json(old_json, back, &error));
  CHECK_FALSE(back.has_walk);
  CHECK_FALSE(back.walking);

  // A block from another walk integration would walk somewhere else: refused, saying so.
  JsonValue future = json;
  JsonValue walk = *future.find("walk");
  walk.set("version", static_cast<u64>(view::k_walk_version + 1));
  future.set("walk", std::move(walk));
  CHECK_FALSE(view::session_from_json(future, back, &error));
  CHECK(error.find("walk integration version") != std::string::npos);
}

TEST_CASE("walk: the title says whether the camera flies or walks, and on what") {
  view::TitleStatus status;
  status.dunes = false;
  status.sun_rate = 0.0;
  status.live = false;
  char text[160];
  status.mode = view::TitleMode::walking;
  (void)view::format_status(status, text, sizeof(text));
  CHECK(std::string(text) ==
        "walking \xC2\xB7 sun \xC3\x97"
        "0");
  status.mode = view::TitleMode::walking_ground;
  (void)view::format_status(status, text, sizeof(text));
  CHECK(std::string(text).rfind("walking (ground only) \xC2\xB7 ", 0) == 0);
  status.mode = view::TitleMode::flying;
  (void)view::format_status(status, text, sizeof(text));
  CHECK(std::string(text).rfind("flying \xC2\xB7 sun", 0) == 0);
}

TEST_CASE("walk: walking back and forth over the same ground is the same walk each time") {
  // Out and back, twice, in one session: the walker's hash chain differs between the legs (it
  // walked them at different ticks), but the feet at the two turning points are where the ground
  // puts them, and the whole session is a function of its events (two runs, one hash).
  const input::ActionMap map = view::default_fly_map();
  view::SessionHeader header = walk_header();
  header.walking = true;
  const input::RawEvent events[] = {key(10, k_key_w, true), key(490, k_key_w, false),
                                    key(500, k_key_s, true), key(980, k_key_s, false)};
  Vec3 out{};
  Vec3 back{};
  Vec3 start{};
  const Walked a =
      walk_session(header, map, events, 1000, [&](const view::FlySession& s, const view::Walker&) {
        if (s.tick().value == 1) start = s.state().position;
        if (s.tick().value == 490) out = s.state().position;
        if (s.tick().value == 1000) back = s.state().position;
      });
  MESSAGE("out to " << out.z << " and back to " << back.z << " from " << start.z);
  CHECK(out.z < start.z - 2.5f);
  CHECK(std::fabs(back.z - start.z) < 0.1f);
  CHECK(std::fabs(back.y - start.y) < 0.05f);
  const Walked b = walk_session(header, map, events, 1000, [](const auto&, const auto&) {});
  CHECK(a.hash == b.hash);
}

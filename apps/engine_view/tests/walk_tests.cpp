// The walk mode with no GPU and no window (walk.h; docs/subsystems/apps.md, "Walking"): a fly
// session over a scene with a small terrain of the renderer's own waves — a ground provider in
// every configuration — switched onto a walker and off it with F, stepping it with WASD, Shift and
// Space, the same events twice to the same trajectory, the walk block of a session header, the
// title. Where the build carries physics and scene collision the walker is a character on the
// collision's heightfields; in the minimal build it follows the ground; the cases hold either.

#include "../crouch_pose.h"
#include "../fly_camera.h"
#include "../time_controls.h"
#include "../walk.h"

#include <core/json/json_value.h>
#include <domain/geometry/cluster_lod.h>
#include <foundation/input/input.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_time.h>

#include <doctest/doctest.h>
#include <test_paths.h>

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
  h.start.position = WorldPos{2.0, 20.0, 3.0};
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

// The waves by the origin, where a float32 metre is the provider's own argument.
f64 ground_under(f64 x, f64 z) {
  const renderer::SceneData scene = waves_scene();
  const renderer::TerrainSampler sampler(scene.terrain);
  return static_cast<f64>(sampler.height(static_cast<f32>(x), static_cast<f32>(z)));
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
  f64 worst_feet = 0.0;
  WorldPos at_walk_end{};
  f64 walked_z = 0.0;
  const Walked a =
      walk_session(header, map, events, 560, [&](const view::FlySession& s, const view::Walker& w) {
        const u64 t = s.tick().value;
        if (t >= 5 && t < 400) {
          REQUIRE(s.walking());
          const WorldPos eye = s.state().position;
          const f64 feet = eye.y - static_cast<f64>(w.params().eye_height);
          worst_feet = std::max(worst_feet, std::fabs(feet - ground_under(eye.x, eye.z)));
        }
        if (t == 260) walked_z = s.state().position.z;
        if (t == 399) at_walk_end = s.state().position;
      });
  MESSAGE("walked: feet at most " << worst_feet << " m off the ground, a second's walk to z "
                                  << walked_z << ", the sprint to z " << at_walk_end.z
                                  << "; the walker's ground error " << a.max_error << " m");
  CHECK(worst_feet <= static_cast<f64>(k_feet_slack));
  // North is -z at yaw 0: a second at 1.5 m/s, and then half a second at 5 m/s.
  CHECK(walked_z < 3.0 - 1.3);
  CHECK(walked_z > 3.0 - 1.7);
  CHECK(at_walk_end.z < walked_z - 2.0);
  // Flying again: the camera flew on from where the walker stood, along its pitched look (down).
  CHECK_FALSE(a.walking);
  CHECK(a.mode_changes == 2);
  CHECK(a.last.position.y < at_walk_end.y);
  CHECK(a.last.position.z < at_walk_end.z - 1.0);
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
  f64 standing = 0.0;
  f64 top = -1.0e9;
  u64 landed = 0;
  const Walked w =
      walk_session(header, map, events, 300, [&](const view::FlySession& s, const view::Walker&) {
        const u64 t = s.tick().value;
        if (t == 29) standing = s.state().position.y;
        if (t >= 30) top = std::max(top, s.state().position.y);
        if (t > 60 && landed == 0 && std::fabs(s.state().position.y - standing) < 0.03) landed = t;
      });
  CHECK(w.walking);
  CHECK(w.mode_changes == 0);  // it started walking; nothing switched
  const f64 feet = standing - static_cast<f64>(header.walk.eye_height);
  CHECK(std::fabs(feet - ground_under(2.0, 3.0)) <= static_cast<f64>(k_feet_slack));
  const f64 expected = static_cast<f64>(header.walk.jump_speed * header.walk.jump_speed /
                                        (2.0f * header.walk.gravity));
  MESSAGE("jumped " << top - standing << " m (v^2/2g " << expected << "), back at tick " << landed);
  CHECK(std::fabs((top - standing) - expected) < 0.05);
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
  // So is one recorded before the doors (S91, walk version 3): a mesh prop's body twelve ticks
  // after it appears, declared doors as kinematic bodies, E opening them. Version 2 walked a
  // house's leaf as part of its wall and a prop's body at once, and version 1 before it.
  for (const u64 before_doors : {u64{2}, u64{1}}) {
    CAPTURE(before_doors);
    JsonValue past = json;
    JsonValue old_walk = *past.find("walk");
    old_walk.set("version", before_doors);
    past.set("walk", std::move(old_walk));
    error.clear();
    CHECK_FALSE(view::session_from_json(past, back, &error));
    CHECK(error.find("walk integration version") != std::string::npos);
  }

  // The crouch (P54) reads back; a block from before it, which has none, is a walker that cannot
  // crouch; a crouch missing a number is refused.
  CHECK(back.walk.crouch_height == h.walk.crouch_height);
  REQUIRE(view::session_from_json(json, back, &error));
  CHECK(back.walk.crouch_height == 1.25f);
  CHECK(back.walk.crouch_eye_height == 1.1f);
  CHECK(back.walk.crouch_speed == 1.0f);
  CHECK(back.walk.crouch_time_s == 0.25f);
  const JsonValue& block = *json.find("walk");
  JsonValue before = JsonValue::object();
  for (const char* k : {"version", "height", "radius", "eye_height", "speed", "sprint",
                        "max_slope_deg", "step_height", "gravity", "jump_speed", "collision"})
    before.set(k, *block.find(k));
  view::WalkParams p;
  REQUIRE_MESSAGE(view::walk_params_from_json(before, p, &error), error);
  CHECK(p.crouch_height == 0.0f);
  JsonValue torn = block;
  JsonValue crouch = JsonValue::object();
  crouch.set("height", JsonValue(1.2f));
  torn.set("crouch", std::move(crouch));
  CHECK_FALSE(view::walk_params_from_json(torn, p, &error));
  CHECK(error.find("crouch") != std::string::npos);
}

// **The crouch layer's pose** (crouch_pose.h; apps.md "The crouch layer"): on a body of its own in
// centimetres — hips at 95, knees at 50 a little ahead, ankles at 8, the trunk's pivot at 105, the
// eye at 165 and 8 ahead — a full crouch's 0.55 m: the turns put each ankle back where it stood
// once the shift is added, the eye goes down by the 0.55 asked, the sole stays level (the leg's
// three turns are the identity together), the knee bends forward, and a quarter of the way bends
// less.
TEST_CASE("walk: the crouch layer keeps the feet planted and lowers the eye by what is asked") {
  view::CrouchRig rig;
  rig.layer = true;
  rig.scale = 0.01f;
  rig.trunk = 30.0f * 0.0174532925f;
  rig.rest[0] = Vec3{0.0f, 105.0f, 0.0f};
  for (u32 leg = 0; leg < 2; ++leg) {
    const f32 x = leg == 0 ? 10.0f : -10.0f;
    rig.rest[1 + 3 * leg] = Vec3{x, 95.0f, 0.0f};
    rig.rest[2 + 3 * leg] = Vec3{x, 50.0f, 2.0f};
    rig.rest[3 + 3 * leg] = Vec3{x, 8.0f, 0.0f};
  }
  for (i32 i = 0; i < static_cast<i32>(view::k_crouch_pivots); ++i)
    rig.joints[i] = i;
  rig.eye = Vec3{0.0f, 165.0f, 8.0f};
  view::CrouchPose pose;
  view::crouch_pose(rig, 0.55f, 1.0f, pose);
  REQUIRE(pose.on);
  const auto about = [](Quat q, Vec3 p, Vec3 pivot) { return rotate(q, p - pivot) + pivot; };
  for (u32 leg = 0; leg < 2; ++leg) {
    const Vec3 h = rig.rest[1 + 3 * leg];
    const Quat hip = pose.turns[1 + 3 * leg];
    const Quat knee = pose.turns[2 + 3 * leg];
    const Quat foot = pose.turns[3 + 3 * leg];
    const Vec3 k = about(hip, rig.rest[2 + 3 * leg], h);
    const Vec3 a = about(knee, about(hip, rig.rest[3 + 3 * leg], h), k);
    const Vec3 planted = a + pose.shift;
    CHECK(std::fabs(planted.y - rig.rest[3 + 3 * leg].y) < 1e-3f);
    CHECK(std::fabs(planted.z - rig.rest[3 + 3 * leg].z) < 1e-3f);
    CHECK(k.z > rig.rest[2 + 3 * leg].z + 5.0f);  // the knee goes forward
    const Quat sole = foot * knee * hip;
    CHECK(std::fabs(sole.x) < 1e-5f);
    CHECK(std::fabs(std::fabs(sole.w) - 1.0f) < 1e-5f);
  }
  const Vec3 eye = about(pose.turns[0], rig.eye, rig.rest[0]) + pose.shift;
  MESSAGE("full crouch: the eye " << (rig.eye.y - eye.y) * rig.scale << " m down, the hips "
                                  << pose.pelvis_drop_m << " m, the knee bent "
                                  << pose.knee_rad * 57.29578f << " degrees");
  CHECK((rig.eye.y - eye.y) * rig.scale == doctest::Approx(0.55f).epsilon(1e-3));
  CHECK(pose.eye_drop_m == doctest::Approx(0.55f).epsilon(1e-3));
  CHECK(eye.z < rig.eye.z + 30.0f);  // the head kept over the feet: the hips went back
  view::CrouchPose quarter;
  view::crouch_pose(rig, 0.55f * 0.25f, 0.25f, quarter);
  REQUIRE(quarter.on);
  CHECK(quarter.knee_rad > 0.0f);
  CHECK(quarter.knee_rad < pose.knee_rad);
  view::CrouchPose standing;
  view::crouch_pose(rig, 0.0f, 0.0f, standing);
  CHECK_FALSE(standing.on);
  rig.layer = false;
  view::crouch_pose(rig, 0.55f, 1.0f, standing);
  CHECK_FALSE(standing.on);
}

// **Crouching** (P54; apps.md "Crouching"): C held while walking lowers the eye from 1.65 m to
// 1.1 m over a quarter second, a fixed step a tick, walks at the crouch's 1 m/s with Shift held,
// and letting go raises the eye again; the same events twice are the same trajectory, and a log
// recorded against the map's sixth revision, which has no `crouch`, walks exactly as if C had
// never been pressed. (Standing refused under a ceiling is the character's: physics'
// character_tests.cpp, "a crouch passes under a low slab".)
TEST_CASE(
    "walk: C crouches the walker, its eye eased down and up, and a replay crouches the same") {
  constexpr u32 k_key_c = 6;
  const input::RawEvent events[] = {
      key(5, k_key_f, true),       key(7, k_key_f, false),         // onto the ground
      key(20, k_key_c, true),      key(20, k_key_w, true),         // crouch and walk north
      key(20, k_key_lshift, true),                                 // sprint held: ignored crouched
      key(500, k_key_c, false),    key(500, k_key_lshift, false),  // stand, walking on
      key(740, k_key_w, false),
  };
  const view::SessionHeader header = walk_header();
  f32 eye_at_eased = 0.0f;
  f32 eye_before = 0.0f;
  f32 eye_after = 0.0f;
  f64 z_start = 0.0;
  f64 z_crouched = 0.0;
  bool crouched_mid = false;
  f32 amount_mid = 0.0f;
  const Walked a = walk_session(
      header, view::default_fly_map(), std::span<const input::RawEvent>(events, std::size(events)),
      760, [&](const view::FlySession& session, const view::Walker& walker) {
        const u64 t = session.tick().value;
        if (t == 19) eye_before = walker.eye_height();
        if (t == 20 + 59) eye_at_eased = walker.eye_height();  // 60 ticks: a quarter second
        if (t == 140) z_start = walker.feet().z;
        if (t == 380) {
          z_crouched = walker.feet().z;
          crouched_mid = walker.crouched();
          amount_mid = walker.crouch_amount();
        }
        if (t == 760) eye_after = walker.eye_height();
      });
  CHECK(a.walking);
  CHECK(eye_before == 1.65f);
  CHECK(eye_at_eased == doctest::Approx(1.1f).epsilon(1e-4));
  CHECK(crouched_mid);
  CHECK(amount_mid == 1.0f);
  // A second (240 ticks) crouched, north, Shift held: the crouch's 1 m/s, not the sprint's 5.
  const f64 crouched_m = z_start - z_crouched;
  MESSAGE("crouched: " << crouched_m << " m north in a second, the eye " << eye_at_eased);
  CHECK(crouched_m == doctest::Approx(1.0).epsilon(0.03));
  CHECK(eye_after == 1.65f);
  const Walked b = walk_session(header, view::default_fly_map(),
                                std::span<const input::RawEvent>(events, std::size(events)), 760,
                                [](const view::FlySession&, const view::Walker&) {});
  CHECK(a.hash == b.hash);
  CHECK(a.walker_hash == b.walker_hash);

  // The sixth revision's map has no `crouch`: its C presses nothing, and the walk is the one
  // without them, to the bit.
  const input::RawEvent plain[] = {
      key(5, k_key_f, true),       key(7, k_key_f, false),        key(20, k_key_w, true),
      key(20, k_key_lshift, true), key(500, k_key_lshift, false), key(740, k_key_w, false),
  };
  const Walked sixth = walk_session(header, view::default_fly_map(6),
                                    std::span<const input::RawEvent>(events, std::size(events)),
                                    760, [](const view::FlySession&, const view::Walker&) {});
  const Walked none = walk_session(header, view::default_fly_map(6),
                                   std::span<const input::RawEvent>(plain, std::size(plain)), 760,
                                   [](const view::FlySession&, const view::Walker&) {});
  CHECK(sixth.hash == none.hash);
  CHECK(sixth.walker_hash == none.walker_hash);
  CHECK(sixth.hash != a.hash);
}

TEST_CASE("walk: the title says whether the camera flies or walks, and on what") {
  view::TitleStatus status;
  status.sun_rate = 0.0;
  status.live = false;
  char text[160];
  status.mode = view::TitleMode::walking;
  (void)view::format_status(status, text, sizeof(text));
  CHECK(std::string(text) ==
        "walking \xC2\xB7 day still \xC2\xB7 world \xC3\x97"
        "1");
  status.mode = view::TitleMode::walking_ground;
  (void)view::format_status(status, text, sizeof(text));
  CHECK(std::string(text).rfind("walking (ground only) \xC2\xB7 ", 0) == 0);
  status.mode = view::TitleMode::flying;
  (void)view::format_status(status, text, sizeof(text));
  CHECK(std::string(text).rfind("flying \xC2\xB7 day", 0) == 0);
  // The camera behind the walker (P53): `third person` after the mode, before the hour.
  status.mode = view::TitleMode::walking;
  status.third_person = true;
  (void)view::format_status(status, text, sizeof(text));
  CHECK(std::string(text) ==
        "walking \xC2\xB7 third person \xC2\xB7 day still \xC2\xB7 world \xC3\x97"
        "1");
  status.has_hour = true;
  status.hour = 7.5;
  (void)view::format_status(status, text, sizeof(text));
  CHECK(std::string(text).rfind("walking \xC2\xB7 third person \xC2\xB7 07:30 \xC2\xB7 day", 0) ==
        0);
}

#if ENGINE_VIEW_TESTS_DUNES && ENGINE_VIEW_WALK_PHYSICS
TEST_CASE("walk: standing on the erg's moving sand, the eye moves with it and never steps") {
  // The owner's start on the committed erg (x = 140, z = -60; the interdune floor at the
  // mega-draa's toe) with the dunes at 600 game seconds a real second, where on 2026-09-29 his eye
  // dropped 4.7 cm at a time every 0.7-4 s while the drawn sand under it sank 0.2-1.3 mm a frame
  // (docs/experiments/walk-in-time-lapse-2026-09-29.md): the walker the window runs, handed the
  // ground as the renderer's own model draws it a frame at a time (its cadence and its blend, the
  // fields waited for) and stepped four ticks a frame with nothing pressed. Its eye moves with the
  // drawn sand under it, a frame at a time and by what the sand moved give or take the collision's
  // millimetre (scene_collision.md, "The walker goes with the ground").
  const std::string committed =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/desert-erg/scene.json",
                      "content/test-scenes/desert-erg/scene.json");
  if (!test::path_exists(committed)) {
    MESSAGE("the erg is not in this bundle");
    return;
  }
  renderer::SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(renderer::read_scene_file(committed, desc, error), error);
  renderer::SceneData scene;
  scene.terrain = desc.terrain;
  const renderer::TerrainSampler sampler(scene.terrain);
  REQUIRE(sampler.moves());
  view::Walker walker;
  REQUIRE_MESSAGE(walker.start(view::WalkParams{}, 240, scene, &error), error);
  REQUIRE(std::string(walker.collision()) == "physics");
  const scene_gen::Lattice lattice = renderer::terrain_scene_lattice(scene.terrain);
  const f64 spacing = lattice.spacing;
  const renderer::TimeLapseConfig lapse;
  renderer::TerrainBlend blend;
  blend.time_a = blend.surface_s = scene.terrain.time_s;
  renderer::TerrainNextField next;
  const auto drawn = [&]() {
    view::DrawnGround d;
    d.lattice = lattice;
    d.moving = true;
    d.time_a = blend.time_a;
    d.time_b = blend.has_b ? blend.time_b : blend.time_a;
    d.blend = blend.blend();
    return d;
  };
  walker.set_drawn(drawn());
  WorldPos eye = walker.drop(WorldPos{140.0, 60.0, -60.0});
  for (u32 tick = 0; tick < 120; ++tick)  // settled
    eye = walker.step(view::WalkInput{});
  // The drawn sand under the feet: the pair at the feet's point, blended as drawn.
  const auto sand_at = [&](f64 x, f64 z) {
    const scene_gen::Lattice point = scene_gen::ring_lattice(1);
    const i32 i = static_cast<i32>(std::llround(x * 1000.0));
    const i32 j = static_cast<i32>(std::llround(z * 1000.0));
    f32 ha = 0.0f;
    f32 hb = 0.0f;
    const view::DrawnGround d = drawn();
    REQUIRE(sampler.provider().evaluate(d.time_a, point, i, j, 1, 1, 0, 1, std::span<f32>(&ha, 1)));
    REQUIRE(sampler.provider().evaluate(d.time_b, point, i, j, 1, 1, 0, 1, std::span<f32>(&hb, 1)));
    const f32 t = static_cast<f32>(d.blend);
    return ha * (1.0f - t) + hb * t;
  };
  f64 game = scene.terrain.time_s;
  f64 last_eye = eye.y;
  f32 last_sand = sand_at(eye.x, eye.z);
  const f32 first_sand = last_sand;
  f64 max_eye_step = 0.0;
  f32 max_sand_step = 0.0f;
  f64 worst_gap = 0.0;  // the eye's step over the sand's, in one frame
  for (u32 frame = 0; frame < 1'200; ++frame) {
    game += 600.0 / 60.0;
    if (!next.ready) {
      const f64 from = blend.has_b ? blend.time_b : blend.time_a;
      next.time_s = renderer::terrain_next_time(sampler, from, spacing, lapse.fraction,
                                                lapse.min_step_s, lapse.max_step_s);
      next.delta_m = 2.0;  // at 600 the rate, not the per-frame bound, sets the pace
      next.ready = true;
    }
    (void)renderer::terrain_blend_frame(blend, game, lapse.fraction * spacing, next);
    walker.set_drawn(drawn());
    for (u32 tick = 0; tick < 4; ++tick)
      eye = walker.step(view::WalkInput{});
    const f32 sand = sand_at(eye.x, eye.z);
    max_eye_step = std::max(max_eye_step, std::fabs(eye.y - last_eye));
    max_sand_step = std::max(max_sand_step, std::fabs(sand - last_sand));
    worst_gap =
        std::max(worst_gap, std::fabs((eye.y - last_eye) - static_cast<f64>(sand - last_sand)));
    last_eye = eye.y;
    last_sand = sand;
  }
  MESSAGE("standing on the erg at 600 for 1,200 frames: the sand under the feet moved "
          << last_sand - first_sand << " m, at most " << max_sand_step
          << " m a frame; the eye at most " << max_eye_step << " m a frame, and at most "
          << worst_gap << " m more or less than the sand; the ground under the feet at most "
          << walker.stats().max_ground_error_m << " m off the drawn grid");
  CHECK(last_sand - first_sand < -0.1f);  // it sinks, as it did under the owner
  // Until 2026-09-29 the eye stood still and then dropped 4.7 cm in one frame, three times here.
  CHECK(max_eye_step < 2.0e-3);
  CHECK(worst_gap < 1.5e-3);
}
#endif

TEST_CASE("walk: walking back and forth over the same ground is the same walk each time") {
  // Out and back, twice, in one session: the walker's hash chain differs between the legs (it
  // walked them at different ticks), but the feet at the two turning points are where the ground
  // puts them, and the whole session is a function of its events (two runs, one hash).
  const input::ActionMap map = view::default_fly_map();
  view::SessionHeader header = walk_header();
  header.walking = true;
  const input::RawEvent events[] = {key(10, k_key_w, true), key(490, k_key_w, false),
                                    key(500, k_key_s, true), key(980, k_key_s, false)};
  WorldPos out{};
  WorldPos back{};
  WorldPos start{};
  const Walked a =
      walk_session(header, map, events, 1000, [&](const view::FlySession& s, const view::Walker&) {
        if (s.tick().value == 1) start = s.state().position;
        if (s.tick().value == 490) out = s.state().position;
        if (s.tick().value == 1000) back = s.state().position;
      });
  MESSAGE("out to " << out.z << " and back to " << back.z << " from " << start.z);
  CHECK(out.z < start.z - 2.5);
  CHECK(std::fabs(back.z - start.z) < 0.1);
  CHECK(std::fabs(back.y - start.y) < 0.05);
  const Walked b = walk_session(header, map, events, 1000, [](const auto&, const auto&) {});
  CHECK(a.hash == b.hash);
}

namespace {

// Water deepening eastwards from x = 2 at half a metre a metre: the chest's 1.4 m at x = 4.8.
f32 shelving(const void*, WorldPos at) noexcept {
  return std::max(0.0f, 0.5f * (static_cast<f32>(at.x) - 2.0f));
}
f32 dry(const void*, WorldPos) noexcept { return 0.0f; }

struct Waded {
  WorldPos feet{};
  view::WalkStats stats;
};

// A walker dropped at the origin of the waves, walking east for `east` ticks and then west for
// `west`, with the water `depth` (none when null).
Waded wade(view::DepthFn depth, u32 east, u32 west) {
  const renderer::SceneData scene = waves_scene();
  view::Walker walker;
  std::string error;
  REQUIRE_MESSAGE(walker.start(view::WalkParams{}, 240, scene, &error), error);
  if (depth != nullptr) walker.set_water(depth, nullptr);
  (void)walker.drop(WorldPos{0.0, 10.0, 0.0});
  view::WalkInput in;
  in.move = Vec2{0.0f, 1.0f};
  in.yaw = -1.5707963f;  // forward (-sin yaw, 0, -cos yaw): +x
  for (u32 t = 0; t < east; ++t)
    (void)walker.step(in);
  in.yaw = 1.5707963f;  // -x
  for (u32 t = 0; t < west; ++t)
    (void)walker.step(in);
  return Waded{walker.feet(), walker.stats()};
}

}  // namespace

TEST_CASE("walk: wading slows the walker by the declared curve, and the chest's water stops it") {
  // The curve: full speed to the ankle, half by the waist, a quarter at the chest and beyond.
  CHECK(view::wade_scale(0.0f) == 1.0f);
  CHECK(view::wade_scale(0.3f) == 1.0f);
  CHECK(view::wade_scale(0.65f) == doctest::Approx(0.75f));
  CHECK(view::wade_scale(1.0f) == doctest::Approx(0.5f));
  CHECK(view::wade_scale(1.2f) == doctest::Approx(0.375f));
  CHECK(view::wade_scale(1.4f) == doctest::Approx(0.25f));
  CHECK(view::wade_scale(3.0f) == 0.25f);
  // Twenty seconds east at a walk, 30 m on dry ground: into the water, slower and slower, and
  // stopped where it reaches the chest — and no further however long W is held.
  const Waded in_water = wade(&shelving, 4800, 0);
  MESSAGE("waded east to x = " << in_water.feet.x << ", " << in_water.stats.max_depth_m
                               << " m deep, " << in_water.stats.stopped_ticks << " ticks stopped");
  CHECK(in_water.feet.x > 4.7);
  CHECK(in_water.feet.x < 4.9);
  CHECK(in_water.stats.max_depth_m >= view::k_wade_chest_m);
  CHECK(in_water.stats.max_depth_m < view::k_wade_chest_m + 0.05f);
  CHECK(in_water.stats.stopped_ticks > 3000u);
  CHECK(in_water.stats.wading_ticks > in_water.stats.stopped_ticks);
  // The wall is one way: a walker at the chest wades back out.
  const Waded back = wade(&shelving, 4800, 960);
  CHECK(back.feet.x < in_water.feet.x - 0.5);
  // The same walk twice is the same walk.
  const Waded again = wade(&shelving, 4800, 960);
  CHECK(again.feet == back.feet);
  CHECK(again.stats.hash == back.stats.hash);
  // Where there is no water the walker is the one it always was, to the bit: no depth function,
  // and a depth function that answers 0 everywhere, walk the same floats.
  const Waded none = wade(nullptr, 4800, 0);
  const Waded zero = wade(&dry, 4800, 0);
  CHECK(none.feet.x > 25.0);
  CHECK(zero.feet == none.feet);
  CHECK(zero.stats.hash == none.stats.hash);
  CHECK(none.stats.wading_ticks == 0u);
}

#if ENGINE_VIEW_WALK_PHYSICS
namespace {

constexpr u32 k_key_e = 8;  // `interact` (fly_camera.cpp, revision 5)

struct DoorMesh {
  Vector<Vec3> positions;
  Vector<u32> indices;
};

// A box standing on y = y0 round (cx, cz), faces wound outwards.
void door_box(DoorMesh& m, Vec3 half, f32 cx, f32 cz) {
  const u32 base = m.positions.size();
  for (u32 k = 0; k < 8; ++k) {
    m.positions.push_back(Vec3{cx + ((k & 1) ? half.x : -half.x), (k & 2) ? 2.0f * half.y : 0.0f,
                               cz + ((k & 4) ? half.z : -half.z)});
  }
  const u32 faces[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1},
                           {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
  for (const auto& f : faces) {
    const u32 tri[6] = {f[0], f[1], f[2], f[0], f[2], f[3]};
    for (const u32 v : tri)
      m.indices.push_back(base + v);
  }
}

// The waves made flat, a wall across z = 20 with a 0.9 m doorway at x = 0 that declares its leaf
// (section 53: hung at x = -0.45 on the wall's south face, shut across the gap, swinging north),
// and the leaf drawn as the scene's instance 1 standing shut in the doorway (`SceneData::leaves`),
// which the collision must leave out: the door's box is what stops the walker.
renderer::SceneData door_scene() {
  renderer::SceneData scene = waves_scene();
  scene.terrain.dune_height = 0.0f;
  DoorMesh wall;
  door_box(wall, Vec3{2.275f, 1.0f, 0.05f}, -2.725f, 20.05f);
  door_box(wall, Vec3{2.275f, 1.0f, 0.05f}, 2.725f, 20.05f);
  DoorMesh leaf;
  door_box(leaf, Vec3{0.45f, 1.0f, 0.025f}, 0.0f, 19.975f);
  Vector<geometry::ClusterLodMesh> lods;
  std::string error;
  for (const DoorMesh* m : {&wall, &leaf}) {
    geometry::ClusterLodMesh lod;
    REQUIRE_MESSAGE(
        geometry::build_cluster_lod(std::span<const Vec3>(m->positions.data(), m->positions.size()),
                                    std::span<const u32>(m->indices.data(), m->indices.size()),
                                    geometry::ClusterLodOptions{}, lod, &error),
        error);
    lods.push_back(std::move(lod));
  }
  REQUIRE_MESSAGE(geometry::merge_cluster_meshes(
                      std::span<const geometry::ClusterLodMesh>(lods.data(), lods.size()),
                      scene.lod, scene.parts, &error),
                  error);
  scene.mesh_fit.resize(scene.parts.size(), Mat4::identity());
  scene.terrain_mesh = ~0u;
  u32 first = 0;
  for (u32 mesh = 0; mesh < 2; ++mesh) {
    renderer::SceneInstance placed;
    placed.mesh = mesh;
    gfx::InstanceDesc desc;
    REQUIRE(renderer::make_instance(scene, placed, first, desc, &error));
    first += scene.parts[mesh].cluster_count;
    scene.instances.push_back(desc);
  }
  scene.pair_count = first;
  scene.sources.resize(scene.parts.size());
  geometry::ClusterFileDoor r;
  r.pivot = Vec3{-0.45f, 0.0f, 20.0f};
  r.shut_x = 1.0f, r.shut_z = 0.0f;
  r.swing_x = 0.0f, r.swing_z = 1.0f;
  r.width_m = 0.9f;
  r.height_m = 2.0f;
  r.thickness_m = 0.05f;
  r.flags = geometry::k_door_flag_door;
  scene.sources[0].doors.push_back(r);
  scene.leaves.push_back(renderer::SceneLeaf{1, 0, 0, 1, 1.0f, 0});
  return scene;
}

bool refuse_doors(void* refused, const view::WalkDoor&, bool) {
  ++*static_cast<u32*>(refused);
  return false;
}

struct DoorWalk {
  f64 end_z = 0.0;
  i32 angle_mdeg = 0;
  u64 interacts = 0, refusals = 0;
  u32 moving_ticks = 0;  // ticks whose flush wrote the leaf
  u32 first_moving = 0, last_moving = 0;
  u32 stray_writes = 0;  // writes of another instance, or more than one a tick
  Quat last_rotation{};
  WorldPos last_origin{};
  WorldPos hinge{};
  u64 hash = 0;
};

// Walking north (yaw pi) from z = 16 toward the shut door with W held, E pressed against it at
// tick 600, through the session and its input log as every other walk here; the leaf's moves read
// after every tick, as engine-view's frames flush them.
DoorWalk walk_to_door(bool refuse) {
  const renderer::SceneData scene = door_scene();
  view::Walker walker;
  std::string error;
  REQUIRE_MESSAGE(walker.start(view::WalkParams{}, 240, scene, &error), error);
  REQUIRE(std::string(walker.collision()) == "physics");
  u32 refused = 0;
  if (refuse) walker.set_door_service(&refuse_doors, &refused);
  view::SessionHeader header = walk_header();
  header.start.position = WorldPos{0.0, 3.0, 16.0};
  header.start.yaw = 3.14159265f;
  header.start.pitch = 0.0f;
  header.walking = true;
  view::FlySession session;
  session.set_walker(&walker);
  const input::ActionMap map = view::default_fly_map();
  REQUIRE_MESSAGE(session.start(map, header, &error), error);
  const input::RawEvent events[] = {key(10, k_key_w, true), key(600, k_key_e, true),
                                    key(602, k_key_e, false), key(1400, k_key_w, false)};
  DoorWalk out;
  u32 cursor = 0;
  for (u64 t = 1; t <= 1500; ++t) {
    cursor = session.run(events, cursor, SimTick{t});
    std::span<const u32> indices;
    std::span<const renderer::SceneInstance> placements;
    walker.leaf_moves(indices, placements);
    if (indices.empty()) continue;
    if (indices.size() != 1 || indices[0] != 1 || placements[0].mesh != 1) ++out.stray_writes;
    if (out.moving_ticks++ == 0) out.first_moving = static_cast<u32>(t);
    out.last_moving = static_cast<u32>(t);
    out.last_rotation = placements[0].transform.rotation;
    out.last_origin = placements[0].origin;
  }
  REQUIRE(walker.door_count() == 1);
  view::WalkDoor door;
  REQUIRE(walker.door(0, door));
  out.end_z = walker.feet().z;
  out.angle_mdeg = door.angle_mdeg;
  out.hinge = door.hinge;
  out.interacts = walker.door_interacts();
  out.refusals = walker.door_refusals();
  CHECK(refused == out.refusals);
  out.hash = session.trajectory().hash;
  return out;
}

}  // namespace

// **The walker's interact opens the door it faces, and the leaf is drawn as it turns** (S91;
// scene_collision.md, "What is drawn swings with what collides"; apps.md, "Doors a module answers
// for"): a walk at the shut door stops at its face; E against it opens it, the leaf's instance is
// written on each tick the door turns and on no other, standing at the door's pose, and the walker
// goes through. A game's door service that answers no keeps it shut and writes nothing.
TEST_CASE(
    "walk: the interact opens the door in front, its leaf drawn as it turns, and a no holds it") {
  const DoorWalk open = walk_to_door(false);
  MESSAGE("opened: the walker ended at z " << open.end_z << ", the leaf written on "
                                           << open.moving_ticks << " ticks, " << open.first_moving
                                           << " to " << open.last_moving);
  CHECK(open.interacts == 1);
  CHECK(open.refusals == 0);
  CHECK(open.angle_mdeg == -90000);  // a quarter turn toward +z
  CHECK(open.end_z > 21.0);          // through the doorway
  CHECK(open.stray_writes == 0);
  // 375 millidegrees a tick: 240 ticks from shut to open, every one a write and none after.
  CHECK(open.moving_ticks == 240);
  CHECK(open.first_moving >= 600);
  CHECK(open.last_moving == open.first_moving + 239);
  // At the end it stands where the door's box does: its hinge, a quarter turn about +y.
  CHECK(std::fabs(open.last_rotation.y + 0.70710678f) < 1e-5f);
  CHECK(std::fabs(open.last_rotation.w - 0.70710678f) < 1e-5f);
  CHECK(length(Vec3{relative(open.last_origin, open.hinge)}) < 1e-6f);

  const DoorWalk held = walk_to_door(true);
  MESSAGE("refused: the walker ended at z " << held.end_z);
  CHECK(held.interacts == 0);
  CHECK(held.refusals == 1);
  CHECK(held.angle_mdeg == 0);
  CHECK(held.end_z < 19.8);  // stopped at the shut leaf's face (19.95) less its radius
  CHECK(held.moving_ticks == 0);
  // The same walk again is the same walk.
  CHECK(walk_to_door(false).hash == open.hash);
}
#endif

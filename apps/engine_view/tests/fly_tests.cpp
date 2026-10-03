// The interactive camera with no GPU and no window: the bindings, the fixed-tick integration and
// its sign conventions, the conversion at the window's edge, the pacing numbers, and — the case
// that matters most — the committed synthetic session and its committed trajectory.
//
// **Why the trajectory is the GPU-free half of the replay test.** A replay's promise is about the
// camera: the same ticks, the same events, the same floats. Pictures follow from the camera, but
// comparing pictures needs a device, and most of the machines CI runs on have none — so the
// promise would go unchecked exactly where most commits are built. The trajectory hash is the
// camera itself, bit for bit, at every tick; it needs a CPU and nothing else, and it is compared
// here against a file committed from Windows, so GCC, Clang and both CPU baselines each prove on
// every build that they fly the recording to the same place. The pictures are checked too, on a
// machine that can draw them (interactive_tests.cpp).
//
// **Nobody here can move a mouse in CI**, so the fixture is synthetic: `make_synthetic_session`
// below scripts key presses and pointer motion over 480 ticks, and the committed file is what it
// writes — the first case checks that, byte for byte, so the fixture can always be regenerated
// from code rather than trusted as an artefact.
#include "../fly_camera.h"
#include "../pacing.h"
#include "../window_input.h"

#include <core/json/json.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <bit>
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
constexpr u32 k_key_m = 16;
constexpr u32 k_key_escape = 41;
constexpr u32 k_key_lshift = 225;
constexpr u32 k_key_lalt = 226;
constexpr u32 k_key_up = 82;  // not bound: what the rebound map moves forward onto

std::string content_path(const char* relative) {
  return test::data_path(std::string(ENGINE_SOURCE_DIR "/") + relative, relative);
}

std::string read_text(const std::string& path) {
  std::string text;
  if (io::read_file(path, text) != io::Status::Ok) return {};
  return text;
}

// ---- the synthetic session ------------------------------------------------------------------

input::RawEvent key(u64 tick, u32 code, bool down) {
  return input::RawEvent{SimTick{tick}, input::Source::Key, code, down ? 1.0f : 0.0f, 0};
}

input::RawEvent pointer(u64 tick, input::MouseAxisCode axis, f32 pixels) {
  return input::RawEvent{SimTick{tick}, input::Source::MouseAxis, static_cast<u32>(axis), pixels,
                         0};
}

// The header the fixture carries: the procedural heightfield at a small grid (it loads in a
// fraction of a second in a debug build and needs no sample assets), a camera 22 m south of it
// looking north and down, and the default parameters at a speed chosen for a 20 m scene.
view::SessionHeader synthetic_header() {
  view::SessionHeader h;
  h.recorded_by = "synthetic: apps/engine_view/tests/fly_tests.cpp make_synthetic_session";
  h.engine_commit = "synthetic";
  h.procedural = "heightfield";
  h.grid = 65;
  h.start.position = Vec3{0.0f, 8.0f, 22.0f};
  h.start.yaw = 0.0f;
  h.start.pitch = -0.3f;
  h.fov_y = 0.9599310886f;
  h.znear = 0.05f;
  h.params.tick_hz = 240;
  h.params.speed = 4.0f;
  h.params.fast = 4.0f;
  h.params.slow = 0.25f;
  h.params.look = 0.0022f;
  h.params.turn_rate = 2.5f;
  h.params.pitch_limit = 1.553343f;
  h.ticks = 480;
  return h;
}

// Two seconds at 240 Hz of everything the camera reads from a keyboard and a mouse: Escape to take
// the pointer, forward with a turn, a strafe with fast held, a climb while looking up, a slow
// reverse, a descent while strafing and looking down, a pull on the pointer far past the pitch
// clamp, a diagonal of two keys, and five markers. Pointer motion is one event per axis per tick,
// which is what the window's edge writes (window_input.h).
input::InputLog make_synthetic_session() {
  Vector<input::RawEvent> events;
  auto press = [&](u64 at, u32 code) { events.push_back(key(at, code, true)); };
  auto release = [&](u64 at, u32 code) { events.push_back(key(at, code, false)); };
  for (u64 t = 1; t <= 480; ++t) {
    if (t == 1) press(t, k_key_escape);
    if (t == 3) release(t, k_key_escape);
    if (t == 10) press(t, k_key_w);
    if (t == 130) release(t, k_key_w);
    if (t >= 20 && t < 80) events.push_back(pointer(t, input::MouseAxisCode::X, 4.0f));
    if (t == 60 || t == 240 || t == 420 || t == 450 || t == 475) press(t, k_key_m);
    if (t == 62 || t == 242 || t == 422 || t == 452 || t == 477) release(t, k_key_m);
    if (t == 140) press(t, k_key_d);
    if (t == 150) press(t, k_key_lshift);
    if (t == 190) release(t, k_key_lshift);
    if (t == 200) release(t, k_key_d);
    if (t == 210) press(t, k_key_e);
    if (t >= 210 && t < 250) events.push_back(pointer(t, input::MouseAxisCode::Y, -3.0f));
    if (t == 260) release(t, k_key_e);
    if (t == 270) press(t, k_key_s);
    if (t == 275) press(t, k_key_lalt);
    if (t == 320) release(t, k_key_lalt);
    if (t == 330) release(t, k_key_s);
    if (t == 340) {
      press(t, k_key_q);
      press(t, k_key_a);
    }
    if (t >= 350 && t < 380) {
      events.push_back(pointer(t, input::MouseAxisCode::X, -1.5f));
      events.push_back(pointer(t, input::MouseAxisCode::Y, 5.0f));
    }
    if (t == 400) {
      release(t, k_key_q);
      release(t, k_key_a);
    }
    if (t == 430) events.push_back(pointer(t, input::MouseAxisCode::Y, -2000.0f));
    if (t == 440) {
      press(t, k_key_w);
      press(t, k_key_d);
    }
    if (t == 470) {
      release(t, k_key_w);
      release(t, k_key_d);
    }
  }
  input::InputLog log;
  // Recorded against the map's first revision, as every session before 2026-09-27 was: the
  // fixture is also the proof that such a recording still replays (fly_camera.h, "The map's
  // revisions").
  log.set_map(view::default_fly_map(1));
  // Events of one tick in the order the lambdas above pushed them; the log only needs the ticks
  // not to go backwards, and within a tick InputState's results do not depend on the order.
  for (const input::RawEvent& e : events)
    log.record(e);
  log.set_session(view::session_to_json(synthetic_header()));
  return log;
}

// Flies a log's session in pieces of `piece` ticks, as frames would, and returns the trajectory.
view::Trajectory fly(const input::InputLog& log, const input::ActionMap& map,
                     const view::SessionHeader& header, u64 piece) {
  view::FlySession session;
  std::string error;
  REQUIRE_MESSAGE(session.start(map, header, &error), error);
  u32 cursor = 0;
  while (session.tick().value < header.ticks) {
    const u64 to =
        session.tick().value + piece < header.ticks ? session.tick().value + piece : header.ticks;
    cursor = session.run(log.events(), cursor, SimTick{to});
  }
  CHECK(cursor == log.size());
  return session.trajectory();
}

// A tick of input from `events`, into `state`, and one camera tick.
void tick(view::FlyState& state, input::InputState& input, const view::FlyActions& actions,
          const view::FlyParams& params, u64 t, std::span<const input::RawEvent> events) {
  input.begin_tick(SimTick{t});
  for (const input::RawEvent& e : events)
    input.feed(e);
  input.end_tick();
  view::fly_tick(state, input, actions, params);
}

bool near(const Vec3& a, const Vec3& b, f32 tolerance) {
  return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance &&
         std::fabs(a.z - b.z) <= tolerance;
}

}  // namespace

TEST_CASE("fly camera: the default map is the committed file, and every action it needs") {
  const input::ActionMap map = view::default_fly_map();
  view::FlyActions actions;
  std::string error;
  REQUIRE_MESSAGE(view::resolve_fly_actions(map, actions, &error), error);
  // The camera's eight, the time-lapse's four keys appended after them (revision 2), the walk
  // mode's two after those (revision 3): `walk` on F and `jump` on Space, and a sky's exposure
  // keys after those (revision 4): `-`, `=` and `0`.
  CHECK(map.action_count() == 17);
  CHECK(view::default_fly_map(3).action_count() == 14);
  CHECK(view::default_fly_map(2).action_count() == 12);
  for (u32 c = 0; c < view::k_view_controls; ++c)
    CHECK(actions.controls[c] != input::k_invalid_action);
  view::FlyActions third;
  REQUIRE(view::resolve_fly_actions(view::default_fly_map(3), third, &error));
  CHECK(third.walk != input::k_invalid_action);
  CHECK(third.controls[static_cast<u32>(view::ViewControl::exposure_hold)] ==
        input::k_invalid_action);  // a revision 3 session has no exposure keys
  CHECK(view::default_fly_map(1).action_count() == 8);
  CHECK(actions.walk != input::k_invalid_action);
  CHECK(actions.jump != input::k_invalid_action);
  view::FlyActions second;
  REQUIRE(view::resolve_fly_actions(view::default_fly_map(2), second, &error));
  CHECK(second.walk == input::k_invalid_action);  // a revision 2 session cannot walk
  CHECK(second.jump == input::k_invalid_action);
  input::ActionMap found;
  CHECK(view::default_fly_map_for(view::default_fly_map(2).hash(), found));
  CHECK(found.action_count() == 12);

  const std::string expected = write_json(map.to_json(), JsonWriteOptions{.pretty = true}) + "\n";
  const std::string path = content_path("content/input-maps/engine-view.json");
  const std::string committed = read_text(path);
  if (committed != expected) {
    const test::TempDir tmp("engine_view_map");
    (void)io::write_file(tmp.file("engine-view.json"), expected);
    tmp.keep();
    FAIL_CHECK(
        "content/input-maps/engine-view.json is not default_fly_map(); the map it should be "
        "is in "
        << tmp.path()
        << ". A changed map stops every recorded session replaying: say so where it is "
           "committed.");
  }
  // And the file, loaded, is the same bindings: the hash a recording carries.
  JsonValue value;
  REQUIRE(parse_json(committed, value).ok);
  input::ActionMap loaded;
  REQUIRE_MESSAGE(loaded.from_json(value, &error), error);
  CHECK(loaded.hash() == map.hash());

  // A map without one of the camera's actions, or with one of the wrong kind, is refused by name.
  input::ActionMap missing;
  (void)missing.add_action("move", input::ActionKind::Axis2);
  CHECK_FALSE(view::resolve_fly_actions(missing, actions, &error));
  CHECK(error.find("lift") != std::string::npos);
  input::ActionMap renamed;
  (void)renamed.add_action("move", input::ActionKind::Axis);  // an axis, not a pair
  CHECK_FALSE(view::resolve_fly_actions(renamed, actions, &error));
  CHECK(error.find("move") != std::string::npos);
}

TEST_CASE("fly camera: the trigonometry is arithmetic, close to the C library's, and symmetric") {
  // Within a few f32 ulps of the library everywhere a camera can point, exactly odd and even, and
  // exact at the quarter turns a camera starts at.
  f32 worst = 0.0f;
  for (i32 i = -4000; i <= 4000; ++i) {
    const f32 angle = static_cast<f32>(i) * 0.00157f;  // -6.28 .. 6.28
    f32 s = 0.0f;
    f32 c = 0.0f;
    view::fly_sin_cos(angle, s, c);
    const f32 ds = std::fabs(s - static_cast<f32>(std::sin(static_cast<f64>(angle))));
    const f32 dc = std::fabs(c - static_cast<f32>(std::cos(static_cast<f64>(angle))));
    worst = ds > worst ? ds : worst;
    worst = dc > worst ? dc : worst;
    f32 s_neg = 0.0f;
    f32 c_neg = 0.0f;
    view::fly_sin_cos(-angle, s_neg, c_neg);
    CHECK(s_neg == -s);
    CHECK(c_neg == c);
  }
  CHECK(worst <= 2.5e-7f);
  f32 s = 1.0f;
  f32 c = 0.0f;
  view::fly_sin_cos(0.0f, s, c);
  CHECK(s == 0.0f);
  CHECK(c == 1.0f);
  // A broken angle is the camera at rest rather than a loop that never ends.
  view::fly_sin_cos(std::bit_cast<f32>(0x7f800000u), s, c);  // +inf
  CHECK(s == 0.0f);
  CHECK(c == 1.0f);
}

TEST_CASE("fly camera: forward is the camera paths' forward, and each action moves it one way") {
  const input::ActionMap map = view::default_fly_map();
  view::FlyActions actions;
  std::string error;
  REQUIRE(view::resolve_fly_actions(map, actions, &error));

  // **The sign conventions are the camera paths'**: an orientation of quat(+y, yaw) * quat(+x,
  // pitch) looks along rotate(q, -z) — `quat_from_euler`, and `CameraKey.rotation`.
  const f32 angles[][2] = {{0.0f, 0.0f}, {0.7f, 0.2f}, {-1.3f, -0.6f}, {2.9f, 1.2f}, {-3.1f, 0.0f}};
  for (const auto& a : angles) {
    view::FlyState s;
    s.yaw = a[0];
    s.pitch = a[1];
    const Vec3 expected = rotate(quat_from_euler(a[0], a[1], 0.0f), Vec3::forward());
    CHECK_MESSAGE(near(view::fly_forward(s), expected, 2e-6f), a[0] << " " << a[1]);
    const Vec3 right = rotate(quat_from_euler(a[0], 0.0f, 0.0f), Vec3::unit_x());
    CHECK(near(view::fly_right(s), right, 2e-6f));
  }
  // And a start taken from a camera looks where the camera looked.
  renderer::Camera camera;
  camera.position = Vec3{3.0f, 4.0f, 5.0f};
  camera.target = Vec3{-2.0f, 1.0f, -7.0f};
  const view::FlyState from = view::fly_state_from_camera(camera);
  const Vec3 looked = normalize(camera.target - camera.position);
  CHECK(near(view::fly_forward(from), looked, 2e-6f));
  const renderer::Camera back = view::fly_view(from, 1.0f, 0.1f);
  CHECK(near(normalize(back.target - back.position), looked, 2e-6f));

  view::FlyParams params;
  params.tick_hz = 240;
  params.speed = 2.0f;
  auto run = [&](std::span<const input::RawEvent> hold, u32 ticks, view::FlyState start = {}) {
    input::InputState input(map);
    view::FlyState s = start;
    for (u32 t = 1; t <= ticks; ++t)
      tick(s, input, actions, params, t, t == 1 ? hold : std::span<const input::RawEvent>{});
    return s;
  };
  // W for one second at 2 m/s: two metres along -z.
  const input::RawEvent w[] = {key(1, k_key_w, true)};
  CHECK(near(run(w, 240).position, Vec3{0.0f, 0.0f, -2.0f}, 1e-4f));
  // D strafes right (+x), A left, S back, E up and Q down, whatever the pitch: lift is world up.
  const input::RawEvent d[] = {key(1, k_key_d, true)};
  CHECK(near(run(d, 240).position, Vec3{2.0f, 0.0f, 0.0f}, 1e-4f));
  const input::RawEvent e[] = {key(1, k_key_e, true)};
  view::FlyState pitched;
  pitched.pitch = 0.5f;
  CHECK(near(run(e, 240, pitched).position, Vec3{0.0f, 2.0f, 0.0f}, 1e-4f));
  const input::RawEvent q[] = {key(1, k_key_q, true)};
  CHECK(near(run(q, 240).position, Vec3{0.0f, -2.0f, 0.0f}, 1e-4f));
  // W looking up flies up: forward includes the pitch.
  CHECK(run(w, 240, pitched).position.y > 0.9f);
  // Shift is four times as fast, Alt a quarter; W and D together are not faster than W.
  const input::RawEvent fast[] = {key(1, k_key_w, true), key(1, k_key_lshift, true)};
  CHECK(near(run(fast, 240).position, Vec3{0.0f, 0.0f, -8.0f}, 1e-3f));
  const input::RawEvent slow[] = {key(1, k_key_w, true), key(1, k_key_lalt, true)};
  CHECK(near(run(slow, 240).position, Vec3{0.0f, 0.0f, -0.5f}, 1e-4f));
  const input::RawEvent diagonal[] = {key(1, k_key_w, true), key(1, k_key_d, true)};
  const Vec3 diag = run(diagonal, 240).position;
  CHECK(std::sqrt(diag.x * diag.x + diag.z * diag.z) == doctest::Approx(2.0f).epsilon(1e-4));

  // The pointer: right turns right (yaw falls, forward swings towards +x), up looks up, and a
  // pull past the pole stops at the limit.
  const input::RawEvent right[] = {pointer(1, input::MouseAxisCode::X, 100.0f)};
  const view::FlyState turned = run(right, 1);
  CHECK(turned.yaw == doctest::Approx(-100.0f * params.look));
  CHECK(view::fly_forward(turned).x > 0.0f);
  const input::RawEvent up[] = {pointer(1, input::MouseAxisCode::Y, -50.0f)};
  CHECK(run(up, 1).pitch == doctest::Approx(50.0f * params.look));
  const input::RawEvent yank[] = {pointer(1, input::MouseAxisCode::Y, -1.0e5f)};
  CHECK(run(yank, 1).pitch == params.pitch_limit);
  // Yaw stays within a turn of zero however far the pointer goes.
  const input::RawEvent spin[] = {pointer(1, input::MouseAxisCode::X, 1.0e4f)};
  const view::FlyState spun = run(spin, 1);
  CHECK(spun.yaw >= -3.1416f);
  CHECK(spun.yaw <= 3.1416f);
  // A stick turns at a rate: full right for a second is `turn_rate` radians.
  const input::RawEvent stick[] = {
      input::RawEvent{SimTick{1}, input::Source::GamepadAxis, 2, 1.0f, 0}};
  CHECK(run(stick, 240).yaw == doctest::Approx(-params.turn_rate).epsilon(1e-3));
}

TEST_CASE("fly camera: the committed synthetic session is what the helper writes") {
  const test::TempDir tmp("engine_view_fixture");
  const input::InputLog generated = make_synthetic_session();
  const std::string written = tmp.file("fly-synthetic.jsonl");
  REQUIRE(generated.save(written) == io::Status::Ok);
  const std::string committed =
      read_text(content_path("content/input-logs/sessions/fly-synthetic.jsonl"));
  if (read_text(written) != committed) {
    tmp.keep();
    FAIL_CHECK(
        "content/input-logs/sessions/fly-synthetic.jsonl is not what make_synthetic_session "
        "writes; the file it should be is "
        << written);
  }
}

TEST_CASE("fly camera: the synthetic session flies the committed trajectory, however it is cut") {
  const std::string log_path = content_path("content/input-logs/sessions/fly-synthetic.jsonl");
  input::InputLog log;
  std::string error;
  REQUIRE_MESSAGE(log.load(log_path, &error) == io::Status::Ok, error);
  view::SessionHeader header;
  REQUIRE_MESSAGE(view::session_from_json(log.session(), header, &error), error);
  CHECK(header.ticks == 480);
  CHECK(header.params.tick_hz == 240);
  // The fixture names the map's first revision, and engine-view finds that revision by the hash.
  CHECK(log.map_hash() == view::default_fly_map(1).hash());
  input::ActionMap map;
  REQUIRE(view::default_fly_map_for(log.map_hash(), map));
  const input::InputState probe(map);
  REQUIRE_MESSAGE(log.check_map(probe, &error), error);

  // One tick at a time, four (an offscreen replay's frame), and pieces no frame loop would cut:
  // the trajectory is a function of the ticks, never of how they were grouped into frames.
  const view::Trajectory one = fly(log, map, header, 1);
  CHECK(one.ticks == 480);
  CHECK(one.markers.size() == 5);
  const u64 pieces[] = {4, 7, 33, 480};
  for (const u64 piece : pieces) {
    const view::Trajectory other = fly(log, map, header, piece);
    CHECK_MESSAGE(other.hash == one.hash, "piece " << piece);
    CHECK(other.markers.size() == one.markers.size());
  }
  // The pull at tick 430 is far past the pole, so the markers after it look straight-ish up.
  REQUIRE(one.markers.size() == 5);
  CHECK(one.markers[0].tick == 60);
  CHECK(one.markers[3].state.pitch == header.params.pitch_limit);
  CHECK(one.markers[3].state.yaw != 0.0f);

  // **The committed trajectory**: the hash of every tick's camera, bit for bit, and the markers.
  const std::string expected = write_json(view::trajectory_to_json(one, header.params.tick_hz),
                                          JsonWriteOptions{.pretty = true}) +
                               "\n";
  const std::string committed =
      read_text(content_path("content/input-logs/sessions/fly-synthetic.trajectory.json"));
  if (committed != expected) {
    const test::TempDir tmp("engine_view_trajectory");
    (void)io::write_file(tmp.file("fly-synthetic.trajectory.json"), expected);
    tmp.keep();
    FAIL_CHECK(
        "the synthetic session no longer flies the committed trajectory. If the camera's "
        "integration changed on purpose, bump view::k_fly_version and commit "
        << tmp.path()
        << "/fly-synthetic.trajectory.json; if it did not, this build flies a recording "
           "somewhere else than the build that recorded it did.");
  }
}

TEST_CASE("fly camera: a live frame draws a camera on the segment between the last two ticks") {
  // A live frame samples the session between ticks and draws `camera_at(alpha)`: the camera
  // `alpha` of the way from the tick before the last to the last, so its motion is the frame's own
  // time rather than the whole ticks that fell due (docs/subsystems/apps.md, "Drawing between
  // ticks"). Every tick of the synthetic session — which turns, strafes, climbs, crosses nothing
  // but the pitch clamp — is checked at five fractions, and the ticks are the ticks: a session
  // asked for its drawn camera after every tick flies the committed trajectory all the same.
  const input::InputLog log = make_synthetic_session();
  const view::SessionHeader header = synthetic_header();
  const input::ActionMap map = view::default_fly_map();
  view::FlySession session;
  std::string error;
  REQUIRE_MESSAGE(session.start(map, header, &error), error);
  // Before any tick both ends are the start.
  CHECK(session.previous().position == header.start.position);
  CHECK(near(session.camera_at(0.5f).position, header.start.position, 0.0f));

  const f32 fractions[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
  u32 cursor = 0;
  f32 worst_off_segment = 0.0f;
  f32 worst_along = 0.0f;
  u32 checked = 0;
  for (u64 t = 1; t <= header.ticks; ++t) {
    cursor = session.run(log.events(), cursor, SimTick{t});
    const view::FlyState a = session.previous();
    const view::FlyState b = session.state();
    const Vec3 d = b.position - a.position;
    const f32 length2 = dot(d, d);
    f32 turn = b.yaw - a.yaw;  // the short way round, as the interpolation takes it
    if (turn > 3.14159265f) turn -= 6.28318531f;
    if (turn < -3.14159265f) turn += 6.28318531f;
    for (const f32 alpha : fractions) {
      const view::FlyState s = view::fly_interpolate(a, b, alpha);
      // On the segment: its distance from the line through a and b is nothing, and it is `alpha`
      // of the way along.
      const Vec3 from_a = s.position - a.position;
      const f32 along = length2 > 0.0f ? dot(from_a, d) / length2 : alpha;
      const Vec3 off = from_a - d * along;
      worst_off_segment = std::fmax(worst_off_segment, std::sqrt(dot(off, off)));
      worst_along = std::fmax(worst_along, std::fabs(along - alpha));
      CHECK(along >= -1e-4f);
      CHECK(along <= 1.0f + 1e-4f);
      // The pitch between the two, and the yaw `alpha` of the short turn from a's.
      CHECK(s.pitch >= std::fmin(a.pitch, b.pitch) - 1e-6f);
      CHECK(s.pitch <= std::fmax(a.pitch, b.pitch) + 1e-6f);
      f32 yaw_moved = s.yaw - a.yaw;
      if (yaw_moved > 3.14159265f) yaw_moved -= 6.28318531f;
      if (yaw_moved < -3.14159265f) yaw_moved += 6.28318531f;
      CHECK(std::fabs(yaw_moved - turn * alpha) <= 1e-5f);
      CHECK(s.yaw >= -3.14159266f);
      CHECK(s.yaw <= 3.14159266f);
      ++checked;
    }
    // The ends are the ticks exactly, and the camera drawn at them is the tick's camera.
    CHECK(view::fly_interpolate(a, b, 0.0f).position == a.position);
    CHECK(view::fly_interpolate(a, b, 1.0f).position == b.position);
    CHECK(session.camera_at(1.0f).position == session.camera().position);
    CHECK(session.camera_at(1.0f).target == session.camera().target);
  }
  MESSAGE(checked << " drawn cameras; furthest off the segment " << worst_off_segment
                  << " m, furthest along it from alpha " << worst_along);
  CHECK(worst_off_segment <= 1e-4f);
  CHECK(worst_along <= 2e-3f);  // f32 steps of a few mm, 20 m from the origin
  // Asked for its drawn camera after every tick, the session flew exactly the ticks it flies
  // when nobody asks.
  CHECK(session.trajectory().hash == fly(log, map, header, 1).hash);

  // Across the yaw's seam: from just short of +pi to just past -pi is a small turn left, and
  // half of it is pi, not the other way round through zero.
  view::FlyState left;
  left.yaw = 3.1f;
  view::FlyState right;
  right.yaw = -3.1f;
  const view::FlyState half = view::fly_interpolate(left, right, 0.5f);
  CHECK(std::fabs(std::fabs(half.yaw) - 3.14159265f) <= 1e-5f);
  const view::FlyState quarter = view::fly_interpolate(left, right, 0.25f);
  CHECK(quarter.yaw > 3.1f);
  CHECK(quarter.yaw <= 3.14159266f);
}

TEST_CASE("fly camera: a recording is refused under another map or another integration") {
  const input::InputLog log = make_synthetic_session();
  // The player moved forward from W to the up arrow after recording: W in the log would now do
  // nothing, and the replay would stand still where the session flew.
  input::ActionMap rebound = view::default_fly_map();
  const input::ActionId move = rebound.find_action("move");
  rebound.clear_bindings(move);
  rebound.bind(move, input::Binding{input::Source::Key, k_key_up, 1.0f}, 1);
  REQUIRE(rebound.hash() != view::default_fly_map().hash());
  const input::InputState state(rebound);
  std::string error;
  CHECK_FALSE(log.check_map(state, &error));
  CHECK(error.find("rebinding invalidates a replay") != std::string::npos);

  // A session header from another integration version is refused rather than misread; so is a
  // log with no session at all (a device recording from engine-input), and a mistyped one.
  JsonValue session = log.session();
  session.set("version", static_cast<u64>(view::k_fly_version + 1));
  view::SessionHeader header;
  CHECK_FALSE(view::session_from_json(session, header, &error));
  CHECK(error.find("version") != std::string::npos);
  CHECK(error.find("refused") != std::string::npos);
  CHECK_FALSE(view::session_from_json(JsonValue(), header, &error));
  CHECK(error.find("no session block") != std::string::npos);
  JsonValue broken = log.session();
  broken.set("fly", JsonValue(3.0));
  CHECK_FALSE(view::session_from_json(broken, header, &error));
  // And the header round-trips to the same bits.
  REQUIRE(view::session_from_json(log.session(), header, &error));
  CHECK(write_json(view::session_to_json(header)) == write_json(log.session()));
}

TEST_CASE("fly camera: window events become input once, at the edge") {
  input::RawEvent out[4];
  window::Event e;
  e.kind = window::EventKind::KeyDown;
  e.scancode = static_cast<u16>(k_key_w);
  REQUIRE(view::convert_window_event(e, SimTick{7}, false, out) == 1);
  CHECK(out[0].tick.value == 7);
  CHECK(out[0].source == input::Source::Key);
  CHECK(out[0].code == k_key_w);
  CHECK(out[0].value == 1.0f);
  e.repeat = true;  // a repeat is not a second press
  CHECK(view::convert_window_event(e, SimTick{7}, false, out) == 0);

  // Pointer motion is looking only while the window holds the pointer.
  window::Event motion;
  motion.kind = window::EventKind::MouseMove;
  motion.dx = 3.0f;
  motion.dy = -2.0f;
  CHECK(view::convert_window_event(motion, SimTick{1}, false, out) == 0);
  REQUIRE(view::convert_window_event(motion, SimTick{1}, true, out) == 2);
  CHECK(out[0].code == static_cast<u32>(input::MouseAxisCode::X));
  CHECK(out[1].value == -2.0f);

  // A pad and a hat keep their slot spaces apart by source; a hat is four signals.
  window::Event pad;
  pad.kind = window::EventKind::GamepadAxis;
  pad.gamepad = 1;
  pad.gamepad_axis = window::GamepadAxis::RightX;
  pad.value = 0.5f;
  REQUIRE(view::convert_window_event(pad, SimTick{1}, false, out) == 1);
  CHECK(out[0].source == input::Source::GamepadAxis);
  CHECK(out[0].device == 1);
  window::Event hat;
  hat.kind = window::EventKind::JoystickHat;
  hat.joystick = 1;
  hat.hat = window::HatDirection::RightUp;
  REQUIRE(view::convert_window_event(hat, SimTick{1}, false, out) == 4);
  CHECK(out[0].value == 1.0f);  // up
  CHECK(out[1].value == 1.0f);  // right
  CHECK(out[2].value == 0.0f);
  CHECK(out[3].source == input::Source::JoystickHat);
  window::Event focus;
  focus.kind = window::EventKind::FocusLost;
  CHECK(view::convert_window_event(focus, SimTick{1}, true, out) == 0);

  // The edge sums a tick's motion into one event per axis, in arrival order, so InputState reads
  // the same total and the log holds one line per axis per tick however fast the mouse polls.
  view::EdgeEvents edge(8);
  edge.add(motion, SimTick{5}, true);
  edge.add(e, SimTick{5}, true);  // a repeat: dropped
  window::Event press = e;
  press.repeat = false;
  edge.add(press, SimTick{5}, true);
  motion.dx = 0.25f;
  motion.dy = 0.0f;
  edge.add(motion, SimTick{5}, true);
  REQUIRE(edge.events().size() == 3);
  CHECK(edge.events()[0].value == 3.25f);
  CHECK(edge.events()[1].value == -2.0f);
  CHECK(edge.events()[2].source == input::Source::Key);
  edge.add(motion, SimTick{6}, true);  // a new tick starts a new sum
  CHECK(edge.events().size() == 4);
  edge.clear();
  CHECK(edge.empty());

  // --inject-input's direction: what a window can be handed back, and what it cannot.
  window::Event back;
  REQUIRE(view::window_event_of(key(3, k_key_escape, true), back));
  CHECK(back.kind == window::EventKind::KeyDown);
  CHECK(back.scancode == k_key_escape);
  REQUIRE(view::window_event_of(pointer(3, input::MouseAxisCode::Y, -4.0f), back));
  CHECK(back.kind == window::EventKind::MouseMove);
  CHECK(back.dy == -4.0f);
  CHECK_FALSE(view::window_event_of(
      input::RawEvent{SimTick{3}, input::Source::GamepadButton, 1, 1.0f, 0}, back));
}

TEST_CASE("fly camera: motion the window reported before relative mode came on is not looking") {
  // The owner's walk (2026-10-02): the window takes the focus, the session takes the pointer, and
  // the first motion polled is the absolute pointer's run from wherever the cursor last was — here
  // 900 px, a turn of two radians at the default look rate — before the frame turns relative mode
  // on. The walk started facing somewhere else. Fed through the edge with the gate in front of it,
  // the session turns by the motion reported after the mode came on and by nothing before it.
  const view::SessionHeader header = synthetic_header();
  const input::ActionMap map = view::default_fly_map();
  view::PointerGate gate;  // a live window: it takes the real pointer
  view::EdgeEvents edge(8);
  const auto feed = [&](f32 dx, u64 at_ns, u64 tick) {
    window::Event motion;
    motion.kind = window::EventKind::MouseMove;
    motion.dx = dx;
    motion.timestamp_ns = at_ns;
    edge.add(motion, SimTick{tick}, gate.looking(motion));
  };
  feed(40.0f, 500, 1);  // not held: pointing
  gate.captured = true;
  feed(900.0f, 1'000, 1);  // held, the mode not on yet: the jump
  gate.relative = true;
  gate.relative_since_ns = 2'000;
  feed(120.0f, 1'500, 2);  // queued before the mode came on, polled after: still the jump's
  feed(3.0f, 2'500, 2);    // looking
  REQUIRE(edge.events().size() == 1);
  CHECK(edge.events()[0].value == 3.0f);
  CHECK(edge.events()[0].tick.value == 2);

  // What a session — and so a recording, which holds exactly what was fed — makes of it.
  input::InputLog log;
  for (const input::RawEvent& e : edge.events())
    log.record(e);
  view::FlySession session;
  std::string error;
  REQUIRE_MESSAGE(session.start(map, header, &error), error);
  (void)session.run(log.events(), 0, SimTick{3});
  const f32 turned = std::abs(session.state().yaw - header.start.yaw);
  CHECK(turned < 0.05f);  // three pixels' worth, not two radians

  // An injected run never takes the real pointer: its capture alone decides, as before.
  view::PointerGate injected;
  injected.takes_pointer = false;
  window::Event motion;
  motion.kind = window::EventKind::MouseMove;
  motion.dx = 900.0f;
  CHECK_FALSE(injected.looking(motion));
  injected.captured = true;
  CHECK(injected.looking(motion));
}

TEST_CASE("fly camera: the title's pacing numbers") {
  view::FramePacing pacing;
  CHECK(pacing.percentile(0, 1'000'000'000, 0.99) == 0.0f);
  const i64 ms = 1'000'000;
  // 100 frames of 10 ms, one of 50: the p99 of 101 is the second largest, and the slow frame is
  // the max. The frames of an earlier second do not count.
  pacing.add(-5000 * ms, 500.0f);
  for (i64 i = 0; i < 100; ++i)
    pacing.add(i * 10 * ms, 10.0f);
  pacing.add(1000 * ms, 50.0f);
  CHECK(pacing.last_ms() == 50.0f);
  CHECK(pacing.in_window(1000 * ms, 1000 * ms) == 101);
  CHECK(pacing.percentile(1000 * ms, 1000 * ms, 0.99) == 10.0f);
  CHECK(pacing.percentile(1000 * ms, 1000 * ms, 1.0) == 50.0f);
  // The ring keeps the newest frames once it is full.
  for (u32 i = 0; i < view::FramePacing::k_capacity + 10; ++i)
    pacing.add(2000 * ms, 1.0f);
  CHECK(pacing.percentile(2000 * ms, 1000 * ms, 1.0) == 1.0f);
}

TEST_CASE("fly camera: the display pacer waits further back when frames miss, and comes back") {
  const i64 ms = 1'000'000;
  const i64 refresh = 12 * ms;
  i64 t = 0;
  view::DisplayPacer pacer(refresh);
  auto wait = [&](i64 refreshes_times_ten) {
    t += refresh * refreshes_times_ten / 10;
    pacer.waited(t, true);
  };
  // Frames that come round every refresh: depth 1 throughout, nothing missed.
  for (u32 i = 0; i < 500; ++i)
    wait(10);
  CHECK(pacer.depth() == 1);
  CHECK(pacer.misses() == 0);
  // One stall of three refreshes: two refreshes went by without a frame, and it is one miss.
  wait(30);
  wait(10);
  CHECK(pacer.misses() == 2);
  CHECK(pacer.depth() == 1);
  // A compositor's jitter — a wait late by six tenths of a refresh, the next early by as much —
  // shows nothing missed however long it goes on, stall or no stall before it.
  for (u32 i = 0; i < 200; ++i)
    wait(i % 2 == 0 ? 16 : 4);
  CHECK(pacer.depth() == 1);
  CHECK(pacer.misses() == 2);
  for (u32 i = 0; i < 100; ++i)
    wait(10);
  // Work that no longer fits: every frame misses its refresh. A miss is known when the wait after
  // it did not make up for it, so the third such wait makes two, and the pacer goes to depth 2.
  wait(20);
  wait(20);
  CHECK(pacer.depth() == 1);
  wait(20);
  CHECK(pacer.depth() == 2);
  CHECK(pacer.switches() == 1);
  // It holds depth 2 for k_hold frames, then tries depth 1 again...
  for (u32 i = 0; i < view::DisplayPacer::k_hold; ++i)
    wait(10);
  CHECK(pacer.depth() == 1);
  // ...where the first wait is the deeper queue draining and is not judged. A try that misses at
  // once goes back with twice the hold.
  wait(20);
  wait(20);
  wait(20);
  CHECK(pacer.depth() == 1);
  wait(20);
  CHECK(pacer.depth() == 2);
  CHECK(pacer.hold() == 2 * view::DisplayPacer::k_hold);
  for (u32 i = 0; i + 1 < 2 * view::DisplayPacer::k_hold; ++i)
    wait(10);
  CHECK(pacer.depth() == 2);
  wait(10);
  CHECK(pacer.depth() == 1);
  // A try that holds for longer than two windows resets the hold the next time it is needed.
  for (u32 i = 0; i < 3 * view::DisplayPacer::k_recent; ++i)
    wait(10);
  wait(20);
  wait(20);
  wait(20);
  CHECK(pacer.depth() == 2);
  CHECK(pacer.hold() == view::DisplayPacer::k_hold);
  // A wait that timed out (nobody can see the window) is neither a miss nor an interval...
  const u64 before = pacer.misses();
  t += 10 * refresh;
  pacer.waited(t, false);
  wait(10);
  CHECK(pacer.misses() == before);
  CHECK(pacer.should_wait());
  // ...and two running stop the waiting for k_blind frames, so a window nobody can see is not
  // held to a timeout a frame.
  pacer.waited(t += 4 * refresh, false);
  pacer.waited(t += 4 * refresh, false);
  for (u32 i = 0; i < view::DisplayPacer::k_blind; ++i)
    CHECK_FALSE(pacer.should_wait());
  CHECK(pacer.should_wait());

  // With no refresh from the swapchain the pacer takes the median of its own waits' intervals,
  // and counts nothing missed until it has one.
  view::DisplayPacer estimated;
  t = 0;
  for (u32 i = 0; i <= view::DisplayPacer::k_window; ++i)
    estimated.waited(t += refresh + (i % 2 == 0 ? ms / 10 : -ms / 10), true);
  CHECK(estimated.refresh_ns() > refresh - ms / 5);
  CHECK(estimated.refresh_ns() < refresh + ms / 5);
  CHECK(estimated.misses() == 0);
  estimated.waited(t += 3 * refresh, true);
  CHECK(estimated.misses() == 2);
}

TEST_CASE("fly camera: --present's names, and display times by present id") {
  view::PresentMode mode = view::PresentMode::Fifo;
  CHECK(view::parse_present_mode("mailbox", &mode));
  CHECK(mode == view::PresentMode::Mailbox);
  CHECK(view::parse_present_mode("fifo-relaxed", &mode));
  CHECK(mode == view::PresentMode::FifoRelaxed);
  CHECK(view::parse_present_mode("fifo-latest-ready", &mode));
  CHECK(mode == view::PresentMode::FifoLatestReady);
  CHECK(view::parse_present_mode("immediate", nullptr));
  CHECK(view::parse_present_mode("fifo", nullptr));
  CHECK_FALSE(view::parse_present_mode("vsync", nullptr));
  CHECK_FALSE(view::parse_present_mode("fifo_relaxed", nullptr));  // the summary's spelling
  CHECK_FALSE(view::parse_present_mode("", nullptr));

  // Presents 1..4 sampled 2 ms apart and shown a refresh apart, 24 ms after the first sample;
  // present 3's display time never came back, and present 5 was sampled but never shown.
  view::DisplayTimes times;
  const i64 ms = 1'000'000;
  for (u64 id = 1; id <= 5; ++id)
    times.sampled(id, static_cast<i64>(id) * 2 * ms);
  times.shown(1, 26 * ms);
  times.shown(2, 38 * ms);
  times.shown(4, 62 * ms);
  CHECK(times.reported() == 3);
  CHECK(times.shown_ms(1) == 0.0);  // no present before the first
  CHECK(times.shown_ms(2) == doctest::Approx(12.0));
  CHECK(times.shown_ms(3) == 0.0);  // its own time is unknown
  CHECK(times.shown_ms(4) == 0.0);  // the one before it is
  CHECK(times.latency_ms(1) == doctest::Approx(24.0));
  CHECK(times.latency_ms(2) == doctest::Approx(34.0));
  CHECK(times.latency_ms(4) == doctest::Approx(54.0));
  CHECK(times.latency_ms(5) == 0.0);
  CHECK(times.latency_ms(9) == 0.0);  // never presented
  times.shown(4, 61 * ms);            // a second report of one present replaces the first
  CHECK(times.reported() == 3);
  CHECK(times.latency_ms(4) == doctest::Approx(53.0));
}

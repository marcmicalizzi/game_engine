// The wind's keys with no GPU and no window (docs/subsystems/apps.md, "The wind on three keys";
// gfx.md, "The live wind"; roadmap W151): the map's eighth revision binds them, the fly session
// steps the live wind at the ticks they were pressed and held on, a recording replays to the same
// wind tick by tick, and nothing of it enters the trajectory's hash.
#include "../fly_camera.h"
#include "../time_controls.h"
#include "../wind_controls.h"

#include <foundation/input/input.h>
#include <foundation/input/input_log.h>

#include <doctest/doctest.h>

#include <cstring>
#include <span>
#include <string>

using namespace engine;

namespace {

constexpr u32 k_key_g = 10;
constexpr u32 k_key_semicolon = 51;
constexpr u32 k_key_apostrophe = 52;
constexpr u32 k_hz = 240;

input::RawEvent key(u64 tick, u32 code, bool down) {
  return input::RawEvent{SimTick{tick}, input::Source::Key, code, down ? 1.0f : 0.0f, 0};
}

// "Calm, step to a gale, a gust held three seconds, released": eight presses of `'` from calm
// (Beaufort 0 to 8, 18.9 m/s) on ticks 10 to 80, G down on tick 240 and up on tick 960.
Vector<input::RawEvent> storm_events() {
  Vector<input::RawEvent> out;
  for (u64 i = 0; i < 8; ++i) {
    out.push_back(key(10 + i * 10, k_key_apostrophe, true));
    out.push_back(key(11 + i * 10, k_key_apostrophe, false));
  }
  out.push_back(key(240, k_key_g, true));
  out.push_back(key(960, k_key_g, false));
  return out;
}

view::SessionHeader calm_header() {
  view::SessionHeader header;
  header.params.tick_hz = k_hz;
  header.params.speed = 1.0f;
  header.has_wind = true;
  header.wind_roughness_m = 0.0002;  // the sea's
  return header;
}

struct WindTick {
  bool set = false;
  f64 mean = 0.0;
  f64 gust = 0.0;
};

// The session's wind after every tick to `ticks`.
Vector<WindTick> fly(const input::ActionMap& map, const view::SessionHeader& header,
                     std::span<const input::RawEvent> events, u64 ticks, u64* hash = nullptr) {
  view::FlySession session;
  std::string error;
  REQUIRE_MESSAGE(session.start(map, header, &error), error);
  Vector<WindTick> out;
  u32 cursor = 0;
  for (u64 t = 1; t <= ticks; ++t) {
    cursor = session.run(events, cursor, SimTick{t});
    const gfx::WindLive live = session.wind().live();
    out.push_back(WindTick{live.set, live.mean_m_s, live.gust});
  }
  if (hash != nullptr) *hash = session.trajectory().hash;
  return out;
}

}  // namespace

// **The ladder and the gust, tick by tick** (W151): the eighth revision binds `;`, `'` and G and
// the seventh binds none of them; calm until the first press (unset: every reader draws the
// scene's wind), a force a press after it, a gale at the eighth; the gust held from tick 240 stands
// at half its level five seconds in and whole at ten... except that it is let go at three, where it
// stands at smoothstep(0.3) and then dies at the decay's pace, gone 3 s after the release; the mean
// never moves under it. The trajectory is the quiet session's: the wind is not the camera's.
TEST_CASE("wind controls: the keys step the mean a Beaufort force and hold a gust, by tick") {
  const input::ActionMap map = view::default_fly_map();
  view::FlyActions actions;
  std::string error;
  REQUIRE_MESSAGE(view::resolve_fly_actions(map, actions, &error), error);
  CHECK(actions.controls[static_cast<u32>(view::ViewControl::wind_weaker)] !=
        input::k_invalid_action);
  CHECK(actions.controls[static_cast<u32>(view::ViewControl::wind_stronger)] !=
        input::k_invalid_action);
  CHECK(actions.gust != input::k_invalid_action);
  const input::ActionMap seventh = view::default_fly_map(7);
  view::FlyActions old;
  REQUIRE(view::resolve_fly_actions(seventh, old, &error));
  CHECK(old.gust == input::k_invalid_action);
  CHECK(map.hash() != seventh.hash());
  input::ActionMap found;
  CHECK(view::default_fly_map_for(seventh.hash(), found));

  const Vector<input::RawEvent> events = storm_events();
  u64 hash = 0;
  const Vector<WindTick> w = fly(map, calm_header(), events, 2400, &hash);
  CHECK_FALSE(w[8].set);  // tick 9: no key yet
  CHECK(w[9].set);        // tick 10: Beaufort 1
  CHECK(w[9].mean == doctest::Approx(gfx::beaufort_speed(1)));
  CHECK(w[79].mean == doctest::Approx(gfx::beaufort_speed(8)));
  CHECK(w[238].gust == 0.0);
  // Held from tick 240; the level after tick t is smoothstep((t - 239) / 2400).
  for (u32 t = 240; t < 960; t += 60) {
    const f64 p = static_cast<f64>(t - 239) / (10.0 * k_hz);
    CHECK(w[t - 1].gust == doctest::Approx(p * p * (3.0 - 2.0 * p)).epsilon(1e-9));
    CHECK(w[t - 1].mean == doctest::Approx(gfx::beaufort_speed(8)));
  }
  const f64 at_release = w[958].gust;
  CHECK(at_release == doctest::Approx(0.216).epsilon(0.01));  // smoothstep(0.3)
  CHECK(w[1000].gust < at_release);
  CHECK(w[960 + 3 * k_hz].gust == 0.0);
  view::FlySession quiet;
  REQUIRE(quiet.start(map, calm_header(), &error));
  (void)quiet.run(std::span<const input::RawEvent>(), 0, SimTick{2400});
  CHECK(quiet.trajectory().hash == hash);
  CHECK_FALSE(quiet.wind().live().set);

  // Down from calm stays calm; down from the gale steps a force.
  const input::RawEvent down[] = {key(5, k_key_semicolon, true), key(6, k_key_semicolon, false)};
  const Vector<WindTick> d = fly(map, calm_header(), down, 10);
  CHECK(d[9].set);
  CHECK(d[9].mean == 0.0);

  // The seventh revision presses nothing of it.
  const Vector<WindTick> before = fly(seventh, calm_header(), events, 1200);
  for (const WindTick& t : before)
    CHECK_FALSE(t.set);
}

// **A replay is the same wind** (W151): the storm recorded into an input log with its header, the
// log written and read back, and replayed — the session's wind at every tick is the recording's to
// the bit. The header carries the wind's start (its roughness and base), so a replay under other
// tunables starts from the same place. And the title says it.
TEST_CASE("wind controls: a recorded storm replays to the same wind at every tick") {
  const input::ActionMap map = view::default_fly_map();
  view::SessionHeader header = calm_header();
  header.wind_mean_m_s = 4.0;  // a base: the first press steps from it
  header.wind_to_deg = 270.0;
  const Vector<input::RawEvent> events = storm_events();
  const Vector<WindTick> live = fly(map, header, events, 1500);
  CHECK(live[9].mean == doctest::Approx(gfx::beaufort_speed(3)));  // from 4 m/s: force 3, 4.3

  const JsonValue block = view::session_to_json(header);
  view::SessionHeader back;
  std::string error;
  REQUIRE_MESSAGE(view::session_from_json(block, back, &error), error);
  CHECK(back.has_wind);
  CHECK(back.wind_roughness_m == header.wind_roughness_m);
  CHECK(back.wind_mean_m_s == header.wind_mean_m_s);
  CHECK(back.wind_to_deg == header.wind_to_deg);
  const Vector<WindTick> replay = fly(map, back, events, 1500);
  REQUIRE(replay.size() == live.size());
  for (u32 i = 0; i < live.size(); ++i) {
    CHECK(replay[i].set == live[i].set);
    CHECK(std::memcmp(&replay[i].mean, &live[i].mean, sizeof(f64)) == 0);
    CHECK(std::memcmp(&replay[i].gust, &live[i].gust, sizeof(f64)) == 0);
  }
  // A header with no wind block (every recording before W151) reads unset and calm.
  view::SessionHeader none = calm_header();
  none.has_wind = false;
  view::SessionHeader none_back;
  REQUIRE(view::session_from_json(view::session_to_json(none), none_back, &error));
  CHECK_FALSE(none_back.has_wind);
  CHECK(none_back.wind_roughness_m == 0.03);

  view::WindControls wind;
  wind.start(0.03, 0.0, 0.0);
  for (u32 i = 0; i < 8; ++i)
    (void)wind.step(true);
  char text[64];
  CHECK(std::string(view::format_wind(wind, text, sizeof(text))) == "wind 18.9 m/s (Bft 8)");
  for (u32 i = 0; i < 2400; ++i)
    wind.tick(1.0 / k_hz, true);
  CHECK(std::string(view::format_wind(wind, text, sizeof(text))) ==
        "wind 18.9 m/s (Bft 8) gusting 27.5");
  view::TitleStatus status;
  status.sun_rate = 24.0;
  status.live = false;
  status.wind = text;
  char title[224];
  (void)view::format_status(status, title, sizeof(title));
  CHECK(std::string(title).find("wind 18.9 m/s (Bft 8) gusting 27.5") != std::string::npos);
}

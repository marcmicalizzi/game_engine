// The time-lapse's keys with no GPU and no window (docs/subsystems/apps.md, "`--interactive`"):
// the two ladders and a step along one, the sun's day as the host keeps it, the title's text, and
// the keys as the fly session counts them — through the map's second revision, and not at all
// through the first, which every session recorded before the keys existed carries.
#include "../fly_camera.h"
#include "../time_controls.h"

#include <foundation/input/input.h>

#include <doctest/doctest.h>

#include <cstring>
#include <iterator>
#include <span>
#include <string>

using namespace engine;

namespace {

constexpr u32 k_key_w = 26;
constexpr u32 k_key_left_bracket = 47;
constexpr u32 k_key_right_bracket = 48;
constexpr u32 k_key_comma = 54;
constexpr u32 k_key_period = 55;

input::RawEvent key(u64 tick, u32 code, bool down) {
  return input::RawEvent{SimTick{tick}, input::Source::Key, code, down ? 1.0f : 0.0f, 0};
}

std::string rate_text(f64 rate) {
  char text[32];
  return view::format_rate(rate, text, sizeof(text));
}

}  // namespace

TEST_CASE("time controls: a key steps its ladder a rung, from anywhere, and stops at the ends") {
  const std::span<const f64> sun(view::k_sun_ladder);
  const std::span<const f64> dunes(view::k_dune_ladder);
  // The ladders the owner asked for.
  const f64 sun_rungs[] = {0.0, 60.0, 600.0, 3'600.0, 8'640.0, 86'400.0};
  const f64 dune_rungs[] = {0.0, 60.0, 600.0, 3'600.0, 8'640.0, 86'400.0, 604'800.0};
  REQUIRE(sun.size() == std::size(sun_rungs));
  REQUIRE(dunes.size() == std::size(dune_rungs));
  for (usize i = 0; i < sun.size(); ++i)
    CHECK(sun[i] == sun_rungs[i]);
  for (usize i = 0; i < dunes.size(); ++i)
    CHECK(dunes[i] == dune_rungs[i]);
  // Up and down one rung at a time, and nowhere past an end.
  f64 rate = 0.0;
  for (usize i = 1; i < dunes.size(); ++i) {
    rate = view::ladder_step(dunes, rate, true);
    CHECK(rate == dunes[i]);
  }
  CHECK(view::ladder_step(dunes, rate, true) == 604'800.0);
  for (usize i = dunes.size() - 1; i-- > 0;) {
    rate = view::ladder_step(dunes, rate, false);
    CHECK(rate == dunes[i]);
  }
  CHECK(view::ladder_step(dunes, 0.0, false) == 0.0);
  CHECK(view::ladder_step(sun, 86'400.0, true) == 86'400.0);
  // From a rate between two rungs (a `--time-rate 5000`), the rung on that side; from past an
  // end, that end.
  CHECK(view::ladder_step(dunes, 5'000.0, true) == 8'640.0);
  CHECK(view::ladder_step(dunes, 5'000.0, false) == 3'600.0);
  CHECK(view::ladder_step(sun, 1.0e6, false) == 86'400.0);
  CHECK(view::ladder_step(sun, 1.0e6, true) == 86'400.0);
  CHECK(view::ladder_step(dunes, 30.0, false) == 0.0);
}

TEST_CASE("time controls: the sun's day moves by the rate and never jumps when it changes") {
  view::SunDay day;
  day.start(3'600.0);
  CHECK(day.rate == 3'600.0);
  CHECK(day.start_rate == 3'600.0);
  CHECK(day.time_s == 0.0);
  day.advance(0.5);
  CHECK(day.time_s == 1'800.0);
  // A change changes how fast the day runs from here, and not where it is.
  CHECK(day.set_rate(86'400.0));
  CHECK(day.time_s == 1'800.0);
  day.advance(0.25);
  CHECK(day.time_s == 1'800.0 + 21'600.0);
  CHECK_FALSE(day.set_rate(86'400.0));  // the same rate is no change
  CHECK(day.set_rate(0.0));
  day.advance(10.0);
  CHECK(day.time_s == 1'800.0 + 21'600.0);
  CHECK(day.changes == 2u);
  CHECK(day.start_rate == 3'600.0);
  day.advance(-1.0);  // time never runs backwards
  CHECK(day.time_s == 1'800.0 + 21'600.0);
  CHECK_FALSE(day.set_rate(-5.0));  // a negative rate is a still one, which it already is
  CHECK(day.rate == 0.0);
}

TEST_CASE("time controls: a rate is written the way the title writes it") {
  CHECK(rate_text(0.0) == "0");
  CHECK(rate_text(60.0) == "60");
  CHECK(rate_text(600.0) == "600");
  CHECK(rate_text(3'600.0) == "3,600");
  CHECK(rate_text(86'400.0) == "86,400");
  CHECK(rate_text(604'800.0) == "604,800");
  CHECK(rate_text(1'234'567.0) == "1,234,567");
  CHECK(rate_text(5'000.5) == "5,000.5");
  CHECK(rate_text(0.5) == "0.5");
  CHECK(rate_text(1.0e9) == "1,000,000,000");

  // The status the title leads with, in UTF-8: the rates, then what the pointer is doing.
  char text[160];
  view::TitleStatus status;
  status.dunes = true;
  status.dune_rate = 86'400.0;
  status.sun_rate = 3'600.0;
  status.live = true;
  status.captured = true;
  const usize n = view::format_status(status, text, sizeof(text));
  CHECK(n == std::strlen(text));
  CHECK(std::string(text) ==
        "dunes \xC3\x97"
        "86,400 \xC2\xB7 sun \xC3\x97"
        "3,600 \xC2\xB7 mouse captured (Esc frees it)");
  status.captured = false;
  (void)view::format_status(status, text, sizeof(text));
  CHECK(std::string(text).find("mouse free (click to capture, Esc again exits)") !=
        std::string::npos);
  // A scene whose sand cannot move leaves the dunes out; a replay says nothing about a pointer.
  status.dunes = false;
  status.live = false;
  (void)view::format_status(status, text, sizeof(text));
  CHECK(std::string(text) ==
        "sun \xC3\x97"
        "3,600");
  // A buffer too small is cut, never overrun.
  char tiny[12];
  status.dunes = true;
  status.live = true;
  const usize cut = view::format_status(status, tiny, sizeof(tiny));
  CHECK(cut < sizeof(tiny));
  CHECK(std::strlen(tiny) == cut);
}

TEST_CASE("time controls: the fly session counts the keys, with the second revision of the map") {
  // The map's second revision binds [ ] , . to the four controls; a press is counted once, on the
  // tick it went down, and moves the camera not at all.
  const input::ActionMap map = view::default_fly_map();
  view::FlyActions actions;
  std::string error;
  REQUIRE_MESSAGE(view::resolve_fly_actions(map, actions, &error), error);
  for (const input::ActionId id : actions.controls)
    CHECK(id != input::k_invalid_action);
  view::SessionHeader header;
  header.params.tick_hz = 240;
  header.params.speed = 1.0f;
  const input::RawEvent events[] = {
      key(2, k_key_right_bracket, true), key(3, k_key_right_bracket, false),
      key(5, k_key_period, true),        key(6, k_key_period, false),
      key(8, k_key_period, true),        key(9, k_key_period, false),
      key(9, k_key_comma, true),         key(10, k_key_comma, false),
      key(12, k_key_left_bracket, true), key(40, k_key_left_bracket, false),
  };
  view::FlySession session;
  REQUIRE_MESSAGE(session.start(map, header, &error), error);
  (void)session.run(std::span<const input::RawEvent>(events), 0, SimTick{48});
  CHECK(session.control_presses(view::ViewControl::sun_faster) == 1u);
  CHECK(session.control_presses(view::ViewControl::dunes_faster) == 2u);
  CHECK(session.control_presses(view::ViewControl::dunes_slower) == 1u);
  CHECK(session.control_presses(view::ViewControl::sun_slower) == 1u);  // held, one press
  CHECK(session.state().position == header.start.position);             // the camera did not move
  // The trajectory is the same whether the keys were pressed or not: they are not the camera's.
  view::FlySession quiet;
  REQUIRE(quiet.start(map, header, &error));
  (void)quiet.run(std::span<const input::RawEvent>(), 0, SimTick{48});
  CHECK(quiet.trajectory().hash == session.trajectory().hash);

  // **The first revision** — the map every session recorded before 2026-09-27 names — flies the
  // camera and has no controls, so the same keys count nothing; and it is found by its hash.
  const input::ActionMap first = view::default_fly_map(1);
  CHECK(first.action_count() == 8u);
  CHECK(map.action_count() == 12u);
  CHECK(first.hash() != map.hash());
  view::FlyActions first_actions;
  REQUIRE(view::resolve_fly_actions(first, first_actions, &error));
  for (const input::ActionId id : first_actions.controls)
    CHECK(id == input::k_invalid_action);
  view::FlySession old;
  REQUIRE(old.start(first, header, &error));
  (void)old.run(std::span<const input::RawEvent>(events), 0, SimTick{48});
  for (u32 c = 0; c < view::k_view_controls; ++c)
    CHECK(old.control_presses(static_cast<view::ViewControl>(c)) == 0u);
  input::ActionMap found;
  CHECK(view::default_fly_map_for(first.hash(), found));
  CHECK(found.hash() == first.hash());
  CHECK(view::default_fly_map_for(map.hash(), found));
  CHECK(found.hash() == map.hash());
  input::ActionMap rebound = view::default_fly_map();
  rebound.bind(rebound.find_action("move"), input::Binding{input::Source::Key, k_key_w, 2.0f}, 1);
  CHECK_FALSE(view::default_fly_map_for(rebound.hash(), found));

  // A map whose control is not a button is a mistake, and refused by name.
  input::ActionMap wrong = view::default_fly_map(1);
  (void)wrong.add_action("dunes_faster", input::ActionKind::Axis);
  CHECK_FALSE(view::resolve_fly_actions(wrong, first_actions, &error));
  CHECK(error.find("dunes_faster") != std::string::npos);
}

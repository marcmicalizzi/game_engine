// The overlay's invariants (docs/subsystems/terrain.md, "The overlay and its bound"): a footprint
// buried by the wind and gone from storage, a dug-out drift refilling toward its steady state, the
// same bytes whatever the cadence it is advanced at and whether stamps are baked eagerly or late,
// the record's round trip and refusals, the storage bound under weeks of footprints, rock that
// takes no print, and the feedback's saturation.
#include <domain/terrain/fixed.h>
#include <domain/terrain/terrain.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;
using namespace engine::terrain;

namespace {

constexpr i64 k_day = k_us_per_day;
constexpr i64 k_hour = k_day / 24;

Stamp footprint(i64 time_us, i32 x, i32 z, u16 yaw = 0) {
  Stamp s;
  s.time_us = time_us;
  s.x_mm = x;
  s.z_mm = z;
  s.radius_mm = 150;
  s.radius2_mm = 60;
  s.yaw_turn = yaw;
  s.depth_mm = 40;
  s.rim_mm = 10;
  s.kind = static_cast<u8>(StampKind::footprint);
  return s;
}

Stamp pit(i64 time_us, i32 x, i32 z, i16 depth_mm, u16 radius_mm) {
  Stamp s;
  s.time_us = time_us;
  s.x_mm = x;
  s.z_mm = z;
  s.radius_mm = radius_mm;
  s.radius2_mm = radius_mm;
  s.depth_mm = depth_mm;
  s.rim_mm = 0;
  s.kind = static_cast<u8>(StampKind::dig);
  return s;
}

u32 record_bytes(const Overlay& overlay, const OverlayRules& rules) {
  Vector<u8> bytes;
  overlay.write(bytes, rules);
  return bytes.size();
}

}  // namespace

TEST_CASE("overlay: a footprint is buried by the wind and then stores nothing") {
  const WindRecord wind(WindParams{.seed = 3});
  const OverlayRules rules;
  Overlay overlay;
  overlay.reset(TileCoord{0, 0}, 32'000, 0);
  REQUIRE(overlay.push(footprint(k_hour, 16'000, 16'000), wind, rules));
  overlay.advance(k_hour, wind, rules);
  const i32 fresh = overlay.deviation_mm(16'000, 16'000);
  CHECK(fresh == -40);
  CHECK(record_bytes(overlay, rules) > 0);
  const i64 predicted = overlay.burial_time_us(wind, rules);
  // Its residual only ever shrinks, hour by hour, and reaches zero when predicted.
  i32 previous = fresh;
  i64 buried_at = -1;
  for (i64 h = 2; h < 24 * 20 && buried_at < 0; ++h) {
    overlay.advance(h * k_hour, wind, rules);
    const i32 now = overlay.deviation_mm(16'000, 16'000);
    CHECK(now >= previous);
    CHECK(now <= 0);
    previous = now;
    if (overlay.empty()) buried_at = h * k_hour;
  }
  MESSAGE("a 4 cm footprint was buried after " << (buried_at - k_hour) / k_hour << " hours");
  REQUIRE(buried_at > 0);
  CHECK(predicted <= buried_at);
  CHECK(predicted > buried_at - k_hour);
  CHECK(record_bytes(overlay, rules) == 0);
}

TEST_CASE("overlay: the same bytes whatever the cadence, eager or queued") {
  const WindRecord wind(WindParams{.seed = 11});
  const OverlayRules rules;
  Overlay eager, lazy, daily;
  for (Overlay* o : {&eager, &lazy, &daily})
    o->reset(TileCoord{2, -1}, 32'000, 0);
  Vector<Stamp> stamps;
  for (u32 k = 0; k < 100; ++k) {
    const i32 x = 64'000 + static_cast<i32>((k * 7919) % 32'000);
    const i32 z = -32'000 + static_cast<i32>((k * 104'729) % 32'000);
    stamps.push_back(
        k % 9 == 0 ? pit(k * 37 * 60 * k_us_per_second, x, z, 300, 700)
                   : footprint(k * 37 * 60 * k_us_per_second, x, z, static_cast<u16>(k * 999)));
  }
  i64 t = 0;
  for (const Stamp& s : stamps) {
    // Eager: advanced every ten minutes of game time and baked at once.
    while (t + 10 * 60 * k_us_per_second <= s.time_us) {
      t += 10 * 60 * k_us_per_second;
      eager.advance(t, wind, rules);
    }
    REQUIRE(eager.push(s, wind, rules));
    eager.advance(s.time_us, wind, rules);
    // Lazy: queued, baked only when the queue fills.
    REQUIRE(lazy.push(s, wind, rules));
    REQUIRE(daily.push(s, wind, rules));
  }
  const i64 end = 3 * k_day + 5;
  eager.advance(end, wind, rules);
  lazy.advance(end, wind, rules);
  for (i64 d = 1; d <= 3; ++d)
    daily.advance(d * k_day, wind, rules);
  daily.advance(end, wind, rules);
  Vector<u8> a, b, c;
  eager.write(a, rules);
  lazy.write(b, rules);
  daily.write(c, rules);
  CHECK(a == b);
  CHECK(a == c);
  CHECK(eager.lag() == lazy.lag());
}

TEST_CASE("overlay: a record reads back as written, and is refused when it is not one") {
  const WindRecord wind(WindParams{.seed = 5});
  const OverlayRules rules;
  Overlay overlay;
  overlay.reset(TileCoord{-3, 4}, 32'000, 0);
  for (u32 k = 0; k < 40; ++k)
    REQUIRE(overlay.push(
        footprint(k * 60 * k_us_per_second, -90'000 + static_cast<i32>(k) * 500, 140'000), wind,
        rules));
  overlay.advance(35 * 60 * k_us_per_second, wind, rules);  // some baked, some still queued
  REQUIRE(overlay.push(footprint(50 * 60 * k_us_per_second, -80'000, 135'000), wind, rules));
  Vector<u8> bytes;
  overlay.write(bytes, rules);
  CHECK(bytes.size() <= k_overlay_record_max_bytes);
  Overlay back;
  std::string error;
  REQUIRE(back.read(bytes, TileCoord{-3, 4}, 32'000, rules, &error));
  CHECK(back.pending() == overlay.pending());
  Vector<u8> again;
  back.write(again, rules);
  CHECK(again == bytes);
  // And they go on to the same state.
  overlay.advance(2 * k_day, wind, rules);
  back.advance(2 * k_day, wind, rules);
  Vector<u8> x, y;
  overlay.write(x, rules);
  back.write(y, rules);
  CHECK(x == y);
  // Refusals.
  CHECK_FALSE(back.read(bytes, TileCoord{-3, 5}, 32'000, rules, &error));
  CHECK(error.find("another tile") != std::string::npos);
  OverlayRules other = rules;
  other.fill_um_per_cm2 = 12;
  CHECK_FALSE(back.read(bytes, TileCoord{-3, 4}, 32'000, other, &error));
  CHECK(error.find("other rules") != std::string::npos);
  Vector<u8> cut = bytes;
  cut.pop_back();
  CHECK_FALSE(back.read(cut, TileCoord{-3, 4}, 32'000, rules, &error));
  Vector<u8> junk;
  junk.resize(64);
  for (u8& b : junk)
    b = 7;
  CHECK_FALSE(back.read(junk, TileCoord{-3, 4}, 32'000, rules, &error));
  // A stamp earlier than the last is refused: time runs one way.
  CHECK_FALSE(overlay.push(footprint(k_hour, 0, 0), wind, rules));
}

TEST_CASE("overlay: weeks of footprints never store more than the bound") {
  const WindRecord wind(WindParams{.seed = 17});
  const OverlayRules rules;
  Overlay overlay;
  overlay.reset(TileCoord{0, 0}, 32'000, 0);
  // A player crossing and re-crossing the tile all day for 30 days: a step every 0.7 m and a few
  // seconds, 12,000 footprints, and a pit dug every third day.
  constexpr u32 k_days = 30;
  constexpr u32 k_steps_per_day = 400;
  u32 largest = 0;
  u32 stamps = 0;
  for (u32 day = 0; day < k_days; ++day) {
    for (u32 k = 0; k < k_steps_per_day; ++k) {
      const i64 t = day * k_day + k * 150 * k_us_per_second;
      const u32 n = day * k_steps_per_day + k;
      const i32 x = static_cast<i32>((n * 700) % 32'000);
      const i32 z = static_cast<i32>(((n / 45) * 1300 + (n % 2) * 300) % 32'000);
      REQUIRE(overlay.push(footprint(t, x, z, static_cast<u16>(n * 40503)), wind, rules));
      ++stamps;
    }
    if (day % 3 == 0) {
      REQUIRE(overlay.push(
          pit(day * k_day + 20 * k_hour, 8'000 + static_cast<i32>(day) * 700, 20'000, 900, 900),
          wind, rules));
      ++stamps;
    }
    overlay.advance((day + 1) * k_day, wind, rules);
    largest = std::max(largest, record_bytes(overlay, rules));
    CHECK(record_bytes(overlay, rules) <= k_overlay_record_max_bytes);
  }
  const i64 last = k_days * k_day;
  const i64 buried = overlay.burial_time_us(wind, rules);
  MESSAGE(stamps << " stamps over " << k_days << " days: largest record " << largest
                 << " bytes (bound " << k_overlay_record_max_bytes << "), lag "
                 << int(overlay.lag().units[0]) << ", buried " << (buried - last) / k_day
                 << " days after the last");
  CHECK(largest <= k_overlay_record_max_bytes);
  // Then nobody comes for as long as the deepest pit takes to fill: the tile keeps its lag (a few
  // bytes — the pits held the dunes back) and nothing else.
  overlay.advance(buried, wind, rules);
  CHECK(overlay.nonzero_blocks() == 0);
  CHECK(overlay.pending() == 0);
  CHECK(record_bytes(overlay, rules) == k_overlay_header_bytes);
  // And one day of the same walking stores no less than thirty did: the bound is the grid's.
  Overlay one_day;
  one_day.reset(TileCoord{0, 0}, 32'000, 0);
  for (u32 k = 0; k < k_steps_per_day; ++k) {
    const i32 x = static_cast<i32>((k * 700) % 32'000);
    const i32 z = static_cast<i32>(((k / 45) * 1300 + (k % 2) * 300) % 32'000);
    REQUIRE(one_day.push(footprint(k * 150 * k_us_per_second, x, z, static_cast<u16>(k * 40503)),
                         wind, rules));
  }
  one_day.advance(k_day, wind, rules);
  CHECK(record_bytes(one_day, rules) <= k_overlay_record_max_bytes);
}

TEST_CASE("overlay: a dug-out drift refills toward its steady state") {
  const WindRecord wind(WindParams{.seed = 23});
  const OverlayRules rules;
  DriftDecl drift;
  drift.from_x = 4'000;
  drift.from_z = 10'000;
  drift.to_x = 12'000;
  drift.to_z = 10'000;
  drift.normal_z_q14 = -16384;
  drift.height_mm = 500;
  drift.reach_mm = 2'500;
  drift.since_day = 3;
  Overlay overlay;
  overlay.reset(TileCoord{0, 0}, 32'000, 3 * k_day);
  // A wall built on day 3 declares its drift: the drift starts empty and the wind fills it.
  overlay.declare_drift(drift, wind, rules);
  const i64 at_face = overlay.deviation_mm(8'000, 10'000);
  CHECK(at_face == -500);
  i32 previous = -500;
  u32 days = 0;
  for (i64 d = 4; d < 60 && !overlay.empty(); ++d) {
    overlay.advance(d * k_day, wind, rules);
    const i32 now = overlay.deviation_mm(8'000, 10'000);
    CHECK(now >=
          previous);  // the residual shrinks every day: the drift grows toward its steady state
    previous = now;
    days = static_cast<u32>(d - 3);
  }
  MESSAGE("a 0.5 m drift reached its steady state in " << days << " days");
  CHECK(overlay.empty());
  CHECK(days > 2);
  // Then dug out by the player, and refilled again.
  overlay.push(pit(70 * k_day, 8'000, 9'500, 400, 800), wind, rules);
  overlay.advance(70 * k_day, wind, rules);
  CHECK(overlay.deviation_mm(8'000, 9'500) == -400);
  overlay.advance(overlay.burial_time_us(wind, rules), wind, rules);
  CHECK(overlay.nonzero_blocks() == 0);
}

TEST_CASE("overlay: rock takes no print") {
  FieldDesc desc;
  desc.ridges.push_back(RidgeFeature{0, 16'000, 32'000, 16'000, 20'000});
  const DuneField field(desc);
  const OverlayRules rules;
  Overlay overlay;
  overlay.reset(TileCoord{0, 0}, 32'000, 0);
  overlay.set_fill_rates(field);
  REQUIRE(overlay.push(footprint(k_hour, 16'000, 16'000), field.wind(), rules));
  REQUIRE(overlay.push(footprint(k_hour, 16'000, 1'000), field.wind(), rules));
  overlay.advance(k_hour, field.wind(), rules);
  CHECK(overlay.deviation_mm(16'000, 16'000) == 0);  // on the ridge's crest
  CHECK(overlay.deviation_mm(16'000, 1'000) < 0);    // on the sand at its foot
  CHECK(overlay.burial_time_us(field.wind(), rules) < 10 * k_day);
}

TEST_CASE("feedback: pits nudge the lag once a day, and it saturates") {
  const WindRecord wind(WindParams{.seed = 29});
  OverlayRules rules;
  rules.feedback.lag_max_units = 20;
  Overlay overlay;
  overlay.reset(TileCoord{0, 0}, 32'000, 0);
  // Two large pits on day 0: one nudge, not two.
  REQUIRE(overlay.push(pit(k_hour, 10'000, 10'000, 1'500, 1'500), wind, rules));
  REQUIRE(overlay.push(pit(2 * k_hour, 20'000, 20'000, 1'500, 1'500), wind, rules));
  overlay.advance(3 * k_hour, wind, rules);
  CHECK(overlay.lag().units[0] == 1);
  // A small hole never nudges.
  Overlay small;
  small.reset(TileCoord{0, 0}, 32'000, 0);
  REQUIRE(small.push(pit(k_hour, 10'000, 10'000, 200, 300), wind, rules));
  small.advance(2 * k_hour, wind, rules);
  CHECK(small.lag().zero());
  // A pit every day for a hundred days: the lag climbs a unit a day to its ceiling and stays there.
  for (i64 d = 1; d < 100; ++d) {
    REQUIRE(overlay.push(pit(d * k_day + k_hour, 16'000, 16'000, 1'800, 1'800), wind, rules));
    overlay.advance(d * k_day + 2 * k_hour, wind, rules);
    CHECK(overlay.lag().units[0] == std::min<i64>(d + 1, 20));
  }
  CHECK(overlay.lag().units[2] == 20);
  // The lag survives the burial of the pits that made it — it is the tile's few bytes of history.
  overlay.advance(overlay.burial_time_us(wind, rules), wind, rules);
  CHECK(overlay.nonzero_blocks() == 0);
  CHECK(overlay.lag().units[1] == 20);
}

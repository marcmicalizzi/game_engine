// The medium's sources on the CPU, with no device (docs/subsystems/volumetrics.md, "Testing"): the
// scene's block, the dust's function of the wind record and the visibility it gives, its
// closed-form optical depth against a sum along the ray, and a committed storm erg's calm, day and
// storm hours.

#include <core/json/json.h>
#include <domain/terrain/dunes.h>
#include <domain/terrain/scene_ground.h>
#include <domain/terrain/wind.h>
#include <systems/renderer/scene.h>
#include <systems/volumetrics/volumetrics.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::volumetrics;

namespace {

JsonValue parse(const char* text) {
  JsonValue out;
  REQUIRE(parse_json(text, out).ok);
  return out;
}

// The hours of a record a test reads: the strongest storm hour, a calm day's noon, and the day of
// the record nearest its mean strength at noon.
struct Hours {
  f64 storm_s = -1.0;
  f64 calm_s = -1.0;
  f64 day_s = -1.0;
};

Hours hours_of(const terrain::DuneField& field) {
  Hours out;
  const terrain::WindRecord& record = field.wind();
  i32 best_peak = 0;
  for (const terrain::WindStorm& storm : record.storms()) {
    if (storm.peak_q16 <= best_peak) continue;
    best_peak = storm.peak_q16;
    // The half sine peaks in the storm's middle hour.
    out.storm_s =
        (static_cast<f64>(storm.day) * 24.0 + storm.first_hour + storm.hours / 2) * 3600.0 + 1800.0;
  }
  f32 nearest = 1.0e9f;
  for (i64 d = 0; d < 400; ++d) {
    const terrain::WindDay& day = record.day(d);
    const f64 noon = (static_cast<f64>(d) * 24.0 + 12.0) * 3600.0;
    if (day.storm != 0) continue;
    if (day.calm != 0 && out.calm_s < 0.0) out.calm_s = noon;
    const MediumWind wind = wind_now(&field, noon);
    if (day.calm == 0 && std::fabs(wind.strength - 1.0f) < nearest) {
      nearest = std::fabs(wind.strength - 1.0f);
      out.day_s = noon;
    }
  }
  return out;
}

}  // namespace

TEST_CASE("volumetrics: a scene's medium block reads with its defaults and refuses what is wrong") {
  MediumDesc desc;
  bool present = true;
  std::string error;
  CHECK(read_medium(JsonValue{}, desc, present, &error));
  CHECK_FALSE(present);

  CHECK(read_medium(parse("{}"), desc, present, &error));
  CHECK(present);
  CHECK(desc.dust);
  CHECK(desc.plumes);
  CHECK_FALSE(desc.clouds);
  CHECK(desc.dust_height_m == 15.0f);

  CHECK(read_medium(parse(R"({"clouds":{"coverage":0.4,"seed":7},"dust":{"scale":2}})"), desc,
                    present, &error));
  CHECK(desc.clouds);
  CHECK(desc.cloud_coverage == doctest::Approx(0.4));
  CHECK(desc.cloud_seed == 7u);
  CHECK(desc.cloud_base_m == 1800.0f);
  CHECK(desc.dust_scale == 2.0f);

  error.clear();
  CHECK_FALSE(read_medium(parse(R"({"dust":{"thickness":2}})"), desc, present, &error));
  CHECK(error.find("thickness") != std::string::npos);
  error.clear();
  CHECK_FALSE(
      read_medium(parse(R"({"clouds":{"base_m":3000,"top_m":2000}})"), desc, present, &error));
  CHECK(error.find("base_m") != std::string::npos);

  MediumDebug debug = MediumDebug::none;
  CHECK(parse_medium_debug("froxels", debug));
  CHECK(debug == MediumDebug::dust);
  CHECK(parse_medium_debug("clouds", debug));
  CHECK(debug == MediumDebug::clouds);
  CHECK_FALSE(parse_medium_debug("fog", debug));
}

TEST_CASE(
    "volumetrics: the dust is a function of the wind: calm clear, the day a haze, a storm a wall") {
  const MediumDesc desc;
  MediumWind calm;
  calm.strength = 0.0f;
  MediumWind day;
  day.strength = 1.0f;
  MediumWind storm;
  storm.strength = k_dust_storm_peak;
  storm.storm = true;
  const DustState c = dust_state(desc, calm);
  const DustState d = dust_state(desc, day);
  const DustState s = dust_state(desc, storm);
  // A walker's eyes.
  const f32 eye = 1.7f;
  const f32 v_calm = visibility_m(dust_extinction(c, eye));
  const f32 v_day = visibility_m(dust_extinction(d, eye));
  const f32 v_storm = visibility_m(dust_extinction(s, eye));
  MESSAGE("visibility: calm " << v_calm << " m, the mean day " << v_day << " m, a storm's peak "
                              << v_storm << " m; scale heights " << c.height_m << ", " << d.height_m
                              << ", " << s.height_m << " m");
  CHECK(v_calm > 100000.0f);
  CHECK(v_day > 3000.0f);
  CHECK(v_day < 15000.0f);
  CHECK(v_storm < 100.0f);
  CHECK(v_storm > 10.0f);
  CHECK(v_storm * 10.0f < v_calm);
  // Saltation stays low, suspension rises: a few metres in a calm, tens in a storm.
  CHECK(c.height_m < 5.0f);
  CHECK(d.height_m == doctest::Approx(15.0));
  CHECK(s.height_m > 25.0f);
  // Stronger is never clearer.
  f32 last = 0.0f;
  for (f32 strength = 0.0f; strength <= 3.0f; strength += 0.1f) {
    MediumWind w;
    w.strength = strength;
    const f32 sigma = dust_state(desc, w).extinction_per_m;
    CHECK(sigma >= last);
    last = sigma;
  }
  // The block's scale multiplies it; off is none.
  MediumDesc twice;
  twice.dust_scale = 2.0f;
  CHECK(dust_state(twice, day).extinction_per_m ==
        doctest::Approx(2.0f * d.extinction_per_m).epsilon(1e-6));
  MediumDesc off;
  off.dust = false;
  CHECK(dust_state(off, storm).extinction_per_m == 0.0f);
  CHECK(visibility_m(0.0f) == k_max_visibility_m);
}

TEST_CASE("volumetrics: the dust's optical depth in closed form is its sum along the ray") {
  MediumWind wind;
  wind.strength = 1.6f;
  const DustState dust = dust_state(MediumDesc{}, wind);
  const f32 eye = 2.5f;
  // Up, level, gently down (crossing the dust's ground), and steeply down.
  const f32 dys[] = {0.6f, 0.05f, 0.0f, -0.02f, -0.5f};
  for (const f32 dy : dys) {
    for (const f32 b : {10.0f, 300.0f, 4000.0f}) {
      const f32 a = 0.5f;
      f64 sum = 0.0;
      const u32 steps = 200000;
      const f64 dt = static_cast<f64>(b - a) / steps;
      for (u32 i = 0; i < steps; ++i) {
        const f64 t = a + (i + 0.5) * dt;
        sum += static_cast<f64>(dust_extinction(
                   dust, static_cast<f32>(static_cast<f64>(eye) + t * static_cast<f64>(dy)))) *
               dt;
      }
      const f32 closed = dust_optical_depth(dust, eye, dy, a, b);
      INFO("dy " << dy << ", to " << b << " m");
      CHECK(closed == doctest::Approx(sum).epsilon(2e-3));
    }
  }
}

TEST_CASE("volumetrics: the storm erg's storm hour sees a tenth as far as its calm hour, or less") {
  JsonValue root;
  const std::string path =
      std::string(ENGINE_SOURCE_DIR) + "/content/test-scenes/desert-erg-storm/scene.json";
  renderer::SceneDesc scene;
  std::string error;
  REQUIRE_MESSAGE(renderer::read_scene_file(path, scene, error), error);
  REQUIRE(scene.terrain.enabled);
  renderer::TerrainSampler ground(scene.terrain);
  REQUIRE_MESSAGE(ground.ok(), ground.error());
  const terrain::DuneField* field = terrain::dune_field(ground.provider());
  REQUIRE(field != nullptr);
  const Hours hours = hours_of(*field);
  REQUIRE(hours.storm_s >= 0.0);
  REQUIRE(hours.calm_s >= 0.0);
  REQUIRE(hours.day_s >= 0.0);
  const MediumDesc desc;
  const f32 eye = 1.7f;
  auto seen = [&](f64 t, MediumWind& w) {
    w = wind_now(field, t);
    return visibility_m(dust_extinction(dust_state(desc, w), eye));
  };
  MediumWind wc, wd, ws;
  const f32 calm = seen(hours.calm_s, wc);
  const f32 day = seen(hours.day_s, wd);
  const f32 storm = seen(hours.storm_s, ws);
  MESSAGE("the storm erg: calm (" << wc.strength << ") " << calm << " m, a mean day ("
                                  << wd.strength << ") " << day << " m, its strongest storm ("
                                  << ws.strength << ", storm " << ws.storm << ") " << storm
                                  << " m");
  CHECK(ws.storm);
  CHECK(storm * 10.0f <= calm);
  CHECK(storm < day);
  // The same hour twice is the same wind and the same number: a function of the time.
  MediumWind again;
  CHECK(seen(hours.storm_s, again) == storm);
  CHECK(again.direction.x == ws.direction.x);
}

// How far the erg's big dunes move in a year (terrain.md, "How far the big dunes move";
// docs/experiments/time-lapse-smoothness-2026-09-27.md, "The big dunes' crests"). The owner, flying
// the erg at a week a second, saw the waves race and the towering dunes stand still, and asked
// whether they are anchored. The generator moves every primitive of a band by one displacement —
// Bagnold's rule, the wind's net flux over the band's celerity height, the middle of its height
// range — so a crest must advance by exactly the band's travel. This measures it on the committed
// scene (content/test-scenes/desert-erg), three years in: the tallest mega-draa and a draa at full
// weight, each by its slip face's mid-height crossing along a transect down the band's travel, at
// t, t + 1 year and t + 3 years, against the displacement and against Bagnold for the dune's own
// height.
#include <core/base/types.h>
#include <domain/terrain/dunes.h>
#include <domain/terrain/fixed.h>
#include <domain/terrain/scene_ground.h>
#include <domain/terrain/wind.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr i64 k_year_us = 365 * terrain::k_us_per_day;

struct Crest {
  f64 brink_m = 0.0;  // along the transect, where the sand is highest
  f64 face_m = 0.0;   // and where the slip face below it is at half the dune's height
  f64 height_m = 0.0;
  Vector<f64> above;  // the profile, metres above the floor
};

// The transect through (x0, z0) along (ux, uz), -span..+span metres at `step`, at `time_us`.
Crest crest_along(const terrain::DuneField& field, f64 x0, f64 z0, f64 ux, f64 uz, f64 span,
                  f64 step, i64 time_us) {
  const auto mm = [](f64 m) { return static_cast<i64>(std::llround(m * 1000.0)); };
  const f64 xa = x0 - ux * span;
  const f64 xb = x0 + ux * span;
  const f64 za = z0 - uz * span;
  const f64 zb = z0 + uz * span;
  terrain::Gather gather;
  field.gather(mm(std::min(xa, xb)), mm(std::min(za, zb)), mm(std::max(xa, xb)),
               mm(std::max(za, zb)), time_us, nullptr, gather);
  const u32 n = static_cast<u32>(2.0 * span / step) + 1;
  Vector<f64> above(n);
  u32 top = 0;
  for (u32 k = 0; k < n; ++k) {
    const f64 s = -span + static_cast<f64>(k) * step;
    const i64 x = mm(x0 + ux * s);
    const i64 z = mm(z0 + uz * s);
    above[k] = static_cast<f64>(field.height_um(gather, x, z, terrain::Detail::dunes) -
                                field.floor_um(x, z)) /
               1.0e6;
    if (above[k] > above[top]) top = k;
  }
  Crest c;
  c.above = above;
  c.brink_m = -span + static_cast<f64>(top) * step;
  c.height_m = above[top];
  const f64 half = 0.5 * above[top];
  c.face_m = span;
  for (u32 k = top + 1; k < n; ++k) {
    if (above[k] < half) {
      const f64 t = (above[k - 1] - half) / (above[k - 1] - above[k]);
      c.face_m = -span + (static_cast<f64>(k - 1) + t) * step;
      break;
    }
  }
  return c;
}

// The shift, in whole steps of the profile, that best lays `later` over `earlier`: the one with the
// least mean squared difference over the samples both cover, within +-`most` steps. The dune's
// bulk motion, which a crest re-forming under a new year's wind (its slip face turning, sharpening
// or rounding) moves far less than it moves the face or the brink.
f64 best_shift(const Vector<f64>& earlier, const Vector<f64>& later, i32 most, f64 step) {
  f64 best = 1.0e300;
  i32 at = 0;
  const i32 n = static_cast<i32>(earlier.size());
  for (i32 d = -most; d <= most; ++d) {
    f64 sum = 0.0;
    u32 count = 0;
    for (i32 k = most; k < n - most; ++k) {
      const f64 e = later[static_cast<u32>(k + d)] - earlier[static_cast<u32>(k)];
      sum += e * e;
      ++count;
    }
    const f64 mean = sum / static_cast<f64>(count);
    if (mean < best) {
      best = mean;
      at = d;
    }
  }
  return static_cast<f64>(at) * step;
}

}  // namespace

TEST_CASE("renderer: the erg's big dunes advance by their band's travel, as Bagnold says") {
  const std::string path =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/desert-erg/scene.json",
                      "content/test-scenes/desert-erg/scene.json");
  if (!test::path_exists(path)) {
    MESSAGE("the desert-erg scene is not here; skipped");
    return;
  }
  SceneDesc scene;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, scene, error), error);
  const TerrainSampler sampler(scene.terrain);
  const terrain::DuneField* field = terrain::dune_field(sampler.provider());
  REQUIRE(field != nullptr);
  const i64 t0 = static_cast<i64>(std::llround(scene.terrain.time_s * 1.0e6));
  const i64 half_extent_mm =
      static_cast<i64>(std::llround(static_cast<f64>(scene.terrain.extent) * 1000.0));

  // Every band's travel in a game year at its physical rate (celerity_scale 1), along the net flux.
  for (u32 b = 0; b < field->band_count(); ++b) {
    i64 ax = 0, az = 0, bx = 0, bz = 0;
    field->displacement(b, t0, ax, az);
    field->displacement(b, t0 + k_year_us, bx, bz);
    const f64 m = std::sqrt(static_cast<f64>(bx - ax) * static_cast<f64>(bx - ax) +
                            static_cast<f64>(bz - az) * static_cast<f64>(bz - az)) /
                  1000.0;
    char row[160];
    std::snprintf(row, sizeof row, "%s: %.2f m a game year (celerity height %.2f m)",
                  field->band_name(b), m, static_cast<f64>(field->band_height(b)) / 1000.0);
    MESSAGE(std::string(row));
  }
  struct Case {
    const char* band;
    f64 span_m;
    f64 step_m;
    bool long_run;  // measured over ten and thirty years too, by its whole profile
  };
  const Case cases[] = {{"mega-draa", 700.0, 0.25, true}, {"draa", 120.0, 0.05, false}};
  for (const Case& c : cases) {
    CAPTURE(c.band);
    u32 b = ~0u;
    for (u32 k = 0; k < field->band_count(); ++k)
      if (std::strcmp(field->band_name(k), c.band) == 0) b = k;
    REQUIRE(b != ~0u);
    // The tallest primitive of the band inside the scene at t0 — for the draa, one at full weight
    // (its band fades in across the flanks of the mega-draa, so a tall one on the floor is not
    // drawn).
    i64 dx0 = 0;
    i64 dz0 = 0;
    field->displacement(b, t0, dx0, dz0);
    const i64 cell = field->band_cell(b);
    const i64 lo_i = terrain::fx::floor_div(-half_extent_mm - dx0, cell);
    const i64 hi_i = terrain::fx::floor_div(half_extent_mm - dx0, cell);
    const i64 lo_j = terrain::fx::floor_div(-half_extent_mm - dz0, cell);
    const i64 hi_j = terrain::fx::floor_div(half_extent_mm - dz0, cell);
    terrain::Primitive best;
    bool found = false;
    for (i64 j = lo_j; j <= hi_j; ++j) {
      for (i64 i = lo_i; i <= hi_i; ++i) {
        terrain::Primitive p;
        if (!field->primitive(b, i, j, t0, p)) continue;
        if (p.kind != static_cast<u8>(terrain::PrimitiveKind::transverse)) continue;
        const i64 wx = p.cx + dx0;
        const i64 wz = p.cz + dz0;
        if (terrain::fx::abs_i64(wx) > half_extent_mm * 3 / 4 ||
            terrain::fx::abs_i64(wz) > half_extent_mm * 3 / 4)
          continue;
        if (found && p.height <= best.height) continue;
        if (field->band(b).couple != terrain::BandCouple::none) {
          terrain::Gather g;
          field->gather(wx, wz, wx, wz, t0, nullptr, g);
          if (field->band_weight_q16(g, b, wx, wz) < 32'768) continue;
        }
        best = p;
        found = true;
      }
    }
    REQUIRE(found);
    // Down the band's travel over the three years.
    i64 dx3 = 0;
    i64 dz3 = 0;
    field->displacement(b, t0 + 3 * k_year_us, dx3, dz3);
    const f64 tx = static_cast<f64>(dx3 - dx0);
    const f64 tz = static_cast<f64>(dz3 - dz0);
    const f64 travel3 = std::sqrt(tx * tx + tz * tz) / 1000.0;
    REQUIRE(travel3 > 0.0);
    const f64 ux = tx / 1000.0 / travel3;
    const f64 uz = tz / 1000.0 / travel3;
    i64 dx1 = 0;
    i64 dz1 = 0;
    field->displacement(b, t0 + k_year_us, dx1, dz1);
    const f64 travel1 =
        (static_cast<f64>(dx1 - dx0) * ux + static_cast<f64>(dz1 - dz0) * uz) / 1000.0;
    const f64 x0 = static_cast<f64>(best.cx + dx0) / 1000.0;
    const f64 z0 = static_cast<f64>(best.cz + dz0) / 1000.0;
    const Crest at0 = crest_along(*field, x0, z0, ux, uz, c.span_m, c.step_m, t0);
    const Crest at1 = crest_along(*field, x0, z0, ux, uz, c.span_m, c.step_m, t0 + k_year_us);
    const Crest at3 = crest_along(*field, x0, z0, ux, uz, c.span_m, c.step_m, t0 + 3 * k_year_us);
    // Bagnold for this dune's own height: the net flux along the travel over the height.
    const terrain::FluxIntegral q = field->wind().between(t0, t0 + k_year_us);
    const f64 net_m2 =
        (static_cast<f64>(q.x) * ux + static_cast<f64>(q.z) * uz) / 10'000.0;  // cm^2 to m^2
    const f64 own = net_m2 / (static_cast<f64>(best.height) / 1.0e6);
    const i32 most = static_cast<i32>(4.0 * travel3 / c.step_m) + 8;
    const f64 bulk1 = best_shift(at0.above, at1.above, most, c.step_m);
    const f64 bulk3 = best_shift(at0.above, at3.above, most, c.step_m);
    char line[560];
    std::snprintf(line, sizeof line,
                  "%s: a %.0f m dune (drawn %.0f m above the floor), band height %.1f m; net flux "
                  "%.0f m^2 a year; band travel %.2f m in a year, %.2f m in three; Bagnold for its "
                  "own height %.2f m a year; the profile moved %.2f m in a year, %.2f in three; "
                  "its slip face %.2f and %.2f, its brink %.2f and %.2f",
                  c.band, static_cast<f64>(best.height) / 1.0e6, at0.height_m,
                  static_cast<f64>(field->band_height(b)) / 1000.0, net_m2, travel1, travel3, own,
                  bulk1, bulk3, at1.face_m - at0.face_m, at3.face_m - at0.face_m,
                  at1.brink_m - at0.brink_m, at3.brink_m - at0.brink_m);
    MESSAGE(std::string(line));
    // Ten and thirty years: long enough for the translation to outgrow the re-forming.
    f64 bulk_long[2] = {};
    f64 travel_long[2] = {};
    const i64 spans[2] = {10, 30};
    for (u32 k = 0; k < 2 && c.long_run; ++k) {
      i64 dxn = 0;
      i64 dzn = 0;
      field->displacement(b, t0 + spans[k] * k_year_us, dxn, dzn);
      travel_long[k] =
          (static_cast<f64>(dxn - dx0) * ux + static_cast<f64>(dzn - dz0) * uz) / 1000.0;
      const Crest atn =
          crest_along(*field, x0, z0, ux, uz, c.span_m, c.step_m, t0 + spans[k] * k_year_us);
      bulk_long[k] = best_shift(at0.above, atn.above,
                                static_cast<i32>(4.0 * travel_long[k] / c.step_m) + 80, c.step_m);
      std::snprintf(line, sizeof line, "  %lld years: band travel %.2f m, the profile moved %.2f",
                    static_cast<long long>(spans[k]), travel_long[k], bulk_long[k]);
      MESSAGE(std::string(line));
    }
    if (c.long_run) {
      // The mega-draa's slip face re-forms with each year's wind (it remembers a year and sharpens
      // over four months), which swings its face and brink by ten metres and more a year while its
      // lattice moves 1.4: over a year the translation is lost in the re-forming, over ten and
      // thirty the profile follows the band to within that swing, and forward.
      CHECK(bulk_long[1] > 0.0);
      CHECK(std::abs(bulk_long[0] - travel_long[0]) < 15.0);
      CHECK(std::abs(bulk_long[1] - travel_long[1]) < 15.0);
    } else {
      // A draa rides on a mega-draa's flank, whose profile the bulk shift follows; its brink is
      // its own, and goes exactly where the band goes.
      CHECK(at1.brink_m - at0.brink_m == doctest::Approx(travel1).epsilon(0.05));
      CHECK(at3.brink_m - at0.brink_m == doctest::Approx(travel3).epsilon(0.05));
    }
  }
}

TEST_CASE("renderer: how fast the erg's bands walk in a storm, gain by gain, at the game's rate") {
  // The number the owner chooses a storm's transport gain by (terrain.md, "A storm scales
  // transport"; docs/experiments/wind-day-and-storm-gain-2026-09-28.md): at one game second a real
  // second, metres a second each band travels over its storms' hours — the mean over the record's
  // storms and the strongest hour of any — at a gain of 1, 10, 50 and 250, beside a calm day's
  // mean.
  const std::string path =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/desert-erg/scene.json",
                      "content/test-scenes/desert-erg/scene.json");
  if (!test::path_exists(path)) {
    MESSAGE("the desert-erg scene is not here; skipped");
    return;
  }
  SceneDesc scene;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, scene, error), error);
  REQUIRE(scene.terrain.storms_per_year > 0u);
  constexpr i64 k_hour_us = 3'600 * terrain::k_us_per_second;
  f64 barchan_at_1 = 0.0;
  for (const f32 gain : {1.0f, 10.0f, 50.0f, 250.0f}) {
    TerrainDesc desc = scene.terrain;
    desc.storm_gain = gain;
    const TerrainSampler sampler(desc);
    const terrain::DuneField* field = terrain::dune_field(sampler.provider());
    REQUIRE(field != nullptr);
    const terrain::WindRecord& wind = field->wind();
    // The storms' own transport per second: over each storm's hours, and its strongest hour.
    f64 storm_sum = 0.0;
    f64 storm_seconds = 0.0;
    f64 strongest = 0.0;
    for (const terrain::WindStorm& st : wind.storms()) {
      for (i32 h = st.first_hour; h < st.first_hour + st.hours; ++h) {
        const i64 t = static_cast<i64>(st.day) * terrain::k_us_per_day + h * k_hour_us;
        const f64 cm2 = static_cast<f64>(wind.between(t, t + k_hour_us).magnitude);
        storm_sum += cm2;
        storm_seconds += 3'600.0;
        strongest = std::max(strongest, cm2 / 3'600.0);
      }
    }
    const f64 storm_mean = storm_sum / storm_seconds;  // cm^2 a second
    // A calm day's: the record's mean day.
    const f64 day_mean = static_cast<f64>(wind.period_total().magnitude) /
                         (static_cast<f64>(terrain::k_record_days) * 86'400.0);
    char line[400];
    i32 n = std::snprintf(line, sizeof line, "gain %4.0f:", static_cast<f64>(gain));
    for (const char* name : {"barchan", "crest", "draa", "mega-draa"}) {
      u32 b = ~0u;
      for (u32 k = 0; k < field->band_count(); ++k)
        if (std::strcmp(field->band_name(k), name) == 0) b = k;
      REQUIRE(b != ~0u);
      // Bagnold's rule as `DuneField::displacement` takes it: cm^2 over the celerity height in mm,
      // times 100 for mm^2; millimetres to metres.
      const f64 height_mm = static_cast<f64>(field->band_height(b));
      const auto mps = [&](f64 cm2_per_s) { return cm2_per_s * 100.0 / height_mm / 1000.0; };
      n += std::snprintf(line + n, sizeof line - static_cast<usize>(n),
                         " %s %.2e m/s in a storm (strongest hour %.2e, a mean day %.2e);", name,
                         mps(storm_mean), mps(strongest), mps(day_mean));
      if (gain == 1.0f && std::strcmp(name, "barchan") == 0) barchan_at_1 = mps(storm_mean);
      if (gain == 250.0f && std::strcmp(name, "barchan") == 0) {
        // The gain multiplies the storm's share of its hours; the day's own share rides along.
        CHECK(mps(storm_mean) > 200.0 * barchan_at_1);
        CHECK(mps(storm_mean) < 250.0 * barchan_at_1);
      }
    }
    MESSAGE(std::string(line));
    // The renderer's cadence reads the same travel: over a storm's hours it is the gained one.
    const terrain::WindStorm& st = wind.storms()[0];
    const f64 from = static_cast<f64>(st.day) * 86'400.0 + st.first_hour * 3'600.0;
    const f64 to = from + st.hours * 3'600.0;
    const f64 travel = terrain_band_travel_m(sampler, from, to);
    const i64 cm2 =
        wind.between(static_cast<i64>(from * 1e6), static_cast<i64>(to * 1e6)).magnitude;
    f64 most = 0.0;
    for (u32 b = 0; b < field->band_count(); ++b)
      most = std::max(
          most, static_cast<f64>(cm2) * 100.0 / static_cast<f64>(field->band_height(b)) / 1000.0);
    CHECK(travel == doctest::Approx(most));
  }
}

TEST_CASE("renderer: the storm erg's barchans walk through its storm and stand after it") {
  // content/test-scenes/desert-erg-storm: the erg with the wind's day on and a storm gain of 1,000,
  // starting at 12:50 in the ten-hour storm that blows from 09:00 of day 1,095 (its README).
  const std::string path =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/desert-erg-storm/scene.json",
                      "content/test-scenes/desert-erg-storm/scene.json");
  if (!test::path_exists(path)) {
    MESSAGE("the desert-erg-storm scene is not here; skipped");
    return;
  }
  SceneDesc scene;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, scene, error), error);
  const TerrainSampler sampler(scene.terrain);
  const terrain::DuneField* field = terrain::dune_field(sampler.provider());
  REQUIRE(field != nullptr);
  REQUIRE(field->wind().diurnal());
  const i64 t0 = static_cast<i64>(std::llround(scene.terrain.time_s * 1.0e6));
  constexpr i64 k_hour_us = 3'600 * terrain::k_us_per_second;
  CHECK(field->wind().wind_at(t0).storm);  // blowing from the first frame
  CHECK(field->wind().wind_at(t0).gain_q16 == 1000 * 65536);
  const auto walk = [&](u32 band, i64 from, i64 to) {
    i64 x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    field->displacement(band, from, x0, z0);
    field->displacement(band, to, x1, z1);
    return std::sqrt(static_cast<f64>(x1 - x0) * static_cast<f64>(x1 - x0) +
                     static_cast<f64>(z1 - z0) * static_cast<f64>(z1 - z0)) /
           1000.0;
  };
  u32 barchan = ~0u;
  for (u32 k = 0; k < field->band_count(); ++k)
    if (std::strcmp(field->band_name(k), "barchan") == 0) barchan = k;
  REQUIRE(barchan != ~0u);
  const i64 start = terrain::day_of(t0) * terrain::k_us_per_day + 9 * k_hour_us;  // 09:00
  CHECK_FALSE(field->wind().wind_at(start - k_hour_us).storm);
  // Hour by hour through the storm: the barchans' speed, millimetres a second at the game's rate.
  std::string hours = "the barchans' speed through the storm, mm/s from 09:00:";
  for (i64 h = 0; h < 10; ++h) {
    char cell[16];
    std::snprintf(cell, sizeof cell, " %.2f",
                  walk(barchan, start + h * k_hour_us, start + (h + 1) * k_hour_us) / 3.6);
    hours += cell;
  }
  MESSAGE(hours);
  const f64 storm = walk(barchan, start, start + 10 * k_hour_us);
  const f64 after = walk(barchan, start + 10 * k_hour_us, start + 20 * k_hour_us);
  MESSAGE("the barchans walk " << storm << " m through the storm's ten hours and " << after
                               << " m in the ten after it");
  CHECK(storm > 150.0);
  CHECK(after < 1.0);
}

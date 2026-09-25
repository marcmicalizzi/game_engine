// The dune field's invariants (docs/subsystems/terrain.md, "Invariants"): the integer arithmetic
// it rests on, the wind record's closed form against the day-by-day sum a simulation would add up,
// the field as a function of time (no state, t2 directly equal to t1 then t2), migration at
// Bagnold's rate, seams between tiles at every time, the sampler against the mesh, the fixed
// features, the lag, the LOD policy, the effect outputs, thread-count independence, and the golden
// hashes of a reference tile at three times that MSVC, GCC and Clang must all reproduce.
#include <core/jobs/job_system.h>
#include <domain/terrain/fixed.h>
#include <domain/terrain/terrain.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

using namespace engine;
using namespace engine::terrain;

namespace {

constexpr i64 k_day = k_us_per_day;
constexpr i64 k_year = 365 * k_day;

// The reference field of the golden hashes: the desert overlook's scale (3 m dunes, 90 m apart),
// seed 2026, one ridge and one basin near the reference tile so both features are in its numbers.
FieldDesc reference_desc() {
  FieldDesc d;
  d.seed = 2026;
  d.wind.seed = 2026;
  d.ridges.push_back(RidgeFeature{-40'000, 90'000, 120'000, 60'000, 30'000});
  d.basins.push_back(BasinFeature{10'000, -20'000, 25'000});
  return d;
}

std::string hex(u64 v) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(v));
  return text;
}

}  // namespace

TEST_CASE("terrain: the integer sine is the sine") {
  CHECK(fx::sin_q15(0) == 0);
  CHECK(fx::sin_q15(16384) == 32768);
  CHECK(fx::sin_q15(32768) == 0);
  CHECK(fx::sin_q15(49152) == -32768);
  CHECK(fx::cos_q15(0) == 32768);
  i32 worst = 0;
  for (u32 a = 0; a < 65536; a += 7) {
    const f64 truth =
        std::sin(static_cast<f64>(a) * 2.0 * 3.14159265358979323846 / 65536.0) * 32768.0;
    worst = std::max(worst, static_cast<i32>(std::fabs(truth - fx::sin_q15(a)) + 0.5));
  }
  MESSAGE("integer sine: worst error " << worst << " in Q15");
  CHECK(worst <= 3);
  for (u64 v : {u64{0}, u64{1}, u64{2}, u64{3}, u64{4}, u64{99}, u64{100}, u64{1} << 40,
                (u64{1} << 62) + 12345, u64{999'999'999'999}}) {
    const u64 r = fx::isqrt(v);
    CHECK(r * r <= v);
    CHECK((r + 1) * (r + 1) > v);
  }
  CHECK(fx::floor_div(-1, 10) == -1);
  CHECK(fx::floor_mod(-1, 10) == 9);
  CHECK(fx::falloff_q16(0) == 65536);
  CHECK(fx::falloff_q16(65536) == 0);
  CHECK(fx::falloff_q16(32768) == 32768);
}

TEST_CASE("terrain: the wind record's integral is the day-by-day sum, at any time") {
  const WindRecord wind(WindParams{.seed = 7});
  // A stepwise simulation adds the days up one by one; the closed form must be that number exactly,
  // across the record's period and before the epoch.
  FluxIntegral sum;
  for (i64 d = 0; d <= 2 * k_record_days + 40; ++d) {
    CHECK(wind.integral(d * k_day) == sum);
    const WindDay& day = wind.day(d);
    sum.x += day.fx;
    sum.z += day.fz;
    sum.magnitude += day.magnitude;
  }
  FluxIntegral back;
  for (i64 d = -1; d >= -400; --d) {
    const WindDay& day = wind.day(d);
    back.x -= day.fx;
    back.z -= day.fz;
    back.magnitude -= day.magnitude;
    CHECK(wind.integral(d * k_day) == back);
  }
  // Between two times is the difference, so any split adds up.
  const i64 t0 = 3 * k_day + 12345;
  const i64 t1 = 400 * k_day + 999;
  const i64 t2 = 5000 * k_day + 77;
  const FluxIntegral whole = wind.between(t0, t2);
  const FluxIntegral a = wind.between(t0, t1);
  const FluxIntegral b = wind.between(t1, t2);
  CHECK(whole.x == a.x + b.x);
  CHECK(whole.z == a.z + b.z);
  CHECK(whole.magnitude == a.magnitude + b.magnitude);
  // Continuous within a day: an hour moves an hour's worth.
  const FluxIntegral hour = wind.between(10 * k_day, 10 * k_day + k_day / 24);
  CHECK(fx::abs_i64(hour.magnitude - wind.day(10).magnitude / 24) <= 1);
  // The mean is the one asked for, and some days are calm.
  const i64 mean = wind.period_total().magnitude / k_record_days;
  CHECK(fx::abs_i64(mean - wind.params().flux_cm2_per_day) <= 2);
  u32 calm = 0;
  for (i64 d = 0; d < k_record_days; ++d)
    calm += wind.day(d).calm;
  MESSAGE("wind: mean " << mean << " cm^2/day, " << calm << " calm days of " << k_record_days);
  CHECK(calm > k_record_days / 20);
  CHECK(calm < k_record_days / 4);
  // A thousand years on costs three lookups and is the same arithmetic.
  const FluxIntegral far = wind.integral(1000 * k_year + 17);
  CHECK(far.magnitude > 0);
}

TEST_CASE("terrain: the field is a function of time — t2 directly is t1 then t2") {
  const FieldDesc desc = reference_desc();
  const DuneField a(desc);
  const DuneField b(desc);
  TileOptions options;
  options.cells = 32;
  TileOutput t1, t2_direct, t2_after;
  evaluate_tile(a, TileCoord{0, 0}, 40 * k_day, options, nullptr, nullptr, t1);
  evaluate_tile(a, TileCoord{0, 0}, 3 * k_year + 5 * k_day, options, nullptr, nullptr, t2_after);
  evaluate_tile(b, TileCoord{0, 0}, 3 * k_year + 5 * k_day, options, nullptr, nullptr, t2_direct);
  CHECK(t2_direct.hash() == t2_after.hash());
  CHECK(t2_direct.height_um == t2_after.height_um);
  CHECK(t1.hash() != t2_direct.hash());
  // And going back in time is as good as going forward: nothing was advanced.
  TileOutput t1_again;
  evaluate_tile(a, TileCoord{0, 0}, 40 * k_day, options, nullptr, nullptr, t1_again);
  CHECK(t1_again.hash() == t1.hash());
}

TEST_CASE("terrain: bands migrate downwind at the flux over their height") {
  const DuneField field(reference_desc());
  const i64 t = 2 * k_year;
  i64 dx[k_bands], dz[k_bands], len[k_bands];
  for (u32 b = 0; b < k_bands; ++b) {
    field.displacement(static_cast<Band>(b), t, dx[b], dz[b]);
    len[b] = fx::length(dx[b], dz[b]);
    MESSAGE(std::string(band_name(static_cast<Band>(b)))
            << " moved " << len[b] / 1000 << " m in two years");
  }
  // Bagnold: the lower the band, the faster; the ratio of speeds is the inverse ratio of heights.
  CHECK(len[2] > len[1]);
  CHECK(len[1] > len[0]);
  const f64 ratio = static_cast<f64>(len[2]) / static_cast<f64>(len[0]);
  const f64 heights = static_cast<f64>(field.band_height(Band::draa)) /
                      static_cast<f64>(field.band_height(Band::barchan));
  CHECK(ratio == doctest::Approx(heights).epsilon(0.001));
  // Downwind: the displacement points within the seasonal swing of the prevailing direction.
  const i64 along =
      (dx[0] * field.wind().prevailing_x_q14() + dz[0] * field.wind().prevailing_z_q14()) >> 14;
  CHECK(along > len[0] * 8 / 10);
  // A barchan of the default field (1 m) in the default wind (200 m^2 a year) moves on the order
  // of a couple of hundred metres a year: visible over a week of play.
  CHECK(len[2] / 2 > 100'000);
  CHECK(len[2] / 2 < 1'000'000);
  // The field moves with the band: a primitive's surface at t is its surface at 0, displaced.
  Primitive p0, p1;
  REQUIRE(field.primitive(Band::draa, 0, 0, 0, p0));
  REQUIRE(field.primitive(Band::draa, 0, 0, t, p1));
  CHECK(p0.cx == p1.cx);  // the same cell, the same centre in the band's own frame
}

TEST_CASE("terrain: tiles meet without a seam, at every time") {
  const DuneField field(reference_desc());
  TileOptions options;
  options.cells = 64;
  const u32 v = options.cells + 1;
  for (const i64 t : {i64{0}, 90 * k_day + 3600 * k_us_per_second, 7 * k_year + 11}) {
    for (const TileCoord base : {TileCoord{0, 0}, TileCoord{-1, 2}, TileCoord{3, -1}}) {
      TileOutput here, east, north;
      evaluate_tile(field, base, t, options, nullptr, nullptr, here);
      evaluate_tile(field, TileCoord{base.x + 1, base.z}, t, options, nullptr, nullptr, east);
      evaluate_tile(field, TileCoord{base.x, base.z + 1}, t, options, nullptr, nullptr, north);
      u32 bad = 0;
      for (u32 k = 0; k < v; ++k) {
        const u32 e_here = k * v + (v - 1);
        const u32 e_there = k * v;
        bad += here.height_um[e_here] != east.height_um[e_there];
        bad += !(here.normals[e_here] == east.normals[e_there]);
        bad += here.material[e_here] != east.material[e_there];
        const u32 n_here = (v - 1) * v + k;
        const u32 n_there = k;
        bad += here.height_um[n_here] != north.height_um[n_there];
        bad += !(here.normals[n_here] == north.normals[n_there]);
        bad += here.material[n_here] != north.material[n_there];
      }
      CHECK(bad == 0);
    }
  }
}

TEST_CASE("terrain: the sampler answers what the mesh has") {
  const DuneField field(reference_desc());
  TileOptions options;
  options.cells = 32;
  const i64 t = 200 * k_day;
  TileOutput tile;
  evaluate_tile(field, TileCoord{1, 0}, t, options, nullptr, nullptr, tile);
  Vector<Vec3> positions, normals;
  Vector<u32> indices;
  build_tile_mesh(tile, positions, normals, indices);
  CHECK(indices.size() == options.cells * options.cells * 6);
  TileSampler sampler(field, t, options);
  u32 bad = 0;
  for (u32 k = 0; k < positions.size(); ++k) {
    const Vec3& p = positions[k];
    bad += sampler.height(p.x, p.z) != p.y;
    bad += !(sampler.normal(p.x, p.z) == normals[k]);
  }
  CHECK(bad == 0);
  // The point query without a sampler is the same number too.
  const Vec3& mid = positions[positions.size() / 2];
  CHECK(height_m(field.height_um(to_mm(mid.x), to_mm(mid.z), t)) == mid.y);
  // Counter-clockwise seen from +y: the first triangle's normal points up.
  const Vec3 a = positions[indices[0]], b = positions[indices[1]], c = positions[indices[2]];
  const f32 ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
  CHECK(ny > 0.0f);
}

TEST_CASE("terrain: the ruins stand on the floor, which does not move") {
  const DuneField field(reference_desc());
  const f32 x = 13.25f;
  const f32 z = -7.5f;
  const f32 ground = DuneField::ground_height(&field, x, z);
  CHECK(ground == height_m(field.floor_um(to_mm(x), to_mm(z))));
  CHECK(field.height_um(to_mm(x), to_mm(z), 0, Detail::floor) ==
        field.height_um(to_mm(x), to_mm(z), 10 * k_year, Detail::floor));
  // Sand is never below the floor: the dunes are on it.
  TileOptions options;
  options.cells = 32;
  for (const i64 t : {i64{0}, 5 * k_year}) {
    Gather g;
    field.gather(0, 0, 32'000, 32'000, t, nullptr, g);
    for (i64 z_mm = 0; z_mm <= 32'000; z_mm += 1000) {
      for (i64 x_mm = 0; x_mm <= 32'000; x_mm += 1000) {
        const Sample s = field.sample(g, x_mm, z_mm, Detail::dunes);
        CHECK(s.sand >= 0);
      }
    }
  }
}

TEST_CASE("terrain: the dunes are dunes — relief, crests, slip faces") {
  const DuneField field(reference_desc());
  TileOptions options;
  options.cells = 128;
  i64 lowest = 0, highest = 0;
  u32 crests = 0, barchans = 0;
  f32 steepest = 0.0f;
  u32 sand = 0, over_repose = 0;
  for (i32 tz = -3; tz <= 3; ++tz) {
    for (i32 tx = -3; tx <= 3; ++tx) {
      TileOutput tile;
      evaluate_tile(field, TileCoord{tx, tz}, 3 * k_year, options, nullptr, nullptr, tile);
      lowest = std::min<i64>(lowest, tile.min_um);
      highest = std::max<i64>(highest, tile.max_um);
      crests += tile.crests.size();
      for (const CrestLine& c : tile.crests) {
        barchans += c.kind == static_cast<u8>(PrimitiveKind::barchan);
        CHECK(std::fabs(std::sqrt(c.lee.x * c.lee.x + c.lee.y * c.lee.y) - 1.0f) < 1e-3f);
        CHECK(c.height_m > 0.0f);
      }
      for (u32 k = 0; k < tile.normals.size(); ++k) {
        if (tile.material[k] != 0) continue;
        const f32 degrees = std::acos(tile.normals[k].y) * 57.29578f;
        steepest = std::max(steepest, degrees);
        ++sand;
        over_repose += degrees > 36.0f;
      }
      CHECK(tile.wind.speed_mps >= 0.0f);
      CHECK(tile.wind.saltation_m2_per_day <= tile.wind.flux_m2_per_day);
    }
  }
  MESSAGE("dunes over 49 tiles: " << height_m(lowest) << " to " << height_m(highest) << " m, "
                                  << crests << " crest lines (" << barchans
                                  << " barchan brinks), steepest sand " << steepest << " degrees, "
                                  << over_repose << " of " << sand << " sand vertices over 36");
  CHECK(height_m(highest - lowest) > 2.0f);
  CHECK(height_m(highest) < 12.0f);
  CHECK(crests > 20);
  CHECK(barchans > 0);
  // A slip face stands at the angle of repose. Where a crest's slip face coincides with a draa's
  // the two bands' slopes add, and the sum can pass it — a known limit of superposing bands
  // (terrain.md, "Not yet"), measured here so a change that makes it worse fails: 0.48% of these 49
  // tiles' sand vertices over 36 degrees when it was written, the worst 53.5.
  CHECK(steepest > 30.0f);
  CHECK(steepest < 55.0f);
  CHECK(over_repose * 100u < sand);
}

TEST_CASE("terrain: a reversing wind moves the slip face to the other side, continuously") {
  FieldDesc desc;
  desc.seed = 5;
  desc.wind.seed = 5;
  desc.wind.seeded_direction = false;
  desc.wind.prevailing_turn = 0;
  desc.wind.swing_turn = 32768;  // half a turn either way over the year: a seasonal reversal
  desc.wind.jitter_turn = 0;
  const DuneField field(desc);
  i32 lowest = 65536, highest = -65536;
  i32 previous = 0;
  i32 largest_step = 0;
  for (i64 d = 0; d < 365; ++d) {
    Primitive p;
    REQUIRE(field.primitive(Band::crest, 3, 4, d * k_day, p));
    lowest = std::min(lowest, p.side_q16);
    highest = std::max(highest, p.side_q16);
    if (d > 0) largest_step = std::max(largest_step, std::abs(p.side_q16 - previous));
    previous = p.side_q16;
  }
  MESSAGE("slip face side over a reversing year: " << lowest << " .. " << highest
                                                   << ", largest daily step " << largest_step);
  CHECK(lowest < 0);
  CHECK(highest > 0);
  CHECK(largest_step < 65536);  // never a flip from one side to the other in a day
}

TEST_CASE("terrain: ridges hold the sand back and thin it; basins flatten it") {
  const FieldDesc desc = reference_desc();
  const DuneField field(desc);
  // On the ridge's segment the sand is thinned to 40% and the lattice held back.
  const i64 mx = (desc.ridges[0].from_x + desc.ridges[0].to_x) / 2;
  const i64 mz = (desc.ridges[0].from_z + desc.ridges[0].to_z) / 2;
  CHECK(field.ridge_q16(mx, mz) == 65536);
  CHECK(field.ridge_q16(mx + 1'000'000, mz) == 0);
  CHECK(material_at(field, mx, mz) == 1);
  CHECK(material_at(field, desc.basins[0].x, desc.basins[0].z) == 3);
  FieldDesc bare = desc;
  bare.ridges.clear();
  const DuneField plain(bare);
  u32 differs_near = 0, differs_far = 0;
  for (i64 k = -20; k <= 20; ++k) {
    differs_near += field.height_um(mx + k * 2000, mz + 5000, 0) !=
                    plain.height_um(mx + k * 2000, mz + 5000, 0);
    differs_far += field.height_um(mx + k * 2000, mz + 400'000, 0) !=
                   plain.height_um(mx + k * 2000, mz + 400'000, 0);
  }
  CHECK(differs_near > 0);
  CHECK(differs_far == 0);
  // At a basin's centre the dunes are flat: the height is the floor.
  const i64 bx = desc.basins[0].x, bz = desc.basins[0].z;
  for (const i64 t : {i64{0}, 3 * k_year}) {
    Gather g;
    field.gather(bx, bz, bx, bz, t, nullptr, g);
    CHECK(field.sample(g, bx, bz, Detail::dunes).sand == 0);
  }
}

TEST_CASE("terrain: a lag field is continuous, exact at tile centres, and bounded") {
  FeedbackRules rules;
  LagField lag(32'000, rules);
  lag.set(TileCoord{0, 0}, TileLag{{10, 20, 30}, 0});
  lag.set(TileCoord{1, 0}, TileLag{{200, 0, 0}, 0});  // over the ceiling: clamped
  CHECK(lag.get(TileCoord{1, 0}).units[0] == rules.lag_max_units);
  CHECK(lag.lag_mm(0, 16'000, 16'000) == 10 * rules.lag_unit_mm);
  CHECK(lag.lag_mm(2, 16'000, 16'000) == 30 * rules.lag_unit_mm);
  // Continuous: no step between neighbouring millimetres anywhere across the two tiles.
  i64 largest_step = 0;
  i64 previous = lag.lag_mm(0, -20'000, 16'000);
  for (i64 x = -19'999; x <= 90'000; ++x) {
    const i64 now = lag.lag_mm(0, x, 16'000);
    largest_step = std::max(largest_step, fx::abs_i64(now - previous));
    previous = now;
  }
  CHECK(largest_step <= 1);
  CHECK(lag.max_lag_mm() == rules.lag_max_units * rules.lag_unit_mm);
  lag.set(TileCoord{0, 0}, TileLag{});
  CHECK(lag.size() == 1);
  // The field reads it: near the lagged tile the dunes are held back; far from it, untouched.
  const DuneField field(reference_desc());
  u32 near_moved = 0, far_moved = 0;
  for (i64 k = 0; k < 32; ++k) {
    near_moved += field.height_um(48'000, k * 1000, k_year, Detail::dunes, &lag) !=
                  field.height_um(48'000, k * 1000, k_year, Detail::dunes, nullptr);
    far_moved += field.height_um(500'000, k * 1000, k_year, Detail::dunes, &lag) !=
                 field.height_um(500'000, k * 1000, k_year, Detail::dunes, nullptr);
  }
  CHECK(near_moved > 0);
  CHECK(far_moved == 0);
}

TEST_CASE("terrain: drifts, derived lag and its saturation") {
  FeedbackRules rules;
  DriftDecl wall;
  wall.from_x = 0;
  wall.to_x = 6'000;
  wall.normal_z_q14 = 16384;  // the face looks along +z
  wall.height_mm = 400;
  wall.reach_mm = 2000;
  wall.since_day = 10;
  CHECK(drift_height_um(wall, 3000, 0) == 400'000);
  CHECK(drift_height_um(wall, 3000, 1000) == 200'000);
  CHECK(drift_height_um(wall, 3000, -10) == 0);  // behind the face
  CHECK(drift_height_um(wall, 7000, 100) == 0);  // off its end
  CHECK(drift_volume_mm3(wall) == i64{6000} * 400 * 2000 / 2);
  const DriftDecl walls[] = {wall};
  CHECK(derived_lag_units(walls, 5, rules) == 0);
  CHECK(derived_lag_units(walls, 10, rules) == 0);
  CHECK(derived_lag_units(walls, 11, rules) == 1);
  CHECK(derived_lag_units(walls, 10 + 50, rules) == 50);
  CHECK(derived_lag_units(walls, 10'000, rules) == static_cast<u32>(rules.lag_max_units));
  DriftDecl small = wall;
  small.height_mm = 10;
  const DriftDecl smalls[] = {small};
  CHECK(derived_lag_units(smalls, 10'000, rules) == 0);  // too small to hold a dune back
}

TEST_CASE("terrain: the LOD policy") {
  CHECK(detail_for_spacing(10) == Detail::full);
  CHECK(detail_for_spacing(250) == Detail::dunes);
  CHECK(detail_for_distance(100'000) == Detail::dunes);
  CHECK(detail_for_distance(5'000'000) == Detail::coarse);
  // Coarse drops the barchans and nothing else; floor is the floor.
  const DuneField field(reference_desc());
  Gather g;
  field.gather(-64'000, -64'000, 96'000, 96'000, k_year, nullptr, g);
  u32 differ = 0;
  u32 points = 0;
  for (i64 z = -64'000; z <= 96'000; z += 1000) {
    for (i64 x = -64'000; x <= 96'000; x += 1000) {
      ++points;
      const i64 dunes = field.height_um(g, x, z, Detail::dunes);
      const i64 coarse = field.height_um(g, x, z, Detail::coarse);
      CHECK(coarse <= dunes);
      differ += coarse != dunes;
      CHECK(field.height_um(g, x, z, Detail::floor) == field.floor_um(x, z));
      // Ripples are millimetres.
      CHECK(fx::abs_i64(field.height_um(g, x, z, Detail::full) - dunes) <= 8'000);
    }
  }
  MESSAGE("coarse differs from dunes at " << differ << " of " << points
                                          << " points (the barchans)");
  CHECK(differ > 0);
}

TEST_CASE("terrain: many tiles are the same on any number of threads") {
  const DuneField field(reference_desc());
  Vector<TileCoord> tiles;
  for (i32 z = -2; z <= 2; ++z)
    for (i32 x = -2; x <= 2; ++x)
      tiles.push_back(TileCoord{x, z});
  TileOptions options;
  options.cells = 32;
  Vector<TileOutput> alone, pooled_one, pooled_many;
  evaluate_tiles(field, tiles, 400 * k_day, options, nullptr, nullptr, alone);
  {
    jobs::JobSystem one(jobs::JobSystemConfig{.performance_workers = 1, .pin_threads = false});
    evaluate_tiles(field, tiles, 400 * k_day, options, nullptr, &one, pooled_one);
  }
  {
    jobs::JobSystem many(jobs::JobSystemConfig{.performance_workers = 3, .pin_threads = false});
    evaluate_tiles(field, tiles, 400 * k_day, options, nullptr, &many, pooled_many);
  }
  REQUIRE(alone.size() == tiles.size());
  for (u32 k = 0; k < tiles.size(); ++k) {
    CHECK(alone[k].hash() == pooled_one[k].hash());
    CHECK(alone[k].hash() == pooled_many[k].hash());
  }
}

// The golden hashes: the reference tile at three times, at the overlay's grid, all bands and
// normals — every integer decision of the wind record, the lattice, the primitives, the fixed
// features, and the float conversion and normalization at the end. Taken on Clang 18 and checked
// on GCC 13 in this change; MSVC is checked on the owner's machine. A mismatch on one toolchain
// means the field depends on the toolchain, which is the bug: never re-pin a hash to make one
// compiler pass. Re-pin only with a change to the field that bumps `k_generator_version`, and say
// in the commit what moved.
TEST_CASE("terrain: golden hashes of the reference tile on every toolchain") {
  const DuneField field(reference_desc());
  struct Row {
    i64 time_us;
    u64 golden;
  };
  const Row rows[] = {
      {0, 0xcdb65fedad6a22caull},
      {90 * k_day + 6 * 3600 * k_us_per_second, 0xfd311c603c266ab5ull},
      {25 * k_year + 200 * k_day, 0xc364843efab5ad9cull},
  };
  MESSAGE("field hash " << hex(field.hash()));
  CHECK(field.hash() == 0xe4ce66bd0e50f703ull);
  for (const Row& row : rows) {
    TileOutput tile;
    evaluate_tile(field, TileCoord{0, 0}, row.time_us, TileOptions{}, nullptr, nullptr, tile);
    MESSAGE("t = " << row.time_us << " us: tile hash " << hex(tile.hash()) << ", "
                   << tile.primitives << " primitives, " << tile.crests.size() << " crests, "
                   << height_m(tile.min_um) << " .. " << height_m(tile.max_um) << " m");
    CHECK(hex(tile.hash()) == hex(row.golden));
  }
}

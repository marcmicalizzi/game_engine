// The dune field's invariants (docs/subsystems/terrain.md, "Invariants"): the integer arithmetic
// it rests on, the wind record's closed form against the day-by-day sum a simulation would add up,
// the field as a function of time (no state, t2 directly equal to t1 then t2), migration at
// Bagnold's rate, seams between tiles at every time, the sampler against the mesh, the fixed
// features, the lag, the LOD policy, the effect outputs, thread-count independence, and the golden
// hashes of a reference tile at three times that MSVC, GCC and Clang must all reproduce.
#include <core/jobs/job_system.h>
#include <domain/terrain/fixed.h>
#include <domain/terrain/stats.h>
#include <domain/terrain/terrain.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

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

// The erg profile of content/test-scenes/desert-erg, band for band (terrain.md, "The erg").
Vector<BandDesc> erg_bands() {
  const auto band = [](const char* name, PrimitiveKind kind, f32 lo, f32 hi, f32 cell, f32 share) {
    BandMetres m;
    m.name = name;
    m.kind = kind;
    m.height_min = lo;
    m.height_max = hi;
    m.cell = cell;
    m.share = share;
    return m;
  };
  BandMetres mega = band("mega-draa", PrimitiveKind::transverse, 80, 200, 2400, 0.85f);
  mega.length_min = 0.6f;
  mega.length_max = 1.0f;
  mega.stoss = 0.42f;
  mega.bend = 0.12f;
  mega.sinuosity = 0.08f;
  mega.spread_deg = 15;
  mega.side_days = 365;
  mega.sharp_days = 120;
  BandMetres draa = band("draa", PrimitiveKind::transverse, 10, 25, 360, 0.7f);
  draa.length_min = 0.5f;
  draa.length_max = 0.9f;
  draa.stoss = 0.4f;
  draa.bend = 0.18f;
  draa.sinuosity = 0.04f;
  draa.spread_deg = 25;
  draa.couple = BandCouple::flanks;
  draa.couple_width = 400;
  BandMetres crest = band("crest", PrimitiveKind::transverse, 2.5f, 6, 110, 0.6f);
  crest.couple = BandCouple::flanks;
  crest.couple_width = 100;
  BandMetres barchan = band("barchan", PrimitiveKind::barchan, 1.5f, 5, 150, 0.35f);
  barchan.couple = BandCouple::floors;
  barchan.couple_width = 80;
  barchan.far = false;
  BandMetres wave = band("wave", PrimitiveKind::transverse, 0.3f, 0.8f, 10, 0.6f);
  wave.length_min = 0.4f;
  wave.length_max = 0.8f;
  wave.stoss = 0.45f;
  wave.bend = 0.2f;
  wave.spread_deg = 30;
  wave.sharpness = 0;
  wave.side_days = 3;
  wave.sharp_days = 1;
  wave.far = false;
  Vector<BandDesc> bands;
  for (const BandMetres& m : {mega, draa, crest, barchan, wave})
    bands.push_back(band_from_metres(m));
  return bands;
}

FieldDesc erg_desc() {
  FieldDesc d;
  d.seed = 7;
  d.wind.seed = 7;
  d.dune_height = 8'000;
  d.wavelength = 110'000;
  d.bands = erg_bands();
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
  i64 dx[k_max_bands], dz[k_max_bands], len[k_max_bands];
  for (u32 b = 0; b < field.band_count(); ++b) {
    field.displacement(b, t, dx[b], dz[b]);
    len[b] = fx::length(dx[b], dz[b]);
    MESSAGE(std::string(field.band_name(b)) << " moved " << len[b] / 1000 << " m in two years");
  }
  // Bagnold: the lower the band, the faster; the ratio of speeds is the inverse ratio of heights.
  CHECK(len[2] > len[1]);
  CHECK(len[1] > len[0]);
  const f64 ratio = static_cast<f64>(len[2]) / static_cast<f64>(len[0]);
  const f64 heights =
      static_cast<f64>(field.band_height(0u)) / static_cast<f64>(field.band_height(2u));
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
  REQUIRE(field.primitive(0u, 0, 0, 0, p0));
  REQUIRE(field.primitive(0u, 0, 0, t, p1));
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
  // A slip face stands at the angle of repose, and the repose limiter keeps the sum of the bands
  // there too (terrain.md, "The repose limiter"): no sand over 36 degrees. Before the limiter,
  // 0.48% of these vertices were, the worst 53.5.
  CHECK(steepest > 30.0f);
  CHECK(steepest < 36.0f);
  CHECK(over_repose == 0u);
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
    REQUIRE(field.primitive(1u, 3, 4, d * k_day, p));
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
      {0, 0x632fb88e12b208bbull},
      {90 * k_day + 6 * 3600 * k_us_per_second, 0x53e88152156ddb13ull},
      {25 * k_year + 200 * k_day, 0xe037c4e040cddcd1ull},
  };
  MESSAGE("field hash " << hex(field.hash()));
  CHECK(field.hash() == 0x950ca7df2c6048adull);
  for (const Row& row : rows) {
    TileOutput tile;
    evaluate_tile(field, TileCoord{0, 0}, row.time_us, TileOptions{}, nullptr, nullptr, tile);
    MESSAGE("t = " << row.time_us << " us: tile hash " << hex(tile.hash()) << ", "
                   << tile.primitives << " primitives, " << tile.crests.size() << " crests, "
                   << height_m(tile.min_um) << " .. " << height_m(tile.max_um) << " m");
    CHECK(hex(tile.hash()) == hex(row.golden));
  }
}

TEST_CASE("terrain: the band table — the default is the three bands, and a bad table is refused") {
  // The empty table is the default's three bands, derived from the dune height and wavelength: the
  // same primitives, the same tile, bit for bit, whether the table is left out or written out.
  const FieldDesc implicit = reference_desc();
  FieldDesc explicit_desc = reference_desc();
  explicit_desc.bands = default_bands(explicit_desc.dune_height, explicit_desc.wavelength);
  REQUIRE(explicit_desc.bands.size() == 3);
  const DuneField a(implicit);
  const DuneField b(explicit_desc);
  CHECK(a.band_count() == 3);
  CHECK(std::string(a.band_name(0)) == "draa");
  CHECK(std::string(a.band_name(2)) == "barchan");
  TileOutput ta, tb;
  TileOptions options;
  options.cells = 64;
  evaluate_tile(a, TileCoord{0, 0}, 200 * k_day, options, nullptr, nullptr, ta);
  evaluate_tile(b, TileCoord{0, 0}, 200 * k_day, options, nullptr, nullptr, tb);
  CHECK(ta.hash() == tb.hash());
  // The hash names a written-out table (it is a different description), and not an absent one.
  CHECK(a.hash() != b.hash());
  std::string error;
  CHECK(validate_bands(explicit_desc.bands, &error));

  // Refusals, each with its sentence.
  CHECK_FALSE(validate_bands({}, &error));
  CHECK(error.find("empty") != std::string::npos);
  Vector<BandDesc> zero = explicit_desc.bands;
  zero[1].presence_q16 = 0;
  CHECK_FALSE(validate_bands(zero, &error));
  CHECK(error.find("zero share") != std::string::npos);
  Vector<BandDesc> small_cell = explicit_desc.bands;
  small_cell[0].cell_cm = 500;  // 5 m cells for a 3.75 m draa
  CHECK_FALSE(validate_bands(small_cell, &error));
  CHECK(error.find("smaller than its own dune") != std::string::npos);
  Vector<BandDesc> small_barchan = explicit_desc.bands;
  small_barchan[2].cell_cm = small_barchan[2].height_hi_cm * 5;
  CHECK_FALSE(validate_bands(small_barchan, &error));
  Vector<BandDesc> order = explicit_desc.bands;
  std::swap(order[0], order[2]);
  CHECK_FALSE(validate_bands(order, &error));
  CHECK(error.find("tallest first") != std::string::npos);
  Vector<BandDesc> heights = explicit_desc.bands;
  heights[1].height_lo_cm = heights[1].height_hi_cm + 1;
  CHECK_FALSE(validate_bands(heights, &error));
  Vector<BandDesc> couple = explicit_desc.bands;
  couple[0].couple = BandCouple::floors;
  couple[0].couple_mm = 1000;
  CHECK_FALSE(validate_bands(couple, &error));  // the first band has nothing to couple to
  Vector<BandDesc> many;
  for (u32 k = 0; k <= k_max_bands; ++k)
    many.push_back(explicit_desc.bands[2]);
  CHECK_FALSE(validate_bands(many, &error));
}

TEST_CASE("terrain: the erg profile — valid, seamless, a function of time, the tall band still") {
  const FieldDesc desc = erg_desc();
  std::string error;
  REQUIRE_MESSAGE(validate_bands(desc.bands, &error), error);
  const DuneField field(desc);
  REQUIRE(field.band_count() == 5);
  TileOptions options;
  options.cells = 32;
  const u32 v = options.cells + 1;
  for (const i64 t : {i64{0}, 200 * k_day + 7 * 3600 * k_us_per_second, 6 * k_year}) {
    for (const TileCoord base : {TileCoord{0, 0}, TileCoord{17, -3}, TileCoord{-40, 22}}) {
      TileOutput here, east, north;
      evaluate_tile(field, base, t, options, nullptr, nullptr, here);
      evaluate_tile(field, TileCoord{base.x + 1, base.z}, t, options, nullptr, nullptr, east);
      evaluate_tile(field, TileCoord{base.x, base.z + 1}, t, options, nullptr, nullptr, north);
      u32 bad = 0;
      for (u32 k = 0; k < v; ++k) {
        bad += here.height_um[k * v + (v - 1)] != east.height_um[k * v];
        bad += !(here.normals[k * v + (v - 1)] == east.normals[k * v]);
        bad += here.height_um[(v - 1) * v + k] != north.height_um[k];
        bad += !(here.normals[(v - 1) * v + k] == north.normals[k]);
      }
      CHECK(bad == 0);
    }
  }
  // t2 directly is t1 then t2.
  const DuneField other(desc);
  TileOutput first, direct, after;
  evaluate_tile(field, TileCoord{5, 5}, 30 * k_day, options, nullptr, nullptr, first);
  evaluate_tile(field, TileCoord{5, 5}, 4 * k_year, options, nullptr, nullptr, after);
  evaluate_tile(other, TileCoord{5, 5}, 4 * k_year, options, nullptr, nullptr, direct);
  CHECK(after.hash() == direct.hash());
  // Bagnold: the 140 m band barely moves while the waves run away.
  i64 dx = 0, dz = 0, wx = 0, wz = 0;
  field.displacement(0, 3 * k_year, dx, dz);
  field.displacement(4, 3 * k_year, wx, wz);
  const i64 mega = fx::length(dx, dz);
  const i64 waves = fx::length(wx, wz);
  MESSAGE("three years: the mega-draa moved " << mega / 1000 << " m, the waves " << waves / 1000
                                              << " m");
  CHECK(mega < 10'000);
  CHECK(waves > 100 * mega);
}

TEST_CASE("terrain: a band coupled to the floors stands only on them") {
  FieldDesc desc;
  desc.seed = 11;
  desc.wind.seed = 11;
  BandMetres big;
  big.name = "big";
  big.height_min = 15;
  big.height_max = 25;
  big.cell = 400;
  big.stoss = 0.4f;
  BandMetres small;
  small.name = "small";
  small.kind = PrimitiveKind::barchan;
  small.height_min = 2;
  small.height_max = 4;
  small.cell = 80;
  small.share = 0.9f;
  small.couple = BandCouple::floors;
  small.couple_width = 60;
  FieldDesc alone = desc;
  alone.bands.push_back(band_from_metres(big));
  desc.bands = alone.bands;
  desc.bands.push_back(band_from_metres(small));
  const DuneField with(desc);
  const DuneField without(alone);
  // The coupling fades across 60 m inside the big band's footprints, which reach at least as far
  // as its sand, and a footprint's depth is measured across the local crest, which is at most the
  // distance over the cosine of the crest's drift (1.2 here): so a point more than 90 m from every
  // bare-floor point is deep enough inside that nothing of the small band may stand there.
  // Measured on a 4 m grid by a chessboard distance transform (two passes), which never exceeds
  // the true distance to the nearest bare grid point (and that, to the nearest bare point, by 4 m).
  constexpr i64 k_step = 4'000;
  constexpr u32 k_n = 201;
  Gather g1, g2;
  with.gather(0, 0, 800'000, 800'000, k_year, nullptr, g1);
  without.gather(0, 0, 800'000, 800'000, k_year, nullptr, g2);
  std::vector<i64> base(k_n * k_n), both(k_n * k_n);
  std::vector<u32> far(k_n * k_n);
  for (u32 j = 0; j < k_n; ++j) {
    for (u32 i = 0; i < k_n; ++i) {
      base[j * k_n + i] = without.sample(g2, i * k_step, j * k_step, Detail::dunes).sand;
      both[j * k_n + i] = with.sample(g1, i * k_step, j * k_step, Detail::dunes).sand;
      far[j * k_n + i] = base[j * k_n + i] == 0 ? 0u : 1'000'000u;
    }
  }
  for (u32 j = 0; j < k_n; ++j) {
    for (u32 i = 0; i < k_n; ++i) {
      u32& d = far[j * k_n + i];
      if (i > 0) d = std::min(d, far[j * k_n + i - 1] + 1);
      if (j > 0) d = std::min(d, far[(j - 1) * k_n + i] + 1);
      if (i > 0 && j > 0) d = std::min(d, far[(j - 1) * k_n + i - 1] + 1);
      if (i + 1 < k_n && j > 0) d = std::min(d, far[(j - 1) * k_n + i + 1] + 1);
    }
  }
  for (u32 j = k_n; j-- > 0;) {
    for (u32 i = k_n; i-- > 0;) {
      u32& d = far[j * k_n + i];
      if (i + 1 < k_n) d = std::min(d, far[j * k_n + i + 1] + 1);
      if (j + 1 < k_n) d = std::min(d, far[(j + 1) * k_n + i] + 1);
      if (i + 1 < k_n && j + 1 < k_n) d = std::min(d, far[(j + 1) * k_n + i + 1] + 1);
      if (i > 0 && j + 1 < k_n) d = std::min(d, far[(j + 1) * k_n + i - 1] + 1);
    }
  }
  u32 on_flanks = 0, on_floor = 0, differ_flank = 0, differ_floor = 0;
  for (u32 k = 0; k < k_n * k_n; ++k) {
    if (static_cast<i64>(far[k]) * k_step > 90'000) {
      ++on_flanks;
      differ_flank += both[k] != base[k];
    } else if (base[k] == 0) {
      ++on_floor;
      differ_floor += both[k] != base[k];
    }
  }
  MESSAGE(differ_floor << " of " << on_floor << " floor points carry a barchan; " << differ_flank
                       << " of " << on_flanks << " flank points do");
  CHECK(on_flanks > 100);
  CHECK(differ_flank == 0);
  CHECK(differ_floor > 0);
}

TEST_CASE("terrain: the statistics are what a brute-force count gives") {
  const DuneField field(erg_desc());
  const i64 x0 = 700'000, z0 = -300'000, s = 1'000;
  const u32 n = 48;
  const i64 t = 2 * k_year;
  FieldStats st;
  field_stats(field, x0, z0, n, n, s, t, Detail::dunes, nullptr, nullptr, st);
  // Brute force: every height a point query of its own, the slope's bins by std::tan.
  const auto h = [&](i64 i, i64 j) { return field.height_um(x0 + i * s, z0 + j * s, t); };
  u64 slope[k_slope_bins] = {};
  u64 sand = 0, flat = 0;
  std::vector<i64> above;
  for (i64 j = 0; j < n; ++j) {
    for (i64 i = 0; i < n; ++i) {
      const i64 over = h(i, j) - field.floor_um(x0 + i * s, z0 + j * s);
      above.push_back(over);
      flat += over <= k_flat_um ? 1u : 0u;
      if (material_at(field, x0 + i * s, z0 + j * s) == 1) continue;
      ++sand;
      const f64 gx =
          static_cast<f64>(h(i + 1, j) - h(i - 1, j)) / (2.0 * static_cast<f64>(s) * 1000.0);
      const f64 gz =
          static_cast<f64>(h(i, j + 1) - h(i, j - 1)) / (2.0 * static_cast<f64>(s) * 1000.0);
      const f64 degrees = std::atan(std::sqrt(gx * gx + gz * gz)) * 57.29577951308232;
      u32 bin = 0;
      while (bin < k_slope_bins - 1 && degrees >= k_slope_edges_deg[bin])
        ++bin;
      ++slope[bin];
    }
  }
  std::sort(above.begin(), above.end());
  CHECK(st.vertices == u64{n} * n);
  CHECK(st.sand_vertices == sand);
  CHECK(st.flat == flat);
  for (u32 b = 0; b < k_slope_bins; ++b)
    CHECK(st.slope[b] == slope[b]);
  CHECK(st.above_floor_um[1] == above[above.size() / 2]);
  CHECK(st.above_floor_um[4] == above.back());
  // And the same numbers on any number of threads.
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 3, .pin_threads = false});
  FieldStats pooled;
  field_stats(field, x0, z0, n, n, s, t, Detail::dunes, nullptr, &pool, pooled);
  CHECK(pooled.hash() == st.hash());
}

// The statistics' goldens (terrain.md, "What the numbers say"): the default reference field over a
// kilometre, the erg over six, and the erg at half a metre over a mega-draa's slip face — every
// count, as one hash, and the numbers a reader checks beside it. The same toolchain rule as the
// tile goldens: a mismatch on one compiler is the bug.
namespace {

struct StatsRow {
  const char* name;
  FieldDesc desc;
  i64 x0, z0;
  u32 n;
  i64 spacing;
  u64 golden;
};

void print_stats(const char* name, const DuneField& field, const FieldStats& st) {
  const f64 sand = static_cast<f64>(st.sand_vertices > 0 ? st.sand_vertices : 1);
  MESSAGE(
      std::string(name) << ": hash " << hex(st.hash()) << ", above floor p10/p50/p90/p99/max "
                        << height_m(st.above_floor_um[0]) << " / " << height_m(st.above_floor_um[1])
                        << " / " << height_m(st.above_floor_um[2]) << " / "
                        << height_m(st.above_floor_um[3]) << " / " << height_m(st.above_floor_um[4])
                        << " m, flat " << static_cast<f64>(st.flat) / static_cast<f64>(st.vertices)
                        << ", 30 to 34 " << static_cast<f64>(st.slope[k_slope_bins - 3]) / sand
                        << ", over 34 " << static_cast<f64>(st.over_repose()) / sand << ", over 36 "
                        << st.over_36() << " vertices, tallest " << height_m(st.tallest_um)
                        << " m (" << std::string(field.band_name(st.tallest_band)) << ") every "
                        << static_cast<f64>(st.tallest_spacing_mm) / 1000.0 << " m");
}

}  // namespace

TEST_CASE("terrain: golden statistics of the default and the erg") {
  const StatsRow rows[] = {
      {"default, 1 km at 4 m", reference_desc(), -512'000, -512'000, 257, 4'000,
       0x988f00dab9665187ull},
      {"erg, 6 km at 24 m", erg_desc(), -3'072'000, -3'072'000, 257, 24'000, 0xbe18cf12866489b1ull},
      {"erg, a slip face at 0.5 m", erg_desc(), 250'000, -128'000, 513, 500, 0x9675f50c35d91508ull},
  };
  for (const StatsRow& row : rows) {
    const DuneField field(row.desc);
    FieldStats st;
    field_stats(field, row.x0, row.z0, row.n, row.n, row.spacing, 3 * k_year, Detail::dunes,
                nullptr, nullptr, st);
    print_stats(row.name, field, st);
    CHECK(hex(st.hash()) == hex(row.golden));
    CHECK(st.over_36() == 0u);  // the repose limiter
  }
}

TEST_CASE(
    "terrain: the repose limiter — no sand over 36 degrees on the erg, at every time sampled") {
  // The windows where the bands' slopes added before the limiter (a draa's reversed slip face on a
  // mega-draa's stoss, a mega-draa's crest end where its meander turns it, a coupled draa at the
  // foot of a slip face), at a metre, at four times across the wind record's seasons.
  const DuneField field(erg_desc());
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 3, .pin_threads = false});
  struct Window {
    i64 x0, z0;
  };
  u64 sand = 0, steep = 0;
  for (const i64 t : {i64{0}, k_year, 3 * k_year, 7 * k_year}) {
    for (const Window w :
         {Window{2'238'000, -3'059'000}, Window{2'183'000, -2'857'000}, Window{-1'842'000, 134'000},
          Window{-1'813'000, 93'000}, Window{11'000, 45'000}, Window{2'033'000, 958'000},
          Window{1'299'000, 141'000}}) {
      FieldStats st;
      field_stats(field, w.x0, w.z0, 201, 201, 1'000, t, Detail::dunes, nullptr, &pool, st);
      CHECK(st.over_36() == 0u);
      sand += st.sand_vertices;
      steep += st.slope[k_slope_bins - 3] + st.over_repose();
    }
  }
  MESSAGE("erg: 0 of " << sand << " sand vertices over 36 degrees, " << steep
                       << " over 30 (the slip faces)");
  CHECK(steep > sand / 100);  // the windows hold slip faces
  // And the erg over the desert overlook's rock (content/test-scenes/desert-dunes): its ridges and
  // oasis basin thin and flatten the sand, and a 150 m dune scaled across a ridge's edge was a
  // slope of its own until the features' cap. The window where it showed, and the basin, at a
  // metre, three years in.
  FieldDesc rock = erg_desc();
  rock.seed = 23;
  rock.wind.seed = 23;
  rock.wind.storms_per_year = 12;
  rock.ridges.push_back(RidgeFeature{-2'200'000, 420'000, -100'000, 330'000, 150'000});
  rock.ridges.push_back(RidgeFeature{100'000, 330'000, 2'200'000, 460'000, 150'000});
  rock.ridges.push_back(RidgeFeature{-700'000, -260'000, 520'000, -340'000, 110'000});
  rock.basins.push_back(BasinFeature{0, 60'000, 200'000});
  const DuneField overlook(rock);
  // The erg with its scene's storms, where a floor-coupled barchan's fade stood past 36 degrees
  // seven years in before the fade's slope joined the budget.
  FieldDesc stormy = erg_desc();
  stormy.wind.storms_per_year = 12;
  FieldStats barchans;
  field_stats(DuneField(stormy), -2'816'000, -3'072'000, 257, 257, 1'000, 7 * k_year, Detail::dunes,
              nullptr, &pool, barchans);
  CHECK(barchans.over_36() == 0u);
  for (const Window w :
       {Window{-1'536'000, 512'000}, Window{-1'280'000, 768'000}, Window{-128'000, -68'000}}) {
    FieldStats st;
    field_stats(overlook, w.x0, w.z0, 257, 257, 1'000, 3 * k_year, Detail::dunes, nullptr, &pool,
                st);
    CHECK(st.over_36() == 0u);
  }
}

TEST_CASE("terrain: storm hours — the integral is the hours' winds, hour by hour") {
  WindParams quiet;
  quiet.seed = 7;
  WindParams stormy = quiet;
  stormy.storms_per_year = 12;
  const WindRecord calm(quiet);
  const WindRecord record(stormy);
  const Vector<WindStorm>& storms = record.storms();
  REQUIRE(storms.size() == 12u * k_record_years);
  constexpr i64 k_hour = 3'600 * k_us_per_second;
  u32 storm_hours = 0;
  for (u32 k = 0; k < storms.size(); ++k) {
    const WindStorm& s = storms[k];
    if (k > 0) CHECK(storms[k - 1].day < s.day);  // in day order, one a day
    CHECK(s.hours >= 3);
    CHECK(s.hours <= 12);
    CHECK(s.first_hour + s.hours <= 24);
    CHECK(record.day(s.day).storm == k + 1);
    // Hour by hour through the storm's day and the day after it: what the integral moves over an
    // hour is exactly the wind that hour reports, and the storm blows in exactly its hours.
    for (i64 day = s.day; day <= s.day + 1; ++day) {
      for (i64 hour = 0; hour < 24; ++hour) {
        const i64 t = day * k_us_per_day + hour * k_hour;
        const WindAt at = record.wind_at(t + k_hour / 2);
        CHECK(record.between(t, t + k_hour) == at.flux);
        // That day's own storm: the day after may have one of its own.
        const u8 index = record.day(day).storm;
        const WindStorm* own = index != 0 ? &storms[index - 1u] : nullptr;
        const bool in_storm =
            own != nullptr && hour >= own->first_hour && hour < own->first_hour + own->hours;
        CHECK(at.storm == in_storm);
        if (day == s.day) storm_hours += at.storm;
        if (in_storm) CHECK(at.turn == own->turn);
      }
    }
    // The day's total is its 24 hours, and the day without its storm is the storm-free record's.
    const i64 t = static_cast<i64>(s.day) * k_us_per_day;
    const FluxIntegral whole = record.between(t, t + k_us_per_day);
    CHECK(whole.x == record.day(s.day).fx);
    CHECK(whole.magnitude == record.day(s.day).magnitude);
    CHECK(record.day(s.day).speed_q16 == calm.day(s.day).speed_q16);
    CHECK(whole.magnitude > calm.day(s.day).magnitude);
  }
  u32 expected_hours = 0;
  for (const WindStorm& s : storms)
    expected_hours += static_cast<u32>(s.hours);
  CHECK(storm_hours == expected_hours);
  // Every day without a storm is the storm-free record's day, and the prefix sums hold the storms:
  // the period's total is the days', and the integral is continuous across every midnight.
  FluxIntegral sum;
  u32 same = 0;
  for (i32 d = 0; d < k_record_days; ++d) {
    const WindDay& a = record.day(d);
    const WindDay& b = calm.day(d);
    if (a.storm == 0)
      same += a.fx == b.fx && a.fz == b.fz && a.magnitude == b.magnitude && a.turn == b.turn;
    sum.x += a.fx;
    sum.z += a.fz;
    sum.magnitude += a.magnitude;
  }
  CHECK(same == static_cast<u32>(k_record_days) - storms.size());
  CHECK(sum == record.period_total());
  for (i64 d = -3; d < 3 * k_record_days; d += 97) {
    const WindDay& day = record.day(d);
    CHECK(record.between(d * k_us_per_day, (d + 1) * k_us_per_day) ==
          (FluxIntegral{day.fx, day.fz, day.magnitude}));
  }
  // And a sample of hours across three periods, before the epoch too.
  u32 checked = 0;
  for (i64 h = -48; h < 3 * k_record_days * 24; h += 101) {
    const i64 t = h * k_hour;
    CHECK(record.between(t, t + k_hour) == record.wind_at(t).flux);
    ++checked;
  }
  // How a storm compares with the weather round it: its day against a mean day, its strongest hour
  // against a mean day's hour.
  const f64 mean_day = static_cast<f64>(calm.period_total().magnitude) / k_record_days;
  f64 biggest_day = 0.0, biggest_hour = 0.0;
  for (const WindStorm& st : storms) {
    biggest_day = std::max(biggest_day, static_cast<f64>(record.day(st.day).magnitude) / mean_day);
    for (i32 h = 0; h < 24; ++h)
      biggest_hour =
          std::max(biggest_hour, static_cast<f64>(st.hour[h + 1].magnitude - st.hour[h].magnitude) /
                                     (mean_day / 24.0));
  }
  MESSAGE("the stormiest day moves " << biggest_day << " mean days of sand, its strongest hour "
                                     << biggest_hour << " mean hours");
  CHECK(biggest_hour > 10.0);
  const f64 extra = static_cast<f64>(record.period_total().magnitude) /
                    static_cast<f64>(calm.period_total().magnitude);
  MESSAGE("12 storms a year: " << expected_hours / k_record_years << " storm hours a year, "
                               << (extra - 1.0) * 100.0 << "% more sand moved; " << checked
                               << " sampled hours exact");
  CHECK(extra > 1.02);
}

TEST_CASE("terrain: a season of storms moves the erg further, the small forms most") {
  FieldDesc calm_desc = erg_desc();
  FieldDesc stormy_desc = erg_desc();
  stormy_desc.wind.storms_per_year = 12;
  const DuneField calm(calm_desc);
  const DuneField stormy(stormy_desc);
  CHECK(calm.hash() != stormy.hash());
  // A year: how far each band moves, with and without storms.
  for (u32 b = 0; b < calm.band_count(); ++b) {
    i64 cx = 0, cz = 0, sx = 0, sz = 0;
    calm.displacement(b, k_year, cx, cz);
    stormy.displacement(b, k_year, sx, sz);
    const f64 c = std::sqrt(static_cast<f64>(cx) * cx + static_cast<f64>(cz) * cz) / 1000.0;
    const f64 s = std::sqrt(static_cast<f64>(sx) * sx + static_cast<f64>(sz) * sz) / 1000.0;
    const f64 turn = std::atan2(static_cast<f64>(cx) * sz - static_cast<f64>(cz) * sx,
                                static_cast<f64>(cx) * sx + static_cast<f64>(cz) * sz) *
                     57.29578;
    MESSAGE(std::string(calm.band_name(b)) << ": " << c << " m a year, " << s
                                           << " m with 12 storms, turned " << turn << " degrees");
    CHECK(s > c);
  }
  // Nothing about the storm-free field moved: its goldens stand (the golden test holds them).
  CHECK(DuneField(erg_desc()).hash() == calm.hash());
}

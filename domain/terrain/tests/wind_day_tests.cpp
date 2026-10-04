// The wind's day, the storms' transport gain and the wind rose (docs/subsystems/terrain.md, "The
// day's wind", "A storm scales transport", "The wind rose"; plan 13, "Day, night, and weather").
// With neither the day nor a gain the record is the one the goldens pin (terrain_tests.cpp holds
// them); here, with them on: the integral still the hours' winds hour by hour and the day-by-day
// sum, exact under any split and across periods; a day's sand redistributed and never added to; a
// storm's hours multiplied by its gain and nothing else moved; the gain and a band's
// `celerity_scale` multiplying; the rose summing to the magnitude integral exactly; and a field on
// every thread count the same bytes.
#include <core/jobs/job_system.h>
#include <domain/terrain/fixed.h>
#include <domain/terrain/stats.h>
#include <domain/terrain/terrain.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace engine;
using namespace engine::terrain;

namespace {

constexpr i64 k_day = k_us_per_day;
constexpr i64 k_hour = 3'600 * k_us_per_second;
constexpr i64 k_year = 365 * k_day;

WindParams stormy(u64 seed) {
  WindParams p;
  p.seed = seed;
  p.storms_per_year = 12;
  return p;
}

// A calm night and an afternoon peak, veering 30 degrees either side.
WindParams with_day(WindParams p) {
  p.diurnal_q16 = 65536;
  p.diurnal_peak_turn = static_cast<u16>(15 * 65536 / 24);
  p.veer_turn = 5461;
  p.veer_phase_turn = 32768;
  return p;
}

f64 length_of(const WindDay& d) {
  return std::sqrt(static_cast<f64>(d.fx) * static_cast<f64>(d.fx) +
                   static_cast<f64>(d.fz) * static_cast<f64>(d.fz));
}

}  // namespace

TEST_CASE("terrain wind: the day's wind redistributes a day's sand and never adds to it") {
  const WindRecord flat(stormy(7));
  const WindRecord daily(with_day(stormy(7)));
  REQUIRE(daily.diurnal());
  REQUIRE_FALSE(flat.diurnal());
  // Every day without a storm moves exactly its own magnitude; its net vector is shortened by the
  // veer and never lengthened.
  f64 ratio_sum = 0.0;
  u32 ratio_days = 0;
  for (i64 d = 0; d < k_record_days; ++d) {
    const WindDay& a = flat.day(d);
    const WindDay& b = daily.day(d);
    CHECK(a.speed_q16 == b.speed_q16);
    CHECK(a.turn == b.turn);
    if (a.storm != 0) continue;
    CHECK(b.magnitude == a.magnitude);
    if (a.magnitude == 0) continue;
    const f64 ratio = length_of(b) / length_of(a);
    CHECK(ratio <= 1.0 + 1.0e-6);
    ratio_sum += ratio;
    ++ratio_days;
  }
  const f64 mean_ratio = ratio_sum / static_cast<f64>(ratio_days);
  MESSAGE("a veer of 30 degrees either side leaves a day " << mean_ratio
                                                           << " of its net transport");
  CHECK(mean_ratio > 0.85);
  CHECK(mean_ratio < 1.0);
  // The period's mean magnitude is the record's without the day, storms and all, to the unit.
  CHECK(daily.period_total().magnitude == flat.period_total().magnitude);

  // Hour by hour through a week: what the integral moves over an hour is what the hour reports,
  // the afternoon is strong, the night calm, and the direction stays within the veer of the day's.
  const WindParams p = with_day(stormy(7));
  i64 afternoon = 0;
  i64 night = 0;
  for (i64 day = 40; day < 47; ++day) {
    for (i64 hour = 0; hour < 24; ++hour) {
      const i64 t = day * k_day + hour * k_hour;
      const WindAt at = daily.wind_at(t + k_hour / 2);
      CHECK(daily.between(t, t + k_hour) == at.flux);
      if (at.storm) continue;
      const i32 off = static_cast<i16>(static_cast<u16>(at.turn - daily.day(day).turn));
      CHECK(std::abs(off) <= p.veer_turn + 1);
      if (hour == 15) afternoon += at.flux.magnitude;
      if (hour == 3) night += at.flux.magnitude;
    }
  }
  MESSAGE("a week's 15:00 hours moved " << afternoon << " cm^2, its 03:00 hours " << night);
  CHECK(afternoon > 20 * (night + 1));

  // The closed form is the day-by-day sum, across two periods and before the epoch.
  FluxIntegral sum;
  for (i64 d = 0; d <= 2 * k_record_days + 40; d += 7) {
    CHECK(daily.integral(d * k_day) == sum);
    for (i64 k = d; k < d + 7; ++k) {
      const WindDay& day = daily.day(k);
      sum.x += day.fx;
      sum.z += day.fz;
      sum.magnitude += day.magnitude;
    }
  }
  const FluxIntegral back = daily.integral(-400 * k_day);
  FluxIntegral walked;
  for (i64 d = -1; d >= -400; --d) {
    walked.x -= daily.day(d).fx;
    walked.z -= daily.day(d).fz;
    walked.magnitude -= daily.day(d).magnitude;
  }
  CHECK(back == walked);
}

TEST_CASE("terrain wind: every integral is exact under any split, day and gain on") {
  WindParams p = with_day(stormy(11));
  p.storm_gain_q16 = 50 * 65536;
  const WindRecord wind(p);
  // A cadence of odd steps across a year and a half, the steps' sum against the whole.
  const i64 t0 = 3 * k_day + 12'345;
  const i64 t1 = 1'000 * k_year / 2 + 777;
  FluxIntegral steps;
  i64 rose_steps[k_rose_sectors] = {};
  u32 count = 0;
  for (i64 t = t0; t < t1; count += 1) {
    const i64 next = std::min(t1, t + 7 * k_hour + 1'234'567 * static_cast<i64>(count % 13));
    const FluxIntegral piece = wind.between(t, next);
    steps.x += piece.x;
    steps.z += piece.z;
    steps.magnitude += piece.magnitude;
    i64 rose[k_rose_sectors];
    wind.rose_between(t, next, rose);
    i64 rose_sum = 0;
    for (u32 s = 0; s < k_rose_sectors; ++s) {
      rose_steps[s] += rose[s];
      rose_sum += rose[s];
      CHECK(rose[s] >= 0);
    }
    CHECK(rose_sum == piece.magnitude);
    t = next;
  }
  CHECK(steps == wind.between(t0, t1));
  i64 rose_whole[k_rose_sectors];
  wind.rose_between(t0, t1, rose_whole);
  for (u32 s = 0; s < k_rose_sectors; ++s)
    CHECK(rose_steps[s] == rose_whole[s]);
  MESSAGE(count << " steps: the integral and the rose's sixteen sectors are the whole to the unit");
  // A thousand years on is the same arithmetic.
  i64 far[k_rose_sectors];
  wind.rose(1000 * k_year + 17, far);
  i64 far_sum = 0;
  for (const i64 v : far)
    far_sum += v;
  CHECK(far_sum == wind.integral(1000 * k_year + 17).magnitude);
}

TEST_CASE(
    "terrain wind: the magnitude integral in full is continuous, and its whole is integral's") {
  // `magnitude_at` keeps what `integral` truncates: its whole is `integral`'s magnitude to the
  // unit, its fraction is in [0, 1), and whole + fraction is the record's straight line within
  // every hour (every kind of day is a line within an hour: a plain day's over the day, a profiled
  // day's and a storm day's over the hour), so it never steps — what the ripples' travel is drawn
  // from (renderer.md, "Ripples that move"). Plain days, the day's profile, storms and a gain;
  // across a period's end and before the epoch.
  WindParams gained = stormy(5);
  gained.storm_gain_q16 = 50 * 65536;
  const WindRecord records[3] = {WindRecord(stormy(7)), WindRecord(with_day(stormy(7))),
                                 WindRecord(gained)};
  for (const WindRecord& wind : records) {
    const auto full = [&](i64 t) {
      i64 whole = 0;
      f64 fraction = -1.0;
      wind.magnitude_at(t, whole, fraction);
      CHECK(whole == wind.integral(t).magnitude);
      CHECK(fraction >= 0.0);
      CHECK(fraction < 1.0);
      return static_cast<f64>(whole) + fraction;
    };
    f64 worst_line = 0.0;
    f64 worst_join = 0.0;
    u32 storm_hours = 0;
    // The epoch, three years in, a period's end, before the epoch, and the day before the period's
    // first storm, whose hours are a table of their own.
    REQUIRE_FALSE(wind.storms().empty());
    const i64 storm_eve = (static_cast<i64>(wind.storms()[0].day) - 1) * k_day;
    for (const i64 start :
         {i64{0}, 1'095 * k_day, (k_record_days - 2) * k_day, -9 * k_day, storm_eve}) {
      for (i64 h = 0; h < 72; ++h) {
        const i64 a = start + h * k_hour;
        const f64 va = full(a);
        const f64 vb = full(a + k_hour);
        if (wind.wind_at(a).storm) ++storm_hours;
        // Within the hour, on its line, at odd microseconds.
        for (i64 k = 1; k < 16; ++k) {
          const i64 into = k * (k_hour / 16) + 4'321 * k;
          const f64 line = va + (vb - va) * static_cast<f64>(into) / static_cast<f64>(k_hour);
          worst_line = std::max(worst_line, std::abs(full(a + into) - line));
        }
        // And across its start, a microsecond either side: no step.
        worst_join = std::max(worst_join, std::abs(full(a + 1) - full(a - 1)));
        CHECK(vb >= va);
      }
    }
    MESSAGE("the integral in full lies on each hour's line to "
            << worst_line << " cm^2, and moves " << worst_join << " cm^2 in two "
            << "microseconds across an hour's "
            << "start (" << storm_hours << " storm hours)");
    CHECK(storm_hours > 0u);
    CHECK(worst_line < 1e-6);
    // The strongest gained storm hour moves a few million cm^2 in an hour: well under one in two
    // microseconds.
    CHECK(worst_join < 0.01);
  }
}

TEST_CASE("terrain wind: a storm's gain multiplies its hours' transport and nothing else") {
  const WindRecord plain(stormy(7));
  WindParams one = stormy(7);
  one.storm_gain_q16 = 65536;
  CHECK(WindRecord(one).period_total() == plain.period_total());  // 1 is the record as it was
  WindParams ten = stormy(7);
  ten.storm_gain_q16 = 10 * 65536;
  ten.storm_gains.push_back(WindParams::StormGain{3, 250 * 65536});
  const WindRecord gained(ten);
  REQUIRE(gained.storms().size() == plain.storms().size());
  for (u32 k = 0; k < plain.storms().size(); ++k) {
    const WindStorm& a = plain.storms()[k];
    const WindStorm& b = gained.storms()[k];
    CAPTURE(k);
    CHECK(a.day == b.day);
    CHECK(a.first_hour == b.first_hour);
    CHECK(a.hours == b.hours);
    CHECK(a.turn == b.turn);
    CHECK(b.gain_q16 == (k == 3 ? 250 : 10) * 65536);
    const i64 gain = k == 3 ? 250 : 10;
    for (i32 h = 0; h < 24; ++h) {
      const i64 blown_a = a.storm_magnitude[h + 1] - a.storm_magnitude[h];
      const i64 blown_b = b.storm_magnitude[h + 1] - b.storm_magnitude[h];
      CHECK(blown_b == blown_a * gain);
      // The day's own share of the hour is untouched.
      CHECK(b.hour[h + 1].magnitude - b.storm_magnitude[h + 1] ==
            a.hour[h + 1].magnitude - a.storm_magnitude[h + 1]);
      // The wind the player lives by is the same wind: direction and strength.
      const i64 t = static_cast<i64>(a.day) * k_day + h * k_hour + k_hour / 2;
      const WindAt wa = plain.wind_at(t);
      const WindAt wb = gained.wind_at(t);
      CHECK(wa.turn == wb.turn);
      CHECK(wa.speed_q16 == wb.speed_q16);
      CHECK(wa.storm == wb.storm);
      CHECK(wb.gain_q16 == (wb.storm ? gain * 65536 : 65536));
    }
  }
  // Days without a storm are the same days.
  for (i64 d = 0; d < k_record_days; ++d) {
    if (plain.day(d).storm != 0) continue;
    CHECK(plain.day(d).magnitude == gained.day(d).magnitude);
    CHECK(plain.day(d).fx == gained.day(d).fx);
  }
}

TEST_CASE("terrain wind: a storm's gain and a band's celerity_scale multiply") {
  FieldDesc base;
  base.seed = 7;
  base.wind = stormy(7);
  base.bands = default_bands(base.dune_height, base.wavelength);
  const WindStorm storm = WindRecord(base.wind).storms()[5];
  const i64 from = static_cast<i64>(storm.day) * k_day + storm.first_hour * k_hour;
  const i64 to = from + storm.hours * k_hour;
  const auto travel = [&](const FieldDesc& d, u32 band) {
    const DuneField field(d);
    i64 x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    field.displacement(band, from, x0, z0);
    field.displacement(band, to, x1, z1);
    return std::sqrt(static_cast<f64>(x1 - x0) * static_cast<f64>(x1 - x0) +
                     static_cast<f64>(z1 - z0) * static_cast<f64>(z1 - z0));
  };
  FieldDesc calm = base;
  calm.wind.storms_per_year = 12;
  FieldDesc g10 = base;
  g10.wind.storm_gain_q16 = 10 * 65536;
  FieldDesc c4 = base;
  c4.bands[2].celerity_q16 = 4 * 65536;
  FieldDesc both = g10;
  both.bands[2].celerity_q16 = 4 * 65536;
  // Over the storm's hours the day's own share is small beside the storm's, so the travel scales
  // by nearly the gain, and the celerity scale multiplies whatever the gain made of it.
  const f64 t1 = travel(base, 2);
  const f64 tg = travel(g10, 2);
  const f64 tc = travel(c4, 2);
  const f64 tb = travel(both, 2);
  MESSAGE("barchans over a storm: " << t1 << " mm; gain 10 " << tg << ", celerity 4 " << tc
                                    << ", both " << tb);
  // The scale divides the band's celerity height in whole millimetres, so the exact factor is the
  // ratio of the two heights (4.0 to within a height's rounding).
  const f64 scale = static_cast<f64>(DuneField(base).band_height(2)) /
                    static_cast<f64>(DuneField(c4).band_height(2));
  CHECK(scale == doctest::Approx(4.0).epsilon(0.01));
  CHECK(tc == doctest::Approx(scale * t1).epsilon(1.0e-3));
  CHECK(tb == doctest::Approx(scale * tg).epsilon(1.0e-3));
  CHECK(tg > 8.0 * t1);
  CHECK(tg <= 10.0 * t1 * (1.0 + 1.0e-3));
  // The gain enters the field's hash only when it is not 1.
  FieldDesc one = base;
  one.wind.storm_gain_q16 = 65536;
  CHECK(field_hash(one) == field_hash(base));
  CHECK(field_hash(g10) != field_hash(base));
}

TEST_CASE("terrain wind: the rose files every hour's transport by where it blew") {
  // Without the day or a storm, a day's sand goes into one sector: its own direction's.
  WindParams p;
  p.seed = 5;
  const WindRecord plain(p);
  i64 rose[k_rose_sectors];
  plain.rose_between(20 * k_day, 21 * k_day, rose);
  const u32 sector = rose_sector(plain.day(20).turn);
  for (u32 s = 0; s < k_rose_sectors; ++s)
    CHECK(rose[s] == (s == sector ? plain.day(20).magnitude : 0));
  // With both: hour by hour, the hour's day share goes where the hour veered and its storm share
  // where the storm blew, and the day's rose is the hours'.
  const WindRecord both(with_day(stormy(5)));
  const WindStorm& storm = both.storms()[2];
  const i64 day = storm.day;
  i64 hours[k_rose_sectors] = {};
  for (i32 h = 0; h < 24; ++h) {
    i64 one[k_rose_sectors];
    const i64 t = day * k_day + h * k_hour;
    both.rose_between(t, t + k_hour, one);
    const WindAt at = both.wind_at(t);
    i64 sum = 0;
    for (u32 s = 0; s < k_rose_sectors; ++s) {
      hours[s] += one[s];
      sum += one[s];
    }
    CHECK(sum == at.flux.magnitude);
    if (at.storm) CHECK(one[rose_sector(storm.turn)] > 0);
  }
  both.rose_between(day * k_day, (day + 1) * k_day, rose);
  for (u32 s = 0; s < k_rose_sectors; ++s)
    CHECK(rose[s] == hours[s]);
  // A year's rose: the prevailing wind's sector and its neighbours hold most of the sand.
  i64 year[k_rose_sectors];
  both.rose_between(0, k_year, year);
  const u32 prevailing = rose_sector(both.prevailing_turn());
  i64 near = 0, all = 0;
  for (u32 s = 0; s < k_rose_sectors; ++s) {
    all += year[s];
    const u32 dist = std::min((s - prevailing) & 15u, (prevailing - s) & 15u);
    if (dist <= 2) near += year[s];
  }
  MESSAGE("a year's rose: " << static_cast<f64>(near) / static_cast<f64>(all)
                            << " of the sand within two sectors of the prevailing wind");
  CHECK(near * 2 > all);
}

TEST_CASE("terrain wind: a field with the day and gained storms is the same on any thread count") {
  FieldDesc d;
  d.seed = 13;
  d.wind = with_day(stormy(13));
  d.wind.storm_gain_q16 = 50 * 65536;
  const DuneField field(d);
  const i64 t = 2 * k_year + 100 * k_day + 15 * k_hour;
  Vector<i64> none, one, three;
  evaluate_grid(field, -40'000, -40'000, 129, 129, 625, t, Detail::dunes, nullptr, nullptr, none);
  jobs::JobSystem single(jobs::JobSystemConfig{.performance_workers = 1, .pin_threads = false});
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 3, .pin_threads = false});
  evaluate_grid(field, -40'000, -40'000, 129, 129, 625, t, Detail::dunes, nullptr, &single, one);
  evaluate_grid(field, -40'000, -40'000, 129, 129, 625, t, Detail::dunes, nullptr, &pool, three);
  REQUIRE(none.size() == one.size());
  REQUIRE(none.size() == three.size());
  u32 same = 0;
  for (u32 i = 0; i < none.size(); ++i)
    same += none[i] == one[i] && none[i] == three[i];
  CHECK(same == none.size());
}

#include <core/hash/hash.h>
#include <domain/terrain/fixed.h>
#include <domain/terrain/wind.h>

#include <algorithm>

namespace engine::terrain {

namespace {

// Stream tags: every draw is a hash of the seed, what the draw is for, and the day, so a draw
// added for one purpose moves nothing drawn for another.
constexpr u64 k_tag_wind = 0x57494E445245434Full;  // "WINDRECO"
constexpr u64 k_tag_prevailing = 1;
constexpr u64 k_tag_direction = 2;
constexpr u64 k_tag_gust = 3;
constexpr u64 k_tag_calm = 4;
constexpr u64 k_tag_storm = 5;  // and its sub-streams below: storms draw nothing a day draws
constexpr i64 k_us_per_hour = 3'600 * k_us_per_second;

u64 draw_hash(u64 seed, u64 tag, i64 index) noexcept {
  return hash_combine(hash_combine(hash_combine(seed, k_tag_wind), tag), static_cast<u64>(index));
}

}  // namespace

i64 day_of(i64 time_us) noexcept { return fx::floor_div(time_us, k_us_per_day); }

WindRecord::WindRecord(const WindParams& params) : params_(params) {
  prevailing_ = params.seeded_direction
                    ? static_cast<u16>(draw_hash(params.seed, k_tag_prevailing, 0) >> 48)
                    : params.prevailing_turn;
  prevailing_x_ = fx::cos_q15(prevailing_) / 2;
  prevailing_z_ = fx::sin_q15(prevailing_) / 2;

  // Strength first, relative to the mean, then the flux by the cube law, normalized so the record's
  // mean flux is the one asked for.
  days_.resize(k_record_days);
  Vector<i64> raw;
  raw.resize(k_record_days);
  i64 raw_total = 0;
  for (i32 d = 0; d < k_record_days; ++d) {
    const i32 doy = d % k_days_per_year;
    const u32 season = static_cast<u32>((static_cast<i64>(doy) * fx::k_turn) / k_days_per_year);
    // The direction swings about the prevailing one over the year, and each day strays from it.
    const i64 swing = (static_cast<i64>(params.swing_turn) * fx::sin_q15(season)) >> 15;
    const i64 jitter = fx::draw(draw_hash(params.seed, k_tag_direction, d), -params.jitter_turn,
                                params.jitter_turn);
    const u16 turn = static_cast<u16>(static_cast<i64>(prevailing_) + swing + jitter);
    // The strength peaks an eighth of a year after the direction's swing does: the windy season
    // is not the season the wind turns in.
    const i64 seasonal =
        (static_cast<i64>(params.seasonal_q16) * fx::sin_q15(season + fx::k_turn / 8)) >> 15;
    const i64 gust =
        fx::draw(draw_hash(params.seed, k_tag_gust, d), -params.gust_q16, params.gust_q16);
    i64 speed = fx::clamp_i64(fx::k_one_q16 + seasonal + gust, 0, 3 * fx::k_one_q16);
    const bool calm = fx::unit_q16(draw_hash(params.seed, k_tag_calm, d)) < params.calm_q16;
    if (calm) speed = 0;
    WindDay& day = days_[d];
    day.turn = turn;
    day.calm = calm ? 1 : 0;
    day.speed_q16 = static_cast<i32>(speed);
    raw[d] = (((speed * speed) >> 16) * speed) >> 16;
    raw_total += raw[d];
  }
  prefix_.resize(k_record_days + 1);
  FluxIntegral sum;
  prefix_[0] = sum;
  for (i32 d = 0; d < k_record_days; ++d) {
    WindDay& day = days_[d];
    const i64 magnitude =
        raw_total > 0 ? (raw[d] * params.flux_cm2_per_day * k_record_days) / raw_total : 0;
    day.magnitude = static_cast<i32>(magnitude);
    day.fx = static_cast<i32>((magnitude * fx::cos_q15(day.turn)) >> 15);
    day.fz = static_cast<i32>((magnitude * fx::sin_q15(day.turn)) >> 15);
  }

  // Storms: drawn from their own streams, so a record with none is the same bytes as before they
  // existed, and each storm's day, hours, direction and peak from a hash of its own index.
  const i32 storm_count =
      std::clamp(params.storms_per_year, 0, k_max_storms_per_year) * k_record_years;
  for (i32 k = 0; k < storm_count && raw_total > 0; ++k) {
    const u64 h = draw_hash(params.seed, k_tag_storm, k);
    WindStorm storm;
    // A day nobody else's storm has, probing forward from the one drawn.
    storm.day = static_cast<i32>(fx::draw(hash_combine(h, 1), 0, k_record_days - 1));
    for (i32 probe = 0; probe < k_record_days && days_[storm.day].storm != 0; ++probe)
      storm.day = (storm.day + 1) % k_record_days;
    const i32 lo = std::clamp(params.storm_hours_min, 1, 24);
    const i32 hi = std::clamp(params.storm_hours_max, lo, 24);
    storm.hours = static_cast<i32>(fx::draw(hash_combine(h, 2), lo, hi));
    storm.first_hour = static_cast<i32>(fx::draw(hash_combine(h, 3), 0, 24 - storm.hours));
    storm.turn = static_cast<u16>(
        static_cast<i64>(prevailing_) +
        fx::draw(hash_combine(h, 4), -params.storm_spread_turn, params.storm_spread_turn));
    // Storms differ: each peaks at 80% to 120% of the strength asked for.
    storm.peak_q16 = static_cast<i32>(
        (static_cast<i64>(params.storm_speed_q16) * fx::draw(hash_combine(h, 5), 52429, 78643)) >>
        16);
    days_[storm.day].storm = 1;  // taken; numbered once the storms are in day order
    storms_.push_back(storm);
  }
  std::sort(storms_.begin(), storms_.end(),
            [](const WindStorm& a, const WindStorm& b) { return a.day < b.day; });
  for (u32 k = 0; k < storms_.size(); ++k) {
    WindStorm& storm = storms_[k];
    WindDay& day = days_[storm.day];
    day.storm = static_cast<u8>(k + 1);
    // The day's own flux spread evenly over its hours, exactly as `integral` spreads a day without
    // a storm, plus the storm's hours: a half sine of strength over them, the cube law's flux on
    // the days' scale (a day of strength s moves raw(s) * flux * days / raw_total).
    FluxIntegral storm_sum;
    for (i32 hour = 0; hour <= 24; ++hour) {
      FluxIntegral& at = storm.hour[hour];
      at.x = fx::floor_div(static_cast<i64>(day.fx) * hour, 24) + storm_sum.x;
      at.z = fx::floor_div(static_cast<i64>(day.fz) * hour, 24) + storm_sum.z;
      at.magnitude = (static_cast<i64>(day.magnitude) * hour) / 24 + storm_sum.magnitude;
      const i32 k_in = hour - storm.first_hour;
      if (hour == 24 || k_in < 0 || k_in >= storm.hours) continue;
      const u32 phase = static_cast<u32>(((2 * k_in + 1) * (fx::k_turn / 2)) / (2 * storm.hours));
      const i64 speed = (static_cast<i64>(storm.peak_q16) * fx::sin_q15(phase)) >> 15;
      const i64 raw_hour = (((speed * speed) >> 16) * speed) >> 16;
      const i64 magnitude = (raw_hour * params.flux_cm2_per_day * k_record_days) / (raw_total * 24);
      storm_sum.x += (magnitude * fx::cos_q15(storm.turn)) >> 15;
      storm_sum.z += (magnitude * fx::sin_q15(storm.turn)) >> 15;
      storm_sum.magnitude += magnitude;
    }
    day.fx = static_cast<i32>(storm.hour[24].x);
    day.fz = static_cast<i32>(storm.hour[24].z);
    day.magnitude = static_cast<i32>(storm.hour[24].magnitude);
  }
  for (i32 d = 0; d < k_record_days; ++d) {
    const WindDay& day = days_[d];
    sum.x += day.fx;
    sum.z += day.fz;
    sum.magnitude += day.magnitude;
    prefix_[d + 1] = sum;
  }
}

const WindDay& WindRecord::day(i64 day_index) const noexcept {
  return days_[static_cast<u32>(fx::floor_mod(day_index, k_record_days))];
}

FluxIntegral WindRecord::integral(i64 time_us) const noexcept {
  const i64 day = day_of(time_us);
  const i64 into = time_us - day * k_us_per_day;
  const i64 cycles = fx::floor_div(day, k_record_days);
  const i32 r = static_cast<i32>(day - cycles * k_record_days);
  const FluxIntegral& total = prefix_[k_record_days];
  const FluxIntegral& before = prefix_[r];
  const WindDay& today = days_[r];
  FluxIntegral out;
  if (today.storm != 0) {
    // A storm day: its hours' cumulative flux, a straight line within the hour.
    const WindStorm& storm = storms_[today.storm - 1u];
    const i64 hour = into / k_us_per_hour;
    const i64 frac = into - hour * k_us_per_hour;
    const FluxIntegral& a = storm.hour[hour];
    const FluxIntegral& b = storm.hour[hour + 1];
    out.x = cycles * total.x + before.x + a.x + fx::floor_div((b.x - a.x) * frac, k_us_per_hour);
    out.z = cycles * total.z + before.z + a.z + fx::floor_div((b.z - a.z) * frac, k_us_per_hour);
    out.magnitude = cycles * total.magnitude + before.magnitude + a.magnitude +
                    ((b.magnitude - a.magnitude) * frac) / k_us_per_hour;
    return out;
  }
  // The day's flux is spread evenly over the day, so the integral is continuous in time and a
  // dune moves smoothly rather than once at midnight.
  out.x = cycles * total.x + before.x + fx::floor_div(today.fx * into, k_us_per_day);
  out.z = cycles * total.z + before.z + fx::floor_div(today.fz * into, k_us_per_day);
  out.magnitude =
      cycles * total.magnitude + before.magnitude + (today.magnitude * into) / k_us_per_day;
  return out;
}

WindAt WindRecord::wind_at(i64 time_us) const noexcept {
  WindAt out;
  out.day = day_of(time_us);
  const i64 into = time_us - out.day * k_us_per_day;
  out.hour = static_cast<i32>(into / k_us_per_hour);
  const WindDay& today = day(out.day);
  out.turn = today.turn;
  out.speed_q16 = today.speed_q16;
  if (today.storm != 0) {
    const WindStorm& storm = storms_[today.storm - 1u];
    out.flux = storm.hour[out.hour + 1] - storm.hour[out.hour];
    const i32 k = out.hour - storm.first_hour;
    if (k >= 0 && k < storm.hours) {
      out.storm = true;
      out.turn = storm.turn;
      const u32 phase = static_cast<u32>(((2 * k + 1) * (fx::k_turn / 2)) / (2 * storm.hours));
      out.speed_q16 =
          std::max(today.speed_q16,
                   static_cast<i32>((static_cast<i64>(storm.peak_q16) * fx::sin_q15(phase)) >> 15));
    }
    return out;
  }
  // The same arithmetic `integral` spreads a day by, at the hour's two ends.
  const i64 h = out.hour;
  out.flux.x = fx::floor_div(static_cast<i64>(today.fx) * (h + 1), 24) -
               fx::floor_div(static_cast<i64>(today.fx) * h, 24);
  out.flux.z = fx::floor_div(static_cast<i64>(today.fz) * (h + 1), 24) -
               fx::floor_div(static_cast<i64>(today.fz) * h, 24);
  out.flux.magnitude = (static_cast<i64>(today.magnitude) * (h + 1)) / 24 -
                       (static_cast<i64>(today.magnitude) * h) / 24;
  return out;
}

}  // namespace engine::terrain

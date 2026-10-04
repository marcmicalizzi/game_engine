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
    day.magnitude = magnitude;
    day.fx = (magnitude * fx::cos_q15(day.turn)) >> 15;
    day.fz = (magnitude * fx::sin_q15(day.turn)) >> 15;
  }

  // The day's profile (terrain.md, "The day's wind"): one table of hours every day shares, a
  // function of the time of day alone. Each hour's strength factor is 1 + diurnal cos(the hour's
  // middle - the peak), its sand the cube of that, normalized so a day's hours sum to its own total
  // exactly (the last cumulative weight is 2^30); its direction the day's plus the veer's sine.
  diurnal_ = params.diurnal_q16 != 0 || params.veer_turn != 0;
  if (diurnal_) {
    i64 raw_hour[24];
    i64 raw_sum = 0;
    for (i32 h = 0; h < 24; ++h) {
      const u32 angle = static_cast<u32>(((2 * h + 1) * static_cast<i64>(fx::k_turn)) / 48);
      const i64 strength = fx::clamp_i64(
          fx::k_one_q16 + ((static_cast<i64>(params.diurnal_q16) *
                            fx::cos_q15(angle - static_cast<u32>(params.diurnal_peak_turn))) >>
                           15),
          0, 3 * fx::k_one_q16);
      strength_[h] = static_cast<i32>(strength);
      raw_hour[h] = (((strength * strength) >> 16) * strength) >> 16;
      raw_sum += raw_hour[h];
      veer_[h] = static_cast<i32>((static_cast<i64>(params.veer_turn) *
                                   fx::sin_q15(angle - static_cast<u32>(params.veer_phase_turn))) >>
                                  15);
    }
    i64 cumulative = 0;
    for (i32 h = 0; h < 24; ++h) {
      // A profile of nothing but calm (no strength at any hour) spreads the day evenly.
      cumulative += raw_sum > 0 ? raw_hour[h] : 1;
      const i64 total = raw_sum > 0 ? raw_sum : 24;
      weight_[h + 1] = (cumulative << 30) / total;
      const i64 w = weight_[h + 1] - weight_[h];
      const u32 veer = static_cast<u32>(veer_[h]);
      along_[h + 1] = along_[h] + ((w * fx::cos_q15(veer)) >> 15);
      across_[h + 1] = across_[h] + ((w * fx::sin_q15(veer)) >> 15);
    }
    // A day's net vector under the profile: the veer shortens it, the magnitude is its own.
    for (i32 d = 0; d < k_record_days; ++d) {
      WindDay& day = days_[d];
      const FluxIntegral whole = profile_at(day, 24);
      day.fx = whole.x;
      day.fz = whole.z;
    }
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
    storm.gain_q16 = std::clamp(params.storm_gain_q16, 1, k_max_storm_gain_q16);
    days_[storm.day].storm = 1;  // taken; numbered once the storms are in day order
    storms_.push_back(storm);
  }
  std::sort(storms_.begin(), storms_.end(),
            [](const WindStorm& a, const WindStorm& b) { return a.day < b.day; });
  for (const WindParams::StormGain& g : params.storm_gains) {
    if (g.storm >= 0 && static_cast<u32>(g.storm) < storms_.size())
      storms_[static_cast<u32>(g.storm)].gain_q16 = std::clamp(g.gain_q16, 1, k_max_storm_gain_q16);
  }
  for (u32 k = 0; k < storms_.size(); ++k) {
    WindStorm& storm = storms_[k];
    WindDay& day = days_[storm.day];
    day.storm = static_cast<u8>(k + 1);
    // The day's own flux spread evenly over its hours, exactly as `integral` spreads a day without
    // a storm, plus the storm's hours: a half sine of strength over them, the cube law's flux on
    // the days' scale (a day of strength s moves raw(s) * flux * days / raw_total).
    // With the day's profile, the day's own share follows it rather than a straight line. The
    // storm's hours are multiplied by its transport gain (1: nothing, and the same bytes).
    FluxIntegral storm_sum;
    for (i32 hour = 0; hour <= 24; ++hour) {
      FluxIntegral& at = storm.hour[hour];
      if (diurnal_) {
        const FluxIntegral own = profile_at(day, hour);
        at.x = own.x + storm_sum.x;
        at.z = own.z + storm_sum.z;
        at.magnitude = own.magnitude + storm_sum.magnitude;
      } else {
        at.x = fx::floor_div(day.fx * hour, 24) + storm_sum.x;
        at.z = fx::floor_div(day.fz * hour, 24) + storm_sum.z;
        at.magnitude = (day.magnitude * hour) / 24 + storm_sum.magnitude;
      }
      storm.storm_magnitude[hour] = storm_sum.magnitude;
      const i32 k_in = hour - storm.first_hour;
      if (hour == 24 || k_in < 0 || k_in >= storm.hours) continue;
      const u32 phase = static_cast<u32>(((2 * k_in + 1) * (fx::k_turn / 2)) / (2 * storm.hours));
      const i64 speed = (static_cast<i64>(storm.peak_q16) * fx::sin_q15(phase)) >> 15;
      const i64 raw_hour = (((speed * speed) >> 16) * speed) >> 16;
      i64 magnitude = (raw_hour * params.flux_cm2_per_day * k_record_days) / (raw_total * 24);
      if (storm.gain_q16 != fx::k_one_q16) magnitude = (magnitude * storm.gain_q16) >> 16;
      storm_sum.x += (magnitude * fx::cos_q15(storm.turn)) >> 15;
      storm_sum.z += (magnitude * fx::sin_q15(storm.turn)) >> 15;
      storm_sum.magnitude += magnitude;
    }
    day.fx = storm.hour[24].x;
    day.fz = storm.hour[24].z;
    day.magnitude = storm.hour[24].magnitude;
  }
  for (i32 d = 0; d < k_record_days; ++d) {
    const WindDay& day = days_[d];
    sum.x += day.fx;
    sum.z += day.fz;
    sum.magnitude += day.magnitude;
    prefix_[d + 1] = sum;
  }

  // The rose's prefix sums: each day's transport filed by direction, hour by hour where the day has
  // hours of its own (a storm, or the day's profile), whole where it is one straight line.
  rose_prefix_.resize(static_cast<usize>(k_record_days + 1) * k_rose_sectors);
  for (u32 s = 0; s < k_rose_sectors; ++s)
    rose_prefix_[s] = 0;
  for (i32 d = 0; d < k_record_days; ++d) {
    i64 today[k_rose_sectors] = {};
    rose_in_day(d, k_us_per_day, today);
    const usize at = static_cast<usize>(d) * k_rose_sectors;
    for (u32 s = 0; s < k_rose_sectors; ++s)
      rose_prefix_[static_cast<u32>(at + k_rose_sectors + s)] =
          rose_prefix_[static_cast<u32>(at + s)] + today[s];
  }
}

FluxIntegral WindRecord::profile_at(const WindDay& day, i32 h) const noexcept {
  // The day's unit vector turned by the hours' weighted veer, Q30, then times the day's magnitude.
  const i64 c = fx::cos_q15(day.turn);
  const i64 s = fx::sin_q15(day.turn);
  const i64 dx = (c * along_[h] - s * across_[h]) >> 15;
  const i64 dz = (s * along_[h] + c * across_[h]) >> 15;
  FluxIntegral out;
  out.x = fx::floor_div(day.magnitude * dx, i64{1} << 30);
  out.z = fx::floor_div(day.magnitude * dz, i64{1} << 30);
  out.magnitude = (day.magnitude * weight_[h]) >> 30;
  return out;
}

u16 WindRecord::profile_turn(const WindDay& day, i32 h) const noexcept {
  return diurnal_ ? static_cast<u16>(static_cast<i64>(day.turn) + veer_[h]) : day.turn;
}

void WindRecord::rose_in_day(i32 r, i64 into, i64 (&out)[k_rose_sectors]) const noexcept {
  const WindDay& today = days_[static_cast<u32>(r)];
  if (today.storm == 0 && !diurnal_) {
    out[rose_sector(today.turn)] += (today.magnitude * into) / k_us_per_day;
    return;
  }
  const i64 hour = into >= k_us_per_day ? 24 : into / k_us_per_hour;
  const i64 frac = into - hour * k_us_per_hour;
  if (today.storm != 0) {
    const WindStorm& storm = storms_[today.storm - 1u];
    const u32 storm_sector = rose_sector(storm.turn);
    for (i32 k = 0; k < hour; ++k) {
      const i64 all = storm.hour[k + 1].magnitude - storm.hour[k].magnitude;
      const i64 blown = storm.storm_magnitude[k + 1] - storm.storm_magnitude[k];
      out[rose_sector(profile_turn(today, k))] += all - blown;
      out[storm_sector] += blown;
    }
    if (hour < 24 && frac > 0) {
      // The integral's own share of the hour, and the storm's part of it by the same rule: what is
      // left is the day's, so the sectors sum to the magnitude exactly.
      const i32 k = static_cast<i32>(hour);
      const i64 all =
          ((storm.hour[k + 1].magnitude - storm.hour[k].magnitude) * frac) / k_us_per_hour;
      const i64 blown =
          ((storm.storm_magnitude[k + 1] - storm.storm_magnitude[k]) * frac) / k_us_per_hour;
      out[rose_sector(profile_turn(today, k))] += all - blown;
      out[storm_sector] += blown;
    }
    return;
  }
  for (i32 k = 0; k < hour; ++k)
    out[rose_sector(profile_turn(today, k))] +=
        profile_at(today, k + 1).magnitude - profile_at(today, k).magnitude;
  if (hour < 24 && frac > 0) {
    const i32 k = static_cast<i32>(hour);
    out[rose_sector(profile_turn(today, k))] +=
        ((profile_at(today, k + 1).magnitude - profile_at(today, k).magnitude) * frac) /
        k_us_per_hour;
  }
}

void WindRecord::rose(i64 time_us, i64 (&out)[k_rose_sectors]) const noexcept {
  const i64 day = day_of(time_us);
  const i64 into = time_us - day * k_us_per_day;
  const i64 cycles = fx::floor_div(day, k_record_days);
  const i32 r = static_cast<i32>(day - cycles * k_record_days);
  const usize total = static_cast<usize>(k_record_days) * k_rose_sectors;
  const usize before = static_cast<usize>(r) * k_rose_sectors;
  for (u32 s = 0; s < k_rose_sectors; ++s)
    out[s] = cycles * rose_prefix_[static_cast<u32>(total + s)] +
             rose_prefix_[static_cast<u32>(before + s)];
  rose_in_day(r, into, out);
}

void WindRecord::rose_between(i64 from_us, i64 to_us, i64 (&out)[k_rose_sectors]) const noexcept {
  i64 from[k_rose_sectors];
  rose(from_us, from);
  rose(to_us, out);
  for (u32 s = 0; s < k_rose_sectors; ++s)
    out[s] -= from[s];
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
  if (diurnal_) {
    // The day's profile: its hours' cumulative flux, a straight line within the hour.
    const i64 hour = into / k_us_per_hour;
    const i64 frac = into - hour * k_us_per_hour;
    const FluxIntegral a = profile_at(today, static_cast<i32>(hour));
    const FluxIntegral b = profile_at(today, static_cast<i32>(hour) + 1);
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

void WindRecord::magnitude_at(i64 time_us, i64& whole, f64& fraction) const noexcept {
  // `integral`'s magnitude term by term: the periods, the prefix to the day, the day's (or the
  // hour's) start, and the one division along the line, `numerator / denominator`, whose remainder
  // is the fraction. Every term is non-negative — the record's cumulative magnitudes never fall and
  // the time into a day is floored — so C++'s truncating division is the floor here.
  const i64 day = day_of(time_us);
  const i64 into = time_us - day * k_us_per_day;
  const i64 cycles = fx::floor_div(day, k_record_days);
  const i32 r = static_cast<i32>(day - cycles * k_record_days);
  const WindDay& today = days_[r];
  i64 base = cycles * prefix_[k_record_days].magnitude + prefix_[r].magnitude;
  i64 numerator = 0;
  i64 denominator = k_us_per_hour;
  if (today.storm != 0) {
    const WindStorm& storm = storms_[today.storm - 1u];
    const i64 hour = into / k_us_per_hour;
    const i64 frac = into - hour * k_us_per_hour;
    base += storm.hour[hour].magnitude;
    numerator = (storm.hour[hour + 1].magnitude - storm.hour[hour].magnitude) * frac;
  } else if (diurnal_) {
    const i64 hour = into / k_us_per_hour;
    const i64 frac = into - hour * k_us_per_hour;
    const i64 a = profile_at(today, static_cast<i32>(hour)).magnitude;
    const i64 b = profile_at(today, static_cast<i32>(hour) + 1).magnitude;
    base += a;
    numerator = (b - a) * frac;
  } else {
    numerator = today.magnitude * into;
    denominator = k_us_per_day;
  }
  whole = base + numerator / denominator;
  fraction = static_cast<f64>(numerator % denominator) / static_cast<f64>(denominator);
}

WindAt WindRecord::wind_at(i64 time_us) const noexcept {
  WindAt out;
  out.day = day_of(time_us);
  const i64 into = time_us - out.day * k_us_per_day;
  out.hour = static_cast<i32>(into / k_us_per_hour);
  const WindDay& today = day(out.day);
  out.turn = profile_turn(today, out.hour);
  out.speed_q16 =
      diurnal_ ? static_cast<i32>((static_cast<i64>(today.speed_q16) * strength_[out.hour]) >> 16)
               : today.speed_q16;
  if (today.storm != 0) {
    const WindStorm& storm = storms_[today.storm - 1u];
    out.flux = storm.hour[out.hour + 1] - storm.hour[out.hour];
    const i32 k = out.hour - storm.first_hour;
    if (k >= 0 && k < storm.hours) {
      out.storm = true;
      out.turn = storm.turn;
      out.gain_q16 = storm.gain_q16;
      const u32 phase = static_cast<u32>(((2 * k + 1) * (fx::k_turn / 2)) / (2 * storm.hours));
      out.speed_q16 =
          std::max(out.speed_q16,
                   static_cast<i32>((static_cast<i64>(storm.peak_q16) * fx::sin_q15(phase)) >> 15));
    }
    return out;
  }
  if (diurnal_) {
    out.flux = profile_at(today, out.hour + 1) - profile_at(today, out.hour);
    return out;
  }
  // The same arithmetic `integral` spreads a day by, at the hour's two ends.
  const i64 h = out.hour;
  out.flux.x = fx::floor_div(today.fx * (h + 1), 24) - fx::floor_div(today.fx * h, 24);
  out.flux.z = fx::floor_div(today.fz * (h + 1), 24) - fx::floor_div(today.fz * h, 24);
  out.flux.magnitude = (today.magnitude * (h + 1)) / 24 - (today.magnitude * h) / 24;
  return out;
}

}  // namespace engine::terrain

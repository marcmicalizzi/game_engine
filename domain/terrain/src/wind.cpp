#include <core/hash/hash.h>
#include <domain/terrain/fixed.h>
#include <domain/terrain/wind.h>

namespace engine::terrain {

namespace {

// Stream tags: every draw is a hash of the seed, what the draw is for, and the day, so a draw
// added for one purpose moves nothing drawn for another.
constexpr u64 k_tag_wind = 0x57494E445245434Full;  // "WINDRECO"
constexpr u64 k_tag_prevailing = 1;
constexpr u64 k_tag_direction = 2;
constexpr u64 k_tag_gust = 3;
constexpr u64 k_tag_calm = 4;

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
  // The day's flux is spread evenly over the day, so the integral is continuous in time and a
  // dune moves smoothly rather than once at midnight.
  FluxIntegral out;
  out.x = cycles * total.x + before.x + fx::floor_div(today.fx * into, k_us_per_day);
  out.z = cycles * total.z + before.z + fx::floor_div(today.fz * into, k_us_per_day);
  out.magnitude =
      cycles * total.magnitude + before.magnitude + (today.magnitude * into) / k_us_per_day;
  return out;
}

}  // namespace engine::terrain

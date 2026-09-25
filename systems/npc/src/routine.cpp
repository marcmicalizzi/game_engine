#include <core/hash/hash.h>
#include <systems/npc/routine.h>

namespace engine::npc {

namespace {

using S = ResidentState;
using P = PlaceRole;

constexpr RoutineRow row(S state, P place, u16 duration_lo, u16 duration_hi) noexcept {
  return RoutineRow{state, place, k_no_start, k_no_start, duration_lo, duration_hi};
}
constexpr RoutineRow at(S state, P place, u16 start_lo, u16 start_hi, u16 duration_lo,
                        u16 duration_hi) noexcept {
  return RoutineRow{state, place, start_lo, start_hi, duration_lo, duration_hi};
}

// ---- the tables (docs/subsystems/npc.md, "The routine tables") --------------------------------
//
// Minutes after midnight for a start, minutes for a duration. A `Travelling` row's place is where
// the trip goes; the trip starts wherever the row before it was. Lunch is eaten at work, so a
// working day has no trip in the middle of it.

constexpr RoutineRow k_day_worker[] = {
    at(S::AtHome, P::Home, 390, 450, 30, 60),     // up between 06:30 and 07:30
    row(S::Travelling, P::Work, 20, 40),          // the commute
    at(S::Working, P::Work, 540, 555, 210, 240),  // the shift starts at 09:00-09:15
    row(S::Eating, P::Work, 30, 60),              // lunch, at work
    row(S::Working, P::Work, 180, 240),           //
    row(S::Travelling, P::Service, 15, 30),       // to the shops
    row(S::Shopping, P::Service, 15, 45),         //
    row(S::Travelling, P::Home, 15, 30),          // home
    row(S::AtHome, P::Home, 60, 180),             //
    at(S::Sleeping, P::Home, 1320, 1380, 0, 0),   // bed between 22:00 and 23:00
};

// The day off every working archetype shares: late up, out for the afternoon, a meal out.
constexpr RoutineRow k_day_off[] = {
    at(S::AtHome, P::Home, 480, 600, 60, 120),  row(S::Travelling, P::Leisure, 15, 30),
    row(S::Leisure, P::Leisure, 120, 240),      row(S::Travelling, P::Service, 15, 30),
    row(S::Eating, P::Service, 45, 90),         row(S::Shopping, P::Service, 30, 60),
    row(S::Travelling, P::Home, 15, 30),        row(S::AtHome, P::Home, 30, 120),
    at(S::Sleeping, P::Home, 1350, 1410, 0, 0),
};

constexpr RoutineRow k_night_worker[] = {
    at(S::AtHome, P::Home, 780, 840, 60, 120),  // up at 13:00-14:00
    row(S::Travelling, P::Service, 15, 30),
    row(S::Shopping, P::Service, 30, 60),
    row(S::Travelling, P::Home, 15, 30),
    row(S::AtHome, P::Home, 120, 240),
    row(S::Travelling, P::Work, 20, 40),
    at(S::Working, P::Work, 1320, 1335, 240, 270),  // 22:00, into the next morning
    row(S::Eating, P::Work, 30, 45),
    row(S::Working, P::Work, 150, 180),
    row(S::Travelling, P::Home, 20, 40),
    row(S::Sleeping, P::Home, 0, 0),  // until the next day's row 0
};

constexpr RoutineRow k_night_off[] = {
    at(S::AtHome, P::Home, 720, 840, 60, 120),
    row(S::Travelling, P::Leisure, 15, 30),
    row(S::Leisure, P::Leisure, 120, 240),
    row(S::Travelling, P::Service, 15, 30),
    row(S::Eating, P::Service, 45, 90),
    row(S::Travelling, P::Home, 15, 30),
    row(S::AtHome, P::Home, 60, 180),
    at(S::Sleeping, P::Home, 1500, 1560, 0, 0),  // 01:00-02:00
};

constexpr RoutineRow k_shopkeeper[] = {
    at(S::AtHome, P::Home, 360, 420, 30, 45),
    row(S::Travelling, P::Work, 10, 20),          // the job is a service place: the shop
    at(S::Working, P::Work, 480, 480, 240, 240),  // opens at 08:00 sharp
    row(S::Eating, P::Work, 30, 45),
    row(S::Working, P::Work, 240, 300),
    row(S::Travelling, P::Home, 10, 20),
    row(S::AtHome, P::Home, 120, 240),
    at(S::Sleeping, P::Home, 1290, 1350, 0, 0),
};

constexpr RoutineRow k_student[] = {
    at(S::AtHome, P::Home, 420, 450, 30, 45),
    row(S::Travelling, P::Work, 10, 25),  // the job is the school
    at(S::Working, P::Work, 500, 510, 180, 200),
    row(S::Eating, P::Work, 30, 45),
    row(S::Working, P::Work, 120, 150),
    row(S::Travelling, P::Leisure, 10, 20),
    row(S::Leisure, P::Leisure, 60, 150),
    row(S::Travelling, P::Home, 10, 20),
    row(S::AtHome, P::Home, 120, 240),
    at(S::Sleeping, P::Home, 1320, 1380, 0, 0),
};

constexpr RoutineRow k_retiree[] = {
    at(S::AtHome, P::Home, 360, 480, 60, 120),  row(S::Travelling, P::Service, 10, 30),
    row(S::Shopping, P::Service, 30, 90),       row(S::Eating, P::Service, 45, 90),
    row(S::Travelling, P::Leisure, 10, 30),     row(S::Leisure, P::Leisure, 60, 180),
    row(S::Travelling, P::Home, 10, 30),        row(S::AtHome, P::Home, 60, 240),
    at(S::Sleeping, P::Home, 1260, 1350, 0, 0),
};

constexpr RoutineRow k_idle[] = {
    at(S::AtHome, P::Home, 540, 720, 120, 300), row(S::Travelling, P::Leisure, 10, 30),
    row(S::Leisure, P::Leisure, 120, 300),      row(S::Travelling, P::Home, 10, 30),
    row(S::AtHome, P::Home, 60, 240),           at(S::Sleeping, P::Home, 1380, 1440, 0, 0),
};

const RoutineTable k_tables[k_routine_count] = {
    {Routine::DayWorker, "day_worker", {k_day_worker}, {k_day_off}, 2, 30},
    {Routine::NightWorker, "night_worker", {k_night_worker}, {k_night_off}, 2, 30},
    {Routine::Shopkeeper, "shopkeeper", {k_shopkeeper}, {k_day_off}, 1, 15},
    {Routine::Student, "student", {k_student}, {k_day_off}, 2, 20},
    {Routine::Retiree, "retiree", {k_retiree}, {k_retiree}, 0, 60},
    {Routine::Idle, "idle", {k_idle}, {k_idle}, 0, 60},
};

constexpr i64 floor_div(i64 a, i64 b) noexcept {
  const i64 q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

constexpr i64 floor_mod(i64 a, i64 b) noexcept { return a - floor_div(a, b) * b; }

// One integer draw in [lo, hi] from the resident's hash and a key naming what is drawn.
u32 draw(u64 h, u64 key, u32 lo, u32 hi) noexcept {
  if (hi <= lo) return lo;
  return lo + static_cast<u32>(mix64(hash_combine(h, key)) % (static_cast<u64>(hi - lo) + 1u));
}

// The latest a day's last row can start, all durations and windows at their longest.
i64 latest_last_start(const RoutineDay& day) noexcept {
  i64 start = 0;
  for (usize i = 0; i < day.rows.size(); ++i) {
    const RoutineRow& r = day.rows[i];
    const i64 natural = i == 0 ? 0 : start + day.rows[i - 1].duration_hi;
    start = r.start_lo == k_no_start ? natural : (natural > r.start_hi ? natural : r.start_hi);
  }
  return start;
}

bool day_valid(const RoutineDay& day, const char** error) noexcept {
  const auto fail = [error](const char* what) {
    if (error != nullptr) *error = what;
    return false;
  };
  if (day.rows.empty() || day.rows.size() > k_max_rows) return fail("rows not in [1, 12]");
  if (day.rows[0].start_lo == k_no_start) return fail("row 0 has no start window");
  for (usize i = 0; i < day.rows.size(); ++i) {
    const RoutineRow& r = day.rows[i];
    if ((r.start_lo == k_no_start) != (r.start_hi == k_no_start))
      return fail("a start window with one end");
    if (r.start_lo != k_no_start && r.start_lo > r.start_hi) return fail("a start window reversed");
    if (i + 1 < day.rows.size()) {
      if (r.duration_lo == 0) return fail("a row that can last no time");
      if (r.duration_lo > r.duration_hi) return fail("a duration window reversed");
    }
  }
  if (day.rows[0].start_hi >= k_minutes_per_day) return fail("row 0 starts after midnight");
  return true;
}

DayPlan draw_plan(const RoutineDay& day, u64 h, u64 key, i32 shift) noexcept {
  DayPlan plan;
  plan.count = static_cast<u8>(day.rows.size());
  i64 start = 0;
  for (usize i = 0; i < day.rows.size(); ++i) {
    const RoutineRow& r = day.rows[i];
    const i64 natural = i == 0
                            ? 0
                            : start + draw(h, key + 0x100u + (i - 1), day.rows[i - 1].duration_lo,
                                           day.rows[i - 1].duration_hi);
    if (r.start_lo == k_no_start) {
      start = natural;
    } else {
      const i64 own = static_cast<i64>(draw(h, key + i, r.start_lo, r.start_hi)) + shift;
      start = natural > own ? natural : own;
    }
    plan.start[i] = static_cast<u16>(start);
  }
  return plan;
}

const RoutineDay& day_of(const Variation& v, bool off) noexcept {
  const RoutineTable& table = routine_table(v.routine);
  return off ? table.day_off : table.workday;
}

// The point for row `row` of routine day `day`.
RoutinePoint point_of(const Variation& v, i64 day, u32 row) noexcept {
  const bool off = is_day_off(v, day);
  const DayPlan& plan = off ? v.day_off : v.workday;
  const RoutineDay& rows = day_of(v, off);
  RoutinePoint p;
  p.day = day;
  p.row = static_cast<u8>(row);
  p.day_off = off;
  p.state = rows.rows[row].state;
  p.place = rows.rows[row].place;
  const i64 base = day * k_day_us;
  p.start_us = base + static_cast<i64>(plan.start[row]) * k_us_per_minute;
  if (row + 1u < plan.count) {
    p.end_us = base + static_cast<i64>(plan.start[row + 1]) * k_us_per_minute;
  } else {
    p.end_us =
        (day + 1) * k_day_us + static_cast<i64>(plan_of(v, day + 1).start[0]) * k_us_per_minute;
  }
  if (row > 0) {
    p.from = rows.rows[row - 1].place;
  } else {
    const RoutineDay& before = day_of(v, is_day_off(v, day - 1));
    p.from = before.rows[before.rows.size() - 1].place;
  }
  return p;
}

}  // namespace

const RoutineTable& routine_table(Routine routine) noexcept {
  const u32 index = static_cast<u32>(routine);
  return k_tables[index < k_routine_count ? index : static_cast<u32>(Routine::Idle)];
}

std::span<const RoutineTable> routine_tables() noexcept { return {k_tables, k_routine_count}; }

bool valid_table(const RoutineTable& table, const char** error) noexcept {
  if (!day_valid(table.workday, error) || !day_valid(table.day_off, error)) return false;
  if (table.days_off > k_days_per_week) {
    if (error != nullptr) *error = "more days off than a week has";
    return false;
  }
  // Every start is shifted by at most `shift` either way; row 0 must stay in the day.
  const i64 shift = table.shift;
  const i64 first_lo = table.workday.rows[0].start_lo < table.day_off.rows[0].start_lo
                           ? table.workday.rows[0].start_lo
                           : table.day_off.rows[0].start_lo;
  if (first_lo - shift < 0 || table.workday.rows[0].start_hi + shift >= k_minutes_per_day ||
      table.day_off.rows[0].start_hi + shift >= k_minutes_per_day) {
    if (error != nullptr) *error = "the shift moves row 0 out of its day";
    return false;
  }
  // The latest any day's last row can start must be before the earliest the next day can begin,
  // whichever kind of day either is.
  const i64 latest_work = latest_last_start(table.workday) + shift;
  const i64 latest_off = latest_last_start(table.day_off) + shift;
  const i64 latest = latest_work > latest_off ? latest_work : latest_off;
  if (latest >= k_minutes_per_day + first_lo - shift) {
    if (error != nullptr) *error = "a day can run into the next";
    return false;
  }
  return true;
}

Variation draw_variation(Routine routine, u64 world_seed, const Id128& id) noexcept {
  const RoutineTable& table = routine_table(routine);
  const u64 h = hash_combine(hash_combine(hash_combine(k_hash_seed, world_seed), id.hi), id.lo);
  Variation v;
  v.routine = table.routine;
  const i32 shift =
      static_cast<i32>(draw(h, 1, 0, 2u * table.shift)) - static_cast<i32>(table.shift);
  v.workday = draw_plan(table.workday, h, 0x1000, shift);
  v.day_off = draw_plan(table.day_off, h, 0x2000, shift);
  // Days off are consecutive, starting on a drawn day of the week: a weekend that falls anywhere.
  const u32 first = draw(h, 2, 0, k_days_per_week - 1u);
  for (u32 i = 0; i < table.days_off; ++i)
    v.off_mask = static_cast<u8>(v.off_mask | (1u << ((first + i) % k_days_per_week)));
  return v;
}

bool is_day_off(const Variation& variation, i64 day) noexcept {
  const u32 dow = static_cast<u32>(floor_mod(day, k_days_per_week));
  return ((variation.off_mask >> dow) & 1u) != 0u;
}

const DayPlan& plan_of(const Variation& variation, i64 day) noexcept {
  return is_day_off(variation, day) ? variation.day_off : variation.workday;
}

RoutinePoint routine_at(const Variation& variation, i64 t_us) noexcept {
  i64 day = floor_div(t_us, k_day_us);
  const DayPlan* plan = &plan_of(variation, day);
  if (t_us < day * k_day_us + static_cast<i64>(plan->start[0]) * k_us_per_minute) {
    --day;
    plan = &plan_of(variation, day);
  }
  const i64 base = day * k_day_us;
  u32 row = plan->count - 1u;
  while (row > 0 && base + static_cast<i64>(plan->start[row]) * k_us_per_minute > t_us)
    --row;
  return point_of(variation, day, row);
}

RoutinePoint routine_at_offset(const Variation& variation, i64 t_us, i64 offset_us) noexcept {
  RoutinePoint p = routine_at(variation, t_us + offset_us);
  p.start_us -= offset_us;
  p.end_us -= offset_us;
  return p;
}

RoutinePoint routine_by_stepping(const Variation& variation, i64 from_us, i64 t_us) noexcept {
  // From row 0 of the day before `from`, which starts before `from` whatever the plans are, one row
  // at a time until the row that holds t.
  i64 day = floor_div(from_us, k_day_us) - 1;
  u32 row = 0;
  RoutinePoint p = point_of(variation, day, row);
  while (p.end_us <= t_us) {
    ++row;
    if (row >= plan_of(variation, day).count) {
      ++day;
      row = 0;
    }
    p = point_of(variation, day, row);
  }
  return p;
}

}  // namespace engine::npc

#pragma once

// A resident's routine as data, and where in it a resident is at any game time
// (docs/subsystems/npc.md, "The routine model"; ADR-0045).
//
// ECS-free on purpose: a routine is a function of (table, variation, t), so a test, a bench, the
// generator and the summarizer all run it without a flecs world.
//
// **A routine is a table of rows**, one day long: (state, place role, start window, duration
// window). Row 0 starts the routine day at a time drawn from its start window; each later row
// starts when the row before it ends — the earlier row's start plus a duration drawn from its
// window — or, when the row has a start window of its own, at the later of that and the time drawn
// from the window, so a shift starts on time however early the commute got there. The last row
// (sleep) runs until the next day's row 0. A routine has two such days: a work day and a day off,
// chosen per day of the week by a days-off mask.
//
// **The variation is drawn once**, from the world seed hashed with the record id: a shift offset
// that moves the whole day, a draw inside every row's windows, and which days are off. Everything
// is integer minutes and integer microseconds, so the plan is the same bytes on every compiler.
//
// **The closed form.** Given the variation, `routine_at(t)` finds the day t falls in (day d, or
// d − 1 when t is before day d's row 0), and the row within it — a walk of at most k_max_rows
// entries — so where a resident is at t costs the same whether t is a minute or a decade after the
// last time anyone looked. That is what lets the summarizer skip a gap without visiting it.

#include <core/base/types.h>
#include <core/ids/id128.h>

#include <schemas/npc.h>
#include <span>

namespace engine::npc {

inline constexpr i64 k_us_per_minute = 60'000'000;
inline constexpr i64 k_minutes_per_day = 1440;
inline constexpr i64 k_day_us = k_minutes_per_day * k_us_per_minute;
inline constexpr u32 k_days_per_week = 7;
inline constexpr u32 k_max_rows = 12;
inline constexpr u32 k_routine_count = 6;
// A window of {0, 0} on a row after the first means "no start of its own: when the last row ends".
inline constexpr u16 k_no_start = 0xFFFFu;

// One row of a routine day. Minutes, integers.
struct RoutineRow {
  ResidentState state = ResidentState::Sleeping;
  PlaceRole place = PlaceRole::Home;
  // Earliest and latest start, minutes after midnight of the routine day (may pass 1440 for a row
  // that starts after midnight). k_no_start: this row starts when the one before it ends.
  u16 start_lo = k_no_start;
  u16 start_hi = k_no_start;
  // Shortest and longest duration, minutes. Ignored on the last row, which runs to the next day.
  u16 duration_lo = 0;
  u16 duration_hi = 0;
};

// One kind of day.
struct RoutineDay {
  std::span<const RoutineRow> rows;
};

// One archetype: its work day, its day off, how many days a week are off, and the widest shift
// offset (minutes, drawn in [-shift, +shift]) that moves a whole day.
struct RoutineTable {
  Routine routine = Routine::Idle;
  const char* name = "";
  RoutineDay workday;
  RoutineDay day_off;
  u8 days_off = 0;
  u16 shift = 0;
};

// The six archetypes, indexed by `Routine`.
const RoutineTable& routine_table(Routine routine) noexcept;
std::span<const RoutineTable> routine_tables() noexcept;

// Whether a table can be run: rows in [1, k_max_rows], row 0 has a start window, every window is
// ordered, and the latest the last row can start is before the earliest the next day's row 0 can,
// so a day never runs into the next. `error` says what is wrong. The shipped tables are checked by
// a test; a game's own table would be checked where it is loaded.
bool valid_table(const RoutineTable& table, const char** error = nullptr) noexcept;

// One resident's day, drawn: every row's start in minutes after midnight of the routine day.
struct DayPlan {
  u16 start[k_max_rows] = {};
  u8 count = 0;
  u8 pad[3] = {};
};

// Everything the variation drew for one resident: both kinds of day, which days of the week are
// off (bit d set: day d of the week is a day off; day 0 of the game is day 0 of the week), and the
// routine it was drawn for. 60 bytes, and a pure function of (routine, world seed, id).
struct Variation {
  DayPlan workday;
  DayPlan day_off;
  u8 off_mask = 0;
  Routine routine = Routine::Idle;
  u8 pad[2] = {};
};

// The draw (ADR-0045: once, from the world seed hashed with the record id).
Variation draw_variation(Routine routine, u64 world_seed, const Id128& id) noexcept;

// Where a resident is at a game time: the row, its state and place, when the row started and when
// it ends, and the place of the row before it (a trip's origin). A row is the half-open interval
// [start, end), so at exactly `end` the resident is in the next row.
struct RoutinePoint {
  i64 start_us = 0;
  i64 end_us = 0;
  i64 day = 0;
  u8 row = 0;
  ResidentState state = ResidentState::Sleeping;
  PlaceRole place = PlaceRole::Home;
  PlaceRole from = PlaceRole::Home;
  bool day_off = false;
  u8 pad[3] = {};
};

// The closed form: O(k_max_rows), independent of `t`.
RoutinePoint routine_at(const Variation& variation, i64 t_us) noexcept;

// The closed form on a clock offset from game time: `routine_at(variation, t + offset)` with the
// point's times moved back onto game time. What the capability and the generator both call.
RoutinePoint routine_at_offset(const Variation& variation, i64 t_us, i64 offset_us) noexcept;

// The same answer by stepping: from `start` row by row until t. O(rows crossed). The test holds the
// closed form to it; nothing else should call it.
RoutinePoint routine_by_stepping(const Variation& variation, i64 from_us, i64 t_us) noexcept;

// Which plan a day uses.
bool is_day_off(const Variation& variation, i64 day) noexcept;
const DayPlan& plan_of(const Variation& variation, i64 day) noexcept;

}  // namespace engine::npc

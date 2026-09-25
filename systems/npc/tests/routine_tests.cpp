// The routine model (docs/subsystems/npc.md, "The routine model"): the tables, the variation drawn
// once, and the closed form against stepping through the rows one at a time. ECS-free, as the
// model is.
#include <core/hash/hash.h>
#include <systems/npc/routine.h>

#include <doctest/doctest.h>

#include <cstring>

using namespace engine;
using namespace engine::npc;

namespace {

Id128 id_of(u32 n) { return Id128::from_parts(0x5EED, n); }

bool same(const RoutinePoint& a, const RoutinePoint& b) {
  return std::memcmp(&a, &b, sizeof(RoutinePoint)) == 0;
}

}  // namespace

TEST_CASE("npc routine: every shipped table can be run") {
  REQUIRE(routine_tables().size() == k_routine_count);
  for (u32 r = 0; r < k_routine_count; ++r) {
    const RoutineTable& table = routine_tables()[r];
    CAPTURE(table.name);
    CHECK(static_cast<u32>(table.routine) == r);
    const char* error = nullptr;
    CHECK_MESSAGE(valid_table(table, &error), (error != nullptr ? error : ""));
  }
}

TEST_CASE("npc routine: a table whose day runs into the next is refused, naming why") {
  // A sleeper who wakes at 06:00 and whose evening can last until 06:10 the next day.
  const RoutineRow rows[] = {
      {ResidentState::AtHome, PlaceRole::Home, 360, 360, 1400, 1450},
      {ResidentState::Sleeping, PlaceRole::Home, k_no_start, k_no_start, 0, 0},
  };
  RoutineTable table;
  table.workday = RoutineDay{rows};
  table.day_off = RoutineDay{rows};
  const char* error = nullptr;
  CHECK_FALSE(valid_table(table, &error));
  REQUIRE(error != nullptr);
  CHECK(std::string_view(error) == "a day can run into the next");

  const RoutineRow backwards[] = {
      {ResidentState::AtHome, PlaceRole::Home, 400, 300, 10, 20},
      {ResidentState::Sleeping, PlaceRole::Home, k_no_start, k_no_start, 0, 0},
  };
  table.workday = RoutineDay{backwards};
  CHECK_FALSE(valid_table(table, &error));
  CHECK(std::string_view(error) == "a start window reversed");
}

TEST_CASE("npc routine: the variation is drawn once, from the seed and the id, the same bytes") {
  const Variation a = draw_variation(Routine::DayWorker, 7, id_of(1));
  const Variation b = draw_variation(Routine::DayWorker, 7, id_of(1));
  CHECK(std::memcmp(&a, &b, sizeof(Variation)) == 0);
  const Variation other_seed = draw_variation(Routine::DayWorker, 8, id_of(1));
  const Variation other_id = draw_variation(Routine::DayWorker, 7, id_of(2));
  CHECK(std::memcmp(&a, &other_seed, sizeof(Variation)) != 0);
  CHECK(std::memcmp(&a, &other_id, sizeof(Variation)) != 0);

  // Every row lands in its windows, shifted as a whole day by at most the table's shift.
  for (u32 r = 0; r < k_routine_count; ++r) {
    const RoutineTable& table = routine_tables()[r];
    for (u32 n = 0; n < 200; ++n) {
      const Variation v = draw_variation(table.routine, 3, id_of(n));
      CHECK(v.workday.count == table.workday.rows.size());
      CHECK(v.day_off.count == table.day_off.rows.size());
      u32 off = 0;
      for (u32 d = 0; d < k_days_per_week; ++d)
        off += (v.off_mask >> d) & 1u;
      CHECK(off == table.days_off);
      for (u32 i = 1; i < v.workday.count; ++i)
        CHECK(v.workday.start[i] > v.workday.start[i - 1]);
      const RoutineRow& first = table.workday.rows[0];
      CHECK(v.workday.start[0] + table.shift >= first.start_lo);
      CHECK(v.workday.start[0] <= first.start_hi + table.shift);
    }
  }
}

TEST_CASE("npc routine: the closed form is where stepping through every row gets to") {
  // Three weeks at every minute boundary for a spread of residents of every routine, plus the
  // microsecond either side of every transition: the two must agree to the byte.
  u64 checked = 0;
  for (u32 r = 0; r < k_routine_count; ++r) {
    for (u32 n = 0; n < 12; ++n) {
      const Variation v = draw_variation(static_cast<Routine>(r), 11, id_of(100 + n));
      const i64 from = 0;
      for (i64 t = from; t < 21 * k_day_us; t += 37 * k_us_per_minute) {
        const RoutinePoint closed = routine_at(v, t);
        const RoutinePoint stepped = routine_by_stepping(v, from, t);
        REQUIRE(same(closed, stepped));
        CHECK(closed.start_us <= t);
        CHECK(t < closed.end_us);
        ++checked;
      }
      // Every transition of the first two weeks: the row ends exactly where the next begins, and a
      // microsecond before it the resident is still in the row.
      RoutinePoint p = routine_at(v, 0);
      while (p.end_us < 14 * k_day_us) {
        const RoutinePoint next = routine_at(v, p.end_us);
        CHECK(next.start_us == p.end_us);
        CHECK(same(routine_at(v, p.end_us - 1), p));
        CHECK(same(next, routine_by_stepping(v, 0, p.end_us)));
        // A trip starts where the row before it was.
        if (next.row > 0) CHECK(next.from == p.place);
        p = next;
        ++checked;
      }
    }
  }
  MESSAGE("points checked: " << checked);
}

TEST_CASE("npc routine: the closed form costs the same a decade on") {
  // Not a timing: the walk is at most k_max_rows rows whatever t is, so the answer at ten years is
  // found without visiting the 3,650 days before it, and it is the one stepping from nine days
  // before finds.
  const Variation v = draw_variation(Routine::NightWorker, 5, id_of(9));
  const i64 decade = 3650 * k_day_us + 13 * k_us_per_minute;
  const RoutinePoint closed = routine_at(v, decade);
  CHECK(same(closed, routine_by_stepping(v, decade - 9 * k_day_us, decade)));
  CHECK(closed.day >= 3649);
  // Before time 0 too: a world whose epoch is negative has residents in their routines.
  const RoutinePoint before = routine_at(v, -3 * k_day_us);
  CHECK(same(before, routine_by_stepping(v, -5 * k_day_us, -3 * k_day_us)));
}

TEST_CASE("npc routine: a night worker's shift crosses midnight and ends the next morning") {
  const Variation v = draw_variation(Routine::NightWorker, 1, id_of(3));
  // Find a working row on a work day and check it runs past midnight into the next day.
  bool crossed = false;
  for (i64 t = 0; t < 7 * k_day_us && !crossed; t += 10 * k_us_per_minute) {
    const RoutinePoint p = routine_at(v, t);
    if (p.state == ResidentState::Working && p.end_us > (p.day + 1) * k_day_us) crossed = true;
  }
  CHECK(crossed);
}

TEST_CASE("npc routine: the variation and the closed form are the same bytes on every compiler") {
  // Integer hashing and integer arithmetic only, so these are the numbers MSVC, Clang and GCC all
  // compute. A change that moves either moves every resident of every saved world, and says so
  // here; a change that moves one on one compiler only is a determinism bug (ADR-0035's kind).
  u64 variations = k_hash_seed;
  u64 points = k_hash_seed;
  for (u32 r = 0; r < k_routine_count; ++r) {
    for (u32 n = 0; n < 64; ++n) {
      const Variation v = draw_variation(static_cast<Routine>(r), 1234, id_of(n));
      variations = hash_combine(variations, hash_bytes(&v, sizeof(Variation)));
      for (i64 t = -k_day_us; t < 9 * k_day_us; t += 97 * k_us_per_minute) {
        const RoutinePoint p = routine_at(v, t);
        points = hash_combine(points, hash_bytes(&p, sizeof(RoutinePoint)));
      }
    }
  }
  MESSAGE("variations " << variations << ", points " << points);
  CHECK(variations == 10698992688090205271ull);
  CHECK(points == 14055978091157025603ull);
}

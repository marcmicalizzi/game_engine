// The rural filler's choice (docs/subsystems/dwellings.md, "The rural filler"): over
// plots, households and seeds every house it plans passes the interior's validators — every room
// reached, every stair with its headroom, no window in front of an inner wall; the household and
// the plot fix the type and the seed varies only what one type's houses differ in; a plot too small
// for what was asked gets the next plan down, and one too small for a cot is refused.

#include <domain/dwellings/rural_filler.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;
using namespace engine::dwellings;

namespace {

constexpr RuralPlot k_wide_plot{2400, 1500};
constexpr RuralPlot k_narrow_plot{760, 1500};
constexpr RuralPlot k_small_plot{620, 520};

}  // namespace

TEST_CASE("rural filler: every generated house passes the interior's rules") {
  const RuralStyle style;
  const RuralPlot plots[3] = {k_wide_plot, k_narrow_plot, k_small_plot};
  u32 seen[k_plan_types] = {};
  u32 stairs = 0;
  u32 houses = 0;
  for (const RuralPlot& plot : plots) {
    for (u8 persons = 1; persons <= 9; ++persons) {
      for (u8 means = 0; means <= 2; ++means) {
        for (u64 seed = 0; seed < 40; ++seed) {
          RuralChoice choice;
          InteriorPlan plan;
          std::string error;
          const Household household{persons, means};
          const bool ok =
              plan_rural(plot, household, style, seed * 7919 + persons, choice, plan, &error);
          INFO("plot " << plot.frontage_cm << " persons " << int{persons} << " means " << int{means}
                       << " seed " << seed << ": " << error);
          REQUIRE(ok);
          // The house stands inside its plot.
          CHECK(choice.outer_width_cm <= plot.frontage_cm);
          CHECK(choice.outer_depth_cm <= plot.depth_cm);
          CHECK(choice.outer_width_cm == plan.params.width_cm + 2 * style.outer_wall_cm);
          Vector<InteriorFinding> findings;
          CHECK(validate_interior(plan, findings));
          // Every room is reached from an outer door.
          Vector<u8> reached;
          CHECK(reachable_rooms(plan, reached) == plan.rooms.size());
          // No stair without headroom.
          for (const Stair& s : plan.stairs) {
            ++stairs;
            for (u32 t = 0; t + 1 < s.risers; ++t) {
              const RectCm tread = s.tread(t);
              CHECK(stair_headroom_cm(plan, s, (tread.x0 + tread.x1) / 2, (tread.z0 + tread.z1) / 2,
                                      s.tread_mm(t)) >= k_headroom_cm);
            }
          }
          // No window into an inner wall: a window's span keeps off every inner wall's end.
          for (const OuterOpening& o : plan.openings) {
            const bool along_x = o.side == Side::Front || o.side == Side::Back;
            for (const InnerWall& w : plan.walls) {
              if (w.storey != o.storey() || (w.x0 == w.x1) != along_x) continue;
              const i32 at = along_x ? w.x0 : w.z0;
              const i32 lo = o.at_cm - o.width_cm / 2;
              CHECK((at <= lo - 15 || at >= lo + o.width_cm + 15));
            }
          }
          ++seen[static_cast<u32>(plan.params.type)];
          ++houses;
        }
      }
    }
  }
  CHECK(houses == 3 * 9 * 3 * 40);
  for (u32 t = 0; t < k_plan_types; ++t)
    CHECK(seen[t] > 0);
  CHECK(stairs > 0);
}

TEST_CASE("rural filler: the household fixes the type and the seed varies the sizes") {
  const RuralStyle style;
  struct Row {
    u8 persons;
    u8 means;
    PlanType type;
    u8 storeys;
    u8 hearths;
    bool inner_room;
  };
  const Row rows[] = {
      {1, 0, PlanType::Cot, 1, 1, false},          {2, 2, PlanType::Cot, 1, 1, false},
      {3, 0, PlanType::HallParlour, 1, 1, false},  {4, 1, PlanType::HallParlour, 1, 1, false},
      {4, 2, PlanType::HallParlour, 2, 2, false},  {5, 1, PlanType::HallParlour, 2, 1, false},
      {6, 0, PlanType::CrossPassage, 1, 1, true},  {7, 0, PlanType::CrossPassage, 1, 1, true},
      {8, 1, PlanType::CrossPassage, 2, 1, false}, {9, 2, PlanType::CrossPassage, 2, 2, true},
  };
  for (const Row& row : rows) {
    u32 widths[64] = {};
    u32 distinct = 0;
    u32 mirrored = 0;
    for (u64 seed = 0; seed < 64; ++seed) {
      RuralChoice choice;
      std::string error;
      REQUIRE(choose_rural(k_wide_plot, Household{row.persons, row.means}, style, seed, choice,
                           &error));
      const InteriorParams& p = choice.interior;
      CHECK(p.type == row.type);
      CHECK(p.storeys == row.storeys);
      CHECK(p.hearths == row.hearths);
      CHECK(p.inner_room == row.inner_room);
      CHECK_FALSE(choice.demoted);
      CHECK(p.width_cm % 10 == 0);
      CHECK(p.depth_cm % 10 == 0);
      bool fresh = true;
      for (u32 i = 0; i < distinct; ++i)
        fresh = fresh && widths[i] != static_cast<u32>(p.width_cm);
      if (fresh) widths[distinct++] = static_cast<u32>(p.width_cm);
      if (p.mirror) ++mirrored;
    }
    CHECK(distinct >= 4);
    CHECK(mirrored > 8);
    CHECK(mirrored < 56);
  }
}

TEST_CASE("rural filler: a small plot gets the next plan down, and a smaller one is refused") {
  const RuralStyle style;
  RuralChoice choice;
  std::string error;

  // Seven for a cross-passage house on a plot 7.6 m wide: the plan one room wide.
  REQUIRE(choose_rural(k_narrow_plot, Household{7, 1}, style, 3, choice, &error));
  CHECK(choice.interior.type == PlanType::TwoUpTwoDown);
  CHECK(choice.demoted);
  CHECK(choice.ridge_along_front == false);

  // Four of some means on a plot with no depth for a stair: one floor, if it is long enough.
  REQUIRE(choose_rural(RuralPlot{1200, 520}, Household{4, 2}, style, 3, choice, &error));
  CHECK(choice.interior.type == PlanType::HallParlour);
  CHECK(choice.interior.storeys == 1);
  CHECK(choice.demoted);

  // The same on a plot 6.2 m by 5.2 m: a cot.
  REQUIRE(choose_rural(k_small_plot, Household{4, 2}, style, 3, choice, &error));
  CHECK(choice.interior.type == PlanType::Cot);
  CHECK(choice.interior.hearths == 1);

  CHECK_FALSE(choose_rural(RuralPlot{500, 400}, Household{2, 0}, style, 3, choice, &error));
  CHECK(error.rfind("rural filler: ", 0) == 0);
}

TEST_CASE("rural filler: sizes stay inside the measured ranges") {
  const RuralStyle style;
  for (u64 seed = 0; seed < 200; ++seed) {
    RuralChoice choice;
    std::string error;
    REQUIRE(choose_rural(k_wide_plot, Household{2, 0}, style, seed, choice, &error));
    CHECK(choice.interior.width_cm >= 450);
    CHECK(choice.interior.width_cm <= 570);
    CHECK(choice.interior.depth_cm >= 360);
    CHECK(choice.interior.depth_cm <= 460);
    REQUIRE(choose_rural(k_wide_plot, Household{4, 2}, style, seed, choice, &error));
    CHECK(choice.interior.width_cm >= 740);
    CHECK(choice.interior.width_cm <= 1000);
    CHECK(choice.interior.depth_cm >= 510);
    CHECK(choice.interior.depth_cm <= 550);
    REQUIRE(choose_rural(k_wide_plot, Household{9, 2}, style, seed, choice, &error));
    CHECK(choice.interior.width_cm <= 1500);
    CHECK(choice.interior.depth_cm <= 550);
  }
}

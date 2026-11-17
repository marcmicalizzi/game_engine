// The same plans on every toolchain (docs/subsystems/dwellings.md, "Determinism"): the
// interior of every plan type over seeds, and the rural filler's houses over plots, households and
// seeds, hashed, against pins taken on MSVC that Clang and GCC reproduce in the containers. Integer
// centimetres and the engine's hash only: a pin moves when the layout's rules do, and
// `k_interior_version` or `k_rural_filler_version` moves with it.

#include <core/hash/hash.h>
#include <domain/dwellings/rural_filler.h>

#include <doctest/doctest.h>

#include <cstdio>
#include <string>

using namespace engine;
using namespace engine::dwellings;

namespace {

// Taken on MSVC 19.44 (msvc-debug) on 2026-11-14 at k_interior_version 2 and
// k_rural_filler_version 2. Version 1's were 0x2fd5eb8b995c3bfb and 0x8d2f548386dde551: they moved
// because a plan's hash covers `k_interior_version`, which moved when a stair's well became the
// flight's run (no record's field changed). Re-taken on MSVC (msvc-release) on 2026-11-16 at
// k_interior_version 3, when the well came to run a person's reach past the first riser (version
// 2's were 0xc1e1e57f0ca96d27 and 0x1eabd65ddcdc87b0; again no record's field changed).
constexpr u64 k_plans_pin = 0xe00d70061fa1f1f6ull;
// And at k_rural_filler_version 3 (G208, 2026-11-16; 0x3b275677de2f881a before): a demotion
// passes over every plan with more storeys than the household asked or the plot allows, so a
// household of no means on a narrow plot gets a cot and not a two-up two-down; plans unchanged.
constexpr u64 k_houses_pin = 0xc148887e3eb95cf9ull;

}  // namespace

TEST_CASE("interiors: plans and the filler's houses are the same on every toolchain") {
  u64 plans = k_hash_seed;
  for (u32 t = 0; t < k_plan_types; ++t) {
    for (u8 storeys = 1; storeys <= 2; ++storeys) {
      for (u64 seed = 0; seed < 16; ++seed) {
        InteriorParams p;
        p.type = static_cast<PlanType>(t);
        p.storeys = storeys;
        p.inner_room = (seed & 2) != 0;
        p.mirror = (seed & 1) != 0;
        p.hearths = static_cast<u8>(1 + ((seed >> 2) & 1));
        p.seed = seed * 0x9e3779b97f4a7c15ull + t;
        const InteriorExtent least = interior_min_extent(p);
        p.width_cm = least.min_width_cm + static_cast<i32>(seed) * 13;
        p.depth_cm = least.min_depth_cm + static_cast<i32>(seed) * 3;
        InteriorPlan plan;
        std::string error;
        REQUIRE(plan_interior(p, plan, &error));
        plans = hash_combine(plans, hash_interior(plan));
      }
    }
  }

  u64 houses = k_hash_seed;
  const RuralStyle style;
  const RuralPlot plots[3] = {RuralPlot{2400, 1500}, RuralPlot{760, 1500}, RuralPlot{1200, 520}};
  for (const RuralPlot& plot : plots) {
    for (u8 persons = 1; persons <= 9; ++persons) {
      for (u8 means = 0; means <= 2; ++means) {
        for (u64 seed = 0; seed < 8; ++seed) {
          RuralChoice choice;
          InteriorPlan plan;
          std::string error;
          REQUIRE(plan_rural(plot, Household{persons, means}, style, seed + 1000 * persons, choice,
                             plan, &error));
          houses = hash_combine(houses, hash_interior(plan));
          houses = hash_combine(houses, u64{static_cast<u32>(choice.outer_width_cm)} << 32 |
                                            static_cast<u32>(choice.outer_depth_cm));
          houses = hash_combine(
              houses, u64{choice.ridge_along_front ? 1u : 0u} << 1 | (choice.demoted ? 1u : 0u));
        }
      }
    }
  }

  if (plans != k_plans_pin || houses != k_houses_pin) {
    std::printf("interiors pins: plans 0x%016llxull houses 0x%016llxull\n",
                static_cast<unsigned long long>(plans), static_cast<unsigned long long>(houses));
  }
  CHECK(plans == k_plans_pin);
  CHECK(houses == k_houses_pin);
}

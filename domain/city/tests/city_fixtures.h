#pragma once

// The islands the city tests share (docs/subsystems/city.md, "Testing"): the default island at the
// golden seed, and a small one for the tests that walk every tile or every building. Made once per
// test binary; a plan is a pure function of its parameters, so sharing one is sharing a constant.

#include <domain/city/city.h>

#include <doctest/doctest.h>

#include <string>

namespace engine::city::test {

inline constexpr u64 k_golden_seed = 2026;

inline Params make_params(u64 seed, f32 radius) {
  IslandParams source = default_island_params();
  source.seed = seed;
  source.radius = radius;
  Params p;
  std::string error;
  const bool ok = params_from_schema(source, p, error);
  REQUIRE_MESSAGE(ok, error);
  return p;
}

inline Plan make_plan(const Params& p) {
  Plan plan;
  std::string error;
  const bool ok = generate_plan(p, plan, error);
  REQUIRE_MESSAGE(ok, error);
  return plan;
}

// The default island (radius 2 km) at the golden seed: every district kind, park kind and
// archetype.
inline const Plan& default_plan() {
  static const Plan plan = make_plan(make_params(k_golden_seed, 2000.0f));
  return plan;
}

// A small island (radius 900 m): a few hundred lots, for the tests that visit all of them.
inline const Plan& small_plan() {
  static const Plan plan = make_plan(make_params(k_golden_seed, 900.0f));
  return plan;
}

}  // namespace engine::city::test

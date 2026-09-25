#pragma once

// The validators (docs/subsystems/city.md, "Validators"; plan 13 §13.2 "Agent role", §13.6 E18):
// the review loop of the grammar. Every generated building runs through them, and the fraction that
// passes **without repair** is the yield. A failure is categorized by the rule it broke, never
// repaired, so a report says which rule of the grammar to fix.
//
// The validators read the description and nothing of how it was made: reachability walks the doors,
// daylight looks for windows in exterior walls, fit measures rooms, and so on. So a fixture that
// removes a door or a window from a generated building fails the validator that guards it, which
// is how each one is tested.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/city/building.h>
#include <domain/city/plan.h>

#include <schemas/city.h>
#include <span>

namespace engine::jobs {
class JobSystem;
}

namespace engine::city {

struct Finding {
  Rule rule = Rule::ReachRoom;
  u32 floor = 0;
  u32 subject = 0;
  i64 value = 0;
  i64 limit = 0;
};

struct Report {
  Vector<Finding> findings;
  u32 by_rule[k_rules] = {};
  void clear() noexcept;
  void add(Rule rule, u32 floor, u32 subject, i64 value, i64 limit);
  bool passed() const noexcept { return findings.empty(); }
};

// The validator a rule belongs to: what the yield table's columns are.
enum class Validator : u8 {
  connectivity = 0,
  blocks = 1,
  parks = 2,
  frontage = 3,
  reachability = 4,
  egress = 5,
  daylight = 6,
  fit = 7,
  services = 8,
  structure = 9,
  site = 10,
};
inline constexpr u32 k_validators = 11;
inline constexpr u32 k_first_building_validator = 4;
Validator validator_of(Rule rule) noexcept;
const char* validator_name(Validator validator) noexcept;

// The plan's rules: connectivity and dead-end arterials, block sizes by district, park share and
// walking distance, frontage. Appends to `out`.
void validate_plan(const Plan& plan, Report& out);

// A building's rules, at the `Rooms` stage (a coarser building is validated only on what it has:
// site and structure at `Massing`, egress and services too at `Floors`). Appends to `out`.
void validate_building(const Plan& plan, const Building& building, Report& out);

// ---- E18: the yield ----------------------------------------------------------------------------

// The lots E18 measures: `count` building lots, taken from each archetype in turn in the order the
// island's seed ranks them, so every archetype the plan has is represented about equally.
void choose_yield_lots(const Plan& plan, u32 count, Vector<u32>& out);

struct YieldRow {
  u32 buildings = 0;
  u32 passed = 0;  // no finding at all
  u32 passed_by_validator[k_validators] = {};
  u32 units = 0;
  u32 units_passed = 0;  // no finding on the unit or its rooms
  u32 failures_by_rule[k_rules] = {};
};

struct YieldTable {
  YieldRow all;
  YieldRow by_archetype[k_archetypes];
};

// Generates and validates every lot of `lots` (on the pool when given) and counts. The table is the
// same whatever the thread count.
bool measure_yield(const Plan& plan, std::span<const u32> lots, jobs::JobSystem* jobs,
                   YieldTable& out, std::string* error);

}  // namespace engine::city

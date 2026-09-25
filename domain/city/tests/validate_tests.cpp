// The validators (docs/subsystems/city.md, "Validators"): every rule on a passing fixture (a
// generated building or plan, as the grammar makes it) and on a failing one (the same, with the
// thing the rule guards taken away), and E18's yield — the floor the default island must hold, the
// same table on any thread count, and the parameter sets that break a rule the grammar does not
// adapt to, each caught by the validator that guards it.
#include "city_fixtures.h"

#include <core/jobs/job_system.h>
#include <domain/city/city.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;
using namespace engine::city;
using engine::city::test::default_plan;
using engine::city::test::k_golden_seed;
using engine::city::test::make_params;
using engine::city::test::make_plan;

namespace {

u32 lot_of(const Plan& plan, Archetype a, u32 skip = 0) {
  Vector<u32> lots;
  choose_yield_lots(plan, 7 * (skip + 1), lots);
  for (const u32 l : lots) {
    if (plan.lots[l].archetype != a) continue;
    if (skip-- == 0) return l;
  }
  return k_no_id;
}

Building make(const Plan& plan, u32 lot) {
  Grammar grammar(plan);
  Building b;
  std::string error;
  const bool ok = grammar.generate(lot, Stage::Rooms, b, &error);
  REQUIRE_MESSAGE(ok, error);
  return b;
}

Report check(const Plan& plan, const Building& b) {
  Report r;
  validate_building(plan, b, r);
  return r;
}

// True when the report holds this rule and nothing the fixture did not break.
bool only(const Report& r, std::initializer_list<Rule> rules) {
  if (r.findings.empty()) return false;
  for (const Finding& f : r.findings) {
    bool listed = false;
    for (const Rule rule : rules)
      listed = listed || f.rule == rule;
    if (!listed) return false;
  }
  return true;
}

std::string rules_of(const Report& r) {
  std::string s;
  for (const Finding& f : r.findings)
    s += std::string(rule_name(f.rule)) + " ";
  return s;
}

void drop_openings(Building& b, auto&& pred) {
  Vector<Opening> kept;
  for (const Opening& o : b.openings) {
    if (!pred(o)) kept.push_back(o);
  }
  b.openings = std::move(kept);
}

}  // namespace

TEST_CASE("city validators: every archetype as generated passes every rule") {
  const Plan& plan = default_plan();
  for (u32 a = 0; a < k_archetypes; ++a) {
    const Building b = make(plan, lot_of(plan, static_cast<Archetype>(a)));
    const Report r = check(plan, b);
    CHECK_MESSAGE(r.passed(), std::string(archetype_name(b.archetype)) << ": " << rules_of(r));
  }
}

TEST_CASE("city validators: reachability") {
  const Plan& plan = default_plan();
  Building b = make(plan, lot_of(plan, Archetype::MidRiseOverShops));
  // A room whose only door is taken away: its unit's bedroom.
  u32 bedroom = k_no_id;
  for (u32 s = 0; s < b.spaces.size() && bedroom == k_no_id; ++s) {
    if (b.spaces[s].kind == SpaceKind::Bedroom) bedroom = s;
  }
  REQUIRE(bedroom != k_no_id);
  Building room = b;
  drop_openings(room, [&](const Opening& o) {
    const Wall& w = room.walls[o.wall];
    return o.kind == OpeningKind::Door && (w.a == bedroom || w.b == bedroom);
  });
  const Report r1 = check(plan, room);
  CHECK_MESSAGE(only(r1, {Rule::ReachRoom}), rules_of(r1));
  // A unit whose door onto the corridor is taken away: it and every room in it.
  const Unit& unit = b.units[b.spaces[bedroom].unit];
  Building unit_cut = b;
  drop_openings(unit_cut, [&](const Opening& o) {
    const Wall& w = unit_cut.walls[o.wall];
    const u32 other = w.a == unit.entry ? w.b : (w.b == unit.entry ? w.a : k_no_id);
    return other != k_no_id && other != k_outside && unit_cut.spaces[other].unit == k_no_id;
  });
  const Report r2 = check(plan, unit_cut);
  CHECK_MESSAGE(r2.by_rule[static_cast<u32>(Rule::ReachUnit)] >= 1, rules_of(r2));
  // The street entrances taken away: nothing inside is reachable, the cores first.
  Building shut = b;
  drop_openings(shut, [](const Opening& o) { return o.kind == OpeningKind::Entrance; });
  const Report r3 = check(plan, shut);
  CHECK(r3.by_rule[static_cast<u32>(Rule::ReachCore)] > 0);
  CHECK(r3.by_rule[static_cast<u32>(Rule::ReachUnit)] > 0);
  CHECK(r3.by_rule[static_cast<u32>(Rule::SiteFrontage)] == 1);
}

TEST_CASE("city validators: egress") {
  const Plan& plan = default_plan();
  const Building b = make(plan, lot_of(plan, Archetype::ApartmentTower));
  REQUIRE(b.floors.size() >= plan.params.rules.two_stair_floors);
  // One stair where two are needed.
  Building one = b;
  for (Core& c : one.cores)
    c.stair = c.main;
  const Report r1 = check(plan, one);
  CHECK(r1.by_rule[static_cast<u32>(Rule::EgressStairs)] == 1);
  // Two stairs side by side.
  Building close = b;
  REQUIRE(close.cores.size() >= 2);
  close.cores[1].rect = close.cores[0].rect;
  const Report r2 = check(plan, close);
  CHECK(r2.by_rule[static_cast<u32>(Rule::EgressSeparation)] == 1);
  // A travel distance the corridor cannot meet: the same building under a stricter rule.
  Plan strict = plan;
  strict.params.rules.max_travel_cm = 100;
  const Report r3 = check(strict, b);
  CHECK_MESSAGE(only(r3, {Rule::EgressTravel}), rules_of(r3));
}

TEST_CASE("city validators: daylight") {
  const Plan& plan = default_plan();
  Building b = make(plan, lot_of(plan, Archetype::Office));
  drop_openings(b, [](const Opening& o) { return o.kind == OpeningKind::Window; });
  const Report r = check(plan, b);
  CHECK_MESSAGE(only(r, {Rule::Daylight}), rules_of(r));
  // Every habitable room, and nothing else.
  u32 habitable = 0;
  for (const Space& s : b.spaces)
    habitable += (s.flags & k_space_habitable) != 0 ? 1u : 0u;
  CHECK(r.by_rule[static_cast<u32>(Rule::Daylight)] == habitable);
}

TEST_CASE("city validators: fit") {
  const Plan& plan = default_plan();
  Building b = make(plan, lot_of(plan, Archetype::DetachedHouse));
  u32 bedroom = k_no_id;
  for (u32 s = 0; s < b.spaces.size() && bedroom == k_no_id; ++s) {
    if (b.spaces[s].kind == SpaceKind::Bedroom) bedroom = s;
  }
  REQUIRE(bedroom != k_no_id);
  Building small = b;
  Rect& r = small.spaces[bedroom].rect;
  r.x1 = r.x0 + 180;
  r.z1 = r.z0 + 180;
  const Report r1 = check(plan, small);
  CHECK(r1.by_rule[static_cast<u32>(Rule::FitRoom)] >= 1);
  CHECK(r1.by_rule[static_cast<u32>(Rule::FitBed)] == 1);
  // A corridor narrower than its building's.
  Building bar = make(plan, lot_of(plan, Archetype::Office));
  Building narrow = bar;
  for (Space& s : narrow.spaces) {
    if (s.kind == SpaceKind::Corridor) s.rect.z1 = s.rect.z0 + 90;
  }
  const Report r2 = check(plan, narrow);
  CHECK(r2.by_rule[static_cast<u32>(Rule::FitCorridor)] == narrow.floors.size());
}

TEST_CASE("city validators: services") {
  const Plan& plan = default_plan();
  const Building b = make(plan, lot_of(plan, Archetype::ApartmentTower));
  REQUIRE(b.floors.size() >= plan.params.rules.elevator_floors);
  Building no_cars = b;
  for (Core& c : no_cars.cores)
    c.elevators = 0;
  const Report r1 = check(plan, no_cars);
  CHECK_MESSAGE(only(r1, {Rule::ServicesElevators}), rules_of(r1));
  Building no_shaft = b;
  for (Core& c : no_shaft.cores)
    c.shaft = 0;
  const Report r2 = check(plan, no_shaft);
  CHECK_MESSAGE(only(r2, {Rule::ServicesShaft}), rules_of(r2));
}

TEST_CASE("city validators: structure") {
  const Plan& plan = default_plan();
  Building b = make(plan, lot_of(plan, Archetype::Warehouse));
  CHECK(check(plan, b).passed());
  b.bay_z_cm = plan.params.rules.max_span_hall_cm + 100;
  const Report r = check(plan, b);
  CHECK_MESSAGE(only(r, {Rule::StructureSpan}), rules_of(r));
}

TEST_CASE("city validators: site") {
  const Plan& plan = default_plan();
  const Building b = make(plan, lot_of(plan, Archetype::DetachedHouse));
  // Onto the street: the front setback.
  Building forward = b;
  forward.footprint.z0 -= 200;
  const Report r1 = check(plan, forward);
  CHECK(r1.by_rule[static_cast<u32>(Rule::SiteSetback)] == 1);
  // Over the whole lot: coverage (and every setback with it).
  Building sprawl = b;
  sprawl.footprint = sprawl.lot_rect;
  const Report r2 = check(plan, sprawl);
  CHECK(r2.by_rule[static_cast<u32>(Rule::SiteCoverage)] == 1);
  // Taller than the district allows.
  Plan low = plan;
  for (Style& s : low.params.styles)
    s.height_max_cm = 300;
  const Report r3 = check(low, b);
  CHECK_MESSAGE(only(r3, {Rule::SiteHeight}), rules_of(r3));
  // No entrance on the street's side.
  Building blind = b;
  drop_openings(blind, [](const Opening& o) { return o.kind == OpeningKind::Entrance; });
  const Report r4 = check(plan, blind);
  CHECK(r4.by_rule[static_cast<u32>(Rule::SiteFrontage)] == 1);
}

TEST_CASE("city validators: the plan's rules on failing fixtures") {
  const Plan& good = default_plan();
  {
    Report r;
    validate_plan(good, r);
    CHECK(r.passed());
  }
  // A street off on its own: two components, and an arterial with nothing at either end.
  {
    Plan plan = good;
    Street s;
    s.id = 0xfffffff0u;
    s.cls = StreetClass::Arterial;
    s.width_cm = 3200;
    plan.streets.push_back(s);
    plan.nodes.push_back(Node{90000000, 90000000});
    plan.nodes.push_back(Node{90010000, 90000000});
    plan.segments.push_back(Segment{plan.streets.size() - 1,
                                    plan.nodes.size() - 2,
                                    plan.nodes.size() - 1,
                                    StreetClass::Arterial,
                                    {0, 0, 0}});
    Report r;
    validate_plan(plan, r);
    CHECK(r.by_rule[static_cast<u32>(Rule::PlanConnectivity)] == 2);
    CHECK(r.by_rule[static_cast<u32>(Rule::PlanDeadEndArterial)] == 1);
  }
  // A block cut down below its district's smallest.
  {
    Plan plan = good;
    for (Block& b : plan.blocks) {
      if (b.park == ParkKind::None) {
        b.rect.x1 = b.rect.x0 + 1000;
        break;
      }
    }
    Report r;
    validate_plan(plan, r);
    CHECK_MESSAGE(only(r, {Rule::PlanBlockSize}), rules_of(r));
  }
  // No parks at all: every district short of its share, every dwelling out of reach.
  {
    Plan plan = good;
    plan.parks.clear();
    Report r;
    validate_plan(plan, r);
    CHECK(r.by_rule[static_cast<u32>(Rule::PlanParkShare)] > 0);
    CHECK(r.by_rule[static_cast<u32>(Rule::PlanParkDistance)] > 0);
    CHECK_MESSAGE(only(r, {Rule::PlanParkShare, Rule::PlanParkDistance}), rules_of(r));
  }
  // A lot that fronts nothing, and one on an alley.
  {
    Plan plan = good;
    u32 at = k_no_id;
    for (u32 l = 0; l < plan.lots.size() && at == k_no_id; ++l) {
      if (plan.lots[l].use != LotUse::Park) at = l;
    }
    REQUIRE(at != k_no_id);
    plan.lots[at].street = k_no_id;
    Report r;
    validate_plan(plan, r);
    CHECK_MESSAGE(only(r, {Rule::PlanFrontage}), rules_of(r));
    plan.lots[at].street = good.lots[at].street;
    plan.streets[plan.lots[at].street].cls = StreetClass::Alley;
    Report r2;
    validate_plan(plan, r2);
    CHECK(r2.by_rule[static_cast<u32>(Rule::PlanFrontage)] >= 1);
  }
}

// E18's floor. The default island's 200 buildings across the archetypes all pass: the floor is
// 1.0 because the default rules are the ones the grammar is built to satisfy (every rule a
// validator checks is one the grammar also knows), and all 255,757 buildings of 21 islands (seven
// seeds at three sizes) passed when this was written. So a failure here is not noise: it is a
// change to the grammar that broke one of its own rules, and the failure names the rule.
TEST_CASE("city yield: the default island's 200 E18 buildings all pass, on any thread count") {
  const Plan& plan = default_plan();
  Vector<u32> lots;
  choose_yield_lots(plan, 200, lots);
  REQUIRE(lots.size() == 200);
  u32 per[k_archetypes] = {};
  for (const u32 l : lots)
    ++per[static_cast<u32>(plan.lots[l].archetype)];
  for (u32 a = 0; a < k_archetypes; ++a)
    CHECK(per[a] >= 200 / k_archetypes - 1);  // every archetype about equally
  YieldTable one;
  std::string error;
  REQUIRE_MESSAGE(
      measure_yield(plan, std::span<const u32>(lots.data(), lots.size()), nullptr, one, &error),
      error);
  std::string failed;
  for (u32 k = 0; k < k_rules; ++k) {
    if (one.all.failures_by_rule[k] != 0)
      failed += std::string(rule_name(static_cast<Rule>(k))) + " " +
                std::to_string(one.all.failures_by_rule[k]) + "; ";
  }
  CHECK_MESSAGE(one.all.passed == one.all.buildings, failed);
  CHECK(one.all.units_passed == one.all.units);
  MESSAGE("E18: " << one.all.passed << " of " << one.all.buildings << " buildings, "
                  << one.all.units_passed << " of " << one.all.units << " units");
  jobs::JobSystem js(jobs::JobSystemConfig{
      .performance_workers = 3, .efficiency_workers = 1, .pin_threads = false});
  YieldTable pooled;
  REQUIRE(measure_yield(plan, std::span<const u32>(lots.data(), lots.size()), &js, pooled, &error));
  CHECK(pooled.all.passed == one.all.passed);
  CHECK(pooled.all.units == one.all.units);
  for (u32 a = 0; a < k_archetypes; ++a)
    CHECK(pooled.by_archetype[a].units_passed == one.by_archetype[a].units_passed);
}

// The review loop's other half: parameters the grammar does not read when it decides are caught by
// the validators that read them, and the table says which rule. Each set changes one limit.
TEST_CASE("city yield: a limit the grammar does not adapt to is caught by its validator") {
  struct Case {
    const char* what;
    void (*change)(IslandParams&);
    Rule rule;
  };
  static const Case k_cases[] = {
      // The bars' unit depth comes from unit_depth_max, not from the span: a shorter span fails
      // the bars' bands.
      {"max_span 7 m", [](IslandParams& p) { p.building.max_span = 7.0f; }, Rule::StructureSpan},
      // An accessible bedroom's 1.5 m turning space beside the bed: the bars' bedrooms are cut to
      // the minimum width and never read the clearance.
      {"bed clearance 1.5 m", [](IslandParams& p) { p.building.bed_clearance = 1.5f; },
       Rule::FitBed},
  };
  for (const Case& c : k_cases) {
    IslandParams source = default_island_params();
    source.seed = k_golden_seed;
    source.radius = 1200.0f;
    c.change(source);
    Params p;
    std::string error;
    REQUIRE_MESSAGE(params_from_schema(source, p, error), error);
    const Plan plan = make_plan(p);
    Vector<u32> lots;
    choose_yield_lots(plan, 140, lots);
    YieldTable t;
    REQUIRE(
        measure_yield(plan, std::span<const u32>(lots.data(), lots.size()), nullptr, t, &error));
    const std::string what = c.what;
    CHECK_MESSAGE(t.all.passed < t.all.buildings, what);
    CHECK_MESSAGE(t.all.failures_by_rule[static_cast<u32>(c.rule)] > 0, what);
    MESSAGE(what << ": " << t.all.passed << " of " << t.all.buildings << " pass, "
                 << t.all.failures_by_rule[static_cast<u32>(c.rule)] << " "
                 << std::string(rule_name(c.rule)));
  }
}

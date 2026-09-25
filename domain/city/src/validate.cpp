// The validators (docs/subsystems/city.md, "Validators"): what each rule of the plan and of a
// building guards, measured on the description alone, categorized and never repaired.
#include "grid.h"

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <domain/city/validate.h>

#include <algorithm>
#include <cstdio>

namespace engine::city {

namespace {

constexpr u64 k_purpose_yield = 0x1e1d;

constexpr Validator k_validator_of[k_rules] = {
    Validator::connectivity, Validator::connectivity, Validator::blocks,
    Validator::parks,        Validator::parks,        Validator::frontage,
    Validator::reachability, Validator::reachability, Validator::reachability,
    Validator::egress,       Validator::egress,       Validator::egress,
    Validator::daylight,     Validator::fit,          Validator::fit,
    Validator::fit,          Validator::services,     Validator::services,
    Validator::structure,    Validator::site,         Validator::site,
    Validator::site,         Validator::site};

constexpr const char* k_validator_names[k_validators] = {
    "connectivity", "blocks", "parks",    "frontage",  "reachability", "egress",
    "daylight",     "fit",    "services", "structure", "site"};

bool is_bar(Archetype a) noexcept {
  return a == Archetype::ApartmentTower || a == Archetype::MidRiseOverShops ||
         a == Archetype::Office || a == Archetype::CivicShell;
}

bool is_dwelling(Archetype a) noexcept {
  return a == Archetype::ApartmentTower || a == Archetype::MidRiseOverShops ||
         a == Archetype::TerracedHouse || a == Archetype::DetachedHouse;
}

bool is_vehicle(StreetClass c) noexcept {
  return c == StreetClass::Arterial || c == StreetClass::Collector || c == StreetClass::Local;
}

u32 root(Vector<u32>& parent, u32 x) noexcept {
  while (parent[x] != x) {
    parent[x] = parent[parent[x]];
    x = parent[x];
  }
  return x;
}

// Rules whose subject is a space, and whose subject is a unit.
bool space_rule(Rule r) noexcept {
  return r == Rule::ReachRoom || r == Rule::ReachCore || r == Rule::Daylight ||
         r == Rule::FitRoom || r == Rule::FitBed || r == Rule::FitCorridor;
}
bool unit_rule(Rule r) noexcept { return r == Rule::ReachUnit || r == Rule::EgressTravel; }

}  // namespace

Validator validator_of(Rule rule) noexcept {
  const u32 i = static_cast<u32>(rule);
  return i < k_rules ? k_validator_of[i] : Validator::site;
}

const char* validator_name(Validator validator) noexcept {
  const u32 i = static_cast<u32>(validator);
  return i < k_validators ? k_validator_names[i] : "unknown";
}

void Report::clear() noexcept {
  findings.clear();
  for (u32& n : by_rule)
    n = 0;
}

void Report::add(Rule rule, u32 floor, u32 subject, i64 value, i64 limit) {
  findings.push_back(Finding{rule, floor, subject, value, limit});
  ++by_rule[static_cast<u32>(rule)];
}

// ---- the plan -------------------------------------------------------------------------------

void validate_plan(const Plan& plan, Report& out) {
  const Params& p = plan.params;
  const u32 nn = plan.nodes.size();
  // Connectivity: every street one component, and the vehicle streets one component.
  for (u32 pass = 0; pass < 2; ++pass) {
    Vector<u32> parent(nn);
    Vector<u8> touched(nn, u8{0});
    for (u32 i = 0; i < nn; ++i)
      parent[i] = i;
    for (const Segment& s : plan.segments) {
      if (pass == 1 && !is_vehicle(plan.streets[s.street].cls)) continue;
      const u32 a = root(parent, s.a);
      const u32 b = root(parent, s.b);
      if (a != b) parent[std::max(a, b)] = std::min(a, b);
      touched[s.a] = touched[s.b] = 1;
    }
    u32 components = 0;
    for (u32 i = 0; i < nn; ++i)
      components += touched[i] != 0 && root(parent, i) == i ? 1u : 0u;
    if (components > 1) out.add(Rule::PlanConnectivity, 0, pass, components, 1);
  }
  // Dead-end arterials: an arterial's end where no other vehicle street continues.
  Vector<u32> vehicle(nn, 0u);
  for (const Segment& s : plan.segments) {
    if (!is_vehicle(plan.streets[s.street].cls)) continue;
    ++vehicle[s.a];
    ++vehicle[s.b];
  }
  for (const Segment& s : plan.segments) {
    if (plan.streets[s.street].cls != StreetClass::Arterial) continue;
    if (vehicle[s.a] == 1 || vehicle[s.b] == 1)
      out.add(Rule::PlanDeadEndArterial, 0, plan.streets[s.street].id, 1, 2);
  }
  // Block sizes, by the district's style: every block but the whole-superblock parks.
  for (const Block& b : plan.blocks) {
    if (b.park == ParkKind::Central || b.park == ParkKind::Greenbelt) continue;
    const Style& style = p.styles[static_cast<u32>(plan.districts[b.district].kind)];
    for (const i32 side : {b.rect.width(), b.rect.depth()}) {
      if (side < style.block_min_cm) {
        out.add(Rule::PlanBlockSize, 0, b.id, side, style.block_min_cm);
        break;
      }
      if (side > style.block_max_cm) {
        out.add(Rule::PlanBlockSize, 0, b.id, side, style.block_max_cm);
        break;
      }
    }
  }
  // Park share, district by district.
  {
    Vector<i64> parks(plan.districts.size(), i64{0});
    for (const Park& park : plan.parks) {
      const i64 area = park.lot == k_no_id ? plan.blocks[park.block].rect.area() : park.rect.area();
      parks[park.district] += area;
    }
    for (u32 k = 0; k < plan.districts.size(); ++k) {
      const District& d = plan.districts[k];
      const Style& style = p.styles[static_cast<u32>(d.kind)];
      const i64 want = grid::mul_q(d.area_cm2, style.park_share_q);
      if (d.area_cm2 > 0 && parks[k] < want) out.add(Rule::PlanParkShare, 0, d.id, parks[k], want);
    }
  }
  // Park distance, and frontage, lot by lot.
  const i64 reach2 = i64{p.walk_distance_cm} * p.walk_distance_cm;
  for (const Lot& lot : plan.lots) {
    if (lot.use == LotUse::Park) continue;
    const i64 cx = (i64{lot.rect.x0} + lot.rect.x1) / 2;
    const i64 cz = (i64{lot.rect.z0} + lot.rect.z1) / 2;
    if (lot.use == LotUse::Building && is_dwelling(lot.archetype)) {
      i64 best = -1;
      for (const Park& park : plan.parks) {
        const i64 d2 = grid::dist2_to_rect(cx, cz, park.rect);
        if (best < 0 || d2 < best) best = d2;
      }
      if (best < 0 || best > reach2) out.add(Rule::PlanParkDistance, 0, lot.id, best, reach2);
    }
    if (lot.street == k_no_id || lot.street >= plan.streets.size()) {
      out.add(Rule::PlanFrontage, 0, lot.id, 0, 1);
      continue;
    }
    const Street& s = plan.streets[lot.street];
    const bool along_x = lot.front == 1 || lot.front == 3;
    const i32 edge = lot.front == 0   ? lot.rect.x1
                     : lot.front == 1 ? lot.rect.z0
                     : lot.front == 2 ? lot.rect.x0
                                      : lot.rect.z1;
    const i32 a0 = along_x ? lot.rect.x0 : lot.rect.z0;
    const i32 a1 = along_x ? lot.rect.x1 : lot.rect.z1;
    const i32 gap = edge > s.line_cm ? edge - s.line_cm : s.line_cm - edge;
    const bool on_street =
        (s.along_x != 0) == along_x && gap <= s.width_cm / 2 + 1 && a1 > s.from_cm && a0 < s.to_cm;
    if (!on_street || s.cls == StreetClass::Alley)
      out.add(Rule::PlanFrontage, 0, lot.id, gap, s.width_cm / 2);
  }
}

// ---- a building -----------------------------------------------------------------------------

void validate_building(const Plan& plan, const Building& b, Report& out) {
  const Params& p = plan.params;
  const Rules& r = p.rules;
  const Lot& lot = plan.lots[b.lot];
  const Style& style = p.styles[static_cast<u32>(plan.districts[lot.district].kind)];
  const u32 n = b.floors.size();

  // Site: setbacks, coverage, height.
  {
    const i32 side = b.archetype == Archetype::TerracedHouse ? 0 : style.setback_side_cm;
    const Rect& f = b.footprint;
    const Rect& l = b.lot_rect;
    const i32 gaps[4] = {f.z0 - l.z0 - style.setback_front_cm, l.z1 - f.z1 - style.setback_rear_cm,
                         f.x0 - l.x0 - side, l.x1 - f.x1 - side};
    for (u32 k = 0; k < 4; ++k) {
      if (gaps[k] < 0) {
        out.add(Rule::SiteSetback, 0, k, gaps[k], 0);
        break;
      }
    }
    const i64 cover = grid::mul_q(l.area(), style.coverage_q);
    if (f.area() > cover) out.add(Rule::SiteCoverage, 0, 0, f.area(), cover);
    if (b.height_cm() > style.height_max_cm)
      out.add(Rule::SiteHeight, 0, 0, b.height_cm(), style.height_max_cm);
  }
  // Structure: the grid's bays against the structure's span.
  {
    i32 limit = r.max_span_cm;
    if (b.archetype == Archetype::TerracedHouse || b.archetype == Archetype::DetachedHouse)
      limit = r.max_span_house_cm;
    if (b.archetype == Archetype::Warehouse) limit = r.max_span_hall_cm;
    const i32 span = std::max(b.bay_x_cm, b.bay_z_cm);
    if (span > limit) out.add(Rule::StructureSpan, 0, 0, span, limit);
  }
  if (b.stage == Stage::Massing) return;

  // Egress and services: the bars (houses and the warehouse stay under every threshold).
  if (is_bar(b.archetype)) {
    u32 stairs = 0, elevators = 0;
    bool shaft = false;
    for (const Core& c : b.cores) {
      stairs += c.stair;
      elevators += c.elevators;
      shaft = shaft || (c.shaft != 0 && c.floor_from == 0 && c.floor_to + 1u == n);
    }
    if (n >= r.two_stair_floors && stairs < 2) out.add(Rule::EgressStairs, 0, 0, stairs, 2);
    if (stairs >= 2) {
      i64 far2 = 0;
      for (const Core& c : b.cores) {
        for (const Core& d : b.cores) {
          if (c.stair == 0 || d.stair == 0) continue;
          const i64 dx = (i64{c.rect.x0} + c.rect.x1 - d.rect.x0 - d.rect.x1) / 2;
          const i64 dz = (i64{c.rect.z0} + c.rect.z1 - d.rect.z0 - d.rect.z1) / 2;
          far2 = std::max(far2, dx * dx + dz * dz);
        }
      }
      const i64 w = b.footprint.width();
      const i64 d = b.footprint.depth();
      const i64 q = r.stair_separation_q;
      // far >= q * diagonal, squared and scaled by 2^32 to stay in integers: a footprint of a few
      // hundred metres keeps both sides below 2^63.
      if (far2 * 65536 * 65536 < q * q * (w * w + d * d))
        out.add(Rule::EgressSeparation, 0, 0, far2, (w * w + d * d) * q / 65536 * q / 65536);
    }
    u64 dwellings_above = 0, work_above = 0;
    for (u32 ui = 0; ui < b.units.size(); ++ui) {
      const Unit& u = b.units[ui];
      if (u.floor == 0) continue;
      if (u.kind <= UnitKind::House) {
        ++dwellings_above;
      } else {
        work_above += u.workplaces;
      }
      // The travel from its entry to the nearest stair, along the corridor.
      const i32 ux = (u.rect.x0 + u.rect.x1) / 2;
      i64 best = -1;
      for (const Core& c : b.cores) {
        if (c.stair == 0) continue;
        const i64 dx = ux - (c.rect.x0 + c.rect.x1) / 2;
        const i64 dist = (dx < 0 ? -dx : dx) + b.corridor_cm / 2;
        if (best < 0 || dist < best) best = dist;
      }
      if (best < 0 || best > r.max_travel_cm)
        out.add(Rule::EgressTravel, u.floor, ui, best, r.max_travel_cm);
    }
    if (n >= r.elevator_floors) {
      const u64 need = std::max<u64>(
          1, std::max((dwellings_above + r.units_per_elevator - 1) / r.units_per_elevator,
                      (work_above + r.workplaces_per_elevator - 1) / r.workplaces_per_elevator));
      if (elevators < need)
        out.add(Rule::ServicesElevators, 0, 0, elevators, static_cast<i64>(need));
    }
    if (n >= 2 && !shaft) out.add(Rule::ServicesShaft, 0, 0, 0, 1);
  }
  if (b.stage != Stage::Rooms) return;

  // Reachability: from the street through the doors and up the stairs and elevators.
  const u32 ns = b.spaces.size();
  const u32 outside = ns;
  Vector<u32> start(ns + 2, 0u);
  auto for_each_door = [&](auto&& fn) {
    for (const Opening& o : b.openings) {
      if (o.kind == OpeningKind::Window) continue;
      const Wall& w = b.walls[o.wall];
      fn(w.a == k_outside ? outside : w.a, w.b == k_outside ? outside : w.b);
    }
    for (const Link& l : b.links)
      fn(l.below, l.above);
  };
  for_each_door([&](u32 a, u32 c) {
    ++start[a + 1];
    ++start[c + 1];
  });
  for (u32 i = 0; i <= ns; ++i)
    start[i + 1] += start[i];
  Vector<u32> adj(start[ns + 1], 0u);
  Vector<u32> fill(start.begin(), start.end() - 1);
  for_each_door([&](u32 a, u32 c) {
    adj[fill[a]++] = c;
    adj[fill[c]++] = a;
  });
  auto walk = [&](u32 from, Vector<u8>& seen, u32 unit) {
    Vector<u32> stack;
    stack.push_back(from);
    seen[from] = 1;
    while (!stack.empty()) {
      const u32 x = stack.back();
      stack.pop_back();
      for (u32 e = start[x]; e < start[x + 1]; ++e) {
        const u32 y = adj[e];
        if (seen[y] != 0) continue;
        if (unit != k_no_id && (y == outside || b.spaces[y].unit != unit)) continue;
        seen[y] = 1;
        stack.push_back(y);
      }
    }
  };
  Vector<u8> from_street(ns + 1, u8{0});
  walk(outside, from_street, k_no_id);
  for (u32 s = 0; s < ns; ++s) {
    const Space& sp = b.spaces[s];
    if (sp.unit != k_no_id || sp.kind == SpaceKind::Shaft) continue;
    if (from_street[s] == 0) out.add(Rule::ReachCore, sp.floor, s, 0, 1);
  }
  Vector<u8> inside(ns + 1, u8{0});
  for (u32 ui = 0; ui < b.units.size(); ++ui) {
    const Unit& u = b.units[ui];
    if (u.space_count == 0) continue;
    if (from_street[u.entry] == 0) out.add(Rule::ReachUnit, u.floor, ui, 0, 1);
    for (u32 s = u.first_space; s < u.first_space + u.space_count; ++s)
      inside[s] = 0;
    walk(u.entry, inside, ui);
    for (u32 s = u.first_space; s < u.first_space + u.space_count; ++s) {
      if (inside[s] == 0) out.add(Rule::ReachRoom, b.spaces[s].floor, s, 0, 1);
    }
  }

  // Daylight: a window in an exterior wall of every habitable room.
  Vector<u8> lit(ns, u8{0});
  for (const Opening& o : b.openings) {
    if (o.kind != OpeningKind::Window) continue;
    const Wall& w = b.walls[o.wall];
    if (w.a != k_outside && w.b == k_outside) lit[w.a] = 1;
    if (w.b != k_outside && w.a == k_outside) lit[w.b] = 1;
  }
  for (u32 s = 0; s < ns; ++s) {
    if ((b.spaces[s].flags & k_space_habitable) != 0 && lit[s] == 0)
      out.add(Rule::Daylight, b.spaces[s].floor, s, 0, 1);
  }

  // Fit: each semantic's minimum, a bed in every bedroom, the corridors' width.
  for (u32 s = 0; s < ns; ++s) {
    const Space& sp = b.spaces[s];
    const i32 small = std::min(sp.rect.width(), sp.rect.depth());
    const i32 large = std::max(sp.rect.width(), sp.rect.depth());
    const u32 k = static_cast<u32>(sp.kind);
    if (small < r.min_width_cm[k] || large < r.min_depth_cm[k])
      out.add(Rule::FitRoom, sp.floor, s, small, r.min_width_cm[k]);
    if (sp.kind == SpaceKind::Bedroom) {
      const i32 bw = r.bed_width_cm + r.bed_clearance_cm;
      const i32 bl = r.bed_length_cm + r.bed_clearance_cm;
      if (!(small >= bw && large >= bl)) out.add(Rule::FitBed, sp.floor, s, small, bw);
    }
    if (sp.kind == SpaceKind::Corridor && small < b.corridor_cm)
      out.add(Rule::FitCorridor, sp.floor, s, small, b.corridor_cm);
  }

  // Frontage: an entrance on the street's facade.
  bool entrance = false;
  for (const Opening& o : b.openings) {
    if (o.kind != OpeningKind::Entrance) continue;
    const Wall& w = b.walls[o.wall];
    entrance = entrance || (w.z0 == w.z1 && w.z0 == b.footprint.z0);
  }
  if (!entrance) out.add(Rule::SiteFrontage, 0, 0, 0, 1);
}

// ---- E18 --------------------------------------------------------------------------------------

void choose_yield_lots(const Plan& plan, u32 count, Vector<u32>& out) {
  out.clear();
  Vector<std::pair<u64, u32>> groups[k_archetypes];
  for (u32 l = 0; l < plan.lots.size(); ++l) {
    const Lot& lot = plan.lots[l];
    if (lot.use == LotUse::Park) continue;
    groups[static_cast<u32>(lot.archetype)].push_back(
        {grid::draw(plan.params.seed, k_purpose_yield, lot.id), l});
  }
  for (auto& g : groups)
    std::sort(g.begin(), g.end());
  u32 next[k_archetypes] = {};
  bool any = true;
  while (out.size() < count && any) {
    any = false;
    for (u32 a = 0; a < k_archetypes && out.size() < count; ++a) {
      if (next[a] >= groups[a].size()) continue;
      out.push_back(groups[a][next[a]++].second);
      any = true;
    }
  }
}

bool measure_yield(const Plan& plan, std::span<const u32> lots, jobs::JobSystem* jobs,
                   YieldTable& out, std::string* error) {
  out = YieldTable{};
  const u32 count = static_cast<u32>(lots.size());
  struct Result {
    YieldRow row;
    Archetype archetype = Archetype::MidRiseOverShops;
    u8 ok = 1;
    std::string error;
  };
  Vector<Result> results(count);
  auto one = [&](u32 i, Building& b, Report& report, Grammar& grammar) {
    Result& res = results[i];
    if (!grammar.generate(lots[i], Stage::Rooms, b, &res.error)) {
      res.ok = 0;
      return;
    }
    report.clear();
    validate_building(plan, b, report);
    res.archetype = b.archetype;
    YieldRow& row = res.row;
    row.buildings = 1;
    row.passed = report.passed() ? 1 : 0;
    bool validator_ok[k_validators];
    for (bool& v : validator_ok)
      v = true;
    bool building_level = false;
    Vector<u8> bad_unit(b.units.size(), u8{0});
    for (const Finding& f : report.findings) {
      ++row.failures_by_rule[static_cast<u32>(f.rule)];
      validator_ok[static_cast<u32>(validator_of(f.rule))] = false;
      if (unit_rule(f.rule)) {
        if (f.subject < bad_unit.size()) bad_unit[f.subject] = 1;
      } else if (space_rule(f.rule)) {
        const u32 u = f.subject < b.spaces.size() ? b.spaces[f.subject].unit : k_no_id;
        if (u != k_no_id) {
          bad_unit[u] = 1;
        } else {
          building_level = true;
        }
      } else {
        building_level = true;
      }
    }
    for (u32 v = 0; v < k_validators; ++v)
      row.passed_by_validator[v] = validator_ok[v] ? 1 : 0;
    row.units = b.units.size();
    for (u32 u = 0; u < b.units.size(); ++u)
      row.units_passed += bad_unit[u] == 0 && !building_level ? 1u : 0u;
  };
  auto run = [&](u32 begin, u32 end) {
    Grammar grammar(plan);
    Building b;
    Report report;
    for (u32 i = begin; i < end; ++i)
      one(i, b, report, grammar);
  };
  if (jobs != nullptr && count > 1 && jobs->worker_count(jobs::Pool::Performance) > 0) {
    jobs->parallel_for(jobs::Pool::Performance, count, 1,
                       [&](u32 begin, u32 end) { run(begin, end); });
  } else {
    run(0, count);
  }
  auto add = [](YieldRow& into, const YieldRow& row) {
    into.buildings += row.buildings;
    into.passed += row.passed;
    into.units += row.units;
    into.units_passed += row.units_passed;
    for (u32 v = 0; v < k_validators; ++v)
      into.passed_by_validator[v] += row.passed_by_validator[v];
    for (u32 k = 0; k < k_rules; ++k)
      into.failures_by_rule[k] += row.failures_by_rule[k];
  };
  for (const Result& res : results) {
    if (res.ok == 0) {
      if (error != nullptr) *error = res.error;
      return false;
    }
    add(out.all, res.row);
    add(out.by_archetype[static_cast<u32>(res.archetype)], res.row);
  }
  return true;
}

// ---- the building's file ----------------------------------------------------------------------

void building_to_schema(const Plan& plan, const Building& b, const Report* report,
                        BuildingFile& out) {
  out = BuildingFile{};
  out.format = "engine.city-building.v1";
  out.generator = k_generator_version;
  char key[17];
  std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(plan.key));
  out.plan_key = key;
  out.lot = b.lot_id;
  out.archetype = b.archetype;
  out.civic = b.civic;
  char seed[17];
  std::snprintf(seed, sizeof(seed), "%016llx", static_cast<unsigned long long>(b.seed));
  out.seed = seed;
  out.stage = b.stage;
  out.origin = {b.origin_x, b.origin_z};
  out.front = b.front;
  out.footprint = {b.footprint.x0, b.footprint.z0, b.footprint.x1, b.footprint.z1};
  out.bay = {b.bay_x_cm, b.bay_z_cm};
  for (u32 f = 0; f < b.floors.size(); ++f) {
    BuildingFloor r;
    r.index = f;
    r.elevation = b.floors[f].elevation_cm;
    r.height = b.floors[f].height_cm;
    out.floors.push_back(r);
  }
  for (const Core& c : b.cores) {
    BuildingCore r;
    r.rect = {c.rect.x0, c.rect.z0, c.rect.x1, c.rect.z1};
    r.stair = c.stair != 0;
    r.elevators = c.elevators;
    r.shaft = c.shaft != 0;
    r.floor_from = c.floor_from;
    r.floor_to = c.floor_to;
    out.cores.push_back(r);
  }
  for (u32 s = 0; s < b.spaces.size(); ++s) {
    const Space& sp = b.spaces[s];
    BuildingSpace r;
    r.id = s;
    r.floor = sp.floor;
    r.unit = sp.unit == k_no_id ? -1 : static_cast<i32>(sp.unit);
    r.kind = sp.kind;
    r.rect = {sp.rect.x0, sp.rect.z0, sp.rect.x1, sp.rect.z1};
    out.spaces.push_back(r);
  }
  for (u32 ui = 0; ui < b.units.size(); ++ui) {
    const Unit& u = b.units[ui];
    BuildingUnit r;
    r.id = ui;
    r.floor = u.floor;
    r.kind = u.kind;
    r.rect = {u.rect.x0, u.rect.z0, u.rect.x1, u.rect.z1};
    r.entry = u.entry;
    r.bedrooms = u.bedrooms;
    r.residents = u.residents;
    r.workplaces = u.workplaces;
    r.work = u.work;
    out.units.push_back(r);
  }
  for (const Wall& w : b.walls) {
    BuildingWall r;
    r.floor = w.floor;
    r.from = {w.x0, w.z0};
    r.to = {w.x1, w.z1};
    r.thickness = w.thickness_cm;
    r.kind = w.kind;
    r.a = w.a == k_outside ? -1 : static_cast<i32>(w.a);
    r.b = w.b == k_outside ? -1 : static_cast<i32>(w.b);
    out.walls.push_back(r);
  }
  for (const Opening& o : b.openings) {
    BuildingOpening r;
    r.wall = o.wall;
    r.kind = o.kind;
    r.at = o.at_cm;
    r.width = o.width_cm;
    r.sill = o.sill_cm;
    r.head = o.head_cm;
    out.openings.push_back(r);
  }
  for (const Zone& z : b.zones) {
    BuildingZone r;
    r.space = z.space;
    r.kind = z.kind;
    r.rect = {z.rect.x0, z.rect.z0, z.rect.x1, z.rect.z1};
    out.zones.push_back(r);
  }
  const OccupancySummary& o = b.occupancy;
  out.occupancy.dwellings = o.dwellings;
  for (u32 k = 0; k < 5; ++k)
    out.occupancy.dwellings_by_kind[k] = o.dwellings_by_kind[k];
  out.occupancy.bedrooms = o.bedrooms;
  out.occupancy.residents = o.residents;
  for (u32 k = 0; k < k_work_kinds; ++k) {
    out.occupancy.workplaces[k] = o.workplaces[k];
    const Hours& h = plan.params.hours[k];
    if (o.workplaces[k] == 0 || !h.set) continue;
    OpeningHours r;
    r.kind = static_cast<WorkKind>(k);
    r.open = h.open;
    r.close = h.close;
    r.days = h.days;
    out.occupancy.hours.push_back(r);
  }
  if (report != nullptr) {
    for (const Finding& f : report->findings) {
      Failure r;
      r.rule = f.rule;
      r.floor = f.floor;
      r.subject = f.subject;
      r.value = f.value;
      r.limit = f.limit;
      out.failures.push_back(r);
    }
  }
}

}  // namespace engine::city

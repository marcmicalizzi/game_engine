// The building grammar (docs/subsystems/city.md, "The building grammar"): one lot to one building
// description, stage by stage — footprint, structural grid, type, floors, cores, entrances, shafts,
// floor plans, units, room graphs, doors and windows, furniture zones — and the occupancy summary.
//
// Two layouts cover the seven archetypes. **Bars** (the apartment tower, mid-rise over shops, the
// office and the civic shell): a corridor down the footprint's long axis, units on one side or
// both, stair cores on the street side at the positions the egress rules ask for, the main core
// holding the elevators and the shaft, and a lobby beside it on the ground floor. **Houses**
// (terraced and detached): a strip along one party wall with the hall, the stair and the landings,
// rooms in columns across the rest, front and back. The warehouse is a hall behind an office strip.
// Every dimension is integer centimetres in the building's frame (x along the front, z away from
// the street), every choice a draw of the lot's seed.
#include "grid.h"
#include "walls.h"

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <domain/city/building.h>
#include <domain/city/validate.h>

#include <algorithm>
#include <cstdio>

namespace engine::city {

namespace {

using grid::between;
using grid::draw;
using grid::pick;

constexpr u64 k_purpose_floors = 0xf100;
constexpr u64 k_purpose_program = 0x9209;
constexpr u64 k_purpose_width = 0x3d7;
constexpr u64 k_purpose_house = 0x4005e;

// Where the frame's axes point on the plan, by the lot's front (the quarter turn toward the
// street): local +z is into the lot, local +x along the front, a proper rotation either way.
constexpr i32 k_ex_x[4] = {0, 1, 0, -1};
constexpr i32 k_ex_z[4] = {1, 0, -1, 0};
constexpr i32 k_ez_x[4] = {-1, 0, 1, 0};
constexpr i32 k_ez_z[4] = {0, 1, 0, -1};

bool is_bar(Archetype a) noexcept {
  return a == Archetype::ApartmentTower || a == Archetype::MidRiseOverShops ||
         a == Archetype::Office || a == Archetype::CivicShell;
}

WorkKind civic_work(CivicKind c) noexcept {
  switch (c) {
    case CivicKind::Hospital: return WorkKind::Medical;
    case CivicKind::University: return WorkKind::Education;
    case CivicKind::Station: return WorkKind::Transport;
    default: return WorkKind::Civic;
  }
}

u8 space_flags(SpaceKind k) noexcept {
  switch (k) {
    case SpaceKind::LivingKitchen:
    case SpaceKind::Bedroom:
    case SpaceKind::Kitchen:
    case SpaceKind::ShopFloor:
    case SpaceKind::OpenOffice:
    case SpaceKind::Meeting:
    case SpaceKind::Department: return k_space_habitable;
    case SpaceKind::Stair:
    case SpaceKind::Elevator:
    case SpaceKind::Shaft: return k_space_vertical;
    default: return 0;
  }
}

struct Maker {
  const Plan& plan;
  const Params& p;
  const Rules& r;
  const Lot& lot;
  const Style& style;
  Building& b;
  Stage stage;
  std::string* error;

  u64 seed(u64 purpose, u64 index) const noexcept { return draw(b.seed, purpose, index); }

  bool fail(const char* what) {
    if (error != nullptr) {
      char text[160];
      std::snprintf(text, sizeof(text), "lot %u (%s): %s", lot.id, archetype_name(lot.archetype),
                    what);
      *error = text;
    }
    return false;
  }

  u32 add_space(u16 floor, const Rect& rect, SpaceKind kind, u32 unit = k_no_id,
                u32 core = k_no_id) {
    Space s;
    s.rect = rect;
    s.unit = unit;
    s.floor = floor;
    s.kind = kind;
    s.flags = space_flags(kind);
    s.core = core;
    b.spaces.push_back(s);
    return b.spaces.size() - 1;
  }

  // The floor count: the style's range clamped to the archetype's, drawn, then held under the
  // district's height.
  u32 floor_count(u32 amin, u32 amax, i32 ground_cm, i32 floor_cm) {
    const u32 lo = std::clamp(style.floors_min, amin, amax);
    const u32 hi = std::clamp(style.floors_max, lo, amax);
    u32 n = static_cast<u32>(
        between(seed(k_purpose_floors, 0), static_cast<i32>(lo), static_cast<i32>(hi)));
    const i32 cap = style.height_max_cm >= ground_cm
                        ? 1 + (style.height_max_cm - ground_cm) / std::max(floor_cm, 1)
                        : 1;
    return std::max<u32>(1, std::min<u32>(n, static_cast<u32>(cap)));
  }

  void make_floors(u32 n, i32 ground_cm, i32 floor_cm) {
    i32 at = 0;
    for (u32 f = 0; f < n; ++f) {
      Floor fl;
      fl.elevation_cm = at;
      fl.height_cm = f == 0 ? ground_cm : floor_cm;
      at += fl.height_cm;
      b.floors.push_back(fl);
    }
  }

  // ---- bars ---------------------------------------------------------------------------------

  i32 program_width(UnitKind k) const noexcept {
    const i32 living = r.min_width_cm[static_cast<u32>(SpaceKind::LivingKitchen)];
    const i32 bed = r.min_width_cm[static_cast<u32>(SpaceKind::Bedroom)];
    i32 need = living;
    switch (k) {
      case UnitKind::OneBed: need = living + bed + r.partition_cm; break;
      case UnitKind::TwoBed: need = living + 2 * (bed + r.partition_cm); break;
      case UnitKind::ThreeBed: need = living + 3 * (bed + r.partition_cm); break;
      default: break;
    }
    // A studio is at least two modules: its service band holds a bathroom and an entry side by
    // side.
    return std::max(grid::ceil_to(need, r.module_cm), 2 * r.module_cm);
  }

  // Cuts the run [x0, x1) of one side of one floor into units of `kind` (dwellings by the style's
  // mix), appending their records. Returns how many.
  void units_in_run(u16 floor, i32 x0, i32 x1, i32 z0, i32 z1, bool dwelling, UnitKind kind,
                    WorkKind work, u64 index) {
    const i32 len = x1 - x0;
    if (len <= 0) return;
    Vector<std::pair<i32, UnitKind>>& plan_units = scratch_units;
    plan_units.clear();
    if (dwelling) {
      const i32 widths[4] = {program_width(UnitKind::Studio), program_width(UnitKind::OneBed),
                             program_width(UnitKind::TwoBed), program_width(UnitKind::ThreeBed)};
      i32 at = 0;
      u32 k = 0;
      while (len - at >= widths[0]) {
        i32 total = 0;
        for (u32 m = 0; m < 4; ++m)
          total += style.unit_mix_q[m];
        u32 prog = 1;
        if (total > 0) {
          i32 pick_at = static_cast<i32>(
              pick(seed(k_purpose_program, (index << 12) | k), static_cast<u32>(total)));
          for (u32 m = 0; m < 4; ++m) {
            pick_at -= style.unit_mix_q[m];
            if (pick_at < 0) {
              prog = m;
              break;
            }
          }
        }
        while (prog > 0 && widths[prog] > len - at)
          --prog;
        plan_units.push_back({widths[prog], static_cast<UnitKind>(prog)});
        at += widths[prog];
        ++k;
      }
      if (plan_units.empty()) {
        add_space(floor, Rect{x0, z0, x1, z1}, SpaceKind::Storage);
        return;
      }
      plan_units.back().first += len - at;
    } else {
      // Workplaces: suites, shops and departments of two to four bays' worth.
      const i32 lo = kind == UnitKind::Shop ? 3 * r.module_cm : 4 * r.module_cm;
      const i32 hi = kind == UnitKind::Shop ? 5 * r.module_cm : 8 * r.module_cm;
      i32 at = 0;
      u32 k = 0;
      while (len - at >= lo) {
        i32 w =
            grid::floor_to(between(seed(k_purpose_width, (index << 12) | k), lo, hi), r.module_cm);
        if (len - at - w < lo) w = len - at;
        plan_units.push_back({w, kind});
        at += w;
        ++k;
      }
      if (plan_units.empty()) {
        if (len >= 2 * r.module_cm) {
          plan_units.push_back({len, kind});
        } else {
          add_space(floor, Rect{x0, z0, x1, z1}, SpaceKind::Storage);
          return;
        }
      } else {
        plan_units.back().first += len - at;
      }
    }
    i32 at = x0;
    for (const auto& [w, k] : plan_units) {
      Unit u;
      u.rect = Rect{at, z0, at + w, z1};
      u.floor = floor;
      u.floor_to = floor;
      u.kind = k;
      u.work = work;
      at += w;
      b.units.push_back(u);
    }
  }
  Vector<std::pair<i32, UnitKind>> scratch_units;

  // The stair cores of a bar of `n` floors: as many stairs as the threshold and the travel distance
  // ask for, spread evenly along it, alternately on the street side and the back side when there is
  // one, each on whole bays; the street-side core nearest the middle is the main one, with the
  // elevators and the shaft, and the ground floor's lobby takes the bay beside it. False when they
  // do not fit.
  bool place_cores(u32 n, i32 width, i32 depth, i32 da, i32 db, i32 c, i32 bay, i32 bays,
                   bool civic, Rect& lobby, u32& main) {
    const Archetype a = lot.archetype;
    b.cores.clear();
    u32 stairs = n >= r.two_stair_floors ? 2 : 1;
    const i32 reach = std::max(r.max_travel_cm - c, bay);
    stairs = std::max<u32>(stairs, static_cast<u32>((width + 2 * reach - 1) / (2 * reach)));
    // Elevators: from the threshold, one per so many units (or workplaces) above the ground,
    // counted at the smallest unit so the estimate never falls short of what is built.
    u32 elevators = 0;
    if (n >= r.elevator_floors) {
      const bool dwellings = a == Archetype::ApartmentTower || a == Archetype::MidRiseOverShops;
      const i64 run = (db > 0 ? 2 : 1) * i64{width};
      if (dwellings) {
        const u64 above = static_cast<u64>((n - 1) * (run / program_width(UnitKind::Studio)));
        elevators = static_cast<u32>((above + r.units_per_elevator - 1) / r.units_per_elevator);
      } else {
        const i64 area = i64{width} * depth;
        const i64 per = civic ? r.civic_area_cm2 : r.office_area_cm2;
        const u64 above = static_cast<u64>((n - 1) * (area / std::max<i64>(per, 1)));
        elevators =
            static_cast<u32>((above + r.workplaces_per_elevator - 1) / r.workplaces_per_elevator);
      }
      elevators = std::clamp<u32>(elevators, 1, 8);
    }
    const i32 x0 = b.footprint.x0;
    const i32 zc1 = b.footprint.z0 + da + c;
    // Which stair is the main one: the street-side stair nearest the middle.
    main = 0;
    i32 best = -1;
    for (u32 k = 0; k < stairs; ++k) {
      const bool back = db > 0 && k % 2 == 1;
      if (back) continue;
      const i32 centre = static_cast<i32>(i64{width} * (2 * k + 1) / (2 * stairs));
      const i32 off = centre > width / 2 ? centre - width / 2 : width / 2 - centre;
      if (best < 0 || off < best) {
        best = off;
        main = k;
      }
    }
    i32 next_free[2] = {0, 0};
    for (u32 k = 0; k < stairs; ++k) {
      const bool is_main = k == main;
      const u32 sd = db > 0 && k % 2 == 1 ? 1 : 0;
      // The stair and the cars side by side; the shaft beside them when the bays leave room for
      // it, else as a strip along the core's facade side (below).
      const i32 need = r.stair_cm + (is_main ? elevators * r.elevator_cm : 0) + 2 * r.core_wall_cm;
      const i32 core_bays = std::max(1, (need + bay - 1) / bay);
      const i32 centre = static_cast<i32>(i64{width} * (2 * k + 1) / (2 * stairs));
      i32 first = std::clamp((centre - core_bays * bay / 2 + bay / 2) / bay, 0,
                             std::max(0, bays - core_bays));
      first = std::max(first, next_free[sd]);
      // The main core keeps a bay free after it for the lobby when it can.
      if (first + core_bays > bays) return false;
      next_free[sd] = first + core_bays + (is_main ? 1 : 0);
      Core core;
      core.rect = sd == 0
                      ? Rect{x0 + first * bay, b.footprint.z0, x0 + (first + core_bays) * bay,
                             b.footprint.z0 + da}
                      : Rect{x0 + first * bay, zc1, x0 + (first + core_bays) * bay, b.footprint.z1};
      core.floor_from = 0;
      core.floor_to = static_cast<u16>(n - 1);
      core.stair = 1;
      core.elevators = static_cast<u8>(is_main ? elevators : 0);
      core.shaft = is_main ? 1 : 0;
      core.main = is_main ? 1 : 0;
      b.cores.push_back(core);
    }
    const Core& mc = b.cores[main];
    for (u32 side = 0; side < 2; ++side) {
      lobby = side == 0 ? Rect{mc.rect.x1, mc.rect.z0, mc.rect.x1 + bay, mc.rect.z1}
                        : Rect{mc.rect.x0 - bay, mc.rect.z0, mc.rect.x0, mc.rect.z1};
      bool ok = lobby.x0 >= b.footprint.x0 && lobby.x1 <= b.footprint.x1;
      for (const Core& other : b.cores)
        ok = ok && !grid::intersects(other.rect, lobby);
      if (ok) return true;
    }
    return false;
  }

  bool bar() {
    const Archetype a = lot.archetype;
    const bool civic = a == Archetype::CivicShell;
    const i32 c = civic ? r.civic_corridor_cm : r.corridor_cm;
    b.corridor_cm = c;
    const i32 W = b.lot_rect.width();
    const i32 D = b.lot_rect.depth();
    i32 side = style.setback_side_cm;
    if (a == Archetype::ApartmentTower || civic) side = std::max(side, 600);
    const Rect avail{side, style.setback_front_cm, W - side, D - style.setback_rear_cm};
    if (avail.width() < 4 * r.module_cm || avail.depth() < r.unit_depth_min_cm + c)
      return fail("the lot leaves no room for a bar");
    // The structural bay: three modules, or half the width where that is narrower, never below two.
    i32 bay = 3 * r.module_cm;
    if (avail.width() < 2 * bay)
      bay = std::max(2 * r.module_cm, grid::floor_to(avail.width() / 2, r.module_cm));
    // Depth: the deepest bar that still leaves two bays under the coverage, double-loaded when two
    // units and a corridor fit in it, else single-loaded.
    const i64 max_area = grid::mul_q(b.lot_rect.area(), style.coverage_q);
    const i32 depth_cap = static_cast<i32>(std::min<i64>(avail.depth(), max_area / (2 * i64{bay})));
    i32 da = 0, db = 0;
    if (depth_cap >= 2 * r.unit_depth_min_cm + c) {
      da = db = grid::floor_to(std::min(r.unit_depth_max_cm, (depth_cap - c) / 2), 10);
    } else if (depth_cap >= r.unit_depth_min_cm + c) {
      da = grid::floor_to(std::min(r.unit_depth_max_cm, depth_cap - c), 10);
    } else {
      return fail("the coverage leaves no room for a bar");
    }
    const i32 depth = da + c + db;
    // Width: whole bays, under the coverage, a tower no wider than twice its depth.
    i32 bays = avail.width() / bay;
    bays = std::min<i32>(bays, static_cast<i32>(max_area / (i64{depth} * bay)));
    if (a == Archetype::ApartmentTower) bays = std::min(bays, std::max(2, 2 * depth / bay));
    if (bays < 2) return fail("the coverage leaves less than two bays");
    const i32 width = bays * bay;
    const i32 x0 = avail.x0 + grid::floor_to((avail.width() - width) / 2, 10);
    b.footprint = Rect{x0, avail.z0, x0 + width, avail.z0 + depth};
    b.bay_x_cm = bay;
    b.bay_z_cm = std::max(da, db);
    // Facades touching a neighbour on a side line have no daylight.
    b.exposed[1] = b.footprint.x1 < W ? 1 : 0;
    b.exposed[3] = b.footprint.x0 > 0 ? 1 : 0;

    // Floors.
    u32 amin = 3, amax = 8;
    if (a == Archetype::ApartmentTower) {
      amin = 8;
      amax = 60;
    } else if (a == Archetype::Office) {
      amin = 2;
      amax = 40;
    } else if (civic) {
      amin = 2;
      amax = lot.civic == CivicKind::Station ? 2 : 8;
    }
    u32 n = floor_count(amin, amax, r.ground_floor_cm, r.floor_cm);
    const i32 zc0 = b.footprint.z0 + da;
    const i32 zc1 = zc0 + c;
    Rect lobby;
    u32 main = 0;
    // Cores, and a lobby beside the main one. A bar too narrow for the stairs and the elevators its
    // height asks for keeps the tallest floor count whose cores still fit: fewer floors need fewer
    // cars, and below the thresholds one stair and none.
    while (!place_cores(n, width, depth, da, db, c, bay, bays, civic, lobby, main)) {
      if (n <= 1) return fail("the stair cores and a lobby do not fit along the bar");
      --n;
    }
    make_floors(n, r.ground_floor_cm, r.floor_cm);
    if (stage == Stage::Massing) {
      b.cores.clear();
      return true;
    }

    const bool shops = a == Archetype::MidRiseOverShops;
    const UnitKind work_unit = civic ? UnitKind::Department : UnitKind::OfficeSuite;
    const WorkKind work = civic ? civic_work(lot.civic) : WorkKind::Office;
    for (u32 f = 0; f < n; ++f) {
      const u16 fl = static_cast<u16>(f);
      b.floors[f].first_space = b.spaces.size();
      add_space(fl, Rect{b.footprint.x0, zc0, b.footprint.x1, zc1}, SpaceKind::Corridor);
      for (u32 k = 0; k < b.cores.size(); ++k) {
        const Core& core = b.cores[k];
        const i32 cars = r.stair_cm + core.elevators * r.elevator_cm;
        const bool beside = core.rect.width() - cars >= r.shaft_cm;
        // A shaft that does not fit beside the stair is a strip along the core's facade side, so
        // the stair and the cars still open onto the corridor.
        const bool back = core.rect.z0 != b.footprint.z0;
        i32 cz0 = core.rect.z0, cz1 = core.rect.z1;
        if (core.shaft != 0 && !beside) {
          const Rect strip =
              back ? Rect{core.rect.x0, core.rect.z1 - r.shaft_cm, core.rect.x1, core.rect.z1}
                   : Rect{core.rect.x0, core.rect.z0, core.rect.x1, core.rect.z0 + r.shaft_cm};
          add_space(fl, strip, SpaceKind::Shaft, k_no_id, k);
          (back ? cz1 : cz0) = back ? strip.z0 : strip.z1;
        }
        i32 at = core.rect.x0;
        add_space(fl, Rect{at, cz0, at + r.stair_cm, cz1}, SpaceKind::Stair, k_no_id, k);
        at += r.stair_cm;
        for (u32 e = 0; e < core.elevators; ++e) {
          add_space(fl, Rect{at, cz0, at + r.elevator_cm, cz1}, SpaceKind::Elevator, k_no_id, k);
          at += r.elevator_cm;
        }
        // Whatever the core's bays hold past the stair and the cars is the shaft (or part of the
        // stair).
        if (core.shaft != 0 && beside) {
          add_space(fl, Rect{at, cz0, core.rect.x1, cz1}, SpaceKind::Shaft, k_no_id, k);
        } else if (at < core.rect.x1) {
          b.spaces.back().rect.x1 = core.rect.x1;
        }
      }
      if (f == 0) add_space(fl, lobby, SpaceKind::Lobby);
      // Each side's runs between its cores (and, on the street side, the lobby).
      for (u32 sd = 0; sd < 2; ++sd) {
        if (sd == 1 && db == 0) continue;
        const i32 z0 = sd == 0 ? b.footprint.z0 : zc1;
        const i32 z1 = sd == 0 ? zc0 : b.footprint.z1;
        Vector<std::pair<i32, i32>> taken;
        for (const Core& core : b.cores) {
          if (core.rect.z0 == z0) taken.push_back({core.rect.x0, core.rect.x1});
        }
        if (f == 0 && sd == 0) taken.push_back({lobby.x0, lobby.x1});
        std::sort(taken.begin(), taken.end());
        i32 at = b.footprint.x0;
        u64 run = 0;
        auto side_run = [&](i32 from, i32 to) {
          if (to <= from) return;
          const u64 index = (u64{f} << 8) | (u64{sd} << 7) | run++;
          if (f == 0 && shops && sd == 0) {
            units_in_run(fl, from, to, z0, z1, false, UnitKind::Shop, WorkKind::Retail, index);
          } else if (a == Archetype::ApartmentTower || a == Archetype::MidRiseOverShops) {
            units_in_run(fl, from, to, z0, z1, true, UnitKind::Studio, WorkKind::Retail, index);
          } else {
            units_in_run(fl, from, to, z0, z1, false, work_unit, work, index);
          }
        };
        for (const auto& [t0, t1] : taken) {
          side_run(at, t0);
          at = t1;
        }
        side_run(at, b.footprint.x1);
      }
      b.floors[f].space_count = b.spaces.size() - b.floors[f].first_space;
    }
    return true;
  }

  // ---- houses -------------------------------------------------------------------------------

  bool house() {
    const bool terraced = lot.archetype == Archetype::TerracedHouse;
    const i32 W = b.lot_rect.width();
    const i32 D = b.lot_rect.depth();
    const i32 side = terraced ? 0 : style.setback_side_cm;
    const Rect avail{side, style.setback_front_cm, W - side, D - style.setback_rear_cm};
    const i32 strip = r.stair_cm / 2;
    const i32 min_depth =
        2 * (r.service_band_cm + r.min_depth_cm[static_cast<u32>(SpaceKind::Bedroom)]);
    if (avail.width() < strip + r.min_width_cm[static_cast<u32>(SpaceKind::Bedroom)] ||
        avail.depth() < min_depth)
      return fail("the lot leaves no room for a house");
    i32 hw = avail.width();
    if (!terraced) {
      hw = std::min(hw, grid::floor_to(between(seed(k_purpose_house, 0), 800, 1200), 10));
    }
    i32 hd = std::min(
        avail.depth(),
        std::max(
            min_depth,
            grid::floor_to(between(seed(k_purpose_house, 1), min_depth, min_depth + 200), 10)));
    const i64 max_area = grid::mul_q(b.lot_rect.area(), style.coverage_q);
    while (i64{hw} * hd > max_area && hd > min_depth)
      hd = std::max(min_depth, hd - 50);
    if (i64{hw} * hd > max_area && !terraced) hw = static_cast<i32>(max_area / hd / 10 * 10);
    if (hw < strip + r.min_width_cm[static_cast<u32>(SpaceKind::Bedroom)])
      return fail("the coverage leaves no room for a house");
    const i32 x0 = avail.x0 + (terraced ? 0 : grid::floor_to((avail.width() - hw) / 2, 10));
    b.footprint = Rect{x0, avail.z0, x0 + hw, avail.z0 + hd};
    b.exposed[1] = terraced ? 0 : 1;
    b.exposed[3] = terraced ? 0 : 1;
    // Columns of rooms across the rest, none wider than a house's span.
    const i32 rest = hw - strip;
    const i32 cols = std::max(1, (rest + r.max_span_house_cm - 1) / r.max_span_house_cm);
    b.bay_x_cm = std::max(strip, (rest + cols - 1) / cols);
    b.bay_z_cm = hd / 2;
    const u32 n = floor_count(2, 3, r.floor_cm, r.floor_cm);
    make_floors(std::max<u32>(n, 2), r.floor_cm, r.floor_cm);
    const u32 floors = b.floors.size();
    if (stage == Stage::Massing) return true;

    Core core;
    const i32 a = 150;    // the hall before the stair
    const i32 run = 300;  // the stair's run
    core.rect = Rect{x0, b.footprint.z0 + a, x0 + strip, b.footprint.z0 + a + run};
    core.floor_to = static_cast<u16>(floors - 1);
    core.stair = 1;
    b.cores.push_back(core);
    Unit u;
    u.rect = b.footprint;
    u.floor = 0;
    u.floor_to = static_cast<u16>(floors - 1);
    u.kind = UnitKind::House;
    b.units.push_back(u);
    const i32 z0 = b.footprint.z0;
    const i32 zm = z0 + hd / 2;
    const i32 z1 = b.footprint.z1;
    for (u32 f = 0; f < floors; ++f) {
      const u16 fl = static_cast<u16>(f);
      b.floors[f].first_space = b.spaces.size();
      if (stage == Stage::Floors) {
        add_space(fl, core.rect, SpaceKind::Stair, 0, 0);
        b.floors[f].space_count = b.spaces.size() - b.floors[f].first_space;
        continue;
      }
      if (f == 0) {
        add_space(fl, Rect{x0, z0, x0 + strip, z0 + a}, SpaceKind::Entry, 0);
        add_space(fl, core.rect, SpaceKind::Stair, 0, 0);
        add_space(fl, Rect{x0, z0 + a + run, x0 + strip, z1}, SpaceKind::Wc, 0);
      } else {
        add_space(fl, Rect{x0, z0, x0 + strip, z0 + a}, SpaceKind::Landing, 0);
        add_space(fl, core.rect, SpaceKind::Stair, 0, 0);
        add_space(fl, Rect{x0, z0 + a + run, x0 + strip, z1}, SpaceKind::Landing, 0);
      }
      for (i32 k = 0; k < cols; ++k) {
        const i32 cx0 = x0 + strip + static_cast<i32>(i64{rest} * k / cols);
        const i32 cx1 = x0 + strip + static_cast<i32>(i64{rest} * (k + 1) / cols);
        if (f == 0) {
          add_space(fl, Rect{cx0, z0, cx1, zm},
                    k == 0 ? SpaceKind::Kitchen : SpaceKind::LivingKitchen, 0);
          add_space(fl, Rect{cx0, zm, cx1, z1}, SpaceKind::LivingKitchen, 0);
        } else {
          add_space(fl, Rect{cx0, z0, cx1, zm}, SpaceKind::Bedroom, 0);
          if (k == 0) {
            add_space(fl, Rect{cx0, zm, cx1, zm + r.service_band_cm}, SpaceKind::Bathroom, 0);
            add_space(fl, Rect{cx0, zm + r.service_band_cm, cx1, z1}, SpaceKind::Bedroom, 0);
          } else {
            add_space(fl, Rect{cx0, zm, cx1, z1}, SpaceKind::Bedroom, 0);
          }
        }
      }
      b.floors[f].space_count = b.spaces.size() - b.floors[f].first_space;
    }
    b.units[0].first_space = 0;
    b.units[0].space_count = b.spaces.size();
    return true;
  }

  // ---- the warehouse ------------------------------------------------------------------------

  bool warehouse() {
    const i32 W = b.lot_rect.width();
    const i32 D = b.lot_rect.depth();
    const Rect avail{style.setback_side_cm, style.setback_front_cm, W - style.setback_side_cm,
                     D - style.setback_rear_cm};
    const i32 bay = 6 * r.module_cm;
    const i32 bays = avail.width() / bay;
    const i32 office = 3 * r.module_cm;
    if (bays < 2 || avail.depth() < office + 2 * bay)
      return fail("the lot leaves no room for a hall");
    const i32 width = bays * bay;
    const i64 max_area = grid::mul_q(b.lot_rect.area(), style.coverage_q);
    const i32 depth =
        grid::floor_to(std::min<i32>(avail.depth(), static_cast<i32>(max_area / width)), 100);
    if (depth < office + 2 * bay) return fail("the coverage leaves no room for a hall");
    const i32 x0 = avail.x0 + grid::floor_to((avail.width() - width) / 2, 10);
    b.footprint = Rect{x0, avail.z0, x0 + width, avail.z0 + depth};
    const i32 hall = depth - office;
    const i32 spans = (hall + r.max_span_hall_cm - 1) / r.max_span_hall_cm;
    b.bay_x_cm = bay;
    b.bay_z_cm = (hall + spans - 1) / spans;
    make_floors(1, 2 * r.ground_floor_cm, 2 * r.ground_floor_cm);
    if (stage == Stage::Massing) return true;
    Unit u;
    u.rect = b.footprint;
    u.kind = UnitKind::Workshop;
    u.work = WorkKind::Industrial;
    b.units.push_back(u);
    b.floors[0].first_space = 0;
    if (stage == Stage::Rooms) {
      const i32 z0 = b.footprint.z0;
      const i32 reception = 2 * r.module_cm;
      const i32 wc = r.min_depth_cm[static_cast<u32>(SpaceKind::Wc)] + 10;
      add_space(0, Rect{x0, z0, x0 + reception, z0 + office}, SpaceKind::Reception, 0);
      add_space(0, Rect{x0 + reception, z0, x0 + reception + wc, z0 + office}, SpaceKind::Wc, 0);
      add_space(0, Rect{x0 + reception + wc, z0, x0 + width, z0 + office}, SpaceKind::OpenOffice,
                0);
      add_space(0, Rect{x0, z0 + office, x0 + width, b.footprint.z1}, SpaceKind::Hall, 0);
    }
    b.units[0].space_count = b.spaces.size();
    b.floors[0].space_count = b.spaces.size();
    return true;
  }

  // ---- rooms --------------------------------------------------------------------------------

  // A bar's unit, once its floor is known: which side of the corridor it is on decides where its
  // facade is.
  void unit_rooms(u32 ui) {
    Unit& u = b.units[ui];
    const bool front = u.rect.z0 == b.footprint.z0;  // the street side: facade at z0
    const i32 band = r.service_band_cm;
    const i32 x0 = u.rect.x0;
    const i32 x1 = u.rect.x1;
    // Service band against the corridor, rooms against the facade.
    const i32 sz0 = front ? u.rect.z1 - band : u.rect.z0;
    const i32 sz1 = front ? u.rect.z1 : u.rect.z0 + band;
    const i32 rz0 = front ? u.rect.z0 : u.rect.z0 + band;
    const i32 rz1 = front ? u.rect.z1 - band : u.rect.z1;
    const u16 fl = u.floor;
    u.first_space = b.spaces.size();
    switch (u.kind) {
      case UnitKind::Studio:
      case UnitKind::OneBed:
      case UnitKind::TwoBed:
      case UnitKind::ThreeBed: {
        const u32 beds = static_cast<u32>(u.kind);
        const i32 bath = r.min_width_cm[static_cast<u32>(SpaceKind::Bathroom)] + 20;
        const i32 wc = beds >= 2 ? r.min_width_cm[static_cast<u32>(SpaceKind::Wc)] + 30 : 0;
        add_space(fl, Rect{x0, sz0, x0 + bath, sz1}, SpaceKind::Bathroom, ui);
        u.entry = add_space(fl, Rect{x0 + bath, sz0, x1 - wc, sz1}, SpaceKind::Entry, ui);
        if (wc > 0) add_space(fl, Rect{x1 - wc, sz0, x1, sz1}, SpaceKind::Wc, ui);
        const i32 living_min = r.min_width_cm[static_cast<u32>(SpaceKind::LivingKitchen)];
        const i32 bed_w =
            beds > 0 ? grid::floor_to((x1 - x0 - living_min) / static_cast<i32>(beds), 10) : 0;
        const i32 living = x1 - x0 - bed_w * static_cast<i32>(beds);
        add_space(fl, Rect{x0, rz0, x0 + living, rz1}, SpaceKind::LivingKitchen, ui);
        for (u32 k = 0; k < beds; ++k) {
          const i32 bx0 = x0 + living + bed_w * static_cast<i32>(k);
          add_space(fl, Rect{bx0, rz0, bx0 + bed_w, rz1}, SpaceKind::Bedroom, ui);
        }
        u.bedrooms = static_cast<u16>(beds);
        break;
      }
      case UnitKind::OfficeSuite: {
        const i32 wc = r.min_width_cm[static_cast<u32>(SpaceKind::Wc)] + 60;
        // A meeting room as long as its minimum, and never narrower than a door's opening.
        const i32 meeting =
            std::max(r.min_depth_cm[static_cast<u32>(SpaceKind::Meeting)], r.door_cm + 60);
        u.entry = add_space(fl, Rect{x0, sz0, x1 - wc, sz1}, SpaceKind::Reception, ui);
        add_space(fl, Rect{x1 - wc, sz0, x1, sz1}, SpaceKind::Wc, ui);
        if (x1 - x0 >= meeting + r.min_depth_cm[static_cast<u32>(SpaceKind::OpenOffice)]) {
          add_space(fl, Rect{x0, rz0, x1 - meeting, rz1}, SpaceKind::OpenOffice, ui);
          add_space(fl, Rect{x1 - meeting, rz0, x1, rz1}, SpaceKind::Meeting, ui);
        } else {
          add_space(fl, Rect{x0, rz0, x1, rz1}, SpaceKind::OpenOffice, ui);
        }
        break;
      }
      case UnitKind::Shop: {
        const i32 wc = r.min_width_cm[static_cast<u32>(SpaceKind::Wc)] + 60;
        u.entry = add_space(fl, Rect{x0, rz0, x1, rz1}, SpaceKind::ShopFloor, ui);
        add_space(fl, Rect{x0, sz0, x1 - wc, sz1}, SpaceKind::BackRoom, ui);
        add_space(fl, Rect{x1 - wc, sz0, x1, sz1}, SpaceKind::Wc, ui);
        break;
      }
      default: u.entry = add_space(fl, u.rect, SpaceKind::Department, ui); break;
    }
    u.space_count = b.spaces.size() - u.first_space;
  }

  // ---- occupancy ----------------------------------------------------------------------------

  void occupancy() {
    OccupancySummary& o = b.occupancy;
    o = OccupancySummary{};
    for (Unit& u : b.units) {
      const i64 area = u.rect.area() * (u.kind == UnitKind::House ? 1 : 1);
      switch (u.kind) {
        case UnitKind::Studio:
        case UnitKind::OneBed:
        case UnitKind::TwoBed:
        case UnitKind::ThreeBed: {
          static constexpr u16 k_people[4] = {1, 2, 4, 5};
          const u32 prog = static_cast<u32>(u.kind);
          u.bedrooms = static_cast<u16>(prog);
          u.residents = k_people[prog];
          break;
        }
        case UnitKind::House: {
          // Bedrooms: every upper floor's front column and back columns, the first back one
          // behind the bathroom.
          const i32 strip = r.stair_cm / 2;
          const i32 rest = b.footprint.width() - strip;
          const i32 cols = std::max(1, (rest + r.max_span_house_cm - 1) / r.max_span_house_cm);
          u.bedrooms = static_cast<u16>((b.floors.size() - 1) * 2 * static_cast<u32>(cols));
          u.residents = static_cast<u16>(u.bedrooms + 1);
          break;
        }
        case UnitKind::Shop:
          u.workplaces = static_cast<u16>(std::max<i64>(1, area / r.retail_area_cm2));
          break;
        case UnitKind::OfficeSuite:
          u.workplaces = static_cast<u16>(std::max<i64>(1, area / r.office_area_cm2));
          break;
        case UnitKind::Workshop: {
          const i64 office = i64{b.footprint.width()} * 3 * r.module_cm;
          u.workplaces = static_cast<u16>(std::max<i64>(
              1, (area - office) / r.industrial_area_cm2 + office / r.office_area_cm2));
          break;
        }
        case UnitKind::Department:
          u.workplaces = static_cast<u16>(std::max<i64>(1, area / r.civic_area_cm2));
          break;
      }
      if (u.kind <= UnitKind::House) {
        ++o.dwellings;
        ++o.dwellings_by_kind[static_cast<u32>(u.kind)];
        o.bedrooms += u.bedrooms;
        o.residents += u.residents;
      } else {
        o.workplaces[static_cast<u32>(u.work)] += u.workplaces;
      }
    }
  }

  bool run() {
    b.floors.clear();
    switch (lot.archetype) {
      case Archetype::TerracedHouse:
      case Archetype::DetachedHouse:
        if (!house()) return false;
        break;
      case Archetype::Warehouse:
        if (!warehouse()) return false;
        break;
      default:
        if (!bar()) return false;
        break;
    }
    if (stage == Stage::Massing) return true;
    occupancy();
    if (stage == Stage::Floors) return true;
    if (is_bar(lot.archetype)) {
      // Rooms unit by unit, then each floor's spaces in floor order: the units' rooms were appended
      // after every floor's circulation, so the spaces are regrouped by floor here.
      const u32 circulation = b.spaces.size();
      for (u32 ui = 0; ui < b.units.size(); ++ui)
        unit_rooms(ui);
      regroup_by_floor(circulation);
    }
    make_walls_and_openings(plan, b, r);
    make_zones(b, r);
    return true;
  }

  // Sorts the spaces by floor (stably, so each floor keeps circulation first and then its units'
  // rooms in unit order), remapping the units' and the floors' ranges.
  void regroup_by_floor(u32 circulation) {
    (void)circulation;
    const u32 n = b.spaces.size();
    Vector<u32> order(n);
    for (u32 i = 0; i < n; ++i)
      order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](u32 x, u32 y) { return b.spaces[x].floor < b.spaces[y].floor; });
    Vector<u32> where(n);
    Vector<Space> sorted(n);
    for (u32 i = 0; i < n; ++i) {
      where[order[i]] = i;
      sorted[i] = b.spaces[order[i]];
    }
    b.spaces = std::move(sorted);
    for (Unit& u : b.units) {
      if (u.space_count == 0) continue;
      u.entry = where[u.entry];
      u.first_space = where[u.first_space];
    }
    for (Floor& f : b.floors) {
      f.first_space = 0;
      f.space_count = 0;
    }
    for (u32 i = 0; i < n; ++i) {
      Floor& f = b.floors[b.spaces[i].floor];
      if (f.space_count == 0) f.first_space = i;
      ++f.space_count;
    }
  }
};

}  // namespace

void Building::clear() noexcept {
  lot = k_no_id;
  lot_id = k_no_id;
  seed = 0;
  exposed[0] = exposed[1] = exposed[2] = exposed[3] = 1;
  floors.clear();
  cores.clear();
  spaces.clear();
  units.clear();
  walls.clear();
  openings.clear();
  zones.clear();
  links.clear();
  occupancy = OccupancySummary{};
  footprint = Rect{};
  lot_rect = Rect{};
  bay_x_cm = bay_z_cm = corridor_cm = 0;
}

i32 Building::height_cm() const noexcept {
  return floors.empty() ? 0 : floors.back().elevation_cm + floors.back().height_cm;
}

Stage stage_for_distance(f32 distance_tiles) noexcept {
  if (distance_tiles < 1.5f) return Stage::Rooms;
  if (distance_tiles < 6.0f) return Stage::Floors;
  return Stage::Massing;
}

bool Grammar::generate(u32 lot_index, Stage stage, Building& out, std::string* error) {
  out.clear();
  if (lot_index >= plan_.lots.size()) {
    if (error != nullptr) *error = "no lot " + std::to_string(lot_index);
    return false;
  }
  const Lot& lot = plan_.lots[lot_index];
  if (lot.use == LotUse::Park) {
    if (error != nullptr) *error = "lot " + std::to_string(lot.id) + " is a park";
    return false;
  }
  const Params& p = plan_.params;
  const Style& style = p.styles[static_cast<u32>(plan_.districts[lot.district].kind)];
  out.lot = lot_index;
  out.lot_id = lot.id;
  out.seed = lot_seed(plan_, lot);
  out.archetype = lot.archetype;
  out.civic = lot.civic;
  out.stage = stage;
  out.front = lot.front;
  const u8 f = lot.front & 3u;
  // The frame's origin is the lot corner the rotation puts at local (0, 0).
  const i32 ox = k_ex_x[f] + k_ez_x[f] < 0 ? lot.rect.x1 : lot.rect.x0;
  const i32 oz = k_ex_z[f] + k_ez_z[f] < 0 ? lot.rect.z1 : lot.rect.z0;
  out.origin_x = ox;
  out.origin_z = oz;
  const bool across_x = f == 1 || f == 3;
  out.lot_rect = Rect{0, 0, across_x ? lot.rect.width() : lot.rect.depth(),
                      across_x ? lot.rect.depth() : lot.rect.width()};
  Maker maker{plan_, p, p.rules, lot, style, out, stage, error, {}};
  const bool ok = maker.run();
  if (ok) {
    // Vertical links: a stair or an elevator to the same space on the floor above.
    for (u32 s = 0; s < out.spaces.size(); ++s) {
      const Space& sp = out.spaces[s];
      if ((sp.flags & k_space_vertical) == 0 || sp.kind == SpaceKind::Shaft) continue;
      if (static_cast<u32>(sp.floor) + 1 >= out.floors.size()) continue;
      const Floor& above = out.floors[sp.floor + 1u];
      for (u32 t = above.first_space; t < above.first_space + above.space_count; ++t) {
        if (out.spaces[t].kind == sp.kind && out.spaces[t].rect == sp.rect) {
          out.links.push_back(Link{s, t});
          break;
        }
      }
    }
  }
  scratch_.clear();
  return ok;
}

bool generate_buildings(const Plan& plan, std::span<const u32> lots, Stage stage,
                        jobs::JobSystem* jobs, Vector<Building>& out, std::string* error) {
  const u32 count = static_cast<u32>(lots.size());
  out.clear();
  out.resize(count);
  Vector<u8> ok(count, u8{1});
  Vector<std::string> errors(count);
  auto one = [&](u32 i) {
    Grammar grammar(plan);
    ok[i] = grammar.generate(lots[i], stage, out[i], &errors[i]) ? 1 : 0;
  };
  if (jobs != nullptr && count > 1 && jobs->worker_count(jobs::Pool::Performance) > 0) {
    jobs->parallel_for(jobs::Pool::Performance, count, 1, [&](u32 begin, u32 end) {
      for (u32 i = begin; i < end; ++i)
        one(i);
    });
  } else {
    for (u32 i = 0; i < count; ++i)
      one(i);
  }
  for (u32 i = 0; i < count; ++i) {
    if (ok[i] == 0) {
      if (error != nullptr) *error = errors[i];
      return false;
    }
  }
  return true;
}

void to_plan(const Building& b, i64 x, i64 z, i64& px, i64& pz) noexcept {
  const u8 f = b.front & 3u;
  px = b.origin_x + x * k_ex_x[f] + z * k_ez_x[f];
  pz = b.origin_z + x * k_ex_z[f] + z * k_ez_z[f];
}

Rect footprint_on_plan(const Building& b) noexcept {
  i64 ax, az, bx, bz;
  to_plan(b, b.footprint.x0, b.footprint.z0, ax, az);
  to_plan(b, b.footprint.x1, b.footprint.z1, bx, bz);
  return Rect{static_cast<i32>(std::min(ax, bx)), static_cast<i32>(std::min(az, bz)),
              static_cast<i32>(std::max(ax, bx)), static_cast<i32>(std::max(az, bz))};
}

u64 hash_building(const Building& b) noexcept {
  u64 h = hash_combine(k_hash_seed, b.seed);
  auto rect = [&](const Rect& r) {
    h = hash_combine(h, u64{static_cast<u32>(r.x0)} << 32 | static_cast<u32>(r.z0));
    h = hash_combine(h, u64{static_cast<u32>(r.x1)} << 32 | static_cast<u32>(r.z1));
  };
  h = hash_combine(h, u64{b.lot_id} << 32 | u64{static_cast<u8>(b.archetype)} << 16 |
                          u64{static_cast<u8>(b.civic)} << 8 | static_cast<u8>(b.stage));
  h = hash_combine(h, u64{static_cast<u32>(b.origin_x)} << 32 | static_cast<u32>(b.origin_z));
  h = hash_combine(h, u64{b.front} << 32 | u64{b.exposed[0]} << 24 | u64{b.exposed[1]} << 16 |
                          u64{b.exposed[2]} << 8 | b.exposed[3]);
  rect(b.footprint);
  rect(b.lot_rect);
  h = hash_combine(h, u64{static_cast<u32>(b.bay_x_cm)} << 32 | static_cast<u32>(b.bay_z_cm));
  h = hash_combine(h, static_cast<u32>(b.corridor_cm));
  h = hash_combine(h, b.floors.size());
  for (const Floor& f : b.floors) {
    h = hash_combine(h,
                     u64{static_cast<u32>(f.elevation_cm)} << 32 | static_cast<u32>(f.height_cm));
    h = hash_combine(h, u64{f.first_space} << 32 | f.space_count);
    h = hash_combine(h, u64{f.first_wall} << 32 | f.wall_count);
  }
  h = hash_combine(h, b.cores.size());
  for (const Core& c : b.cores) {
    rect(c.rect);
    h = hash_combine(h, u64{c.floor_from} << 48 | u64{c.floor_to} << 32 | u64{c.stair} << 24 |
                            u64{c.elevators} << 16 | u64{c.shaft} << 8 | c.main);
  }
  h = hash_combine(h, b.spaces.size());
  for (const Space& s : b.spaces) {
    rect(s.rect);
    h = hash_combine(
        h, u64{s.unit} << 32 | u64{s.floor} << 16 | u64{static_cast<u8>(s.kind)} << 8 | s.flags);
    h = hash_combine(h, s.core);
  }
  h = hash_combine(h, b.units.size());
  for (const Unit& u : b.units) {
    rect(u.rect);
    h = hash_combine(h, u64{u.first_space} << 32 | u.space_count);
    h = hash_combine(h, u64{u.entry} << 32 | u64{u.floor} << 16 | u.floor_to);
    h = hash_combine(h, u64{static_cast<u8>(u.kind)} << 56 | u64{static_cast<u8>(u.work)} << 48 |
                            u64{u.bedrooms} << 32 | u64{u.residents} << 16 | u.workplaces);
  }
  h = hash_combine(h, b.walls.size());
  for (const Wall& w : b.walls) {
    h = hash_combine(h, u64{static_cast<u32>(w.x0)} << 32 | static_cast<u32>(w.z0));
    h = hash_combine(h, u64{static_cast<u32>(w.x1)} << 32 | static_cast<u32>(w.z1));
    h = hash_combine(h, u64{w.a} << 32 | w.b);
    h = hash_combine(h, u64{w.floor} << 32 | u64{w.thickness_cm} << 8 | static_cast<u8>(w.kind));
  }
  h = hash_combine(h, b.openings.size());
  for (const Opening& o : b.openings) {
    h = hash_combine(h, u64{o.wall} << 32 | static_cast<u32>(o.at_cm));
    h = hash_combine(h, u64{o.width_cm} << 48 | u64{o.sill_cm} << 32 | u64{o.head_cm} << 16 |
                            static_cast<u8>(o.kind));
  }
  h = hash_combine(h, b.zones.size());
  for (const Zone& z : b.zones) {
    rect(z.rect);
    h = hash_combine(h, u64{z.space} << 8 | static_cast<u8>(z.kind));
  }
  h = hash_combine(h, b.links.size());
  for (const Link& l : b.links)
    h = hash_combine(h, u64{l.below} << 32 | l.above);
  const OccupancySummary& o = b.occupancy;
  h = hash_combine(h, u64{o.dwellings} << 32 | o.bedrooms);
  h = hash_combine(h, o.residents);
  for (const u32 v : o.dwellings_by_kind)
    h = hash_combine(h, v);
  for (const u32 v : o.workplaces)
    h = hash_combine(h, v);
  return h;
}

}  // namespace engine::city

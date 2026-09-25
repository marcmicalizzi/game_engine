// The building grammar (docs/subsystems/city.md, "The building grammar", "Invariants"): one
// building per archetype pinned by a golden hash, the same buildings on any thread count, the LOD
// stages as prefixes of one another, a floor's spaces tiling its footprint, walls between exactly
// two spaces, the occupancy summary, and the description as a file.
#include "city_fixtures.h"

#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/city/city.h>

#include <doctest/doctest.h>

#include <cstdio>
#include <string>

using namespace engine;
using namespace engine::city;
using engine::city::test::default_plan;
using engine::city::test::small_plan;

namespace {

// One building per archetype on the default island at the golden seed — the first lot of each in
// E18's order — at the rooms stage. Integers and the engine's hash only, so the same on every
// toolchain; a change to the grammar moves the archetype it touches.
// Taken on Clang 18 and reproduced by GCC 13 on 2026-09-25; MSVC has not run it yet.
constexpr u64 k_building_golden[k_archetypes] = {
    0xdbeb1c94e39fee15ull,  // apartment tower
    0x3b39a82a3ca81610ull,  // mid-rise over shops
    0xc04440ab5b2cd6feull,  // office
    0x3545b2f1609a5f0full,  // terraced house
    0x172aa98645ad77ebull,  // detached house
    0x2b4244dfa638a6ccull,  // warehouse
    0x9340e1bc26baf73cull,  // civic shell
};

u32 first_of(const Plan& plan, Archetype a) {
  Vector<u32> lots;
  choose_yield_lots(plan, k_archetypes, lots);
  for (const u32 l : lots) {
    if (plan.lots[l].archetype == a) return l;
  }
  return k_no_id;
}

Building make(const Plan& plan, u32 lot, Stage stage) {
  Grammar grammar(plan);
  Building b;
  std::string error;
  const bool ok = grammar.generate(lot, stage, b, &error);
  REQUIRE_MESSAGE(ok, error);
  return b;
}

}  // namespace

TEST_CASE("city building: one building per archetype hashes to the committed number") {
  const Plan& plan = default_plan();
  for (u32 a = 0; a < k_archetypes; ++a) {
    const u32 lot = first_of(plan, static_cast<Archetype>(a));
    REQUIRE_MESSAGE(lot != k_no_id, std::string(archetype_name(static_cast<Archetype>(a))));
    const Building b = make(plan, lot, Stage::Rooms);
    const u64 h = hash_building(b);
    char text[19];
    std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(h));
    MESSAGE(std::string(archetype_name(b.archetype))
            << " lot " << plan.lots[lot].id << ": " << b.floors.size() << " floors, "
            << b.units.size() << " units, " << b.spaces.size() << " spaces, hash " << text);
    CHECK_MESSAGE(h == k_building_golden[a], std::string(archetype_name(b.archetype))
                                                 << ": this build's hash is " << text);
  }
}

TEST_CASE("city building: the same lot is the same building, alone or among many, on any pool") {
  const Plan& plan = small_plan();
  Vector<u32> lots;
  choose_yield_lots(plan, 60, lots);
  Vector<Building> one, pooled;
  std::string error;
  REQUIRE(generate_buildings(plan, std::span<const u32>(lots.data(), lots.size()), Stage::Rooms,
                             nullptr, one, &error));
  for (const u32 workers : {1u, 5u}) {
    jobs::JobSystem js(jobs::JobSystemConfig{
        .performance_workers = workers, .efficiency_workers = 1, .pin_threads = false});
    REQUIRE(generate_buildings(plan, std::span<const u32>(lots.data(), lots.size()), Stage::Rooms,
                               &js, pooled, &error));
    REQUIRE(pooled.size() == one.size());
    for (u32 i = 0; i < one.size(); ++i)
      CHECK(hash_building(pooled[i]) == hash_building(one[i]));
  }
  // A building made alone is the one made among the others.
  const Building alone = make(plan, lots[7], Stage::Rooms);
  CHECK(hash_building(alone) == hash_building(one[7]));
}

TEST_CASE("city building: each LOD stage is a prefix of the next") {
  const Plan& plan = default_plan();
  for (u32 a = 0; a < k_archetypes; ++a) {
    const u32 lot = first_of(plan, static_cast<Archetype>(a));
    const Building massing = make(plan, lot, Stage::Massing);
    const Building floors = make(plan, lot, Stage::Floors);
    const Building rooms = make(plan, lot, Stage::Rooms);
    // The massing is the same at every stage.
    for (const Building* b : {&floors, &rooms}) {
      CHECK(b->footprint == massing.footprint);
      CHECK(b->floors.size() == massing.floors.size());
      CHECK(b->height_cm() == massing.height_cm());
      CHECK(b->bay_x_cm == massing.bay_x_cm);
    }
    CHECK(massing.units.empty());
    CHECK(massing.walls.empty());
    // Cores, units and the occupancy are fixed by the floors stage.
    REQUIRE(floors.cores.size() == rooms.cores.size());
    for (u32 k = 0; k < floors.cores.size(); ++k)
      CHECK(floors.cores[k].rect == rooms.cores[k].rect);
    REQUIRE(floors.units.size() == rooms.units.size());
    for (u32 k = 0; k < floors.units.size(); ++k) {
      CHECK(floors.units[k].rect == rooms.units[k].rect);
      CHECK(floors.units[k].kind == rooms.units[k].kind);
    }
    CHECK(floors.occupancy.residents == rooms.occupancy.residents);
    CHECK(floors.walls.empty());
    CHECK(!rooms.walls.empty());
  }
  // The policy: rooms near, floors in the middle, massing far, never finer farther away.
  CHECK(stage_for_distance(0.5f) == Stage::Rooms);
  CHECK(stage_for_distance(3.0f) == Stage::Floors);
  CHECK(stage_for_distance(20.0f) == Stage::Massing);
  for (f32 d = 0.0f; d < 30.0f; d += 0.25f)
    CHECK(static_cast<u32>(stage_for_distance(d)) >=
          static_cast<u32>(stage_for_distance(d + 0.25f)));
}

TEST_CASE("city building: a floor's spaces tile its footprint, and walls part exactly two spaces") {
  const Plan& plan = small_plan();
  Vector<u32> lots;
  choose_yield_lots(plan, 40, lots);
  for (const u32 lot : lots) {
    const Building b = make(plan, lot, Stage::Rooms);
    for (u32 f = 0; f < b.floors.size(); ++f) {
      const Floor& fl = b.floors[f];
      i64 area = 0;
      for (u32 s = fl.first_space; s < fl.first_space + fl.space_count; ++s) {
        const Space& sp = b.spaces[s];
        CHECK(sp.floor == f);
        CHECK(sp.rect.width() > 0);
        CHECK(sp.rect.depth() > 0);
        CHECK(sp.rect.x0 >= b.footprint.x0);
        CHECK(sp.rect.x1 <= b.footprint.x1);
        CHECK(sp.rect.z0 >= b.footprint.z0);
        CHECK(sp.rect.z1 <= b.footprint.z1);
        area += sp.rect.area();
        for (u32 t = s + 1; t < fl.first_space + fl.space_count; ++t) {
          const Rect& q = b.spaces[t].rect;
          const bool apart =
              sp.rect.x1 <= q.x0 || q.x1 <= sp.rect.x0 || sp.rect.z1 <= q.z0 || q.z1 <= sp.rect.z0;
          CHECK_MESSAGE(apart, "lot " << b.lot_id << " floor " << f << ": spaces " << s << " and "
                                      << t << " overlap");
        }
      }
      // Disjoint and inside, with the footprint's area: they tile it.
      CHECK_MESSAGE(area == b.footprint.area(), "lot " << b.lot_id << " floor " << f);
      for (u32 w = fl.first_wall; w < fl.first_wall + fl.wall_count; ++w) {
        const Wall& wall = b.walls[w];
        CHECK(wall.floor == f);
        CHECK(wall.a != wall.b);
        CHECK((wall.x0 == wall.x1) != (wall.z0 == wall.z1));
        if (wall.kind == WallKind::Exterior) {
          const bool on_facade = wall.z0 == b.footprint.z0 || wall.z0 == b.footprint.z1 ||
                                 wall.x0 == b.footprint.x0 || wall.x0 == b.footprint.x1;
          CHECK(on_facade);
        }
      }
    }
    // Every opening lies inside its wall; every zone inside its room.
    for (const Opening& o : b.openings) {
      const Wall& w = b.walls[o.wall];
      const i32 len = (w.x1 - w.x0) + (w.z1 - w.z0);
      CHECK(o.at_cm - o.width_cm / 2 >= 0);
      CHECK(o.at_cm + o.width_cm / 2 <= len);
    }
    for (const Zone& z : b.zones) {
      const Rect& q = b.spaces[z.space].rect;
      CHECK(z.rect.x0 >= q.x0);
      CHECK(z.rect.x1 <= q.x1);
      CHECK(z.rect.z0 >= q.z0);
      CHECK(z.rect.z1 <= q.z1);
    }
    // The footprint on the plan is inside the lot.
    const Rect on_plan = footprint_on_plan(b);
    const Rect& lot_rect = plan.lots[lot].rect;
    CHECK(on_plan.x0 >= lot_rect.x0);
    CHECK(on_plan.x1 <= lot_rect.x1);
    CHECK(on_plan.z0 >= lot_rect.z0);
    CHECK(on_plan.z1 <= lot_rect.z1);
  }
}

TEST_CASE("city building: the occupancy summary counts homes and jobs") {
  const Plan& plan = default_plan();
  for (u32 a = 0; a < k_archetypes; ++a) {
    const Archetype arch = static_cast<Archetype>(a);
    const Building b = make(plan, first_of(plan, arch), Stage::Floors);
    const OccupancySummary& o = b.occupancy;
    u32 dwellings = 0, residents = 0, bedrooms = 0, jobs = 0;
    for (const Unit& u : b.units) {
      if (u.kind <= UnitKind::House) {
        ++dwellings;
        residents += u.residents;
        bedrooms += u.bedrooms;
        CHECK(u.residents > 0);
      } else {
        jobs += u.workplaces;
        CHECK(u.workplaces > 0);
      }
    }
    CHECK(o.dwellings == dwellings);
    CHECK(o.residents == residents);
    CHECK(o.bedrooms == bedrooms);
    u32 counted = 0;
    for (const u32 w : o.workplaces)
      counted += w;
    CHECK(counted == jobs);
    const bool homes = arch == Archetype::ApartmentTower || arch == Archetype::MidRiseOverShops ||
                       arch == Archetype::TerracedHouse || arch == Archetype::DetachedHouse;
    CHECK_MESSAGE((o.dwellings > 0) == homes, std::string(archetype_name(arch)));
    CHECK_MESSAGE(
        (counted > 0) == (arch != Archetype::ApartmentTower && arch != Archetype::TerracedHouse &&
                          arch != Archetype::DetachedHouse),
        std::string(archetype_name(arch)));
  }
}

TEST_CASE("city building: the description reads back from its file") {
  const Plan& plan = default_plan();
  const Building b = make(plan, first_of(plan, Archetype::MidRiseOverShops), Stage::Rooms);
  Report report;
  validate_building(plan, b, report);
  BuildingFile file;
  building_to_schema(plan, b, &report, file);
  CHECK(file.units.size() == b.units.size());
  CHECK(file.walls.size() == b.walls.size());
  CHECK(file.occupancy.residents == b.occupancy.residents);
  CHECK(!file.occupancy.hours.empty());  // its shops keep retail hours
  const std::string text = write_json(schema::to_json(file));
  JsonValue root;
  REQUIRE(parse_json(text, root).ok);
  BuildingFile back;
  schema::ReadContext ctx;
  REQUIRE(schema::from_json(back, root, ctx));
  CHECK(ctx.ok());
  CHECK(back == file);
}

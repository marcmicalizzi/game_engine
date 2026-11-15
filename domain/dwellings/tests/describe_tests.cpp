// A plan as a description (docs/subsystems/dwellings.md, "The description a plan becomes"): every
// house the filler writes passes the mesher's validator and meshes; the mesher's own graph reaches
// every room; its walk records hold the plan's doorways, doors and flight where the plan put them;
// and the descriptions are the same on every toolchain.

#include <core/hash/hash.h>
#include <domain/dwellings/describe.h>
#include <domain/dwellings/tour.h>
#include <domain/structures/structures.h>

#include <doctest/doctest.h>

#include <cstdio>
#include <string>

using namespace engine;
using namespace engine::dwellings;

namespace {

// Taken on MSVC (msvc-debug) on 2026-11-14 at k_interior_version 1, k_rural_filler_version 1 and
// k_structure_version 1.
constexpr u64 k_descriptions_pin = 0xf109f353ed345becull;

bool near_cm(f32 metres, i32 cm) {
  const f32 d = metres * 100.0f - static_cast<f32>(cm);
  return d > -0.01f && d < 0.01f;
}

}  // namespace

TEST_CASE("describe: every house the filler writes is one the mesher takes") {
  const RuralStyle style;
  const RuralPlot plots[3] = {RuralPlot{2400, 1500}, RuralPlot{760, 1500}, RuralPlot{1200, 520}};
  u64 pin = k_hash_seed;
  u32 most_triangles = 0;
  u32 houses = 0;
  for (const RuralPlot& plot : plots) {
    for (u8 persons = 1; persons <= 9; ++persons) {
      for (u8 means = 0; means <= 2; ++means) {
        for (u64 seed = 0; seed < 4; ++seed) {
          RuralChoice choice;
          InteriorPlan plan;
          std::string error;
          const u64 house_seed = seed * 131 + persons;
          INFO("plot " << plot.frontage_cm << " persons " << int{persons} << " means " << int{means}
                       << " seed " << seed);
          REQUIRE(
              plan_rural(plot, Household{persons, means}, style, house_seed, choice, plan, &error));
          structure::Description d;
          describe(plan, choice, "house", house_seed, d);
          std::string why;
          const bool valid = structures::validate(d, why);
          INFO(why);
          REQUIRE(valid);
          const i32 off = description_offset_cm(choice);

          // The mesher's own graph reaches every room, and has the plan's links.
          i32 unreached = -1;
          CHECK(structures::rooms_reachable(d, &unreached));
          Vector<structures::RoomLink> links;
          structures::room_links(d, links);
          u32 doors = 0;
          for (const OuterOpening& o : plan.openings)
            doors += o.door() ? 1u : 0u;
          CHECK(links.size() == plan.doorways.size() + plan.stairs.size() + doors);
          CHECK(d.rooms.size() == plan.rooms.size());
          CHECK(d.chimneys.size() == plan.hearths.size());

          // The walk records: every doorway where the plan put it, every flight the plan's.
          structures::Walk walk;
          REQUIRE(structures::walk_records(d, walk, why));
          u32 found = 0;
          for (const Doorway& door : plan.doorways) {
            const InnerWall& w = plan.walls[door.wall];
            const bool along_z = w.x0 == w.x1;
            const i32 mid = (along_z ? w.z0 : w.x0) + door.at_cm + off;
            const i32 line = (along_z ? w.x0 : w.z0) + off;
            for (const structures::WalkOpening& o : walk.openings) {
              if (o.kind != structure::OpeningKind::doorway) continue;
              const f32 along = along_z ? (o.a.y + o.b.y) * 0.5f : (o.a.x + o.b.x) * 0.5f;
              const f32 across = along_z ? o.a.x : o.a.y;
              const f32 width = along_z ? o.b.y - o.a.y : o.b.x - o.a.x;
              if (near_cm(across, line) && near_cm(along, mid) &&
                  near_cm(width < 0 ? -width : width, door.width_cm) &&
                  near_cm(o.head - o.sill, door.head_cm)) {
                ++found;
                break;
              }
            }
          }
          CHECK(found == plan.doorways.size());
          REQUIRE(walk.flights.size() == plan.stairs.size());
          CHECK(walk.wells.size() == plan.stairs.size());
          for (u32 i = 0; i < plan.stairs.size(); ++i) {
            const Stair& s = plan.stairs[i];
            const structures::WalkFlight& f = walk.flights[i];
            CHECK(f.risers == s.risers);
            CHECK(near_cm(f.going, k_stair_going_cm));
            CHECK(near_cm(f.top, s.rise_mm / 10));
            CHECK(f.rise * static_cast<f32>(f.risers) == doctest::Approx(f.top).epsilon(0.001));
            CHECK(f.rise <= 0.2101f);
            // The flight's foot is where the tour starts to climb.
            Vector<TourPoint> tour;
            REQUIRE(tour_interior(plan, 150, tour));
            bool on_tour = false;
            for (const TourPoint& t : tour) {
              on_tour =
                  on_tour || (near_cm(f.foot.x, t.x_cm + off) && near_cm(f.foot.z, t.z_cm + off));
            }
            CHECK(on_tour);
          }

          // It meshes, inside a budget (dwellings.md, "Budgets").
          structures::MeshPart parts[structures::k_surfaces];
          structures::Report report;
          REQUIRE(structures::solve(d, structure::MeshScope::whole, parts, report, why));
          CHECK(report.triangles > 300);
          CHECK(report.triangles < 6000);
          if (report.triangles > most_triangles) most_triangles = report.triangles;
          CHECK(report.surface_triangles[static_cast<u32>(structure::Surface::wall_inner)] > 0);
          if (!plan.stairs.empty()) {
            CHECK(report.surface_triangles[static_cast<u32>(structure::Surface::boards)] > 0);
            CHECK(report.surface_triangles[static_cast<u32>(structure::Surface::ceiling)] > 0);
          }

          structure::Recipe recipe;
          recipe.description = d;
          pin = hash_combine(pin, structures::source_hash(recipe));
          ++houses;
        }
      }
    }
  }
  CHECK(houses == 3 * 9 * 3 * 4);
  std::printf("dwellings: the largest of %u houses is %u triangles\n", houses, most_triangles);
  if (pin != k_descriptions_pin) {
    std::printf("dwellings description pin: 0x%016llxull\n", static_cast<unsigned long long>(pin));
  }
  CHECK(pin == k_descriptions_pin);
}

TEST_CASE("describe: fill_rural is the filler, and says why a plot holds nothing") {
  const RuralStyle style;
  structure::Description d;
  std::string error;
  REQUIRE(fill_rural(RuralPlot{2400, 1500}, Household{5, 2}, style, 7, "farmhouse", d, &error));
  CHECK(d.name == "farmhouse");
  CHECK(d.style == "rural/hall-parlour");
  CHECK(d.storeys.size() == 2);
  CHECK(d.stairs.size() == 1);
  CHECK(d.chimneys.size() == 2);
  CHECK(d.provenance.filler == "rural");
  CHECK(d.provenance.version == k_rural_filler_version);
  CHECK(d.provenance.seed == 7);
  // The front wall is the footprint's last, along z = 0, and holds the door.
  u32 front_doors = 0;
  for (const structure::Opening& o : d.openings) {
    if (o.kind == structure::OpeningKind::door && o.wall == 3) ++front_doors;
  }
  CHECK(front_doors == 1);

  CHECK_FALSE(fill_rural(RuralPlot{400, 300}, Household{2, 0}, style, 7, "none", d, &error));
  CHECK(error.rfind("rural filler: ", 0) == 0);
}

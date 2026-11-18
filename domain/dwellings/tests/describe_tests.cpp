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

// Taken on MSVC (msvc-debug) on 2026-11-14 at k_interior_version 2, k_rural_filler_version 2 and
// k_structure_version 2. Version 1's was 0xf109f353ed345bec: it moved because a stair's well ends
// where its flight does, a hearth's stone and timber are the mesher's and no longer two boxes, and
// a one-storey house's overhang is cut back to clear its door's head (0x722929a1172e24df then);
// and again at k_furnish_version 1, when a description gained its timber and furniture as boxes.
// And at k_structure_version 4 (0x3cbcd86acf0d2f28 before it), when the mesher's description
// gained leaves, mullions and walls that wander and the filler began to state them by the seed.
// And at k_structure_version 5 (0x6f53539871304142 before it): a thatch's courses and cut eave,
// the limewash's and the thatch's variants by the seed, and a batter of seven eighths.
// And on 2026-11-15 (0xbee0164e0fc3342f before it), for the village's walk and its pictures: a
// partition doorway made a door is 10 cm wider, its leaf never opens into a passage or a stair, a
// back door's leaf stands shut, and a roof states no courses and no cut eave. MSVC's alone until a
// container has run it. And on 2026-11-16 (0x05eaa3436b76acbf before it), at k_interior_version
// 3: a stair's well runs 30 cm past its first riser, so a walker on the first treads has its crown
// clear of the upper floor's edge (MSVC msvc-release; GCC in the container). And on 2026-11-16
// (0x19bf9170cb041fd7 before it), G208: two houses of sixteen are stone-faced, their outer walls'
// role `stone/rubble` in place of the limewash, by a draw of their own; and at
// k_rural_filler_version 3 a smaller plot never gives a household more storeys than it asked or
// than the plot allows (MSVC msvc-debug).
// And at G219 (2026-11-17; 0x1dbed3833524ec80 before): joists under a ceiled one-storey house's
// ceiling and tie beams in a cot, a one-hearth two-up two-down's fire in its kitchen, and an inner
// room for a cross-passage house of five or six on one storey (MSVC msvc-debug). And at S91
// (2026-11-18; 0xff3c25d2ee19f79d before): a description that hangs a leaf takes the "DOORS053" tag
// in its source hash, since its container now carries section 53; no description changed (MSVC
// msvc-debug alone until a container has run it).
constexpr u64 k_descriptions_pin = 0x679628bb629ad846ull;

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
              // A partition's doorway is a bare doorway or, with a leaf, a door.
              if (o.kind == structure::OpeningKind::window ||
                  d.walls[o.wall].kind != structure::WallKind::inner) {
                continue;
              }
              const f32 along = along_z ? (o.a.y + o.b.y) * 0.5f : (o.a.x + o.b.x) * 0.5f;
              const f32 across = along_z ? o.a.x : o.a.y;
              const f32 width = along_z ? o.b.y - o.a.y : o.b.x - o.a.x;
              // A doorway made a door is 10 cm wider than the plan's, for its frame and its leaf.
              const auto wide = static_cast<decltype(door.width_cm)>(
                  door.width_cm + (o.kind == structure::OpeningKind::door ? 10 : 0));
              if (near_cm(across, line) && near_cm(along, mid) &&
                  near_cm(width < 0 ? -width : width, wide) &&
                  near_cm(o.head - o.sill, door.head_cm)) {
                ++found;
                break;
              }
            }
          }
          CHECK(found == plan.doorways.size());
          // The furniture is in the walk records: a box for every solid member, and nothing else.
          u32 solid_boxes = 0;
          for (const structure::Box& box : d.boxes)
            solid_boxes += box.solid ? 1u : 0u;
          CHECK(walk.boxes.size() == solid_boxes);
          if (plan.params.type != PlanType::TwoUpTwoDown) CHECK(solid_boxes >= 1);
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
          CHECK(report.triangles < 16000);
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

TEST_CASE("describe: eight houses of one household are not one wall, one window and one door") {
  const RuralStyle style;
  u64 seen[8] = {};
  u32 distinct = 0, waves = 0, batters = 0, mullioned = 0, shuttered = 0, inner_doors = 0;
  for (u64 seed = 0; seed < 8; ++seed) {
    structure::Description d;
    std::string error;
    REQUIRE(fill_rural(RuralPlot{2400, 1500}, Household{4, 1}, style, seed + 40, "h", d, &error));
    u32 mullions = 0, shutters = 0;
    bool entrance = false;
    for (const structure::Opening& o : d.openings) {
      const structure::Wall& w = d.walls[o.wall];
      if (o.leaf != structure::LeafKind::none) {
        // A leaf hangs in a door or a window, open a quarter turn: the walk goes through it.
        CHECK(o.kind != structure::OpeningKind::doorway);
        CHECK(o.leaf_open == 90);
      }
      if (o.kind == structure::OpeningKind::door && w.kind == structure::WallKind::exterior) {
        // Every outer door has its leaf, opening inward (the wall's left is its room).
        CHECK(o.leaf == structure::LeafKind::ledged);
        CHECK_FALSE(o.opens_right);
        entrance = true;
      }
      if (o.kind == structure::OpeningKind::door && w.kind == structure::WallKind::inner) {
        ++inner_doors;
        CHECK(o.width - 12 >= 66);  // what the village's walker passes, inside the frame
      }
      if (o.kind == structure::OpeningKind::window) {
        mullions += o.mullions;
        shutters += o.leaf != structure::LeafKind::none ? 1u : 0u;
      }
    }
    CHECK(entrance);
    const structure::Wall& outer = d.walls[0];
    const u64 key = hash_combine(
        hash_combine(
            u64{static_cast<u32>(outer.wave)} << 32 | static_cast<u32>(outer.top_thickness),
            u64{static_cast<u32>(d.arris)} << 32 | static_cast<u32>(d.plinth_proud)),
        u64{mullions != 0 ? 1u : 0u} << 1 | (shutters != 0 ? 1u : 0u));
    bool fresh = true;
    for (u32 i = 0; i < distinct; ++i)
      fresh = fresh && seen[i] != key;
    if (fresh) seen[distinct++] = key;
    waves += outer.wave != 0 ? 1u : 0u;
    batters += outer.top_thickness != 0 ? 1u : 0u;
    mullioned += mullions != 0 ? 1u : 0u;
    shuttered += shutters != 0 ? 1u : 0u;
  }
  std::printf(
      "dwellings variety over 8 houses: %u distinct shells, %u battered, %u mullioned, "
      "%u shuttered, %u inner doors\n",
      distinct, batters, mullioned, shuttered, inner_doors);
  CHECK(distinct >= 5);
  CHECK(waves == 8);
  CHECK(batters >= 1);
  CHECK(batters < 8);
  CHECK(mullioned >= 1);
  CHECK(mullioned < 8);
  CHECK(shuttered >= 1);
  CHECK(shuttered < 8);

  // A plain shell states none of it.
  RuralStyle plain;
  plain.dressed = false;
  structure::Description d;
  std::string error;
  REQUIRE(fill_rural(RuralPlot{2400, 1500}, Household{4, 1}, plain, 40, "h", d, &error));
  CHECK(d.arris == 0);
  CHECK(d.plinth_proud == 0);
  for (const structure::Wall& w : d.walls) {
    CHECK(w.wave == 0);
    CHECK(w.top_thickness == 0);
  }
  for (const structure::Opening& o : d.openings) {
    CHECK(o.leaf == structure::LeafKind::none);
    CHECK(o.mullions == 0);
  }
}

TEST_CASE("describe: a village's limewash and thatch are not one colour and one age") {
  const RuralStyle style;
  u32 washes = 0, base_wash = 0, aged = 0, cut = 0, stone = 0;
  std::string seen_wash[6];
  for (u64 seed = 0; seed < 24; ++seed) {
    structure::Description d;
    std::string error;
    REQUIRE(fill_rural(RuralPlot{2400, 1500}, Household{4, 1}, style, seed + 400, "h", d, &error));
    REQUIRE(d.roofs.size() == 1);
    const structure::Roof& roof = d.roofs[0];
    // A role's variant falls back to its base where a pack lacks it, so any name is safe; these
    // are the names the generated sets answer.
    // A stone-faced house shows its rubble; every other is limewashed.
    const bool rubble = d.roles.wall_outer == "stone/rubble";
    stone += rubble ? 1u : 0u;
    CHECK((rubble || d.roles.wall_outer.rfind("plaster/lime", 0) == 0));
    CHECK((roof.role.empty() || roof.role.rfind("thatch/straw_", 0) == 0));
    // A smooth coat and a rolled eave: no courses and no cut eave are stated (describe.cpp, where
    // the roof is written: stepped courses read as slate over a whole street).
    CHECK(roof.course == 0);
    CHECK(roof.course_step_mm == 0);
    bool fresh = true;
    for (u32 i = 0; i < washes; ++i)
      fresh = fresh && seen_wash[i] != d.roles.wall_outer;
    if (fresh && washes < 6) seen_wash[washes++] = d.roles.wall_outer;
    base_wash += d.roles.wall_outer == "plaster/lime" ? 1u : 0u;
    aged += roof.role.empty() ? 0u : 1u;
    cut += roof.eave_cut ? 1u : 0u;
  }
  std::printf(
      "dwellings limewash over 24 houses: %u washes, %u the base; %u thatches not the "
      "base's age; %u eaves cut square; %u stone-faced\n",
      washes, base_wash, aged, cut, stone);
  // Two in sixteen, so a few of 24 and never most.
  CHECK(stone >= 1);
  CHECK(stone <= 8);
  CHECK(washes >= 3);
  CHECK(base_wash < 24);
  CHECK(aged >= 1);
  CHECK(aged < 24);
  CHECK(cut == 0);
}

TEST_CASE("describe: a door under an eave keeps its head clear of the eave's edge") {
  // The mesher refuses a way out under an eave lower than its head. A house of one storey has its
  // door on the storey the roof sits on: its overhang is cut back; a house of two keeps the
  // mesher's example's.
  const RuralStyle style;
  const i32 example = structures::example_cot().roofs[0].overhang;
  for (u8 persons = 1; persons <= 9; ++persons) {
    for (u8 means = 0; means <= 2; ++means) {
      structure::Description d;
      std::string error;
      INFO("persons " << int{persons} << " means " << int{means});
      const bool filled =
          fill_rural(RuralPlot{2400, 1500}, Household{persons, means}, style, 11, "h", d, &error);
      INFO(error);
      REQUIRE(filled);
      REQUIRE(d.roofs.size() == 1);
      const structure::Roof& roof = d.roofs[0];
      if (d.storeys.size() == 2) {
        CHECK(roof.overhang == example);
        continue;
      }
      CHECK(roof.overhang > 20);
      for (const structure::Opening& o : d.openings) {
        if (o.kind != structure::OpeningKind::door) continue;
        // The edge's height over the floor, a centimetre inside the edge as the validator samples.
        const i32 edge = roof.eaves - roof.pitch * (roof.overhang - 1) / 1000;
        CHECK(edge >= o.head);
        // The walker's clear height under the frame's head (the village's rule: 185 cm).
        CHECK(o.head - 8 >= 185);
      }
    }
  }
}

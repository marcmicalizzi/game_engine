// A cave network's plan (network.h; docs/subsystems/caves.md, "The plan"; roadmap G146): a
// network in the host rock with its openings found against the ground — every passage inside the
// soluble member by the stated margin, levels at successive base levels, loops and dead ends,
// mouths whose outlines are closed and on the ground, a hall that falls in under thin cover, the
// walker's clearance, the same plan whatever asks, and the refusals.
#include "network_test_support.h"

#include <core/schema/json_reflect.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>

using namespace engine;
using namespace engine::caves;

namespace {

Plan planned(const Network& n, const caves::test::Hill& hill, i64 tile_mm = 32000) {
  Plan plan;
  std::string error;
  REQUIRE_MESSAGE(plan_network(n, caves::test::ground_of(hill), tile_mm, plan, error), error);
  return plan;
}

bool inside_polygon(const Vector<OutlinePoint>& ring, i64 x, i64 z) {
  bool in = false;
  for (u32 i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
    const f64 xi = static_cast<f64>(ring[i].x), zi = static_cast<f64>(ring[i].z);
    const f64 xj = static_cast<f64>(ring[j].x), zj = static_cast<f64>(ring[j].z);
    if ((zi > static_cast<f64>(z)) != (zj > static_cast<f64>(z)) &&
        static_cast<f64>(x) < (xj - xi) * (static_cast<f64>(z) - zi) / (zj - zi) + xi)
      in = !in;
  }
  return in;
}

struct Opened {
  const scene_gen::PlacementGeneratorDesc* desc = nullptr;
  void* state = nullptr;
  ~Opened() {
    if (state != nullptr) desc->close(state);
  }
};

}  // namespace

TEST_CASE("caves: a network is laid in the host rock, at its levels, with loops and dead ends") {
  const caves::test::Hill hill;
  const Network n = caves::test::hill_network();
  const Plan plan = planned(n, hill);
  const PlanReport& r = plan.report;
  MESSAGE("the network: " << r.levels << " levels, " << r.strands << " strands, "
                          << plan.edges.size() << " edges, " << plan.nodes.size() << " nodes, "
                          << r.passage_m << " m of passage, " << r.walkable_m << " m walkable, "
                          << r.reachable_m << " m reached from a mouth; " << r.loops << " loops, "
                          << r.dead_ends << " dead ends; mouths " << r.mouths_found << " found, "
                          << r.mouths_led << " led, " << r.fissures << " fissures; "
                          << plan.cells.size() << " cells, " << plan.portals.size()
                          << " portals; base level " << r.base_level_m << " m, the least rock to "
                          << "the member's edge " << r.least_margin_m << " m; " << r.notes);
  CHECK(r.levels == 2);
  CHECK(r.base_level_m == doctest::Approx(0.0));  // the valley floor, found from the ground
  CHECK(r.loops >= 2);      // the two connectors between the levels, and the bypass
  CHECK(r.dead_ends >= 3);  // crawls, a sump, chokes
  CHECK(r.passage_m > 400.0f);
  CHECK(plan.mouths.size() >= 2);

  // Every passage lies inside the limestone by the margin: each station's section, across the
  // beds, checked against the formation's own coordinate (a centimetre for the whole millimetres).
  const f64 margin = static_cast<f64>(n.member_margin_m);
  const f64 cos_dip = std::cos(8.0 * 3.14159265358979 / 180.0);
  f64 least = 1e30;
  for (const PlanStation& ps : plan.stations) {
    const CellStation& s = ps.station;
    if (s.frame == SectionFrame::Shaft) continue;  // a shaft is a collapse, not a solution passage
    const f64 c = bed_coordinate(plan, s.x, s.y, s.z);
    const f64 up = s.b / 1000.0;
    const f64 down = std::max(s.b / 1000.0, -s.slot_bot / 1000.0 * cos_dip);
    least = std::min(least, std::min(r.member_top_m - (c + up), (c - down) - r.member_base_m));
  }
  CHECK(r.member_top_m - r.member_base_m == doctest::Approx(24.0));
  CHECK(least >= margin - 0.01);
  CHECK(least == doctest::Approx(r.least_margin_m).epsilon(0.01));

  // The levels: each trunk's stations lie about its level's height, the upper a spacing higher.
  f64 level_y[2] = {0.0, 0.0};
  u32 level_n[2] = {0, 0};
  for (const PlanEdge& e : plan.edges) {
    if (e.kind != StrandKind::trunk) continue;
    for (u32 j = 0; j < e.count; ++j) {
      level_y[e.level] += static_cast<f64>(plan.stations[e.first + j].station.y) / 1000.0;
      ++level_n[e.level];
    }
  }
  REQUIRE(level_n[0] > 0);
  REQUIRE(level_n[1] > 0);
  CHECK(level_y[1] / level_n[1] - level_y[0] / level_n[0] ==
        doctest::Approx(static_cast<f64>(n.level_spacing_m)).epsilon(0.15));
  // The trunks run along strike (z here), the feeders along dip (x).
  for (const PlanEdge& e : plan.edges) {
    if (e.kind != StrandKind::trunk && e.kind != StrandKind::feeder) continue;
    if (e.count < 12) continue;
    const CellStation& a = plan.stations[e.first].station;
    const CellStation& b = plan.stations[e.first + e.count - 1].station;
    const f64 dx = std::fabs(static_cast<f64>(b.x - a.x)),
              dz = std::fabs(static_cast<f64>(b.z - a.z));
    if (e.kind == StrandKind::trunk) {
      CHECK(dz > dx);
    } else {
      CHECK(dx > 0.6 * dz);
    }
  }
}

TEST_CASE("caves: a network's openings are found against the ground") {
  const caves::test::Hill hill;
  const Network n = caves::test::hill_network();
  const Plan plan = planned(n, hill);
  REQUIRE(plan.mouths.size() >= 2);
  // The lowest trunk runs out of the hill where the side valley cuts it: nobody declared that.
  CHECK(plan.report.mouths_found >= 1);
  CHECK(plan.report.mouths_led >= 1);
  const f64 step = static_cast<f64>(plan.voxel_mm);
  for (const PlanMouth& m : plan.mouths) {
    MESSAGE("a mouth at (" << m.x / 1000.0 << ", " << m.y / 1000.0 << ", " << m.z / 1000.0
                           << "): " << m.outline.size() << " points, " << m.area_m2
                           << " m2, the ground's slope " << m.slope << ", " << m.plan_depth_m
                           << " m deep in plan" << std::string(m.pit ? ", a pit" : ""));
    REQUIRE(m.outline.size() >= 8);
    CHECK(m.area_m2 > 1.0f);
    for (u32 k = 0; k < m.outline.size(); ++k) {
      const OutlinePoint& p = m.outline[k];
      const OutlinePoint& q = m.outline[(k + 1) % m.outline.size()];
      // Closed: every point a lattice cell at most from the next, the last from the first.
      const f64 gap = std::hypot(static_cast<f64>(q.x - p.x), static_cast<f64>(q.z - p.z));
      CHECK(gap <= 1.5 * step);
      // On the ground, to the lattice's interpolation.
      CHECK(std::fabs(static_cast<f64>(p.y) / 1000.0 - caves::test::hill_height(&hill, p.x, p.z)) <
            0.05);
      // Inside the hole the terrain is asked for.
      CHECK(inside_polygon(m.hole, p.x, p.z));
    }
    CHECK(inside_polygon(m.outline, m.x, m.z));
  }
  // A network nobody can enter is said to be sealed.
  Network buried = n;
  buried.openings = 0;
  caves::test::Hill plateau;
  plateau.slope = 0.0;
  plateau.valley = 40.0;
  buried.base_level_auto = false;
  buried.base_level_m = 0.0;
  const Plan sealed = planned(buried, plateau);
  CHECK(sealed.mouths.empty());
  CHECK(sealed.report.notes.find("sealed") != std::string::npos);
}

TEST_CASE("caves: a hall under too little cover has fallen in") {
  const caves::test::Hill hill;
  const Network n = caves::test::hill_network();
  const Plan plan = planned(n, hill);
  REQUIRE(plan.halls.size() == 1);
  const PlanHall& hall = plan.halls[0];
  MESSAGE("the hall: span " << hall.span_m << " m, " << hall.cover_m << " m of cover");
  CHECK_FALSE(hall.fallen);
  CHECK(plan.report.fissures == 0);
  // The same network under a doline that leaves the dome two metres of rock: a fissure.
  caves::test::Hill sunk = hill;
  sunk.doline_x = static_cast<f64>(hall.hall.x) / 1000.0;
  sunk.doline_z = static_cast<f64>(hall.hall.z) / 1000.0;
  sunk.radius = 12.0;
  sunk.depth = static_cast<f64>(hall.cover_m) - 2.0;
  const Plan fallen = planned(n, sunk);
  REQUIRE(fallen.halls.size() == 1);
  CHECK(fallen.halls[0].cover_m == doctest::Approx(2.0).epsilon(0.05));
  CHECK(fallen.halls[0].fallen);
  CHECK(fallen.report.fissures == 1);
  CHECK(fallen.mouths.size() == plan.mouths.size() + 1);
  bool pit = false;
  for (const PlanMouth& m : fallen.mouths)
    pit = pit || m.pit;
  CHECK(pit);
}

TEST_CASE("caves: the plan's clearance says where a walker passes") {
  const caves::test::Hill hill;
  const Network n = caves::test::hill_network();
  const Plan plan = planned(n, hill);
  u32 walkable = 0, crawls = 0;
  for (const PlanEdge& e : plan.edges) {
    for (u32 j = 0; j < e.count; ++j) {
      const PlanStation& ps = plan.stations[e.first + j];
      if (e.walkable) {
        CHECK(ps.passable);
        CHECK(ps.clear_h_mm >= 2050);  // 1.8 m and a quarter
        CHECK(ps.clear_w_mm >= 800);   // 0.6 m and a fifth
      }
    }
    if (e.walkable) ++walkable;
    if (e.kind == StrandKind::crawl) {
      ++crawls;
      CHECK_FALSE(e.walkable);
    }
    if (e.kind == StrandKind::sump) CHECK_FALSE(e.walkable);
  }
  CHECK(walkable >= 6);
  CHECK(crawls >= 1);
  // A walker gets in: most of what is walkable is reached from a mouth.
  CHECK(plan.report.reachable_m > 0.6f * plan.report.walkable_m);
}

TEST_CASE("caves: the plan is the same whatever asks, and pinned") {
  const caves::test::Hill hill;
  const Network n = caves::test::hill_network();
  const Plan a = planned(n, hill), b = planned(n, hill);
  CHECK(plan_hash(a) == plan_hash(b));
  // The world's tile size files the cells and moves nothing.
  const Plan wide = planned(n, hill, 64000);
  REQUIRE(wide.stations.size() == a.stations.size());
  bool same = true;
  for (u32 i = 0; i < a.stations.size(); ++i)
    same = same && a.stations[i].station.x == wide.stations[i].station.x &&
           a.stations[i].station.y == wide.stations[i].station.y &&
           a.stations[i].station.z == wide.stations[i].station.z;
  CHECK(same);
  CHECK(plan_hash(planned(caves::test::hill_network(12), hill)) != plan_hash(a));
  // Taken on MSVC; Clang and GCC in the containers reproduce it (the solver rules, 1).
  MESSAGE("plan hash " << plan_hash(a));
  CHECK(plan_hash(a) == 6708698286208108111ull);

  // Through the registry: the tiles are the whole read, in either order, and a far ring keeps
  // only the cells that hold a mouth.
  engine::test::TempDir dir("caves");
  scene_gen::Context context;
  context.tile_size = 32.0f;
  context.ddc = dir.path();
  context.where = "test: caves 0";
  context.ground = caves::test::ground_of(hill);
  Opened g;
  g.desc = scene_gen::GeneratorRegistry::global().find_placement("caves");
  REQUIRE(g.desc != nullptr);
  std::string error;
  REQUIRE_MESSAGE(g.desc->open(schema::to_json(n), context, &g.state, &error), error);
  Vector<scene_gen::PlacementMesh> meshes;
  REQUIRE(g.desc->meshes(g.state, context, meshes, &error));
  u32 meshed = 0;
  for (const PlanCell& c : a.cells)
    if (!c.flooded) ++meshed;
  CHECK(meshes.size() == 2 * meshed);  // a cell's rock and its dressing
  for (const auto& m : meshes)
    CHECK(io::exists(m.path));
  scene_gen::Placements whole;
  REQUIRE(g.desc->expand(g.state, context, whole, &error));
  REQUIRE(whole.instances.size() == meshes.size());
  for (const bool backwards : {false, true}) {
    Vector<u8> seen(whole.instances.size() + 1, u8{0});
    for (u32 k = 0; k < 16 * 16; ++k) {
      const u32 at = backwards ? 16 * 16 - 1 - k : k;
      const scene_gen::TileCoord t{static_cast<i32>(at % 16) - 8, static_cast<i32>(at / 16) - 8};
      scene_gen::Placements out;
      REQUIRE(g.desc->tile(g.state, t, context, out, &error));
      CHECK(out.instances.empty() == !g.desc->occupies(g.state, t));
      for (const auto& p : out.instances) {
        REQUIRE(p.id >= 1);
        REQUIRE(p.id <= whole.instances.size());
        ++seen[p.id];
        const auto& w = whole.instances[p.id - 1];
        CHECK(p.position.x == w.position.x);
        CHECK(p.position.y == w.position.y);
        CHECK(p.position.z == w.position.z);
        CHECK(p.mesh == w.mesh);
      }
    }
    for (u32 id = 1; id <= whole.instances.size(); ++id)
      CHECK(seen[id] == 1);
  }
  scene::WorldRings rings;
  context.world = &rings;
  context.ring = 3;
  u32 far_count = 0, mouth_cells = 0;
  for (const PlanCell& c : a.cells)
    if (c.mouth) ++mouth_cells;
  for (u32 k = 0; k < 16 * 16; ++k) {
    const scene_gen::TileCoord t{static_cast<i32>(k % 16) - 8, static_cast<i32>(k / 16) - 8};
    scene_gen::Placements out;
    REQUIRE(g.desc->tile(g.state, t, context, out, &error));
    far_count += out.instances.size();
  }
  CHECK(mouth_cells >= 2);
  CHECK(far_count == 2 * mouth_cells);
}

TEST_CASE("caves: a network's refusals name the field") {
  const caves::test::Hill hill;
  Plan plan;
  std::string error;
  Network thin = caves::test::hill_network();
  thin.member.clear();
  thin.member_thickness_m = 6.0f;
  thin.member_margin_m = 2.5f;
  CHECK_FALSE(plan_network(thin, caves::test::ground_of(hill), 32000, plan, error));
  CHECK(error.find("too thin") != std::string::npos);
  Network unknown = caves::test::hill_network();
  unknown.member = "marble";
  CHECK_FALSE(plan_network(unknown, caves::test::ground_of(hill), 32000, plan, error));
  CHECK(error.find("marble") != std::string::npos);
  Network coarse = caves::test::hill_network();
  coarse.voxel_m = 0.3f;
  CHECK_FALSE(plan_network(coarse, caves::test::ground_of(hill), 32000, plan, error));
  CHECK(error.find("voxel_m") != std::string::npos);
  Network apron = caves::test::hill_network();
  apron.apron_m = 2.0f;
  CHECK_FALSE(plan_network(apron, caves::test::ground_of(hill), 32000, plan, error));
  CHECK(error.find("apron_m") != std::string::npos);
  // With no formation the beds are level, and a level that is not in the member is left out.
  Network level = caves::test::hill_network();
  level.formation.reset();
  level.member.clear();
  level.member_thickness_m = 14.0f;
  level.origin = WorldPos{40.0, 2.0, 0.0};
  REQUIRE_MESSAGE(plan_network(level, caves::test::ground_of(hill), 32000, plan, error), error);
  CHECK(plan.report.levels == 1);
  CHECK(plan.report.notes.find("left out") != std::string::npos);
}

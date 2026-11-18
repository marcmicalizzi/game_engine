// The village capability (docs/subsystems/village.md): the regular plan at irregularity 0 and the
// grown one above it, ways found round a hill, the green, the order plots are taken in, cottages
// and back lanes, the plots a slope and water leave empty, the pads, treads and paths as the tile
// set writes them, the shapes, the wear, the boundaries, the house's turn, the plan's determinism,
// and the refusals.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/scene_gen/exclusion.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/structures/structures.h>
#include <domain/tileset/cuts.h>
#include <domain/village/village.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::village;

namespace {

// Grounds of products and sums, so the heights are the same bits on every toolchain.
f64 flat(const void*, i64, i64) noexcept { return 2.0; }
// A gentle rise toward +z, 4 %, with a slight cross fall: what a village would be built on.
f64 gentle(const void*, i64 x, i64 z) noexcept {
  return 0.04 * static_cast<f64>(z) / 1000.0 + 0.01 * static_cast<f64>(x) / 1000.0;
}
// Flat south of z = 0 and a 60 % bank north of z = 6 m: the left row's houses would stand on it.
f64 bank(const void*, i64, i64 z) noexcept {
  const f64 zz = static_cast<f64>(z) / 1000.0;
  return zz > 6.0 ? 0.6 * (zz - 6.0) : 0.0;
}
// A round hill 12 m high and 35 m across at the origin, on level ground.
f64 hill(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  const f64 r2 = xx * xx + zz * zz;
  return r2 < 306.25 ? 12.0 * (1.0 - r2 / 306.25) : 0.0;
}

scene_gen::Ground ground(scene_gen::HeightFn fn) {
  scene_gen::Ground g;
  g.surface_fn = fn;
  return g;
}

VillageLayer street(f64 x0, f64 z0, f64 x1, f64 z1) {
  VillageLayer p;
  p.name = "test";
  p.seed = 11;
  Lane lane;
  lane.name = "street";
  lane.waypoints.push_back(WorldPos{x0, 0.0, z0});
  lane.waypoints.push_back(WorldPos{x1, 0.0, z1});
  p.lanes.push_back(lane);
  return p;
}
// The same, laid out: no wander, no gaps, no turn, no green, no church, no back lane.
VillageLayer regular(VillageLayer p) {
  p.irregularity = 0.0f;
  p.green_m = 0.0f;
  p.church = false;
  p.back_lanes = false;
  return p;
}

VillagePlan planned(const VillageLayer& p, scene_gen::HeightFn fn, BoxHouse* box = nullptr,
                    const scene_gen::Exclusion* exclusion = nullptr) {
  scene_gen::Context context;
  context.ground = ground(fn);
  context.exclusion = exclusion;
  VillagePlan plan;
  std::string error;
  REQUIRE_MESSAGE(plan_village(p, context.ground, context, &box_house, box, plan, error), error);
  return plan;
}

// The signed distance of a world point (mm) from the lane's line, positive on its left.
f64 left_of(const VillageLayer& p, i64 x, i64 z) {
  const WorldPos a = p.lanes[0].waypoints[0], b = p.lanes[0].waypoints[1];
  const f64 dx = b.x - a.x, dz = b.z - a.z, len = std::sqrt(dx * dx + dz * dz);
  return ((static_cast<f64>(x) / 1000.0 - a.x) * (-dz) +
          (static_cast<f64>(z) / 1000.0 - a.z) * dx) /
         len;
}

// Whether a world point (mm) stands inside a plot's rectangle, `in_mm` in from its edges.
bool in_plot(const Plot& q, f64 x, f64 z, f64 in_mm) {
  const f64 rx = x - static_cast<f64>(q.front_x_mm), rz = z - static_cast<f64>(q.front_z_mm);
  const f64 u = rx * q.along_x + rz * q.along_z, v = rx * q.inward_x + rz * q.inward_z;
  return std::fabs(u) < static_cast<f64>(q.frontage_mm) / 2.0 - in_mm && v > in_mm &&
         v < static_cast<f64>(q.depth_mm) - in_mm;
}

// How many points of a 1 m lattice over each plot fall inside another plot.
u32 overlaps(const VillagePlan& plan) {
  u32 n = 0;
  for (u32 a = 0; a < plan.plots.size(); ++a) {
    const Plot& q = plan.plots[a];
    for (f64 u = -static_cast<f64>(q.frontage_mm) / 2 + 500;
         u < static_cast<f64>(q.frontage_mm) / 2; u += 1000)
      for (f64 v = 500; v < static_cast<f64>(q.depth_mm); v += 1000) {
        const f64 x = static_cast<f64>(q.front_x_mm) + q.along_x * u + q.inward_x * v;
        const f64 z = static_cast<f64>(q.front_z_mm) + q.along_z * u + q.inward_z * v;
        for (u32 b = 0; b < plan.plots.size(); ++b)
          if (b != a && in_plot(plan.plots[b], x, z, 1.0)) ++n;
      }
  }
  return n;
}

// How many points of the ways' treads fall inside a plot.
u32 treads_in_plots(const VillagePlan& plan) {
  u32 n = 0;
  for (const LaneLine& lane : plan.lanes)
    for (const LaneStation& s : lane.stations)
      for (const Plot& q : plan.plots)
        if (in_plot(q, static_cast<f64>(s.x_mm), static_cast<f64>(s.z_mm), 1.0)) ++n;
  return n;
}

}  // namespace

TEST_CASE("village: at irregularity 0 a way on level ground is a row plan of whole perches") {
  VillageLayer p = regular(street(0, 0, 120, 0));
  const VillagePlan plan = planned(p, flat);
  CHECK(plan.built == 8);
  CHECK(std::string(plan.size_class) == "village");
  CHECK(plan.steep + plan.wet + plan.no_fit + plan.refused + plan.outside == 0);
  CHECK(plan.cottages == 0);
  CHECK(!plan.church);
  CHECK(!plan.well);
  REQUIRE(plan.lanes.size() == 1);
  // The way found between two places on level ground is the straight line.
  CHECK(plan.lanes[0].length_mm == 120000);
  CHECK(plan.lanes[0].stations.size() == 61);  // every 2 m, both ends
  for (const LaneStation& s : plan.lanes[0].stations) {
    CHECK(s.top_mm == 2000);
    CHECK(s.z_mm == 0);
    CHECK(s.left_mm == 3050);
    CHECK(s.right_mm == 3050);
  }
  u32 left = 0, right = 0;
  for (const Plot& q : plan.plots) {
    CHECK(q.kind == PlotKind::tenement);
    // Whole perches, two to four, the entry's depth, square to the street.
    CHECK(q.frontage_mm % 5030 == 0);
    CHECK(q.frontage_mm >= 2 * 5030);
    CHECK(q.frontage_mm <= 4 * 5030);
    CHECK(q.depth_mm == 8 * 5030);
    CHECK(std::fabs(std::fabs(q.along_x) - 1.0) < 1e-12);
    // The plot's front on its street edge, 3.05 m from the lane's line, on its own side.
    const f64 side = left_of(p, q.front_x_mm, q.front_z_mm);
    CHECK(std::fabs(std::fabs(side) - 3.05) < 0.0011);
    CHECK((side > 0) == (q.side == 0));
    if (q.state != PlotState::built) continue;
    (q.side == 0 ? left : right) += 1;
    // The door faces the street: its threshold is the house's nearest point to the lane, the
    // setback behind the plot's front.
    const f64 door = std::fabs(left_of(p, q.door_x_mm, q.door_z_mm));
    CHECK(door == doctest::Approx(3.05 + 2.0).epsilon(1e-4));
    const f64 middle = std::fabs(left_of(p, q.house_x_mm, q.house_z_mm));
    CHECK(middle > door);
    CHECK(q.pad_mm == 2000);
    CHECK(q.storeys == 2);
    // The gate on the plot's front, square out of the door; the path's end on the tread's edge.
    CHECK(std::fabs(left_of(p, q.gate_x_mm, q.gate_z_mm)) == doctest::Approx(3.05).epsilon(1e-3));
    CHECK(std::fabs(left_of(p, q.path_x_mm, q.path_z_mm)) == doctest::Approx(1.5).epsilon(1e-3));
  }
  // Each row's plots follow each other along the street without overlap or gap, out from the
  // middle both ways.
  for (u8 side = 0; side < 2; ++side) {
    std::vector<const Plot*> row;
    for (const Plot& q : plan.plots)
      if (q.side == side) row.push_back(&q);
    std::sort(row.begin(), row.end(),
              [](const Plot* a, const Plot* b) { return a->along_mm < b->along_mm; });
    for (usize k = 1; k < row.size(); ++k)
      CHECK(row[k]->along_mm - row[k]->frontage_mm / 2 ==
            row[k - 1]->along_mm + row[k - 1]->frontage_mm / 2);
  }
  CHECK(overlaps(plan) == 0);
  // The rows alternate out from the middle, so eight houses stand four a side, give or take one.
  CHECK(left + right == 8);
  CHECK(std::max(left, right) - std::min(left, right) <= 2);
  MESSAGE("120 m of street: " << plan.plots.size() << " plots, " << left << " houses left, "
                              << right << " right");

  // Fewer than six houses is a hamlet; one row keeps to its side.
  p.houses = 4;
  p.lanes[0].rows = Rows::Right;
  const VillagePlan hamlet = planned(p, flat);
  CHECK(hamlet.built == 4);
  CHECK(std::string(hamlet.size_class) == "hamlet");
  for (const Plot& q : hamlet.plots)
    CHECK(q.side == 1);
}

TEST_CASE("village: a way is found round a hill, not ruled over it") {
  // Two places either side of a 12 m hill: a ruled lane would climb it at 70 %.
  VillageLayer p = regular(street(-60, 0, 60, 0));
  p.houses = 2;
  const VillagePlan plan = planned(p, hill);
  REQUIRE(plan.lanes.size() == 1);
  const LaneLine& way = plan.lanes[0];
  i64 widest = 0, highest = 0;
  for (const LaneStation& s : way.stations) {
    widest = std::max(widest, s.z_mm < 0 ? -s.z_mm : s.z_mm);
    highest = std::max(highest, s.top_mm);
  }
  MESSAGE("round the hill: " << way.length_mm << " mm, " << widest << " mm off the line at most, "
                             << highest << " mm up at most");
  CHECK(widest > 8000);           // it went round
  CHECK(highest < 4000);          // and stayed off the hill's shoulder
  CHECK(way.length_mm < 160000);  // without a long detour
  // Its ends are the two places.
  CHECK(way.stations[0].x_mm == -60000);
  CHECK(way.stations.back().x_mm == 60000);
}

namespace {
// The longest stretch of a way whose stations all stand within `off_mm` of the chord between its
// two ends, metres: how long the way runs straight.
f64 longest_straight(const LaneLine& way, f64 off_mm) {
  f64 longest = 0.0;
  const u32 n = way.stations.size();
  u32 j = 0;
  for (u32 i = 0; i < n; ++i) {
    j = std::max(j, i + 1);
    while (j + 1 < n) {
      const LaneStation& a = way.stations[i];
      const LaneStation& b = way.stations[j + 1];
      const f64 ex = static_cast<f64>(b.x_mm - a.x_mm), ez = static_cast<f64>(b.z_mm - a.z_mm);
      const f64 el = std::sqrt(ex * ex + ez * ez);
      bool straight = el > 0.0;
      for (u32 k = i + 1; k <= j && straight; ++k) {
        const f64 d = (static_cast<f64>(way.stations[k].x_mm - a.x_mm) * ez -
                       static_cast<f64>(way.stations[k].z_mm - a.z_mm) * ex) /
                      el;
        straight = std::fabs(d) <= off_mm;
      }
      if (!straight) break;
      ++j;
    }
    if (j < n) {
      const f64 dx = static_cast<f64>(way.stations[j].x_mm - way.stations[i].x_mm);
      const f64 dz = static_cast<f64>(way.stations[j].z_mm - way.stations[i].z_mm);
      longest = std::max(longest, std::sqrt(dx * dx + dz * dz) / 1000.0);
    }
  }
  return longest;
}
}  // namespace

TEST_CASE("village: a grown way goes round what it went round, and is never long straight") {
  // Two places 300 m apart on level ground: a row plan's way is the line between them; a grown
  // one goes round the holdings, the wet places and the copses that stood when it was first
  // trodden (the cost's seeded discs), so it runs straight for no more than a few tens of metres,
  // and its tread's width wanders.
  VillageLayer p = street(0, 0, 300, 0);
  p.houses = 2;
  const VillagePlan ruled = planned(regular(p), flat);
  CHECK(longest_straight(ruled.lanes[0], 250.0) == doctest::Approx(300.0));
  p.irregularity = 1.0f;
  const VillagePlan grown = planned(p, flat);
  const LaneLine& way = grown.lanes[0];
  const f64 straight = longest_straight(way, 500.0);
  i64 lo = way.stations[0].tread_mm, hi = lo, off = 0;
  for (const LaneStation& s : way.stations) {
    lo = std::min(lo, s.tread_mm);
    hi = std::max(hi, s.tread_mm);
    off = std::max(off, s.z_mm < 0 ? -s.z_mm : s.z_mm);
  }
  MESSAGE("grown way: " << way.length_mm << " mm long (sinuosity "
                        << static_cast<f64>(way.length_mm) / 300000.0 << "), " << off
                        << " mm off the line at most, straight for " << straight
                        << " m at most (50 cm), tread half width " << lo << " to " << hi);
  CHECK(straight < 60.0);
  CHECK(off > 4000);
  CHECK(way.length_mm < 345000);  // sinuosity under 1.15
  CHECK(hi - lo > 300);
}

TEST_CASE("village: a grown village is tofts side by side, uneven, subdivided, with cottages") {
  VillageLayer p = street(0, 0, 200, 30);
  p.houses = 16;
  p.irregularity = 1.0f;
  const VillagePlan plan = planned(p, gentle);
  CHECK(plan.built == 16);
  u32 whole = 0, tenements = 0, beside = 0;
  std::vector<i64> depths, frontages;
  // A plot's front corners, u = -half and +half.
  const auto corner = [](const Plot& q, f64 u, i64& x, i64& z) {
    x = q.front_x_mm + scene_gen::nearest_mm(q.along_x * u / 1000.0);
    z = q.front_z_mm + scene_gen::nearest_mm(q.along_z * u / 1000.0);
  };
  for (const Plot& q : plan.plots) {
    if (q.kind != PlotKind::tenement) continue;
    ++tenements;
    whole += q.frontage_mm % 5030 == 0 ? 1u : 0u;
    depths.push_back(q.depth_mm);
    frontages.push_back(q.frontage_mm);
    // Side by side: a front corner of this toft is a front corner of another, to 2 cm.
    bool shares = false;
    for (const Plot& o : plan.plots) {
      if (&o == &q) continue;
      for (const f64 su : {-0.5, 0.5})
        for (const f64 so : {-0.5, 0.5}) {
          i64 ax = 0, az = 0, bx = 0, bz = 0;
          corner(q, su * static_cast<f64>(q.frontage_mm), ax, az);
          corner(o, so * static_cast<f64>(o.frontage_mm), bx, bz);
          shares = shares || (std::abs(ax - bx) <= 20 && std::abs(az - bz) <= 20);
        }
    }
    beside += shares ? 1u : 0u;
  }
  std::sort(depths.begin(), depths.end());
  std::sort(frontages.begin(), frontages.end());
  MESSAGE("grown: " << plan.plots.size() << " plots, " << tenements << " tenements, "
                    << plan.cottages << " cottages built, " << beside << " beside another, "
                    << whole << " of whole perches, frontages " << frontages.front() << " to "
                    << frontages.back() << ", depths " << depths.front() << " to "
                    << depths.back());
  CHECK(whole * 4 < tenements);  // most frontages are no whole number of perches
  CHECK(depths.back() - depths.front() > 8000);
  // Gaps are the exception: one tenement in seven is followed by one at `irregularity` 1, and a
  // toft the street crosses round a bend or the green's lens is not set out; three in five stand
  // beside another (two in three before G208, when a toft too narrow to halve into two houses'
  // widths was halved anyway and its halves stood beside each other).
  CHECK(beside * 5 >= tenements * 3);
  // Narrow on the street: the middle toft under 2.5 perches, subdivided ones down to what the
  // filler's least house takes with its side gaps (6.6 m along the way since G208; 1.1 perches,
  // 5.53 m, before). A toft's frontage is the chord of the street's wandering edge, which on the
  // inside of a bend or a wander is less: 6.05 m here, where the filler's least house is refused
  // and counted (village.md, "Houses asked and built").
  CHECK(frontages[frontages.size() / 2] < 12575);
  CHECK(frontages.front() >= 5000);
  CHECK(plan.cottages > 0);
  CHECK(overlaps(plan) == 0);
  CHECK(treads_in_plots(plan) == 0);
  // A cottage asks one storey, a tenement the entry's.
  for (const Plot& q : plan.plots) {
    if (q.state != PlotState::built) continue;
    CHECK(q.storeys == (q.kind == PlotKind::cottage ? 1u : p.storeys));
  }
}

TEST_CASE("village: plots are taken nearest the green first, and the church stands further out") {
  // (The street runs on 160 m past the meeting, so the church has room at the village's end.)
  VillageLayer p = street(0, 0, 240, 0);
  Lane second;
  second.name = "to the mill";
  second.waypoints.push_back(WorldPos{70.0, 0.0, -90.0});
  second.waypoints.push_back(WorldPos{80.0, 0.0, -4.0});
  p.lanes.push_back(second);
  p.houses = 14;
  const VillagePlan plan = planned(p, gentle);
  CHECK(plan.built == 14);
  // The second way's end meets the first: the focus is a station of the first way (which goes round
  // what it went round, so within 20 m of the line between its places), the second way ends on it,
  // and the green and the well stand at it.
  CHECK(std::fabs(static_cast<f64>(plan.focus_x_mm) - 80000.0) < 12000.0);
  CHECK(std::fabs(static_cast<f64>(plan.focus_z_mm)) < 20000.0);
  CHECK(plan.lanes[1].stations.back().x_mm == plan.focus_x_mm);
  CHECK(plan.lanes[1].stations.back().z_mm == plan.focus_z_mm);
  CHECK(plan.well);
  u32 nearest = 0;
  for (u32 k = 0; k < plan.lanes[0].stations.size(); ++k)
    if (plan.lanes[0].stations[k].x_mm == plan.focus_x_mm &&
        plan.lanes[0].stations[k].z_mm == plan.focus_z_mm)
      nearest = k;
  const LaneStation& at = plan.lanes[0].stations[nearest];
  CHECK(at.left_mm > 10000);
  CHECK(at.right_mm > 10000);
  CHECK(plan.lanes[0].stations[2].left_mm < 5000);  // the street, far from the green
  // The houses' order: the first tenements nearer the focus than the last.
  const auto from_focus = [&](const Plot& q) {
    const f64 dx = static_cast<f64>(q.front_x_mm - plan.focus_x_mm),
              dz = static_cast<f64>(q.front_z_mm - plan.focus_z_mm);
    return std::sqrt(dx * dx + dz * dz);
  };
  std::vector<f64> built;
  u32 order = 0;
  for (const Plot& q : plan.plots) {
    CHECK(q.order == order++);
    if (q.state == PlotState::built && q.kind == PlotKind::tenement) built.push_back(from_focus(q));
  }
  REQUIRE(built.size() >= 6);
  f64 first = 0.0, last = 0.0;
  for (usize k = 0; k < 3; ++k) {
    first += built[k];
    last += built[built.size() - 1 - k];
  }
  CHECK(first < last);
  std::sort(built.begin(), built.end());
  // The church's plot is reserved, 42 by 24 m, on no street and on no plot, at the village's end:
  // set out after the last house, further from the focus than half the houses.
  CHECK(plan.church);
  u32 churches = 0, last_house = 0;
  for (const Plot& q : plan.plots)
    if (q.state == PlotState::built) last_house = std::max(last_house, q.order);
  for (const Plot& q : plan.plots)
    if (q.kind == PlotKind::church) {
      ++churches;
      CHECK(q.state == PlotState::reserved);
      CHECK(q.frontage_mm == 42000);
      CHECK(q.depth_mm == 24000);
      CHECK(q.order > last_house);
      CHECK(from_focus(q) > built[built.size() / 2]);
    }
  CHECK(churches == 1);
  CHECK(overlaps(plan) == 0);
  CHECK(treads_in_plots(plan) == 0);
  // No plot stands on the well.
  for (const Plot& q : plan.plots)
    CHECK(
        !in_plot(q, static_cast<f64>(plan.focus_x_mm), static_cast<f64>(plan.focus_z_mm), -1500.0));
  MESSAGE("two ways: " << plan.plots.size() << " plots, " << plan.cottages << " cottages, "
                       << plan.back_lanes << " back lanes, focus (" << plan.focus_x_mm << ", "
                       << plan.focus_z_mm << ")");
}

TEST_CASE("village: a back lane runs behind a built row") {
  VillageLayer p = regular(street(0, 0, 150, 0));
  p.lanes[0].rows = Rows::Left;
  p.back_lanes = true;
  p.houses = 6;
  const VillagePlan plan = planned(p, flat);
  REQUIRE(plan.built == 6);
  REQUIRE(plan.back_lanes == 1);
  REQUIRE(plan.lanes.size() == 2);
  const LaneLine& back = plan.lanes[1];
  CHECK(back.back);
  CHECK(back.from_lane == 0);
  CHECK(back.half_tread_mm == 1000);
  // Behind the plots' backs, on their side, and on none of them.
  for (const LaneStation& s : back.stations) {
    CHECK(left_of(p, s.x_mm, s.z_mm) > 3.05 + 8 * 5.03);
    CHECK(left_of(p, s.x_mm, s.z_mm) < 3.05 + 8 * 5.03 + 3.0);
  }
  CHECK(treads_in_plots(plan) == 0);
}

TEST_CASE("village: a house's placement turns its frame so its door is where the plan put it") {
  // A lane running at an angle, so no direction is an axis, and plots turned off it.
  const VillageLayer p = street(-10, 5, 90, 65);
  BoxHouse box;
  box.length_mm = 9000;
  box.width_mm = 4600;
  const VillagePlan plan = planned(p, gentle, &box);
  REQUIRE(plan.built > 0);
  for (const Plot& q : plan.plots) {
    if (q.state != PlotState::built) continue;
    const House& h = plan.houses[q.house];
    // Long side to the street where the frontage holds it, gable-on where it does not.
    CHECK((h.name == "box, gable-on") == (q.frontage_mm < 10000));
    const scene_gen::Placement at = house_placement(q);
    CHECK(length(at.rotation) == doctest::Approx(1.0f).epsilon(1e-6));
    // The engine's own rotation of the door's point in the house's frame lands on the plan's door.
    const Vec3 local{static_cast<f32>(h.door_x_mm) / 1000.0f, 0.0f,
                     static_cast<f32>(h.door_z_mm) / 1000.0f};
    const Vec3 turned = rotate(at.rotation, local);
    // (Within 1.5 mm: the door is kept to the millimetre and the quaternion is float32.)
    CHECK(std::fabs(at.position.x + static_cast<f64>(turned.x) -
                    static_cast<f64>(q.door_x_mm) / 1000.0) < 0.0015);
    CHECK(std::fabs(at.position.z + static_cast<f64>(turned.z) -
                    static_cast<f64>(q.door_z_mm) / 1000.0) < 0.0015);
    // And `house_point` is the same turn in f64.
    const WorldPos door = house_point(q, h.door_x_mm, 0, h.door_z_mm);
    CHECK(std::fabs(door.x - static_cast<f64>(q.door_x_mm) / 1000.0) < 0.0011);
    CHECK(std::fabs(door.z - static_cast<f64>(q.door_z_mm) / 1000.0) < 0.0011);
    CHECK(door.y == static_cast<f64>(q.pad_mm) / 1000.0);
    // The pad is the mean of the ground under the footprint (on this plane, the ground at its
    // middle, within the rounding of five heights) for the first house built, which has no
    // neighbour; a later one's is held within a step of its neighbours' and never more than the
    // entry's `max_pad_step_m` off the ground.
    const f64 mid_x = (static_cast<f64>(q.pad_x_mm[0] + q.pad_x_mm[2])) / 2.0;
    const f64 mid_z = (static_cast<f64>(q.pad_z_mm[0] + q.pad_z_mm[2])) / 2.0;
    const f64 off =
        std::fabs(gentle(nullptr, static_cast<i64>(mid_x), static_cast<i64>(mid_z)) * 1000.0 -
                  static_cast<f64>(q.pad_mm));
    CHECK(off < (q.house == 0 ? 3.0 : 1500.0));
  }
  // A 12 m house on the same plots turns on every plot under 13 m.
  box.length_mm = 12000;
  const VillagePlan longer = planned(p, gentle, &box);
  u32 turned_houses = 0;
  for (const Plot& q : longer.plots)
    if (q.state == PlotState::built) {
      const bool gable = longer.houses[q.house].name == "box, gable-on";
      CHECK(gable == (q.frontage_mm < 13000));
      turned_houses += gable ? 1 : 0;
    }
  CHECK(turned_houses > 0);
}

TEST_CASE("village: no two plots overlap and none stands in a street, round a bend and a meeting") {
  VillageLayer p = street(0, 0, 80, 0);
  p.lanes[0].waypoints.push_back(WorldPos{80.0, 0.0, 90.0});  // a right angle toward +z
  Lane second;
  second.waypoints.push_back(WorldPos{-40.0, 0.0, 70.0});
  second.waypoints.push_back(WorldPos{40.0, 0.0, 2.0});
  p.lanes.push_back(second);
  p.houses = 40;
  for (const f32 irregularity : {0.0f, 0.5f, 1.0f}) {
    p.irregularity = irregularity;
    const VillagePlan plan = planned(p, flat);
    CHECK(plan.built >= 8);
    CHECK(overlaps(plan) == 0);
    CHECK(treads_in_plots(plan) == 0);
    MESSAGE("irregularity " << irregularity << ": " << plan.plots.size() << " plots, " << plan.built
                            << " houses, " << plan.cottages << " cottages, " << plan.back_lanes
                            << " back lanes");
  }
}

TEST_CASE("village: a way runs into the way it meets, with no doubled tread and no step") {
  // The second way's least-cost path to its meeting runs along the first way's street; it gives
  // that stretch up and meets the first at one of its stations, at its top.
  VillageLayer p = street(0, 0, 120, 0);
  Lane second;
  second.name = "joining";
  second.waypoints.push_back(WorldPos{20.0, 0.0, 60.0});
  second.waypoints.push_back(WorldPos{90.0, 0.0, 1.0});
  p.lanes.push_back(second);
  p.houses = 12;
  for (const f32 irregularity : {0.0f, 1.0f}) {
    p.irregularity = irregularity;
    const VillagePlan plan = planned(p, gentle);
    REQUIRE(plan.lanes.size() >= 2);
    const LaneLine& first = plan.lanes[0];
    const LaneLine& joining = plan.lanes[1];
    const LaneStation& tip = joining.stations[joining.stations.size() - 1];
    const LaneStation* meet = nullptr;
    for (const LaneStation& s : first.stations)
      if (s.x_mm == tip.x_mm && s.z_mm == tip.z_mm) meet = &s;
    REQUIRE(meet != nullptr);  // it ends on a station of the way it meets
    CHECK(tip.top_mm == meet->top_mm);
    // Its stations within the first way's half street: the fork's own, a few metres, never a run
    // alongside.
    const f64 half_street = static_cast<f64>(p.street_width_m) * 500.0;
    u32 alongside = 0;
    for (const LaneStation& s : joining.stations) {
      f64 nearest = 1e300;
      for (const LaneStation& f : first.stations) {
        const f64 dx = static_cast<f64>(s.x_mm - f.x_mm), dz = static_cast<f64>(s.z_mm - f.z_mm);
        nearest = std::min(nearest, dx * dx + dz * dz);
      }
      if (nearest < half_street * half_street) ++alongside;
    }
    MESSAGE("irregularity " << irregularity << ": " << alongside << " of "
                            << joining.stations.size() << " stations within the half street");
    CHECK(static_cast<f64>(alongside) * static_cast<f64>(p.station_m) <=
          3.0 * half_street / 1000.0);
    // The mask regions: every way's verge, then every wear, then every tread, so no way's verge or
    // wear lies over another's tread.
    const JsonValue regions = ground_regions_json(p, plan);
    u32 last_verge = 0, first_wear = ~0u, last_wear = 0, first_tread = ~0u;
    for (u32 k = 0; k < regions.as_array().size(); ++k) {
      const std::string name(regions[k].find("name")->as_string());
      const auto ends = [&](const char* tail) {
        const std::string t(tail);
        return name.size() > t.size() && name.compare(name.size() - t.size(), t.size(), t) == 0;
      };
      if (ends(" verge")) last_verge = std::max(last_verge, k);
      if (ends("street wear") || ends("joining wear")) {
        first_wear = std::min(first_wear, k);
        last_wear = std::max(last_wear, k);
      }
      if (ends(" tread")) first_tread = std::min(first_tread, k);
    }
    CHECK(last_verge < first_wear);
    CHECK(last_wear < first_tread);
  }
}

TEST_CASE("village: a croft stops where the ground behind it steepens") {
  // Level for 18 m each side of the street, then a 1 in 2 bank: the tofts stand on the level and
  // their crofts end at the bank's foot, a step of 4 m onto it at most, the houses all built.
  VillageLayer p = regular(street(0, 0, 160, 0));
  p.houses = 8;
  const auto banks = [](const void*, i64, i64 z) noexcept {
    const f64 zz = std::fabs(static_cast<f64>(z) / 1000.0);
    return zz > 18.0 ? 0.5 * (zz - 18.0) : 0.0;
  };
  const VillagePlan plan = planned(p, banks);
  CHECK(plan.built == 8);
  u32 shortened = 0;
  for (const Plot& q : plan.plots) {
    if (q.state != PlotState::built) continue;
    const f64 back =
        std::fabs(static_cast<f64>(q.front_z_mm) + q.inward_z * static_cast<f64>(q.depth_mm)) /
        1000.0;
    CHECK(back <= 18.0 + 4.0 * 0.2 / 0.5 + 0.01);
    if (q.depth_mm < static_cast<i64>(p.perch_m * 1000.0f) * p.depth_perches) ++shortened;
  }
  CHECK(shortened == plan.built);
}

TEST_CASE("village: a plot too steep for a pad or standing in water is left empty and counted") {
  VillageLayer p = regular(street(0, 0, 120, 0));
  p.houses = 6;
  const VillagePlan plan = planned(p, bank);
  // The left row's footprints reach z = 5.05 + 4.6: on the bank, 0.6 * 3.65 m = 2.2 m of rise.
  CHECK(plan.steep > 0);
  CHECK(plan.built == 6);
  for (const Plot& q : plan.plots) {
    if (q.side == 0) CHECK((q.state == PlotState::steep || q.state == PlotState::spare));
    if (q.state == PlotState::built) CHECK(q.side == 1);
  }
  // A pond over the right-hand plots by the middle, where the village starts: those are wet, and
  // the houses stand further out.
  scene::WaterBody pond;
  JsonValue pond_json;
  REQUIRE(
      parse_json(std::string("{\"name\":\"pond\",\"level_m\":5,\"shape\":{\"circle\":{\"center\":"
                             "[60,0,-10],\"radius_m\":9}}}"),
                 pond_json)
          .ok);
  schema::ReadContext pond_reader;
  REQUIRE(schema::from_json(pond, pond_json, pond_reader));
  REQUIRE(pond_reader.ok());
  scene_gen::Exclusion exclusion;
  std::string error;
  REQUIRE_MESSAGE(exclusion.build({}, {&pond, 1}, {}, &error), error);
  const VillagePlan wet = planned(p, flat, nullptr, &exclusion);
  CHECK(wet.wet > 0);
  CHECK(wet.built == 6);
  for (const Plot& q : wet.plots)
    if (q.state == PlotState::built)
      CHECK(!exclusion.excluded_mm(ground(flat), q.house_x_mm, q.house_z_mm));
}

TEST_CASE("village: the pads, treads and paths are benches the tile set writes level") {
  const VillageLayer p = street(0, 0, 100, 30);
  const VillagePlan plan = planned(p, gentle);
  Vector<scene::TerrainEdit> edits;
  ground_edits(p, plan, edits);
  // A tread a way (back lanes included), and a pad and a path a house.
  CHECK(edits.size() == plan.lanes.size() + 2 * plan.built);
  for (const scene::TerrainEdit& e : edits)
    CHECK(e.mode == scene::TerrainEditMode::Bench);
  tileset::TerrainEdits built;
  std::string error;
  REQUIRE_MESSAGE(built.build({edits.data(), edits.size()}, &error), error);
  const auto edited = [&](i64 x, i64 z) {
    const f64 g = gentle(nullptr, x, z) * 1000.0;
    f64 v = g;
    if (!built.edited_mm(static_cast<f64>(x), static_cast<f64>(z), 50, g, v)) v = g;
    return v;
  };
  u32 checked = 0;
  for (const Plot& q : plan.plots) {
    if (q.state != PlotState::built) continue;
    const House& h = plan.houses[q.house];
    // Under the whole footprint the ground is the pad's level, to the millimetre.
    for (i64 x = h.min_x_mm; x <= h.max_x_mm; x += 500)
      for (i64 z = h.min_z_mm; z <= h.max_z_mm; z += 500) {
        const WorldPos w = house_point(q, x, 0, z);
        CHECK(std::fabs(edited(scene_gen::nearest_mm(w.x), scene_gen::nearest_mm(w.z)) -
                        static_cast<f64>(q.pad_mm)) <= 1.0);
        ++checked;
      }
    // Six metres behind the pad the ground is as it was.
    const WorldPos back = house_point(q, 0, 0, h.max_z_mm + 6600);
    const i64 bx = scene_gen::nearest_mm(back.x), bz = scene_gen::nearest_mm(back.z);
    CHECK(std::fabs(edited(bx, bz) - gentle(nullptr, bx, bz) * 1000.0) <= 1.0);
  }
  CHECK(checked > 400);
  // The tread is level across its width at each station's top, away from the houses' paths.
  const LaneLine& lane = plan.lanes[0];
  for (u32 k = 3; k + 3 < lane.stations.size(); k += 5) {
    const LaneStation& s = lane.stations[k];
    CHECK(std::fabs(edited(s.x_mm, s.z_mm) - static_cast<f64>(s.top_mm)) <= 25.0);
  }
}

TEST_CASE("village: the shapes cover the treads, the pads, the paths and the well, and no garden") {
  const VillageLayer p = street(0, 0, 100, 30);
  const VillagePlan plan = planned(p, gentle);
  REQUIRE(plan.well);
  scene_gen::FootprintList all;
  footprints(p, plan, all);
  scene_gen::Exclusion exclusion;
  std::string error;
  REQUIRE_MESSAGE(exclusion.build({}, {}, {}, &error), error);
  REQUIRE_MESSAGE(exclusion.add_footprints(all.span(), &error), error);
  const scene_gen::Ground g = ground(gentle);
  for (const Plot& q : plan.plots) {
    if (q.state != PlotState::built) continue;
    const House& h = plan.houses[q.house];
    CHECK(exclusion.excluded_mm(g, q.house_x_mm, q.house_z_mm));  // the pad
    CHECK(exclusion.excluded_mm(g, (q.door_x_mm + q.gate_x_mm) / 2,
                                (q.door_z_mm + q.gate_z_mm) / 2));  // the path to the gate
    CHECK(exclusion.excluded_mm(g, (q.gate_x_mm + q.path_x_mm) / 2,
                                (q.gate_z_mm + q.path_z_mm) / 2));    // and on to the tread
    const WorldPos garden = house_point(q, 0, 0, h.max_z_mm + 3000);  // behind the pad
    CHECK(!exclusion.excluded_mm(g, scene_gen::nearest_mm(garden.x),
                                 scene_gen::nearest_mm(garden.z)));
  }
  const LaneStation& mid = plan.lanes[0].stations[plan.lanes[0].stations.size() / 3];
  CHECK(exclusion.excluded_mm(g, mid.x_mm, mid.z_mm));                       // the tread
  CHECK(exclusion.excluded_mm(g, plan.focus_x_mm + 1000, plan.focus_z_mm));  // the well

  // The tiles' shapes together are the whole read's: every shape is handed by the tile its disc
  // stands on, and a tile far from the village hands none.
  u32 handed = 0;
  for (i64 tz = -3; tz <= 4; ++tz)
    for (i64 tx = -2; tx <= 5; ++tx) {
      scene_gen::FootprintList tile;
      footprints(p, plan, tile, 32000, tx, tz, 2000);
      handed += tile.size();
      for (u32 k = 0; k < tile.size(); ++k) {
        const scene_gen::Footprint& f = tile[k];
        const i64 r = static_cast<i64>(f.radius * 1000.0) + 2001;
        CHECK(f.x_mm + r >= tx * 32000);
        CHECK(f.x_mm - r < (tx + 1) * 32000);
      }
    }
  CHECK(handed >= all.size());
  scene_gen::FootprintList far_tile;
  footprints(p, plan, far_tile, 32000, 40, 40, 2000);
  CHECK(far_tile.size() == 0);
}

namespace {

// Whether a point (metres) is inside a mask region's outline: the crossing count, in doubles.
bool inside(const JsonValue& polygon, f64 x, f64 z) {
  const auto& pts = polygon.as_array();
  const auto at = [&](u32 point, usize axis) {
    f64 v = 0.0;
    REQUIRE(pts[point][axis].get_f64(v));
    return v;
  };
  bool in = false;
  for (u32 k = 0, j = pts.size() - 1; k < pts.size(); j = k++) {
    const f64 xk = at(k, 0), zk = at(k, 2);
    const f64 xj = at(j, 0), zj = at(j, 2);
    if ((zk > z) != (zj > z) && x < (xj - xk) * (z - zk) / (zj - zk) + xk) in = !in;
  }
  return in;
}

// The region whose name ends so, or null.
const JsonValue* named(const JsonValue& regions, const std::string& ending) {
  for (const JsonValue& r : regions.as_array()) {
    const std::string n(r.find("name")->as_string());
    if (n.size() >= ending.size() &&
        n.compare(n.size() - ending.size(), ending.size(), ending) == 0)
      return &r;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("village: the wear is a verge, a worn margin and a tread a lane, and a yard a house") {
  const VillageLayer p = regular(street(0, 0, 100, 30));
  const VillagePlan plan = planned(p, gentle);
  REQUIRE(plan.built > 0);
  const JsonValue regions = ground_regions_json(p, plan);
  // Three bands, and a yard and a path a house.
  REQUIRE(regions.as_array().size() == 3 + 2 * plan.built);
  // A lane's three bands, each inside the last, widest first so the narrower is laid over it; a
  // straight street's is its four corners, whatever its stations.
  const char* layers[3] = {"verge", "worn", "lane"};
  const LaneStation& mid = plan.lanes[0].stations[plan.lanes[0].stations.size() / 2];
  for (u32 k = 0; k < 3; ++k) {
    CHECK(regions[k].find("material")->as_string() == layers[k]);
    CHECK(regions[k].find("polygon")->as_array().size() == 4);
    CHECK(inside(*regions[k].find("polygon"), static_cast<f64>(mid.x_mm) / 1000.0,
                 static_cast<f64>(mid.z_mm) / 1000.0));
  }
  // 2.5 m from the line is verge alone; 1.7 m is in the worn margin and outside the middle.
  const f64 len = std::sqrt(100.0 * 100.0 + 30.0 * 30.0), nx = -30.0 / len, nz = 100.0 / len;
  const f64 mx = static_cast<f64>(mid.x_mm) / 1000.0, mz = static_cast<f64>(mid.z_mm) / 1000.0;
  CHECK(inside(*regions[0].find("polygon"), mx + nx * 2.5, mz + nz * 2.5));
  CHECK(!inside(*regions[1].find("polygon"), mx + nx * 2.5, mz + nz * 2.5));
  CHECK(inside(*regions[1].find("polygon"), mx + nx * 1.7, mz + nz * 1.7));
  CHECK(!inside(*regions[2].find("polygon"), mx + nx * 1.7, mz + nz * 1.7));
  // A yard holds the ground just before its door and a place a metre along the wall from it, not
  // the plot's far back; its path's band holds the way from the gate to the tread's edge.
  for (u32 k = 0; k < plan.plots.size(); ++k) {
    const Plot& q = plan.plots[k];
    if (q.state != PlotState::built) continue;
    const JsonValue* yard = named(regions, " plot " + std::to_string(k) + " yard");
    const JsonValue* path = named(regions, " plot " + std::to_string(k) + " path");
    REQUIRE(yard != nullptr);
    REQUIRE(path != nullptr);
    const JsonValue& y = *yard->find("polygon");
    const f64 dx = static_cast<f64>(q.door_x_mm) / 1000.0,
              dz = static_cast<f64>(q.door_z_mm) / 1000.0;
    CHECK(inside(y, dx - q.inward_x * 0.3, dz - q.inward_z * 0.3));
    CHECK(inside(y, dx - q.inward_x * 0.3 + q.along_x, dz - q.inward_z * 0.3 + q.along_z));
    CHECK(!inside(y, dx + q.inward_x * 12.0, dz + q.inward_z * 12.0));
    CHECK(inside(*path->find("polygon"), static_cast<f64>(q.gate_x_mm + q.path_x_mm) / 2000.0,
                 static_cast<f64>(q.gate_z_mm + q.path_z_mm) / 2000.0));
  }
  // No layer named, no region; no yard and no path, the lanes' alone.
  CHECK(ground_regions_json(p, plan, GroundLayers{"", "", ""}).as_array().size() == 0);
  VillageLayer bare = p;
  bare.yard_m = 0.0f;
  bare.path_width_m = 0.0f;
  CHECK(ground_regions_json(bare, planned(bare, gentle)).as_array().size() == 3);

  // A grown village's verge follows the street's wander and opens into the green round the well.
  const VillageLayer grown = street(0, 0, 100, 30);
  const VillagePlan green = planned(grown, gentle);
  REQUIRE(green.well);
  const JsonValue wear = ground_regions_json(grown, green);
  const JsonValue* verge = named(wear, "street verge");
  const JsonValue* well = named(wear, " well");
  REQUIRE(verge != nullptr);
  REQUIRE(well != nullptr);
  CHECK(verge->find("polygon")->as_array().size() > 8);
  const f64 fx = static_cast<f64>(green.focus_x_mm) / 1000.0,
            fz = static_cast<f64>(green.focus_z_mm) / 1000.0;
  CHECK(inside(*verge->find("polygon"), fx + nx * 9.0, fz + nz * 9.0));  // the green's breadth
  CHECK(inside(*well->find("polygon"), fx + 1.0, fz));
}

TEST_CASE("village: a plot is bounded once, and its front has a gateway at its gate") {
  for (const f32 irregularity : {0.0f, 0.8f}) {
    VillageLayer p = irregularity == 0.0f ? regular(street(0, 0, 100, 30)) : street(0, 0, 100, 30);
    p.irregularity = irregularity;
    p.houses = 5;  // so some built plots have a spare neighbour and some a built one
    const VillagePlan plan = planned(p, gentle);
    REQUIRE(plan.built == 5);
    Vector<BoundaryLine> lines;
    plot_boundaries(p, plan, lines);
    REQUIRE(lines.size() >= plan.built);
    // Every side of every built plot is on exactly one line: count the lines' segments that lie
    // on each of a plot's four sides (both ends within 2 cm of the side's ends, either way round).
    struct Segment {
      i64 ax, az, bx, bz;
    };
    Vector<Segment> segments;
    u32 gateways = 0;
    for (const BoundaryLine& b : lines) {
      REQUIRE(b.points_mm.size() >= 4);
      const u32 n = b.points_mm.size() / 2;
      for (u32 k = 0; k + 1 < n || (b.closed && k < n); ++k) {
        const u32 j = (k + 1) % n;
        segments.push_back(Segment{b.points_mm[2 * k], b.points_mm[2 * k + 1], b.points_mm[2 * j],
                                   b.points_mm[2 * j + 1]});
      }
      gateways += b.gateways_mm.size();
      const Plot& q = plan.plots[b.plot];
      CHECK(q.state == PlotState::built);
      for (const i64 station : b.gateways_mm) {
        // Walk the line to the station: the point there is the plot's gate.
        f64 left = static_cast<f64>(station), gx = 0.0, gz = 0.0;
        for (u32 k = 0; k + 1 < n; ++k) {
          const f64 ax = static_cast<f64>(b.points_mm[2 * k]),
                    az = static_cast<f64>(b.points_mm[2 * k + 1]);
          const f64 dx = static_cast<f64>(b.points_mm[2 * k + 2]) - ax;
          const f64 dz = static_cast<f64>(b.points_mm[2 * k + 3]) - az;
          const f64 length = std::sqrt(dx * dx + dz * dz);
          if (left <= length) {
            gx = ax + dx * left / length;
            gz = az + dz * left / length;
            break;
          }
          left -= length;
        }
        CHECK(std::fabs(gx - static_cast<f64>(q.gate_x_mm)) < 5.0);
        CHECK(std::fabs(gz - static_cast<f64>(q.gate_z_mm)) < 5.0);
      }
    }
    CHECK(gateways == plan.built);
    // Every point of every built plot's four sides is on exactly one segment: sampled every half
    // metre along each side (its corners left out), each within 2 cm of one segment and no more.
    const auto on_segment = [](const Segment& s, f64 x, f64 z) {
      const f64 ex = static_cast<f64>(s.bx - s.ax), ez = static_cast<f64>(s.bz - s.az);
      const f64 ee = ex * ex + ez * ez;
      if (!(ee > 0.0)) return false;
      const f64 t = ((x - static_cast<f64>(s.ax)) * ex + (z - static_cast<f64>(s.az)) * ez) / ee;
      if (t < 0.0 || t > 1.0) return false;
      const f64 dx = static_cast<f64>(s.ax) + t * ex - x, dz = static_cast<f64>(s.az) + t * ez - z;
      return dx * dx + dz * dz <= 400.0;
    };
    u32 points = 0, wrong = 0;
    for (const Plot& q : plan.plots) {
      if (q.state != PlotState::built) continue;
      const f64 half = static_cast<f64>(q.frontage_mm) * 0.5, depth = static_cast<f64>(q.depth_mm);
      const f64 cu[4] = {-half, half, half, -half}, cv[4] = {0.0, 0.0, depth, depth};
      for (u32 c = 0; c < 4; ++c) {
        const u32 d = (c + 1) % 4;
        const f64 length =
            std::sqrt((cu[d] - cu[c]) * (cu[d] - cu[c]) + (cv[d] - cv[c]) * (cv[d] - cv[c]));
        // (Not within a metre of a corner: two tofts round the green's lens share a front corner
        // and their sides leave it a few degrees apart, both walls, within 2 cm for the first
        // few decimetres.)
        for (f64 along = 1000.0; along < length - 1000.0; along += 500.0) {
          const f64 u = cu[c] + (cu[d] - cu[c]) * along / length;
          const f64 v = cv[c] + (cv[d] - cv[c]) * along / length;
          const f64 x = static_cast<f64>(q.front_x_mm) + q.along_x * u + q.inward_x * v;
          const f64 z = static_cast<f64>(q.front_z_mm) + q.along_z * u + q.inward_z * v;
          // (A point within 3 cm of a segment's end, where a deeper croft's side carries on past
          // its neighbour's back, is on both and is not counted.)
          bool at_end = false;
          for (const Segment& s : segments)
            for (const auto& e : {std::pair<i64, i64>{s.ax, s.az}, std::pair<i64, i64>{s.bx, s.bz}})
              at_end = at_end || std::hypot(static_cast<f64>(e.first) - x,
                                            static_cast<f64>(e.second) - z) < 30.0;
          if (at_end) continue;
          u32 on = 0;
          for (const Segment& s : segments)
            on += on_segment(s, x, z) ? 1u : 0u;
          ++points;
          wrong += on == 1 ? 0u : 1u;
        }
      }
    }
    CHECK(points > 0);
    CHECK(wrong == 0);
    // Two built neighbours of a row plan share a side, so there is less boundary than four sides a
    // plot. (A grown village's tofts share their corners, but round the green's lens their sides
    // fan, so the five nearest it may share none: the corners are the grown case's to count.)
    f64 drawn = 0.0, sides = 0.0;
    for (const Segment& s : segments)
      drawn += std::sqrt(
          static_cast<f64>((s.bx - s.ax) * (s.bx - s.ax) + (s.bz - s.az) * (s.bz - s.az)));
    for (const Plot& q : plan.plots)
      if (q.state == PlotState::built) sides += 2.0 * static_cast<f64>(q.frontage_mm + q.depth_mm);
    if (irregularity == 0.0f) CHECK(drawn < sides - 10000.0);
    const f64 shared_sides = (sides - drawn) / 1000.0;
    MESSAGE("irregularity " << irregularity << ": " << lines.size() << " lines, " << segments.size()
                            << " segments, " << shared_sides << " m of side shared, for "
                            << plan.built << " plots");
  }
}

TEST_CASE("village: the plan is the same bytes on every toolchain") {
  VillageLayer p = street(-40, 12, 60, -8);
  p.lanes[0].waypoints.push_back(WorldPos{110.0, 0.0, 40.0});
  Lane second;
  second.name = "back";
  second.rows = Rows::Left;
  second.waypoints.push_back(WorldPos{-30.0, 0.0, -70.0});
  second.waypoints.push_back(WorldPos{20.0, 0.0, 0.0});
  p.lanes.push_back(second);
  p.houses = 14;
  p.seed = 2026;
  const VillagePlan plan = planned(p, gentle);
  const u64 hash = plan_hash(plan);
  char text[24];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
  MESSAGE("plan hash " << text << ": " << plan.plots.size() << " plots, " << plan.built
                       << " houses, " << plan.cottages << " cottages, " << plan.back_lanes
                       << " back lanes, " << plan.steep << " steep");
  // Taken on MSVC (x86-64-v3); GCC and Clang in the Linux containers reproduce it.
  // Re-pinned for k_village_version 3 (the nucleated plan: ways that go round what is gone, tofts
  // side by side, uneven setbacks, the church at the end, the green's outline). Re-pinned for
  // version 4: the joining way gives up its stretch along the first way's street and eases its
  // top onto it, and the crofts stop where the ground steepens (the version is hashed too).
  // Re-pinned for version 5 (G208): no toft is cut, and no cottage squeezed, narrower than the
  // filler's least house with its side gaps (0xbe81d225360d56f9 before).
  CHECK(hash == 0xb6c67236aba86a53ull);
  // The same entry again, and the summary's hash, are the same.
  CHECK(plan_hash(planned(p, gentle)) == hash);
  CHECK(plan_json(p, plan).find("hash")->as_string() == std::string(text));
  // Another seed is another village.
  p.seed = 2027;
  CHECK(plan_hash(planned(p, gentle)) != hash);
}

// **Houses that differ** (village.md, "Houses that differ"; G208, the forest session's needs 110,
// 133 and 134): on a street of the rural filler's houses the storeys follow each household's
// means, so one-storey and two-storey houses stand in one row; no household is larger than its
// house sleeps; and every house asked stands where the street has room for it, since no toft is
// cut narrower than the least house.
TEST_CASE("village: houses differ by household, each household fits its house, asked is built") {
  VillageLayer p = street(0, 0, 300, 0);
  p.seed = 31;
  p.houses = 18;
  p.irregularity = 1.0f;
  scene_gen::Context context;
  context.ground = ground(gentle);
  VillagePlan plan;
  std::string error;
  REQUIRE_MESSAGE(plan_scene_village(p, context, plan, error), error);
  MESSAGE("houses that differ: " << plan.built << " of " << p.houses << " built, " << plan.refused
                                 << " refused, " << plan.no_fit << " no fit: " << plan.first_refusal);
  CHECK(plan.built == p.houses);
  CHECK(plan.refused == 0);
  u32 one = 0, two = 0, fitted = 0;
  for (const Plot& q : plan.plots) {
    if (q.state != PlotState::built) continue;
    JsonValue d;
    std::string text;
    REQUIRE_MESSAGE(describe_house(p, plan, q.house, d, text, error), error);
    const JsonValue& f = *d.find("filler");
    const JsonValue& hh = *d.find("household");
    const u64 storeys = f.find("storeys")->as_uint();
    // A cottage is of one storey: the storeys a plot is allowed are the most its house has.
    if (q.kind == PlotKind::cottage) {
      CHECK(storeys == 1);
      CHECK(f.find("type")->as_string() != std::string_view("two-up-two-down"));
      continue;
    }
    const std::string type{f.find("type")->as_string()};
    const u64 persons = hh.find("persons")->as_uint();
    one += storeys == 1 ? 1u : 0u;
    two += storeys == 2 ? 1u : 0u;
    fitted += hh.find("drawn_persons")->as_uint() != persons ? 1u : 0u;
    const u64 sleeps = type == "cot"              ? 2u
                       : type == "hall-parlour"   ? (storeys >= 2 ? 6u : 4u)
                       : type == "cross-passage" ? 8u
                                                  : 6u;
    CHECK_MESSAGE(persons <= sleeps, "house " << q.house << ": " << persons << " in a " << type);
  }
  MESSAGE("tenements of one storey " << one << ", of two " << two << ", households fitted "
                                     << fitted);
  CHECK(one >= 1);
  CHECK(two >= 1);
}

// **A house as data** (village.md, "A house as data"; the forest session's need 113 and 132): what
// `describe_house` writes is the plan's own house — the plot's seed and setback, the household the
// filler drew, a frame that carries the house's door onto the plan's door — so a consumer never
// re-derives a draw.
TEST_CASE(
    "village: a house as data is the plan's own house, its plot's seed, setback and household") {
  VillageLayer p = street(0, 0, 130, 0);
  p.seed = 7;
  p.houses = 6;
  scene_gen::Context context;
  context.ground = ground(gentle);
  VillagePlan plan;
  std::string error;
  REQUIRE_MESSAGE(plan_scene_village(p, context, plan, error), error);
  REQUIRE(plan.built >= 3);
  const JsonValue summary = plan_json(p, plan);
  const JsonValue& listed = *summary.find("plot_list");
  REQUIRE(listed.as_array().size() == plan.plots.size());
  u32 checked = 0;
  for (u32 k = 0; k < plan.plots.size(); ++k) {
    const Plot& q = plan.plots[k];
    const JsonValue& row = listed[k];
    CHECK(row.find("index")->as_uint() == k);
    CHECK(row.find("seed")->as_uint() == q.seed);
    if (q.state != PlotState::built) continue;
    REQUIRE(row.find("house_index") != nullptr);
    CHECK(row.find("house_index")->as_uint() == q.house);
    CHECK(std::fabs(row.find("setback_m")->as_float() - static_cast<f64>(q.setback_mm) / 1000.0) <
          1e-9);
    CHECK(q.setback_mm > 0);
    const House& h = plan.houses[q.house];
    CHECK(h.persons >= 1);  // a cot's household is two at most since G208
    CHECK(h.persons <= 8);
    CHECK(!h.type.empty());

    JsonValue d;
    std::string text;
    REQUIRE_MESSAGE(describe_house(p, plan, q.house, d, text, error), error);
    CHECK(d.find("format")->as_string() == std::string("engine.village.house.v1"));
    CHECK(d.find("plot")->as_uint() == k);
    const JsonValue& ask = *d.find("ask");
    CHECK(ask.find("seed")->as_uint() == q.seed);
    CHECK(std::fabs(ask.find("setback_m")->as_float() - static_cast<f64>(q.setback_mm) / 1000.0) <
          1e-9);
    CHECK(ask.find("storeys")->as_uint() == q.storeys);
    CHECK(d.find("household")->find("persons")->as_uint() == h.persons);
    CHECK(d.find("filler")->find("seed")->as_uint() == h.filler_seed);
    CHECK(d.find("filler")->find("type")->as_string() == h.type);
    CHECK(d.find("rooms")->as_array().size() > 0);
    CHECK(d.find("pieces")->as_array().size() > 0);
    CHECK(d.find("route")->as_array().size() == h.route.size());
    CHECK(text.find("storey 0") != std::string::npos);
    // The frame carries the house's door onto the plan's door, to the millimetre.
    const JsonValue& frame = *d.find("frame");
    const JsonValue& origin = *frame.find("origin");
    const JsonValue& xa = *frame.find("x_axis");
    const JsonValue& za = *frame.find("z_axis");
    const JsonValue& door = *d.find("door")->find("house");
    const f64 dx = door[0].as_float(), dz = door[2].as_float();
    const f64 wx = origin[0].as_float() + dx * xa[0].as_float() + dz * za[0].as_float();
    const f64 wz = origin[2].as_float() + dx * xa[2].as_float() + dz * za[2].as_float();
    CHECK(std::fabs(wx - static_cast<f64>(q.door_x_mm) / 1000.0) < 0.0015);
    CHECK(std::fabs(wz - static_cast<f64>(q.door_z_mm) / 1000.0) < 0.0015);
    CHECK(std::fabs(origin[1].as_float() - static_cast<f64>(q.pad_mm + q.floor_mm) / 1000.0) <
          0.0015);
    ++checked;
  }
  CHECK(checked == plan.built);
  JsonValue none;
  std::string text;
  CHECK(!describe_house(p, plan, plan.built, none, text, error));
  CHECK(error.find("built houses") != std::string::npos);
  // A plan made by another filler has no description to give.
  VillagePlan boxes;
  REQUIRE_MESSAGE(plan_village(p, context.ground, context, &box_house, nullptr, boxes, error),
                  error);
  REQUIRE(boxes.built > 0);
  CHECK(!describe_house(p, boxes, 0, none, text, error));
}

namespace {

KitPiece kit_row(const char* name, KitReplaces replaces, KitFront front, f32 x, f32 y, f32 z) {
  KitPiece k;
  k.name = name;
  k.mesh = std::string("kit/") + name + ".glb";
  k.scale =
      0.5f;  // the meshes are modelled at twice their size: a row is read off them as they are
  k.bounds_min[0] = -x;
  k.bounds_min[1] = 0.1f;  // its lowest point a little over its origin
  k.bounds_min[2] = -z;
  k.bounds_max[0] = x;
  k.bounds_max[1] = 0.1f + y;
  k.bounds_max[2] = z;
  k.front = front;
  k.replaces = replaces;
  return k;
}

// How near a segment from a to b comes to a rectangle (x0, z0, x1, z1), metres.
f64 segment_to_rect(f64 ax, f64 az, f64 bx, f64 bz, f64 x0, f64 z0, f64 x1, f64 z1) {
  f64 least = 1e30;
  for (u32 s = 0; s <= 100; ++s) {
    const f64 t = static_cast<f64>(s) / 100.0;
    const f64 px = ax + (bx - ax) * t, pz = az + (bz - az) * t;
    const f64 dx = px < x0 ? x0 - px : (px > x1 ? px - x1 : 0.0);
    const f64 dz = pz < z0 ? z0 - pz : (pz > z1 ? pz - z1 : 0.0);
    least = std::min(least, std::sqrt(dx * dx + dz * dz));
  }
  return least;
}

}  // namespace

// **A kit in the box furniture's place** (village.md, "A kit"; dwellings.md, "Kit pieces"): a
// scene's own meshes stand where the furnisher's table sets, beds and chests would, turned to face
// into the room, at the furnisher's clearances (its validator reads them back), the boxes they
// replace gone from the description, the way through the house kept off them as it is off the
// furniture, and the generator hands each piece's mesh with its house. A row too tall for any room
// is never placed, and a row that replaces nothing is listed and not placed.
TEST_CASE(
    "village: a kit's pieces stand in the box furniture's place, and the way keeps off them") {
  VillageLayer p = street(0, 0, 130, 0);
  p.seed = 7;
  p.houses = 6;
  scene_gen::Context context;
  context.ground = ground(gentle);
  VillagePlan boxes;
  std::string error;
  REQUIRE_MESSAGE(plan_scene_village(p, context, boxes, error), error);
  // Model units at scale 0.5: a table set 1.6 by 1.1 m and 0.8 m high facing -z, a bed 1.4 by
  // 2.0 m facing +x (its foot), a chest, a settle 2.6 m tall, and a dresser that replaces nothing.
  p.kit.push_back(kit_row("table", KitReplaces::TableSet, KitFront::NegZ, 1.6f, 1.6f, 1.1f));
  p.kit.push_back(kit_row("bed", KitReplaces::Bed, KitFront::PosX, 2.0f, 2.0f, 1.4f));
  p.kit.push_back(kit_row("chest", KitReplaces::Chest, KitFront::PosZ, 0.9f, 1.0f, 0.44f));
  p.kit.push_back(kit_row("tall-settle", KitReplaces::Settle, KitFront::NegZ, 1.6f, 5.2f, 0.6f));
  p.kit.push_back(kit_row("dresser", KitReplaces::None, KitFront::NegZ, 1.2f, 3.4f, 0.9f));
  VillagePlan plan;
  REQUIRE_MESSAGE(plan_scene_village(p, context, plan, error), error);
  // The kit moves no plot and no house's box.
  REQUIRE(plan.built == boxes.built);
  CHECK(plan.plots.size() == boxes.plots.size());
  u32 placed = 0, rows_seen[5] = {0, 0, 0, 0, 0};
  for (u32 i = 0; i < plan.houses.size(); ++i) {
    const House& h = plan.houses[i];
    CHECK(h.min_x_mm == boxes.houses[i].min_x_mm);
    CHECK(h.max_z_mm == boxes.houses[i].max_z_mm);
    if (!h.kit.empty()) CHECK(h.recipe.size() < boxes.houses[i].recipe.size());
    JsonValue d;
    std::string text;
    REQUIRE_MESSAGE(describe_house(p, plan, i, d, text, error), error);
    CHECK(d.find("kit")->as_array().size() == h.kit.size());
    CHECK(d.find("kit_rows")->as_array().size() == p.kit.size());
    // Every row as the entry states it (need 153 (e)): its class, front, heights and the piece it
    // stands in for, by name.
    for (u32 r = 0; r < p.kit.size(); ++r) {
      const JsonValue& row = (*d.find("kit_rows"))[r];
      REQUIRE(row.find("placement") != nullptr);
      CHECK(row.find("placement")->as_string() == "Wall");
      REQUIRE(row.find("front") != nullptr);
      REQUIRE(row.find("height_min_m") != nullptr);
      REQUIRE(row.find("height_max_m") != nullptr);
      REQUIRE(row.find("scale") != nullptr);
      CHECK(row.find("foot")->as_array().size() == 4);
      CHECK(row.find("replaces")->as_string() == (r == 0   ? "TableSet"
                                                  : r == 1 ? "Bed"
                                                  : r == 2 ? "Chest"
                                                  : r == 3 ? "Settle"
                                                           : "None"));
    }
    CHECK((*d.find("kit_rows"))[1].find("front")->as_string() == "PosX");
    // A kit chest's lid is a place (need 153 (a)).
    u32 kit_chests = 0, lids = 0;
    for (const KitPlace& k : h.kit)
      kit_chests += k.row == 2 ? 1u : 0u;
    for (const JsonValue& place : d.find("places")->as_array()) {
      const u32 of = static_cast<u32>(place.find("of")->as_uint());
      if (place.find("kind")->as_string() == "chest-lid" &&
          (*d.find("pieces"))[of].find("kit_row") != nullptr)
        ++lids;
    }
    CHECK(lids == kit_chests);
    const JsonValue& pieces = *d.find("pieces");
    const JsonValue& route = *d.find("route");
    for (const KitPlace& k : h.kit) {
      ++placed;
      REQUIRE(k.row < 5);
      ++rows_seen[k.row];
      const JsonValue& piece = pieces[k.piece];
      REQUIRE(piece.find("kit_row") != nullptr);
      CHECK(piece.find("kit_row")->as_uint() == k.row);
      CHECK(piece.find("members")->as_uint() == 0);
      const f64 x0 = piece.find("low")->operator[](0).as_float();
      const f64 y0 = piece.find("low")->operator[](1).as_float();
      const f64 z0 = piece.find("low")->operator[](2).as_float();
      const f64 x1 = piece.find("high")->operator[](0).as_float();
      const f64 z1 = piece.find("high")->operator[](2).as_float();
      // The mesh's lowest point on the floor: its origin 0.05 m under it (0.1 at scale 0.5).
      CHECK(std::fabs(static_cast<f64>(k.y_mm) / 1000.0 - (y0 - 0.05)) < 0.0015);
      // Its origin over its foot's middle (the meshes are centred on their origins).
      CHECK(std::fabs(static_cast<f64>(k.x_mm) / 1000.0 - 0.5 * (x0 + x1)) < 0.0015);
      CHECK(std::fabs(static_cast<f64>(k.z_mm) / 1000.0 - 0.5 * (z0 + z1)) < 0.0015);
      // The way through the house keeps a capsule's radius off its foot.
      for (u32 r = 0; r + 1 < route.as_array().size(); ++r) {
        const JsonValue& a = *route[r].find("house");
        const JsonValue& b = *route[r + 1].find("house");
        if (std::fabs(a[1].as_float() - y0) > 0.3 || std::fabs(b[1].as_float() - y0) > 0.3)
          continue;
        CHECK(segment_to_rect(a[0].as_float(), a[2].as_float(), b[0].as_float(), b[2].as_float(),
                              x0, z0, x1, z1) >= 0.3);
      }
    }
  }
  MESSAGE(placed << " kit pieces in " << plan.built << " houses: " << rows_seen[0] << " tables, "
                 << rows_seen[1] << " beds, " << rows_seen[2] << " chests");
  CHECK(rows_seen[0] > 0);
  CHECK(rows_seen[1] > 0);
  CHECK(rows_seen[2] > 0);
  CHECK(rows_seen[3] == 0);  // 2.6 m stands under no joists and no plate
  CHECK(rows_seen[4] == 0);  // replaces nothing
  // The kit changes the plan's hash; the same kit gives the same plan.
  CHECK(plan_hash(plan) != plan_hash(boxes));
  VillagePlan again;
  REQUIRE_MESSAGE(plan_scene_village(p, context, again, error), error);
  CHECK(plan_hash(again) == plan_hash(plan));

  // The generator hands each piece's mesh with its house, resolved against the scene's directory.
  const scene_gen::PlacementGeneratorDesc* desc =
      scene_gen::GeneratorRegistry::global().find_placement("village");
  REQUIRE(desc != nullptr);
  JsonValue params = schema::to_json(p);
  scene_gen::Context scene = context;
  scene.dir = "scenes";
  void* state = nullptr;
  REQUIRE_MESSAGE(desc->open(params, scene, &state, &error), error);
  scene_gen::Placements whole;
  REQUIRE_MESSAGE(desc->expand(state, scene, whole, &error), error);
  CHECK(whole.instances.size() == plan.built + placed);
  u32 kit_instances = 0;
  for (const scene_gen::Placement& q : whole.instances) {
    REQUIRE(q.mesh < whole.meshes.size());
    const scene_gen::PlacementMesh& m = whole.meshes[q.mesh];
    if (!m.recipe.empty()) continue;
    ++kit_instances;
    CHECK(m.path.find("kit") != std::string::npos);
    CHECK(m.path.rfind("scenes", 0) == 0);
    CHECK(std::fabs(q.scale.x - 0.5f) < 1e-6f);
  }
  CHECK(kit_instances == placed);
  desc->close(state);
}

TEST_CASE(
    "village: the generator's tiles hand what its whole read hands, and it plans the ground") {
  const scene_gen::PlacementGeneratorDesc* desc =
      scene_gen::GeneratorRegistry::global().find_placement("village");
  REQUIRE(desc != nullptr);
  CHECK(desc->tile_meshes);
  REQUIRE(desc->plan_ground != nullptr);
  JsonValue params;
  REQUIRE(parse_json(std::string("{\"name\":\"gen\",\"seed\":7,\"houses\":8,\"lanes\":[{\"name\":"
                                 "\"street\",\"waypoints\":[[-20,0,10],[40,0,25],[110,0,20]]}]}"),
                     params)
              .ok);
  scene_gen::Context context;
  context.ground = ground(gentle);
  context.where = "test: placements 0";
  void* state = nullptr;
  std::string error;
  REQUIRE_MESSAGE(desc->open(params, context, &state, &error), error);
  scene_gen::GroundPlan planned;
  REQUIRE_MESSAGE(desc->plan_ground(state, context, planned, &error), error);
  scene_gen::Placements whole;
  REQUIRE_MESSAGE(desc->expand(state, context, whole, &error), error);
  CHECK(whole.things == 8);
  // A tread a way, and a pad and a path a house; the same things as shapes.
  u32 pads = 0, paths = 0, treads = 0;
  for (const scene::TerrainEdit& e : planned.edits) {
    const auto ends = [&](const char* s) {
      const std::string tail(s);
      return e.name.size() >= tail.size() &&
             e.name.compare(e.name.size() - tail.size(), tail.size(), tail) == 0;
    };
    pads += ends(" pad") ? 1u : 0u;
    paths += ends(" path") ? 1u : 0u;
    treads += ends(" tread") ? 1u : 0u;
  }
  CHECK(pads == whole.things);
  CHECK(paths == whole.things);
  CHECK(treads >= 1);
  CHECK(pads + paths + treads == planned.edits.size());
  CHECK(planned.holes.empty());
  CHECK(whole.shapes.size() > 2 * whole.things);
  // Every instance names a mesh the read lists, and houses of one description share one.
  for (const scene_gen::Placement& p : whole.instances)
    CHECK(p.mesh < whole.meshes.size());
  CHECK(whole.meshes.size() <= whole.instances.size());
  // Tile by tile: the same houses, each from the tile its origin stands on, with its mesh; a tile
  // the generator says it does not occupy hands nothing.
  u32 instances = 0, occupied = 0;
  for (i32 tz = -3; tz <= 4; ++tz)
    for (i32 tx = -3; tx <= 5; ++tx) {
      scene_gen::Placements tile;
      const scene_gen::TileCoord at{tx, tz};
      REQUIRE_MESSAGE(desc->tile(state, at, context, tile, &error), error);
      if (!desc->occupies(state, at)) {
        CHECK(tile.instances.empty());
        CHECK(tile.shapes.size() == 0);
        continue;
      }
      ++occupied;
      instances += tile.instances.size();
      for (const scene_gen::Placement& p : tile.instances) {
        REQUIRE(p.mesh < tile.meshes.size());
        CHECK(p.position.x >= static_cast<f64>(tx) * 32.0);
        CHECK(p.position.x < static_cast<f64>(tx + 1) * 32.0);
        CHECK(p.position.z >= static_cast<f64>(tz) * 32.0);
        CHECK(p.position.z < static_cast<f64>(tz + 1) * 32.0);
        // The tile's mesh is the whole read's, recipe for recipe.
        bool listed = false;
        for (const scene_gen::PlacementMesh& m : whole.meshes)
          listed = listed ||
                   (m.path == tile.meshes[p.mesh].path && m.recipe == tile.meshes[p.mesh].recipe);
        CHECK(listed);
      }
    }
  CHECK(instances == whole.instances.size());
  CHECK(occupied > 0);
  MESSAGE("the generator: " << whole.things << " houses, " << whole.instances.size()
                            << " placements of " << whole.meshes.size() << " meshes, "
                            << planned.edits.size() << " edits, " << whole.shapes.size()
                            << " shapes, " << occupied << " tiles");
  desc->close(state);
  // A moving ground is refused by name.
  scene_gen::Context moving = context;
  moving.stand = ground(flat);
  CHECK(!desc->open(params, moving, &state, &error));
  CHECK(error.find("static ground") != std::string::npos);
}

TEST_CASE("village: an entry out of range is refused with a sentence") {
  const auto refused = [](const VillageLayer& p) {
    scene_gen::Context context;
    context.ground = ground(flat);
    VillagePlan plan;
    std::string error;
    const bool ok = plan_village(p, context.ground, context, &box_house, nullptr, plan, error);
    CHECK(!ok);
    return error;
  };
  VillageLayer p = street(0, 0, 100, 0);
  p.lanes.clear();
  CHECK(refused(p).find("no lanes") != std::string::npos);
  p = street(0, 0, 0, 0);
  CHECK(refused(p).find("one place") != std::string::npos);
  p = street(0, 0, 100, 0);
  p.houses = 0;
  CHECK(refused(p).find("houses") != std::string::npos);
  p = street(0, 0, 100, 0);
  p.perch_m = 3.0f;
  CHECK(refused(p).find("perch_m") != std::string::npos);
  p = street(0, 0, 100, 0);
  p.frontage_min_perches = 5;
  p.frontage_max_perches = 4;
  CHECK(refused(p).find("frontage") != std::string::npos);
  p = street(0, 0, 100, 0);
  p.tread_width_m = 9.0f;  // wider than the street
  CHECK(refused(p).find("tread_width_m") != std::string::npos);
  p = street(0, 0, 100, 0);
  p.irregularity = 1.5f;
  CHECK(refused(p).find("irregularity") != std::string::npos);
  p = street(0, 0, 100, 0);
  p.green_m = 3.0f;  // narrower than the street
  CHECK(refused(p).find("green_m") != std::string::npos);
  // No house filler.
  p = street(0, 0, 100, 0);
  scene_gen::Context context;
  context.ground = ground(flat);
  VillagePlan plan;
  std::string error;
  CHECK(!plan_village(p, context.ground, context, nullptr, nullptr, plan, error));
  CHECK(error.find("house filler") != std::string::npos);
}

TEST_CASE("village: a house's cut coarsens with distance and never falls under its root") {
  // What the cluster LOD makes of the houses the scene's own filler writes (village.md, "What was
  // measured"): each distinct house of a street meshed as the tile would mesh it, and its DAG cut
  // from 50 m, 200 m and a kilometre before its front, at 2160 lines and a 60 degree field of view
  // with a one-pixel threshold (the structures capability's own case, on its cot).
  VillageLayer p = street(0, 0, 130, 0);
  p.seed = 7;
  scene_gen::Context context;
  context.ground = ground(gentle);
  VillagePlan plan;
  std::string error;
  REQUIRE_MESSAGE(plan_scene_village(p, context, plan, error), error);
  REQUIRE(plan.built > 0);
  std::vector<std::string> measured;
  for (const House& h : plan.houses) {
    if (std::find(measured.begin(), measured.end(), h.path) != measured.end()) continue;
    measured.push_back(h.path);
    structure::Recipe recipe;
    REQUIRE_MESSAGE(structures::read_file(h.recipe, recipe, error), error);
    geometry::ClusterFileData container;
    structures::Report report;
    const assets::MeshGeneratorInputs inputs;
    const geometry::ClusterLodOptions lod;
    REQUIRE_MESSAGE(structures::build_container(recipe, inputs, lod, 0, container, report, error),
                    error);
    u32 before = ~0u;
    u32 cut_triangles[3] = {0, 0, 0}, cut_clusters[3] = {0, 0, 0};
    const f32 distances[3] = {50.0f, 200.0f, 1000.0f};
    for (u32 k = 0; k < 3; ++k) {
      geometry::LodView view;
      view.camera = {static_cast<f32>(h.min_x_mm + h.max_x_mm) * 0.0005f, 1.7f,
                     static_cast<f32>(h.min_z_mm) * 0.001f - distances[k]};
      view.proj_scale = 1.7320508f * 1080.0f;
      view.threshold_px = 1.0f;
      Vector<u32> cut;
      geometry::select_lod(container.mesh, view, cut);
      for (const u32 c : cut)
        cut_triangles[k] += container.mesh.mesh.clusters[c].triangle_count;
      cut_clusters[k] = cut.size();
      CHECK(cut_triangles[k] > 0);
      CHECK(cut_triangles[k] <= before);
      before = cut_triangles[k];
    }
    CHECK(before >= report.root_triangles);
    std::printf(
        "village: %s: %u triangles in %u clusters; the cut at 50 m %u in %u, at 200 m %u "
        "in %u, at 1 km %u in %u; root %u\n",
        h.name.c_str(), report.triangles, report.clusters, cut_triangles[0], cut_clusters[0],
        cut_triangles[1], cut_clusters[1], cut_triangles[2], cut_clusters[2],
        report.root_triangles);
  }
  CHECK(measured.size() >= 2);
}

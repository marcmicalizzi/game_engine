// The village capability (docs/subsystems/village.md): the row plan on flat and on rising ground,
// the plots a bend, a slope and water leave empty, the pads, treads and paths as the tile set
// writes them, the shapes, the house's turn, the plan's determinism, and the refusals.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/scene_gen/exclusion.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/cuts.h>
#include <domain/village/village.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

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
  return ((static_cast<f64>(x) / 1000.0 - a.x) * (-dz) + (static_cast<f64>(z) / 1000.0 - a.z) * dx) /
         len;
}

}  // namespace

TEST_CASE("village: a two-row street lays plots of whole perches and stands the entry's houses") {
  VillageLayer p = street(0, 0, 120, 0);
  const VillagePlan plan = planned(p, flat);
  CHECK(plan.built == 8);
  CHECK(std::string(plan.size_class) == "village");
  CHECK(plan.steep + plan.wet + plan.no_fit + plan.refused == 0);
  REQUIRE(plan.lanes.size() == 1);
  CHECK(plan.lanes[0].length_mm == 120000);
  CHECK(plan.lanes[0].stations.size() == 61);  // every 2 m, both ends
  for (const LaneStation& s : plan.lanes[0].stations)
    CHECK(s.top_mm == 2000);
  u32 left = 0, right = 0;
  i64 cursor[2] = {0, 0};
  for (const Plot& q : plan.plots) {
    // Whole perches, two to four.
    CHECK(q.frontage_mm % 5030 == 0);
    CHECK(q.frontage_mm >= 2 * 5030);
    CHECK(q.frontage_mm <= 4 * 5030);
    CHECK(q.depth_mm == 8 * 5030);
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
    // The pad holds the house with its margin, and stands inside the plot's depth.
    for (u32 c = 0; c < 4; ++c) {
      const f64 d = std::fabs(left_of(p, q.pad_x_mm[c], q.pad_z_mm[c]));
      CHECK(d > 3.05 + 2.0 - 0.61);
      CHECK(d < 3.05 + 8 * 5.03);
    }
    CHECK(q.pad_mm == 2000);
    // The path ends on the tread's near edge.
    CHECK(std::fabs(left_of(p, q.path_x_mm, q.path_z_mm)) == doctest::Approx(1.5).epsilon(1e-3));
  }
  // Plots of one row follow each other without overlap or gap.
  for (const Plot& q : plan.plots) {
    CHECK(q.along_mm - q.frontage_mm / 2 == cursor[q.side]);
    cursor[q.side] += q.frontage_mm;
  }
  // The two rows alternate along the lane, so eight houses stand four a side, give or take one.
  CHECK(left + right == 8);
  CHECK(std::max(left, right) - std::min(left, right) <= 2);
  MESSAGE("120 m of street: " << plan.plots.size() << " plots, " << left << " houses left, " << right
                              << " right");

  // Fewer than six houses is a hamlet; one row keeps to its side.
  p.houses = 4;
  p.lanes[0].rows = Rows::Right;
  const VillagePlan hamlet = planned(p, flat);
  CHECK(hamlet.built == 4);
  CHECK(std::string(hamlet.size_class) == "hamlet");
  for (const Plot& q : hamlet.plots)
    CHECK(q.side == 1);
}

TEST_CASE("village: a house's placement turns its frame so its door is where the plan put it") {
  // A lane running at an angle, so no direction is an axis.
  const VillageLayer p = street(-10, 5, 90, 65);
  BoxHouse box;
  box.length_mm = 9000;
  box.width_mm = 4600;
  const VillagePlan plan = planned(p, gentle, &box);
  REQUIRE(plan.built > 0);
  u32 gable_on = 0;
  for (const Plot& q : plan.plots) {
    if (q.state != PlotState::built) continue;
    const House& h = plan.houses[q.house];
    if (h.name == "box, gable-on") ++gable_on;
    const scene_gen::Placement at = house_placement(q);
    CHECK(length(at.rotation) == doctest::Approx(1.0f).epsilon(1e-6));
    // The engine's own rotation of the door's point in the house's frame lands on the plan's door.
    const Vec3 local{static_cast<f32>(h.door_x_mm) / 1000.0f, 0.0f,
                     static_cast<f32>(h.door_z_mm) / 1000.0f};
    const Vec3 turned = rotate(at.rotation, local);
    CHECK(at.position.x + static_cast<f64>(turned.x) ==
          doctest::Approx(static_cast<f64>(q.door_x_mm) / 1000.0).epsilon(1e-5));
    CHECK(at.position.z + static_cast<f64>(turned.z) ==
          doctest::Approx(static_cast<f64>(q.door_z_mm) / 1000.0).epsilon(1e-5));
    // And `house_point` is the same turn in f64.
    const WorldPos door = house_point(q, h.door_x_mm, 0, h.door_z_mm);
    CHECK(std::fabs(door.x - static_cast<f64>(q.door_x_mm) / 1000.0) < 0.0011);
    CHECK(std::fabs(door.z - static_cast<f64>(q.door_z_mm) / 1000.0) < 0.0011);
    CHECK(door.y == static_cast<f64>(q.pad_mm) / 1000.0);
    // The pad is the mean of the ground under the footprint: on this plane, the ground at its
    // middle, within the rounding of five heights.
    const f64 mid_x = (static_cast<f64>(q.pad_x_mm[0] + q.pad_x_mm[2])) / 2.0;
    const f64 mid_z = (static_cast<f64>(q.pad_z_mm[0] + q.pad_z_mm[2])) / 2.0;
    CHECK(std::fabs(gentle(nullptr, static_cast<i64>(mid_x), static_cast<i64>(mid_z)) * 1000.0 -
                    static_cast<f64>(q.pad_mm)) < 3.0);
  }
  // A 9 m house needs 10 m of frontage: a 2-perch plot (10.06 m) holds it long side on, so none of
  // these is gable-on; a 12 m house on the same plots turns on every plot under 3 perches.
  CHECK(gable_on == 0);
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

TEST_CASE("village: a bend leaves a gap on its inside, and no two plots overlap") {
  VillageLayer p = street(0, 0, 80, 0);
  p.lanes[0].waypoints.push_back(WorldPos{80.0, 0.0, 90.0});  // a right angle toward +z: left inside
  p.houses = 40;
  const VillagePlan plan = planned(p, flat);
  CHECK(plan.built >= 8);
  // Every pair of plots: their rectangles (frontage by depth) share no interior point, tested on a
  // 1 m lattice of each plot's own rectangle against every other's frame.
  const auto inside = [](const Plot& q, f64 x, f64 z) {
    const f64 rx = x - static_cast<f64>(q.front_x_mm), rz = z - static_cast<f64>(q.front_z_mm);
    const f64 u = rx * q.along_x + rz * q.along_z, v = rx * q.inward_x + rz * q.inward_z;
    return std::fabs(u) < static_cast<f64>(q.frontage_mm) / 2.0 - 1.0 && v > 1.0 &&
           v < static_cast<f64>(q.depth_mm) - 1.0;
  };
  u32 overlaps = 0;
  for (u32 a = 0; a < plan.plots.size(); ++a) {
    const Plot& q = plan.plots[a];
    for (f64 u = -static_cast<f64>(q.frontage_mm) / 2 + 500; u < static_cast<f64>(q.frontage_mm) / 2;
         u += 1000)
      for (f64 v = 500; v < static_cast<f64>(q.depth_mm); v += 1000) {
        const f64 x = static_cast<f64>(q.front_x_mm) + q.along_x * u + q.inward_x * v;
        const f64 z = static_cast<f64>(q.front_z_mm) + q.along_z * u + q.inward_z * v;
        for (u32 b = 0; b < plan.plots.size(); ++b)
          if (b != a && inside(plan.plots[b], x, z)) ++overlaps;
      }
  }
  CHECK(overlaps == 0);
  // The inside (left) row of the first leg stops short of the corner by the street's half width
  // and the plots' depth; the outside row runs to it.
  i64 last_left = 0, last_right = 0;
  for (const Plot& q : plan.plots)
    if (q.segment == 0) {
      i64& last = q.side == 0 ? last_left : last_right;
      last = std::max(last, q.along_mm + q.frontage_mm / 2);
    }
  CHECK(last_left <= 80000 - 3050 - 8 * 5030);
  CHECK(last_right > last_left);
}

TEST_CASE("village: a plot too steep for a pad or standing in water is left empty and counted") {
  VillageLayer p = street(0, 0, 120, 0);
  p.houses = 6;
  const VillagePlan plan = planned(p, bank);
  // The left row's footprints reach z = 5.05 + 4.6: on the bank, 0.6 * 3.65 m = 2.2 m of rise.
  CHECK(plan.steep > 0);
  CHECK(plan.built == 6);
  for (const Plot& q : plan.plots) {
    if (q.side == 0) CHECK((q.state == PlotState::steep || q.state == PlotState::spare));
    if (q.state == PlotState::built) CHECK(q.side == 1);
  }
  // A pond over the first right-hand plots: those are wet, and the houses stand further along.
  scene::WaterBody pond;
  JsonValue pond_json;
  REQUIRE(parse_json(std::string("{\"name\":\"pond\",\"level_m\":5,\"shape\":{\"circle\":{\"center\":"
                                 "[8,0,-10],\"radius_m\":9}}}"),
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
  // One tread, and a pad and a path a house.
  CHECK(edits.size() == 1 + 2 * plan.built);
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
  CHECK(checked > 800);
  // The tread is level across its width at each station's top, away from the houses' paths.
  const LaneLine& lane = plan.lanes[0];
  for (u32 k = 3; k + 3 < lane.stations.size(); k += 5) {
    const LaneStation& s = lane.stations[k];
    CHECK(std::fabs(edited(s.x_mm, s.z_mm) - static_cast<f64>(s.top_mm)) <= 25.0);
  }
}

TEST_CASE("village: the shapes cover the treads, the pads and the paths and nothing else") {
  const VillageLayer p = street(0, 0, 100, 30);
  const VillagePlan plan = planned(p, gentle);
  scene_gen::FootprintList all;
  footprints(p, plan, all);
  CHECK(all.size() == (plan.lanes[0].stations.size() - 1) + 2 * plan.built);
  scene_gen::Exclusion exclusion;
  std::string error;
  REQUIRE_MESSAGE(exclusion.build({}, {}, {}, &error), error);
  REQUIRE_MESSAGE(exclusion.add_footprints(all.span(), &error), error);
  const scene_gen::Ground g = ground(gentle);
  for (const Plot& q : plan.plots) {
    if (q.state != PlotState::built) continue;
    const House& h = plan.houses[q.house];
    CHECK(exclusion.excluded_mm(g, q.house_x_mm, q.house_z_mm));                  // the pad
    CHECK(exclusion.excluded_mm(g, (q.door_x_mm + q.path_x_mm) / 2,
                                (q.door_z_mm + q.path_z_mm) / 2));               // the path
    const WorldPos garden = house_point(q, 0, 0, h.max_z_mm + 3000);             // behind the pad
    CHECK(!exclusion.excluded_mm(g, scene_gen::nearest_mm(garden.x), scene_gen::nearest_mm(garden.z)));
  }
  const LaneStation& mid = plan.lanes[0].stations[plan.lanes[0].stations.size() / 2];
  CHECK(exclusion.excluded_mm(g, mid.x_mm, mid.z_mm));  // the tread

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

  // The treads as mask regions: one polygon a lane, two points a station.
  const JsonValue regions = tread_regions_json(p, plan, "trail");
  REQUIRE(regions.as_array().size() == 1);
  CHECK(regions[0].find("polygon")->as_array().size() == 2 * plan.lanes[0].stations.size());
  CHECK(regions[0].find("material")->as_string() == "trail");
}

TEST_CASE("village: the plan is the same bytes on every toolchain") {
  VillageLayer p = street(-40, 12, 60, -8);
  p.lanes[0].waypoints.push_back(WorldPos{110.0, 0.0, 40.0});
  Lane second;
  second.name = "back";
  second.rows = Rows::Left;
  second.waypoints.push_back(WorldPos{-30.0, 0.0, -70.0});
  second.waypoints.push_back(WorldPos{70.0, 0.0, -90.0});
  p.lanes.push_back(second);
  p.houses = 14;
  p.seed = 2026;
  const VillagePlan plan = planned(p, gentle);
  const u64 hash = plan_hash(plan);
  char text[24];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
  MESSAGE("plan hash " << text << ": " << plan.plots.size() << " plots, " << plan.built
                       << " houses, " << plan.steep << " steep");
  // Taken on MSVC (x86-64-v3); GCC and Clang in the Linux containers reproduce it.
  CHECK(hash == 0x0ull);
  // The same entry again, and the summary's hash, are the same.
  CHECK(plan_hash(planned(p, gentle)) == hash);
  CHECK(plan_json(p, plan).find("hash")->as_string() == std::string(text));
  // Another seed is another village.
  p.seed = 2027;
  CHECK(plan_hash(planned(p, gentle)) != hash);
}

TEST_CASE("village: the generator's tiles hand what its whole read hands, and it plans the ground") {
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
  // A tread, and a pad and a path a house; the same things as shapes, the tread a band a stretch.
  CHECK(planned.edits.size() == 1 + 2 * whole.things);
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
          listed = listed || (m.path == tile.meshes[p.mesh].path &&
                              m.recipe == tile.meshes[p.mesh].recipe);
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
  // No house filler.
  p = street(0, 0, 100, 0);
  scene_gen::Context context;
  context.ground = ground(flat);
  VillagePlan plan;
  std::string error;
  CHECK(!plan_village(p, context.ground, context, nullptr, nullptr, plan, error));
  CHECK(error.find("house filler") != std::string::npos);
}

// The routes capability (docs/subsystems/routes.md): the grade cap and where it cannot hold, two
// routes merging, the plan's determinism and its file, the tread as the tile set writes it, the
// band footprints, the wear, and the refusals.
#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/routes/routes.h>
#include <domain/scene_gen/exclusion.h>
#include <domain/tileset/cuts.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace engine;
using namespace engine::routes;

namespace {

// Grounds of products, sums and one division, so the heights are the same bits on every toolchain.
// A plane climbing 40 % toward +z: steeper than the cap, so a route straight up it must traverse.
f64 steep_plane(const void*, i64 x, i64 z) noexcept {
  (void)x;
  return .4 * static_cast<f64>(z) / 1000.0;
}
// A side slope: 30 % falling toward -x, level along z.
f64 side_slope(const void*, i64 x, i64) noexcept { return .3 * static_cast<f64>(x) / 1000.0; }
// Gentle ground with a rounded hill: what the merge and the pins stand on.
f64 hill(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  const f64 r2 = (xx - 30) * (xx - 30) + (zz - 20) * (zz - 20);
  return .03 * xx + 6.0 / (1.0 + r2 / 300.0);
}

scene_gen::Ground ground(scene_gen::HeightFn fn) {
  scene_gen::Ground g;
  g.surface_fn = fn;
  return g;
}

Route route(const char* name, f64 x0, f64 z0, f64 x1, f64 z1) {
  Route r;
  r.name = name;
  r.waypoints.push_back(WorldPos{x0, 0.0, z0});
  r.waypoints.push_back(WorldPos{x1, 0.0, z1});
  return r;
}

RouteLayer region(f64 x0, f64 z0, f64 x1, f64 z1) {
  RouteLayer p;
  p.name = "test";
  p.region_min = {x0, z0};
  p.region_max = {x1, z1};
  return p;
}

// Every stretch between stations of a line, its rise against the cap times its run.
f64 worst_excess(const RouteNetwork& n, f64 cap, u32* steep_stretches = nullptr) {
  f64 worst = -1e30;
  for (const RouteLine& l : n.lines)
    for (u32 k = 0; k + 1 < l.stations.size(); ++k) {
      const Station& a = l.stations[k];
      const Station& b = l.stations[k + 1];
      const f64 run =
          std::sqrt((b.at.x - a.at.x) * (b.at.x - a.at.x) + (b.at.z - a.at.z) * (b.at.z - a.at.z));
      const f64 e = std::fabs(b.at.y - a.at.y) - cap * run;
      if (a.flags & k_station_steep) {
        if (steep_stretches) ++*steep_stretches;
        continue;
      }
      worst = std::max(worst, e);
    }
  return worst;
}

}  // namespace

// **The grade cap** (routes.md, "The plan"): straight up a 40 % plane under a 25 % cap the route
// traverses and turns back, and no stretch of its tread climbs more than the cap; in a region too
// narrow to traverse, the leg is planned again with steep steps allowed, and they are counted.
TEST_CASE("routes: a route keeps under the grade cap, and says where it cannot") {
  RouteLayer p = region(-40, -4, 40, 44);
  p.routes.push_back(route("up", 0, 0, 0, 40));
  RouteNetwork n;
  std::string error;
  REQUIRE_MESSAGE(plan_routes(p, ground(steep_plane), nullptr, n, error), error);
  CHECK(n.legs == 1);
  CHECK(n.steep_legs == 0);
  CHECK(n.unreached == 0);
  CHECK(n.over_cap_m == 0.0);
  REQUIRE(!n.lines.empty());
  CHECK(worst_excess(n, .25) <= 1e-9);
  // 16 m of rise at 25 % is at least 64 m of path, against 40 m straight up.
  CHECK(n.length_m >= 64.0);
  CHECK(n.switchbacks >= 1);
  for (const RouteLine& l : n.lines)
    CHECK(l.steep_steps == 0);
  MESSAGE("up a 40 % plane under a 25 % cap: " << n.length_m << " m, " << n.switchbacks
                                               << " switchbacks, held at most "
                                               << n.lines[0].max_hold_m << " m");

  // Two spacings wide and a 10 % cap: no step up the plane is under it.
  RouteLayer narrow = region(-2, -4, 2, 44);
  narrow.grade_cap = .1f;
  narrow.routes.push_back(route("up", 0, 0, 0, 40));
  RouteNetwork m;
  REQUIRE_MESSAGE(plan_routes(narrow, ground(steep_plane), nullptr, m, error), error);
  CHECK(m.steep_legs == 1);
  CHECK(m.unreached == 0);
  CHECK(m.over_cap_m > 0.0);
  u32 steep_stretches = 0;
  worst_excess(m, .1, &steep_stretches);
  CHECK(steep_stretches > 0);
}

// **Two routes to one place merge** (routes.md, "The plan"): the second walks the first's steps
// for half their time, so the two share a trunk, the network has a junction, and it is shorter than
// the two planned alone.
TEST_CASE("routes: two routes to one place merge") {
  RouteLayer p = region(-50, -30, 50, 50);
  p.routes.push_back(route("a", -44, -20, 40, 40));
  p.routes.push_back(route("b", -44, 10, 40, 40));
  RouteNetwork both;
  std::string error;
  REQUIRE_MESSAGE(plan_routes(p, ground(hill), nullptr, both, error), error);
  f64 alone = 0;
  for (u32 k = 0; k < 2; ++k) {
    RouteLayer one = p;
    one.routes.clear();
    one.routes.push_back(p.routes[k]);
    RouteNetwork n;
    REQUIRE_MESSAGE(plan_routes(one, ground(hill), nullptr, n, error), error);
    alone += n.length_m;
  }
  CHECK(both.lines.size() == 3);  // two branches and the trunk they share
  CHECK(both.length_m < .85 * alone);
  MESSAGE("merged " << both.length_m << " m against " << alone << " m alone");
}

// **The same plan from any thread count and any host** (routes.md, "Determinism"): the file's bytes
// with no jobs and with six workers, through `ensure_plan` from contexts that differ in everything
// but the ground and the layer; and pinned, so the containers reproduce MSVC's bytes.
TEST_CASE("routes: the plan is the same bytes from any thread count and any host, and pinned") {
  RouteLayer p = region(-50, -30, 50, 50);
  p.routes.push_back(route("a", -44, -20, 40, 40));
  p.routes.push_back(route("b", -44, 10, 40, 40));
  p.routes.push_back(route("c", 40, -24, -10, 46));
  std::string error;
  RoutePlan serial, parallel;
  REQUIRE_MESSAGE(plan_routes(p, ground(hill), nullptr, serial.network, error), error);
  jobs::JobSystemConfig config;
  config.performance_workers = 6;
  config.efficiency_workers = 0;
  jobs::JobSystem js(config);
  REQUIRE_MESSAGE(plan_routes(p, ground(hill), &js, parallel.network, error), error);
  const std::string a = plan_bytes(serial), b = plan_bytes(parallel);
  CHECK(a == b);

  test::TempDir scratch;
  std::string dir;
  REQUIRE_MESSAGE(derived_dir(scratch.path(), scratch.path(), dir, error), error);
  scene_gen::Context near_origin, elsewhere;
  near_origin.ground = elsewhere.ground = ground(hill);
  near_origin.tile_size = 32;
  elsewhere.tile_size = 64;
  elsewhere.ring = 2;
  elsewhere.where = "another host";
  RoutePlan one, two;
  REQUIRE_MESSAGE(ensure_plan(p, near_origin, dir, one, error), error);
  test::TempDir other;
  std::string dir2;
  REQUIRE_MESSAGE(derived_dir(other.path(), other.path(), dir2, error), error);
  REQUIRE_MESSAGE(ensure_plan(p, elsewhere, dir2, two, error), error);
  CHECK(one.planned);
  CHECK(two.planned);
  CHECK(one.key == two.key);
  CHECK(plan_bytes(one) == plan_bytes(two));

  // The pins: the plan's bytes and the treads' edits as JSON, taken on MSVC (x86-64-v3).
  JsonWriteOptions compact;
  compact.pretty = false;
  const std::string edits = write_json(tread_edits_json(p, serial.network), compact);
  const u64 plan_hash = hash_bytes(a.data(), a.size());
  const u64 edit_hash = hash_bytes(edits.data(), edits.size());
  MESSAGE("plan " << plan_hash << " edits " << edit_hash << " key " << plan_key_text(one.key) << " "
                  << serial.network.length_m << " m");
  CHECK(plan_hash == 9261413012588863944ull);
  CHECK(edit_hash == 8388828807951059492ull);
}

// **One plan** (routes.md, "One plan"): made once and written; asked again, read; named, read
// whatever the ground; named and missing, or made from another layer, refused with the command.
TEST_CASE("routes: one plan, read again and named on its own treads") {
  test::TempDir scratch;
  std::string dir, error;
  REQUIRE_MESSAGE(derived_dir(scratch.path(), scratch.path(), dir, error), error);
  scene_gen::Context c;
  c.ground = ground(hill);
  RouteLayer p = region(-50, -30, 50, 50);
  p.routes.push_back(route("a", -44, -20, 40, 40));
  const u64 before = plans_made();
  RoutePlan first, again, named;
  REQUIRE_MESSAGE(ensure_plan(p, c, dir, first, error), error);
  CHECK(first.planned);
  CHECK(plans_made() == before + 1);
  CHECK(io::exists(plan_path(dir, first.key)));
  REQUIRE_MESSAGE(ensure_plan(p, c, dir, again, error), error);
  CHECK_FALSE(again.planned);
  CHECK(plans_made() == before + 1);
  CHECK(plan_bytes(again) == plan_bytes(first));
  // Named, on another ground (the treads changed it): read, never planned.
  RouteLayer on_treads = p;
  on_treads.plan = plan_key_text(first.key);
  scene_gen::Context cut = c;
  cut.ground = ground(side_slope);
  REQUIRE_MESSAGE(ensure_plan(on_treads, cut, dir, named, error), error);
  CHECK_FALSE(named.planned);
  CHECK(plans_made() == before + 1);
  CHECK(plan_bytes(named) == plan_bytes(first));
  // Made from another layer, or missing, or not a key: refused with a sentence.
  RoutePlan refused;
  RouteLayer wider = on_treads;
  wider.tread_width_m = 1.0f;
  CHECK_FALSE(ensure_plan(wider, cut, dir, refused, error));
  CHECK(error.find("another layer") != std::string::npos);
  RouteLayer missing = p;
  missing.plan = "0123456789abcdef";
  CHECK_FALSE(ensure_plan(missing, cut, dir, refused, error));
  CHECK(error.find("engine-content routes") != std::string::npos);
  missing.plan = "nonsense";
  CHECK_FALSE(ensure_plan(missing, cut, dir, refused, error));
  // A torn file is refused, not misread.
  const std::string bytes = plan_bytes(first);
  CHECK(read_plan(bytes, refused, error));
  CHECK_FALSE(read_plan(std::string_view(bytes).substr(0, bytes.size() / 2), refused, error));
}

// **The tread on the ground** (routes.md, "The tread"): a route along the contour of a 30 % side
// slope, its edits built by the tile set as the scene would build them. Across the tread the
// edited ground is flat at the station's top to the millimetre (a full bench: the top is the
// downhill edge's ground, sunk); the uphill face is cut and the downhill edge filled no more than
// the sink; and two metres past the tread's edges the ground is the ground.
TEST_CASE("routes: the tread is benched flat across a side slope and meets the ground") {
  RouteLayer p = region(-10, -6, 10, 66);
  p.routes.push_back(route("contour", 0, 0, 0, 60));
  RouteNetwork n;
  std::string error;
  REQUIRE_MESSAGE(plan_routes(p, ground(side_slope), nullptr, n, error), error);
  REQUIRE(n.lines.size() == 1);
  const JsonValue list = tread_edits_json(p, n);
  REQUIRE(list.as_array().size() == 1);
  Vector<scene::TerrainEdit> edits;
  for (const JsonValue& e : list.as_array()) {
    scene::TerrainEdit edit;
    schema::ReadContext reader;
    REQUIRE(schema::from_json(edit, e, reader));
    REQUIRE(reader.ok());
    edits.push_back(edit);
  }
  CHECK(edits[0].mode == scene::TerrainEditMode::Bench);
  CHECK(edits[0].points.size() < n.lines[0].stations.size());  // thinned on the straight
  tileset::TerrainEdits built;
  REQUIRE_MESSAGE(built.build({edits.data(), edits.size()}, &error), error);
  const f64 half = .5 * static_cast<f64>(p.tread_width_m);
  const auto edited = [&](f64 x, f64 z) {
    const f64 g = side_slope(nullptr, scene_gen::nearest_mm(x), 0) * 1000.0;
    f64 v = g;
    if (!built.edited_mm(x * 1000.0, z * 1000.0, 50, g, v)) v = g;
    return v / 1000.0;
  };
  u32 checked = 0;
  for (const Station& s : n.lines[0].stations) {
    if (s.at.z < 5 || s.at.z > 55) continue;  // away from the caps at the ends
    // The tread: flat at the top, which is the downhill edge's ground sunk 4 cm.
    CHECK(std::fabs(s.at.y - (.3 * (s.at.x - half) - .04)) <= .0011);
    for (f64 across = -half + .02; across <= half - .02; across += .05)
      CHECK(std::fabs(edited(s.at.x + across, s.at.z) - s.at.y) <= .0011);
    // Cut uphill, filled downhill by no more than the sink.
    CHECK(edited(s.at.x + half + .2, s.at.z) <
          side_slope(nullptr, scene_gen::nearest_mm(s.at.x + half + .2), 0));
    CHECK(edited(s.at.x - half, s.at.z) >= .3 * (s.at.x - half) - .0411);
    // Two metres past each edge, the ground as it was; and no step of a wall between.
    for (const f64 side : {-1.0, 1.0}) {
      const f64 x = s.at.x + side * (half + 2.0);
      CHECK(std::fabs(edited(x, s.at.z) - .3 * x) <= .0011);
      f64 prev = edited(s.at.x, s.at.z);
      for (f64 d = .05; d <= half + 2.0; d += .05) {
        const f64 v = edited(s.at.x + side * d, s.at.z);
        CHECK(std::fabs(v - prev) <= .06);  // at most 1.2 rise over run across 5 cm
        prev = v;
      }
    }
    ++checked;
  }
  CHECK(checked > 50);
}

// **The band footprints** (routes.md, "The tread"): the tread and nothing else. A point a few
// millimetres inside either edge is covered, one a centimetre outside is not, on every station of a
// curving line; and the exclusion that holds the bands excludes the same.
TEST_CASE("routes: the band footprints exclude the tread and nothing else") {
  RouteLayer p = region(-50, -30, 50, 50);
  p.routes.push_back(route("a", -44, -20, 40, 40));
  RouteNetwork n;
  std::string error;
  REQUIRE_MESSAGE(plan_routes(p, ground(hill), nullptr, n, error), error);
  Vector<scene_gen::Footprint> bands;
  tread_footprints(p, n, bands);
  REQUIRE(!bands.empty());
  scene_gen::Exclusion exclusion;
  REQUIRE(exclusion.build({}, {}, {}, &error));
  REQUIRE_MESSAGE(exclusion.add_footprints({bands.data(), bands.size()}, &error), error);
  const scene_gen::Ground g = ground(hill);
  const f64 half = .5 * static_cast<f64>(p.tread_width_m);
  const auto covered = [&](f64 x, f64 z) {
    const i64 xm = scene_gen::nearest_mm(x), zm = scene_gen::nearest_mm(z);
    bool any = false;
    for (const scene_gen::Footprint& f : bands)
      any = any || f.covers_mm(xm, zm);
    CHECK(any == exclusion.excluded_mm(g, xm, zm, scene_gen::k_exclude_footprints));
    return any;
  };
  u32 inside = 0, outside = 0;
  for (const RouteLine& l : n.lines)
    for (u32 k = 1; k + 1 < l.stations.size(); ++k) {
      const Station& a = l.stations[k - 1];
      const Station& b = l.stations[k + 1];
      f64 tx = b.at.x - a.at.x, tz = b.at.z - a.at.z;
      const f64 tl = std::sqrt(tx * tx + tz * tz);
      tx /= tl;
      tz /= tl;
      const Station& s = l.stations[k];
      for (const f64 side : {-1.0, 1.0}) {
        // The bands are chords within 5 mm of the line, their ends rounded to the millimetre.
        const f64 in = half - .008, out = half + .012;
        inside += covered(s.at.x - side * tz * in, s.at.z + side * tx * in) ? 1u : 0u;
        outside += covered(s.at.x - side * tz * out, s.at.z + side * tx * out) ? 0u : 1u;
      }
    }
  const u32 stations = n.lines[0].stations.size();
  CHECK(inside == 2 * (stations - 2));
  CHECK(outside == 2 * (stations - 2));
}

// **Wear** (routes.md, "Wear"): the cover left falls monotonically toward the tread, is the
// tread's own on it and whole past the reach; and a network asked at a point answers the same.
TEST_CASE("routes: the trampling gradient is monotone and reaches the tread") {
  for (const f64 floor : {0.0, .2})
    for (const f64 reach : {.5, 2.0, 6.0}) {
      f64 prev = wear_factor(-1.0, reach, floor);
      CHECK(prev == floor);
      for (f64 d = -1.0; d <= reach + 1.0; d += .01) {
        const f64 w = wear_factor(d, reach, floor);
        CHECK(w >= prev);
        CHECK(w >= floor);
        CHECK(w <= 1.0);
        prev = w;
      }
      CHECK(wear_factor(reach, reach, floor) == 1.0);
    }
  CHECK(wear_factor(1.0, 0.0, .3) == 1.0);  // no reach: the cover stops at the edge

  RouteLayer p = region(-10, -6, 10, 66);
  p.routes.push_back(route("contour", 0, 0, 0, 60));
  RouteNetwork n;
  std::string error;
  REQUIRE_MESSAGE(plan_routes(p, ground(side_slope), nullptr, n, error), error);
  RouteWear wear;
  wear.build(p, n);
  REQUIRE(!wear.empty());
  const f64 x0 = n.lines[0].stations[n.lines[0].stations.size() / 2].at.x;
  f64 prev = -1;
  for (f64 d = 0; d <= 4.0; d += .05) {
    const f64 c = wear.cover_mm(scene_gen::nearest_mm(x0 + d), 30000);
    CHECK(c >= prev);
    prev = c;
  }
  CHECK(wear.cover_mm(scene_gen::nearest_mm(x0), 30000) == 0.0);
  CHECK(wear.cover_mm(scene_gen::nearest_mm(x0 + 2.5), 30000) == 1.0);
  CHECK(wear.edge_distance_mm(scene_gen::nearest_mm(x0 + 1.3), 30000, 5.0) ==
        doctest::Approx(1.0).epsilon(.01));
  CHECK(wear.edge_distance_mm(scene_gen::nearest_mm(x0 + 40), 30000, 5.0) > 1e20);
}

// **Water** (routes.md, "Water"): a stream across the way is forded where the route crosses its
// corridor, square to it when fording costs more than walking, and the crossing is listed where a
// bridge would stand; a planned exclusion enters the key.
TEST_CASE("routes: a route fords a stream short and lists the crossing") {
  scene::WaterChannel stream;
  stream.name = "stream";
  stream.points.push_back(WorldPos{-60, 0, 20});
  stream.points.push_back(WorldPos{60, 0, 20});
  stream.width_m = {3.0f, 3.0f};
  stream.depth_m = {.5f, .5f};
  stream.slope = {.01f, .01f};
  scene_gen::Exclusion water;
  std::string error;
  REQUIRE_MESSAGE(water.build({}, {}, {&stream, 1}, &error), error);
  const auto plan = [&](f32 ford, RouteNetwork& n) {
    RouteLayer p = region(-50, -4, 50, 44);
    p.ford_penalty = ford;
    p.routes.push_back(route("across", -36, 0, 36, 40));
    REQUIRE_MESSAGE(plan_routes(p, ground(hill), nullptr, n, error, &water), error);
  };
  RouteNetwork wading, square;
  plan(1.0f, wading);
  plan(4.0f, square);
  CHECK(square.fords == 1);
  CHECK(wading.fords == 1);
  // The corridor is 5.3 m across (the stream's 3 m and a bank of 0.25 widths and 0.4 m each side);
  // a ford counts the stretch on each side that touches it (a station, 0.5 m). Within 30 degrees of
  // square: at most 5.3 / cos 30 + 1 m.
  CHECK(square.ford_m < wading.ford_m);
  CHECK(square.ford_m <= 5.3 / .8660 + 1.0);
  const JsonValue crossings = crossings_json(square);
  REQUIRE(crossings.as_array().size() == 1);
  const JsonValue& at = *crossings[0].find("at");
  CHECK(std::fabs(at[2].as_float() - 20.0) <= 3.0);
  MESSAGE("fording: " << square.ford_m << " m at 4x, " << wading.ford_m << " m at 1x");
  // A pond on the way: gone round, never entered (a 6 m circle whose level stands over the ground).
  scene::WaterBody pond;
  pond.name = "pond";
  pond.level_m = 100;
  pond.shape.circle = scene::WaterCircle{};
  pond.shape.circle->center = WorldPos{0, 0, 10};
  pond.shape.circle->radius_m = 6.0f;
  scene_gen::Exclusion lake;
  REQUIRE_MESSAGE(lake.build({}, {&pond, 1}, {}, &error), error);
  RouteNetwork round;
  {
    RouteLayer p = region(-50, -4, 50, 44);
    p.routes.push_back(route("past", -36, 10, 36, 10));
    REQUIRE_MESSAGE(plan_routes(p, ground(hill), nullptr, round, error, &lake), error);
  }
  CHECK(round.unreached == 0);
  f64 nearest = 1e30;
  for (const RouteLine& l : round.lines)
    for (const Station& s : l.stations)
      nearest = std::min(nearest, std::sqrt(s.at.x * s.at.x + (s.at.z - 10) * (s.at.z - 10)));
  CHECK(nearest >= 6.0);
  MESSAGE("nearest station to the pond's centre: " << nearest << " m");
  // No water handed: no ford, and the stream is crossed as dry ground.
  RouteNetwork dry;
  RouteLayer p = region(-50, -4, 50, 44);
  p.routes.push_back(route("across", -36, 0, 36, 40));
  REQUIRE_MESSAGE(plan_routes(p, ground(hill), nullptr, dry, error), error);
  CHECK(dry.fords == 0);
  CHECK(dry.wet.empty());
}

TEST_CASE("routes: a layer out of range is refused with the field") {
  std::string error;
  RouteLayer p = region(-10, -10, 10, 10);
  p.routes.push_back(route("a", -5, -5, 5, 5));
  CHECK(layer_in_range(p, error));
  const auto refused = [&](RouteLayer q, const char* field) {
    CHECK_FALSE(layer_in_range(q, error));
    CHECK_MESSAGE(error.find(field) != std::string::npos, error);
  };
  RouteLayer q = p;
  q.grade_cap = 0;
  refused(q, "grade_cap");
  q = p;
  q.tread_width_m = 3;
  refused(q, "tread_width_m");
  q = p;
  q.region_max = {11, 10};
  refused(q, "region");
  q = p;
  q.routes[0].waypoints.pop_back();
  refused(q, "routes");
  q = p;
  q.max_samples = 10;
  RouteNetwork n;
  CHECK_FALSE(plan_routes(q, ground(hill), nullptr, n, error));
  CHECK(error.find("max_samples") != std::string::npos);
  CHECK_FALSE(plan_routes(p, scene_gen::Ground{}, nullptr, n, error));
}

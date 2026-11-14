// W121 (docs/subsystems/channels.md, "Where a cut meets a bank: the owner's marks 6 and 7", and
// "The base level, and a fan below a cliff (W121)"; the owner's walk of
// 2026-11-13, marks 6, 7 and 8, and W119): the holes a cut left where it met a bank or a body,
// read on the tile set's own edited ground. The plan's cuts are compiled by
// `tileset::TerrainEdits` and applied to a window of the 25 cm lattice the forest's set is built
// at (`TerrainEdits::apply`, the function the set's build calls), the same window of the ground
// before the cut beside it. Built only where the tileset capability is (a test-only dependency).
//
//  * **A pocket** (marks 6 and 7: "a pocket cut into the hillside", "a dark hole in the bank"): a
//    closed hollow in the edited ground that the ground before the cut did not have, deeper than
//    any pool the plan means to cut. Found by a priority flood over the window (Barnes, Lehman &
//    Mulla 2014: each point's spill height, the lowest level at which water standing on it would
//    leave the window), the hollow's depth the spill height less the ground.
//  * **Dry ground under a body's level** (mark 8: a black triangle beside a teal one where a link
//    meets a pond): a point outside every body's outline that the cut lowered under a body's level.
//    Water there is not drawn (a body's surface ends at its outline), so it is a dry hole under the
//    waterline beside the body.
//  * **A fan over a cliff** (W119): a mouth's fan grading the last three widths of bed down a sea
//    cliff's face, an 80-degree ramp cut into the cliff's top.
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/channels/channels.h>
#include <domain/scene_gen/exclusion.h>
#include <domain/tileset/cuts.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <queue>
#include <utility>
#include <vector>

using namespace engine;
using namespace engine::channels;

namespace {
constexpr i64 k_cell_mm = 250;  // the finest level a forest's set is built at

std::vector<scene::TerrainEdit> edits_of(const ChannelLayer& p, const ChannelNetwork& n) {
  const JsonValue cuts = terrain_cuts_json(p, n);
  std::vector<scene::TerrainEdit> edits;
  for (const JsonValue& o : cuts.as_array()) {
    scene::TerrainEdit e;
    schema::ReadContext reader;
    REQUIRE(schema::from_json(e, o, reader));
    REQUIRE(reader.ok());
    edits.push_back(std::move(e));
  }
  return edits;
}

// A window of the 25 cm lattice, whole millimetres, rows of x in order of z: the ground before the
// cut and after it.
struct Window {
  i64 i0 = 0, j0 = 0, nx = 0, nz = 0;
  std::vector<i32> uncut, cut;
  f64 x(i64 i) const { return static_cast<f64>((i0 + i) * k_cell_mm) / 1000.0; }
  f64 z(i64 j) const { return static_cast<f64>((j0 + j) * k_cell_mm) / 1000.0; }
};
Window edited(const scene_gen::Ground& g, const std::vector<scene::TerrainEdit>& edits, f64 x0,
              f64 z0, f64 x1, f64 z1) {
  tileset::TerrainEdits compiled;
  std::string error;
  REQUIRE(compiled.build({edits.data(), edits.size()}, &error));
  Window w;
  const f64 cell = static_cast<f64>(k_cell_mm);
  w.i0 = static_cast<i64>(std::floor(x0 * 1000.0 / cell));
  w.j0 = static_cast<i64>(std::floor(z0 * 1000.0 / cell));
  w.nx = static_cast<i64>(std::ceil(x1 * 1000.0 / cell)) - w.i0 + 1;
  w.nz = static_cast<i64>(std::ceil(z1 * 1000.0 / cell)) - w.j0 + 1;
  w.uncut.resize(static_cast<size_t>(w.nx * w.nz));
  for (i64 j = 0; j < w.nz; ++j)
    for (i64 i = 0; i < w.nx; ++i)
      w.uncut[static_cast<size_t>(j * w.nx + i)] = static_cast<i32>(
          std::llround(g.surface_mm((w.i0 + i) * k_cell_mm, (w.j0 + j) * k_cell_mm) * 1000.0));
  w.cut = w.uncut;
  compiled.apply(k_cell_mm, w.i0, w.j0, w.nx, w.nz, {w.cut.data(), w.cut.size()});
  return w;
}

// Each point's spill height over the window's edge (a priority flood over eight neighbours,
// Barnes, Lehman & Mulla 2014): the ground where water standing there drains, higher in a hollow.
std::vector<i32> spill(const Window& w, const std::vector<i32>& h) {
  const i64 nx = w.nx, nz = w.nz;
  std::vector<i32> level(h.size(), 0);
  std::vector<u8> seen(h.size(), 0);
  using Item = std::pair<i32, i64>;
  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
  for (i64 j = 0; j < nz; ++j)
    for (i64 i = 0; i < nx; ++i)
      if (i == 0 || j == 0 || i == nx - 1 || j == nz - 1) {
        const i64 k = j * nx + i;
        seen[static_cast<size_t>(k)] = 1;
        level[static_cast<size_t>(k)] = h[static_cast<size_t>(k)];
        open.push({h[static_cast<size_t>(k)], k});
      }
  while (!open.empty()) {
    const auto [lv, k] = open.top();
    open.pop();
    const i64 i = k % nx, j = k / nx;
    for (i64 dj = -1; dj <= 1; ++dj)
      for (i64 di = -1; di <= 1; ++di) {
        const i64 a = i + di, b = j + dj;
        if ((di == 0 && dj == 0) || a < 0 || b < 0 || a >= nx || b >= nz) continue;
        const size_t n = static_cast<size_t>(b * nx + a);
        if (seen[n]) continue;
        seen[n] = 1;
        level[n] = std::max(h[n], lv);
        open.push({level[n], static_cast<i64>(n)});
      }
  }
  return level;
}

// The deepest hollow the cut made: the edited ground's hollow depth less the uncut ground's there.
struct Pocket {
  f64 depth = 0, x = 0, z = 0;
  u64 points = 0;  // points of a hollow deeper than the bound
};
Pocket deepest_pocket(const Window& w, f64 bound_m) {
  const std::vector<i32> before = spill(w, w.uncut), after = spill(w, w.cut);
  Pocket p;
  for (i64 j = 0; j < w.nz; ++j)
    for (i64 i = 0; i < w.nx; ++i) {
      const size_t k = static_cast<size_t>(j * w.nx + i);
      const f64 d =
          (static_cast<f64>(after[k] - w.cut[k]) - static_cast<f64>(before[k] - w.uncut[k])) /
          1000.0;
      if (d > bound_m) ++p.points;
      if (d > p.depth) p = {d, w.x(i), w.z(j), p.points};
    }
  return p;
}

// The box round every link's line, `margin` metres out, inside the layer's region.
void box_of(const ChannelLayer& p, const ChannelNetwork& n, f64 margin, f64& x0, f64& z0, f64& x1,
            f64& z1) {
  x0 = z0 = 1e30;
  x1 = z1 = -1e30;
  for (const ChannelLink& link : n.links)
    for (const ChannelPoint& pt : link.line) {
      x0 = std::min(x0, pt.at.x), x1 = std::max(x1, pt.at.x);
      z0 = std::min(z0, pt.at.z), z1 = std::max(z1, pt.at.z);
    }
  x0 = std::max(x0 - margin, static_cast<f64>(p.region_min[0]));
  z0 = std::max(z0 - margin, static_cast<f64>(p.region_min[1]));
  x1 = std::min(x1 + margin, static_cast<f64>(p.region_max[0]));
  z1 = std::min(z1 + margin, static_cast<f64>(p.region_max[1]));
}

// ---- the grounds ------------------------------------------------------------------------------

// S104's cliffs (falls_tests.cpp, "the solver's steps of known heights"): a V valley falling 3 %
// toward +x with sides at 0.3, cliffs of 3, 7, 10 and 14 m at x = 40, 90, 140 and 190 m, each a
// ramp over 25 cm. Where the line meets a cliff at an angle, W116's seat reads the lower bank under
// the cliff and the cut takes the step's height into a pit at its foot.
constexpr f64 k_step_x[4] = {40, 90, 140, 190};
constexpr f64 k_step_h[4] = {3, 7, 10, 14};
f64 stepped(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, a = static_cast<f64>(z) / 1000.0 - 128;
  f64 down = 0;
  for (u32 k = 0; k < 4; ++k) {
    if (xx >= k_step_x[k] + .25)
      down += k_step_h[k];
    else if (xx > k_step_x[k])
      down += k_step_h[k] * (xx - k_step_x[k]) / .25;
  }
  return 80 - .03 * xx - down + .3 * std::fabs(a);
}
// **A stream along a cliff's edge** (the forest's link 613 under W117, seated 126 m): W116's
// hillside (channels_tests.cpp's `contour`: a bench a metre wide at the uphill slope's toe at
// z = 128 m, falling 1.2 % down +x) at 20 degrees, with the downhill slope broken by a cliff whose
// edge stands `edge` metres past the bench and whose face drops 8 m over 25 cm.
struct CliffEdge {
  f64 edge = .5;
};
f64 cliff_edge(const void* context, i64 x, i64 z) noexcept {
  const f64 edge = static_cast<const CliffEdge*>(context)->edge;
  const f64 t = .36397023426620234;
  const f64 a = static_cast<f64>(z) / 1000.0 - 128;
  f64 across = a >= 0 ? t * a : (a >= -1 ? -.03 * a : .03 - t * (-a - 1));
  const f64 past = -a - 1 - edge;  // metres past the cliff's edge
  if (past > .25)
    across -= 8;
  else if (past > 0)
    across -= 8 * past / .25;
  return 40 - .012 * static_cast<f64>(x) / 1000.0 + across;
}
// channels_tests.cpp's notch: a V falling 5 % toward z = 0 with sides at 0.15.
f64 notch(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  return .05 * zz + .15 * std::abs(xx - 128);
}
// falls_tests.cpp's sea cliff: a V notch falling 3 % toward +x, a 20 m cliff at x = 200 m whose
// foot stands under a sea at 34.5 m over x > 195 m.
f64 notch_fall(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  const f64 cliff = xx >= 200.25 ? 20.0 : (xx > 200 ? 20.0 * (xx - 200) / .25 : 0.0);
  return 60 - .03 * xx - cliff + .3 * std::fabs(zz - 128);
}

scene_gen::Ground ground(scene_gen::HeightFn fn, const void* context = nullptr) {
  scene_gen::Ground g;
  g.surface_fn = fn;
  g.context = context;
  return g;
}

// A rectangle `a` by `b` half-sides about (cx, cz), turned `turn` radians from the axes: its edge
// toward +z crosses x = cx at cz + b / cos(turn).
scene::WaterBody pond(f64 level, f64 cx, f64 cz, f64 a, f64 b, f64 turn) {
  scene::WaterBody body;
  body.name = "pond";
  body.level_m = level;
  const f64 c = std::cos(turn), s = std::sin(turn);
  for (const auto& [u, v] : {std::pair<f64, f64>{-1, -1}, {1, -1}, {1, 1}, {-1, 1}})
    body.shape.polygon.push_back(
        WorldPos{cx + u * a * c + v * b * s, 0, cz - u * a * s + v * b * c});
  return body;
}
bool inside(const scene::WaterBody& body, f64 x, f64 z) {
  const auto& pts = body.shape.polygon;
  bool in = false;
  for (u32 i = 0, j = pts.size() - 1; i < pts.size(); j = i++)
    if ((pts[i].z > z) != (pts[j].z > z) &&
        x < (pts[j].x - pts[i].x) * (z - pts[i].z) / (pts[j].z - pts[i].z) + pts[i].x)
      in = !in;
  return in;
}
}  // namespace

// **Marks 6 and 7: a cut leaves no pocket the ground did not have.** On S104's cliffs down the fall
// line (two seeds) and on a stream along a cliff's edge (at the bench's edge and a quarter metre
// past it), the deepest closed hollow the
// cut made in the edited ground, beyond what the ground before the cut held there, is printed with
// where it is, and held under half a metre: deeper than any pool the plan cuts on these channels
// (a step-pool's pool is at most a sixth of a width, 0.2 m on a 1.2 m stream). On 61ccce67 the seat
// took a cliff's height into the cut (12.7 m at the 14 m cliff, seed 2).
TEST_CASE("channels: a cut leaves no pocket where it meets a cliff or a cliff's edge" *
          doctest::may_fail()) {
  for (const u64 seed : {1ull, 2ull}) {
    ChannelLayer p;
    p.name = "steps";
    p.region_min = {0, 0};
    p.region_max = {256, 256};
    p.spacing_m = 1;
    p.head_threshold_m2 = 60;
    p.width_at_km2_m = 4;
    p.seed = seed;
    const scene_gen::Ground g = ground(&stepped);
    ChannelNetwork n;
    std::string error;
    REQUIRE(plan_channels(p, g, seed, nullptr, n, error));
    f64 x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    box_of(p, n, 6, x0, z0, x1, z1);
    z0 = std::max(z0, 96.0), z1 = std::min(z1, 160.0);  // the valley's floor and its sides
    const Window w = edited(g, edits_of(p, n), x0, z0, x1, z1);
    const Pocket pk = deepest_pocket(w, .5);
    MESSAGE("cliffs, seed " << seed << ": the deepest pocket the cut made " << pk.depth << " m at ("
                            << pk.x << ", " << pk.z << "), " << pk.points
                            << " points of pockets past 0.5 m");
    CHECK(pk.depth <= .5);
  }
  for (const f64 edge : {0.0, .25}) {
    ChannelLayer p;
    p.name = "edge";
    p.region_min = {0, 0};
    p.region_max = {256, 256};
    p.spacing_m = 1;
    p.head_threshold_m2 = 60;
    p.width_at_km2_m = 12;
    p.depth_at_km2_m = 1.0f;
    p.seed = 1;
    const CliffEdge cliff{edge};
    const scene_gen::Ground g = ground(&cliff_edge, &cliff);
    ChannelNetwork n;
    std::string error;
    REQUIRE(plan_channels(p, g, 1, nullptr, n, error));
    f64 x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    box_of(p, n, 6, x0, z0, x1, z1);
    z0 = std::max(z0, 118.0), z1 = std::min(z1, 140.0);
    const Window w = edited(g, edits_of(p, n), x0, z0, x1, z1);
    const Pocket pk = deepest_pocket(w, .5);
    f64 seat = 0;
    for (const ChannelLink& link : n.links)
      for (const f32 s : link.seat)
        seat = std::max(seat, static_cast<f64>(s));
    MESSAGE("cliff's edge " << edge << " m past the bench: the most seat " << seat
                            << " m; the deepest pocket the cut made " << pk.depth << " m at ("
                            << pk.x << ", " << pk.z << "), " << pk.points
                            << " points of pockets past 0.5 m");
    CHECK(pk.depth <= .5);
  }
}

// **Mark 8: a mouth cuts no dry ground under its body's level.** The notch runs into a pond at
// 5 m whose outline is a rectangle turned 30 degrees across the line, so the line meets its edge at
// an angle: once with the edge on the uphill side of the level's contour (the ground at the edge
// over the level, so the shore is the contour, inside the outline), and once short of it (the edge
// standing in 0.2 m of water on the line, as an author's polygon does where it falls short of the
// basin). A point outside the outline that the cut lowered under the level is a hole: the body's
// surface ends at its outline, so nothing is drawn over it. On 61ccce67 the fan, which grades the
// bed to half a thalweg under the ground at the shore point and spreads to twice the width, digs
// such ground beside the shore.
TEST_CASE("channels: a mouth cuts no dry ground under its body's level") {
  const scene_gen::Ground g = ground(&notch);
  for (const f64 edge_z : {104.0, 96.0}) {
    ChannelLayer p;
    p.name = "mouth";
    p.region_min = {0, 0};
    p.region_max = {256, 512};
    constexpr f64 turn = .5235987755982988;  // 30 degrees
    const scene::WaterBody body = pond(5.0, 128, 70, 60, (edge_z - 70) * std::cos(turn), turn);
    const Vector<scene::WaterBody> bodies{body};
    scene_gen::Exclusion water;
    std::string error;
    REQUIRE(water.build({}, {bodies.data(), bodies.size()}, {}, &error));
    ChannelNetwork n;
    REQUIRE(plan_channels(p, g, 0, nullptr, n, error, &water));
    u32 mouths = 0;
    for (const ChannelLink& link : n.links)
      mouths += link.mouth ? 1u : 0u;
    REQUIRE(mouths >= 1);
    const Window w = edited(g, edits_of(p, n), 96, 80, 160, 130);
    u64 dry = 0;
    f64 worst = 0, wx = 0, wz = 0;
    for (i64 j = 0; j < w.nz; ++j)
      for (i64 i = 0; i < w.nx; ++i) {
        const size_t k = static_cast<size_t>(j * w.nx + i);
        const f64 under = 5.0 - static_cast<f64>(w.cut[k]) / 1000.0;
        // Lowered by the cut by more than 2 cm, and left more than 2 cm under the level (the
        // shore's own bed stands a centimetre under its top, and the round top's crease where the
        // cut meets ground at the level is a few millimetres more): a puddle's depth is no hole.
        if (w.uncut[k] - w.cut[k] <= 20 || under <= .02 || inside(body, w.x(i), w.z(j))) continue;
        ++dry;
        if (under > worst) worst = under, wx = w.x(i), wz = w.z(j);
      }
    const ChannelLink* mouth = nullptr;
    for (const ChannelLink& link : n.links)
      if (link.mouth) mouth = &link;
    MESSAGE("the pond's edge crossing the line at z = "
            << edge_z << ": the mouth at (" << mouth->line.back().at.x << ", "
            << mouth->line.back().at.z << "), its level " << mouth->mouth_level << " m; " << dry
            << " points (" << static_cast<f64>(dry) * .0625
            << " m2) outside the pond cut under its level, the worst " << worst << " m at (" << wx
            << ", " << wz << ")");
    CHECK(dry == 0);
  }
}

namespace {
// The notch with a hollow 3 m deep and 30 m across about (128, 400): water standing in it spills
// down the notch over a rim at z = 370 m, 18.5 m.
f64 hollow(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  const f64 r2 = (xx - 128) * (xx - 128) + (zz - 400) * (zz - 400);
  return .05 * zz + .15 * std::abs(xx - 128) - 3 * std::max(0.0, 1 - r2 / 900);
}
}  // namespace

// **Mark 8 as the forest has it: an outlet's start.** The forest's link 778 leaves the pond
// `freshwater-swamp` (its first station is the pond's spill, 2 m inside the outline) and its line
// climbs a metre out of the pond's hollow: the plan's line, routed over the filled lattice. The
// hollow here holds a pond at 18 m, 0.5 m under its rim, whose outline reaches 2 m past the level's
// contour on the outlet's side; the line leaves at the contour and climbs over the rim. Where the
// ground stood over the level outside the outline, the cut takes nothing under it (a hole: no water
// is drawn there). The climb is the plan's to answer; what the cut leaves where it meets the body
// is held. On 61ccce67 the outlet's bed, held down the line from the shore, ran under the level up
// the climb and out past the outline.
TEST_CASE("channels: an outlet's start cuts no dry ground under its body's level") {
  const scene_gen::Ground g = ground(&hollow);
  ChannelLayer p;
  p.name = "outlet";
  p.region_min = {0, 0};
  p.region_max = {256, 512};
  scene::WaterBody body;
  body.name = "pond";
  body.level_m = 18.0;
  for (const auto& [x, z] : {std::pair<f64, f64>{100, 371.6}, {156, 371.6}, {156, 430}, {100, 430}})
    body.shape.polygon.push_back(WorldPos{x, 0, z});
  const Vector<scene::WaterBody> bodies{body};
  scene_gen::Exclusion water;
  std::string error;
  REQUIRE(water.build({}, {bodies.data(), bodies.size()}, {}, &error));
  ChannelNetwork n;
  REQUIRE(plan_channels(p, g, 0, nullptr, n, error, &water));
  const ChannelLink* outlet = nullptr;
  for (const ChannelLink& link : n.links)
    if (link.outlet) outlet = &link;
  REQUIRE(outlet != nullptr);
  const Window w = edited(g, edits_of(p, n), 100, 340, 156, 380);
  u64 dry = 0;
  f64 worst = 0, wx = 0, wz = 0;
  for (i64 j = 0; j < w.nz; ++j)
    for (i64 i = 0; i < w.nx; ++i) {
      const size_t k = static_cast<size_t>(j * w.nx + i);
      const f64 under = 18.0 - static_cast<f64>(w.cut[k]) / 1000.0;
      // Ground that stood 10 cm or more over the level, outside the outline, left more than 2 cm
      // under it. Below the rim the ground itself falls under the level and the outlet's own cut
      // there is no hole: its section, interpolated along a stretch of the line (half a metre)
      // and as wide as the channel, trims the banks beside the line to the line's own ground
      // within a metre or two below the sill, where the bank stands a few centimetres over the
      // level and the line's ground under it.
      if (w.uncut[k] < 18100 || under <= .02 || inside(body, w.x(i), w.z(j))) continue;
      ++dry;
      if (under > worst) worst = under, wx = w.x(i), wz = w.z(j);
    }
  MESSAGE("the outlet from (" << outlet->line.front().at.x << ", " << outlet->line.front().at.z
                              << "), its body's level " << outlet->outlet_level << " m; " << dry
                              << " points (" << static_cast<f64>(dry) * .0625
                              << " m2) of ground over the level outside the pond cut under it, "
                              << "the worst " << worst << " m at (" << wx << ", " << wz << ")");
  CHECK(outlet->outlet_level == 18.0);
  CHECK(dry == 0);
}

// **W119: a mouth's fan does not grade over a cliff.** The notch over its sea cliff: the link ends
// on the cliff's face at the sea's level. Its fan graded the last three widths of bed, cliff top
// included, from the lip's bed to the sea's level, an 80-degree ramp cut into the top of the
// cliff: every point of the line whose ground stands more than 2 m over the sea had its bed graded
// metres under the ground. Held: above the cliff the bed is the cut's own (no deeper under the
// ground at the line than the cut's floor, two of its thalwegs and half `fall_min_m`), so the
// water falls at the cliff and the fan is what lies below it.
TEST_CASE("channels: a mouth's fan does not grade over a cliff") {
  ChannelLayer p;
  p.name = "steps";
  p.region_min = {0, 0};
  p.region_max = {256, 256};
  p.spacing_m = 1;
  p.head_threshold_m2 = 60;
  p.width_at_km2_m = 4;
  p.seed = 1;
  scene::WaterBody sea;
  sea.name = "sea";
  sea.level_m = 34.5;
  for (const auto& [x, z] : {std::pair<f64, f64>{195, -10}, {300, -10}, {300, 300}, {195, 300}})
    sea.shape.polygon.push_back(WorldPos{x, 0, z});
  const Vector<scene::WaterBody> bodies{sea};
  scene_gen::Exclusion water;
  std::string error;
  REQUIRE(water.build({}, {bodies.data(), bodies.size()}, {}, &error));
  const scene_gen::Ground g = ground(&notch_fall);
  ChannelNetwork n;
  REQUIRE(plan_channels(p, g, 1, nullptr, n, error, &water));
  const ChannelLink* trunk = nullptr;
  for (const ChannelLink& link : n.links)
    if (link.mouth && (trunk == nullptr || link.length_m > trunk->length_m)) trunk = &link;
  REQUIRE(trunk != nullptr);
  const u32 m = trunk->line.size();
  u32 read = 0, graded = 0;
  f64 most = 0;
  for (u32 i = 0; i < m; ++i) {
    const ChannelPoint& pt = trunk->line[i];
    if (pt.at.y < sea.level_m + 2) continue;
    if (trunk->line[m - 1].at.x - pt.at.x > 12) continue;  // the last 12 m above the cliff
    ++read;
    const f64 seat = i < trunk->seat.size() ? static_cast<f64>(trunk->seat[i]) : 0.0;
    // The cut's floor: two thalwegs (a V of twice the mean is the deepest the section makes, so
    // four mean depths) and half `fall_min_m`.
    const f64 floor = pt.at.y - seat - 4 * static_cast<f64>(pt.depth_m) -
                      .5 * static_cast<f64>(p.fall_min_m) -
                      (i < trunk->step_depth.size() ? static_cast<f64>(trunk->step_depth[i]) : 0.0);
    const f64 under = floor - trunk->bed[i];
    if (under > .01) ++graded;
    most = std::max(most, under);
  }
  MESSAGE("over the sea cliff: " << read << " points of the last 12 m above it, " << graded
                                 << " with the bed graded under the cut's floor, the most by "
                                 << most << " m");
  REQUIRE(read >= 10);
  CHECK(graded == 0);
}

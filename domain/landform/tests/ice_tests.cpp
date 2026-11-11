// Tests for the landform's ice (docs/subsystems/landform.md, "Ice"): under a glacier a valley's
// cross-section is a trough where a river leaves a V, a tributary's mouth hangs above its trunk's
// floor, a glacier that leaves its region is graded to the region's edge and one that ends inside
// it leaves the edge alone, what melts out of the ice is till, and the same bytes come from any
// number of workers. All CPU.
#include "test_sketches.h"

#include <core/jobs/job_system.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace engine;
using namespace landform_test;

namespace {

landform::Landform solved(const landform::Sketch& sketch, const landform::StartField* start,
                          const landform::SolveOptions& options = {}) {
  landform::Landform land;
  std::string error;
  REQUIRE_MESSAGE(landform::solve(sketch, start, options, land, error), error);
  return land;
}

template <class F>
landform::StartField start_from(u32 side_x, u32 side_z, i64 spacing_mm, F&& height) {
  landform::StartField start;
  start.width = side_x;
  start.height = side_z;
  start.spacing_mm = spacing_mm;
  start.data.resize(side_x * side_z);
  const f64 spacing = static_cast<f64>(spacing_mm) / 1000.0;
  for (u32 z = 0; z < side_z; ++z)
    for (u32 x = 0; x < side_x; ++x)
      start.data[z * side_x + x] =
          static_cast<f32>(height(spacing * static_cast<f64>(x), spacing * static_cast<f64>(z)));
  return start;
}

// A trunk valley falling east at 0.02 from 1,400 m between sides at 0.5 (a trunk is gentle, and
// how level a glacier wears its floor goes with how gentle: the cross fall it leaves is about
// twice the fall along), with a tributary valley from the north that meets it half way, falling
// at 0.12: 6.4 km by 4.8 km at 50 m.
constexpr u32 k_wide = 128, k_deep = 96, k_axis = 64, k_side = 64;
// The east edge is open and held at the trunk's own level there.
constexpr i32 k_edge_mm = 1273000;

landform::StartField two_valleys() {
  return start_from(k_wide, k_deep, 50000, [](f64 x, f64 z) {
    const f64 trunk = 1400.0 - 0.02 * x + 0.5 * std::fabs(z - 3200.0);
    if (z >= 3200.0) return trunk;
    const f64 mouth = 1400.0 - 0.02 * 3200.0;
    const f64 side = mouth + 0.12 * (3200.0 - z) + 0.5 * std::fabs(x - 3200.0);
    return side < trunk ? side : trunk;
  });
}

landform::Sketch two_valleys_sketch(bool with_ice) {
  landform::Sketch sketch = plain_sketch(k_wide, 50000);
  sketch.height = k_deep;
  sketch.edges.west = landform::EdgeKind::Closed;
  sketch.edges.north = landform::EdgeKind::Closed;
  sketch.edges.south = landform::EdgeKind::Closed;
  sketch.base_level_m = 1400.0 - 0.02 * 6350.0;
  sketch.border_reach_m = 0.0;
  sketch.start = "valleys.json";
  sketch.duration_yr = 100000;
  if (with_ice) {
    landform::Ice ice;
    // The line is under the trunk's floor (1,400 m to 1,273 m), so the valley fills.
    ice.ela_m = 1150.0;
    sketch.ice = ice;
  }
  return sketch;
}

// A column's width `up_mm` above its lowest point, in points.
u32 width_above(const landform::Landform& land, u32 x, i32 up_mm) {
  i32 lowest = land.height_mm[x];
  for (u32 z = 0; z < land.height; ++z)
    if (land.height_mm[z * land.width + x] < lowest) lowest = land.height_mm[z * land.width + x];
  u32 count = 0;
  for (u32 z = 0; z < land.height; ++z)
    if (land.height_mm[z * land.width + x] <= lowest + up_mm) ++count;
  return count;
}

// The stated measure of a cross-section's shape: its width half way up its lowest hundred
// metres over its width at the hundred. A V of straight sides is 0.5, a parabola (Svensson
// 1959's fit of a glacial trough) 0.71, a floor between upright walls 1.
f64 trough_share(const landform::Landform& land, u32 x) {
  return static_cast<f64>(width_above(land, x, 50000)) /
         static_cast<f64>(width_above(land, x, 100000));
}
// The largest fall between two points that follow each other down the tributary's axis, from a
// kilometre above the trunk to it, millimetres.
i32 steepest_step(const landform::Landform& land, u32* row = nullptr) {
  i32 step = 0;
  for (u32 z = k_axis - 20; z < k_axis; ++z) {
    const i32 fall =
        land.height_mm[z * k_wide + k_side] - land.height_mm[(z + 1) * k_wide + k_side];
    if (fall > step) {
      step = fall;
      if (row != nullptr) *row = z;
    }
  }
  return step;
}

// One valley falling east at 0.03 from 1,500 m between sides at 0.5 that rise 600 m, 8 km of
// it at 50 m, open at its east end at the valley's own level there.
constexpr u32 k_valley_wide = 160, k_valley_deep = 48, k_valley_axis = 24;
constexpr i32 k_valley_edge_mm = 1261500;

landform::StartField one_valley() {
  return start_from(k_valley_wide, k_valley_deep, 50000,
                    [](f64 x, f64 z) { return 1500.0 - 0.03 * x + 0.5 * std::fabs(z - 1200.0); });
}

landform::Sketch one_valley_sketch(f64 ela_m) {
  landform::Sketch sketch = plain_sketch(k_valley_wide, 50000);
  sketch.height = k_valley_deep;
  sketch.edges.west = landform::EdgeKind::Closed;
  sketch.edges.north = landform::EdgeKind::Closed;
  sketch.edges.south = landform::EdgeKind::Closed;
  sketch.base_level_m = 1500.0 - 0.03 * 7950.0;
  sketch.border_reach_m = 0.0;
  sketch.erodibility_per_yr = 0.00002;
  sketch.start = "valley.json";
  sketch.duration_yr = 100000;
  landform::Ice ice;
  ice.ela_m = ela_m;
  sketch.ice = ice;
  return sketch;
}

}  // namespace

TEST_CASE("landform: ice leaves a trough where a river leaves a V, and a tributary hangs") {
  const landform::StartField start = two_valleys();
  const landform::Landform river = solved(two_valleys_sketch(false), &start);
  const landform::Landform glacier = solved(two_valleys_sketch(true), &start);
  CHECK(river.ice_steps == 0);
  CHECK(glacier.ice_steps == 20);
  // Three sections of the trunk below the tributary.
  f64 river_share = 0.0, glacier_share = 0.0;
  u32 river_width = 0, glacier_width = 0, swath_width = 0;
  for (const u32 x : {84u, 96u, 108u}) {
    river_share += trough_share(river, x) / 3.0;
    glacier_share += trough_share(glacier, x) / 3.0;
    river_width += width_above(river, x, 100000);
    glacier_width += width_above(glacier, x, 100000);
    // The swath there, as the solver finds it: the points within half the glacier's width of
    // its line, the width four times the ice's thickness (`thickness_ratio` 0.25).
    const f64 thick_m = static_cast<f64>(glacier.ice[k_axis * k_wide + x]) / 1000.0;
    swath_width += 2 * static_cast<u32>(0.5 * thick_m / 0.25 / 50.0) + 1;
  }
  const u32 junction = k_axis * k_wide + k_side;
  u32 step_row = 0;
  const i32 river_step = steepest_step(river), glacier_step = steepest_step(glacier, &step_row);
  // The ice over the step's upper point and over the point below it.
  const u32 ice_above = glacier.ice[step_row * k_wide + k_side];
  const u32 ice_below = glacier.ice[(step_row + 1) * k_wide + k_side];
  std::printf(
      "landform ice: the tributary's steepest step is at row %u, where the ice goes "
      "from %u to %u mm\n",
      step_row, ice_above, ice_below);
  std::printf(
      "landform ice: half way up its lowest 100 m the trunk is %.2f of its width at the 100 m "
      "under ice and %.2f under a river, and there %u points wide against %u over three "
      "sections (the swaths are %u); the trunk's ice is %u mm thick at the junction and the "
      "tributary's %u mm a kilometre up; the tributary's steepest step is %d mm under ice and "
      "%d mm under a river; the trunk's bed at the junction stands at %d and %d mm\n",
      glacier_share, river_share, glacier_width, river_width, swath_width, glacier.ice[junction],
      glacier.ice[(k_axis - 20) * k_wide + k_side], glacier_step, river_step,
      glacier.height_mm[junction], river.height_mm[junction]);
  // The measure is the published one in another dress. A section y = a x^b (Svensson 1959's
  // form for a glacial valley's side) is (1/2)^(1/b) as wide half way up as at the top, so the
  // share is the exponent: b = log(1/2) / log(share). A V of straight sides is b = 1, and a
  // glacial trough is a parabola or flatter: Svensson's own valley fits b "only slightly" off
  // 2, and the published fits of troughs run from about 1.5 up (how far each source was read
  // is on the grounding page: docs/experiments/landform.md, "A trough's section: the
  // sources"). So the bounds are the exponents': the river's valley under the glacial range's
  // 1.5, the glacier's inside it or past it (a floor between walls is b without bound).
  //
  // **What the model must give, and why** (W126, after the held edge was mended: stage C's
  // numbers here were a trench against the region's edge, and its "50 m below the river's bed"
  // went with it). The points of a glacier's swath are worn toward its line with the line's
  // own flux, so the swath is a floor whose cross fall is a few times the fall along the line;
  // outside the swath a slope's thinner ice meets the trunk's at a step in the ice's surface
  // and hangs above the floor by it. So the section a hundred metres up is at least the swath
  // wide, and the trough is a floor between walls: after 100,000 years flatter than a parabola
  // (the page says what that means and what is missing).
  const f64 river_b = std::log(0.5) / std::log(river_share);
  const f64 glacier_b = std::log(0.5) / std::log(glacier_share);
  std::printf("landform ice: as y = a x^b the river's section is b = %.2f, the glacier's %.2f\n",
              river_b, glacier_b);
  CHECK(river_b > 0.66);  // not a gorge either: the share's old lower bound of 0.35
  CHECK(river_b < 1.5);
  CHECK(glacier_b >= 1.5);
  CHECK(glacier_width >= swath_width);
  CHECK(glacier_width * 2 > river_width * 3);
  // The trunk's ice is the thicker, and the tributary's bed meets it at a step: the tributary
  // is worn to the trunk's ice, not to its bed. **What the model gives is where the step is,
  // not how tall**: a point cannot be worn below its receiver's bed raised by the ice's share
  // of the step in the ice, so the tributary's steepest step stands where its own ice meets
  // the trunk's swath, and is steeper than a river leaves anywhere on it. How tall it grows
  // depends on how long the swath's margin stays on one point (in this valley the trunk's ice
  // thins from 320 m to 294 m over the run, its swath narrows by a point, and the step at the
  // new margin is what has grown since: 14 m). Stage C held "five times the river's" on a
  // step of 40 m that the trench at the region's edge had made (W126).
  CHECK(glacier.ice[junction] > 100000);
  CHECK(glacier_step > river_step);
  CHECK(ice_below > ice_above + 50000);
  // The ice wears the trunk faster than the river does (its erodibility is twice the rivers'
  // on a flux the swath shares), and no lower than the edge the trunk leaves through.
  CHECK(glacier.height_mm[junction] < river.height_mm[junction]);
  CHECK(glacier.height_mm[junction] >= k_edge_mm);
}

TEST_CASE("landform: a glacier that leaves through a held edge is graded to that edge") {
  // The trunk of the case above fills with ice to the open east edge and its ice leaves there.
  // Every point makes ice (the line is under the floor), so the ice thickens all the way down
  // and nowhere thins: the model then has no rule that wears a bed below its receiver's, and
  // **no point of the region may stand below the edge's height**. Stage C's swath (a disc
  // about each point of a line) put the trunk's ice on points that drain to the edge's held
  // points beside the line, which read the edge as a snout 60 to 320 m tall: 2,407 points
  // stood under the edge, the lowest by 274 m, and the trunk ended in a 230 m step.
  const landform::StartField start = two_valleys();
  landform::Sketch sketch = two_valleys_sketch(true);
  sketch.diffusivity_m2_per_yr = 0.0;
  const landform::Landform land = solved(sketch, &start);
  u32 under = 0, deepest_fill = 0;
  i32 lowest = k_edge_mm;
  for (u32 p = 0; p < k_wide * k_deep; ++p) {
    if (land.height_mm[p] < k_edge_mm) ++under;
    if (land.height_mm[p] < lowest) lowest = land.height_mm[p];
    if (land.fill[p] > deepest_fill) deepest_fill = land.fill[p];
  }
  // The trunk's axis from the junction to the edge: never rising toward the edge, never above
  // where it started (nothing is laid down under ice that melts nowhere).
  i32 worst_rise = 0, last_step = 0;
  bool under_start = true;
  for (u32 x = k_side; x + 1 < k_wide; ++x) {
    const i32 here = land.height_mm[k_axis * k_wide + x];
    const i32 next = land.height_mm[k_axis * k_wide + x + 1];
    if (next - here > worst_rise) worst_rise = next - here;
    if (here > static_cast<i32>(start.data[k_axis * k_wide + x] * 1000.0f) + 1) under_start = false;
    if (x + 2 == k_wide) last_step = here - next;
  }
  const u32 out = k_axis * k_wide + k_wide - 1;
  std::printf(
      "landform ice: with its ice leaving through the held edge at %d mm, %u points stand "
      "under the edge (the lowest at %d mm), the deepest standing water is %u mm, the trunk's "
      "axis rises toward the edge by %d mm at most and meets it at a step of %d mm; the ice is "
      "%u mm thick on the last point inside and %u mm on the edge\n",
      k_edge_mm, under, lowest, deepest_fill, worst_rise, last_step, land.ice[out - 1],
      land.ice[out]);
  CHECK(under == 0);
  // No hollow either: the rivers' sweeps end within a millimetre a point, and nothing else
  // may leave one.
  CHECK(deepest_fill <= 10);
  CHECK(worst_rise <= 1);
  CHECK(under_start);
  // Graded: the last point inside meets the edge at no more than the fall the valley began
  // with over one point (0.02 over 50 m).
  CHECK(last_step >= 0);
  CHECK(last_step <= 1000);
  // The edge is not a snout: the held point carries the ice of the point that drains to it.
  CHECK(land.ice[out] == land.ice[out - 1]);
  CHECK(land.ice[out] > 100000);
}

TEST_CASE("landform: a glacier that ends inside its region leaves the edge alone") {
  // The one valley with the line at 1,750 m and nothing laid down, so the ground is rock and a
  // hollow cannot hide under till. The ice gathers on the upper sides, comes down the valley
  // floor and ends by melting about half way down the 8 km; as the ground wears down the
  // glacier shrinks. The run is solved to each of its twenty steps, so the test knows the
  // farthest the ice ever reached and the thickest ice that ever stood on each point.
  const landform::StartField start = one_valley();
  landform::Sketch sketch = one_valley_sketch(1750.0);
  sketch.deposition = 0.0;
  sketch.diffusivity_m2_per_yr = 0.0;
  const u32 n = k_valley_wide * k_valley_deep;
  std::vector<u32> most(n, 0u);
  u32 farthest = 0, nearest = k_valley_wide;
  landform::Landform land;
  for (i64 years = 5000; years <= 100000; years += 5000) {
    sketch.duration_yr = years;
    land = solved(sketch, &start);
    u32 snout = 0;
    for (u32 p = 0; p < n; ++p) {
      if (land.ice[p] > most[p]) most[p] = land.ice[p];
      const u32 z = p / k_valley_wide;
      // The valley glacier's snout: ice on the floor, within two rows of the axis. (The upper
      // sides carry slope ice all the way down the valley; that is not the glacier.)
      if (land.ice[p] > 0 && z + 2 >= k_valley_axis && z <= k_valley_axis + 2 &&
          p % k_valley_wide > snout)
        snout = p % k_valley_wide;
    }
    if (snout > farthest) farthest = snout;
    if (years <= 10000 && snout < nearest) nearest = snout;
  }
  u32 under = 0, hollows = 0, deepest = 0, over_ice = 0, dry_hollows = 0;
  for (u32 p = 0; p < n; ++p) {
    if (land.height_mm[p] < k_valley_edge_mm) ++under;
    if (land.fill[p] > deepest) deepest = land.fill[p];
    if (land.fill[p] >= 1000) ++hollows;
    // A bed is worn below its receiver's only where the ice over it is thicker than over the
    // receiver, by that difference at most, so a hollow is no deeper than the ice that dug it
    // was thick; and ground no ice ever stood on has none.
    if (land.fill[p] > most[p] + 10) ++over_ice;
    if (most[p] == 0 && land.fill[p] > 10) ++dry_hollows;
  }
  std::printf(
      "landform ice: a glacier with its line at 1,750 m reached column %u of %u at the "
      "farthest (column %u at the least in its first two steps); %u points stand under the "
      "edge's height, %u hold a metre of water (the deepest %u mm), %u hold more than the "
      "thickest ice that stood on them, %u of those on ground no ice reached\n",
      farthest, k_valley_wide, nearest, under, hollows, deepest, over_ice, dry_hollows);
  CHECK(nearest > 40);                   // a glacier came down the valley floor
  CHECK(farthest + 40 < k_valley_wide);  // and ended inside the region, at every step
  CHECK(under == 0);
  CHECK(over_ice == 0);
  CHECK(dry_hollows == 0);
}

namespace {

// A valley with a firn field above it: the west four kilometres are a plateau falling east at
// 0.025 from 2,000 m, dished gently toward the axis, and below it the one valley's shape (east
// at 0.03 from 1,500 m between sides at 0.5) for six kilometres, to an open east edge at the
// valley's level there. With the line at 1,750 m the plateau feeds a glacier that fills the
// valley to its edge for the first tens of thousands of years and draws back up it as the
// plateau wears down, so every reach of the valley has stood under a snout.
constexpr u32 k_firn_wide = 200, k_firn_deep = 48, k_firn_axis = 24;

landform::StartField firn_valley() {
  return start_from(k_firn_wide, k_firn_deep, 50000, [](f64 x, f64 z) {
    const f64 across = std::fabs(z - 1200.0);
    if (x < 4000.0) return 2000.0 - 0.025 * x + 0.05 * across;
    return 1500.0 - 0.03 * (x - 4000.0) + 0.5 * across;
  });
}

// Nothing is laid down and the creep is off, so the ground is rock and what stands in a hollow
// is the hollow the ice dug: a basin is told from a pond among till.
landform::Sketch firn_sketch(bool with_ice) {
  landform::Sketch sketch = plain_sketch(k_firn_wide, 50000);
  sketch.height = k_firn_deep;
  sketch.edges.west = landform::EdgeKind::Closed;
  sketch.edges.north = landform::EdgeKind::Closed;
  sketch.edges.south = landform::EdgeKind::Closed;
  sketch.base_level_m = 1500.0 - 0.03 * 5950.0;
  sketch.border_reach_m = 0.0;
  sketch.erodibility_per_yr = 0.00002;
  sketch.start = "firn.json";
  sketch.duration_yr = 100000;
  sketch.diffusivity_m2_per_yr = 0.0;
  sketch.deposition = 0.0;
  if (with_ice) {
    landform::Ice ice;
    ice.ela_m = 1750.0;
    sketch.ice = ice;
  }
  return sketch;
}

// The standing water a ground holds: the points with a metre or more, and the deepest.
struct Water {
  u32 points = 0;
  u32 deepest = 0;
  u32 first = 0;  // the columns they span
  u32 last = 0;
};

Water water_of(const landform::Landform& land) {
  Water out;
  out.first = land.width;
  for (u32 p = 0; p < land.width * land.height; ++p) {
    if (land.fill[p] > out.deepest) out.deepest = land.fill[p];
    if (land.fill[p] < 1000) continue;
    ++out.points;
    if (p % land.width < out.first) out.first = p % land.width;
    if (p % land.width > out.last) out.last = p % land.width;
  }
  return out;
}

}  // namespace

TEST_CASE("landform: a glacier digs basins its rivers cannot, and they hold water") {
  const landform::StartField start = firn_valley();
  const Water river = water_of(solved(firn_sketch(false), &start));
  // The glaciated run, solved to each of its steps: the thickest ice that ever stood on each
  // point, and whether any ice did.
  landform::Sketch sketch = firn_sketch(true);
  const u32 n = k_firn_wide * k_firn_deep;
  std::vector<u32> most(n, 0u);
  landform::Landform land;
  for (i64 years = 5000; years <= 100000; years += 5000) {
    sketch.duration_yr = years;
    land = solved(sketch, &start);
    for (u32 p = 0; p < n; ++p)
      if (land.ice[p] > most[p]) most[p] = land.ice[p];
  }
  const Water ice = water_of(land);
  u32 dry_hollows = 0, over_ice = 0, floor_points = 0;
  i32 worst_over = 0;
  for (u32 p = 0; p < n; ++p) {
    if (most[p] == 0 && land.fill[p] > 10) ++dry_hollows;
    const i32 over = static_cast<i32>(land.fill[p]) - static_cast<i32>(most[p]);
    if (over > 10) ++over_ice;
    if (over > worst_over) worst_over = over;
    const u32 z = p / k_firn_wide;
    if (land.fill[p] >= 1000 && z + 3 >= k_firn_axis && z <= k_firn_axis + 3) ++floor_points;
  }
  // The same valley with the 1-point rule (the slope read to the receiver), and with the
  // adverse-slope limit tighter and looser than the published 1.5.
  const auto with = [&](f64 coupling, f64 adverse) {
    landform::Sketch other = firn_sketch(true);
    other.ice->coupling = coupling;
    other.ice->adverse_slope = adverse;
    return water_of(solved(other, &start));
  };
  const Water tight = with(2.0, 0.5), loose = with(2.0, 6.0), next_point = with(0.0, 1.5);
  std::printf(
      "landform ice: with nothing laid down a glacier's valley holds %u points of standing "
      "water of a metre or more, columns %u to %u of %u, the deepest %u mm (%u of them within "
      "three rows of the axis); under its rivers alone %u points (deepest %u mm); %u points "
      "hold more than the thickest ice that stood on them (by %d mm at most), %u hold water "
      "where no ice stood. With the adverse-slope limit at 0.5 the deepest is %u mm, at 6 it "
      "is %u mm; reading the slope to the next point it is %u mm over %u points\n",
      ice.points, ice.first, ice.last, k_firn_wide, ice.deepest, floor_points, river.points,
      river.deepest, over_ice, worst_over, dry_hollows, tight.deepest, loose.deepest,
      next_point.deepest, next_point.points);
  // Rivers wear toward their receiver and leave no hollow; ice wears toward the level that
  // lays its surface flat, which under thinning ice is below the bed down the valley.
  CHECK(river.points == 0);
  CHECK(river.deepest <= 10);
  CHECK(ice.points > 0);
  CHECK(floor_points > 0);
  // Ground no ice stood on holds none.
  CHECK(dry_hollows == 0);
  // The limit bounds the dig: an adverse bed is worn until it rises `adverse_slope` times as
  // steeply as the ice's surface falls, so a tighter limit leaves a shallower basin.
  CHECK(tight.deepest < ice.deepest);
  CHECK(ice.deepest <= loose.deepest);
}

TEST_CASE("landform: what a glacier's basin holds is filled from its head once the ice has gone") {
  // The firn valley again with the rivers laying down what they carry: 60,000 years of ice,
  // and then the same ground under its rivers for 40,000 more. The ice leaves the basin, the
  // flood finds the lake, and the rivers fill it: by the end less water stands where the ice
  // stood than when it left, and what replaced it is sediment over the rock floor.
  const landform::StartField start = firn_valley();
  landform::Sketch sketch = firn_sketch(true);
  sketch.deposition = 1.0;
  sketch.ice->until_yr = 60000;
  sketch.duration_yr = 60000;
  const landform::Landform left = solved(sketch, &start);
  sketch.duration_yr = 100000;
  const landform::Landform filled = solved(sketch, &start);
  const Water before = water_of(left), after = water_of(filled);
  // The volume of water standing, and of sediment where water stood when the ice left, in
  // metres over one cell.
  f64 water_before = 0.0, water_after = 0.0, laid = 0.0;
  u32 basin_points = 0, rock_under = 0;
  for (u32 p = 0; p < k_firn_wide * k_firn_deep; ++p) {
    water_before += static_cast<f64>(left.fill[p]) / 1000.0;
    water_after += static_cast<f64>(filled.fill[p]) / 1000.0;
    if (left.fill[p] < 1000) continue;
    ++basin_points;
    const f64 gained =
        (static_cast<f64>(filled.sediment[p]) - static_cast<f64>(left.sediment[p])) / 1000.0;
    if (gained > 0.0) laid += gained;
    // The rock's surface, the height less the sediment, did not rise under the fill.
    if (filled.height_mm[p] - static_cast<i32>(filled.sediment[p]) <=
        left.height_mm[p] - static_cast<i32>(left.sediment[p]) + 2)
      ++rock_under;
  }
  std::printf(
      "landform ice: when the ice left, %u points held a metre of water or more (deepest %u "
      "mm, %.0f m of water over a cell in all); 40,000 years of rivers later %u do (deepest %u "
      "mm, %.0f m), and those points gained %.0f m of sediment; the rock floor stands no "
      "higher at %u of the %u\n",
      before.points, before.deepest, water_before, after.points, after.deepest, water_after, laid,
      rock_under, basin_points);
  CHECK(before.points > 0);
  CHECK(water_after < water_before);
  CHECK(laid > 0.0);
  CHECK(rock_under == basin_points);
}

TEST_CASE("landform: a bed under ice rises no steeper than the limit lets it") {
  // The firn valley's ice, solved step by step to **the first step that leaves the axis'
  // bed rising anywhere**: every rise there was made in that one step, under the ice the
  // field file records, so the test can read the slope the solver read: down the valley's
  // axis, from each point to the point twice `coupling` thicknesses on. (A later step's ground
  // carries rises an earlier, thicker ice made, which the limit does not bound against the
  // later ice.)
  const landform::StartField start = firn_valley();
  for (const f64 adverse : {1.5, 0.5}) {
    CAPTURE(adverse);
    landform::Sketch sketch = firn_sketch(true);
    sketch.ice->adverse_slope = adverse;
    landform::Landform land;
    u32 rises = 0, line_points = 0, steps_taken = 0;
    f64 worst = 0.0;
    for (i64 years = sketch.step_yr; years <= 100000 && rises == 0; years += sketch.step_yr) {
      sketch.duration_yr = years;
      land = solved(sketch, &start);
      ++steps_taken;
      line_points = 0;
      const f64 spacing = static_cast<f64>(land.spacing_mm);
      for (u32 x = 0; x + 1 < k_firn_wide; ++x) {
        const u32 p = k_firn_axis * k_firn_wide + x;
        // A point of the glacier's own line whose water and ice go east along the axis.
        if (land.ice[p] == 0 || land.receiver[p] != 0) continue;
        ++line_points;
        const f64 rise = static_cast<f64>(land.height_mm[p + 1] - land.height_mm[p]);
        if (rise <= 1.0) continue;
        u32 steps = static_cast<u32>(
            std::ceil(2.0 * sketch.ice->coupling * static_cast<f64>(land.ice[p]) / spacing));
        if (steps < 1) steps = 1;
        if (x + steps >= k_firn_wide) steps = k_firn_wide - 1 - x;
        const u32 q = p + steps;
        const f64 fall = static_cast<f64>(land.height_mm[p]) + static_cast<f64>(land.ice[p]) -
                         static_cast<f64>(land.height_mm[q]) - static_cast<f64>(land.ice[q]);
        ++rises;
        const f64 ratio = fall > 0.0 ? rise * static_cast<f64>(steps) / fall : 1.0e9;
        if (ratio > worst) worst = ratio;
      }
    }
    std::printf(
        "landform ice: with the limit at %.1f the axis' bed first rises after %u steps: "
        "%u of its %u points of ice rise to the next, the steepest at %.3f of the ice "
        "surface's slope\n",
        adverse, steps_taken, rises, line_points, worst);
    CHECK(line_points > 50);
    CHECK(rises > 0);
    // The sweeps end within a millimetre a point, which on a rise of a metre is a part in a
    // thousand; two in a hundred covers the shortest rises counted.
    CHECK(worst <= adverse * 1.02);
  }
}

TEST_CASE("landform: a glacier ends where its ice is spent, and leaves till where it melted") {
  // The one valley with the line at 1,600 m for 100,000 years: the ice gathers on the upper
  // sides and the head and melts down the valley, and what it carried is laid where it melts.
  // (The last column with ice anywhere is mostly the upper sides' slope ice, which stands the
  // length of the valley: the case above measures the valley glacier's own snout.)
  const u32 wide = k_valley_wide, deep = k_valley_deep, axis = k_valley_axis;
  const landform::StartField start = one_valley();
  const landform::Landform land = solved(one_valley_sketch(1600.0), &start);
  CHECK(land.ice_steps == 20);
  u32 snout = 0, iced = 0;
  for (u32 x = 0; x < wide; ++x)
    for (u32 z = 0; z < deep; ++z)
      if (land.ice[z * wide + x] > 0) {
        ++iced;
        if (x > snout) snout = x;
      }
  // What it left: standing water in its bed, and till.
  u32 lake_points = 0, deepest = 0, till_points = 0, thickest_till = 0, thickest_ice = 0;
  for (u32 p = 0; p < wide * deep; ++p) {
    if (land.ice[p] > 0 && land.fill[p] >= 1000) {
      ++lake_points;
      if (land.fill[p] > deepest) deepest = land.fill[p];
    }
    if (land.till[p] >= 500) {
      ++till_points;
      if (land.till[p] > thickest_till) thickest_till = land.till[p];
    }
    if (land.ice[p] > thickest_ice) thickest_ice = land.ice[p];
    CHECK(land.till[p] <= land.sediment[p]);  // till is sediment
  }
  std::printf(
      "landform ice: %u points were under ice, the last at column %u of %u (axis row %u); %u "
      "of them hold a metre of water or more now (deepest %u mm); %u points carry half a metre "
      "of till (thickest %u mm; the thickest ice is %u mm)\n",
      iced, snout, wide, axis, lake_points, deepest, till_points, thickest_till, thickest_ice);
  CHECK(iced > 300);
  CHECK(snout > 40);          // the ice came down the valley
  CHECK(snout + 8 < wide);    // and ended inside the region, by melting
  CHECK(till_points >= 100);  // and it left till, all of it sediment (checked above)
}

TEST_CASE("landform: an ice age solves to the same bytes on one, four and sixteen workers") {
  const landform::StartField start = two_valleys();
  landform::Sketch sketch = two_valleys_sketch(true);
  sketch.duration_yr = 30000;
  const landform::Landform serial = solved(sketch, &start);
  for (const u32 workers : {1u, 4u, 16u}) {
    CAPTURE(workers);
    jobs::JobSystemConfig config;
    config.performance_workers = workers;
    config.efficiency_workers = 1;
    config.pin_threads = false;
    jobs::JobSystem jobs(config);
    landform::SolveOptions options;
    options.jobs = &jobs;
    options.level_grain = 8;
    const landform::Landform land = solved(sketch, &start, options);
    CHECK(hash_of(land.height_mm) == hash_of(serial.height_mm));
    CHECK(hash_of(land.ice) == hash_of(serial.ice));
    CHECK(hash_of(land.till) == hash_of(serial.till));
    CHECK(hash_of(land.sediment) == hash_of(serial.sediment));
    CHECK(hash_of(land.discharge) == hash_of(serial.discharge));
  }
  // Pinned on MSVC (msvc-debug), 2026-11-11, and reproduced by Clang and GCC: the ice is the
  // one part of the solver that takes a power that is not a square root (det::exp, det::log).
  // Re-taken by W126: a glacier's swath is the points that drain to its line and a held point
  // carries its donors' ice, where a disc about each line point put a glacier's ice on points
  // that drain out of it (the trench at a held edge); and again for the basins' rule (the
  // slope read over a coupling length on a glacier's own line, and meltwater that never fills).
  CHECK(hash_of(serial.height_mm) == 0xcb332c9fa2168f28ull);
  CHECK(hash_of(serial.ice) == 0xe26756e1f278b69cull);
  CHECK(hash_of(serial.till) == 0x06a454b0b5827417ull);
  std::printf("landform ice pins: height %016llx ice %016llx till %016llx\n",
              static_cast<unsigned long long>(hash_of(serial.height_mm)),
              static_cast<unsigned long long>(hash_of(serial.ice)),
              static_cast<unsigned long long>(hash_of(serial.till)));
}

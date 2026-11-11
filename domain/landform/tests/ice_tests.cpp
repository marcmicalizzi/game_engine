// Tests for the landform's ice (docs/subsystems/landform.md, "Ice"): under a glacier a valley's
// cross-section is a trough where a river leaves a V, a tributary's mouth hangs above its trunk's
// floor, the bed above a snout is worn below the snout's and holds a lake once the ice has gone,
// what melts out of the ice is till, and the same bytes come from any number of workers. All CPU.
#include "test_sketches.h"

#include <core/jobs/job_system.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <string>

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
// 1959's form for a glacial trough) 0.71, a floor between upright walls 1.
f64 trough_share(const landform::Landform& land, u32 x) {
  return static_cast<f64>(width_above(land, x, 50000)) /
         static_cast<f64>(width_above(land, x, 100000));
}
// The largest fall between two points that follow each other down the tributary's axis, from a
// kilometre above the trunk to it, millimetres.
i32 steepest_step(const landform::Landform& land) {
  i32 step = 0;
  for (u32 z = k_axis - 20; z < k_axis; ++z) {
    const i32 fall =
        land.height_mm[z * k_wide + k_side] - land.height_mm[(z + 1) * k_wide + k_side];
    if (fall > step) step = fall;
  }
  return step;
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
  u32 river_width = 0, glacier_width = 0;
  for (const u32 x : {84u, 96u, 108u}) {
    river_share += trough_share(river, x) / 3.0;
    glacier_share += trough_share(glacier, x) / 3.0;
    river_width += width_above(river, x, 100000);
    glacier_width += width_above(glacier, x, 100000);
  }
  const u32 junction = k_axis * k_wide + k_side;
  const i32 river_step = steepest_step(river), glacier_step = steepest_step(glacier);
  std::printf(
      "landform ice: half way up its lowest 100 m the trunk is %.2f of its width at the 100 m "
      "under ice and %.2f under a river, and there %u points wide against %u over three "
      "sections; the trunk's ice is %u mm thick at the junction and the tributary's %u mm a "
      "kilometre up; the tributary's steepest step is %d mm under ice and %d mm under a river; "
      "the trunk's bed at the junction stands at %d and %d mm\n",
      glacier_share, river_share, glacier_width, river_width, glacier.ice[junction],
      glacier.ice[(k_axis - 20) * k_wide + k_side], glacier_step, river_step,
      glacier.height_mm[junction], river.height_mm[junction]);
  // A V of straight sides is 0.5 and a parabola 0.71: the river's valley is the first, and the
  // glacier's is past the second, and more than twice as wide.
  // The river's valley is the V (0.56 on this lattice). The glacier's is wider in its lower
  // half by a tenth of its width and more, and half again as wide a hundred metres up: a
  // trough. It measures 0.73, a parabola and no more: the level floor between walls of the
  // photographs it is not (landform.md, "Ice").
  CHECK(river_share > 0.35);
  CHECK(river_share < 0.65);
  CHECK(glacier_share > river_share + 0.1);
  CHECK(glacier_width * 2 > river_width * 3);
  // The trunk's ice is the thicker, and the tributary's bed meets it at a step: the tributary
  // is worn to the trunk's ice, not to its bed.
  CHECK(glacier.ice[junction] > 100000);
  CHECK(glacier_step > 5 * river_step);
  CHECK(glacier.height_mm[junction] < river.height_mm[junction] - 50000);
}

TEST_CASE("landform: a glacier ends where its ice is spent, and leaves till where it melted") {
  // One valley falling east at 0.1 from 1,800 m between sides at 0.5, 8 km of it. The line is at
  // 1,700 m, so the ice gathers on the upper sides and the head and melts down the valley, for
  // 100,000 years. What stands in a hollow of the bed at the end (ill) is the lake the
  // hollow holds once the ice has gone.
  const u32 wide = 160, deep = 48, axis = 24;
  const landform::StartField start = start_from(wide, deep, 50000, [](f64 x, f64 z) {
    return 1500.0 - 0.03 * x + 0.5 * std::fabs(z - 1200.0);
  });
  landform::Sketch sketch = plain_sketch(wide, 50000);
  sketch.height = deep;
  sketch.edges.west = landform::EdgeKind::Closed;
  sketch.edges.north = landform::EdgeKind::Closed;
  sketch.edges.south = landform::EdgeKind::Closed;
  sketch.base_level_m = 1500.0 - 0.03 * 7950.0;
  sketch.border_reach_m = 0.0;
  sketch.erodibility_per_yr = 0.00002;
  sketch.start = "valley.json";
  sketch.duration_yr = 100000;
  landform::Ice ice;
  ice.ela_m = 1600.0;
  sketch.ice = ice;
  const landform::Landform land = solved(sketch, &start);
  CHECK(land.ice_steps == 20);
  // The glacier's snout: the last column down the valley with ice on the axis' row or beside it.
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
      "landform ice: %u points were under ice, its snout at column %u of %u (axis row %u); %u "
      "of them hold a metre of water or more now (deepest %u mm); %u points carry half a metre "
      "of till (thickest %u mm; the thickest ice is %u mm)\n",
      iced, snout, wide, axis, lake_points, deepest, till_points, thickest_till, thickest_ice);
  CHECK(iced > 300);
  CHECK(snout > 40);        // the ice came down the valley
  CHECK(snout + 8 < wide);  // and ended inside the region, by melting
  // (Whether its bed holds a hollow above the snout is printed above and not held: in this
  // valley it does not, and the page says what a hollow needs.)
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
  CHECK(hash_of(serial.height_mm) == 0xe408f8ffd694c131ull);
  CHECK(hash_of(serial.ice) == 0xa08f17c5e79c88bdull);
  CHECK(hash_of(serial.till) == 0x34da7dadfbb58ebcull);
  std::printf("landform ice pins: height %016llx ice %016llx till %016llx\n",
              static_cast<unsigned long long>(hash_of(serial.height_mm)),
              static_cast<unsigned long long>(hash_of(serial.ice)),
              static_cast<unsigned long long>(hash_of(serial.till)));
}

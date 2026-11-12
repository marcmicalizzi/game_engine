// Tests for what stage C gave the landform's rivers (docs/subsystems/landform.md, "Flow that
// divides", "Lateral erosion", "Valleys that are not a comb"): flow that divides where the ground
// is gentle conserves its discharge and lays a fan whose contours are arcs; lateral erosion gives
// a river a floor more than a point wide, and a river that cuts down again leaves that floor as a
// terrace behind a scarp; and a range raised unevenly is not a comb of parallel valleys. All CPU.
#include "test_sketches.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <string>

using namespace engine;
using namespace landform_test;

namespace {

landform::Landform solved(const landform::Sketch& sketch,
                          const landform::StartField* start = nullptr) {
  landform::Landform land;
  std::string error;
  REQUIRE_MESSAGE(landform::solve(sketch, start, {}, land, error), error);
  return land;
}

// A grid of `side_x` by `side_z` points at `spacing_mm`, heights from `height(x_m, z_m)`.
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

// A solved ground as the next run's start: its own whole millimetres.
landform::StartField start_of(const landform::Landform& land) {
  landform::StartField start;
  start.width = land.width;
  start.height = land.height;
  start.spacing_mm = land.spacing_mm;
  start.origin_x_mm = land.origin_x_mm;
  start.origin_z_mm = land.origin_z_mm;
  start.vertical_mm_per_unit = 1.0;
  start.data.resize(land.width * land.height);
  for (u32 p = 0; p < land.width * land.height; ++p)
    start.data[p] = static_cast<f32>(land.height_mm[p]);
  return start;
}

// The ground's height at a place between lattice points, millimetres.
f64 height_at(const landform::Landform& land, f64 x_m, f64 z_m) {
  const f64 spacing = static_cast<f64>(land.spacing_mm) / 1000.0;
  const f64 fx = x_m / spacing;
  const f64 fz = z_m / spacing;
  const u32 ix = static_cast<u32>(fx);
  const u32 iz = static_cast<u32>(fz);
  const f64 tx = fx - static_cast<f64>(ix);
  const f64 tz = fz - static_cast<f64>(iz);
  const auto at = [&](u32 x, u32 z) {
    return static_cast<f64>(land.height_mm[z * land.width + x]);
  };
  const f64 north = at(ix, iz) + (at(ix + 1, iz) - at(ix, iz)) * tx;
  const f64 south = at(ix, iz + 1) + (at(ix + 1, iz + 1) - at(ix, iz + 1)) * tx;
  return north + (south - north) * tz;
}

// How a range's channels run (the count behind "Valleys that are not a comb"): of the points
// that drain a quarter of a square kilometre or more down a slope of 0.03 or more (a mountain
// channel, not a plain's), the junctions a kilometre of channel and the share that run along
// the lattice's z, which is straight down a range that lies along x.
struct ChannelCount {
  u32 points = 0;
  f64 junctions_per_km = 0.0;
  f64 down_range_share = 0.0;
};

ChannelCount count_channels(const landform::Landform& land) {
  const u32 n = land.width * land.height;
  const f64 spacing = static_cast<f64>(land.spacing_mm) / 1000.0;
  const u32 least = static_cast<u32>(250000.0 / (spacing * spacing));
  Vector<u8> donors(n, u8{0});
  ChannelCount count;
  f64 length = 0.0;
  u32 along = 0;
  const auto channel = [&](u32 p) {
    return land.receiver[p] != landform::k_no_receiver && land.drainage_area[p] >= least &&
           land.slope[p] >= 30000;
  };
  for (u32 p = 0; p < n; ++p) {
    if (!channel(p)) continue;
    const u8 code = land.receiver[p];
    ++count.points;
    if (code == 2 || code == 6) ++along;
    length += spacing * ((code & 1u) != 0 ? std::sqrt(2.0) : 1.0);
    const i32 x = static_cast<i32>(p % land.width) + landform::k_receiver_dx[code];
    const i32 z = static_cast<i32>(p / land.width) + landform::k_receiver_dz[code];
    ++donors[static_cast<u32>(z) * land.width + static_cast<u32>(x)];
  }
  u32 junctions = 0;
  for (u32 p = 0; p < n; ++p)
    if (channel(p) && donors[p] >= 2) ++junctions;
  if (count.points > 0) {
    count.junctions_per_km = static_cast<f64>(junctions) / (length / 1000.0);
    count.down_range_share = static_cast<f64>(along) / static_cast<f64>(count.points);
  }
  return count;
}

}  // namespace

TEST_CASE("landform: flow that divides conserves its discharge, and only gentle ground divides") {
  // A cone falling at 0.03 from the middle of the north edge to a held south edge: gentle
  // everywhere, so the flow divides at nearly every point.
  const u32 side = 64;
  const i64 spacing_mm = 50000;
  Vector<i32> gentle(side * side), steep(side * side);
  Vector<u8> held(side * side, u8{0});
  for (u32 z = 0; z < side; ++z)
    for (u32 x = 0; x < side; ++x) {
      const f64 dx = 50.0 * (static_cast<f64>(x) - 31.5);
      const f64 dz = 50.0 * static_cast<f64>(z);
      const f64 r = std::sqrt(dx * dx + dz * dz);
      gentle[z * side + x] = static_cast<i32>(200000.0 - 30.0 * r);
      steep[z * side + x] = static_cast<i32>(3000000.0 - 500.0 * r);
      held[z * side + x] = z == side - 1 ? u8{1} : u8{0};
    }
  Vector<u8> receiver;
  Vector<u16> share;
  Vector<u64> discharge;
  const auto divided = [&](const Vector<i32>& ground, f64 below, u64& out_sum) {
    REQUIRE(landform::route_divided(side, side, spacing_mm, ground, held, 7, below, nullptr,
                                    receiver, share, discharge));
    u32 points = 0;
    out_sum = 0;
    for (u32 p = 0; p < side * side; ++p) {
      u32 sum = 0, parts = 0;
      for (u32 k = 0; k < 8; ++k) {
        sum += share[p * 8 + k];
        if (share[p * 8 + k] != 0) ++parts;
      }
      if (held[p] != 0) {
        out_sum += discharge[p];
        CHECK(sum == 0);
      } else {
        CHECK(sum == landform::k_share_one);  // a point's shares are its whole flow, exactly
        if (parts >= 2) ++points;
      }
    }
    return points;
  };
  u64 sum = 0;
  const u32 gentle_divides = divided(gentle, 0.1, sum);
  // Nothing lost and nothing made: what leaves through the held points is the lattice.
  CHECK(sum == u64{side} * side * landform::k_share_one);
  // Away from the cone's own axes a point has two or three lower neighbours.
  CHECK(gentle_divides > side * (side - 1) * 3 / 4);
  const u32 never = divided(gentle, 0.0, sum);
  CHECK(never == 0);
  CHECK(sum == u64{side} * side * landform::k_share_one);
  // At 0.5 the same cone is steeper than the limit everywhere: one receiver a point.
  const u32 steep_divides = divided(steep, 0.1, sum);
  CHECK(steep_divides == 0);
  CHECK(sum == u64{side} * side * landform::k_share_one);
  std::printf("landform divided flow: %u of %u points divide on a 0.03 cone, %u at 0.5\n",
              gentle_divides, side * (side - 1), steep_divides);
}

TEST_CASE("landform: a single mouth's fan is a cone: its contours are arcs about the mouth") {
  // A catchment shaped as a funnel, 1,600 m deep and 3,200 m across, drains through one mouth in
  // the middle of its south side onto a level plain; the catchment rises at 2 mm/yr for 200,000
  // years and the plain does not. With one receiver a point the load is laid along whichever
  // way the river runs that step; divided, it is laid across the whole front.
  const u32 wide = 64, deep = 80;
  const f64 mouth_x = 1600.0, front_z = 1600.0, plain_m = 40.0;
  const landform::StartField start = start_from(wide, deep, 50000, [&](f64 x, f64 z) {
    if (z >= front_z) return plain_m;
    f64 t = (front_z - z) / 300.0;
    t = t > 1.0 ? 1.0 : t;
    const f64 rise = t * t * (3.0 - 2.0 * t);
    return plain_m + rise * (0.08 * (front_z - z) + 0.35 * std::fabs(x - mouth_x));
  });
  const auto fan = [&](f64 divide_below, f64& relief_mm, f64& spread_mm) {
    landform::Sketch sketch = plain_sketch(wide, 50000);
    sketch.height = deep;
    sketch.edges.west = landform::EdgeKind::Closed;
    sketch.edges.east = landform::EdgeKind::Closed;
    sketch.edges.north = landform::EdgeKind::Closed;
    sketch.base_level_m = plain_m;
    sketch.border_reach_m = 0.0;
    landform::Ridge ridge;
    ridge.points.push_back(landform::Point{0.0, 600.0});
    ridge.points.push_back(landform::Point{3200.0, 600.0});
    ridge.rate_mm_per_yr = 2.0;
    ridge.reach_m = 900.0;
    ridge.variation = 0.0;
    sketch.uplift.ridges.push_back(ridge);
    sketch.erodibility_per_yr = 0.00002;
    sketch.lateral_erosion = 0.0;
    sketch.divide_below_slope = divide_below;
    sketch.start = "funnel.json";
    sketch.duration_yr = 200000;
    sketch.step_yr = 2000;
    const landform::Landform land = solved(sketch, &start);
    // Five places on an arc 500 m from the mouth, 25 degrees apart about due south.
    const f64 radius = 500.0;
    const f64 sines[5] = {-0.766044, -0.422618, 0.0, 0.422618, 0.766044};
    const f64 cosines[5] = {0.642788, 0.906308, 1.0, 0.906308, 0.642788};
    f64 lowest = 1.0e30, highest = -1.0e30, mean = 0.0;
    for (u32 k = 0; k < 5; ++k) {
      const f64 mm = height_at(land, mouth_x + radius * sines[k], front_z + radius * cosines[k]);
      lowest = mm < lowest ? mm : lowest;
      highest = mm > highest ? mm : highest;
      mean += mm / 5.0;
    }
    relief_mm = mean - plain_m * 1000.0;
    spread_mm = highest - lowest;
  };
  f64 relief = 0.0, spread = 0.0, single_relief = 0.0, single_spread = 0.0;
  fan(0.1, relief, spread);
  fan(0.0, single_relief, single_spread);
  std::printf(
      "landform fan cone: on an arc 500 m from the mouth the ground stands %.0f mm above the "
      "plain and varies by %.0f mm along it; with one receiver a point %.0f mm and %.0f mm\n",
      relief, spread, single_relief, single_spread);
  // A fan: metres of deposit half a kilometre out.
  CHECK(relief > 2000.0);
  // A cone: along the arc the ground varies by under a third of its height above the plain.
  CHECK(spread < relief / 3.0);
}

TEST_CASE("landform: a river's plain is wider than a point, and cutting down leaves a terrace") {
  // A straight valley falling east at 0.01 between sides at 0.3, 6.4 km long, draining twenty
  // square kilometres through its mouth; nothing rises. For 400,000 years the river wears the
  // bank on the outside of each bend.
  const u32 wide = 128, deep = 64, axis = 32;
  const landform::StartField start = start_from(wide, deep, 50000, [](f64 x, f64 z) {
    return 100.0 - 0.01 * x + 0.3 * std::fabs(z - 1600.0);
  });
  const f64 mouth_m = 100.0 - 0.01 * 6350.0;
  landform::Sketch sketch = plain_sketch(wide, 50000);
  sketch.height = deep;
  sketch.edges.west = landform::EdgeKind::Closed;
  sketch.edges.north = landform::EdgeKind::Closed;
  sketch.edges.south = landform::EdgeKind::Closed;
  sketch.base_level_m = mouth_m;
  sketch.border_reach_m = 0.0;
  sketch.erodibility_per_yr = 0.00002;
  sketch.start = "valley.json";
  // Banks three times as erodible as the bed (weak rock: Langston and Tucker's range is 1 and up),
  // so that 400,000 years leave a floor wide enough to keep a tread of once the river cuts down.
  sketch.lateral_erosion = 3.0;
  sketch.duration_yr = 400000;
  // The floor of a column: its points within two metres of its lowest.
  const auto floor_width = [&](const landform::Landform& land, u32 x) {
    i32 lowest = land.height_mm[axis * wide + x];
    for (u32 z = 0; z < deep; ++z)
      if (land.height_mm[z * wide + x] < lowest) lowest = land.height_mm[z * wide + x];
    u32 count = 0;
    for (u32 z = 0; z < deep; ++z)
      if (land.height_mm[z * wide + x] <= lowest + 2000) ++count;
    return count;
  };
  const auto mean_floor = [&](const landform::Landform& land) {
    f64 sum = 0.0;
    for (u32 x = 64; x < 120; ++x)
      sum += static_cast<f64>(floor_width(land, x)) / 56.0;
    return sum;
  };
  const landform::Landform land = solved(sketch, &start);
  landform::Sketch still = sketch;
  still.lateral_erosion = 0.0;
  const landform::Landform narrow = solved(still, &start);
  const f64 with_banks = mean_floor(land), without = mean_floor(narrow);
  std::printf(
      "landform river plain: the lower valley's floor is %.1f points wide (50 m points) with "
      "lateral erosion, %.1f without; %u banks taken\n",
      with_banks, without, land.banks_taken);
  CHECK(land.banks_taken > 100);
  CHECK(with_banks >= 3.0);  // 150 m of floor and more
  // And the banks made a point of it or more: without them the same valley's floor is what
  // its river laid down and creep levelled in the same time.
  CHECK(with_banks > without + 1.0);

  // The base level falls 20 m and the river cuts down for 8,000 years, which takes the cut about
  // a kilometre up from the mouth. What it does not reach is the old floor, standing above the
  // new one behind a scarp. (A tread is not for ever: the streams that cross it from the valley's
  // side cut it down toward the new floor, the edge first, at a millimetre a year here.)
  const landform::StartField floor_start = start_of(land);
  landform::Sketch cut = sketch;
  cut.base_level_m = mouth_m - 20.0;
  cut.duration_yr = 8000;
  cut.step_yr = 1000;
  const landform::Landform after = solved(cut, &floor_start);
  u32 columns = 0, wide_enough = 0, with_terrace = 0, scarp_beside_floor = 0;
  f64 incision = 0.0, tread = 0.0, old_width = 0.0, new_width = 0.0;
  for (u32 x = 104; x < 124; ++x) {
    // The column's old floor (phase one's points within two metres of its lowest) and the new
    // channel (the lowest point now).
    i32 old_low = land.height_mm[axis * wide + x];
    i32 new_low = after.height_mm[axis * wide + x];
    u32 channel = axis;
    for (u32 z = 0; z < deep; ++z) {
      if (land.height_mm[z * wide + x] < old_low) old_low = land.height_mm[z * wide + x];
      if (after.height_mm[z * wide + x] < new_low) {
        new_low = after.height_mm[z * wide + x];
        channel = z;
      }
    }
    ++columns;
    incision += static_cast<f64>(old_low - new_low) / 20.0;
    const u32 was = floor_width(land, x), now = floor_width(after, x);
    old_width += static_cast<f64>(was) / 20.0;
    new_width += static_cast<f64>(now) / 20.0;
    // A terrace is owed where the river cut down five metres and its old floor was wider than
    // its new one and the scarps' own run (a point each side) together.
    if (old_low - new_low < 5000 || was < now + 3) continue;
    ++wide_enough;
    // Terrace points: old floor that still stands within a metre and a half of where it stood.
    u32 treads = 0, nearest = deep;
    for (u32 z = 0; z < deep; ++z) {
      const u32 p = z * wide + x;
      if (land.height_mm[p] > old_low + 2000) continue;
      if (after.height_mm[p] < land.height_mm[p] - 1500) continue;
      ++treads;
      const u32 away = z > channel ? z - channel : channel - z;
      if (away < nearest) nearest = away;
    }
    if (treads == 0) continue;
    ++with_terrace;
    tread += static_cast<f64>(treads);
    // The scarp stands at the edge of what the river has cut since: within the new floor's
    // width, and a point more for the scarp's own run, of the channel.
    if (nearest <= now + 1) ++scarp_beside_floor;
  }
  std::printf(
      "landform terrace: over %u columns the river cut down %.1f m, its floor %.1f points wide "
      "before and %.1f after; %u columns owe a terrace, %u keep old floor (%.1f points each), "
      "%u with its edge beside the new floor\n",
      columns, incision / 1000.0, old_width, new_width, wide_enough, with_terrace,
      with_terrace > 0 ? tread / with_terrace : 0.0, scarp_beside_floor);
  // The river followed its base level down: over the last kilometre, a quarter of the fall
  // and more in 8,000 years (all of it at the mouth, none yet where the cut has not arrived).
  CHECK(incision > 5000.0);
  CHECK(new_width < old_width);  // into a floor narrower than the one it left
  CHECK(wide_enough >= 5);
  // Where a terrace is owed one stands, its edge beside the new floor.
  CHECK(with_terrace * 4 >= wide_enough * 3);
  CHECK(scarp_beside_floor * 4 >= with_terrace * 3);
}

TEST_CASE("landform: a range raised unevenly is not a comb of parallel valleys") {
  // One range along x, 9.6 km of it, for 500,000 years: raised evenly along and across (the
  // kernel alone), and with the default unevenness over the plan. (Two solves of a hundred
  // steps on a lattice large enough to hold a dozen valleys a side: 6 s in a debug build, which
  // is what a count of valleys needs.)
  const auto range = [](f64 unevenness) {
    landform::Sketch sketch = plain_sketch(96, 100000);
    landform::Ridge ridge;
    ridge.points.push_back(landform::Point{0.0, 4750.0});
    ridge.points.push_back(landform::Point{9500.0, 4750.0});
    ridge.rate_mm_per_yr = 1.5;
    ridge.reach_m = 4000.0;
    ridge.variation = 0.0;
    sketch.uplift.ridges.push_back(ridge);
    sketch.uplift.variation = unevenness;
    sketch.erodibility_per_yr = 0.00002;
    sketch.duration_yr = 500000;
    return sketch;
  };
  const landform::Sketch defaults;
  const ChannelCount even = count_channels(solved(range(0.0)));
  const ChannelCount uneven = count_channels(solved(range(defaults.uplift.variation)));
  std::printf(
      "landform comb: raised evenly, %u channel points, %.3f junctions a km, %.0f%% running "
      "straight down the range; unevenly, %u, %.3f and %.0f%%\n",
      even.points, even.junctions_per_km, 100.0 * even.down_range_share, uneven.points,
      uneven.junctions_per_km, 100.0 * uneven.down_range_share);
  CHECK(even.points > 200);
  CHECK(uneven.points > 200);
  // More of the valleys join each other, and fewer run straight down the range.
  CHECK(uneven.junctions_per_km > 1.15 * even.junctions_per_km);
  CHECK(uneven.down_range_share < even.down_range_share);
}

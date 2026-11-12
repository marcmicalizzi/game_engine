// Tests for the landform capability (docs/subsystems/landform.md, "Testing"): what the routing
// guarantees, what a steady state measures against the published law, the two border defects the
// proof of concept had, a starting ground refined, and the files. All CPU. The bytes are
// determinism_tests.cpp's.
#include "test_sketches.h"

#include <doctest/doctest.h>
#include <test_temp_dir.h>

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

u32 receiver_of(const landform::Landform& land, u32 p) {
  const u8 code = land.receiver[p];
  const i32 x = static_cast<i32>(p % land.width) + landform::k_receiver_dx[code];
  const i32 z = static_cast<i32>(p / land.width) + landform::k_receiver_dz[code];
  return static_cast<u32>(z) * land.width + static_cast<u32>(x);
}

// Every point's path of receivers ends at a held point, and the held points' areas are the
// lattice: nothing is lost and nothing is counted twice.
void check_drains(const landform::Landform& land) {
  const u32 n = land.width * land.height;
  Vector<u8> reaches(n, u8{0});
  u64 outlet_area = 0;
  for (u32 p = 0; p < n; ++p)
    if (land.receiver[p] == landform::k_no_receiver) {
      reaches[p] = 1;
      outlet_area += land.drainage_area[p];
    }
  CHECK(outlet_area == n);
  u32 stranded = 0;
  Vector<u32> path;
  for (u32 p = 0; p < n; ++p) {
    path.clear();
    u32 q = p;
    while (reaches[q] == 0 && path.size() <= n) {
      path.push_back(q);
      REQUIRE(land.receiver[q] < 8);
      q = receiver_of(land, q);
    }
    if (reaches[q] == 0) {
      ++stranded;
      continue;
    }
    for (u32 k = 0; k < path.size(); ++k)
      reaches[path[k]] = 1;
  }
  CHECK(stranded == 0);
}

// The least-squares slope of y on x.
f64 regress(const Vector<f64>& x, const Vector<f64>& y) {
  const f64 count = static_cast<f64>(x.size());
  f64 sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (u32 i = 0; i < x.size(); ++i) {
    sx += x[i];
    sy += y[i];
    sxx += x[i] * x[i];
    sxy += x[i] * y[i];
  }
  return (count * sxy - sx * sy) / (count * sxx - sx * sx);
}

// The steepest rise over run between a point and any of its eight neighbours, the excess over
// `critical` reported in millimetres of height (rounding to whole millimetres adds up to one).
f64 worst_excess_mm(const landform::Landform& land, f64 critical) {
  const f64 spacing = static_cast<f64>(land.spacing_mm);
  f64 worst = -1.0e30;
  for (u32 z = 0; z < land.height; ++z)
    for (u32 x = 0; x < land.width; ++x) {
      const u32 p = z * land.width + x;
      if (land.receiver[p] == landform::k_no_receiver) continue;  // held points do not fall
      for (u32 k = 0; k < 8; ++k) {
        const i32 nx = static_cast<i32>(x) + landform::k_receiver_dx[k];
        const i32 nz = static_cast<i32>(z) + landform::k_receiver_dz[k];
        if (nx < 0 || nz < 0 || nx >= static_cast<i32>(land.width) ||
            nz >= static_cast<i32>(land.height))
          continue;
        const u32 q = static_cast<u32>(nz) * land.width + static_cast<u32>(nx);
        const f64 run = spacing * ((k & 1u) != 0 ? std::sqrt(2.0) : 1.0);
        const f64 excess = static_cast<f64>(land.height_mm[p] - land.height_mm[q]) - critical * run;
        if (excess > worst) worst = excess;
      }
    }
  return worst;
}

}  // namespace

TEST_CASE("landform: the flood breaks ties by cell index and drains a flat and a pit") {
  // A flat held at one corner: every point is raised a unit a ring, and its receiver is the
  // steepest lower neighbour, the first of equals in code order.
  const u32 side = 5;
  Vector<i32> flat(side * side, 0);
  Vector<u8> held(side * side, u8{0});
  held[0] = 1;
  Vector<i64> filled;
  Vector<u32> order;
  Vector<u8> receiver;
  Vector<u32> area;
  REQUIRE(landform::route(side, side, flat, held, 0, nullptr, filled, order, receiver, area));
  CHECK(order[0] == 0u);
  CHECK(area[0] == side * side);
  CHECK(receiver[0] == landform::k_no_receiver);
  CHECK(receiver[1 * side + 1] == 5);   // north-west, to the corner
  CHECK(receiver[2] == 4);              // west along the edge: a cardinal beats the diagonal
  CHECK(filled[side * side - 1] == 4);  // four rings out, four units up
  for (u32 k = 1; k < side * side; ++k) {
    CAPTURE(k);
    // The order ascends in (filled, index): the total order the flood pops in.
    const bool ascending = filled[order[k - 1]] < filled[order[k]] ||
                           (filled[order[k - 1]] == filled[order[k]] && order[k - 1] < order[k]);
    CHECK(ascending);
  }

  // A pit a metre deep inside a held rim: it fills to the rim and drains over it.
  Vector<i32> pit(side * side, 1000);
  pit[2 * side + 2] = 0;
  Vector<u8> rim(side * side, u8{0});
  for (u32 i = 0; i < side; ++i) {
    rim[i] = 1;
    rim[(side - 1) * side + i] = 1;
    rim[i * side] = 1;
    rim[i * side + side - 1] = 1;
  }
  REQUIRE(landform::route(side, side, pit, rim, 0, nullptr, filled, order, receiver, area));
  const i64 centre = filled[2 * side + 2];
  CHECK(centre > 1000 * landform::k_fill_units_per_mm);  // above its neighbours' by epsilon
  CHECK(centre / landform::k_fill_units_per_mm == 1000);
  CHECK(receiver[2 * side + 2] < 8);

  Vector<u8> none(side * side, u8{0});
  CHECK_FALSE(landform::route(side, side, flat, none, 0, nullptr, filled, order, receiver, area));
}

TEST_CASE("landform: every point of a solved region drains to an outlet or the sea") {
  const landform::Landform land = solved(golden_sketch());
  check_drains(land);
  // The sea's edge and the two open edges are held; the closed one is not.
  CHECK(land.receiver[40 * land.width] == landform::k_no_receiver);
  CHECK(land.receiver[40 * land.width + land.width - 1] != landform::k_no_receiver);
  CHECK(land.has_sea);
  CHECK(land.height_mm[40 * land.width] == -150000);
}

TEST_CASE("landform: a uniform uplift's steady state holds the concavity and Hack's law") {
  // Stream power alone (no diffusion, a critical slope that never binds, no variation, no taper),
  // 1 mm/yr everywhere for two million years. At steady state S = (U / K) A^-1/2 at every point.
  landform::Sketch sketch = plain_sketch(96, 200000);
  // Stream power alone: the steady state measured here is that law's, so the processes stage C
  // added beside it (banks worn sideways, flow divided on gentle ground) are off, as deposition
  // is.
  sketch.lateral_erosion = 0.0;
  sketch.divide_below_slope = 0.0;
  sketch.uplift.regional_mm_per_yr = 1.0;
  sketch.border_reach_m = 0.0;
  sketch.erodibility_per_yr = 0.00002;
  sketch.diffusivity_m2_per_yr = 0.0;
  sketch.critical_slope = 4.0;
  sketch.critical_slope_resistant = 4.0;
  sketch.rock.variation = 0.0;
  sketch.deposition = 0.0;  // stream power alone: nothing a river carries is laid down again
  sketch.duration_yr = 2000000;
  sketch.step_yr = 10000;
  const landform::Landform land = solved(sketch);
  check_drains(land);
  const u32 n = land.width * land.height;
  const f64 cell_m2 = 200.0 * 200.0;

  u32 free_points = 0, steady = 0, near = 0;
  f64 rate_sum = 0.0;
  Vector<f64> log_area, log_slope;
  for (u32 p = 0; p < n; ++p) {
    if (land.receiver[p] == landform::k_no_receiver) continue;
    ++free_points;
    // The last step removed what it raised: 1 mm/yr is 1000 micrometres a year.
    if (land.erosion_rate[p] >= 990 && land.erosion_rate[p] <= 1010) ++steady;
    if (land.erosion_rate[p] >= 900 && land.erosion_rate[p] <= 1100) ++near;
    rate_sum += static_cast<f64>(land.erosion_rate[p]);
    if (land.slope[p] <= 0) continue;
    log_area.push_back(std::log(static_cast<f64>(land.drainage_area[p]) * cell_m2));
    log_slope.push_back(std::log(static_cast<f64>(land.slope[p]) * 1.0e-6));
  }
  const f64 steady_share = static_cast<f64>(steady) / static_cast<f64>(free_points);
  const f64 near_share = static_cast<f64>(near) / static_cast<f64>(free_points);
  const f64 mean_rate = rate_sum / static_cast<f64>(free_points);
  std::printf("landform steady state: within 10%% %.4f, mean rate %.1f um/yr\n", near_share,
              mean_rate);
  const f64 concavity = -regress(log_area, log_slope);

  // Hack's law: the longest path above a point against its area, over the channels (eight cells
  // and more). The order comes from routing the solved ground again, as a reader would.
  Vector<u8> held(n, u8{0});
  for (u32 p = 0; p < n; ++p)
    held[p] = land.receiver[p] == landform::k_no_receiver ? u8{1} : u8{0};
  Vector<i64> filled;
  Vector<u32> order;
  Vector<u8> receiver;
  Vector<u32> area;
  REQUIRE(landform::route(land.width, land.height, land.height_mm, held, land.route_seed, nullptr,
                          filled, order, receiver, area));
  CHECK(hash_of(receiver) == hash_of(land.receiver));
  CHECK(hash_of(area) == hash_of(land.drainage_area));
  Vector<f64> length(n, 0.0);
  for (u32 k = n; k-- > 0;) {
    const u32 p = order[k];
    if (receiver[p] == landform::k_no_receiver) continue;
    const f64 step = 200.0 * ((receiver[p] & 1u) != 0 ? std::sqrt(2.0) : 1.0);
    const u32 r = receiver_of(land, p);
    if (length[p] + step > length[r]) length[r] = length[p] + step;
  }
  Vector<f64> hack_area, hack_length;
  for (u32 p = 0; p < n; ++p) {
    if (held[p] != 0 || area[p] < 8) continue;
    hack_area.push_back(std::log(static_cast<f64>(area[p]) * cell_m2));
    hack_length.push_back(std::log(length[p]));
  }
  const f64 hack = regress(hack_area, hack_length);
  std::printf(
      "landform steady state: %.4f of points at the uplift rate, concavity %.4f, Hack's "
      "exponent %.4f over %u channel points\n",
      steady_share, concavity, hack, hack_length.size());
  // Steady: the region as a whole loses what it gains (the mean rate within 1% of the uplift),
  // and all but the points on shifting divides do so point by point (their receivers and areas
  // change from step to step, which is the explicit drainage area: experiments/landform.md).
  CHECK(mean_rate == doctest::Approx(1000.0).epsilon(0.01));
  CHECK(near_share > 0.95);
  CHECK(steady_share > 0.75);
  // The concavity chosen, m/n = 1/2, within 0.02 (the published range is 0.4 to 0.6).
  CHECK(concavity == doctest::Approx(0.5).epsilon(0.04));
  // Hack 1957: 0.6; the published range 0.5 to 0.7 (experiments/landform.md).
  CHECK(hack >= 0.5);
  CHECK(hack <= 0.7);
}

TEST_CASE("landform: the ground meets an open edge without a step") {
  // A ridge across a region whose four edges are open. With the border's taper (the default, 16
  // cells) the first row inside an edge stands within a stated bound of the edge's level; with
  // the taper off, uplift switches on in one row and the same row stands metres above it, which
  // is the proof of concept's ramp at a region's edge.
  landform::Sketch sketch = plain_sketch(96, 100000);
  sketch.uplift.regional_mm_per_yr = 0.5;
  landform::Ridge ridge;
  ridge.points.push_back(landform::Point{1500.0, 4750.0});
  ridge.points.push_back(landform::Point{8000.0, 4750.0});
  ridge.rate_mm_per_yr = 3.0;
  ridge.reach_m = 3000.0;
  sketch.uplift.ridges.push_back(ridge);
  sketch.erodibility_per_yr = 0.00002;
  sketch.critical_slope = 0.9;
  sketch.duration_yr = 500000;
  // Along every line inward from each of the four edges: the first row's height over the edge's
  // (the step), and the largest change of slope from one row to the next over the first four
  // rows (the bend: a ramp switched on in one row is a bend of the whole slope at once; further
  // in, a gully's side is a bend of its own and no fault of the edge).
  const auto profile = [](const landform::Landform& land, i32& step, i32& bend) {
    step = 0;
    bend = 0;
    const u32 w = land.width;
    const u32 h = land.height;
    for (u32 i = 12; i + 12 < w; ++i) {
      for (u32 edge = 0; edge < 4; ++edge) {
        i32 line[4];
        for (u32 r = 0; r < 4; ++r) {
          const u32 p = edge == 0   ? r * w + i
                        : edge == 1 ? (h - 1 - r) * w + i
                        : edge == 2 ? i * w + r
                                    : i * w + (w - 1 - r);
          line[r] = land.height_mm[p];
        }
        if (line[1] - line[0] > step) step = line[1] - line[0];
        for (u32 r = 0; r + 2 < 4; ++r) {
          const i32 change = line[r + 2] - 2 * line[r + 1] + line[r];
          if (change > bend) bend = change;
          if (-change > bend) bend = -change;
        }
      }
    }
  };
  i32 tapered_step = 0, tapered_bend = 0, abrupt_step = 0, abrupt_bend = 0;
  const landform::Landform tapered = solved(sketch);
  profile(tapered, tapered_step, tapered_bend);
  sketch.border_reach_m = 0.0;
  const landform::Landform abrupt = solved(sketch);
  profile(abrupt, abrupt_step, abrupt_bend);
  std::printf(
      "landform edge: step %d mm and bend %d mm a row with the taper; %d mm and %d mm "
      "without\n",
      tapered_step, tapered_bend, abrupt_step, abrupt_bend);
  // The bound, over a 100 m cell: the first row within two metres of the edge's level (a slope
  // of 2%), and no row's slope more than 0.2 from the row before it. Without the taper the first
  // row stands at the critical slope, 90 m up, and the slope arrives in one row.
  CHECK(tapered_step <= 2000);
  CHECK(tapered_bend <= 20000);
  CHECK(abrupt_step > 50000);
  CHECK(abrupt_bend > 50000);
  check_drains(tapered);
}

TEST_CASE("landform: the sea bed takes part: no wall at the coast") {
  SUBCASE("a solved coast holds the critical slope on land, across the waterline and on the bed") {
    landform::Sketch sketch = golden_sketch();
    sketch.rock.formation.reset();
    sketch.rock.resistance = 0.0;
    sketch.critical_slope = 0.9;  // one critical slope everywhere
    // Nothing laid down, so the shelf is still a sea after four hundred thousand years: with
    // the default deposition the rivers fill it (the subcase below is the coast they build).
    sketch.deposition = 0.0;
    const landform::Landform land = solved(sketch);
    const f64 excess = worst_excess_mm(land, 0.9);
    u32 under = 0;
    for (u32 p = 0; p < land.width * land.height; ++p)
      if (land.height_mm[p] <= land.sea_level_mm) ++under;
    std::printf(
        "landform coast: worst excess over the critical slope %.1f mm, %u points under "
        "the sea\n",
        excess, under);
    CHECK(under > land.height * 3);  // there is a sea bed to speak of
    CHECK(excess <= 2.0);            // whole-millimetre rounding, twice
  }
  SUBCASE("a coast the rivers build out holds the critical slope too, delta fronts included") {
    // The same coast with deposition, and a sea wide and deep enough to outlast the run: the
    // rivers' sediment settles to the sea's level at their mouths and the fronts stand no
    // steeper than any other ground.
    landform::Sketch sketch = golden_sketch();
    sketch.rock.formation.reset();
    sketch.rock.resistance = 0.0;
    sketch.critical_slope = 0.9;
    sketch.sea->shelf_m = 4000.0;
    sketch.sea->depth_m = 600.0;
    const landform::Landform land = solved(sketch);
    const f64 excess = worst_excess_mm(land, 0.9);
    u32 under = 0, delta = 0;
    for (u32 p = 0; p < land.width * land.height; ++p) {
      if (land.height_mm[p] > land.sea_level_mm) continue;
      ++under;
      if (land.sediment[p] >= 1000) ++delta;
    }
    std::printf(
        "landform delta coast: worst excess %.1f mm, %u points under the sea, %u of them on a "
        "metre or more of sediment\n",
        excess, under, delta);
    CHECK(under > land.height * 3);
    CHECK(delta > land.height);  // the sea took sediment along the coast
    CHECK(excess <= 2.0);
    check_drains(land);
  }
  SUBCASE("a starting ground with a 370 m underwater wall loses it") {
    // Land at 50 m meeting a bed at -320 m in one 100 m cell: the proof of concept's fixed sea
    // bed. The sea is the east edge; nothing is raised; fifty thousand years.
    landform::StartField start;
    start.width = 64;
    start.height = 64;
    start.spacing_mm = 100000;
    start.data.resize(64 * 64);
    for (u32 z = 0; z < 64; ++z)
      for (u32 x = 0; x < 64; ++x)
        start.data[z * 64 + x] = x < 32 ? 50.0f : -320.0f;
    landform::Sketch sketch = plain_sketch(64, 100000);
    sketch.base_level_m.reset();
    sketch.edges.west = landform::EdgeKind::Closed;
    sketch.edges.north = landform::EdgeKind::Closed;
    sketch.edges.south = landform::EdgeKind::Closed;
    sketch.edges.east = landform::EdgeKind::Sea;
    sketch.sea = landform::Sea{};
    sketch.rock.variation = 0.0;
    sketch.critical_slope = 0.9;
    sketch.start = "wall.json";
    sketch.duration_yr = 50000;
    const landform::Landform land = solved(sketch, &start);
    // Before: 370,000 mm in one cell, 280,000 over the critical slope.
    const f64 excess = worst_excess_mm(land, 0.9);
    i32 worst_step = 0;
    for (u32 z = 0; z < 64; ++z)
      for (u32 x = 0; x + 1 < 64; ++x) {
        const i32 step = land.height_mm[z * 64 + x] - land.height_mm[z * 64 + x + 1];
        if (step > worst_step) worst_step = step;
      }
    std::printf(
        "landform wall: the largest step between neighbours is %d mm (370000 before), "
        "the worst excess %.1f mm\n",
        worst_step, excess);
    CHECK(worst_step <= 90002);  // the critical slope over a 100 m cell
    CHECK(excess <= 2.0);
    // The bed is still the bed and the land is still land: the wall became a slope, not a fill.
    CHECK(land.height_mm[32 * 64 + 63] == -320000);
    CHECK(land.height_mm[32 * 64 + 2] > 0);
    check_drains(land);
  }
}

TEST_CASE("landform: a starting heightfield is refined, and keeps its large shapes") {
  // A smooth massif of two summits, 450 m of relief on a 6.4 km square, cut for twenty thousand
  // years with nothing raised. Its edges keep their heights (no base level is named).
  const u32 side = 128;
  landform::StartField start;
  start.width = side;
  start.height = side;
  start.spacing_mm = 50000;
  start.data.resize(side * side);
  const auto bump = [](f64 x, f64 z, f64 cx, f64 cz, f64 radius) {
    const f64 d2 = ((x - cx) * (x - cx) + (z - cz) * (z - cz)) / (radius * radius);
    return d2 < 1.0 ? (1.0 - d2) * (1.0 - d2) : 0.0;
  };
  f32 lowest = 1.0e9f, highest = -1.0e9f;
  for (u32 z = 0; z < side; ++z)
    for (u32 x = 0; x < side; ++x) {
      const f64 px = 50.0 * static_cast<f64>(x), pz = 50.0 * static_cast<f64>(z);
      const f64 v = 100.0 + 0.01 * px + 350.0 * bump(px, pz, 2600.0, 3000.0, 2400.0) +
                    220.0 * bump(px, pz, 4300.0, 3600.0, 1700.0);
      const f32 f = static_cast<f32>(v);
      start.data[z * side + x] = f;
      if (f < lowest) lowest = f;
      if (f > highest) highest = f;
    }
  landform::Sketch sketch = plain_sketch(side, 50000);
  sketch.base_level_m.reset();
  sketch.start = "massif.json";
  sketch.duration_yr = 40000;
  const landform::Landform land = solved(sketch, &start);
  check_drains(land);

  // The low-pass: the mean over a square 17 points (800 m) wide.
  const i32 half = 8;
  f64 worst_low = 0.0, worst_point = 0.0;
  u32 channels = 0;
  for (i32 z = half; z + half < static_cast<i32>(side); z += 4)
    for (i32 x = half; x + half < static_cast<i32>(side); x += 4) {
      f64 before = 0.0, after = 0.0;
      for (i32 dz = -half; dz <= half; ++dz)
        for (i32 dx = -half; dx <= half; ++dx) {
          const u32 p = static_cast<u32>((z + dz) * static_cast<i32>(side) + x + dx);
          before += static_cast<f64>(start.data[p]);
          after += static_cast<f64>(land.height_mm[p]) / 1000.0;
        }
      const f64 change = std::fabs(after - before) / 289.0;
      if (change > worst_low) worst_low = change;
    }
  for (u32 p = 0; p < side * side; ++p) {
    const f64 change =
        std::fabs(static_cast<f64>(land.height_mm[p]) / 1000.0 - static_cast<f64>(start.data[p]));
    if (change > worst_point) worst_point = change;
    if (land.drainage_area[p] >= 64) ++channels;
  }
  const f64 relief = static_cast<f64>(highest - lowest);
  std::printf(
      "landform refine: relief %.1f m, the low-pass moved at most %.2f m, a point at "
      "most %.2f m, %u channel points\n",
      relief, worst_low, worst_point, channels);
  // The bound: the 800 m low-pass moves less than a twentieth of the relief (nothing is raised,
  // so what the run removes is gone), while single points (the valleys cut) move more than the
  // low-pass does.
  CHECK(worst_low < 0.05 * relief);
  CHECK(worst_point > 1.5 * worst_low);
  CHECK(channels > side);

  // No steps: the starting ground's own fields, its heights untouched.
  sketch.duration_yr = 0;
  const landform::Landform same = solved(sketch, &start);
  bool equal = true;
  for (u32 p = 0; p < side * side; ++p)
    if (same.height_mm[p] !=
        static_cast<i32>(std::floor(static_cast<f64>(start.data[p]) * 1000.0 + 0.5)))
      equal = false;
  CHECK(equal);
  CHECK(same.steps == 0u);
  check_drains(same);
}

TEST_CASE("landform: the files round-trip and a field answers a window") {
  engine::test::TempDir dir("landform");
  landform::Sketch sketch = golden_sketch();
  sketch.duration_yr = 20000;
  const landform::Landform land = solved(sketch);
  landform::WrittenFiles files;
  std::string error;
  REQUIRE_MESSAGE(landform::write_landform(land, sketch, dir.path(), "region", files, error),
                  error);

  // The heightfield is the tile set's format, a whole millimetre a float.
  landform::StartField back;
  REQUIRE_MESSAGE(landform::read_start_field(files.heightfield, back, error), error);
  CHECK(back.width == land.width);
  CHECK(back.height == land.height);
  CHECK(back.spacing_mm == land.spacing_mm);
  CHECK(back.origin_x_mm == land.origin_x_mm);
  CHECK(back.origin_z_mm == land.origin_z_mm);
  CHECK(back.vertical_mm_per_unit == 1.0);
  bool exact = true;
  for (u32 p = 0; p < land.width * land.height; ++p)
    if (static_cast<i32>(back.data[p]) != land.height_mm[p]) exact = false;
  CHECK(exact);

  landform::Fields header;
  REQUIRE_MESSAGE(landform::read_fields(files.fields, header, error), error);
  CHECK(header.width == land.width);
  CHECK(header.fields.size() == 16u);
  CHECK(header.version == 4u);
  // The rock's surface is the height less the sediment, point for point.
  const u32 fw = 11, fh = 13;
  Vector<i32> rock(fw * fh);
  Vector<u32> loose(fw * fh);
  REQUIRE_MESSAGE(landform::read_field_window(files.fields, header, "rock_floor", 5, 7, fw, fh,
                                              rock.data(), usize{fw} * fh * 4, error),
                  error);
  REQUIRE_MESSAGE(landform::read_field_window(files.fields, header, "sediment", 5, 7, fw, fh,
                                              loose.data(), usize{fw} * fh * 4, error),
                  error);
  bool floor_agrees = true;
  for (u32 z = 0; z < fh; ++z)
    for (u32 x = 0; x < fw; ++x)
      if (rock[z * fw + x] !=
          land.height_mm[(7 + z) * land.width + 5 + x] - static_cast<i32>(loose[z * fw + x]))
        floor_agrees = false;
  CHECK(floor_agrees);
  CHECK(header.solver_version == landform::k_solver_version);
  REQUIRE(header.sea_level_mm.has_value());
  CHECK(*header.sea_level_mm == 0);

  // A window inside the lattice, and one as wide as it.
  const u32 x0 = 5, z0 = 7, w = 11, h = 13;
  Vector<u32> window(w * h);
  REQUIRE_MESSAGE(landform::read_field_window(files.fields, header, "drainage_area", x0, z0, w, h,
                                              window.data(), usize{w} * h * 4, error),
                  error);
  bool same = true;
  for (u32 z = 0; z < h; ++z)
    for (u32 x = 0; x < w; ++x)
      if (window[z * w + x] != land.drainage_area[(z0 + z) * land.width + x0 + x]) same = false;
  CHECK(same);
  Vector<u8> rows(land.width * 3);
  REQUIRE_MESSAGE(landform::read_field_window(files.fields, header, "receiver", 0, 70, land.width,
                                              3, rows.data(), usize{land.width} * 3, error),
                  error);
  same = true;
  for (u32 i = 0; i < land.width * 3; ++i)
    if (rows[i] != land.receiver[70 * land.width + i]) same = false;
  CHECK(same);

  // Past the region's edge the fields hold nothing, and say so.
  CHECK_FALSE(landform::read_field_window(files.fields, header, "receiver", land.width - 2, 0, 3, 1,
                                          rows.data(), 3, error));
  CHECK(error.find("past the region's edge") != std::string::npos);
  CHECK_FALSE(
      landform::read_field_window(files.fields, header, "soil", 0, 0, 1, 1, rows.data(), 1, error));
}

TEST_CASE("landform: a sketch outside a range is refused by a sentence naming the field") {
  std::string error;
  landform::Sketch sketch = golden_sketch();
  REQUIRE(landform::validate_sketch(sketch, error));

  landform::Sketch bad = sketch;
  bad.origin_mm = {50, 0};
  CHECK_FALSE(landform::validate_sketch(bad, error));
  CHECK(error.find("origin_mm") != std::string::npos);

  bad = sketch;
  bad.sea.reset();
  CHECK_FALSE(landform::validate_sketch(bad, error));
  CHECK(error.find("sea") != std::string::npos);

  bad = sketch;
  bad.edges = landform::Edges{landform::EdgeKind::Closed, landform::EdgeKind::Closed,
                              landform::EdgeKind::Closed, landform::EdgeKind::Closed};
  bad.outlets.clear();
  CHECK_FALSE(landform::validate_sketch(bad, error));
  CHECK(error.find("outlet") != std::string::npos);

  bad = sketch;
  bad.rock.formation->dip_deg = 89.0f;
  CHECK_FALSE(landform::validate_sketch(bad, error));
  CHECK(error.find("dip_deg") != std::string::npos);

  bad = sketch;
  bad.erodibility_per_yr = 1.0;
  CHECK_FALSE(landform::validate_sketch(bad, error));
  CHECK(error.find("erodibility_per_yr") != std::string::npos);

  bad = sketch;
  bad.duration_yr = 0;
  CHECK_FALSE(landform::validate_sketch(bad, error));
  CHECK(error.find("starting heightfield") != std::string::npos);

  // A start the sketch names and nobody read; a start that does not cover the lattice.
  bad = sketch;
  bad.start = "missing.json";
  landform::Landform land;
  CHECK_FALSE(landform::solve(bad, nullptr, {}, land, error));
  landform::StartField small;
  small.width = 4;
  small.height = 4;
  small.spacing_mm = 100000;
  small.data.resize(16);
  CHECK_FALSE(landform::solve(bad, &small, {}, land, error));
  CHECK(error.find("cover") != std::string::npos);
}

// ---- deposition (landform.md, "Deposition") -----------------------------------------------------

namespace {

// |a - b| over the larger of the two.
f64 relative(f64 a, f64 b) {
  const f64 scale = std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b);
  return scale > 0.0 ? std::fabs(a - b) / scale : 0.0;
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

}  // namespace

TEST_CASE("landform: what is eroded is deposited or leaves through the held points") {
  // The golden sketch (a sea, a closed edge, two open ones, an outlet, lakes behind its ridge):
  // every part of the deposition runs. Two identities, each of sums the solver keeps separately:
  // what the rivers cut and the slopes shed was laid down again or carried out through a held
  // point; and the ground's volume changed by what was raised less what went.
  const landform::Landform land = solved(golden_sketch());
  const landform::Budget& b = land.budget;
  std::printf(
      "landform budget: uplifted %.6g eroded %.6g deposited %.6g left %.6g crept out %.6g "
      "settled %.6g unrouted %.6g change %.6g m3\n",
      b.uplifted_m3, b.eroded_m3, b.deposited_m3, b.left_m3, b.crept_out_m3, b.settled_m3,
      b.unrouted_m3, b.change_m3);
  CHECK(b.eroded_m3 > 0.0);
  CHECK(b.deposited_m3 > 0.01 * b.eroded_m3);
  CHECK(b.left_m3 > 0.01 * b.eroded_m3);
  // The bound: nine significant digits. The sums are of 7,680 points over 80 steps in f64, and
  // nothing but the rounding of those additions separates the two sides.
  CHECK(relative(b.eroded_m3, b.deposited_m3 + b.left_m3 + b.unrouted_m3) < 1.0e-9);
  CHECK(relative(b.change_m3, b.uplifted_m3 - b.left_m3 - b.crept_out_m3 - b.settled_m3 -
                                  b.unrouted_m3) < 1.0e-9);
  // The settling after the last step, and what the last step's slopes shed, are the two places
  // material leaves the model: under a fiftieth of what was eroded, together.
  CHECK(b.settled_m3 >= 0.0);
  CHECK(b.unrouted_m3 >= 0.0);
  CHECK(b.settled_m3 + b.unrouted_m3 < 0.02 * b.eroded_m3);
  // The field: sediment only under a point, never more of it than the ground above the sea bed's
  // lowest point could hold, and some of it somewhere.
  u32 covered = 0;
  for (u32 p = 0; p < land.width * land.height; ++p)
    if (land.sediment[p] >= 1000) ++covered;
  CHECK(covered > 50u);

  // With the coefficient at zero a river lays nothing down: the detachment-limited law.
  landform::Sketch bare = golden_sketch();
  bare.deposition = 0.0;
  bare.edges.west = landform::EdgeKind::Open;  // no sea, so nothing settles in one either
  bare.sea.reset();
  bare.outlets.clear();
  const landform::Landform cut = solved(bare);
  CHECK(relative(cut.budget.eroded_m3,
                 cut.budget.deposited_m3 + cut.budget.left_m3 + cut.budget.unrouted_m3) < 1.0e-9);
}

TEST_CASE("landform: with deposition a steady river stands 1 + G times as steep") {
  // Yuan et al. 2019, their equation 24: at steady state S = (1 + G) (U / K) A^-1/2, the
  // concavity unchanged. Stream power and deposition alone, sediment as erodible as the rock.
  landform::Sketch sketch = plain_sketch(64, 200000);
  // The law with deposition and nothing else: 1 + G is its steady state, not that of a river
  // that also wears its banks and divides on its plain (stage C), so those two are off here.
  sketch.lateral_erosion = 0.0;
  sketch.divide_below_slope = 0.0;
  sketch.uplift.regional_mm_per_yr = 1.0;
  sketch.border_reach_m = 0.0;
  sketch.erodibility_per_yr = 0.00004;
  sketch.diffusivity_m2_per_yr = 0.0;
  sketch.critical_slope = 4.0;
  sketch.critical_slope_resistant = 4.0;
  sketch.rock.variation = 0.0;
  sketch.sediment_erodibility = 1.0;
  sketch.deposition = 1.0;
  sketch.duration_yr = 3000000;
  sketch.step_yr = 10000;
  const landform::Landform land = solved(sketch);
  const u32 n = land.width * land.height;
  // The steepness S sqrt(A) K / U over the channels (eight cells and more), where the discrete
  // form's (N - 1) / N is within an eighth of one.
  f64 sum = 0.0, rate = 0.0;
  u32 count = 0, free_points = 0;
  Vector<f64> log_area, log_slope;
  for (u32 p = 0; p < n; ++p) {
    if (land.receiver[p] == landform::k_no_receiver) continue;
    ++free_points;
    rate += static_cast<f64>(land.erosion_rate[p]);
    if (land.drainage_area[p] < 8 || land.slope[p] <= 0) continue;
    const f64 area = static_cast<f64>(land.drainage_area[p]);
    const f64 slope = static_cast<f64>(land.slope[p]) * 1.0e-6;
    // U / K = 25 m; A in cells of 200 m, so S sqrt(N) * 200 / 25 is the factor.
    sum += slope * std::sqrt(area) * 200.0 / 25.0;
    ++count;
    log_area.push_back(std::log(area));
    log_slope.push_back(std::log(slope));
  }
  const f64 steepness = sum / static_cast<f64>(count);
  const f64 concavity = -regress(log_area, log_slope);
  const f64 mean_rate = rate / static_cast<f64>(free_points);
  std::printf(
      "landform deposition steady state: steepness factor %.4f over %u channel points, "
      "concavity %.4f, mean rate %.1f um/yr\n",
      steepness, count, concavity, mean_rate);
  CHECK(mean_rate == doctest::Approx(1000.0).epsilon(0.02));
  // 1 + G (N - 1) / N averaged over the channels is 1.9 to 2; held within a twentieth.
  CHECK(steepness > 1.85);
  CHECK(steepness < 2.05);
  CHECK(concavity == doctest::Approx(0.5).epsilon(0.06));
}

namespace {

// What the dammed valley's floor measures (the two cases below).
struct DammedFloor {
  f64 upper = 0.0;       // the run of fill about the axis, points, where the fill begins
  f64 lower = 0.0;       // and in the lower third
  i32 worst_across = 0;  // the largest cross fall of that run in the lower third, millimetres
  u32 thickest = 0;      // the thickest fill, millimetres
  f64 river = 0.0;       // the river's own floor in the lower third, points a column
  i32 worst_river = 0;   // the largest cross fall of the river's own floor, millimetres
  i32 worst_step = 0;    // the tallest step from the river's floor to the fill beside it
};

// A straight V-shaped valley falling east at 0.04 between sides at 0.5, its mouth held 90 m
// above its bed (a base level that rose: a moraine, a landslide, a trunk valley's own fill). The
// ground rises at 0.3 mm/yr and sheds into it for a hundred thousand years.
DammedFloor dammed_valley(f64 lateral_erosion, f64 divide_below_slope, const char* what) {
  const u32 wide = 96, deep = 64;
  const landform::StartField start = start_from(wide, deep, 50000, [](f64 x, f64 z) {
    return 300.0 - 0.04 * x + 0.5 * std::fabs(z - 1600.0);
  });
  landform::Sketch sketch = plain_sketch(wide, 50000);
  sketch.height = deep;
  sketch.edges.west = landform::EdgeKind::Closed;
  sketch.edges.north = landform::EdgeKind::Closed;
  sketch.edges.south = landform::EdgeKind::Closed;
  sketch.base_level_m = 200.0;
  sketch.border_reach_m = 0.0;
  sketch.erodibility_per_yr = 0.00002;
  sketch.start = "valley.json";
  sketch.duration_yr = 100000;
  sketch.lateral_erosion = lateral_erosion;
  sketch.divide_below_slope = divide_below_slope;
  const landform::Landform land = solved(sketch, &start);
  check_drains(land);
  DammedFloor out;
  // The floor of a column: the run of points about the axis (row 32) on five metres or more of
  // fill.
  u32 widths[96] = {};
  u32 columns = 0;
  for (u32 x = 0; x + 1 < wide; ++x) {
    if (land.sediment[32 * wide + x] < 5000) continue;
    u32 first = 32, last = 32;
    while (first > 0 && land.sediment[(first - 1) * wide + x] >= 5000)
      --first;
    while (last + 1 < deep && land.sediment[(last + 1) * wide + x] >= 5000)
      ++last;
    u32 bed = 32;
    i32 lowest = land.height_mm[32 * wide + x], highest = lowest;
    for (u32 z = first; z <= last; ++z) {
      const i32 mm = land.height_mm[z * wide + x];
      if (mm < lowest) {
        lowest = mm;
        bed = z;
      }
      if (mm > highest) highest = mm;
    }
    widths[x] = last - first + 1;
    if (x < 80) continue;
    // The lower reach, where the fill is the lake's and the river's plain over it.
    if (highest - lowest > out.worst_across) out.worst_across = highest - lowest;
    // The river's own floor there: the points about the bed that stand within the water's depth
    // of it, the depth the bank rule itself gives the reach (0.4 Q^0.35, Q the area times
    // 4e-6 m/s: landform.md, "Lateral erosion"). A bank the river takes is lowered to its
    // reach's receiver, so it joins this floor; fill it has not taken stands above it.
    const f64 area_m2 = static_cast<f64>(land.discharge[bed * wide + x]) * 2500.0;
    const i32 water = static_cast<i32>(400.0 * std::pow(area_m2 * 4.0e-6, 0.35));
    u32 a = bed, b = bed;
    while (a > first && land.height_mm[(a - 1) * wide + x] <= lowest + water)
      --a;
    while (b < last && land.height_mm[(b + 1) * wide + x] <= lowest + water)
      ++b;
    out.river += static_cast<f64>(b - a + 1);
    ++columns;
    for (u32 z = a; z <= b; ++z)
      if (land.height_mm[z * wide + x] - lowest > out.worst_river)
        out.worst_river = land.height_mm[z * wide + x] - lowest;
    if (a > first && land.height_mm[(a - 1) * wide + x] - lowest > out.worst_step)
      out.worst_step = land.height_mm[(a - 1) * wide + x] - lowest;
    if (b < last && land.height_mm[(b + 1) * wide + x] - lowest > out.worst_step)
      out.worst_step = land.height_mm[(b + 1) * wide + x] - lowest;
  }
  if (columns > 0) out.river /= static_cast<f64>(columns);
  // Thirds of the filled reach, upstream to downstream (the fill begins near x = 50 points,
  // where the bed stood at the mouth's level).
  for (u32 x = 50; x < 65; ++x)
    out.upper += static_cast<f64>(widths[x]) / 15.0;
  for (u32 x = 80; x < 95; ++x)
    out.lower += static_cast<f64>(widths[x]) / 15.0;
  for (u32 p = 0; p < wide * deep; ++p)
    if (land.sediment[p] > out.thickest) out.thickest = land.sediment[p];
  std::printf(
      "landform valley floor (%s): %.1f points wide upstream, %.1f downstream, level across "
      "within %d mm, thickest fill %u mm; the river's own floor %.1f points a column, level "
      "within %d mm, the tallest step beside it %d mm\n",
      what, out.upper, out.lower, out.worst_across, out.thickest, out.river, out.worst_river,
      out.worst_step);
  return out;
}

}  // namespace

TEST_CASE("landform: a dammed valley fills to a floor level across that widens downstream") {
  // The case as it was written, for what it was written for: a river of one thread that takes
  // no bank (stage C's lateral erosion and divided flow off, as the steady-state cases run).
  const DammedFloor floor = dammed_valley(0.0, 0.0, "one thread, no banks");
  CHECK(floor.lower >= 5.0);               // 250 m of floor and more at the lower end
  CHECK(floor.lower > 1.5 * floor.upper);  // and it widens downstream
  // Level across: the start's V fell 87 m over the floor's 350 m; the floor's own cross fall is
  // the river's bed, cut a few metres into the fill, under the feet of the side slopes' aprons.
  CHECK(floor.worst_across <= 8000);
  CHECK(floor.thickest > 50000);  // the fill is tens of metres deep at the mouth
}

TEST_CASE("landform: a river that moves across a dammed valley's fill keeps a floor of its own") {
  // The same valley with stage C's defaults (the flow divides on the fill and the river takes
  // its banks), beside the same with no bank taken. What the bank rule gives, and no more
  // (landform.md, "Lateral erosion"): a bank is lowered to its reach's receiver once the wear
  // reaches what the water touches of it, **whatever stands above the water**, and nothing is
  // ever raised. So:
  const DammedFloor moving = dammed_valley(1.0, 0.1, "banks and divided flow");
  const DammedFloor still = dammed_valley(0.0, 0.1, "divided flow, no banks");
  CHECK(moving.lower >= 5.0);
  CHECK(moving.lower > 1.5 * moving.upper);
  CHECK(moving.thickest > 50000);
  // 1. The river's own floor (the points within the water's depth of its bed) is wider than a
  //    thread, and no narrower than where it takes no bank: a taken bank joins it.
  CHECK(moving.river > 1.0);
  CHECK(moving.river >= still.river);
  // 2. Beside that floor stands what the river has not taken, cut back to a scarp: a taller
  //    step than the graded foot of the aprons where no bank is taken.
  CHECK(moving.worst_step > still.worst_step);
  // 3. And the rule puts no bound of its own on that scarp, since a bank of fill falls when its
  //    foot is worn however tall it is. What bounds it is the ground: the fill beside the river
  //    rises with the uplift while the river holds its grade to the dam, so a bank left
  //    standing gains on the bed by no more than the ground rose in the run, 0.3 mm/yr for a
  //    hundred thousand years. (The whole run's cross fall, 12.9 m here, is that scarp on the
  //    side slopes' aprons; stage C held it under "a quarter of the 87 m the V fell", which
  //    nothing derives.)
  CHECK(moving.worst_step <= 30000);
  CHECK(moving.worst_across <= 30000 + 8000);  // the scarp over the one thread's 8 m of floor
}

TEST_CASE("landform: a massif's rivers leave a fan on the plain at its foot") {
  // A massif two kilometres across rises at 2 mm/yr beside a plain that does not rise and
  // drains to open edges north and south. For three hundred thousand years its rivers carry
  // what they cut out onto the plain.
  const u32 side = 96;
  landform::Sketch sketch = plain_sketch(side, 50000);
  sketch.edges.west = landform::EdgeKind::Closed;
  sketch.edges.east = landform::EdgeKind::Closed;
  landform::Ridge ridge;
  ridge.points.push_back(landform::Point{2400.0, 900.0});
  ridge.points.push_back(landform::Point{2400.0, 1500.0});
  ridge.rate_mm_per_yr = 2.0;
  ridge.reach_m = 900.0;
  sketch.uplift.ridges.push_back(ridge);
  sketch.erodibility_per_yr = 0.00002;
  sketch.roughness_m = 0.2;
  sketch.duration_yr = 300000;
  sketch.step_yr = 2000;
  const landform::Landform land = solved(sketch);
  check_drains(land);
  // Along a line across the plain 400 m south of the massif's foot (row 56): the ground stands
  // highest opposite the massif and falls away to both sides, so a contour there bows away
  // from the mouths, and far to the side it stands under what an even sheet would.
  const u32 row = 56, mid = 48;
  const auto height_at = [&](u32 x) { return land.height_mm[row * side + x]; };
  i32 crown = height_at(mid);
  for (u32 x = mid - 3; x <= mid + 3; ++x)
    if (height_at(x) > crown) crown = height_at(x);
  const i32 near_west = height_at(mid - 14), near_east = height_at(mid + 14);
  const i32 far_west = height_at(mid - 34), far_east = height_at(mid + 34);
  u32 apex = 0;
  for (u32 x = mid - 3; x <= mid + 3; ++x)
    if (land.sediment[50 * side + x] > apex) apex = land.sediment[50 * side + x];
  std::printf(
      "landform fan: 400 m out the ground stands %d mm opposite the massif, %d and %d mm "
      "700 m to either side, %d and %d mm 1,700 m; %u mm of sediment at the foot\n",
      crown, near_west, near_east, far_west, far_east, apex);
  // What an even sheet of the same load would stand at along the line: its mean.
  i64 sum = 0;
  for (u32 x = 0; x < side; ++x)
    sum += height_at(x);
  const i32 even = static_cast<i32>(sum / side);
  std::printf("landform fan: an even sheet along that line would stand %d mm\n", even);
  CHECK(crown > 3000);
  CHECK(crown > near_west + 1000);
  CHECK(crown > near_east + 1000);
  CHECK(near_west > far_west);
  CHECK(near_east > far_east);
  // The deposit thins away from the mouths, and the bound on the far points is what the rule
  // delivers there. By the shares, none of the water of the mouths that face the line reaches
  // them: down a plane a point's flow goes half straight on and a quarter to each diagonal
  // (Quinn's slope times contour length), so over the eight rows from the massif's foot it
  // spreads two points either way, and the far points stand sixteen beyond the massif's edge.
  // What reaches them is the massif's east and west flanks' own rivers, which cross the plain
  // beside it and turn south: the plain there was never going to be "as it was" (this check was
  // "under a metre" while it measured 0.93 m, then "under half the crown"; neither was derived).
  // A point lays down G times the load a unit of its water carries, and that water has run
  // farther over the plain than the water opposite the massif, laying down and gathering the
  // plain's clear water all the way: so its deposit is under what the same load laid evenly
  // along the line would be, and the crown's is over it.
  CHECK(far_west < even);
  CHECK(far_east < even);
  CHECK(crown > even);
  CHECK(apex > 3000);
}
TEST_CASE("landform: a lake's floor fills level, and its river's mouth builds a delta") {
  // A plane falling south at 0.1 into a basin 40 m deep and a kilometre across, whose south rim
  // is its sill; the south edge is open. Nothing rises: the slope's rivers carry what they cut
  // into the basin for ten thousand years, which does not fill it.
  const u32 side = 80;
  const auto ground = [](f64 x, f64 z) {
    const f64 d2 = ((x - 2000.0) * (x - 2000.0) + (z - 2600.0) * (z - 2600.0)) / (500.0 * 500.0);
    const f64 bowl = d2 < 1.0 ? 40.0 * (1.0 - d2) : 0.0;
    return 100.0 + (z < 2000.0 ? 0.1 * (2000.0 - z) : 0.0) - bowl - 0.005 * z;
  };
  const landform::StartField start = start_from(side, side, 50000, ground);
  landform::Sketch sketch = plain_sketch(side, 50000);
  sketch.edges.west = landform::EdgeKind::Closed;
  sketch.edges.east = landform::EdgeKind::Closed;
  sketch.edges.north = landform::EdgeKind::Closed;
  sketch.base_level_m.reset();
  sketch.border_reach_m = 0.0;
  sketch.erodibility_per_yr = 0.00002;
  sketch.start = "basin.json";
  sketch.duration_yr = 10000;
  sketch.step_yr = 1000;
  // What a run leaves: the floor (the basin's points within 250 m of its centre, on the side
  // away from the mouths), how level it
  // is and where it stands, the water still over it, and the sediment on the basin's north
  // shelf, where the slope's rivers enter (250 to 500 m north of the centre).
  struct Lake {
    i32 lowest = 0, highest = 0;
    u32 floor_points = 0, water_mm = 0;
    u64 shelf_mm = 0;
  };
  const auto measure = [&](f64 share) {
    sketch.delta_share = share;
    const landform::Landform land = solved(sketch, &start);
    check_drains(land);
    Lake lake;
    for (u32 z = 0; z < side; ++z)
      for (u32 x = 0; x < side; ++x) {
        const f64 dx = 50.0 * static_cast<f64>(x) - 2000.0;
        const f64 dz = 50.0 * static_cast<f64>(z) - 2600.0;
        const f64 d2 = dx * dx + dz * dz;
        const u32 p = z * side + x;
        if (d2 <= 250.0 * 250.0 && dz >= 0.0) {
          const i32 mm = land.height_mm[p];
          if (lake.floor_points == 0 || mm < lake.lowest) lake.lowest = mm;
          if (lake.floor_points == 0 || mm > lake.highest) lake.highest = mm;
          ++lake.floor_points;
          if (land.fill[p] > lake.water_mm) lake.water_mm = land.fill[p];
        } else if (d2 <= 500.0 * 500.0 && dz < -250.0) {
          lake.shelf_mm += land.sediment[p];
        }
      }
    std::printf(
        "landform lake (delta share %.1f): %u floor points from %d to %d mm under %u mm of "
        "water, %llu mm of sediment summed over the north shelf\n",
        share, lake.floor_points, lake.lowest, lake.highest, lake.water_mm,
        static_cast<unsigned long long>(lake.shelf_mm));
    return lake;
  };
  const Lake plain = measure(0.0);
  const Lake delta = measure(0.5);
  // The start's floor falls 10 m from its rim to its centre over these 250 m; what settled
  // levelled it to the millimetres the heights round to and the creep of ten thousand years.
  CHECK(plain.floor_points > 30u);
  CHECK(plain.water_mm > 1000u);  // still a lake
  CHECK(plain.highest - plain.lowest <= 50);
  CHECK(delta.highest - delta.lowest <= 50);
  // With half of what arrives settling at the mouths the shelf holds more and the floor stands
  // lower.
  CHECK(delta.shelf_mm > plain.shelf_mm + plain.shelf_mm / 4);
  CHECK(delta.highest < plain.lowest);
}

// ---- structure (landform.md, "Structure")
// --------------------------------------------------------

TEST_CASE(
    "landform: under dipping beds a ridge's two sides differ and a hard bed stands as a step") {
  // A block rising at 1 mm/yr between open edges north and south, so its divide runs east to
  // west, in beds that dip 15 degrees to the north: 300 m of soft rock under 200 m of hard,
  // repeating, the hard a tenth as erodible. A million years carries a kilometre of rock, two
  // periods of the beds, up through the surface.
  const u32 side = 96;
  landform::Sketch sketch = plain_sketch(side, 100000);
  sketch.edges.west = landform::EdgeKind::Closed;
  sketch.edges.east = landform::EdgeKind::Closed;
  sketch.uplift.regional_mm_per_yr = 1.0;
  sketch.erodibility_per_yr = 0.00004;
  sketch.rock.variation = 0.0;
  sketch.rock.formation = dipping_formation(15.0f, -90.0f, 300.0f, 200.0f);
  // The contrast is this test's own, not the default: what it measures is that the beds are
  // read, in the rock's frame, and a tenfold contrast makes that a ratio with room in it. At the
  // default of 2 (stage C: inside the published range) the same ratios stand, smaller.
  sketch.rock.contrast = 10.0;
  sketch.duration_yr = 1000000;
  const landform::Landform land = solved(sketch);
  check_drains(land);
  // The two sides: the divide of each column (its highest point), and the mean gradient of the
  // ground north of it (where the surface falls the way the beds dip) and south of it (where it
  // cuts across their edges), over the middle columns.
  f64 north_sum = 0.0, south_sum = 0.0, divide_sum = 0.0;
  u32 north_count = 0, south_count = 0;
  // A hard bed as a step: the mean gradient where the rock at the surface is hard against where
  // it is soft, over the same columns.
  f64 hard_sum = 0.0, soft_sum = 0.0;
  u32 hard_count = 0, soft_count = 0;
  // And how wide a hard bed's outcrop is on each side: the points of hard rock over the runs of
  // them along a column. Down the dip the surface runs with the bed and its outcrop is a plate;
  // across the beds it is an edge.
  u32 plate_points = 0, plate_runs = 0, edge_points = 0, edge_runs = 0;
  for (u32 x = 16; x < side - 16; ++x) {
    u32 top = 0;
    for (u32 z = 1; z < side; ++z)
      if (land.height_mm[z * side + x] > land.height_mm[top * side + x]) top = z;
    divide_sum += static_cast<f64>(top);
    for (u32 z = 8; z + 8 < side; ++z) {
      if (z == top || land.resistance[z * side + x] < 230) continue;
      const bool starts = z == 8 || z - 1 == top || land.resistance[(z - 1) * side + x] < 230;
      (z < top ? plate_points : edge_points) += 1;
      if (starts) (z < top ? plate_runs : edge_runs) += 1;
    }
    for (u32 z = 8; z + 8 < side; ++z) {
      const u32 p = z * side + x;
      if (land.slope[p] <= 0) continue;
      const f64 slope = static_cast<f64>(land.slope[p]) * 1.0e-6;
      if (z < top) {
        north_sum += slope;
        ++north_count;
      } else if (z > top) {
        south_sum += slope;
        ++south_count;
      }
      if (land.resistance[p] >= 230) {
        hard_sum += slope;
        ++hard_count;
      } else if (land.resistance[p] <= 25) {
        soft_sum += slope;
        ++soft_count;
      }
    }
  }
  const f64 north = north_sum / static_cast<f64>(north_count);
  const f64 south = south_sum / static_cast<f64>(south_count);
  const f64 hard = hard_sum / static_cast<f64>(hard_count);
  const f64 soft = soft_sum / static_cast<f64>(soft_count);
  const f64 divide = divide_sum / static_cast<f64>(side - 32);
  std::printf(
      "landform structure: mean gradient %.4f north of the divide (down the dip) and %.4f "
      "south (across the beds), the divide at row %.1f of 96; %.4f on hard rock (%u points) "
      "and %.4f on soft (%u)\n",
      north, south, divide, hard, hard_count, soft, soft_count);
  const f64 plate = static_cast<f64>(plate_points) / static_cast<f64>(plate_runs);
  const f64 edge = static_cast<f64>(edge_points) / static_cast<f64>(edge_runs);
  std::printf(
      "landform structure: a hard bed's outcrop is %.2f points wide down the dip and %.2f "
      "across the beds\n",
      plate, edge);
  CHECK(hard_count > 200u);
  CHECK(soft_count > 200u);
  // The same sketch in uniform rock is symmetric: the comparison the asymmetry is against.
  sketch.rock.formation.reset();
  sketch.rock.resistance = 0.4;
  const landform::Landform even = solved(sketch);
  f64 even_north = 0.0, even_south = 0.0;
  u32 even_north_count = 0, even_south_count = 0;
  for (u32 x = 16; x < side - 16; ++x) {
    u32 top = 0;
    for (u32 z = 1; z < side; ++z)
      if (even.height_mm[z * side + x] > even.height_mm[top * side + x]) top = z;
    for (u32 z = 8; z + 8 < side; ++z) {
      const u32 p = z * side + x;
      if (even.slope[p] <= 0 || z == top) continue;
      (z < top ? even_north : even_south) += static_cast<f64>(even.slope[p]) * 1.0e-6;
      ++(z < top ? even_north_count : even_south_count);
    }
  }
  const f64 even_ratio = (even_south / static_cast<f64>(even_south_count)) /
                         (even_north / static_cast<f64>(even_north_count));
  std::printf("landform structure: in uniform rock the two sides' gradients stand %.4f to 1\n",
              even_ratio);
  // The stated ratios. A hard bed stands as a step: the ground on it is over one and a half
  // times as steep as on soft rock (measured 2.2). The two sides differ: across the beds the
  // ground is a twentieth steeper than down the dip (measured 1.09 to 1, and 1.0002 to 1 in
  // uniform rock), and a hard bed's outcrop is over one and a half times as wide down the dip,
  // where the surface runs with it, as across the beds, where it is an edge (measured 1.93).
  CHECK(hard > 1.5 * soft);
  CHECK(south > 1.05 * north);
  CHECK(even_ratio == doctest::Approx(1.0).epsilon(0.01));
  CHECK(plate > 1.5 * edge);
}

// ---- uplift along a ridge (landform.md, "Uplift along a ridge")
// ----------------------------------

TEST_CASE("landform: a ridge's uplift varies along its line, more at long wavelengths than short") {
  // One step with next to no erosion, so the ground along the line is the line's uplift: a
  // straight ridge 25 km long read every 50 m, raised for a thousand years.
  landform::Sketch sketch = plain_sketch(512, 50000);
  sketch.height = 16;
  sketch.edges.north = landform::EdgeKind::Closed;
  sketch.edges.south = landform::EdgeKind::Closed;
  sketch.border_reach_m = 0.0;
  sketch.roughness_m = 0.0;
  landform::Ridge ridge;
  ridge.points.push_back(landform::Point{0.0, 400.0});
  ridge.points.push_back(landform::Point{25550.0, 400.0});
  ridge.rate_mm_per_yr = 2.0;
  ridge.reach_m = 300.0;
  ridge.variation = 0.5;
  ridge.variation_length_m = 3200.0;
  sketch.uplift.ridges.push_back(ridge);
  sketch.erodibility_per_yr = 0.00000001;
  sketch.diffusivity_m2_per_yr = 0.0;
  sketch.critical_slope = 4.0;
  sketch.critical_slope_resistant = 4.0;
  sketch.rock.variation = 0.0;
  sketch.deposition = 0.0;
  sketch.duration_yr = 1000;
  sketch.step_yr = 1000;
  const landform::Landform land = solved(sketch);
  // Row 8 is the line (z = 400 m): 2 m where the rate is 2 mm/yr.
  const u32 first = 8, last = 504;
  const auto rate = [&](u32 x) { return static_cast<f64>(land.height_mm[8 * 512 + x]) / 1000.0; };
  f64 lowest = rate(first), highest = lowest, sum = 0.0;
  for (u32 x = first; x < last; ++x) {
    const f64 v = rate(x);
    if (v < lowest) lowest = v;
    if (v > highest) highest = v;
    sum += v;
  }
  const f64 mean = sum / static_cast<f64>(last - first);
  // The mean squared difference between stations a lag apart: of a noise whose spectrum falls
  // as the wavenumber squared it grows with the lag, about in proportion below the longest
  // wavelength.
  const auto structure = [&](u32 lag) {
    f64 total = 0.0;
    for (u32 x = first; x + lag < last; ++x) {
      const f64 d = rate(x + lag) - rate(x);
      total += d * d;
    }
    return total / static_cast<f64>(last - first - lag);
  };
  const f64 near = structure(4);  // 200 m
  const f64 far = structure(32);  // 1,600 m, half the longest wavelength
  std::printf(
      "landform ridge: the rate along the line runs %.3f to %.3f about a mean of %.3f mm/yr; "
      "the mean squared difference is %.5f at 200 m and %.5f at 1,600 m (%.1f times)\n",
      lowest, highest, mean, near, far, far / near);
  CHECK(mean == doctest::Approx(2.0).epsilon(0.15));
  CHECK(lowest >= 2.0 * 0.5 - 0.001);
  CHECK(highest <= 2.0 * 1.5 + 0.001);
  CHECK(highest - lowest > 0.6);  // it does vary: over a third of the range it may
  CHECK(far > 4.0 * near);        // and far more over a long reach than a short one

  // With no variation the line is raised evenly.
  sketch.uplift.ridges[0].variation = 0.0;
  const landform::Landform even = solved(sketch);
  i32 even_low = even.height_mm[8 * 512 + first], even_high = even_low;
  for (u32 x = first; x < last; ++x) {
    const i32 mm = even.height_mm[8 * 512 + x];
    if (mm < even_low) even_low = mm;
    if (mm > even_high) even_high = mm;
  }
  CHECK(even_high - even_low <= 1);
}

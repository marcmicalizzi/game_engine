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
  sketch.uplift.regional_mm_per_yr = 1.0;
  sketch.border_reach_m = 0.0;
  sketch.erodibility_per_yr = 0.00002;
  sketch.diffusivity_m2_per_yr = 0.0;
  sketch.critical_slope = 4.0;
  sketch.critical_slope_resistant = 4.0;
  sketch.rock.variation = 0.0;
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
    sketch.rock.bands.clear();
    sketch.rock.resistance = 0.0;  // one critical slope everywhere: 0.9
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
  CHECK(header.fields.size() == 5u);
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
  bad.rock.bands[1].top_m = 100.0;
  CHECK_FALSE(landform::validate_sketch(bad, error));
  CHECK(error.find("ascend") != std::string::npos);

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

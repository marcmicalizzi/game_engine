// Which lights reach a point (light_grid.h; R87; docs/subsystems/renderer.md, "Lights by reach"):
// no device. The promise the culled picture rests on — a light whose windowed inverse square is
// above zero at a point has its bit in the point's cell — checked with the shader's f32 arithmetic
// on the distance and on the cell. `lamp_tests.cpp` holds the GPU's culled picture to the
// unculled one's bytes.

#include <domain/gfx/light_grid.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

using namespace engine;
using namespace engine::gfx;

namespace {

// brdf.slang's `brdf_distance_attenuation`, operation for operation in f32.
f32 attenuation(Vec3 light, f32 radius, Vec3 p) {
  const Vec3 d{light.x - p.x, light.y - p.y, light.z - p.z};
  const f32 distance_sq = d.x * d.x + d.y * d.y + d.z * d.z;
  f32 ratio = distance_sq / std::max(radius * radius, 1e-8f);
  ratio = std::clamp(ratio, 0.0f, 1.0f);
  const f32 window = 1.0f - ratio * ratio;
  return window * window / std::max(distance_sq, 1e-8f);
}

ResolveLight point(Vec3 at, f32 reach) {
  ResolveLight light{};
  light.position_radius = Vec4{at, reach};
  light.color_intensity = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
  light.direction_type = Vec4{0.0f, 0.0f, 0.0f, k_light_point};
  return light;
}

ResolveLight moon() {
  ResolveLight light{};
  light.position_radius = Vec4{0.0f, 1.0f, 0.0f, 0.0f};
  light.color_intensity = Vec4{1.0f, 1.0f, 1.0f, 1e-6f};
  light.direction_type = Vec4{0.0f, 0.0f, 0.0f, k_light_directional};
  return light;
}

bool has_bit(const LightGrid& grid, const std::vector<u32>& cells, u32 cell, u32 light) {
  const u32 bit = light - grid.first;
  return (cells[u64{cell} * grid.words + bit / 32] >> (bit % 32) & 1u) != 0;
}

// Every light that reaches `p` (attenuation above zero) is walked there: a leading one, or a bit
// of the cell the reader finds.
u32 check_point(const std::vector<ResolveLight>& lights, const LightGrid& grid,
                const std::vector<u32>& cells, Vec3 p) {
  u32 reached = 0;
  u32 cell = 0;
  const bool inside = light_grid_cell(grid, p, cell);
  for (u32 i = grid.first; i < lights.size(); ++i) {
    if (!(attenuation(lights[i].position_radius.xyz(), lights[i].position_radius.w, p) > 0.0f))
      continue;
    ++reached;
    REQUIRE(inside);
    CHECK(has_bit(grid, cells, cell, i));
  }
  return reached;
}

}  // namespace

TEST_CASE("light grid: every light that reaches a point is in the point's cell") {
  std::mt19937 rng(87);
  std::uniform_real_distribution<f32> unit(0.0f, 1.0f);
  std::vector<u32> cells(k_light_grid_words);
  u32 reached = 0;
  u32 near_edge = 0;
  // Villages of lamps at 0, near the origin, and two kilometres from the eye (the frame's space
  // is the eye's), small reaches and large, a few lights and the frame's room.
  const f32 offsets[] = {0.0f, 37.5f, 2000.0f};
  const u32 counts[] = {1, 5, 64, 322};
  for (f32 offset : offsets) {
    for (u32 count : counts) {
      std::vector<ResolveLight> lights;
      lights.push_back(moon());
      for (u32 i = 0; i < count; ++i) {
        const Vec3 at{offset + 120.0f * unit(rng), 2.6f + 4.0f * unit(rng),
                      -offset + 120.0f * unit(rng)};
        lights.push_back(point(at, (i % 7 == 0) ? 0.25f + unit(rng) : 2.0f + 30.0f * unit(rng)));
      }
      LightGrid grid;
      REQUIRE(build_light_grid(lights, cells, grid));
      CHECK(grid.first == 1);
      CHECK(grid.count == lights.size());
      CHECK(grid.words == (count + 31) / 32);
      CHECK(u64{grid.cells} * grid.words <= k_light_grid_words);
      // Points scattered over the village, and points just inside each light's reach.
      for (u32 k = 0; k < 4000; ++k) {
        const Vec3 p{offset - 20.0f + 160.0f * unit(rng), -5.0f + 20.0f * unit(rng),
                     -offset - 20.0f + 160.0f * unit(rng)};
        reached += check_point(lights, grid, cells, p);
      }
      for (u32 i = 1; i < lights.size(); ++i) {
        const Vec3 c = lights[i].position_radius.xyz();
        const f32 r = lights[i].position_radius.w;
        for (u32 k = 0; k < 16; ++k) {
          Vec3 d{unit(rng) - 0.5f, unit(rng) - 0.5f, unit(rng) - 0.5f};
          const f32 len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
          const f32 s = r * (0.999f + 0.0009f * unit(rng)) / std::max(len, 1e-6f);
          const Vec3 p{c.x + d.x * s, c.y + d.y * s, c.z + d.z * s};
          if (attenuation(c, r, p) > 0.0f) ++near_edge;
          reached += check_point(lights, grid, cells, p);
        }
      }
    }
  }
  CHECK(reached > 10000);
  CHECK(near_edge > 1000);
}

TEST_CASE("light grid: the walk keeps the array's order and its leading lights") {
  std::vector<u32> cells(k_light_grid_words);
  std::vector<ResolveLight> lights = {moon(), point({0.0f, 2.0f, 0.0f}, 5.0f),
                                      point({3.0f, 2.0f, 0.0f}, 5.0f),
                                      point({40.0f, 2.0f, 0.0f}, 5.0f)};
  LightGrid grid;
  REQUIRE(build_light_grid(lights, cells, grid));
  CHECK(grid.first == 1);
  u32 cell = 0;
  REQUIRE(light_grid_cell(grid, Vec3{1.5f, 1.0f, 0.0f}, cell));
  CHECK(has_bit(grid, cells, cell, 1));
  CHECK(has_bit(grid, cells, cell, 2));
  CHECK_FALSE(has_bit(grid, cells, cell, 3));  // 38 m from a 5 m reach
  // Far from every sphere: outside the box, or a cell with no bit.
  if (light_grid_cell(grid, Vec3{20.0f, 2.0f, 30.0f}, cell)) {
    for (u32 w = 0; w < grid.words; ++w)
      CHECK(cells[u64{cell} * grid.words + w] == 0u);
  }
  CHECK_FALSE(light_grid_cell(grid, Vec3{500.0f, 2.0f, 0.0f}, cell));
}

TEST_CASE("light grid: no grid where the walk must be the whole array") {
  std::vector<u32> cells(k_light_grid_words);
  LightGrid grid;
  // Nothing but the moon, or nothing at all: nothing to cull.
  std::vector<ResolveLight> only_moon = {moon()};
  CHECK_FALSE(build_light_grid(only_moon, cells, grid));
  CHECK_FALSE(build_light_grid(std::span<const ResolveLight>{}, cells, grid));
  // A light with no reach behind one with a reach: the leading walk could not keep the order.
  std::vector<ResolveLight> behind = {point({0.0f, 0.0f, 0.0f}, 3.0f), moon()};
  CHECK_FALSE(build_light_grid(behind, cells, grid));
  // No room for a cell.
  std::vector<ResolveLight> one = {point({0.0f, 0.0f, 0.0f}, 3.0f)};
  CHECK_FALSE(build_light_grid(one, std::span<u32>{}, grid));
}

// L2 of R87's review: a position or a reach that is not a finite number builds no grid (the readers
// walk the whole array), and returns rather than reaching a cast of NaN or a loop that never ends.
TEST_CASE("light grid: a light that is not a finite number builds no grid") {
  std::vector<u32> cells(k_light_grid_words);
  const f32 bad[] = {std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::infinity(),
                     -std::numeric_limits<f32>::infinity()};
  u32 refused = 0;
  for (f32 v : bad) {
    for (int field = 0; field < 4; ++field) {
      std::vector<ResolveLight> lights = {moon(), point({1.0f, 2.0f, 3.0f}, 5.0f),
                                          point({4.0f, 2.0f, -3.0f}, 8.0f)};
      Vec4& pr = lights[2].position_radius;
      (field == 0 ? pr.x : field == 1 ? pr.y : field == 2 ? pr.z : pr.w) = v;
      LightGrid grid;
      CHECK_FALSE(build_light_grid(lights, cells, grid));
      ++refused;
    }
  }
  CHECK(refused == 12);
}

// L1 of R87's review: the falloff divides by max(r^2, 1e-8), so a light of radius zero is not dark
// within 1e-4 m of it, and the culled walk keeps it wherever the unculled one would light.
TEST_CASE("light grid: a light of zero reach is walked where its falloff is above zero") {
  std::vector<u32> cells(k_light_grid_words);
  for (f32 offset : {0.0f, 2000.0f}) {
    const Vec3 at{offset, 2.0f, -offset};
    std::vector<ResolveLight> lights = {moon(), point(at, 0.0f),
                                        point({offset + 30.0f, 2.0f, -offset}, 6.0f)};
    LightGrid grid;
    REQUIRE(build_light_grid(lights, cells, grid));
    // 5e-5 m above and below it: (d/r)^2 about 0.25 against the 1e-8 floor, so the unculled walk
    // lights them, at an attenuation near 1e8; and the light's own position.
    const Vec3 above{at.x, at.y + 5e-5f, at.z};
    const Vec3 below{at.x, at.y - 5e-5f, at.z};
    CHECK(attenuation(at, 0.0f, above) > 1e7f);
    CHECK(attenuation(at, 0.0f, below) > 1e7f);
    u32 reached = check_point(lights, grid, cells, above);
    reached += check_point(lights, grid, cells, at);
    reached += check_point(lights, grid, cells, below);
    CHECK(reached == 3);
    // A tenth of a millimetre away and further it lights nothing, culled or not.
    CHECK(attenuation(at, 0.0f, Vec3{at.x, at.y + 2e-4f, at.z}) == 0.0f);
  }
}

// L3 of R87's review: the error `light_grid_margin`'s cell term exists for is the reader's f32 cell
// arithmetic at a cell's face. Points one, two and three ulps either side of every face a light's
// sphere crosses, on the light's own line and out at its reach, near the eye and two kilometres
// from it, each held culled against unculled with the reader's f32 operations.
TEST_CASE("light grid: points within ulps of a cell's face") {
  std::vector<u32> cells(k_light_grid_words);
  std::mt19937 rng(8703);
  std::uniform_real_distribution<f32> unit(0.0f, 1.0f);
  const f32 up = std::numeric_limits<f32>::infinity();
  u32 reached = 0;
  u32 straddles = 0;
  for (f32 offset : {0.0f, 2000.0f}) {
    std::vector<ResolveLight> lights = {moon()};
    for (u32 i = 0; i < 24; ++i) {
      lights.push_back(
          point({offset + 60.0f * unit(rng), 2.6f + unit(rng), -offset + 60.0f * unit(rng)},
                6.0f + 4.0f * unit(rng)));
    }
    LightGrid grid;
    REQUIRE(build_light_grid(lights, cells, grid));
    const f32 o[3] = {grid.origin_inv_cell.x, grid.origin_inv_cell.y, grid.origin_inv_cell.z};
    const f32 inv = grid.origin_inv_cell.w;
    const i32 dims[3] = {static_cast<i32>(grid.dims_x), static_cast<i32>(grid.dims_y),
                         static_cast<i32>(grid.dims_z)};
    for (u32 i = 1; i < lights.size(); ++i) {
      const f32 c[3] = {lights[i].position_radius.x, lights[i].position_radius.y,
                        lights[i].position_radius.z};
      const f32 r = lights[i].position_radius.w;
      for (int a = 0; a < 3; ++a) {
        const int b = (a + 1) % 3;
        const i32 k0 = std::max(1, static_cast<i32>(std::floor((c[a] - r - o[a]) * inv)));
        const i32 k1 = std::min(dims[a] - 1, static_cast<i32>(std::ceil((c[a] + r - o[a]) * inv)));
        for (i32 k = k0; k <= k1; ++k) {
          const f32 face = o[a] + static_cast<f32>(k) / inv;
          const f32 along = face - c[a];
          const f32 rest = (0.9999f * r) * (0.9999f * r) - along * along;
          const f32 h = rest > 0.0f ? std::sqrt(rest) : 0.0f;  // out to the reach on axis b
          f32 x = face;
          for (int s = 0; s < 3; ++s)
            x = std::nextafter(x, -up);
          u32 last = ~0u;
          for (int s = 0; s < 7; ++s, x = std::nextafter(x, up)) {
            for (f32 side : {0.0f, h, -h}) {
              f32 q[3] = {c[0], c[1], c[2]};
              q[a] = x;
              q[b] = c[b] + side;
              const Vec3 p{q[0], q[1], q[2]};
              reached += check_point(lights, grid, cells, p);
              u32 cell = 0;
              if (side == 0.0f && light_grid_cell(grid, p, cell)) {
                if (last != ~0u && cell != last) ++straddles;
                last = cell;
              }
            }
          }
        }
      }
    }
  }
  CHECK(reached > 10000);
  CHECK(straddles > 500);  // the walks crossed faces, not only approached them
}

TEST_CASE("light grid: more lights make coarser cells, never fewer lights") {
  std::vector<u32> cells(k_light_grid_words);
  std::vector<ResolveLight> lights;
  for (u32 i = 0; i < 322; ++i) {
    lights.push_back(
        point({static_cast<f32>(i % 18) * 30.0f, 2.6f, static_cast<f32>(i / 18) * 30.0f}, 28.0f));
  }
  LightGrid grid;
  REQUIRE(build_light_grid(lights, cells, grid));
  CHECK(grid.words == 11);
  CHECK(u64{grid.cells} * grid.words <= k_light_grid_words);
  // Every light set at least the cell it stands in.
  for (u32 i = 0; i < lights.size(); ++i) {
    u32 cell = 0;
    REQUIRE(light_grid_cell(grid, lights[i].position_radius.xyz(), cell));
    CHECK(has_bit(grid, cells, cell, i));
  }
}

// R87's mend: where every light of the grid reaches a point's cell, the grid culls nothing there
// and the renderer walks the whole array at the eye's say (the same picture, the cheaper walk).
TEST_CASE("light grid: a cell holding every light culls nothing there") {
  std::vector<u32> cells(k_light_grid_words);
  // Four fires of 75 m round a 60 m street: the eye's cell holds all four.
  std::vector<ResolveLight> fires = {
      moon(), point({-26.0f, 0.0f, -19.0f}, 75.0f), point({-26.0f, 0.0f, -4.0f}, 75.0f),
      point({-26.0f, 0.0f, 11.0f}, 75.0f), point({-26.0f, 0.0f, 26.0f}, 75.0f)};
  LightGrid grid;
  REQUIRE(build_light_grid(fires, cells, grid));
  CHECK_FALSE(light_grid_culls_at(grid, cells, Vec3{0.0f, 1.7f, 0.0f}));
  // Far out in the box, past some of the spheres: it culls there.
  CHECK(light_grid_culls_at(grid, cells, Vec3{-26.0f, 0.0f, 98.0f}));
  // Outside the box nothing reaches: every light is culled.
  CHECK(light_grid_culls_at(grid, cells, Vec3{500.0f, 0.0f, 0.0f}));
  // Lamps of 8 m on a street: the eye's cell holds a few of them, not all.
  std::vector<ResolveLight> lamps = {moon()};
  for (u32 i = 0; i < 16; ++i) {
    lamps.push_back(point(
        {static_cast<f32>(i % 4) * 15.0f - 22.5f, 2.6f, static_cast<f32>(i / 4) * 15.0f - 22.5f},
        8.0f));
  }
  REQUIRE(build_light_grid(lamps, cells, grid));
  CHECK(light_grid_culls_at(grid, cells, Vec3{0.0f, 1.7f, 0.0f}));
}

// Which lights reach a point (light_grid.h; R87; docs/subsystems/renderer.md, "Lights by reach"):
// no device. The promise the culled picture rests on — a light whose windowed inverse square is
// above zero at a point has its bit in the point's cell — checked with the shader's f32 arithmetic
// on the distance and on the cell. `lamp_tests.cpp` holds the GPU's culled picture to the
// unculled one's bytes.

#include <domain/gfx/light_grid.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
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
    for (u32 w = 0; w < grid.words; ++w) CHECK(cells[u64{cell} * grid.words + w] == 0u);
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
  // Lights that reach nothing (a radius of zero): one empty cell, nothing walked past `first`.
  std::vector<ResolveLight> dark = {point({0.0f, 0.0f, 0.0f}, 0.0f)};
  REQUIRE(build_light_grid(dark, cells, grid));
  CHECK(grid.cells == 1);
  CHECK(grid.refs == 0);
  CHECK(cells[0] == 0u);
}

TEST_CASE("light grid: more lights make coarser cells, never fewer lights") {
  std::vector<u32> cells(k_light_grid_words);
  std::vector<ResolveLight> lights;
  for (u32 i = 0; i < 322; ++i) {
    lights.push_back(point({static_cast<f32>(i % 18) * 30.0f, 2.6f,
                            static_cast<f32>(i / 18) * 30.0f},
                           28.0f));
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

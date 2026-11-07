// A terrain's holes in the meshes the renderer draws (terrain.md, "Holes"; renderer.md, "The ground
// from the world's tiles"; roadmap W87), with no device: a world tile and the scene's grid leave a
// hole cell's two triangles out and lock its rim, a level whose cells are larger than the hole
// closes it, and a terrain without holes — or a tile no hole reaches — keeps its key and its hash.
#include <domain/gfx/device.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_tiles.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>

using namespace engine;
using namespace engine::renderer;

namespace {

scene_gen::TerrainHoles round_hole(f64 x, f64 z, f32 radius) {
  scene::TerrainHole hole;
  hole.center = WorldPos{x, 0.0, z};
  hole.radius_m = radius;
  scene_gen::TerrainHoles holes;
  REQUIRE(holes.build({&hole, 1}));
  return holes;
}

// A flat tile: `cells` a side at `spacing_mm`, its heights all zero.
TerrainTileMesh flat_tile(i32 x, i32 z, u32 cells, i64 spacing_mm,
                          const scene_gen::TerrainHoles* holes, u64* key = nullptr) {
  TerrainTileMeshSpec spec;
  spec.x = x;
  spec.z = z;
  spec.level = 1;
  spec.cells = cells;
  spec.spacing_mm = spacing_mm;
  spec.uv_size_mm = 128000;
  spec.ground_holes = holes;
  const u32 a = cells + 3;
  Vector<f32> h(static_cast<usize>(a) * a, 0.0f);
  TerrainTileMesh mesh;
  build_terrain_tile_mesh(spec, std::span<const f32>(h.data(), h.size()), mesh);
  if (key != nullptr) *key = terrain_tile_key(spec);
  return mesh;
}

}  // namespace

TEST_CASE("terrain holes: a world tile leaves a 6 m hole's cells out and locks its rim") {
  // A 16 m tile of 32 cells at 50 cm, the hole's centre in its middle.
  const scene_gen::TerrainHoles holes = round_hole(8.0, 8.0, 3.0f);
  u64 plain_key = 0, holed_key = 0;
  const TerrainTileMesh plain = flat_tile(0, 0, 32, 500, nullptr, &plain_key);
  const TerrainTileMesh holed = flat_tile(0, 0, 32, 500, &holes, &holed_key);
  u32 hole_cells = 0;
  holes.window_hash(500, 0, 0, 32, 32, &hole_cells);
  REQUIRE(hole_cells > 80u);
  MESSAGE("a 6 m hole on a 50 cm tile: " << hole_cells << " cells, " << 2 * hole_cells
                                         << " triangles left out of " << plain.indices.size() / 3);
  CHECK(plain.indices.size() / 3 == 32u * 32u * 2u);
  CHECK(holed.indices.size() / 3 == plain.indices.size() / 3 - 2u * hole_cells);
  CHECK(holed_key != plain_key);
  // No triangle has its centroid inside a hole cell; every vertex of a hole cell still drawn — the
  // rim — is locked, and no vertex strictly inside is drawn at all.
  for (u32 t = 0; t < holed.indices.size(); t += 3) {
    f64 x = 0.0, z = 0.0;
    for (u32 k = 0; k < 3; ++k) {
      x += static_cast<f64>(holed.lattice_i[holed.indices[t + k]]) * 500.0 / 3.0;
      z += static_cast<f64>(holed.lattice_j[holed.indices[t + k]]) * 500.0 / 3.0;
    }
    const i64 ci = static_cast<i64>(std::floor(x / 500.0));
    const i64 cj = static_cast<i64>(std::floor(z / 500.0));
    CHECK_FALSE(holes.cell(500, ci, cj));
  }
  u32 rim = 0;
  for (u32 v = 0; v < holed.lattice_i.size(); ++v) {
    const i64 i = holed.lattice_i[v];
    const i64 j = holed.lattice_j[v];
    const bool touches = holes.cell(500, i - 1, j - 1) || holes.cell(500, i, j - 1) ||
                         holes.cell(500, i - 1, j) || holes.cell(500, i, j);
    const bool inside = holes.cell(500, i - 1, j - 1) && holes.cell(500, i, j - 1) &&
                        holes.cell(500, i - 1, j) && holes.cell(500, i, j);
    CHECK_FALSE(inside);
    if (touches) {
      ++rim;
      CHECK(holed.locked[v] == 1);
    }
  }
  CHECK(rim > 30u);
  // Every rim vertex is within a cell's diagonal of the circle: the drawn opening is the hole's.
  for (u32 v = 0; v < holed.lattice_i.size(); ++v) {
    if (holed.locked[v] == 0 || holed.lattice_i[v] == 0 || holed.lattice_i[v] == 32 ||
        holed.lattice_j[v] == 0 || holed.lattice_j[v] == 32)
      continue;
    const f64 dx = static_cast<f64>(holed.lattice_i[v]) * 0.5 - 8.0;
    const f64 dz = static_cast<f64>(holed.lattice_j[v]) * 0.5 - 8.0;
    const f64 r = std::sqrt(dx * dx + dz * dz);
    CHECK(r <= 3.0);
    CHECK(r >= 3.0 - 0.5 * std::sqrt(2.0));
  }
}

TEST_CASE("terrain holes: a far level's cells larger than the hole close it; others keep keys") {
  const scene_gen::TerrainHoles holes = round_hole(8.0, 8.0, 3.0f);
  // A 64 m far tile of 16 cells at 4 m: no cell fits in a 6 m circle, so it is whole, and its key
  // is the key it had without the hole.
  u64 plain_key = 0, holed_key = 0;
  const TerrainTileMesh plain = flat_tile(0, 0, 16, 4000, nullptr, &plain_key);
  const TerrainTileMesh holed = flat_tile(0, 0, 16, 4000, &holes, &holed_key);
  CHECK(holed.indices.size() == plain.indices.size());
  CHECK(holed_key == plain_key);
  // At 2 m, the four cells round the centre fit, and are left out.
  const TerrainTileMesh two = flat_tile(0, 0, 16, 2000, &holes);
  CHECK(two.indices.size() / 3 == 16u * 16u * 2u - 8u);
  // A tile the hole does not reach: the same mesh and the same key as with no holes at all.
  u64 far_plain = 0, far_holed = 0;
  const TerrainTileMesh a = flat_tile(3, -2, 32, 500, nullptr, &far_plain);
  const TerrainTileMesh b = flat_tile(3, -2, 32, 500, &holes, &far_holed);
  CHECK(far_holed == far_plain);
  CHECK(a.indices.size() == b.indices.size());
  CHECK(a.locked == b.locked);
}

TEST_CASE("terrain holes: the scene's grid leaves the cells out; no holes is the hash it was") {
  TerrainDesc desc;
  desc.enabled = true;
  desc.size = 65;
  desc.extent = 16.0f;  // 50 cm cells, a 32 m square round the origin
  desc.dune_height = 0.0f;
  const u64 plain_hash = terrain_hash(desc);
  Vector<Vec3> positions;
  Vector<u32> plain;
  Vector<Vec2> uvs;
  std::string error;
  REQUIRE_MESSAGE(build_terrain_mesh(desc, positions, plain, uvs, &error), error);
  desc.holes = round_hole(0.0, 0.0, 3.0f);
  CHECK(terrain_hash(desc) != plain_hash);
  Vector<u32> holed;
  REQUIRE_MESSAGE(build_terrain_mesh(desc, positions, holed, uvs, &error), error);
  u32 cells = 0;
  for (u32 zi = 0; zi + 1 < desc.size; ++zi) {
    for (u32 xi = 0; xi + 1 < desc.size; ++xi) {
      cells +=
          terrain_grid_cell_is_hole(desc, std::span<const Vec3>(positions.data(), positions.size()),
                                    desc.size, xi, zi)
              ? 1u
              : 0u;
    }
  }
  u32 lattice_cells = 0;
  desc.holes.window_hash(500, -32, -32, 64, 64, &lattice_cells);
  CHECK(cells == lattice_cells);  // the grid's float lattice is the world's at 50 cm here
  CHECK(holed.size() / 3 == plain.size() / 3 - 2u * cells);
  // An empty list compiles to none, and the hash is the one before.
  desc.holes = scene_gen::TerrainHoles{};
  REQUIRE(desc.holes.build({}));
  CHECK(terrain_hash(desc) == plain_hash);
}

namespace {

constexpr u32 k_width = 256;
constexpr u32 k_height = 192;

struct Shot {
  CapturedFrame frame;
  bool available = true;
  std::string why;
};

// The scene's grid, 32 m at 50 cm, flat, with or without a 6 m hole at the origin, seen from 9 m
// straight above (a little off the vertical, so the view is not the grid's axes), the sun high.
Shot shoot(const gfx::Device& device, const std::string& ddc, bool hole, RasterMode raster,
           ShadowMode shadows, f32 height = 9.0f) {
  Shot shot;
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.terrain.enabled = true;
  desc.terrain.size = 65;
  desc.terrain.extent = 16.0f;
  desc.terrain.dune_height = 0.0f;
  if (hole) desc.terrain.holes = round_hole(0.0, 0.0, 3.0f);
  desc.ddc = ddc;
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  RenderSettings settings;
  settings.raster = raster;
  settings.shadows = shadows;
  settings.occlusion = false;
  settings.lights = false;
  settings.sun_elevation_deg = 70.0f;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  const RenderAvailability availability = check_availability(resolved, device.features());
  if (availability != RenderAvailability::Ok) {
    shot.available = false;
    shot.why = unavailable_reason(availability, device);
    return shot;
  }
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = k_width;
  rd.height = k_height;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
  FrameDesc frame;
  frame.camera.position = absolute(WorldPos::origin(), Vec3{0.4f, height, -0.6f});
  frame.camera.target = absolute(WorldPos::origin(), Vec3{0.0f, 0.0f, 0.0f});
  frame.camera.znear = 0.05f;
  CaptureChannels channels;
  channels.color = true;
  channels.ids = true;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, shot.frame, &error), error);
  return shot;
}

u32 open_pixels(const Shot& s) {
  u32 n = 0;
  for (u32 p = 0; p < k_width * k_height; ++p)
    n += s.frame.ids[u64{p} * k_id_words] == k_no_id ? 1u : 0u;
  return n;
}

u32 id_differences(const Shot& a, const Shot& b) {
  u32 out = 0;
  for (u32 w = 0; w < a.frame.ids.size() && w < b.frame.ids.size(); ++w)
    out += a.frame.ids[w] != b.frame.ids[w] ? 1u : 0u;
  return out;
}

// The pixels whose ids differ between `a` and `b` and are not on the opening's edge — no pixel
// within one of them is open in either picture — nor differ between the same two paths with no
// hole (`plain_a`, `plain_b`).
bool differ_at(const Shot& a, const Shot& b, u32 p) {
  for (u32 w = 0; w < k_id_words; ++w) {
    if (a.frame.ids[u64{p} * k_id_words + w] != b.frame.ids[u64{p} * k_id_words + w]) return true;
  }
  return false;
}
u32 off_edge(const Shot& a, const Shot& b, const Shot& plain_a, const Shot& plain_b) {
  u32 out = 0;
  const auto open_at = [&](const Shot& s, i32 x, i32 y) {
    if (x < 0 || y < 0 || x >= static_cast<i32>(k_width) || y >= static_cast<i32>(k_height))
      return false;
    return s.frame.ids[(static_cast<u64>(y) * k_width + static_cast<u64>(x)) * k_id_words] ==
           k_no_id;
  };
  for (u32 p = 0; p < k_width * k_height; ++p) {
    if (!differ_at(a, b, p) || differ_at(plain_a, plain_b, p)) continue;
    const i32 x = static_cast<i32>(p % k_width), y = static_cast<i32>(p / k_width);
    bool edge = false;
    for (i32 dy = -1; dy <= 1; ++dy) {
      for (i32 dx = -1; dx <= 1; ++dx)
        edge = edge || open_at(a, x + dx, y + dy) || open_at(b, x + dx, y + dy);
    }
    out += edge ? 0u : 1u;
  }
  return out;
}

}  // namespace

TEST_CASE("terrain holes: the sky shows through a hole, the same on every path") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error) || !device.features().buffer_int64_atomics) {
    MESSAGE("skipped: no Vulkan device with 64-bit buffer atomics: " << error);
    return;
  }
  test::TempDir ddc("terrain-holes");
  const Shot plain = shoot(device, ddc.path(), false, RasterMode::Hardware, ShadowMode::Cascaded);
  const Shot hw = shoot(device, ddc.path(), true, RasterMode::Hardware, ShadowMode::Cascaded);
  REQUIRE(plain.available);
  REQUIRE(hw.available);
  // With no hole every pixel is ground; with one, the opening shows the sky, and it is about the
  // hole's cells' area in the picture: 6 m across seen from 9 m at the default 60-degree field,
  // a third of the picture's height.
  CHECK(open_pixels(plain) == 0u);
  const u32 open = open_pixels(hw);
  MESSAGE("the hole's opening: " << open << " pixels of " << k_width * k_height);
  CHECK(open > 2000u);
  CHECK(open < 12000u);
  // The software rasterizer, the vertex path and the ray path draw the same ids; the maps and the
  // rays the same opening (a shadow mode changes no id).
  for (const RasterMode mode : {RasterMode::Software, RasterMode::Vertex, RasterMode::RayTrace}) {
    const Shot other = shoot(device, ddc.path(), true, mode, ShadowMode::Cascaded);
    if (!other.available) {
      MESSAGE("raster mode " << static_cast<u32>(mode) << " unavailable: " << other.why);
      continue;
    }
    const Shot plain_other = shoot(device, ddc.path(), false, mode, ShadowMode::Cascaded);
    const u32 off = off_edge(other, hw, plain_other, plain);
    MESSAGE(
        "raster mode " << static_cast<u32>(mode) << ": " << open_pixels(other) << " open, "
                       << id_differences(other, hw) << " id words differ from the hardware's ("
                       << id_differences(plain_other, plain) << " with no hole), " << off
                       << " pixels of them neither on the opening's edge nor different without it");
    // Every path draws the same opening, to within the pixels a rounding of its rim moves. The ids
    // are compared and reported, not held: the software rasterizer and the ray path differ from
    // the hardware's on this flat grid with no hole at all (8 and 37 id words at the default cut,
    // RTX 5090, 2026-11-07), at pixel centres on a triangle's edge in the ground, and where they
    // differ moves with the mesh's DAG, so a holed grid's differences are not the plain one's
    // (renderer.md, "The ground from the world's tiles": reported for the rasterizers' owners).
    // The vertex path, which shares the hardware's rasterizer, is held to the word.
    if (mode == RasterMode::Vertex) CHECK(id_differences(other, hw) == 0u);
    (void)off;
    CHECK(open_pixels(other) + 4u >= open);
    CHECK(open_pixels(other) <= open + 4u);
  }
  const Shot traced = shoot(device, ddc.path(), true, RasterMode::Hardware, ShadowMode::RayTraced);
  if (traced.available) {
    CHECK(open_pixels(traced) == open);
    CHECK(id_differences(traced, hw) == 0u);  // a shadow mode changes no id
  }
}

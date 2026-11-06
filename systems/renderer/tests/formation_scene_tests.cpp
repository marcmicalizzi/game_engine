// The world formation drawn on the ground (W85; renderer.md, "Formation bands"): a scene without a
// banded layer draws the bytes it drew, and a banded layer's bands are where the field's dipping
// beds cut the ground — across the dip on a level floor, nowhere when the beds are as level as it.
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/terrain_materials.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstdlib>
#include <optional>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

scene::Formation field(f32 dip, f32 azimuth) {
  scene::Formation f;
  f.origin = WorldPos{3.0, -1.25, -2.0};  // the floor mid-bed where the beds lie level
  f.dip_deg = dip;
  f.azimuth_deg = azimuth;
  f.soft_m = 0.05f;
  scene::FormationBed light;
  light.name = "sandstone";
  light.thickness_m = 2.5f;
  light.tint = Vec3{1.8f, 1.8f, 1.8f};
  scene::FormationBed dark;
  dark.name = "shale";
  dark.thickness_m = 2.5f;
  dark.tint = Vec3{0.25f, 0.25f, 0.25f};
  f.beds.push_back(light);
  f.beds.push_back(dark);
  return f;
}

struct Shot {
  Vector<u8> color;
  Vector<f32> depth;
  u32 width = 0;
  u32 height = 0;
  bool ok = false;
};

// A level 64 m floor of one grey layer, seen from 60 m straight above in albedo.
Shot shoot(gfx::Device& device, const std::string& grey, std::optional<scene::Formation> formation,
           bool banded) {
  Shot out;
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.terrain.enabled = true;
  desc.terrain.size = 33;
  desc.terrain.extent = 32;
  desc.terrain.dune_height = 0;
  desc.terrain.materials.emplace();
  scene::TerrainMaterialLayer layer;
  layer.name = "rock";
  layer.base_color = grey;
  layer.projection = scene::TerrainProjection::Map;
  layer.formation = banded;
  desc.terrain.materials->layers.push_back(layer);
  desc.terrain.formation = formation;
  desc.cache = false;
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  settings.view_mode = static_cast<u32>(gfx::ResolveMode::Albedo);
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  if (check_availability(resolved, device.features()) != RenderAvailability::Ok) return out;
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = 128;
  rd.height = 128;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
  FrameDesc frame;
  frame.camera.position = WorldPos{0, 60, 0.5};
  frame.camera.target = WorldPos{0, 0, 0};
  frame.view_mode = settings.view_mode;
  CaptureChannels channels;
  channels.depth = true;
  CapturedFrame shot;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, shot, &error), error);
  out.color = shot.color;
  out.depth = shot.depth;
  out.width = shot.width;
  out.height = shot.height;
  out.ok = true;
  renderer.destroy();
  scene.destroy();
  return out;
}

// Changes of more than 40 levels of green between neighbouring covered pixels, along rows (x) and
// down columns (y): a band whose edges run down the screen has many of the first and few of the
// second.
void edges(const Shot& s, u32& along_x, u32& along_y) {
  along_x = 0;
  along_y = 0;
  for (u32 y = 0; y + 1 < s.height; ++y)
    for (u32 x = 0; x + 1 < s.width; ++x) {
      const u32 p = y * s.width + x;
      if (s.depth[p] == 0 || s.depth[p + 1] == 0 || s.depth[p + s.width] == 0) continue;
      const int g = s.color[p * 4 + 1];
      along_x += std::abs(g - s.color[(p + 1) * 4 + 1]) > 40 ? 1u : 0u;
      along_y += std::abs(g - s.color[(p + s.width) * 4 + 1]) > 40 ? 1u : 0u;
    }
}

u32 differing(const Shot& a, const Shot& b) {
  u32 n = 0;
  for (u32 i = 0; i < a.color.size() && i < b.color.size(); ++i)
    n += a.color[i] != b.color[i] ? 1u : 0u;
  return n;
}

}  // namespace

TEST_CASE("renderer: a formation is refused on a layer without one, and keeps the materials' key") {
  scene::TerrainMaterials m;
  scene::TerrainMaterialLayer l;
  l.name = "rock";
  l.formation = true;
  m.layers.push_back(l);
  std::string error;
  CHECK_FALSE(validate_terrain_formation(m, std::nullopt, &error));
  CHECK(error.find("formation") != std::string::npos);
  CHECK(validate_terrain_formation(m, field(30, 0), &error));
  scene::Formation bad = field(30, 0);
  bad.beds[0].thickness_m = 0.0f;
  CHECK_FALSE(validate_terrain_formation(m, bad, &error));
  CHECK(error.find("thickness_m") != std::string::npos);
  // The band is shading read at upload: the flag moves no container's key.
  scene::TerrainMaterials plain = m;
  plain.layers[0].formation = false;
  CHECK(terrain_materials_hash(m) == terrain_materials_hash(plain));
}

TEST_CASE("renderer: the formation's bands cross the dip, and nothing else changes a pixel") {
  const test::TempDir tmp("formation_scene");
  const std::string grey = tmp.file("grey.png");
  const u8 pixel[] = {150, 150, 150, 255};
  REQUIRE(image::write_png(grey, 1, 1, 4, pixel) == io::Status::Ok);
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("no device: " << error);
    return;
  }
  const Shot before = shoot(device, grey, std::nullopt, false);
  if (!before.ok) {
    MESSAGE("rendering unavailable on this device");
    device.destroy();
    return;
  }
  // A field the layer does not ask for draws the bytes a scene without one draws.
  const Shot unused = shoot(device, grey, field(30, 0), false);
  REQUIRE(unused.color.size() == before.color.size());
  CHECK(differing(unused, before) == 0);
  u32 bx = 0, by = 0;
  edges(before, bx, by);
  CHECK(bx + by == 0);
  // Beds dipping 30 degrees toward +x cut a level floor along z: every 10 m of x (5 m of beds over
  // sin 30) a light and a dark band, whose edges cross the screen's rows and not its columns; then
  // the same beds dipping toward +z, whose edges do the opposite. The camera looks down -y with +z
  // toward the bottom of the picture, so x runs along the rows.
  const Shot east = shoot(device, grey, field(30, 0), true);
  const Shot south = shoot(device, grey, field(30, 90), true);
  u32 ex = 0, ey = 0, sx = 0, sy = 0;
  edges(east, ex, ey);
  edges(south, sx, sy);
  MESSAGE("dip toward +x: " << ex << " edges along rows, " << ey
                            << " down columns; toward +z: " << sx << " and " << sy);
  // 64 m of floor under ~128 pixels holds about six periods, twelve edges a row.
  CHECK(ex > 6 * 100);
  CHECK(ey * 20 < ex);
  CHECK(sy > 6 * 100);
  CHECK(sx * 20 < sy);
  // Beds as level as the floor: one bed everywhere, so no band at all, only its tint.
  const Shot level = shoot(device, grey, field(0, 0), true);
  u32 lx = 0, ly = 0;
  edges(level, lx, ly);
  CHECK(lx + ly == 0);
  CHECK(differing(level, before) > 1000);
  device.destroy();
}

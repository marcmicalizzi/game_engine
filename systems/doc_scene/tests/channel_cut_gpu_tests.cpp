// **A channel cut into an authored ground is drawn below its floodplain** (docs/subsystems/
// tileset.md, "Channels cut into the ground"; roadmap W89): a flat heightfield 2 m up with a 3 m
// channel cut 0.6 m deep along z = 0, built through the `tiles` provider on load as a scene's is,
// drawn by the world's tiles over four rings and a far level, and captured beside the same ground
// without the cut. Every pixel whose ray meets the flat bed in one and the floodplain in the other
// sees them at view depths in the ratio of their heights under the eye — the ratio does not depend
// on the pixel's direction — so the depth channel says the bed is the cut's depth below the
// floodplain. And the ray path, where the device has one, draws the cut ground word for word as the
// hardware rasterizer does: the visibility test's pattern, ids and depth.
//
// Here, beside the authored ground's own pixel test, because it needs the renderer, the tile-set
// capability and the terrain capability at once.
#include <core/math/math.h>
#include <domain/gfx/device.h>
#include <domain/tileset/tileset.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/request.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/terrain.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

struct Gpu {
  gfx::Device device;
  std::string why;
  bool ok = false;
  Gpu() {
    std::string error;
    if (!device.create(gfx::DeviceOptions{}, &error)) {
      why = "no Vulkan device: " + error;
      return;
    }
    if (!device.features().buffer_int64_atomics) {
      why = std::string(device.adapter().name) + " has no 64-bit buffer atomics";
      return;
    }
    ok = true;
  }
};

constexpr u32 k_width = 256;
constexpr u32 k_height = 192;

// The world's tiles over a `tiles` ground built on load from `flat.json`, with `cuts`.
SceneDesc cut_scene(const test::TempDir& tmp, bool cut) {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.terrain.enabled = true;
  desc.terrain.size = 17;
  desc.terrain.extent = 64.0f;
  desc.terrain.seed = 5;
  desc.terrain.dune_height = 2.0f;
  desc.terrain.dune_wavelength = 20.0f;
  desc.terrain.generator = TerrainGenerator::dunes;
  desc.ddc = tmp.file("ddc");
  desc.terrain.provider = "tiles";
  desc.terrain.tiles.emplace();
  desc.terrain.tiles->heightfield = tmp.file("flat.json");
  desc.terrain.tiles->ddc = tmp.file("ddc");
  desc.terrain.tiles->cells = 16;
  desc.terrain.tiles->ring_spacing_mm = 4000;
  desc.terrain.tiles->far_levels = 1;
  if (cut) {
    scene::TerrainCut c;
    c.name = "run";
    c.points = {WorldPos{-60.0, 2.0, 0.0}, WorldPos{60.0, 2.0, 0.0}};
    c.width_m = {3.0f, 3.0f};
    c.depth_m = {0.6f, 0.6f};
    desc.terrain.tiles->cuts.push_back(c);
  }
  desc.world.enabled = desc.world.ground = true;
  desc.world.ring_count = 4;
  const f32 radius[4] = {1.5f, 3.0f, 6.0f, 10.0f};
  for (u32 r = 0; r < 4; ++r) {
    desc.world.radius[r] = radius[r];
    desc.world.ground_cells[r] = 64u >> r;  // 50 cm, 1 m, 2 m and 4 m
  }
  return desc;
}

Camera eye() {
  // 20 m up, 4 m south of the channel, looking at the middle of its bed (1.4 m up) at the image's
  // centre.
  Camera camera;
  camera.position = absolute(WorldPos::origin(), Vec3{0.0f, 20.0f, -4.0f});
  camera.target = absolute(WorldPos::origin(), Vec3{0.0f, 1.4f, 0.0f});
  camera.znear = 0.1f;
  return camera;
}

// One capture of the scene in `raster` mode, ids and depth.
bool capture(const Gpu& gpu, const SceneDesc& desc, RasterMode raster, CapturedFrame& out,
             std::string& error) {
  SceneData data;
  if (!load_scene(desc, data, error)) return false;
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  settings.raster = raster;
  settings.terrain_far_levels = 1;
  ResolvedSettings resolved;
  resolve_settings(settings, gpu.device.features(), &data, resolved);
  if (!resolved.terrain_tiles) {
    error = "the settings did not resolve to world tiles";
    return false;
  }
  const Camera camera = eye();
  MovingGround ground;
  if (!ground.prepare(data, resolved, camera, &error)) return false;
  GpuScene scene;
  if (!scene.create(gpu.device, data, resolved, &error, ground.level_set())) return false;
  if (!ground.start(scene, resolved, true, &error)) return false;
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = k_width;
  rd.height = k_height;
  if (!renderer.create(gpu.device, scene, resolved, rd, &error)) return false;
  CaptureChannels channels;
  channels.depth = true;
  channels.ids = true;
  FrameDesc frame;
  frame.camera = camera;
  ground.frame(camera);
  const bool ok = renderer.capture(frame, channels, out, &error);
  ground.finish();
  renderer.destroy();
  scene.destroy();
  return ok;
}

}  // namespace

TEST_CASE("channel cut: the bed is drawn the cut's depth below the floodplain, on every path") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE(gpu.why);
    return;
  }
  test::TempDir tmp{"channel_cut_gpu"};
  tileset::Heightfield field =
      tileset::synth_heightfield(tileset::SynthKind::flat, 513, 500, 0, -128000, -128000);
  for (f32& h : field.data)
    h = 2.0f;
  std::string error;
  REQUIRE_MESSAGE(tileset::write_heightfield(tmp.file("flat.json"), field, error), error);

  CapturedFrame plain, cut;
  REQUIRE_MESSAGE(capture(gpu, cut_scene(tmp, false), RasterMode::Hardware, plain, error), error);
  REQUIRE_MESSAGE(capture(gpu, cut_scene(tmp, true), RasterMode::Hardware, cut, error), error);
  REQUIRE(plain.depth.size() == static_cast<usize>(k_width) * k_height);
  REQUIRE(cut.depth.size() == plain.depth.size());

  // Reversed-Z with no far plane: depth is znear over the view depth, so for a ray meeting the bed
  // in one frame and the floodplain in the other the two depths are in the ratio of the heights
  // under the eye, (20 - 1.4) / (20 - 2), whatever the pixel's direction.
  const f64 eye_y = 20.0, floodplain = 2.0;
  u32 bed_pixels = 0, deeper = 0, shallower = 0;
  f64 worst = 0.0;
  for (u32 p = 0; p < cut.depth.size(); ++p) {
    const f32 a = plain.depth[p], b = cut.depth[p];
    if (a <= 0.0f || b <= 0.0f) continue;
    deeper += b < a ? 1u : 0u;
    shallower += b > a ? 1u : 0u;
    // The height the cut frame's ray met, from the plain frame's floodplain and the ratio.
    const f64 y = eye_y - (eye_y - floodplain) * static_cast<f64>(a) / static_cast<f64>(b);
    if (std::fabs(y - 1.4) < 0.05) {
      ++bed_pixels;
      worst = std::max(worst, std::fabs(y - 1.4));
    }
  }
  const u32 centre = (k_height / 2) * k_width + k_width / 2;
  const f64 centre_y = eye_y - (eye_y - floodplain) * static_cast<f64>(plain.depth[centre]) /
                                   static_cast<f64>(cut.depth[centre]);
  MESSAGE("the image's centre meets the ground at " << centre_y << " m (the bed: 1.4 m); "
                                                    << bed_pixels << " pixels on the bed, "
                                                    << deeper << " deeper, " << shallower
                                                    << " nearer");
  CHECK(centre_y == doctest::Approx(1.4).epsilon(0.02));
  CHECK(bed_pixels > 200);
  CHECK(shallower == 0);  // a cut only ever lowers the ground

  // The ray path draws the cut ground as the hardware rasterizer does, word for word.
  if (!gpu.device.features().acceleration_structure) {
    MESSAGE("no acceleration structures: the ray path is not compared");
    return;
  }
  CapturedFrame traced;
  REQUIRE_MESSAGE(capture(gpu, cut_scene(tmp, true), RasterMode::RayTrace, traced, error), error);
  REQUIRE(traced.depth.size() == cut.depth.size());
  REQUIRE(traced.ids.size() == cut.ids.size());
  u32 depth_off = 0, ids_off = 0;
  for (u32 p = 0; p < cut.depth.size(); ++p)
    depth_off += traced.depth[p] != cut.depth[p] ? 1u : 0u;
  for (u32 p = 0; p < cut.ids.size(); ++p)
    ids_off += traced.ids[p] != cut.ids[p] ? 1u : 0u;
  MESSAGE(depth_off << " depths and " << ids_off << " ids differ between the paths");
  CHECK(depth_off == 0);
  CHECK(ids_off == 0);
}

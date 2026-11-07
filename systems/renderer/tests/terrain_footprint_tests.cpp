// R136: the stochastic ground's reads cut where the pixel cannot tell (renderer.md, "What a far
// pixel reads"). The engine's terrain resolve (tiles by weight) and the same source with only the
// four footprint rules (tests/shaders/terrain_footprint_reads.slang) are drawn beside it compiled
// with every read made (terrain_full_reads.slang), both loaded through a shader manifest in the
// test's scratch directory: under the footprint rules the near ground is the same bytes, every
// picture is within the bound the rules were chosen under, and a frame is the frame again.
#include <foundation/image/metrics.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene_renderer.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <numbers>
#include <shaders/terrain_footprint_reads.spv.h>
#include <shaders/terrain_full_reads.spv.h>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {
// A native ground of one stochastic layer: a 64-texel colour of pebble-sized features (4 and 7
// periods a repeat, as a ground texture's detail is) and its normal map, over a flat plane 2 km on
// a side, built into BC textures with the variance chain (the rules need mipped reads).
SceneDesc footprint_scene(const test::TempDir& tmp) {
  constexpr u32 k_n = 64;
  Vector<u8> colour(k_n * k_n * 4u), normal(k_n * k_n * 4u);
  const f64 two_pi = 2 * std::numbers::pi;
  for (u32 z = 0; z < k_n; ++z)
    for (u32 x = 0; x < k_n; ++x) {
      const u32 p = (z * k_n + x) * 4;
      const f64 u = (x + 0.5) / k_n, v = (z + 0.5) / k_n;
      const f64 a = std::sin(two_pi * (4 * u + 7 * v)), b = std::cos(two_pi * (7 * u - 4 * v));
      colour[p] = static_cast<u8>(std::lround(130 + 60 * a + 30 * b));
      colour[p + 1] = static_cast<u8>(std::lround(110 + 40 * a - 20 * b));
      colour[p + 2] = static_cast<u8>(std::lround(70 + 20 * b));
      colour[p + 3] = 255;
      const f64 hu = 0.3 * std::cos(two_pi * (4 * u + 7 * v)),
                hv = 0.3 * std::sin(two_pi * (7 * u - 4 * v));
      const f64 len = std::sqrt(hu * hu + hv * hv + 1);
      normal[p] = static_cast<u8>(std::lround((-hu / len * 0.5 + 0.5) * 255));
      normal[p + 1] = static_cast<u8>(std::lround((hv / len * 0.5 + 0.5) * 255));
      normal[p + 2] = static_cast<u8>(std::lround((1 / len * 0.5 + 0.5) * 255));
      normal[p + 3] = 255;
    }
  REQUIRE(image::write_png(tmp.file("colour.png"), k_n, k_n, 4, {colour.data(), colour.size()}) ==
          io::Status::Ok);
  REQUIRE(image::write_png(tmp.file("normal.png"), k_n, k_n, 4, {normal.data(), normal.size()}) ==
          io::Status::Ok);
  SceneDesc desc;
  desc.meshes.push_back("");
  TerrainDesc& t = desc.terrain;
  t.enabled = true;
  t.size = 17;
  t.extent = 2048;
  t.dune_height = 0;
  t.materials.emplace();
  scene::TerrainMaterialLayer l;
  l.name = "ground";
  l.base_color = tmp.file("colour.png");
  l.normal = tmp.file("normal.png");
  l.tile_m = 2.0f;
  l.sampling = scene::TerrainSampling::Stochastic;
  t.materials->layers.push_back(l);
  desc.ddc = tmp.file("ddc");
  return desc;
}

// A test module where the engine names its terrain resolve: shadows off, no water, no stochastic
// mesh material (scene_renderer.cpp's `resolve_module`).
std::string module_manifest(const test::TempDir& tmp, const char* name, const unsigned char* bytes,
                            usize size) {
  const std::string spirv = tmp.file(std::string(name) + ".spv");
  REQUIRE(io::write_file(spirv, std::string_view(reinterpret_cast<const char*>(bytes), size)) ==
          io::Status::Ok);
  std::string path = spirv;
  for (char& c : path)
    if (c == '\\') c = '/';
  const std::string manifest = tmp.file(std::string(name) + ".json");
  // No source: the library loads the SPIR-V as it is and never recompiles it.
  REQUIRE(io::write_file(manifest,
                         "{\"slangc\": \"none\", \"shaders\": [{\"name\": "
                         "\"visibility_resolve_terrain\", \"source\": \"\", \"spirv\": \"" +
                             path + "\"}]}") == io::Status::Ok);
  return manifest;
}

Vector<u8> draw(gfx::Device& device, GpuScene& scene, const ResolvedSettings& resolved,
                const std::string& manifest, const Camera& camera, u32 frames = 1) {
  std::string error;
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = 256;
  rd.height = 144;
  rd.shader_manifest = manifest;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
  FrameDesc frame;
  frame.camera = camera;
  CapturedFrame shot;
  for (u32 f = 0; f < frames; ++f)
    REQUIRE_MESSAGE(renderer.render_offscreen(frame, &error), error);
  REQUIRE_MESSAGE(renderer.capture(frame, CaptureChannels{}, shot, &error), error);
  renderer.destroy();
  return shot.color;
}

u32 differing(const Vector<u8>& a, const Vector<u8>& b, u32 row_begin, u32 row_end) {
  u32 n = 0;
  for (u32 y = row_begin; y < row_end; ++y)
    for (u32 x = 0; x < 256; ++x) {
      const u32 p = (y * 256 + x) * 4;
      n += a[p] != b[p] || a[p + 1] != b[p + 1] || a[p + 2] != b[p + 2] ? 1u : 0u;
    }
  return n;
}

f32 flip_mean(const Vector<u8>& reference, const Vector<u8>& test) {
  image::Image a, b;
  a.width = b.width = 256;
  a.height = b.height = 144;
  a.channels = b.channels = 4;
  a.pixels = reference;
  b.pixels = test;
  image::FloatImage map;
  std::string error;
  REQUIRE_MESSAGE(image::flip(a, b, map, image::FlipOptions{}, &error), error);
  return image::pool_mean(map);
}
}  // namespace

TEST_CASE("renderer: the ground reads fewer tiles where a pixel cannot tell, within the bound") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("no device: " << error);
    return;
  }
  if (!device.features().texture_compression_bc) {
    MESSAGE("no BC textures: the rules need a built texture's mips");
    device.destroy();
    return;
  }
  const test::TempDir tmp("terrain_footprint");
  const SceneDesc desc = footprint_scene(tmp);
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  RenderSettings settings;
  settings.lights = false;
  settings.shadows = ShadowMode::Off;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
    MESSAGE("rendering unavailable on this device");
    device.destroy();
    return;
  }
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  REQUIRE(scene.normal_variance_textures() == 1);
  const std::string full =
      module_manifest(tmp, "terrain_full_reads", shaders::k_terrain_full_reads_spirv,
                      shaders::k_terrain_full_reads_spirv_size);
  const std::string footprint =
      module_manifest(tmp, "terrain_footprint_reads", shaders::k_terrain_footprint_reads_spirv,
                      shaders::k_terrain_footprint_reads_spirv_size);

  // The near ground: looking down from 3 m, a pixel covers about 2 cm of a 2 m repeat whose cells
  // are 0.58 m. The four footprint rules are exactly off: the same bytes. Tiles by weight, the
  // engine's default, acts here too and stays within the bound.
  Camera down;
  down.position = WorldPos{0.3, 3.0, 0.7};
  down.target = WorldPos{0.0, 0.0, -1.0};
  const Vector<u8> near_full = draw(device, scene, resolved, full, down);
  const Vector<u8> near_footprint = draw(device, scene, resolved, footprint, down);
  const Vector<u8> near_engine = draw(device, scene, resolved, "", down);
  CHECK(differing(near_footprint, near_full, 0, 144) == 0);
  const f32 near_flip = flip_mean(near_full, near_engine);
  MESSAGE("near ground, tiles by weight: " << differing(near_engine, near_full, 0, 144)
                                           << " pixels of 36864 differ, FLIP mean " << near_flip);
  CHECK(differing(near_engine, near_full, 0, 144) > 0);  // the engine's module is the one drawn
  CHECK(near_flip < 0.005f);

  // To the horizon from 1.7 m: the footprint passes a cell about 25 m out and a repeat's filter
  // width a few hundred metres out. Under the footprint rules the bottom rows (under 10 m) hold
  // their bytes and the far ground differs, within the bound.
  Camera level;
  level.position = WorldPos{0.0, 1.7, 0.0};
  level.target = WorldPos{0.0, 0.2, -40.0};
  const Vector<u8> far_full = draw(device, scene, resolved, full, level);
  const Vector<u8> far_footprint = draw(device, scene, resolved, footprint, level);
  const Vector<u8> far_engine = draw(device, scene, resolved, "", level);
  const u32 changed = differing(far_footprint, far_full, 0, 144);
  const u32 changed_near = differing(far_footprint, far_full, 120, 144);
  const f32 far_flip = flip_mean(far_full, far_footprint);
  const f32 engine_flip = flip_mean(far_full, far_engine);
  MESSAGE("to the horizon, footprint rules: " << changed << " pixels of 36864 differ, "
                                              << changed_near << " in the bottom 24 rows; FLIP "
                                              << far_flip << "; tiles by weight: FLIP "
                                              << engine_flip);
  CHECK(changed > 0);  // the rules act (and the test's module is the one drawn)
  CHECK(changed_near == 0);
  CHECK(far_flip < 0.005f);
  CHECK(engine_flip < 0.005f);

  // A frame is the frame again: no rule reads anything that changes between frames.
  CHECK(differing(far_engine, draw(device, scene, resolved, "", level, 3), 0, 144) == 0);
  CHECK(differing(far_footprint, draw(device, scene, resolved, footprint, level, 3), 0, 144) == 0);
  scene.destroy();
  device.destroy();
}

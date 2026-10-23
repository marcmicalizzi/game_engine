#include <foundation/image/decode.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/terrain_materials.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <limits>

using namespace engine;
using namespace engine::renderer;

namespace {
void write_pixel(const std::string& path, u8 r, u8 g, u8 b) {
  const u8 pixel[] = {r, g, b, 255};
  REQUIRE(image::write_png(path, 1, 1, 4, pixel) == io::Status::Ok);
}

TerrainDesc ground(const std::string& colour) {
  TerrainDesc d;
  d.enabled = true;
  d.size = 17;
  d.extent = 8;
  d.dune_height = 0;
  d.materials.emplace();
  scene::TerrainMaterialLayer l;
  l.name = "ground";
  l.base_color = colour;
  d.materials->layers.push_back(l);
  return d;
}
}  // namespace

TEST_CASE("renderer: native terrain materials validate budgets, images and finite numbers") {
  const test::TempDir tmp("terrain_material_validation");
  const std::string colour = tmp.file("colour.png");
  write_pixel(colour, 64, 128, 192);
  auto t = ground(colour);
  std::string error;
  CHECK(validate_terrain_materials(*t.materials, &error));
  t.materials->layer_budget = 0;
  CHECK_FALSE(validate_terrain_materials(*t.materials, &error));
  CHECK(error.find("layer_budget") != std::string::npos);
  t.materials->layer_budget = 8;
  t.materials->detail_far_m = t.materials->detail_near_m;
  CHECK_FALSE(validate_terrain_materials(*t.materials, &error));
  t.materials->detail_far_m = 128;
  t.materials->layers[0].tile_m = std::numeric_limits<f32>::quiet_NaN();
  CHECK_FALSE(validate_terrain_materials(*t.materials, &error));
  t.materials->layers[0].tile_m = 1;
  t.materials->layers[0].normal = tmp.file("missing.png");
  CHECK_FALSE(validate_terrain_materials(*t.materials, &error));
  CHECK(error.find("missing.png") != std::string::npos);
}

TEST_CASE("renderer: native terrain material images change cache identity and no height") {
  const test::TempDir tmp("terrain_material_cache");
  const std::string colour = tmp.file("colour.png");
  const std::string roughness = tmp.file("roughness.png");
  write_pixel(colour, 64, 128, 192);
  write_pixel(roughness, 73, 210, 244);
  auto t = ground(colour);
  t.materials->layers[0].roughness = roughness;
  std::string error;
  const u64 before = terrain_hash(t);
  write_pixel(colour, 65, 128, 192);
  CHECK(terrain_hash(t) != before);
  const f32 height = terrain_height(t, 0.5f, -1.25f);
  assets::MeshData mesh;
  mesh.materials.emplace_back();
  REQUIRE_MESSAGE(append_terrain_materials(t, mesh, &error), error);
  REQUIRE(mesh.materials.size() == 2);
  const auto& layer = mesh.materials[1];
  REQUIRE(layer.metallic_roughness_image >= 0);
  const auto& ref = mesh.images[static_cast<u32>(layer.metallic_roughness_image)];
  image::Image decoded;
  REQUIRE(image::decode_image({ref.bytes.data(), ref.bytes.size()}, decoded, 4, &error));
  CHECK(decoded.pixels[1] == 73);
  CHECK(decoded.pixels[2] == 0);
  CHECK(layer.metallic == doctest::Approx((73.0 / 255) * 0.9));
  t.materials.reset();
  CHECK(terrain_height(t, 0.5f, -1.25f) == height);
}

TEST_CASE(
    "renderer: native terrain material paths resolve from the scene and invalid blocks refuse") {
  const test::TempDir tmp("terrain_material_scene");
  write_pixel(tmp.file("ground.png"), 22, 110, 45);
  const std::string scene = tmp.file("scene.json");
  REQUIRE(io::write_file(scene, R"({"format":"engine.scene.v1","terrain":{"size":17,"extent":8,
    "materials":{"layers":[{"base_color":"ground.png","projection":"Map"}]}}})") == io::Status::Ok);
  SceneDesc d;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(scene, d, error), error);
  REQUIRE(d.terrain.materials.has_value());
  CHECK(io::is_absolute_path(d.terrain.materials->layers[0].base_color));
  REQUIRE(io::write_file(scene,
                         R"({"format":"engine.scene.v1","terrain":{"materials":{"layers":[]}}})") ==
          io::Status::Ok);
  CHECK_FALSE(read_scene_file(scene, d, error));
  CHECK(error.find("layers") != std::string::npos);
  REQUIRE(io::write_file(scene, R"({"format":"engine.scene.v1","terrain":{
    "materials":{"layers":[{}]},"detail":{}}})") == io::Status::Ok);
  CHECK_FALSE(read_scene_file(scene, d, error));
  CHECK(error.find("alternative surface-detail") != std::string::npos);
}

TEST_CASE(
    "renderer: native layers shade the one native surface and preserve depth across cache paths") {
  const test::TempDir tmp("terrain_material_gpu");
  const std::string colour = tmp.file("colour.png");
  write_pixel(colour, 24, 160, 48);
  write_pixel(tmp.file("blue.png"), 0, 0, 200);
  write_pixel(tmp.file("red.png"), 240, 0, 0);
  write_pixel(tmp.file("half.png"), 128, 128, 128);
  write_pixel(tmp.file("zero.png"), 0, 0, 0);
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("no device: " << error);
    return;
  }
  CapturedFrame first;
  for (const bool cache : {false, true}) {
    SceneDesc desc;
    desc.meshes.push_back("");
    desc.terrain = ground(colour);
    scene::TerrainMaterialLayer blue;
    blue.base_color = tmp.file("blue.png");
    blue.mask = tmp.file("half.png");
    desc.terrain.materials->layers.push_back(blue);
    scene::TerrainMaterialLayer red;
    red.base_color = tmp.file("red.png");
    red.mask = tmp.file("zero.png");
    desc.terrain.materials->layers.push_back(red);
    desc.cache = cache;
    desc.ddc = tmp.file("ddc");
    SceneData data;
    REQUIRE_MESSAGE(load_scene(desc, data, error), error);
    RenderSettings settings;
    settings.shadows = ShadowMode::Off;
    settings.view_mode = static_cast<u32>(gfx::ResolveMode::Albedo);
    ResolvedSettings resolved;
    resolve_settings(settings, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      MESSAGE("rendering unavailable on this device");
      device.destroy();
      return;
    }
    GpuScene scene;
    REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
    SceneRenderer renderer;
    SceneRenderer::Desc rd;
    rd.width = 96;
    rd.height = 96;
    REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
    FrameDesc frame;
    frame.camera.position = WorldPos{0, 8, 5};
    frame.camera.target = WorldPos{0, 0, 0};
    frame.view_mode = settings.view_mode;
    CaptureChannels channels;
    channels.depth = true;
    CapturedFrame shot;
    REQUIRE_MESSAGE(renderer.capture(frame, channels, shot, &error), error);
    REQUIRE(shot.covered > 1000);
    u32 green = 0;
    for (u32 p = 0; p < shot.width * shot.height; ++p) {
      if (shot.depth[p] == 0) continue;
      green += shot.color[p * 4] < 40 && shot.color[p * 4 + 1] > 80 &&
                       shot.color[p * 4 + 2] > 100 && shot.color[p * 4 + 2] > shot.color[p * 4 + 1]
                   ? 1u
                   : 0u;
    }
    CHECK(green == shot.covered);
    if (cache) {
      REQUIRE(shot.depth.size() == first.depth.size());
      for (u32 p = 0; p < shot.depth.size(); ++p)
        CHECK(shot.depth[p] == first.depth[p]);
    } else {
      first = std::move(shot);
    }
    renderer.destroy();
    scene.destroy();
  }
  device.destroy();
}

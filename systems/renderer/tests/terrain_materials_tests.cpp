#include <domain/scene_gen/scene_gen.h>
#include <foundation/image/decode.h>
#include <foundation/image/metrics.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/reference.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/terrain_materials.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <bit>
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
  REQUIRE(mesh.materials.size() == 3);  // the root, the layer, and the terrain's record
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

TEST_CASE("renderer: ground masks are a palette's, over a tile set, and refused otherwise") {
  const test::TempDir tmp("terrain_masks_scene");
  write_pixel(tmp.file("a.png"), 22, 110, 45);
  const std::string scene = tmp.file("scene.json");
  SceneDesc d;
  std::string error;
  // Masks with no materials: nothing for them to weigh.
  REQUIRE(io::write_file(scene, R"({"format":"engine.scene.v1","terrain":{"provider":"tiles",
    "tiles":{"heightfield":"none.json"},"masks":{}}})") == io::Status::Ok);
  CHECK_FALSE(read_scene_file(scene, d, error));
  CHECK(error.find("palette of terrain.materials") != std::string::npos);
  // Masks over a ground that is not a tile set.
  REQUIRE(io::write_file(scene, R"({"format":"engine.scene.v1","terrain":{"size":17,"extent":8,
    "materials":{"layer_budget":4,"layers":[{"name":"a","base_color":"a.png"}]},"masks":{}}})") ==
          io::Status::Ok);
  CHECK_FALSE(read_scene_file(scene, d, error));
  CHECK(error.find("authored tile set") != std::string::npos);
  // With masks the layers are a palette larger than the per-tile budget, which must be 1 to 4.
  scene::TerrainMaterials palette;
  for (const char* name : {"a", "b", "c", "d", "e", "f"}) {
    scene::TerrainMaterialLayer l;
    l.name = name;
    l.base_color = tmp.file("a.png");
    palette.layers.push_back(l);
  }
  palette.layer_budget = 4;
  CHECK_FALSE(validate_terrain_materials(palette, &error));
  CHECK(validate_terrain_materials(palette, &error, true));
  palette.layer_budget = 8;
  CHECK_FALSE(validate_terrain_materials(palette, &error, true));
  CHECK(error.find("per-tile budget") != std::string::npos);
  palette.layer_budget = 4;
  palette.layers[0].porosity = 1.5f;
  CHECK_FALSE(validate_terrain_materials(palette, &error, true));
  CHECK(error.find("porosity") != std::string::npos);
  // A region naming a water body the scene does not have.
  REQUIRE(io::write_file(scene, R"({"format":"engine.scene.v1","terrain":{"provider":"tiles",
    "tiles":{"heightfield":"none.json"},
    "materials":{"layer_budget":4,"layers":[{"name":"a","base_color":"a.png"}]},
    "masks":{"regions":[{"name":"pond","water_body":"lake","bed":"a"}]}}})") == io::Status::Ok);
  CHECK_FALSE(read_scene_file(scene, d, error));
  CHECK(error.find("\"lake\"") != std::string::npos);
}

TEST_CASE("renderer: a terrain root is an ordinary material plus its flag and its record's index") {
  // Every reader not gated on k_material_terrain_layers reads the root's own normal scale,
  // occlusion strength and UV words; only uv_offset.z, an alpha-tested material's word, names the
  // record (visibility_resolve.h, "A terrain root is an ordinary material plus a flag").
  gfx::ResolveMaterial root;
  root.normal_scale = 0.75f;
  root.occlusion_strength = 0.5f;
  const gfx::ResolveMaterial before = root;
  gfx::set_terrain_root(root, 1234);
  CHECK(gfx::terrain_record_index(root) == 1234);
  CHECK(root.normal_scale == before.normal_scale);
  CHECK(root.occlusion_strength == before.occlusion_strength);
  CHECK((root.flags & gfx::k_material_uv_transform) == 0);
  CHECK(root.flags == (before.flags | gfx::k_material_terrain_layers));
  CHECK(root.uv_transform.x == before.uv_transform.x);
  CHECK(root.uv_transform.w == before.uv_transform.w);
  CHECK(root.uv_offset.x == before.uv_offset.x);
  CHECK(root.uv_offset.w == before.uv_offset.w);
  gfx::TerrainRecord record;
  record.first_layer = 7;
  record.detail_far_m = 96;
  record.mask = 0x123456789abcull;
  const gfx::ResolveMaterial packed = gfx::pack_terrain_record(record);
  CHECK(std::bit_cast<gfx::TerrainRecord>(packed).mask == record.mask);
  CHECK(std::bit_cast<gfx::TerrainRecord>(packed).first_layer == 7);
}

namespace {
// A test ground (scene_gen's registrar pattern) that is flat and answers a synthetic mask level:
// 16 m tiles of 8 cells at 2 m over [-32, 32] m, layer 0 west of x = 0 and layer 1 east of it (both
// halves on the seam's points), and wet north of z = 8 m — every tile holding an edge point holds
// the same bytes, as the builder guarantees (ground_masks.h).
constexpr u32 k_probe_cells = 8;
constexpr i64 k_probe_spacing = 2000;
f32 probe_height(const void*, f32, f32) noexcept { return 0.0f; }
f32 probe_height_mm(const void*, f64, i64, i64) noexcept { return 0.0f; }
f32 probe_surface_mm(const void*, i64, i64) noexcept { return 0.0f; }
void probe_grid(const void*, const scene_gen::Lattice&, i32, i32, u32 nx, u32 nz,
                std::span<f32> heights) noexcept {
  for (u32 i = 0; i < nx * nz; ++i)
    heights[i] = 0.0f;
}
bool probe_info(const void*, scene_gen::GroundMaskInfo& out) noexcept {
  out = scene_gen::GroundMaskInfo{};
  out.spacing_mm = k_probe_spacing;
  out.cells = k_probe_cells;
  out.tile_min_x = out.tile_min_z = -2;
  out.tile_max_x = out.tile_max_z = 1;
  out.palette = 2;
  out.budget = 2;
  return true;
}
bool probe_tile(const void*, i32 tx, i32 tz, std::span<u32> words) noexcept {
  if (tx < -2 || tx > 1 || tz < -2 || tz > 1) return false;
  for (u32& w : words)
    w = 0;
  words[0] = 0u | (1u << 8) | 0xFFFF0000u;
  const u32 points = scene_gen::mask_points(k_probe_cells);
  for (u32 m = 0; m < scene_gen::mask_mips(k_probe_cells); ++m) {
    const u32 side = scene_gen::mask_mip_side(k_probe_cells, m);
    for (u32 b = 0; b < side; ++b)
      for (u32 a = 0; a < side; ++a) {
        const i64 x = (static_cast<i64>(tx) * k_probe_cells + (static_cast<i64>(a) << m));
        const i64 z = (static_cast<i64>(tz) * k_probe_cells + (static_cast<i64>(b) << m));
        const u32 west = x < 0 ? 255u : (x == 0 ? 128u : 0u);
        const u32 p = scene_gen::mask_mip_offset(k_probe_cells, m) + b * side + a;
        words[1 + p] = west | ((255u - west) << 8);
        words[1 + points + p / 4] |= (z * k_probe_spacing > 8000 ? 255u : 0u) << (8 * (p % 4));
      }
  }
  return true;
}
constexpr scene_gen::GroundMaskOps k_probe_mask_ops{.info = &probe_info, .tile = &probe_tile};
bool probe_masks(const void*, scene_gen::GroundMasks& out, std::string*) {
  out = scene_gen::GroundMasks{&k_probe_mask_ops, nullptr};
  return true;
}
constexpr scene_gen::GroundOps k_probe_ops{.destroy = nullptr,
                                           .height = &probe_height,
                                           .floor = nullptr,
                                           .grid = &probe_grid,
                                           .evaluate = nullptr,
                                           .travel_m = nullptr,
                                           .wind = nullptr,
                                           .transport = nullptr,
                                           .make_rings = nullptr,
                                           .open_tiles = nullptr,
                                           .record = nullptr,
                                           .height_mm = &probe_height_mm,
                                           .surface_mm = &probe_surface_mm,
                                           .floor_mm = &probe_surface_mm,
                                           .tile_source = nullptr,
                                           .masks = &probe_masks};
bool probe_make(const scene::Terrain&, const scene_gen::Context&, scene_gen::GroundProvider& out,
                std::string*) {
  out = scene_gen::GroundProvider(&k_probe_ops, nullptr);
  return true;
}
constexpr scene_gen::GroundProviderDesc k_probe_provider{
    .name = "mask_probe", .make = &probe_make, .flags = 0};
const scene_gen::Registrar k_probe_registrar{k_probe_provider};
}  // namespace

TEST_CASE("renderer: ground masks shade their layers, wet them, and agree with the path tracer") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("no device: " << error);
    return;
  }
  const test::TempDir tmp("terrain_masks_gpu");
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.terrain.enabled = true;
  desc.terrain.size = 17;
  desc.terrain.extent = 32;
  desc.terrain.provider = "mask_probe";
  desc.terrain.materials.emplace();
  desc.terrain.materials->layer_budget = 2;
  for (const auto& [name, tint] :
       {std::pair{"red", Vec3{0.7f, 0.05f, 0.05f}}, std::pair{"blue", Vec3{0.05f, 0.05f, 0.7f}}}) {
    scene::TerrainMaterialLayer l;
    l.name = name;
    l.tint = tint;
    l.porosity = 1.0f;
    desc.terrain.materials->layers.push_back(l);
  }
  desc.terrain.masks.emplace();
  desc.cache = false;
  desc.ddc = tmp.file("ddc");
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  // Traced shadows where the device has them, so the reference has the frame's structures to trace.
  RenderSettings settings;
  settings.shadows = ShadowMode::RayTraced;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
    settings.shadows = ShadowMode::Off;
    resolve_settings(settings, device.features(), &data, resolved);
  }
  if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
    MESSAGE("rendering unavailable on this device");
    device.destroy();
    return;
  }
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  CHECK(scene.terrain_mask_tiles() == 16);
  CHECK(scene.terrain_mask_bytes() ==
        (16u + 16u * scene_gen::mask_tile_words(k_probe_cells)) * sizeof(u32));
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = 160;
  rd.height = 120;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
  Camera camera;
  camera.position = WorldPos{0, 18, 14};
  camera.target = WorldPos{0, 0, 2};
  FrameDesc frame;
  frame.camera = camera;
  frame.view_mode = static_cast<u32>(gfx::ResolveMode::Albedo);
  CaptureChannels channels;
  channels.depth = true;
  CapturedFrame albedo;
  REQUIRE_MESSAGE(renderer.render_offscreen(frame, &error), error);
  REQUIRE_MESSAGE(renderer.capture(frame, channels, albedo, &error), error);
  // West red, east blue; north of z = 8 m (the picture's top) the same layers darker by porosity 1
  // at full wetness: Lagarde's 0.2 of the dry albedo.
  const Mat4 inv = inverse(renderer.views()[0].view_proj);
  u32 dry[2] = {}, wet[2] = {}, wrong = 0;
  f64 dry_sum[2] = {}, wet_sum[2] = {};
  for (u32 py = 0; py < albedo.height; ++py)
    for (u32 px = 0; px < albedo.width; ++px) {
      const u32 i = py * albedo.width + px;
      if (albedo.depth[i] == 0) continue;
      // The pixel's ground point, from its depth (reversed-Z, the frame's own projection).
      const f32 ndc_x = (static_cast<f32>(px) + 0.5f) / albedo.width * 2.0f - 1.0f;
      const f32 ndc_y = 1.0f - (static_cast<f32>(py) + 0.5f) / albedo.height * 2.0f;
      const Vec4 h = inv * Vec4{ndc_x, ndc_y, albedo.depth[i], 1.0f};
      const f32 x = h.x / h.w, z = h.z / h.w + 14.0f;  // the frame's origin is the eye (0, 18, 14)
      if (std::abs(x) < 2.5f || std::abs(z - 8.0f) < 2.5f || std::abs(x) > 30 || std::abs(z) > 30)
        continue;  // the blends' cells, and the grid's edge
      const u8* c = &albedo.color[i * 4];
      const u32 layer = x < 0 ? 0 : 1;
      const u32 strong = layer == 0 ? c[0] : c[2];
      const u32 weak = layer == 0 ? c[2] : c[0];
      if (strong <= 2 * weak) ++wrong;
      if (z > 8) {
        ++wet[layer];
        wet_sum[layer] += strong;
      } else {
        ++dry[layer];
        dry_sum[layer] += strong;
      }
    }
  MESSAGE("dry " << dry[0] << "/" << dry[1] << ", wet " << wet[0] << "/" << wet[1] << ", wrong "
                 << wrong);
  REQUIRE(dry[0] > 200);
  REQUIRE(dry[1] > 200);
  REQUIRE(wet[0] > 200);
  REQUIRE(wet[1] > 200);
  CHECK(wrong == 0);
  for (u32 l = 0; l < 2; ++l)
    CHECK(wet_sum[l] / wet[l] < 0.75 * dry_sum[l] / dry[l]);
  // The reference path tracer reads the same masks: FLIP between its picture and the resolve's
  // within the reference tests' own bound for a diffuse plane under the sun (0.05).
  std::string why;
  if (reference_available(resolved, device, &why)) {
    ReferenceRenderer reference;
    REQUIRE_MESSAGE(reference.create(device, scene, renderer, {}, &error), error);
    ReferenceSettings rs;
    rs.spp = 64;
    rs.max_bounces = 1;
    rs.pixel_center = true;
    ReferenceFrame traced;
    REQUIRE_MESSAGE(reference.render(camera, rs, traced, &error), error);
    FrameDesc lit;
    lit.camera = camera;
    CapturedFrame shot;
    REQUIRE_MESSAGE(renderer.render_offscreen(lit, &error), error);
    REQUIRE_MESSAGE(renderer.capture(lit, CaptureChannels{}, shot, &error), error);
    image::Image a, b;
    a.width = b.width = rd.width;
    a.height = b.height = rd.height;
    a.channels = b.channels = 4;
    a.pixels = traced.color;
    b.pixels = shot.color;
    image::FloatImage map;
    REQUIRE(image::flip(a, b, map, image::FlipOptions{}, &error));
    const f32 flip = image::pool_mean(map);
    MESSAGE("FLIP mean, resolve against the path tracer over masked ground: " << flip);
    CHECK(flip < 0.05f);
    reference.destroy();
  } else {
    MESSAGE("no reference here: " << why);
  }
  renderer.destroy();
  scene.destroy();
  device.destroy();
}

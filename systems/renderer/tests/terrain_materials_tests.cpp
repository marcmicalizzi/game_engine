#include <domain/scene_gen/scene_gen.h>
#include <foundation/image/decode.h>
#include <foundation/image/metrics.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>
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
#include <numbers>

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
struct ProbeReads {
  u32 reads = 0;
  u32 resident_mib = 256;
  bool sparse = false;
  bool fail_tile = false;
};
thread_local ProbeReads* probe_reads = nullptr;
bool probe_present(const void*, i32 tx, i32 tz) noexcept {
  return tx >= -2 && tx <= 1 && tz >= -2 && tz <= 1 &&
         !(probe_reads != nullptr && probe_reads->sparse && tx == -1 && tz == -1);
}
bool probe_tile(const void*, i32 tx, i32 tz, std::span<u32> words) noexcept {
  if (probe_reads != nullptr) {
    ++probe_reads->reads;
    if (probe_reads->fail_tile) return false;
  }
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
bool counted_probe_info(const void* state, scene_gen::GroundMaskInfo& out) noexcept {
  if (!probe_info(state, out)) return false;
  if (probe_reads != nullptr) out.resident_mib = probe_reads->resident_mib;
  return true;
}
constexpr scene_gen::GroundMaskOps k_probe_mask_ops{
    .present = &probe_present, .info = &counted_probe_info, .tile = &probe_tile};
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

TEST_CASE("renderer: mask upload reads present tiles once and checks budget before payloads") {
  struct Counting {
    ProbeReads state;
    Counting() { probe_reads = &state; }
    ~Counting() { probe_reads = nullptr; }
  } counted;
  TerrainDesc desc;
  desc.enabled = true;
  desc.provider = "mask_probe";
  desc.size = 17;
  desc.extent = 32;
  TerrainMaskUpload upload;
  std::string error;
  counted.state.sparse = true;
  REQUIRE_MESSAGE(prepare_terrain_masks(desc, upload, &error), error);
  CHECK(upload.tiles == 15);
  CHECK(counted.state.reads == 15);
  CHECK(upload.words.size() == 16 + 15 * scene_gen::mask_tile_words(k_probe_cells));
  CHECK(upload.words[5] == gfx::k_terrain_no_mask_tile);
  for (u32 p = 0, slot = 0; p < 16; ++p) {
    if (p == 5) continue;
    CHECK(upload.words[p] == slot++);
  }
  counted.state.reads = 0;
  counted.state.resident_mib = 0;
  CHECK_FALSE(prepare_terrain_masks(desc, upload, &error));
  CHECK(counted.state.reads == 0);
  CHECK(error.find("resident_mib") != std::string::npos);
  counted.state.resident_mib = 256;
  counted.state.fail_tile = true;
  CHECK_FALSE(prepare_terrain_masks(desc, upload, &error));
  CHECK(counted.state.reads == 1);
  CHECK(error.find("(-2, -2)") != std::string::npos);
}
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

TEST_CASE(
    "renderer: stochastic sampling validates, keeps tiled scenes' keys and carries a height mean") {
  const test::TempDir tmp("terrain_stochastic_validation");
  const std::string colour = tmp.file("colour.png");
  const std::string height = tmp.file("height.png");
  write_pixel(colour, 64, 128, 192);
  write_pixel(height, 51, 51, 51);
  auto t = ground(colour);
  t.materials->layers[0].height = height;
  std::string error;
  const u64 tiled = terrain_materials_hash(*t.materials);
  // Tiled is the default and writes nothing new: the key and the container are what they were.
  assets::MeshData mesh;
  mesh.materials.emplace_back();
  REQUIRE_MESSAGE(append_terrain_materials(t, mesh, &error), error);
  CHECK(mesh.materials[1].alpha_cutoff == 0.5f);
  t.materials->layers[0].sampling = scene::TerrainSampling::Stochastic;
  CHECK(validate_terrain_materials(*t.materials, &error));
  CHECK(terrain_materials_hash(*t.materials) != tiled);
  assets::MeshData stochastic;
  stochastic.materials.emplace_back();
  REQUIRE_MESSAGE(append_terrain_materials(t, stochastic, &error), error);
  CHECK(stochastic.materials[1].alpha_cutoff == doctest::Approx(0.2));
  // A turn is a stochastic layer's; it is bounded; and a painting is never made stochastic.
  t.materials->layers[0].rotation_deg = 190;
  CHECK_FALSE(validate_terrain_materials(*t.materials, &error));
  t.materials->layers[0].rotation_deg = 30;
  CHECK(validate_terrain_materials(*t.materials, &error));
  // The turn is shading read at upload, not a container's identity.
  const u64 turned = terrain_materials_hash(*t.materials);
  t.materials->layers[0].rotation_deg = 0;
  CHECK(terrain_materials_hash(*t.materials) == turned);
  t.materials->layers[0].projection = scene::TerrainProjection::Map;
  CHECK_FALSE(validate_terrain_materials(*t.materials, &error));
  CHECK(error.find("triplanar") != std::string::npos);
  t.materials->layers[0].projection = scene::TerrainProjection::Triplanar;
  t.materials->layers[0].sampling = scene::TerrainSampling::Tiled;
  t.materials->layers[0].rotation_deg = 30;
  CHECK_FALSE(validate_terrain_materials(*t.materials, &error));
}

TEST_CASE(
    "renderer: a stochastic layer shades the same ground in the resolve and the path tracer") {
  // R96's three readers: the resolve (with traced shadows where the device has them, so its ray
  // module is the one drawn) and the reference path tracer read one lattice; FLIP between their
  // pictures stays within the reference tests' bound for a diffuse plane under the sun.
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("no device: " << error);
    return;
  }
  const test::TempDir tmp("terrain_stochastic_reference");
  constexpr u32 k_n = 32;
  Vector<u8> colour(k_n * k_n * 4u), normal(k_n * k_n * 4u);
  for (u32 z = 0; z < k_n; ++z)
    for (u32 x = 0; x < k_n; ++x) {
      const u32 p = (z * k_n + x) * 4;
      const f64 a = 2 * std::numbers::pi * (x + 2.0 * z) / k_n;
      colour[p] = static_cast<u8>(std::lround(120 + 70 * std::sin(a)));
      colour[p + 1] = static_cast<u8>(std::lround(100 + 50 * std::cos(a)));
      colour[p + 2] = 60;
      colour[p + 3] = 255;
      // A gentle OpenGL normal map of the height 0.02 sin(a) over a 2 m repeat.
      const f64 hu = 0.02 * std::cos(a) * 2 * std::numbers::pi / 2.0;
      const f64 hv = 2 * hu;
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
  desc.terrain = ground(tmp.file("colour.png"));
  desc.terrain.extent = 32;
  desc.terrain.materials->layers[0].normal = tmp.file("normal.png");
  desc.terrain.materials->layers[0].tile_m = 2.0f;
  desc.terrain.materials->layers[0].sampling = scene::TerrainSampling::Stochastic;
  desc.terrain.materials->layers[0].rotation_deg = 60;
  desc.cache = false;
  desc.ddc = tmp.file("ddc");
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
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
  std::string why;
  if (!reference_available(resolved, device, &why)) {
    MESSAGE("no reference here: " << why);
    device.destroy();
    return;
  }
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = 160;
  rd.height = 120;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
  Camera camera;
  camera.position = WorldPos{0, 9, 12};
  camera.target = WorldPos{0, 0, 2};
  FrameDesc lit;
  lit.camera = camera;
  CapturedFrame shot;
  REQUIRE_MESSAGE(renderer.render_offscreen(lit, &error), error);
  REQUIRE_MESSAGE(renderer.capture(lit, CaptureChannels{}, shot, &error), error);
  ReferenceRenderer reference;
  REQUIRE_MESSAGE(reference.create(device, scene, renderer, {}, &error), error);
  ReferenceSettings rs;
  rs.spp = 64;
  rs.max_bounces = 1;
  rs.pixel_center = true;
  ReferenceFrame traced;
  REQUIRE_MESSAGE(reference.render(camera, rs, traced, &error), error);
  image::Image a, b;
  a.width = b.width = rd.width;
  a.height = b.height = rd.height;
  a.channels = b.channels = 4;
  a.pixels = traced.color;
  b.pixels = shot.color;
  image::FloatImage map;
  REQUIRE(image::flip(a, b, map, image::FlipOptions{}, &error));
  const f32 flip = image::pool_mean(map);
  MESSAGE("FLIP mean, resolve against the path tracer over stochastic ground: " << flip);
  CHECK(flip < 0.05f);
  reference.destroy();
  renderer.destroy();
  scene.destroy();
  device.destroy();
}

namespace {
// A tunable set for one scope and put back after it, as a test that compares two settings needs.
struct ScopedTunable {
  tunables::Tunable* tunable = nullptr;
  std::string previous;
  ScopedTunable(const char* name, const char* value) {
    tunable = tunables::find(name);
    REQUIRE_MESSAGE(tunable != nullptr, name);
    tunable->append_value(previous);
    REQUIRE(tunable->set_from_text(value));
  }
  ~ScopedTunable() {
    if (tunable != nullptr) (void)tunable->set_from_text(previous);
  }
};

// sRGB's decode: the resolve and the path tracer both end on the display encode, so their pictures
// are compared, and averaged, as the light they stand for.
f64 linear_of(u8 code) {
  const f64 c = static_cast<f64>(code) / 255.0;
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

// The picture's linear RGB, each `factor` x `factor` block averaged in light: a supersampled render
// brought to the size of the 1x ones.
Vector<f64> linear_picture(const Vector<u8>& rgba, u32 width, u32 height, u32 factor) {
  const u32 w = width / factor;
  const u32 h = height / factor;
  Vector<f64> out(w * h * 3, 0.0);
  const f64 share = 1.0 / static_cast<f64>(factor * factor);
  for (u32 y = 0; y < height; ++y)
    for (u32 x = 0; x < width; ++x)
      for (u32 c = 0; c < 3; ++c)
        out[((y / factor) * w + x / factor) * 3 + c] +=
            share * linear_of(rgba[(y * width + x) * 4 + c]);
  return out;
}

f64 rmse(const Vector<f64>& a, const Vector<f64>& b) {
  f64 sum = 0.0;
  for (u32 i = 0; i < a.size(); ++i)
    sum += (a[i] - b[i]) * (a[i] - b[i]);
  return std::sqrt(sum / static_cast<f64>(a.size()));
}

f64 mean_of(const Vector<f64>& a) {
  f64 sum = 0.0;
  for (const f64 v : a)
    sum += v;
  return sum / static_cast<f64>(a.size());
}
}  // namespace

TEST_CASE(
    "renderer: normal filtering holds a grazing normal-mapped plane to its converged picture") {
  // R97's fixture (normal-filtering.md, "The plane"): flat ground carrying a fine bump map — 8
  // periods of sin x sin z in a 64-texel image tiled every 0.5 m, so a bump is 6.25 cm — at a flat
  // roughness, dark, under a low sun straight ahead, seen at the sun's own elevation so the glint
  // lies on the image. Three distances, 1, 4 and 16 m, at one grazing angle and a 20° view: the
  // pixel's footprint grows fourfold each step, from a quarter of a bump to about four. The truth
  // is the reference path tracer's (level-zero textures, 256 jittered samples a pixel: it
  // integrates the footprint, and reads no variance chain). Beside it, for the record, the resolve
  // rendered at 8x a side and box-averaged in linear light: no truth for the unfiltered path, whose
  // sub-pixels' mips still flatten bumps the footprint holds (normal-filtering.md has the table).
  // The roughness is 0.7 so that no glint clips and the pictures' light can be compared.
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("no device: " << error);
    return;
  }
  if (!device.features().texture_compression_bc) {
    MESSAGE("no BC textures: the variance chain is a built texture's");
    device.destroy();
    return;
  }
  const test::TempDir tmp("terrain_normal_filter");
  constexpr u32 k_n = 64;
  constexpr f64 k_slope = 0.35;  // the bumps' largest slope
  Vector<u8> normal(k_n * k_n * 4u);
  for (u32 z = 0; z < k_n; ++z)
    for (u32 x = 0; x < k_n; ++x) {
      const u32 p = (z * k_n + x) * 4;
      const f64 u = 2 * std::numbers::pi * (x + 0.5) / 8.0;
      const f64 v = 2 * std::numbers::pi * (z + 0.5) / 8.0;
      const f64 hu = k_slope * std::cos(u) * std::sin(v);
      const f64 hv = k_slope * std::sin(u) * std::cos(v);
      const f64 len = std::sqrt(hu * hu + hv * hv + 1);
      normal[p] = static_cast<u8>(std::lround((-hu / len * 0.5 + 0.5) * 255));
      normal[p + 1] = static_cast<u8>(std::lround((hv / len * 0.5 + 0.5) * 255));
      normal[p + 2] = static_cast<u8>(std::lround((1 / len * 0.5 + 0.5) * 255));
      normal[p + 3] = 255;
    }
  REQUIRE(image::write_png(tmp.file("normal.png"), k_n, k_n, 4, {normal.data(), normal.size()}) ==
          io::Status::Ok);
  write_pixel(tmp.file("colour.png"), 20, 20, 20);
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.terrain = ground(tmp.file("colour.png"));
  desc.terrain.extent = 256;
  scene::TerrainMaterialLayer& layer = desc.terrain.materials->layers[0];
  layer.normal = tmp.file("normal.png");
  layer.tile_m = 0.5f;
  layer.roughness_factor = 0.7f;
  desc.ddc = tmp.file("ddc");  // built textures: the variance chain is a built texture's section
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  RenderSettings settings;
  settings.lights = false;
  // Traced shadows only so that the frame builds the structures the path tracer traces; a flat
  // plane casts nothing on itself.
  settings.shadows = ShadowMode::RayTraced;
  settings.sun_azimuth_deg = -90.0f;  // straight ahead of a camera looking along -z
  settings.sun_elevation_deg = 12.0f;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
    MESSAGE("rendering unavailable on this device");
    device.destroy();
    return;
  }
  std::string why;
  if (!reference_available(resolved, device, &why)) {
    MESSAGE("no reference here: " << why);
    device.destroy();
    return;
  }
  GpuScene filtered;
  REQUIRE_MESSAGE(filtered.create(device, data, resolved, &error), error);
  REQUIRE(filtered.normal_variance_textures() == 1);
  GpuScene unfiltered;
  {
    const ScopedTunable off("renderer.material.normal_filter", "false");
    REQUIRE_MESSAGE(unfiltered.create(device, data, resolved, &error), error);
  }
  CHECK(unfiltered.normal_variance_textures() == 0);
  CHECK(unfiltered.texture_bytes() + filtered.normal_variance_bytes() == filtered.texture_bytes());
  constexpr u32 k_w = 160;
  constexpr u32 k_h = 120;
  constexpr u32 k_super = 8;
  SceneRenderer one_f, one_u, eight_f, eight_u;
  SceneRenderer::Desc rd;
  rd.width = k_w;
  rd.height = k_h;
  REQUIRE_MESSAGE(one_f.create(device, filtered, resolved, rd, &error), error);
  REQUIRE_MESSAGE(one_u.create(device, unfiltered, resolved, rd, &error), error);
  SceneRenderer::Desc big = rd;
  big.width = k_w * k_super;
  big.height = k_h * k_super;
  REQUIRE_MESSAGE(eight_f.create(device, filtered, resolved, big, &error), error);
  REQUIRE_MESSAGE(eight_u.create(device, unfiltered, resolved, big, &error), error);
  ReferenceRenderer reference;
  REQUIRE_MESSAGE(reference.create(device, filtered, one_f, {}, &error), error);
  const auto shoot = [&](SceneRenderer& renderer, const FrameDesc& frame, CapturedFrame& shot) {
    REQUIRE_MESSAGE(renderer.render_offscreen(frame, &error), error);
    REQUIRE_MESSAGE(renderer.capture(frame, CaptureChannels{}, shot, &error), error);
  };
  const f64 tilt = std::tan(12.0 * std::numbers::pi / 180.0);
  for (const f64 distance : {1.0, 4.0, 16.0}) {
    Camera camera;
    camera.position = WorldPos{0, distance * tilt, 0};
    camera.target = WorldPos{0, 0, -distance};
    camera.fov_y = static_cast<f32>(20.0 * std::numbers::pi / 180.0);
    camera.znear = 0.01f;
    FrameDesc frame;
    frame.camera = camera;
    CapturedFrame f1, u1, f8, u8;
    shoot(one_f, frame, f1);
    shoot(one_u, frame, u1);
    shoot(eight_f, frame, f8);
    shoot(eight_u, frame, u8);
    ReferenceSettings rs;
    rs.spp = 256;
    rs.max_bounces = 1;
    ReferenceFrame traced;
    REQUIRE_MESSAGE(reference.render(camera, rs, traced, &error), error);
    const Vector<f64> truth = linear_picture(traced.color, k_w, k_h, 1);
    const Vector<f64> lf1 = linear_picture(f1.color, k_w, k_h, 1);
    const Vector<f64> lu1 = linear_picture(u1.color, k_w, k_h, 1);
    const Vector<f64> lf8 = linear_picture(f8.color, k_w * k_super, k_h * k_super, k_super);
    const Vector<f64> lu8 = linear_picture(u8.color, k_w * k_super, k_h * k_super, k_super);
    image::Image a, b;
    a.width = b.width = k_w;
    a.height = b.height = k_h;
    a.channels = b.channels = 4;
    a.pixels = traced.color;
    b.pixels = f1.color;
    image::FloatImage map;
    REQUIRE(image::flip(a, b, map, image::FlipOptions{}, &error));
    const f32 flip_filtered = image::pool_mean(map);
    b.pixels = u1.color;
    REQUIRE(image::flip(a, b, map, image::FlipOptions{}, &error));
    const f32 flip_unfiltered = image::pool_mean(map);
    const f64 e_f1 = rmse(lf1, truth);
    const f64 e_u1 = rmse(lu1, truth);
    const f64 m_truth = mean_of(truth);
    MESSAGE("distance " << distance << " m: mean linear, traced " << mean_of(truth)
                        << ", filtered 1x " << mean_of(lf1) << ", unfiltered 1x " << mean_of(lu1)
                        << ", 8x filtered " << mean_of(lf8) << ", 8x unfiltered " << mean_of(lu8));
    MESSAGE("distance " << distance << " m: RMSE against the path tracer, filtered 1x " << e_f1
                        << ", unfiltered 1x " << e_u1 << ", 8x filtered " << rmse(lf8, truth)
                        << ", 8x unfiltered " << rmse(lu8, truth)
                        << "; against the 8x unfiltered resolve, filtered 1x " << rmse(lf1, lu8)
                        << ", unfiltered 1x " << rmse(lu1, lu8) << "; FLIP against the path "
                        << "tracer, filtered " << flip_filtered << ", unfiltered "
                        << flip_unfiltered);
    // Nearer the truth pixel by pixel at every distance, by at least a third where the bumps are
    // unresolved; nearer in the picture's light; and no worse by FLIP. (FLIP's 0.05 of the
    // reference tests is a diffuse plane's: here the path tracer's box-integrated pixel and the
    // resolve's point sample differ wherever the bumps are resolved, filtered or not.)
    CHECK(e_f1 < e_u1);
    if (distance > 1.0) CHECK(e_f1 < e_u1 * (2.0 / 3.0));
    CHECK(std::abs(mean_of(lf1) - m_truth) < std::abs(mean_of(lu1) - m_truth));
    CHECK(flip_filtered <= flip_unfiltered);
  }
  reference.destroy();
  eight_u.destroy();
  eight_f.destroy();
  one_u.destroy();
  one_f.destroy();
  unfiltered.destroy();
  filtered.destroy();
  device.destroy();
}

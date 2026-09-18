// The renderer, headless. Every case here builds a scene from a cube GLB written at test time,
// renders it into an offscreen color target, and reads the result back: no window, no surface,
// no swapchain, and no file in the tree. On a machine with no Vulkan device, or one that cannot
// write the visibility buffer, the cases record a skip, exactly as engine-view exits 3.
//
// The renderer must work without a window, because engine-host has none. That is a build-time
// property, not a runtime one: `foundation/window` is not a dependency of this module, so its
// header is not on the include path of anything that links it, and the check below fails the
// build the day someone adds one.
#if __has_include(<foundation/window/window.h>)
#error "systems/renderer must not depend on foundation/window: engine-host renders without one"
#endif

#include <core/math/math.h>
#include <domain/gfx/device.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::renderer;

namespace {

void put_u32(std::vector<u8>& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((v >> (8 * i)) & 0xffu));
}

void put_u16(std::vector<u8>& out, u16 v) {
  out.push_back(static_cast<u8>(v & 0xffu));
  out.push_back(static_cast<u8>(v >> 8));
}

void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  put_u32(out, bits);
}

std::string n(u32 v) { return std::to_string(v); }

// A unit cube as a GLB: 24 vertices with normals and UVs and one primitive of twelve triangles
// with one plain material. The same shape apps/engine_view/tests writes, minus the second
// material and the embedded texture, because nothing here is about materials.
bool write_cube_glb(const std::string& path) {
  const Vec3 normals[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                           Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
  const Vec3 tangents[6] = {Vec3{0, 1, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1},
                            Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{1, 0, 0}};
  std::vector<u8> bin;
  for (u32 f = 0; f < 6; ++f) {  // corners wound counter-clockwise seen from outside
    const Vec3 nrm = normals[f];
    const Vec3 t = tangents[f];
    const Vec3 b = cross(nrm, t);
    const Vec3 corners[4] = {nrm * 0.5f - t * 0.5f - b * 0.5f, nrm * 0.5f + t * 0.5f - b * 0.5f,
                             nrm * 0.5f + t * 0.5f + b * 0.5f, nrm * 0.5f - t * 0.5f + b * 0.5f};
    for (const Vec3& c : corners) {
      put_f32(bin, c.x);
      put_f32(bin, c.y);
      put_f32(bin, c.z);
    }
  }
  const u32 normal_offset = static_cast<u32>(bin.size());
  for (u32 f = 0; f < 6; ++f) {
    for (u32 c = 0; c < 4; ++c) {
      put_f32(bin, normals[f].x);
      put_f32(bin, normals[f].y);
      put_f32(bin, normals[f].z);
    }
  }
  const u32 uv_offset = static_cast<u32>(bin.size());
  for (u32 f = 0; f < 6; ++f) {
    const f32 uvs[8] = {0, 0, 1, 0, 1, 1, 0, 1};
    for (const f32 v : uvs)
      put_f32(bin, v);
  }
  const u32 index_offset = static_cast<u32>(bin.size());
  for (u32 f = 0; f < 6; ++f) {
    const u16 base = static_cast<u16>(f * 4);
    const u16 tris[6] = {base, static_cast<u16>(base + 1), static_cast<u16>(base + 2),
                         base, static_cast<u16>(base + 2), static_cast<u16>(base + 3)};
    for (const u16 i : tris)
      put_u16(bin, i);
  }
  const u32 index_bytes = static_cast<u32>(bin.size()) - index_offset;
  while (bin.size() % 4 != 0)
    bin.push_back(0);

  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
      "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.5,0.2,1],"
      "\"metallicFactor\":0,\"roughnessFactor\":0.6}}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\","
      "\"min\":[-0.5,-0.5,-0.5],\"max\":[0.5,0.5,0.5]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":24,\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5123,\"count\":36,\"type\":\"SCALAR\"}],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      n(normal_offset) + "},{\"buffer\":0,\"byteOffset\":" + n(normal_offset) +
      ",\"byteLength\":" + n(uv_offset - normal_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(uv_offset) +
      ",\"byteLength\":" + n(index_offset - uv_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(index_offset) + ",\"byteLength\":" + n(index_bytes) +
      "}],\"buffers\":[{\"byteLength\":" + n(static_cast<u32>(bin.size())) + "}]}";
  while (json.size() % 4 != 0)
    json += ' ';

  std::vector<u8> glb;
  put_u32(glb, 0x46546c67u);  // "glTF"
  put_u32(glb, 2u);
  put_u32(glb, static_cast<u32>(12 + 8 + json.size() + 8 + bin.size()));
  put_u32(glb, static_cast<u32>(json.size()));
  put_u32(glb, 0x4e4f534au);  // "JSON"
  glb.insert(glb.end(), json.begin(), json.end());
  put_u32(glb, static_cast<u32>(bin.size()));
  put_u32(glb, 0x004e4942u);  // "BIN\0"
  glb.insert(glb.end(), bin.begin(), bin.end());
  std::ofstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  f.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
  return f.good();
}

std::string slashes(const std::filesystem::path& p) {
  std::string s = p.string();
  for (char& c : s) {
    if (c == '\\') c = '/';
  }
  return s;
}

// A device, or nothing: every case skips the same way engine-view exits 3.
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

// The scene, the settings, the GPU scene, and the renderer, in the one order they build in.
struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;

  bool build(const gfx::Device& device, const SceneDesc& desc, const RenderSettings& settings,
             u32 width, u32 height) {
    if (!load_scene(desc, data, error)) return false;
    resolve_settings(settings, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      error = availability_message(check_availability(resolved, device.features()));
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = width;
    rd.height = height;
    return renderer.create(device, scene, resolved, rd, &error);
  }
};

const u32* pixel_id(const CapturedFrame& frame, u32 x, u32 y) {
  return &frame.ids[(u64{y} * frame.width + x) * k_id_words];
}

}  // namespace

TEST_CASE("renderer: draws a cube offscreen with no window and reads every channel back") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_renderer");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "cube.glb");
  REQUIRE(write_cube_glb(mesh));

  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.ddc = slashes(dir / "ddc");  // never the repository's cache
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, RenderSettings{}, 160, 120), rig.error);
  CHECK(rig.data.cluster_count() >= 1);
  CHECK(rig.data.lod.leaf_triangle_count == 12);
  CHECK(rig.scene.instance_count() == 1);
  CHECK(rig.scene.pair_count() == rig.data.cluster_count());

  FrameDesc frame;
  frame.camera = orbit_camera_at(rig.data.center, rig.data.radius, 40.0f, 0.0f, k_orbit_pitch);
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  channels.normals = true;
  CapturedFrame shot;
  std::string error;
  REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &error), error);
  CHECK(shot.width == 160);
  CHECK(shot.height == 120);
  CHECK(shot.color.size() == u64{160} * 120 * 4);
  CHECK(shot.normals.size() == u64{160} * 120 * 3);
  CHECK(shot.ids.size() == u64{160} * 120 * k_id_words);
  CHECK(shot.depth.size() == u64{160} * 120);
  CHECK(shot.covered > 0);
  CHECK(shot.depth_max > 0.0f);
  CHECK(shot.depth_min <= shot.depth_max);

  // The center pixel names the one instance placed there, a cluster of that scene, and a
  // triangle within it. A pixel the cube does not cover names nothing at all.
  const u32* center = pixel_id(shot, 80, 60);
  CHECK(center[0] == 0);
  CHECK(center[1] < rig.data.cluster_count());
  CHECK(center[2] < 128u);
  const u32* corner = pixel_id(shot, 0, 0);
  CHECK(corner[0] == k_no_id);
  CHECK(corner[1] == k_no_id);
  CHECK(corner[2] == k_no_id);

  // Every statistic the JSON summary and a benchmark report is filled in. The GPU zones are
  // read a frame late, so the first frame may have none; the visible counts are not.
  const Stats& stats = rig.renderer.stats();
  CHECK(stats.frames == 2);  // the color frame and the normals frame
  CHECK(stats.visible_pairs() > 0);
  CHECK(stats.visible_min <= stats.visible_pairs());
  CHECK(stats.visible_max >= stats.visible_pairs());
  CHECK(stats.cpu_ms_per_frame() > 0.0);
  CHECK(stats.cull_ms() >= 0.0);
  CHECK(stats.hw_ms() >= 0.0);
  CHECK(stats.sw_ms() >= 0.0);
  CHECK(stats.hiz_ms() >= 0.0);
  CHECK(stats.resolve_ms() >= 0.0);
  CHECK(stats.rt_ms() >= 0.0);
  CHECK(stats.clas_ms() >= 0.0);
  CHECK(stats.deform_ms() >= 0.0);
  CHECK(stats.trace_ms() >= 0.0);
  CHECK(stats.total_ms() >= 0.0);

  // Device memory, which is what says whether a frame was timed against a card another process
  // was holding. create() samples it, so it is filled without the caller asking.
  CHECK(stats.gpu_memory.device_local_total_mib > 0);
  if (stats.gpu_memory.valid) {  // VK_EXT_memory_budget; a device without it reports zeros
    CHECK(stats.gpu_memory.budget_mib > 0);
    CHECK(stats.gpu_memory.budget_mib <= stats.gpu_memory.device_local_total_mib);
    CHECK(stats.gpu_memory.used_mib <= stats.gpu_memory.device_local_total_mib);
  }
  // A reset does not leave the summary without a figure, and a second sample still agrees.
  rig.renderer.sample_gpu_memory();
  CHECK(stats.gpu_memory.device_local_total_mib > 0);

  // The channels land on disk in the documented shapes.
  CaptureFiles files;
  REQUIRE_MESSAGE(write_capture(slashes(dir), "shot", shot, files, error), error);
  CHECK(std::filesystem::exists(files.color));
  CHECK(std::filesystem::exists(files.normals));
  CHECK(std::filesystem::exists(files.depth));
  CHECK(std::filesystem::exists(files.ids));
  CHECK(std::filesystem::exists(files.ids_header));
  CHECK(std::filesystem::file_size(files.ids) == u64{160} * 120 * k_id_words * 4);
}

TEST_CASE("renderer: two instances give two ids") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_renderer_ids");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "cube.glb");
  REQUIRE(write_cube_glb(mesh));

  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.ddc = slashes(dir / "ddc");
  SceneInstance left;
  left.transform.position = Vec3{-1.0f, 0.0f, 0.0f};
  SceneInstance right;
  right.transform.position = Vec3{1.0f, 0.0f, 0.0f};
  desc.instances.push_back(left);
  desc.instances.push_back(right);
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, RenderSettings{}, 200, 120), rig.error);
  CHECK(rig.scene.instance_count() == 2);
  CHECK(rig.scene.pair_count() == rig.data.cluster_count() * 2);

  FrameDesc frame;
  frame.camera =
      orbit_camera_at(rig.data.center, rig.data.radius, 40.0f, radians(90.0f), k_orbit_pitch);
  CaptureChannels channels;
  channels.color = false;
  channels.ids = true;
  CapturedFrame shot;
  std::string error;
  REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &error), error);

  bool saw[2] = {false, false};
  u32 other = 0;
  for (u32 p = 0; p < shot.width * shot.height; ++p) {
    const u32 instance = shot.ids[u64{p} * k_id_words];
    if (instance == k_no_id) continue;
    if (instance < 2) {
      saw[instance] = true;
    } else {
      ++other;
    }
  }
  CHECK(saw[0]);
  CHECK(saw[1]);
  CHECK(other == 0);
}

TEST_CASE("renderer: settings resolve the same way for every host") {
  gfx::DeviceFeatures features;  // a device with nothing optional: the baseline tier
  RenderSettings settings;
  ResolvedSettings resolved;
  resolve_settings(settings, features, nullptr, resolved);
  CHECK(resolved.settings.raster == RasterMode::Vertex);  // no mesh shaders: the baseline tier
  CHECK(resolved.vertex_path);
  CHECK_FALSE(resolved.shadows);
  CHECK_FALSE(resolved.rt_chain);
  CHECK(resolved.occlusion);
  CHECK(check_availability(resolved, features) == RenderAvailability::NoVisibilityBuffer);

  features.buffer_int64_atomics = true;
  settings.raster = RasterMode::RayTrace;
  resolve_settings(settings, features, nullptr, resolved);
  CHECK(resolved.ray_path);
  CHECK(check_availability(resolved, features) == RenderAvailability::NoAccelerationStructures);

  features.cluster_acceleration_structure = true;
  features.ray_query = true;
  features.mesh_shader = true;
  settings.raster = RasterMode::Hardware;
  settings.cull = false;
  settings.rt_templates = true;
  resolve_settings(settings, features, nullptr, resolved);
  CHECK(resolved.shadows);  // auto: the device can build the structures
  CHECK(resolved.rt_chain);
  CHECK(resolved.settings.cull);    // forced on: the structures come from the cull output
  CHECK_FALSE(resolved.occlusion);  // off with shadows: one visible list to build from
  CHECK(resolved.settings.rt_templates);
  CHECK(check_availability(resolved, features) == RenderAvailability::Ok);

  // A path that cannot shadow says so and leaves everything else alone.
  settings.raster = RasterMode::Software;
  settings.shadows = ShadowMode::RayTraced;
  settings.rt_templates = true;
  resolve_settings(settings, features, nullptr, resolved);
  CHECK_FALSE(resolved.shadows);
  CHECK_FALSE(resolved.rt_chain);
  CHECK_FALSE(resolved.settings.rt_templates);
}

TEST_CASE("renderer: the direct path has no id or depth channel") {
  Gpu gpu;
  if (!gpu.ok || !gpu.device.features().mesh_shader) {
    MESSAGE("renderer unavailable here: " << (gpu.ok ? "no mesh shaders" : gpu.why));
    return;
  }
  const test::TempDir tmp("engine_renderer_direct");
  const std::filesystem::path dir = tmp.native();
  const std::string mesh = slashes(dir / "cube.glb");
  REQUIRE(write_cube_glb(mesh));
  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.ddc = slashes(dir / "ddc");
  RenderSettings settings;
  settings.raster = RasterMode::Direct;
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, settings, 96, 96), rig.error);

  FrameDesc frame;
  frame.camera = orbit_camera_at(rig.data.center, rig.data.radius, 40.0f, 0.0f, k_orbit_pitch);
  CaptureChannels channels;
  channels.ids = true;
  CapturedFrame shot;
  std::string error;
  CHECK_FALSE(rig.renderer.capture(frame, channels, shot, &error));
  CHECK(error.find("visibility buffer") != std::string::npos);
  // Color alone still works there.
  channels.ids = false;
  error.clear();
  REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &error), error);
  CHECK(shot.color.size() == u64{96} * 96 * 4);
}

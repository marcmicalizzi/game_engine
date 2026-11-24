// Alpha-tested geometry (docs/subsystems/renderer.md, "Alpha-tested geometry"; roadmap R65): a quad
// whose glTF material is MASK, drawn by every path that writes the visibility buffer, against the
// same quad drawn opaque.
//
// - Where the mask keeps a pixel, the masked quad writes exactly the opaque quad's word — the same
//   instance, cluster, triangle and depth — and where it cuts, nothing; the cut is where the opaque
//   quad's albedo (white where the mask's alpha is 255, black where it is 0) is black.
// - The mesh path, the indexed vertex draw, the capacity vertex draw and the ray-traced visibility
//   write the same ids and the same coverage, pixel for pixel.
// - A masked cluster never reaches the software rasterizer: with every cluster small enough to go
//   there, the masked quad is still cut by its mask.
// - The opaque shadow proxy (R109): a masked cluster whose baked coverage is at or above
//   `renderer.shadow.opaque_coverage` casts the opaque caster's shadow under the rays, below it the
//   mask still rules, and the tunable at 0 draws exactly what the rays drew before the proxy.
// - The measurement sweep (R115 stage 2) steps the sun ray's rules eight frames at a time, and the
//   proxy's table it bakes is never handed to a frame whose threshold is 0.

#include "test_device.h"

#include <core/math/math.h>
#include <domain/gfx/device.h>
#include <domain/gfx/visibility_resolve.h>
#include <foundation/image/png.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/micromap_cache.h>
#include <systems/renderer/reference.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr u32 k_size = 128;
constexpr u32 k_albedo_view = static_cast<u32>(gfx::ResolveMode::Albedo);
constexpr f32 k_fov = 0.9599310886f;  // Camera's default, radians(55)
constexpr u32 k_texels = 8;

bool write_file(const std::string& path, const void* data, usize size) {
  std::ofstream f(path, std::ios::binary);
  f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  return f.good();
}

void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((bits >> (8 * i)) & 0xffu));
}

// The mask: 2x2-texel blocks of an 8x8 image in a checker, white and opaque where kept, black and
// transparent where cut, read through a nearest sampler so a pixel is the one texel it lands in.
bool write_mask_image(const std::filesystem::path& dir) {
  std::vector<u8> rgba(k_texels * k_texels * 4);
  for (u32 y = 0; y < k_texels; ++y) {
    for (u32 x = 0; x < k_texels; ++x) {
      const bool keep = ((x / 2 + y / 2) & 1u) == 0;
      u8* t = &rgba[(y * k_texels + x) * 4];
      t[0] = t[1] = t[2] = keep ? 255 : 0;
      t[3] = keep ? 255 : 0;
    }
  }
  Vector<u8> png;
  if (!image::encode_png(k_texels, k_texels, 4, std::span<const u8>(rgba.data(), rgba.size()), png))
    return false;
  return write_file((dir / "mask.png").string(), png.data(), png.size());
}

// A quad of half-width 1 in the XY plane facing +z, UV (0,0) at the top left, double-sided, its
// base colour the mask; `alpha_mode` is the glTF material's ("OPAQUE" or "MASK").
bool write_quad(const std::filesystem::path& dir, const char* name, const char* alpha_mode) {
  std::vector<u8> bin;
  const f32 positions[12] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
  const f32 normals[12] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  const f32 uvs[8] = {0, 1, 1, 1, 1, 0, 0, 0};
  for (const f32 v : positions)
    put_f32(bin, v);
  for (const f32 v : normals)
    put_f32(bin, v);
  for (const f32 v : uvs)
    put_f32(bin, v);
  const u16 indices[6] = {0, 1, 2, 0, 2, 3};
  for (const u16 i : indices) {
    bin.push_back(static_cast<u8>(i & 0xff));
    bin.push_back(static_cast<u8>(i >> 8));
  }
  if (!write_file((dir / "quad.bin").string(), bin.data(), bin.size())) return false;
  const std::string json =
      std::string(
          "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
          "\"nodes\":[{\"mesh\":0}],"
          "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
          "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],"
          "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},"
          "\"metallicFactor\":0,\"roughnessFactor\":1},\"doubleSided\":true,"
          "\"alphaMode\":\"") +
      alpha_mode +
      "\",\"alphaCutoff\":0.5}],"
      "\"samplers\":[{\"magFilter\":9728,\"minFilter\":9728}],"
      "\"textures\":[{\"source\":0,\"sampler\":0}],"
      "\"images\":[{\"uri\":\"mask.png\"}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\","
      "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":32},"
      "{\"buffer\":0,\"byteOffset\":128,\"byteLength\":12}],"
      "\"buffers\":[{\"uri\":\"quad.bin\",\"byteLength\":140}]}";
  return write_file((dir / name).string(), json.data(), json.size());
}

// A ground 6 units across at y = 0, white and opaque, and the mask's quad lying flat 1 unit above
// it (the XY quad turned -90 degrees about x), `alpha_mode` as above: what the sun's shadow of a
// masked caster falls on.
bool write_caster_scene(const std::filesystem::path& dir, const char* name,
                        const char* alpha_mode) {
  std::vector<u8> bin;
  const f32 quad_positions[12] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
  const f32 quad_normals[12] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  const f32 quad_uvs[8] = {0, 1, 1, 1, 1, 0, 0, 0};
  const f32 ground_positions[12] = {-3, 0, 3, 3, 0, 3, 3, 0, -3, -3, 0, -3};
  const f32 ground_normals[12] = {0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0};
  const f32 ground_uvs[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (const f32 v : quad_positions)
    put_f32(bin, v);  // 0
  for (const f32 v : quad_normals)
    put_f32(bin, v);  // 48
  for (const f32 v : quad_uvs)
    put_f32(bin, v);  // 96
  for (const f32 v : ground_positions)
    put_f32(bin, v);  // 128
  for (const f32 v : ground_normals)
    put_f32(bin, v);  // 176
  for (const f32 v : ground_uvs)
    put_f32(bin, v);                          // 224
  const u16 indices[6] = {0, 1, 2, 0, 2, 3};  // 256
  for (const u16 i : indices) {
    bin.push_back(static_cast<u8>(i & 0xff));
    bin.push_back(static_cast<u8>(i >> 8));
  }
  for (u32 i = 0; i < 4; ++i)
    bin.push_back(0);  // 268: pad to 272
  if (!write_file((dir / "caster.bin").string(), bin.data(), bin.size())) return false;
  const std::string json =
      std::string(
          "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0,1]}],"
          "\"nodes\":[{\"mesh\":0,\"translation\":[0,1,0],"
          "\"rotation\":[-0.70710678,0,0,0.70710678]},{\"mesh\":1}],"
          "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
          "\"TEXCOORD_0\":2},\"indices\":6,\"material\":0}]},"
          "{\"primitives\":[{\"attributes\":{\"POSITION\":3,\"NORMAL\":4,\"TEXCOORD_0\":5},"
          "\"indices\":6,\"material\":1}]}],"
          "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},"
          "\"metallicFactor\":0,\"roughnessFactor\":1},\"doubleSided\":true,\"alphaMode\":\"") +
      alpha_mode +
      "\",\"alphaCutoff\":0.5},"
      "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.9,0.9,0.9,1],\"metallicFactor\":0,"
      "\"roughnessFactor\":1},\"doubleSided\":true}],"
      "\"samplers\":[{\"magFilter\":9728,\"minFilter\":9728}],"
      "\"textures\":[{\"source\":0,\"sampler\":0}],"
      "\"images\":[{\"uri\":\"mask.png\"}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\","
      "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\","
      "\"min\":[-3,0,-3],\"max\":[3,0,3]},"
      "{\"bufferView\":4,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
      "{\"bufferView\":5,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
      "{\"bufferView\":6,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":32},"
      "{\"buffer\":0,\"byteOffset\":128,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":176,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":224,\"byteLength\":32},"
      "{\"buffer\":0,\"byteOffset\":256,\"byteLength\":12}],"
      "\"buffers\":[{\"uri\":\"caster.bin\",\"byteLength\":272}]}";
  return write_file((dir / name).string(), json.data(), json.size());
}

// R208-M1's fixture: a micromap over a MiB at level 6, the budget setting's grain. The XY quad of
// half-width 1 cut into `n` x `n` quads, each its own four vertices mapping its own cell of a
// 128x128 mask of deterministic noise, so every triangle covers a dozen texels of its own and its
// micro-triangles' states are its own: an entry of 1 KiB a triangle at level 6 (the coarser LOD
// levels' clusters too), a quarter of that at 5.
bool write_noise_grid(const std::filesystem::path& dir, const char* name, u32 n) {
  constexpr u32 k_noise = 128;
  std::vector<u8> rgba(k_noise * k_noise * 4);
  u32 state = 0x2545f491u;
  for (u32 t = 0; t < k_noise * k_noise; ++t) {
    state = state * 1664525u + 1013904223u;
    const u8 v = ((state >> 24) & 1u) != 0 ? u8{255} : u8{0};
    rgba[t * 4 + 0] = rgba[t * 4 + 1] = rgba[t * 4 + 2] = rgba[t * 4 + 3] = v;
  }
  Vector<u8> png;
  if (!image::encode_png(k_noise, k_noise, 4, std::span<const u8>(rgba.data(), rgba.size()), png))
    return false;
  if (!write_file((dir / "noise.png").string(), png.data(), png.size())) return false;
  const u32 vertices = 4 * n * n;
  std::vector<u8> bin;
  for (u32 j = 0; j < n; ++j)
    for (u32 i = 0; i < n; ++i) {
      const f32 x0 = -1.0f + 2.0f * static_cast<f32>(i) / static_cast<f32>(n);
      const f32 x1 = -1.0f + 2.0f * static_cast<f32>(i + 1) / static_cast<f32>(n);
      const f32 y0 = -1.0f + 2.0f * static_cast<f32>(j) / static_cast<f32>(n);
      const f32 y1 = -1.0f + 2.0f * static_cast<f32>(j + 1) / static_cast<f32>(n);
      for (const f32 v : {x0, y0, 0.0f, x1, y0, 0.0f, x1, y1, 0.0f, x0, y1, 0.0f})
        put_f32(bin, v);
    }
  for (u32 v = 0; v < vertices; ++v)
    for (const f32 c : {0.0f, 0.0f, 1.0f})
      put_f32(bin, c);
  for (u32 j = 0; j < n; ++j)
    for (u32 i = 0; i < n; ++i) {
      const f32 u0 = static_cast<f32>(i) / static_cast<f32>(n);
      const f32 u1 = static_cast<f32>(i + 1) / static_cast<f32>(n);
      const f32 v0 = 1.0f - static_cast<f32>(j) / static_cast<f32>(n);
      const f32 v1 = 1.0f - static_cast<f32>(j + 1) / static_cast<f32>(n);
      for (const f32 c : {u0, v0, u1, v0, u1, v1, u0, v1})
        put_f32(bin, c);
    }
  const usize index_offset = bin.size();
  for (u32 q = 0; q < n * n; ++q)
    for (const u32 k : {0u, 1u, 2u, 0u, 2u, 3u}) {
      const u32 i = 4 * q + k;
      bin.push_back(static_cast<u8>(i & 0xff));
      bin.push_back(static_cast<u8>(i >> 8));
    }
  const usize index_bytes = bin.size() - index_offset;
  while (bin.size() % 4 != 0)
    bin.push_back(0);
  if (!write_file((dir / "grid.bin").string(), bin.data(), bin.size())) return false;
  const std::string count = std::to_string(vertices);
  const std::string p_bytes = std::to_string(vertices * 12);
  const std::string uv_at = std::to_string(vertices * 24);
  const std::string uv_bytes = std::to_string(vertices * 8);
  const std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
      "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},"
      "\"metallicFactor\":0,\"roughnessFactor\":1},\"doubleSided\":true,"
      "\"alphaMode\":\"MASK\",\"alphaCutoff\":0.5}],"
      "\"samplers\":[{\"magFilter\":9728,\"minFilter\":9728}],"
      "\"textures\":[{\"source\":0,\"sampler\":0}],"
      "\"images\":[{\"uri\":\"noise.png\"}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":" +
      count +
      ",\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":" +
      count +
      ",\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":" +
      count +
      ",\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5123,\"count\":" +
      std::to_string(6 * n * n) +
      ",\"type\":\"SCALAR\"}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      p_bytes + "},{\"buffer\":0,\"byteOffset\":" + p_bytes + ",\"byteLength\":" + p_bytes +
      "},{\"buffer\":0,\"byteOffset\":" + uv_at + ",\"byteLength\":" + uv_bytes +
      "},{\"buffer\":0,\"byteOffset\":" + std::to_string(index_offset) +
      ",\"byteLength\":" + std::to_string(index_bytes) +
      "}],"
      "\"buffers\":[{\"uri\":\"grid.bin\",\"byteLength\":" +
      std::to_string(bin.size()) + "}]}";
  return write_file((dir / name).string(), json.data(), json.size());
}

struct Shot {
  CapturedFrame frame;
  std::string skip;  // why the path could not run here, or empty
  bool masked_scene = false;
};

// One frame of `mesh` through `settings`, from 2.5 units in front of the quad, at a slant so the
// depth varies across it: the albedo view, the ids and the depth.
struct ShotView {
  Vec3 eye{0.6f, 0.4f, 2.5f};
  Vec3 target{0.0f, 0.0f, 0.0f};
  u32 mode = k_albedo_view;
};

Shot shoot(const std::string& mesh, RenderSettings settings, ShotView view = ShotView{}) {
  Shot out;
  gfx::Device device;
  std::string error;
  if (!device.create(renderer_test::device_options(), &error)) {
    out.skip = "device unavailable: " + error;
    return out;
  }
  SceneData data;
  GpuScene scene;
  SceneRenderer renderer;
  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.cache = false;  // decoded at one level: every reader samples the same texels
  settings.view_mode = view.mode;
  REQUIRE_MESSAGE(load_scene(desc, data, error), "scene: " << error);
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  const RenderAvailability availability = check_availability(resolved, device.features());
  if (availability != RenderAvailability::Ok) {
    out.skip = "unavailable here: " + unavailable_reason(availability, device);
    device.destroy();
    return out;
  }
  if (settings.shadows == ShadowMode::RayTraced && !resolved.shadows) {
    out.skip = "no ray-traced shadows on this device";
    device.destroy();
    return out;
  }
  if (settings.raster == RasterMode::RayTrace && !resolved.ray_path) {
    out.skip = "no ray-traced visibility on this device";
    device.destroy();
    return out;
  }
  if (settings.raster == RasterMode::Hardware && resolved.vertex_path) {
    out.skip = "no mesh shaders on this device";
    device.destroy();
    return out;
  }
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), "gpu scene: " << error);
  out.masked_scene = scene.has_masked();
  SceneRenderer::Desc rd;
  rd.width = k_size;
  rd.height = k_size;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), "renderer: " << error);
  FrameDesc frame;
  frame.camera.position = absolute(WorldPos::origin(), view.eye);
  frame.camera.target = absolute(WorldPos::origin(), view.target);
  frame.camera.fov_y = k_fov;
  frame.view_mode = view.mode;
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, out.frame, &error), error);
  renderer.destroy();
  scene.destroy();
  device.destroy();
  return out;
}

bool covered(const CapturedFrame& f, u32 p) { return f.depth[p] != 0.0f; }

struct Fixture {
  test::TempDir tmp{"alpha_mask"};
  std::string opaque;
  std::string masked;
  std::string caster_opaque;
  std::string caster_masked;
  Fixture() {
    const std::filesystem::path dir(tmp.path());
    REQUIRE(write_mask_image(dir));
    REQUIRE(write_quad(dir, "opaque.gltf", "OPAQUE"));
    REQUIRE(write_quad(dir, "masked.gltf", "MASK"));
    REQUIRE(write_caster_scene(dir, "caster_opaque.gltf", "OPAQUE"));
    REQUIRE(write_caster_scene(dir, "caster_masked.gltf", "MASK"));
    caster_opaque = (dir / "caster_opaque.gltf").string();
    caster_masked = (dir / "caster_masked.gltf").string();
    opaque = (dir / "opaque.gltf").string();
    masked = (dir / "masked.gltf").string();
  }
};

RenderSettings base_settings(RasterMode raster) {
  RenderSettings s;
  s.raster = raster;
  s.shadows = ShadowMode::Off;
  return s;
}

}  // namespace

TEST_CASE(
    "alpha mask: a masked quad writes the opaque quad's words where it keeps, none where it cuts") {
  Fixture fx;
  const Shot opaque = shoot(fx.opaque, base_settings(RasterMode::Hardware));
  if (!opaque.skip.empty()) {
    MESSAGE("skipped: " << opaque.skip);
    return;
  }
  const Shot masked = shoot(fx.masked, base_settings(RasterMode::Hardware));
  REQUIRE(masked.skip.empty());
  CHECK(masked.masked_scene);
  CHECK_FALSE(opaque.masked_scene);
  const CapturedFrame& a = opaque.frame;
  const CapturedFrame& b = masked.frame;
  REQUIRE(a.width == k_size);
  u32 kept = 0;
  u32 cut = 0;
  u32 wrong = 0;
  for (u32 p = 0; p < k_size * k_size; ++p) {
    if (!covered(a, p)) {
      CHECK_FALSE(covered(b, p));  // nothing appears where the quad is not
      continue;
    }
    // The opaque quad's albedo is the mask's colour: white where it keeps, black where it cuts.
    const bool white = a.color[p * 4 + 1] > 127;
    if (covered(b, p)) {
      ++kept;
      CHECK(b.depth[p] == a.depth[p]);
      for (u32 w = 0; w < k_id_words; ++w)
        CHECK(b.ids[p * k_id_words + w] == a.ids[p * k_id_words + w]);
      if (!white) ++wrong;
    } else {
      ++cut;
      if (white) ++wrong;
    }
  }
  MESSAGE("kept " << kept << ", cut " << cut << ", disagreeing with the albedo " << wrong);
  CHECK(kept > 1000);
  CHECK(cut > 1000);
  // The resolve's UV (screen barycentrics) and the rasterizer's (an interpolated attribute) can
  // round to different texels only on a pixel whose centre lies on a texel edge.
  CHECK(wrong <= 4);
}

TEST_CASE("alpha mask: every rasterizer and the ray path write the same masked picture") {
  Fixture fx;
  struct Path {
    const char* name;
    RenderSettings settings;
  };
  RenderSettings capacity = base_settings(RasterMode::Vertex);
  capacity.cull = false;  // the capacity draw: every cluster, no index buffer
  // Every cluster is small enough for the software rasterizer, and the masked one must not go.
  RenderSettings split = base_settings(RasterMode::Auto);
  split.sw_px = 1.0e6f;
  const Path paths[] = {
      {"mesh", base_settings(RasterMode::Hardware)},
      {"vertex", base_settings(RasterMode::Vertex)},
      {"vertex capacity", capacity},
      {"split, all small", split},
      {"ray", base_settings(RasterMode::RayTrace)},
  };
  std::optional<CapturedFrame> first;
  std::string first_name;
  for (const Path& path : paths) {
    const Shot shot = shoot(fx.masked, path.settings);
    if (!shot.skip.empty()) {
      MESSAGE(std::string(path.name) << " skipped: " << shot.skip);
      continue;
    }
    const std::string name = path.name;
    CAPTURE(name);
    REQUIRE(shot.masked_scene);
    if (!first) {
      first = shot.frame;
      first_name = path.name;
      continue;
    }
    u32 differing = 0;
    for (u32 p = 0; p < k_size * k_size; ++p) {
      bool same = covered(*first, p) == covered(shot.frame, p);
      for (u32 w = 0; w < k_id_words && same; ++w)
        same = first->ids[p * k_id_words + w] == shot.frame.ids[p * k_id_words + w];
      if (!same) ++differing;
    }
    MESSAGE(name << " against " << first_name << ": " << differing << " pixels differ, "
                 << shot.frame.covered << " covered");
    CHECK(differing == 0);
  }
}

namespace {

// The share of the ground's pixels in shadow: the ground is the cluster that covers the most,
// and a pixel of it is shadowed where its green is below the midpoint of the ground's darkest and
// brightest (the sun's share of a white ground is most of its light).
struct ShadowCount {
  u32 ground = 0;
  u32 shadowed = 0;
  Vector<u8> classes;  // per pixel: 0 not ground, 1 lit, 2 shadowed
  f64 share() const { return ground != 0 ? static_cast<f64>(shadowed) / ground : 0.0; }
};

ShadowCount count_shadow(const CapturedFrame& f) {
  ShadowCount out;
  out.classes.assign(k_size * k_size, u8{0});
  // The ground is the cluster that covers the most pixels: the quad is a quarter of the view.
  std::map<u32, u32> counts;
  for (u32 p = 0; p < k_size * k_size; ++p)
    if (covered(f, p)) ++counts[f.ids[p * k_id_words + 1]];
  u32 ground_cluster = 0;
  u32 most = 0;
  for (const auto& [cluster, count] : counts) {
    if (count > most) {
      most = count;
      ground_cluster = cluster;
    }
  }
  u8 lo = 255;
  u8 hi = 0;
  for (u32 p = 0; p < k_size * k_size; ++p) {
    if (!covered(f, p) || f.ids[p * k_id_words + 1] != ground_cluster) continue;
    const u8 g = f.color[p * 4 + 1];
    lo = g < lo ? g : lo;
    hi = g > hi ? g : hi;
  }
  const u32 mid = (static_cast<u32>(lo) + hi) / 2;
  for (u32 p = 0; p < k_size * k_size; ++p) {
    if (!covered(f, p) || f.ids[p * k_id_words + 1] != ground_cluster) continue;
    ++out.ground;
    const bool dark = f.color[p * 4 + 1] < mid;
    out.shadowed += dark ? 1u : 0u;
    out.classes[p] = dark ? 2 : 1;
  }
  return out;
}

}  // namespace

TEST_CASE("alpha mask: the cascades and the shadow rays let the sun through the same holes") {
  Fixture fx;
  ShotView view;
  // From the side the shadow falls on (the sun is up and towards +x +z), so the quad hides the
  // ground away from its shadow rather than over it.
  view.eye = Vec3{-2.5f, 4.0f, -2.5f};
  view.target = Vec3{-0.4f, 0.0f, -0.4f};
  view.mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  RenderSettings rays = base_settings(RasterMode::Hardware);
  rays.shadows = ShadowMode::RayTraced;
  RenderSettings maps = base_settings(RasterMode::Hardware);
  maps.shadows = ShadowMode::Cascaded;
  struct Case {
    const char* name;
    RenderSettings settings;
  };
  const Case cases[] = {{"rays", rays}, {"cascades", maps}};
  ShadowCount masked[2];
  ShadowCount solid_counts[2];
  bool ran[2] = {false, false};
  for (u32 i = 0; i < 2; ++i) {
    const Shot solid = shoot(fx.caster_opaque, cases[i].settings, view);
    if (!solid.skip.empty()) {
      MESSAGE(std::string(cases[i].name) << " skipped: " << solid.skip);
      continue;
    }
    const Shot cut = shoot(fx.caster_masked, cases[i].settings, view);
    REQUIRE(cut.skip.empty());
    const ShadowCount full = count_shadow(solid.frame);
    solid_counts[i] = full;
    masked[i] = count_shadow(cut.frame);
    ran[i] = true;
    MESSAGE(std::string(cases[i].name) << ": the opaque caster shadows " << full.share()
                                       << " of the ground, the masked one " << masked[i].share());
    // The mask's checker keeps half the quad, so the sun comes through about half its shadow.
    CHECK(full.share() > 0.05);
    CHECK(masked[i].share() > 0.3 * full.share());
    CHECK(masked[i].share() < 0.7 * full.share());
  }
  if (ran[0] && ran[1]) {
    // Pixel for pixel away from the shadow's edges: a ground pixel whose 7x7 neighbourhood the rays
    // call all lit or all shadowed, which leaves out the band the maps' 3x3 filter softens and
    // their texels step (the shading tests leave out the same band). Three pixels here are about
    // 0.11 m of ground, two or three of the nearest cascade's texels.
    auto disagreement = [&](const ShadowCount& traced, const ShadowCount& mapped, u32& interior) {
      u32 disagree = 0;
      interior = 0;
      for (u32 y = 3; y + 3 < k_size; ++y) {
        for (u32 x = 3; x + 3 < k_size; ++x) {
          const u32 p = y * k_size + x;
          const u8 ray = traced.classes[p];
          if (ray == 0 || mapped.classes[p] == 0) continue;
          // Where the two already disagree under the opaque caster — the maps' speckle at the
          // frame's near edge, where a cascade texel is largest on screen — the mask is not what
          // is being compared.
          if (solid_counts[0].classes[p] != solid_counts[1].classes[p]) continue;
          bool uniform = true;
          for (u32 dy = 0; dy < 7 && uniform; ++dy)
            for (u32 dx = 0; dx < 7 && uniform; ++dx)
              uniform = traced.classes[(y + dy - 3) * k_size + (x + dx - 3)] == ray;
          if (!uniform) continue;
          ++interior;
          if (mapped.classes[p] != ray) ++disagree;
        }
      }
      return disagree;
    };
    u32 interior = 0;
    u32 solid_interior = 0;
    const u32 disagree = disagreement(masked[0], masked[1], interior);

    const u32 baseline = disagreement(solid_counts[0], solid_counts[1], solid_interior);
    MESSAGE("rays and cascades disagree on " << disagree << " of " << interior
                                             << " ground pixels away from the masked shadow's "
                                                "edges; on "
                                             << baseline << " of " << solid_interior
                                             << " under the opaque caster");
    CHECK(interior > 4000);
    // What is left is the same speckle where it fell on other pixels under the masked caster:
    // 1.0% measured on the RTX 5090 (56 of 5514), held to 2%.
    CHECK(baseline == 0);
    CHECK(disagree <= interior / 50);
  }
}
// **The opaque shadow proxy** (renderer.md, "The opaque shadow proxy"; R109). The checker keeps
// half of the quad, so its baked coverage is a half: with the threshold at 0.45 the masked quad is
// solid to the shadow rays and casts the opaque quad's shadow on every ground pixel the two
// pictures share; at 0.55 its mask still decides, and the picture is the one the tunable at 0 —
// no proxy, every mask looked up as before R109 — draws, byte for byte.
TEST_CASE("alpha mask: a dense masked cluster is solid to a shadow ray at the proxy's threshold") {
  Fixture fx;
  ShotView view;
  view.eye = Vec3{-2.5f, 4.0f, -2.5f};
  view.target = Vec3{-0.4f, 0.0f, -0.4f};
  view.mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  RenderSettings rays = base_settings(RasterMode::Hardware);
  rays.shadows = ShadowMode::RayTraced;
  struct Restore {
    ~Restore() { (void)tunables::apply_overrides("renderer.shadow.opaque_coverage=0"); }
  } restore;
  REQUIRE(tunables::apply_overrides("renderer.shadow.opaque_coverage=0"));
  const Shot solid = shoot(fx.caster_opaque, rays, view);
  if (!solid.skip.empty()) {
    MESSAGE("skipped: " << solid.skip);
    return;
  }
  const Shot off = shoot(fx.caster_masked, rays, view);
  REQUIRE(tunables::apply_overrides("renderer.shadow.opaque_coverage=0.55"));
  const Shot below = shoot(fx.caster_masked, rays, view);
  REQUIRE(tunables::apply_overrides("renderer.shadow.opaque_coverage=0.45"));
  const Shot above = shoot(fx.caster_masked, rays, view);
  REQUIRE(off.skip.empty());
  REQUIRE(below.skip.empty());
  REQUIRE(above.skip.empty());
  const ShadowCount full = count_shadow(solid.frame);
  const ShadowCount mask = count_shadow(off.frame);
  const ShadowCount proxy = count_shadow(above.frame);
  MESSAGE("the opaque caster shadows " << full.share() << " of the ground, the masked one "
                                       << mask.share() << ", the masked one by the proxy "
                                       << proxy.share());
  // Below the threshold the mask rules, and the bytes are the tunable at 0's.
  CHECK(below.frame.color.size() == off.frame.color.size());
  CHECK(below.frame.color == off.frame.color);
  CHECK(mask.share() < 0.7 * full.share());
  // At or above it, the opaque caster's shadow on every ground pixel the two pictures share (the
  // masked quad lets the camera see ground through its holes that the opaque one hides).
  u32 shared = 0;
  u32 differ = 0;
  u32 changed = 0;
  for (u32 p = 0; p < k_size * k_size; ++p) {
    if (full.classes[p] == 0 || proxy.classes[p] == 0) continue;
    ++shared;
    if (full.classes[p] != proxy.classes[p]) ++differ;
    if (mask.classes[p] != 0 && mask.classes[p] != proxy.classes[p]) ++changed;
  }
  MESSAGE(shared << " shared ground pixels: " << differ
                 << " differ from the opaque caster's shadow, " << changed
                 << " changed by the proxy from the mask's");
  CHECK(shared > 4000);
  CHECK(differ == 0);
  CHECK(changed > 200);  // the witness: the mask's holes were lit, and the proxy closed them
}

// The measurement sweep's proxy table (R115 stage 2; renderer.md, "Where the resolve's time goes"):
// with `renderer.shadow.sweep` the scene bakes the proxy's coverage at load whatever the threshold,
// for the sweep's proxy rules, and a frame of the "as set" rule — the threshold at 0 — must not be
// handed it (a byte of 1 would make every masked cluster with any coverage solid): the picture is
// the one drawn without the sweep, byte for byte.
TEST_CASE("alpha mask: the sweep's coverage table is not handed to a frame with no threshold") {
  Fixture fx;
  ShotView view;
  view.eye = Vec3{-2.5f, 4.0f, -2.5f};
  view.target = Vec3{-0.4f, 0.0f, -0.4f};
  view.mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  RenderSettings rays = base_settings(RasterMode::Hardware);
  rays.shadows = ShadowMode::RayTraced;
  struct Restore {
    ~Restore() { (void)tunables::apply_overrides("renderer.shadow.sweep=false"); }
  } restore;
  REQUIRE(tunables::apply_overrides("renderer.shadow.sweep=false"));
  REQUIRE(tunables::apply_overrides("renderer.shadow.opaque_coverage=0"));
  const Shot off = shoot(fx.caster_masked, rays, view);
  if (!off.skip.empty()) {
    MESSAGE("skipped: " << off.skip);
    return;
  }
  REQUIRE(tunables::apply_overrides("renderer.shadow.sweep=true"));
  const Shot swept = shoot(fx.caster_masked, rays, view);
  REQUIRE(swept.skip.empty());
  const ShadowCount mask = count_shadow(off.frame);
  MESSAGE("the masked caster shadows " << mask.share() << " of the ground");
  CHECK(mask.share() > 0.0);
  CHECK(swept.frame.color.size() == off.frame.color.size());
  CHECK(swept.frame.color == off.frame.color);
}

// The sweep the experiment flies (R115 stage 2; renderer.md, "Where the resolve's time goes"): with
// `renderer.shadow.sweep`, frame f takes the sun ray's rule (f / 8) % 6, so a flight under
// `renderer.resolve.stop_after=cycle` measures every stage of every rule; without it every frame
// takes the tunables'. CPU only.
TEST_CASE("alpha mask: the shadow sweep steps the sun ray's rules eight frames at a time") {
  struct Restore {
    ~Restore() { (void)tunables::apply_overrides("renderer.shadow.sweep=false"); }
  } restore;
  REQUIRE(tunables::apply_overrides("renderer.shadow.sweep=false"));
  for (u64 f : {u64{0}, u64{8}, u64{40}, u64{47}}) {
    const ShadowRayRule rule = shadow_ray_rule(f, 0.25f);
    CHECK(rule.opaque == false);
    CHECK(rule.opaque_coverage == 0.25f);
    CHECK(rule.count == false);
    CHECK(rule.index == 0);
  }
  REQUIRE(tunables::apply_overrides("renderer.shadow.sweep=true"));
  for (u64 period : {u64{0}, u64{48}}) {
    for (u64 within : {u64{0}, u64{7}}) {
      const u64 base = period + within;
      const ShadowRayRule as_set = shadow_ray_rule(base, 0.25f);
      CHECK(as_set.opaque_coverage == 0.25f);
      CHECK_FALSE(as_set.opaque);
      CHECK_FALSE(as_set.count);
      CHECK(shadow_ray_rule(base + 8, 0.25f).opaque);
      CHECK(shadow_ray_rule(base + 8, 0.25f).opaque_coverage == 0.0f);
      CHECK(shadow_ray_rule(base + 16, 0.25f).opaque_coverage == 0.5f);
      CHECK(shadow_ray_rule(base + 24, 0.25f).opaque_coverage == 0.3f);
      CHECK(shadow_ray_rule(base + 32, 0.25f).opaque_coverage == 0.2f);
      const ShadowRayRule count = shadow_ray_rule(base + 40, 0.25f);
      CHECK(count.count);
      CHECK(count.opaque_coverage == 0.25f);
      // The rule's number, which the frame record carries (T70): rule r for frames 8r to 8r + 7.
      for (u32 r = 0; r < k_shadow_sweeps; ++r)
        CHECK(shadow_ray_rule(base + 8 * u64{r}, 0.25f).index == r);
    }
  }
}

TEST_CASE("alpha mask: the reference path tracer sees through the holes the rasterizer leaves") {
  // The masked quad through the reference (k_pt_alpha_masks: its camera rays decide masked
  // candidates by the mask) and through the real-time frame: a pixel the rasterizer leaves empty is
  // the sky in the reference, to the bit, and a pixel it covers is not. The sky is the furnace's
  // one colour, so a hole reads exactly the background a pixel off the quad reads.
  Fixture fx;
  gfx::Device device;
  std::string error;
  if (!device.create(renderer_test::device_options(), &error)) {
    MESSAGE("skipped: device unavailable: " << error);
    return;
  }
  SceneData data;
  GpuScene scene;
  SceneRenderer renderer;
  ReferenceRenderer reference;
  SceneDesc desc;
  desc.meshes.push_back(fx.masked);
  desc.cache = false;
  RenderSettings settings = base_settings(RasterMode::Hardware);
  settings.shadows = ShadowMode::RayTraced;  // what makes the frame build the structures
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  if (check_availability(resolved, device.features()) != RenderAvailability::Ok ||
      !reference_available(resolved, device, &error)) {
    MESSAGE("skipped: no reference here: " << error);
    device.destroy();
    return;
  }
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  SceneRenderer::Desc rd;
  rd.width = k_size;
  rd.height = k_size;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
  REQUIRE_MESSAGE(reference.create(device, scene, renderer, {}, &error), error);
  FrameDesc frame;
  frame.camera.position = absolute(WorldPos::origin(), ShotView{}.eye);
  frame.camera.target = absolute(WorldPos::origin(), ShotView{}.target);
  frame.camera.fov_y = k_fov;
  CaptureChannels channels;
  channels.depth = true;
  CapturedFrame raster;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, raster, &error), error);
  ReferenceSettings rs;
  rs.spp = 8;
  rs.batch = 8;
  rs.uniform_sky = true;
  rs.pixel_center = true;
  ReferenceFrame traced;
  REQUIRE_MESSAGE(reference.render(frame.camera, rs, traced, &error), error);
  REQUIRE(traced.width == k_size);
  // The background: a pixel the quad covers in neither picture (the frame's corner).
  REQUIRE_FALSE(covered(raster, 0));
  const f32* sky = &traced.hdr[0];
  u32 holes = 0;
  u32 surface = 0;
  u32 wrong = 0;
  for (u32 p = 0; p < k_size * k_size; ++p) {
    const f32* px = &traced.hdr[p * 4];
    const bool background = px[0] == sky[0] && px[1] == sky[1] && px[2] == sky[2];
    if (covered(raster, p)) {
      ++surface;
      if (background) ++wrong;
    } else {
      ++holes;
      if (!background) ++wrong;
    }
  }
  MESSAGE("reference: " << surface << " surface pixels, " << holes << " background or hole pixels, "
                        << wrong << " disagreeing");
  CHECK(surface > 1000);
  CHECK(wrong <= 4);
  reference.destroy();
  renderer.destroy();
  scene.destroy();
  device.destroy();
}

// ---- a shading rate by region (R126; renderer.md, "A shading rate by region") ------------------
namespace {

// The half-rate tunables a case sets, put back to their defaults however it ends.
struct HalfRateRestore {
  ~HalfRateRestore() {
    (void)tunables::apply_overrides("renderer.resolve.half_rate_sides=-1");
    (void)tunables::apply_overrides("renderer.resolve.half_rate_centre=false");
  }
};

// The sides' tunable at exactly the default's third (1/3 as a float), so an explicit third draws
// what the default does.
constexpr const char* k_third = "0.3333333432674408";

// From just under the masked quad, looking almost straight down at its shadow: the ground (one
// pair) fills the frame and the mask's checker of shadow crosses every third of it.
ShotView half_rate_low_view() {
  ShotView v;
  v.eye = Vec3{-0.5f, 0.9f, -0.56f};
  v.target = Vec3{-0.5f, 0.0f, -0.5f};
  v.mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  return v;
}

// The masked caster under the sun's rays, shaded, `width` x k_size from `view`, captured once per
// entry of `sides` (the sides' tunable as text) in order, through one renderer: the frames after
// the first are drawn by a renderer that has drawn the ones before.
std::vector<CapturedFrame> shoot_half_rate(const std::string& mesh, u32 width, const ShotView& view,
                                           std::initializer_list<const char*> sides,
                                           std::string& skip) {
  std::vector<CapturedFrame> out;
  gfx::Device device;
  std::string error;
  if (!device.create(renderer_test::device_options(), &error)) {
    skip = "device unavailable: " + error;
    return out;
  }
  SceneData data;
  GpuScene scene;
  SceneRenderer renderer;
  SceneDesc desc;
  desc.meshes.push_back(mesh);
  desc.cache = false;
  RenderSettings settings = base_settings(RasterMode::Hardware);
  settings.shadows = ShadowMode::RayTraced;
  settings.view_mode = view.mode;
  REQUIRE_MESSAGE(load_scene(desc, data, error), "scene: " << error);
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  const RenderAvailability availability = check_availability(resolved, device.features());
  if (availability != RenderAvailability::Ok || !resolved.shadows) {
    skip = availability != RenderAvailability::Ok
               ? "unavailable here: " + unavailable_reason(availability, device)
               : std::string("no ray-traced shadows on this device");
    device.destroy();
    return out;
  }
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), "gpu scene: " << error);
  SceneRenderer::Desc rd;
  rd.width = width;
  rd.height = k_size;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), "renderer: " << error);
  FrameDesc frame;
  frame.camera.position = absolute(WorldPos::origin(), view.eye);
  frame.camera.target = absolute(WorldPos::origin(), view.target);
  frame.camera.fov_y = k_fov;
  frame.view_mode = view.mode;
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  for (const char* s : sides) {
    REQUIRE(tunables::apply_overrides(std::string("renderer.resolve.half_rate_sides=") + s));
    CapturedFrame captured;
    REQUIRE_MESSAGE(renderer.capture(frame, channels, captured, &error), error);
    out.push_back(std::move(captured));
  }
  renderer.destroy();
  scene.destroy();
  device.destroy();
  return out;
}

bool same_pixel(const CapturedFrame& a, const CapturedFrame& b, u32 p) {
  for (u32 c = 0; c < 4; ++c)
    if (a.color[p * 4 + c] != b.color[p * 4 + c]) return false;
  return true;
}

// The pair a pixel holds: its instance and cluster.
bool same_pair(const CapturedFrame& f, u32 p, u32 q) {
  return f.ids[p * k_id_words] == f.ids[q * k_id_words] &&
         f.ids[p * k_id_words + 1] == f.ids[q * k_id_words + 1];
}

// How a half-rate picture differs from the full-rate one, pixel by pixel, sorted by where.
struct HalfRateDiff {
  u32 centre = 0;    // differing pixels in the full-rate columns
  u32 mixed = 0;     // in a half-rate block whose four pixels are not one covered pair
  u32 top_left = 0;  // a one-pair block's top-left pixel, whose own ray the block took
  u32 taken = 0;     // a one-pair block's other three: the shading rule's own change
  u32 one_pair_blocks = 0;
  u32 mixed_blocks = 0;
};

HalfRateDiff half_rate_diff(const CapturedFrame& full, const CapturedFrame& half, f32 sides) {
  HalfRateDiff d;
  const u32 w = full.width;
  const gfx::HalfRateWindow window = gfx::half_rate_window(sides, false, w, 0, w, w);
  for (u32 by = 0; by < (k_size + 1) / 2; ++by) {
    for (u32 bx = 0; bx < (w + 1) / 2; ++bx) {
      const u32 x = bx * 2;
      const u32 y = by * 2;
      const bool half_rate = x < window.x0 || x >= window.x1;
      const u32 p[4] = {y * w + x, y * w + x + 1, (y + 1) * w + x, (y + 1) * w + x + 1};
      bool one_pair = x + 1 < w && y + 1 < k_size;
      for (u32 i = 0; one_pair && i < 4; ++i)
        one_pair = covered(full, p[i]) && same_pair(full, p[0], p[i]);
      if (half_rate) ++(one_pair ? d.one_pair_blocks : d.mixed_blocks);
      for (u32 i = 0; i < 4; ++i) {
        if (p[i] >= w * k_size || same_pixel(full, half, p[i])) continue;
        if (!half_rate) {
          ++d.centre;
        } else if (!one_pair) {
          ++d.mixed;
        } else if (i == 0) {
          ++d.top_left;
        } else {
          ++d.taken;
        }
      }
    }
  }
  return d;
}

}  // namespace

// (a) and (c): the rule off draws the full-rate bytes, on a frame before and after one drawn at
// half rate; a surround's default is its sides and a narrower frame's is nothing.
TEST_CASE("half rate: off is the full-rate picture, and the default is the sides of a surround") {
  Fixture fx;
  HalfRateRestore restore;
  std::string skip;
  // 3:1, a surround: the default's thirds are on.
  const std::vector<CapturedFrame> wide = shoot_half_rate(
      fx.caster_masked, 3 * k_size, half_rate_low_view(), {"0", k_third, "0", "-1"}, skip);
  if (!skip.empty()) {
    MESSAGE("skipped: " << skip);
    return;
  }
  REQUIRE(wide.size() == 4);
  CHECK(wide[0].color == wide[2].color);        // off after a frame at half rate: the same bytes
  CHECK_FALSE(wide[0].color == wide[1].color);  // the witness: the thirds changed something
  CHECK(wide[3].color == wide[1].color);        // the default is the thirds
  // 2:1: the default is off.
  const std::vector<CapturedFrame> narrow = shoot_half_rate(
      fx.caster_masked, 2 * k_size, half_rate_low_view(), {"0", "-1", k_third}, skip);
  REQUIRE(narrow.size() == 3);
  CHECK(narrow[1].color == narrow[0].color);
  CHECK_FALSE(narrow[2].color == narrow[0].color);  // the thirds would have changed it
}

// (b): at a third a side the centre is the full-rate picture to the byte, and a side pixel differs
// only where its 2x2 block holds one pair and it is not the block's top-left pixel, whose ray the
// block took — so a silhouette, a mask's edge and a material's boundary are drawn at full rate.
TEST_CASE(
    "half rate: the sides change only the pixels of one-pair blocks that took another's ray") {
  Fixture fx;
  HalfRateRestore restore;
  const f32 third = 1.0f / 3.0f;
  std::string skip;
  // From under the quad: the ground's shadow, one pair in nearly every block.
  const std::vector<CapturedFrame> low =
      shoot_half_rate(fx.caster_masked, k_size, half_rate_low_view(), {"0", k_third}, skip);
  if (!skip.empty()) {
    MESSAGE("skipped: " << skip);
    return;
  }
  REQUIRE(low.size() == 2);
  // From above, the masked quad over its shadow: blocks across the mask's edges hold two pairs.
  ShotView above;
  above.eye = Vec3{-2.5f, 4.0f, -2.5f};
  above.target = Vec3{-0.4f, 0.0f, -0.4f};
  above.mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  const std::vector<CapturedFrame> slant =
      shoot_half_rate(fx.caster_masked, k_size, above, {"0", k_third}, skip);
  REQUIRE(slant.size() == 2);
  for (const std::vector<CapturedFrame>* shots : {&low, &slant}) {
    const HalfRateDiff d = half_rate_diff((*shots)[0], (*shots)[1], third);
    MESSAGE("half-rate blocks: " << d.one_pair_blocks << " of one pair, " << d.mixed_blocks
                                 << " mixed; differing pixels: " << d.taken << " taken, "
                                 << d.top_left << " top-left, " << d.mixed << " mixed, " << d.centre
                                 << " centre");
    CHECK(d.centre == 0);
    CHECK(d.mixed == 0);
    CHECK(d.top_left == 0);
  }
  CHECK(half_rate_diff(low[0], low[1], third).taken > 20);  // the witness: the rule changed it
  CHECK(half_rate_diff(slant[0], slant[1], third).mixed_blocks > 50);  // and mixed blocks exist
}

// The measurement sweep (renderer.md, "A shading rate by region"): with
// `renderer.resolve.half_rate_sweep`, frame f takes the sides' share (f / 8) % 3 — none, as set, a
// half — so a flight under `renderer.resolve.stop_after=cycle` measures every stage at each;
// without it every frame takes the tunable. CPU only.
TEST_CASE("half rate: the sweep steps the sides' share eight frames at a time") {
  HalfRateRestore restore;
  struct SweepRestore {
    ~SweepRestore() { (void)tunables::apply_overrides("renderer.resolve.half_rate_sweep=false"); }
  } sweep_restore;
  REQUIRE(tunables::apply_overrides("renderer.resolve.half_rate_sides=0.25"));
  for (u64 f : {u64{0}, u64{8}, u64{16}, u64{47}})
    CHECK(half_rate_sides_tunable(f) == 0.25f);
  REQUIRE(tunables::apply_overrides("renderer.resolve.half_rate_sweep=true"));
  for (u64 base : {u64{0}, u64{24}, u64{240}}) {
    CHECK(half_rate_sides_tunable(base) == 0.0f);
    CHECK(half_rate_sides_tunable(base + 7) == 0.0f);
    CHECK(half_rate_sides_tunable(base + 8) == 0.25f);
    CHECK(half_rate_sides_tunable(base + 16) == 0.5f);
  }
}

// **Opacity micromaps** (renderer.md, "Opacity micromaps"; R120). The scene bakes a conservative
// 4-state micromap for every masked triangle and the chain's drawn alpha-tested records name it, so
// the traversal decides a micro-triangle its mask decides the same way everywhere on it and hands
// the loops the rest. Nothing a ray loop decides changes, so the picture is the same bytes with
// micromaps and without: on the per-frame build and on the cache's, under the shadow rays and the
// ray path, and on a device that has no micromaps at all.
namespace {

struct MicromapShot {
  CapturedFrame frame;
  std::string skip;
  gfx::MicromapStats stats;
  bool baked = false;
  MicromapCacheReport cache;  // R207: baked or read back, with a derived-data root
};

struct TunableScope {
  TunableScope(const char* set, const char* restore) : restore_(restore) {
    REQUIRE(tunables::apply_overrides(set));
  }
  ~TunableScope() { (void)tunables::apply_overrides(restore_); }
  TunableScope(const TunableScope&) = delete;
  TunableScope& operator=(const TunableScope&) = delete;
  const char* restore_;
};

MicromapShot shoot_micromaps(const std::string& mesh, RenderSettings settings, ShotView view,
                             bool without_extension = false, const std::string& ddc = {},
                             const std::string& second = {}) {
  MicromapShot out;
  gfx::Device device;
  std::string error;
  gfx::DeviceOptions options = renderer_test::device_options();
  if (without_extension) options.overrides.absent.push_back("VK_EXT_opacity_micromap");
  if (!device.create(options, &error)) {
    out.skip = "device unavailable: " + error;
    return out;
  }
  if (!device.features().cluster_acceleration_structure || !device.features().ray_query) {
    out.skip = "no cluster acceleration structures and ray queries on this device";
    device.destroy();
    return out;
  }
  SceneData data;
  GpuScene scene;
  SceneRenderer renderer;
  SceneDesc desc;
  desc.meshes.push_back(mesh);
  if (!second.empty()) desc.meshes.push_back(second);  // R208: a part another load also holds
  desc.cache = !ddc.empty();  // R207: a root of the test's own, where the bake is stored
  desc.ddc = ddc;
  settings.view_mode = view.mode;
  REQUIRE_MESSAGE(load_scene(desc, data, error), "scene: " << error);
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
    out.skip = "unavailable here";
    device.destroy();
    return out;
  }
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), "gpu scene: " << error);
  SceneRenderer::Desc rd;
  rd.width = k_size;
  rd.height = k_size;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), "renderer: " << error);
  out.baked = scene.micromap_tables_address() != 0;
  out.stats = scene.micromap_stats();
  out.cache = scene.micromap_cache();
  FrameDesc frame;
  frame.camera.position = absolute(WorldPos::origin(), view.eye);
  frame.camera.target = absolute(WorldPos::origin(), view.target);
  frame.camera.fov_y = k_fov;
  frame.view_mode = view.mode;
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, out.frame, &error), error);
  renderer.destroy();
  scene.destroy();
  device.destroy();
  return out;
}

u32 differing_pixels(const CapturedFrame& a, const CapturedFrame& b) {
  REQUIRE(a.color.size() == b.color.size());
  REQUIRE(a.ids.size() == b.ids.size());
  const u32 pixels = a.width * a.height;
  const u32 id_words = pixels == 0 ? 0u : static_cast<u32>(a.ids.size() / pixels);
  u32 differ = 0;
  for (u32 p = 0; p < pixels; ++p) {
    bool same = std::memcmp(&a.color[p * 4], &b.color[p * 4], 4) == 0;
    for (u32 w = 0; w < id_words && same; ++w)
      same = a.ids[p * id_words + w] == b.ids[p * id_words + w];
    if (!same) ++differ;
  }
  return differ;
}

// Two captured micromap inputs (`renderer.rt.micromap_capture`) compared triangle by triangle,
// each through its own index: a special index against the other's, an entry's level, format and
// every micro-triangle's two-bit state against the other's entry, whatever the entries' numbers.
struct StateDiff {
  u32 triangles = 0;        // the masked triangles compared
  u32 triangles_apart = 0;  // any difference at all
  u32 special_apart = 0;    // a special index against another special index or an entry
  u32 level_apart = 0;      // both entries, another level or format
  u64 micro_compared = 0;
  u64 micro_apart = 0;  // both entries at one level: micro-triangles of another state
  u32 first_apart = ~0u;
};
StateDiff compare_states(const MicromapCacheReport& a, const MicromapCacheReport& b) {
  StateDiff d;
  REQUIRE(a.captured_indices.size() == b.captured_indices.size());
  d.triangles = static_cast<u32>(a.captured_indices.size());
  for (u32 t = 0; t < d.triangles; ++t) {
    const i32 ia = a.captured_indices[t];
    const i32 ib = b.captured_indices[t];
    bool apart = false;
    if (ia < 0 || ib < 0) {
      apart = ia != ib;
      d.special_apart += apart ? 1u : 0u;
    } else {
      REQUIRE(static_cast<u32>(ia) < a.captured_triangles.size());
      REQUIRE(static_cast<u32>(ib) < b.captured_triangles.size());
      const gfx::MicromapTriangle& ra = a.captured_triangles[static_cast<u32>(ia)];
      const gfx::MicromapTriangle& rb = b.captured_triangles[static_cast<u32>(ib)];
      if (ra.level != rb.level || ra.format != rb.format) {
        apart = true;
        ++d.level_apart;
      } else {
        const u64 states = u64{1} << (2 * ra.level);
        const u32* wa = a.captured_data.data() + ra.data_offset / 4;
        const u32* wb = b.captured_data.data() + rb.data_offset / 4;
        for (u64 s = 0; s < states; ++s) {
          const u32 sa = (wa[s / 16] >> (2 * (s % 16))) & 3u;
          const u32 sb = (wb[s / 16] >> (2 * (s % 16))) & 3u;
          ++d.micro_compared;
          if (sa != sb) {
            ++d.micro_apart;
            apart = true;
          }
        }
      }
    }
    if (apart) {
      ++d.triangles_apart;
      if (d.first_apart == ~0u) d.first_apart = t;
    }
  }
  return d;
}

ShotView micromap_shadow_view() {
  ShotView view;
  view.eye = Vec3{-2.5f, 4.0f, -2.5f};
  view.target = Vec3{-0.4f, 0.0f, -0.4f};
  view.mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  return view;
}

}  // namespace

TEST_CASE("alpha mask: opacity micromaps leave the shadow rays' and the ray path's picture alone") {
  Fixture fx;
  RenderSettings rays = base_settings(RasterMode::Hardware);
  rays.shadows = ShadowMode::RayTraced;
  RenderSettings traced = base_settings(RasterMode::RayTrace);
  traced.shadows = ShadowMode::RayTraced;
  ShotView traced_view;
  traced_view.mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  struct Case {
    const char* name;
    std::string mesh;
    RenderSettings settings;
    ShotView view;
    const char* cache;
  };
  const Case cases[] = {
      {"shadow rays, per-frame build", fx.caster_masked, rays, micromap_shadow_view(),
       "renderer.rt.cache=false"},
      {"shadow rays, the cache", fx.caster_masked, rays, micromap_shadow_view(),
       "renderer.rt.cache=true"},
      {"ray path, per-frame build", fx.masked, traced, traced_view, "renderer.rt.cache=false"},
      {"ray path, the cache", fx.masked, traced, traced_view, "renderer.rt.cache=true"},
  };
  // Micro-triangles an eighth of a texel across (level 6): the quad maps the 8x8 mask exactly, so
  // every micro-triangle edge falls on a texel boundary and a coarser one always reads two blocks.
  TunableScope fine("renderer.rt.micromap_texels=0.01", "renderer.rt.micromap_texels=4");
  TunableScope cap("renderer.rt.micromap_level=6", "renderer.rt.micromap_level=6");
  for (const Case& c : cases) {
    TunableScope cache(c.cache, "renderer.rt.cache=true");
    // Four states pinned: the shadow rays' two-state answer (on by default since R120 batch 53) is
    // meant to move a shadow edge's band, which the case below bounds; this one asserts nothing
    // moves.
    TunableScope four("renderer.shadow.micromap_2state=false",
                      "renderer.shadow.micromap_2state=true");
    MicromapShot on;
    {
      TunableScope set("renderer.shadow.micromaps=true", "renderer.shadow.micromaps=true");
      on = shoot_micromaps(c.mesh, c.settings, c.view);
    }
    if (!on.skip.empty()) {
      MESSAGE(c.name << " skipped: " << on.skip);
      continue;
    }
    MicromapShot off;
    {
      TunableScope set("renderer.shadow.micromaps=false", "renderer.shadow.micromaps=true");
      off = shoot_micromaps(c.mesh, c.settings, c.view);
    }
    REQUIRE(off.skip.empty());
    CHECK_FALSE(off.baked);
    if (!on.baked) {
      MESSAGE(c.name << ": no micromaps on this device; nothing to compare");
      continue;
    }
    // The bake decided something: the quad's two triangles keep data, and the checker leaves
    // micro-triangles of every kind — known ones the traversal settles, unknown ones along the
    // blocks' edges, which the loops still ask.
    CHECK(on.stats.triangles == 2);
    CHECK(on.stats.micro_opaque > 0);
    CHECK(on.stats.micro_transparent > 0);
    CHECK(on.stats.micro_unknown > 0);
    const u32 differ = differing_pixels(on.frame, off.frame);
    MESSAGE(c.name << ": " << on.stats.micro_opaque << " opaque, " << on.stats.micro_transparent
                   << " transparent and " << on.stats.micro_unknown
                   << " unknown micro-triangles at level cap " << on.stats.level_cap << "; "
                   << differ << " pixels differ");
    CHECK(differ == 0);
  }
}

// **A miss draws what a hit draws** (R207-M1; renderer.md, "The bake is read back"). A
// container keeps a material's alpha cutoff as a unorm16 and its foliage optics in fifteenths, so a
// glTF's 0.5 reads back 0.50000763: the load that built the container drew, and keyed the opacity
// micromaps' bake, with the import's floats, and every later load with the container's. The miss
// path now takes the container's words (`assets::round_trip_alpha_word`), and the two loads' five
// constants are the same bits. Before that mend the masked quad's material counted one word apart
// (`import_apart` below is that count, taken from the import itself). No device.
TEST_CASE("alpha mask: the load that builds the container shades with the words a hit reads") {
  Fixture fx;
  SceneDesc desc;
  desc.ddc = fx.tmp.path() + "/ddc_words";
  desc.cache = true;
  desc.meshes.push_back(fx.masked);
  desc.instances.push_back(SceneInstance{0, Transform3::identity()});
  std::string error;
  SceneData miss;
  REQUIRE_MESSAGE(load_scene(desc, miss, error), error);
  SceneData hit;
  REQUIRE_MESSAGE(load_scene(desc, hit, error), error);
  REQUIRE(miss.sources.size() == 1);
  REQUIRE(hit.sources.size() == 1);
  CHECK(std::string(miss.sources[0].cache) == "miss");
  CHECK(std::string(hit.sources[0].cache) == "hit");
  assets::MeshData import;
  REQUIRE_MESSAGE(assets::load_gltf(fx.masked, import, &error), error);
  const auto& a = miss.sources[0].data.materials;
  const auto& b = hit.sources[0].data.materials;
  REQUIRE(a.size() == b.size());
  REQUIRE(import.materials.size() == b.size());
  auto bits = [](f32 v) {
    u32 out = 0;
    std::memcpy(&out, &v, sizeof(out));
    return out;
  };
  auto apart = [&](const assets::Material& x, const assets::Material& y) {
    return x.alpha_mode != y.alpha_mode || x.double_sided != y.double_sided ||
           bits(x.alpha_cutoff) != bits(y.alpha_cutoff) ||
           bits(x.transmission) != bits(y.transmission) ||
           bits(x.normal_spread) != bits(y.normal_spread);
  };
  u32 masked = 0;
  u32 miss_apart = 0;
  u32 import_apart = 0;
  for (u32 i = 0; i < b.size(); ++i) {
    masked += b[i].alpha_mode == assets::k_alpha_mask ? 1u : 0u;
    miss_apart += apart(a[i], b[i]) ? 1u : 0u;
    import_apart += apart(import.materials[i], b[i]) ? 1u : 0u;
  }
  MESSAGE(masked << " masked material(s); the import " << import_apart
                 << " apart from the hit's words, the miss " << miss_apart);
  CHECK(masked == 1);
  CHECK(import_apart == 1);  // the word is what moved: 0.5 is not a unorm16
  CHECK(miss_apart == 0);
}

// **The bake is read back** (R207; renderer.md, "The bake is read back"; micromap_cache.h). With a
// derived-data root the first load bakes and stores its sections under a key that names the device
// and everything the bake read; the second finds them and builds the micromap with no bake. The
// two draw the same bytes, colour and ids, under the shadow rays' two-state answer (the default,
// which reads the unknown sub-state) and on the ray path.
TEST_CASE(
    "alpha mask: a second load reads the micromaps the first baked, and draws the same bytes") {
  Fixture fx;
  test::TempDir root{"omm_read"};
  RenderSettings rays = base_settings(RasterMode::Hardware);
  rays.shadows = ShadowMode::RayTraced;
  RenderSettings traced = base_settings(RasterMode::RayTrace);
  traced.shadows = ShadowMode::RayTraced;
  ShotView traced_view;
  traced_view.mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  struct Case {
    const char* name;
    std::string mesh;
    RenderSettings settings;
    ShotView view;
  };
  const Case cases[] = {
      {"shadow rays", fx.caster_masked, rays, micromap_shadow_view()},
      {"ray path", fx.masked, traced, traced_view},
  };
  TunableScope on("renderer.shadow.micromaps=true", "renderer.shadow.micromaps=true");
  TunableScope fine("renderer.rt.micromap_texels=0.01", "renderer.rt.micromap_texels=4");
  TunableScope cap("renderer.rt.micromap_level=6", "renderer.rt.micromap_level=6");
  // The fixtures are a few triangles: stored whatever the store's floor is set to.
  TunableScope floor("renderer.rt.micromap_store_min=0", "renderer.rt.micromap_store_min=0");
  u32 index = 0;
  for (const Case& c : cases) {
    const std::string ddc = root.path() + "/ddc" + std::to_string(index++);
    // An empty root's first load builds the mesh, its textures and the bake into it; the second
    // reads all three. The two key alike because the load that built the container shades with
    // the container's own alpha words (R207-M1; the case below holds the words).
    const MicromapShot first = shoot_micromaps(c.mesh, c.settings, c.view, false, ddc);
    if (!first.skip.empty()) {
      MESSAGE(c.name << " skipped: " << first.skip);
      continue;
    }
    if (!first.baked) {
      MESSAGE(c.name << ": no micromaps on this device; nothing to read back");
      continue;
    }
    CHECK(first.cache.parts == 1);
    CHECK(first.cache.baked == 1);
    CHECK(first.cache.read == 0);
    CHECK(first.cache.generation != 0);
    CHECK(first.cache.unkeyed.empty());
    CHECK(first.cache.stored);
    CHECK(std::filesystem::exists(first.cache.path));
    const MicromapShot second = shoot_micromaps(c.mesh, c.settings, c.view, false, ddc);
    REQUIRE(second.skip.empty());
    MESSAGE(c.name << ": the empty root's load baked " << first.cache.baked << " part(s) into "
                   << first.cache.path << ", the next read " << second.cache.read);
    CHECK(second.cache.baked == 0);
    CHECK(second.cache.read == 1);
    CHECK(second.cache.packs_read == 1);
    CHECK_FALSE(second.cache.stored);
    CHECK(second.cache.generation == first.cache.generation);
    CHECK(second.cache.refused.empty());
    CHECK(second.baked);
    // The bake's measures come back with it, and the micromap is the same size.
    CHECK(second.stats.triangles == first.stats.triangles);
    CHECK(second.stats.unique_entries == first.stats.unique_entries);
    CHECK(second.stats.data_bytes == first.stats.data_bytes);
    CHECK(second.stats.micro_unknown == first.stats.micro_unknown);
    CHECK(second.stats.level_cap == first.stats.level_cap);
    CHECK(second.stats.storage_bytes == first.stats.storage_bytes);
    const u32 differ = differing_pixels(first.frame, second.frame);
    MESSAGE(c.name << ": baked " << first.cache.ms << " ms, read " << second.cache.ms << " ms, "
                   << first.cache.stored_bytes << " bytes stored; " << differ << " pixels differ");
    CHECK(differ == 0);
  }
}

// **A stored pack is never trusted without its keys** (R207, R208): a key the bake read differently
// (here the texel target) is another part, baked and stored as a pack beside the first; a pack at
// the first's path that holds another part's bake is read for nothing (the part it was asked for
// is not in it); one made for another generation or version, a short one or a changed byte is
// refused by name; a sound pack whose part has the scene part's key and other triangles is a miss
// (R208-M2: never a read past the part); each time the part is baked again, the picture is the
// bake's, and the pack the bake stores takes the spoiled one's place.
TEST_CASE("alpha mask: a stored micromap pack is refused for another key, version or byte") {
  Fixture fx;
  test::TempDir root{"omm_stale"};
  RenderSettings rays = base_settings(RasterMode::Hardware);
  rays.shadows = ShadowMode::RayTraced;
  TunableScope on("renderer.shadow.micromaps=true", "renderer.shadow.micromaps=true");
  TunableScope cap("renderer.rt.micromap_level=6", "renderer.rt.micromap_level=6");
  TunableScope floor("renderer.rt.micromap_store_min=0", "renderer.rt.micromap_store_min=0");
  const std::string ddc = root.path() + "/ddc";
  auto shoot = [&] {
    return shoot_micromaps(fx.caster_masked, rays, micromap_shadow_view(), false, ddc);
  };
  MicromapShot first;
  {
    TunableScope fine("renderer.rt.micromap_texels=0.01", "renderer.rt.micromap_texels=4");
    first = shoot();
  }
  if (!first.skip.empty() || !first.baked) {
    MESSAGE("skipped: " << (first.skip.empty() ? std::string("no micromaps here") : first.skip));
    return;
  }
  REQUIRE(first.cache.stored);
  const std::filesystem::path stored(first.cache.path);
  // Another texel target is another part key: baked, and stored as a pack beside the first.
  MicromapShot other;
  {
    TunableScope coarse("renderer.rt.micromap_texels=0.02", "renderer.rt.micromap_texels=4");
    other = shoot();
  }
  CHECK(other.cache.baked == 1);
  CHECK(other.cache.read == 0);
  CHECK(other.cache.path != first.cache.path);
  REQUIRE(other.cache.stored);
  TunableScope fine("renderer.rt.micromap_texels=0.01", "renderer.rt.micromap_texels=4");
  struct Spoil {
    const char* name;
    const char* refused;  // a word the refusal names; empty: the pack is sound and not asked for
  };
  const Spoil spoils[] = {{"another part's bake", ""},
                          {"another generation", "another generation"},
                          {"another version", "version"},
                          {"a short file", "length"},
                          {"a changed state", "content hash"},
                          {"a colliding key over other content", "triangle count"}};
  auto get_u64 = [](const std::string& b, usize at) {
    u64 v = 0;
    for (u32 i = 0; i < 8; ++i)
      v |= u64{static_cast<u8>(b[at + i])} << (8 * i);
    return v;
  };
  for (const Spoil& spoil : spoils) {
    // The first key's pack as the last bake stored it, spoiled.
    std::ifstream in(stored, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    REQUIRE(bytes.size() > 64);
    const std::string_view name(spoil.name);
    if (name == "another part's bake") {
      std::ifstream theirs(std::filesystem::path(other.cache.path), std::ios::binary);
      bytes.assign((std::istreambuf_iterator<char>(theirs)), std::istreambuf_iterator<char>());
    } else if (name == "another generation") {
      bytes[8] = static_cast<char>(bytes[8] ^ 0x01);
    } else if (name == "another version") {
      bytes[4] = static_cast<char>(bytes[4] + 1);
    } else if (name == "a short file") {
      bytes.resize(bytes.size() - 4);
    } else if (name == "a colliding key over other content") {
      // R208-M2: a sound pack of this generation whose one part has the scene part's key and
      // other triangles (a 64-bit collision, or a pack made to look like one): a miss, baked,
      // never read past the part.
      const u64 generation = get_u64(bytes, 8);
      const u64 key = get_u64(bytes, k_micromap_pack_fixed_bytes);
      const u32 stored_triangles =
          static_cast<u32>(get_u64(bytes, k_micromap_pack_fixed_bytes + 8) & 0xffffffffu);
      MicromapPackPart part;
      part.key = key;
      part.triangle_count = stored_triangles > 1 ? stored_triangles - 1 : stored_triangles + 1;
      const std::vector<i32> indices(part.triangle_count, gfx::k_micromap_fully_opaque);
      MicromapPackView forged;
      forged.parts = std::span<const MicromapPackPart>(&part, 1);
      forged.indices = std::span<const i32>(indices.data(), indices.size());
      const std::string at = root.path() + "/forged.omm";
      std::string error;
      REQUIRE_MESSAGE(write_micromap_pack(at, generation, forged, &error), error);
      std::ifstream theirs(std::filesystem::path(at), std::ios::binary);
      bytes.assign((std::istreambuf_iterator<char>(theirs)), std::istreambuf_iterator<char>());
    } else {
      bytes[bytes.size() - 1] = static_cast<char>(bytes[bytes.size() - 1] ^ 0x01);
    }
    {
      std::ofstream out(stored, std::ios::binary | std::ios::trunc);
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    const MicromapShot again = shoot();
    INFO(spoil.name << ": refused '" << again.cache.refused << "'");
    CHECK(again.cache.read == 0);
    CHECK(again.cache.baked == 1);
    if (spoil.refused[0] != '\0')
      CHECK(again.cache.refused.find(spoil.refused) != std::string::npos);
    else
      CHECK(again.cache.refused.empty());
    CHECK(differing_pixels(first.frame, again.frame) == 0);
    // The bake it fell back to is stored at the spoiled pack's path.
    REQUIRE(again.cache.stored);
    CHECK(again.cache.path == first.cache.path);
  }
  // And the last of those is read by the next load, and the other texel target's pack still is.
  const MicromapShot after = shoot();
  CHECK(after.cache.read == 1);
  CHECK(after.cache.baked == 0);
  CHECK(differing_pixels(first.frame, after.frame) == 0);
  CHECK(std::filesystem::exists(std::filesystem::path(other.cache.path)));
}

// **A start elsewhere reads what it shares and bakes only what is new** (R208; renderer.md, "The
// bake is read back"). In a streamed world what a load holds follows where it starts; here the
// first load holds one masked mesh and the second that mesh and another, as a second start would
// hold the tiles it shares with the first and some of its own. The second bakes only the part the
// first did not store (a count: one part read, the rest baked, one pack read and one stored), and
// draws what a load with no store draws, to the byte; a third holding either reads both.
TEST_CASE("alpha mask: a second start reads the parts the first stored and bakes only the rest") {
  Fixture fx;
  test::TempDir root{"omm_parts"};
  RenderSettings rays = base_settings(RasterMode::Hardware);
  rays.shadows = ShadowMode::RayTraced;
  TunableScope on("renderer.shadow.micromaps=true", "renderer.shadow.micromaps=true");
  TunableScope cap("renderer.rt.micromap_level=6", "renderer.rt.micromap_level=6");
  TunableScope fine("renderer.rt.micromap_texels=0.01", "renderer.rt.micromap_texels=4");
  TunableScope floor("renderer.rt.micromap_store_min=0", "renderer.rt.micromap_store_min=0");
  const std::string ddc = root.path() + "/ddc";
  const ShotView view = micromap_shadow_view();
  const MicromapShot first = shoot_micromaps(fx.caster_masked, rays, view, false, ddc);
  if (!first.skip.empty() || !first.baked) {
    MESSAGE("skipped: " << (first.skip.empty() ? std::string("no micromaps here") : first.skip));
    return;
  }
  REQUIRE(first.cache.stored);
  CHECK(first.cache.parts == 1);
  CHECK(first.cache.baked == 1);
  const MicromapShot second = shoot_micromaps(fx.caster_masked, rays, view, false, ddc, fx.masked);
  const MicromapShot unstored = shoot_micromaps(fx.caster_masked, rays, view, false, {}, fx.masked);
  MESSAGE("the second start: " << second.cache.parts << " parts, " << second.cache.read
                               << " read from " << second.cache.packs_read << " pack(s), "
                               << second.cache.baked << " baked, stored " << second.cache.stored);
  CHECK(second.cache.parts == 2);
  CHECK(second.cache.read == 1);   // the first start's part
  CHECK(second.cache.baked == 1);  // only the new one
  CHECK(second.cache.packs_read == 1);
  CHECK(second.cache.stored);
  CHECK(second.cache.path != first.cache.path);
  CHECK(differing_pixels(second.frame, unstored.frame) == 0);
  CHECK(second.stats.triangles == unstored.stats.triangles);
  CHECK(second.stats.micro_unknown == unstored.stats.micro_unknown);
  CHECK(second.stats.micro_opaque == unstored.stats.micro_opaque);
  // A third start holding both reads both, from the two packs, and bakes nothing.
  const MicromapShot third = shoot_micromaps(fx.masked, rays, view, false, ddc, fx.caster_masked);
  CHECK(third.cache.parts == 2);
  CHECK(third.cache.read == 2);
  CHECK(third.cache.baked == 0);
  CHECK(third.cache.packs_read == 2);
  CHECK_FALSE(third.cache.stored);
}

// **A scene over its budget is read back at the lowered cap** (R208-M1; renderer.md, "The bake is
// read back"). The budget is held to the assembled micromap, so a load whose micromap at the top
// cap is over it lowers the cap and tries again: the first load bakes and stores at both caps, and
// every later load reads the top cap's pack to find it over (its sections freed before the next
// cap is tried), then the lowered cap's, and bakes nothing. The budget is the setting's grain, a
// MiB, under a grid whose micromap is over it at the top cap (2.9 MB, a premise measured by a load
// of its own) and under it a level down, so exactly one level drops. The keyed loads are held to
// an unkeyed bake on their own load path (a derived-data root, the store's floor above the scene):
// the second load draws the first's picture and the unkeyed bake's, to the byte; at the lowered
// cap reached directly, the keyed micromap's states equal the bake's triangle by triangle, through
// each one's own index (`renderer.rt.micromap_capture`), and the two draw the same bytes.
//
// A load with no derived-data root is another load path, not another micromap: it imports the
// glTF and decodes the PNG where a rooted load reads the container and the block-compressed
// texture built from it. On this noise it draws one pixel apart from the rooted loads on the 5090
// with every micromap state equal (R208's mend, 2026-11-24: keyed against unkeyed on one root, 0
// pixels; the same states with no root, 1), so the case prints that count and asserts the states.
TEST_CASE(
    "alpha mask: a micromap over its budget is read back at the lowered cap, baking nothing") {
  test::TempDir root{"omm_budget"};
  const std::filesystem::path dir(root.path());
  REQUIRE(write_noise_grid(dir, "grid.gltf", 28));  // 1,568 triangles of their own states
  const std::string grid = (dir / "grid.gltf").string();
  RenderSettings traced = base_settings(RasterMode::RayTrace);
  traced.shadows = ShadowMode::RayTraced;
  ShotView view;
  view.mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  TunableScope on("renderer.shadow.micromaps=true", "renderer.shadow.micromaps=true");
  TunableScope cap("renderer.rt.micromap_level=6", "renderer.rt.micromap_level=6");
  TunableScope fine("renderer.rt.micromap_texels=0.01", "renderer.rt.micromap_texels=4");
  TunableScope floor("renderer.rt.micromap_store_min=0", "renderer.rt.micromap_store_min=0");
  // The top cap's assembled size, from a stored load at the default budget into a root of its own.
  const MicromapShot measure = shoot_micromaps(grid, traced, view, false, root.path() + "/measure");
  if (!measure.skip.empty() || !measure.baked) {
    MESSAGE(
        "skipped: " << (measure.skip.empty() ? std::string("no micromaps here") : measure.skip));
    return;
  }
  REQUIRE(measure.stats.level_cap == 6);
  REQUIRE(measure.stats.levels_dropped == 0);
  const u64 top = measure.stats.data_bytes + measure.stats.index_bytes +
                  u64{measure.stats.unique_entries} * gfx::k_micromap_triangle_bytes;
  const u64 budget = u64{1} << 20;
  REQUIRE(top > budget);  // the premise: the top cap is over a MiB
  TunableScope small("renderer.rt.micromap_mib=1", "renderer.rt.micromap_mib=1024");
  const std::string ddc = root.path() + "/ddc";
  // An unkeyed bake on the keyed loads' load path: a root of its own, the floor above the scene.
  auto unkeyed_shot = [&] {
    TunableScope above("renderer.rt.micromap_store_min=1073741824",
                       "renderer.rt.micromap_store_min=0");
    return shoot_micromaps(grid, traced, view, false, root.path() + "/unkeyed");
  };
  const MicromapShot first = shoot_micromaps(grid, traced, view, false, ddc);
  const MicromapShot second = shoot_micromaps(grid, traced, view, false, ddc);
  const MicromapShot unstored = unkeyed_shot();
  MESSAGE("the top cap assembles to "
          << top << " bytes; budget " << budget << ": the first load " << first.cache.baked
          << " baked, cap " << first.stats.level_cap << "; the second read " << second.cache.read
          << " from " << second.cache.packs_read << " pack(s) (" << second.cache.read_bytes
          << " bytes), " << second.cache.baked << " baked, cap " << second.stats.level_cap);
  // The first load drops a level, bakes the part at each cap and stores both packs.
  CHECK(first.cache.levels_dropped == 1);
  CHECK(first.stats.level_cap == 5);
  CHECK(first.cache.baked == 1);
  CHECK(first.cache.stored);
  // The second reports the lowered cap and bakes nothing: the top cap's pack read and found over,
  // then the lowered cap's.
  CHECK(second.cache.levels_dropped == 1);
  CHECK(second.stats.level_cap == 5);
  CHECK(second.stats.levels_dropped == 1);
  CHECK(second.cache.baked == 0);
  CHECK(second.cache.read == 1);
  CHECK(second.cache.packs_read == 2);
  CHECK_FALSE(second.cache.stored);
  CHECK(second.cache.refused.empty());
  // The unkeyed bake holds the same budget to its own entries and lands on the same cap.
  CHECK_FALSE(unstored.cache.unkeyed.empty());
  CHECK(unstored.cache.read == 0);
  CHECK_FALSE(unstored.cache.stored);
  CHECK(unstored.stats.level_cap == 5);
  CHECK(unstored.stats.levels_dropped == 1);
  CHECK(second.stats.data_bytes == first.stats.data_bytes);
  CHECK(second.stats.unique_entries == unstored.stats.unique_entries);
  CHECK(second.stats.micro_unknown == unstored.stats.micro_unknown);
  CHECK(differing_pixels(first.frame, second.frame) == 0);
  CHECK(differing_pixels(second.frame, unstored.frame) == 0);
  // The lowered cap reached directly, keyed and unkeyed, each micromap's input captured: the budget
  // path draws what the cap draws, and the keyed micromap is the bake's, state for state.
  TunableScope big("renderer.rt.micromap_mib=1024", "renderer.rt.micromap_mib=1");
  TunableScope five("renderer.rt.micromap_level=5", "renderer.rt.micromap_level=6");
  TunableScope capture("renderer.rt.micromap_capture=true", "renderer.rt.micromap_capture=false");
  const MicromapShot keyed = shoot_micromaps(grid, traced, view, false, root.path() + "/direct");
  const MicromapShot unkeyed = unkeyed_shot();
  CHECK(keyed.stats.levels_dropped == 0);
  CHECK(keyed.cache.unkeyed.empty());
  CHECK_FALSE(unkeyed.cache.unkeyed.empty());
  CHECK(differing_pixels(second.frame, keyed.frame) == 0);
  CHECK(differing_pixels(unstored.frame, unkeyed.frame) == 0);
  const StateDiff states = compare_states(keyed.cache, unkeyed.cache);
  MESSAGE("states by triangle, keyed against unkeyed at cap 5: "
          << states.triangles << " triangles, " << states.triangles_apart << " apart (special "
          << states.special_apart << ", level or format " << states.level_apart
          << "), micro-triangles " << states.micro_apart << " of " << states.micro_compared
          << " apart; entries " << keyed.cache.captured_triangles.size() << "/"
          << unkeyed.cache.captured_triangles.size());
  CHECK(states.triangles > 0);
  CHECK(states.micro_compared > 0);
  CHECK(states.triangles_apart == 0);
  CHECK(states.micro_apart == 0);
  CHECK(differing_pixels(keyed.frame, unkeyed.frame) == 0);
  // A load with no root: the same states, another load path (above); its pixels printed.
  const MicromapShot bare = shoot_micromaps(grid, traced, view, false, {});
  const StateDiff bare_states = compare_states(keyed.cache, bare.cache);
  CHECK(bare_states.triangles_apart == 0);
  CHECK(bare_states.micro_apart == 0);
  MESSAGE("a load with no derived-data root, the same states: "
          << differing_pixels(keyed.frame, bare.frame) << " pixel(s) from the rooted loads");
}

// The micromaps are really in the structures: `renderer.shadow.opaque_rays` forces every caster
// solid, which a micromap's transparent micro-triangle is not — the traversal never reports it,
// forced or not — so with micromaps the forced shadow loses the bake's transparent micro-triangles,
// and without them it is the opaque quad's. The one picture that is meant to move, and by how much
// is the bake's own count: the micro-triangles of one level are of one area, so the shadow shrinks
// by the transparent share of them (576 of 8,192 here, 7.0 %; measured 6.9 %), within half of it
// either way for the pixels along the shadow's edge.
// It failed from R120's first commit until the pipelines of a device with micromaps were keyed
// apart (device.cpp, `micromap_key`): the driver handed the resolve a compile it had made on a
// device without the extension, whose ray queries trace every micromap as absent (R120b).
TEST_CASE("alpha mask: a forced-solid shadow ray still skips a micromap's transparent interior") {
  Fixture fx;
  RenderSettings rays = base_settings(RasterMode::Hardware);
  rays.shadows = ShadowMode::RayTraced;
  TunableScope forced("renderer.shadow.opaque_rays=true", "renderer.shadow.opaque_rays=false");
  // Written against the four-state answer, which was the default until R120 batch 53.
  TunableScope four("renderer.shadow.micromap_2state=false",
                    "renderer.shadow.micromap_2state=true");
  TunableScope fine("renderer.rt.micromap_texels=0.01", "renderer.rt.micromap_texels=4");
  TunableScope cap("renderer.rt.micromap_level=6", "renderer.rt.micromap_level=6");
  MicromapShot on;
  {
    TunableScope set("renderer.shadow.micromaps=true", "renderer.shadow.micromaps=true");
    on = shoot_micromaps(fx.caster_masked, rays, micromap_shadow_view());
  }
  if (!on.skip.empty() || !on.baked) {
    MESSAGE("skipped: " << (on.skip.empty() ? std::string("no micromaps here") : on.skip));
    return;
  }
  MicromapShot off;
  {
    TunableScope set("renderer.shadow.micromaps=false", "renderer.shadow.micromaps=true");
    off = shoot_micromaps(fx.caster_masked, rays, micromap_shadow_view());
  }
  REQUIRE(off.skip.empty());
  const ShadowCount with = count_shadow(on.frame);
  const ShadowCount without = count_shadow(off.frame);
  MESSAGE("forced solid: " << without.share() << " of the ground shadowed without micromaps, "
                           << with.share() << " with them");
  const f64 micro =
      static_cast<f64>(on.stats.micro_opaque + on.stats.micro_transparent + on.stats.micro_unknown);
  REQUIRE(micro > 0.0);
  const f64 transparent = static_cast<f64>(on.stats.micro_transparent) / micro;
  REQUIRE(transparent > 0.0);
  CHECK(with.share() < without.share() * (1.0 - 0.5 * transparent));
  CHECK(with.share() > without.share() * (1.0 - 1.5 * transparent));
}

// **The casters in instances of their own** (R140; renderer.md, "The casters in instances of their
// own"). The masked quad seen from below its own height, so the whole of it is outside the frustum
// and every cluster of it is an off-frustum caster, while its shadow is on the ground in view. With
// `renderer.rt.caster_instances` the caster's record is built as a drawn one's, with the
// micromap; without it, non-opaque without one. Under four states the two draw the same bytes —
// the traversal decides only what the mask decides one way everywhere — and the forced-solid probe
// shows the micromap is really on the caster: forced solid, a caster without one is the solid
// quad's shadow, and one with it loses the bake's transparent micro-triangles.
ShotView off_frustum_caster_view() {
  ShotView view;
  view.eye = Vec3{-0.9f, 0.7f, -0.9f};
  view.target = Vec3{-1.3f, 0.0f, -1.3f};
  view.mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  return view;
}

TEST_CASE("alpha mask: an off-frustum caster's own record carries the drawn record's micromap") {
  Fixture fx;
  RenderSettings rays = base_settings(RasterMode::Hardware);
  rays.shadows = ShadowMode::RayTraced;
  TunableScope four("renderer.shadow.micromap_2state=false",
                    "renderer.shadow.micromap_2state=true");
  TunableScope fine("renderer.rt.micromap_texels=0.01", "renderer.rt.micromap_texels=4");
  TunableScope cap("renderer.rt.micromap_level=6", "renderer.rt.micromap_level=6");
  TunableScope set("renderer.shadow.micromaps=true", "renderer.shadow.micromaps=true");
  const auto shot = [&](bool own, bool forced) {
    TunableScope split(
        own ? "renderer.rt.caster_instances=true" : "renderer.rt.caster_instances=false",
        "renderer.rt.caster_instances=false");
    TunableScope solid(
        forced ? "renderer.shadow.opaque_rays=true" : "renderer.shadow.opaque_rays=false",
        "renderer.shadow.opaque_rays=false");
    return shoot_micromaps(fx.caster_masked, rays, off_frustum_caster_view());
  };
  const MicromapShot own = shot(true, false);
  if (!own.skip.empty() || !own.baked) {
    MESSAGE("skipped: " << (own.skip.empty() ? std::string("no micromaps here") : own.skip));
    return;
  }
  const MicromapShot shared = shot(false, false);
  REQUIRE(shared.skip.empty());
  const ShadowCount cast = count_shadow(shared.frame);
  const u32 differ = differing_pixels(own.frame, shared.frame);
  MESSAGE("off-frustum masked caster: "
          << cast.shadowed << " of " << cast.ground << " ground pixels shadowed; " << differ
          << " pixels differ between its own record and a shared one");
  // The quad is outside the view and still casts onto it: the case can tell.
  CHECK(cast.shadowed > 100);
  CHECK(differ == 0);
  // Forced solid: the caster's micromap is on its own record, so its transparent interior stays
  // open, by the bake's own transparent share within half of it either way (the probe above).
  const ShadowCount with = count_shadow(shot(true, true).frame);
  const ShadowCount without = count_shadow(shot(false, true).frame);
  const f64 micro = static_cast<f64>(own.stats.micro_opaque + own.stats.micro_transparent +
                                     own.stats.micro_unknown);
  REQUIRE(micro > 0.0);
  const f64 transparent = static_cast<f64>(own.stats.micro_transparent) / micro;
  MESSAGE("forced solid: " << without.share() << " of the ground shadowed by a caster without a "
                           << "micromap, " << with.share() << " by its own record with one");
  REQUIRE(transparent > 0.0);
  CHECK(with.share() < without.share() * (1.0 - 0.5 * transparent));
  CHECK(with.share() > without.share() * (1.0 - 1.5 * transparent));
}

TEST_CASE("alpha mask: a device without opacity micromaps draws what it drew") {
  Fixture fx;
  RenderSettings rays = base_settings(RasterMode::Hardware);
  rays.shadows = ShadowMode::RayTraced;
  MicromapShot without;
  {
    TunableScope set("renderer.shadow.micromaps=true", "renderer.shadow.micromaps=true");
    without = shoot_micromaps(fx.caster_masked, rays, micromap_shadow_view(), true);
  }
  if (!without.skip.empty()) {
    MESSAGE("skipped: " << without.skip);
    return;
  }
  MicromapShot off;
  {
    TunableScope set("renderer.shadow.micromaps=false", "renderer.shadow.micromaps=true");
    off = shoot_micromaps(fx.caster_masked, rays, micromap_shadow_view());
  }
  REQUIRE(off.skip.empty());
  CHECK_FALSE(without.baked);
  CHECK(differing_pixels(without.frame, off.frame) == 0);
}

// **The shadow rays' two-state answer** (renderer.md, "Opacity micromaps"; R120 stage 3). With
// `renderer.shadow.micromap_2state` the masked shadow query forces the micromaps two-state, so an
// unknown micro-triangle is decided by its centre sample in the traversal. What may move is the
// shadow's edge, by at most one micro-triangle: every pixel that changes lies within that band of
// a lit/shadowed boundary of the four-state picture, the band narrows as the level rises, and the
// primary rays never carry the flag, so the ray path's ids are the same words. Something must
// change at the coarser level, or the driver ignored the bit (the module does not declare the
// capability; the flag is a run-time operand). Measured: 63 flips at level 4 and 6 at level 7.
namespace {

// The quad is 2 m across; at `level` a micro-triangle's leg is 2 / 2^level m and its longest side
// sqrt(2) of that. Two states move the shadow's edge by at most that side, which the shadow on
// the ground draws at most as many pixels long as the disc's shadow draws its 1.28 m diameter
// along its longer axis (the sun's slant stretches one axis; the scale is measured, not assumed).
// So a changed pixel lies within this many pixels of a boundary of the four-state picture.
u32 two_state_band_pixels(u32 level, const CapturedFrame& four) {
  const ShadowCount reference = count_shadow(four);
  u32 x0 = four.width;
  u32 y0 = four.height;
  u32 x1 = 0;
  u32 y1 = 0;
  for (u32 p = 0; p < four.width * four.height; ++p) {
    if (reference.classes[p] != 2) continue;
    const u32 x = p % four.width;
    const u32 y = p / four.width;
    x0 = std::min(x0, x);
    y0 = std::min(y0, y);
    x1 = std::max(x1, x);
    y1 = std::max(y1, y);
  }
  const f64 extent = x1 >= x0 ? static_cast<f64>(std::max(x1 - x0, y1 - y0) + 1u) : 0.0;
  const f64 pixels_per_m = extent / 1.28125;
  const f64 side_m = std::sqrt(2.0) * 2.0 / static_cast<f64>(1u << level);
  return 1u + static_cast<u32>(std::ceil(side_m * pixels_per_m));
}

struct TwoStateChange {
  u32 changed = 0;      // pixels whose colour or ids differ
  u32 ids_changed = 0;  // pixels whose ids differ
  u32 flipped = 0;      // ground pixels lit in one picture and shadowed in the other
  u32 outside = 0;      // flipped pixels farther than the band from a boundary
};

TwoStateChange two_state_change(const CapturedFrame& four, const CapturedFrame& two, u32 band) {
  TwoStateChange out;
  const u32 pixels = four.width * four.height;
  const u32 id_words = pixels == 0 ? 0u : static_cast<u32>(four.ids.size() / pixels);
  const ShadowCount reference = count_shadow(four);
  const ShadowCount moved = count_shadow(two);
  for (u32 p = 0; p < pixels; ++p) {
    bool ids_same = true;
    for (u32 w = 0; w < id_words && ids_same; ++w)
      ids_same = four.ids[p * id_words + w] == two.ids[p * id_words + w];
    const bool same = ids_same && std::memcmp(&four.color[p * 4], &two.color[p * 4], 4) == 0;
    if (!ids_same) ++out.ids_changed;
    if (same) continue;
    ++out.changed;
    // Only a sun-shadow flip is the sun's edge; a few codes elsewhere are the array lights' own
    // shadow edges, which take the flag too and whose boundaries this map does not draw.
    const u8 mine = reference.classes[p];
    if (mine == 0 || moved.classes[p] == 0 || moved.classes[p] == mine) continue;
    ++out.flipped;
    const i32 x = static_cast<i32>(p % four.width);
    const i32 y = static_cast<i32>(p / four.width);
    const i32 r = static_cast<i32>(band);
    bool near = false;
    for (i32 dy = -r; dy <= r && !near; ++dy) {
      for (i32 dx = -r; dx <= r && !near; ++dx) {
        const i32 qx = x + dx;
        const i32 qy = y + dy;
        if (qx < 0 || qy < 0 || qx >= static_cast<i32>(four.width) ||
            qy >= static_cast<i32>(four.height))
          continue;
        const u8 other =
            reference.classes[static_cast<u32>(qy) * four.width + static_cast<u32>(qx)];
        near = other != 0 && other != mine;
      }
    }
    if (!near) ++out.outside;
  }
  return out;
}

// The caster scene with a **disc** for its mask instead of the checker, sampled bilinearly: the
// checker's edges lie on texel boundaries, which the micro-triangles of an aligned quad never
// cross, so every unknown micro-triangle there is unknown for the filter's footprint and its centre
// gives the exact answer — two states would move nothing. A disc's edge crosses micro-triangles at
// every level. 64 x 64 texels, radius 20.5 about (32.37, 31.81), alpha ramping across two texels so
// the bilinear lookup's 0.5 contour is the circle. Returns the scene's path.
std::string write_disc_caster(const Fixture& fx) {
  const std::filesystem::path dir(fx.tmp.path());
  constexpr u32 n = 64;
  std::vector<u8> rgba(n * n * 4);
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x) {
      const f64 dx = static_cast<f64>(x) + 0.5 - 32.37;
      const f64 dy = static_cast<f64>(y) + 0.5 - 31.81;
      const f64 a = std::clamp(0.5 + (20.5 - std::sqrt(dx * dx + dy * dy)) * 0.5, 0.0, 1.0);
      u8* t = &rgba[(y * n + x) * 4];
      t[0] = t[1] = t[2] = 255;
      t[3] = static_cast<u8>(std::lround(a * 255.0));
    }
  }
  Vector<u8> png;
  REQUIRE(image::encode_png(n, n, 4, std::span<const u8>(rgba.data(), rgba.size()), png));
  REQUIRE(write_file((dir / "disc.png").string(), png.data(), png.size()));
  std::ifstream in((dir / "caster_masked.gltf").string(), std::ios::binary);
  std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto replace = [&](const std::string& from, const std::string& to) {
    const usize at = json.find(from);
    REQUIRE(at != std::string::npos);
    json.replace(at, from.size(), to);
  };
  replace("\"uri\":\"mask.png\"", "\"uri\":\"disc.png\"");
  replace("\"magFilter\":9728,\"minFilter\":9728", "\"magFilter\":9729,\"minFilter\":9729");
  const std::string path = (dir / "caster_disc.gltf").string();
  REQUIRE(write_file(path, json.data(), json.size()));
  return path;
}

}  // namespace

TEST_CASE("alpha mask: two-state micromaps move only a shadow edge's band, never a primary ray") {
  Fixture fx;
  const std::string disc = write_disc_caster(fx);
  RenderSettings rays = base_settings(RasterMode::Hardware);
  rays.shadows = ShadowMode::RayTraced;
  RenderSettings traced = base_settings(RasterMode::RayTrace);
  traced.shadows = ShadowMode::RayTraced;
  TunableScope fine("renderer.rt.micromap_texels=0.01", "renderer.rt.micromap_texels=4");
  TunableScope maps("renderer.shadow.micromaps=true", "renderer.shadow.micromaps=true");
  u32 changed_at[2] = {0, 0};
  const u32 levels[2] = {4, 7};
  for (u32 i = 0; i < 2; ++i) {
    const std::string cap = "renderer.rt.micromap_level=" + std::to_string(levels[i]);
    TunableScope level(cap.c_str(), "renderer.rt.micromap_level=6");
    MicromapShot four;
    {
      TunableScope set("renderer.shadow.micromap_2state=false",
                       "renderer.shadow.micromap_2state=true");
      four = shoot_micromaps(disc, rays, micromap_shadow_view());
    }
    if (!four.skip.empty() || !four.baked) {
      MESSAGE("skipped: " << (four.skip.empty() ? std::string("no micromaps here") : four.skip));
      return;
    }
    MicromapShot two;
    {
      TunableScope set("renderer.shadow.micromap_2state=true",
                       "renderer.shadow.micromap_2state=true");
      two = shoot_micromaps(disc, rays, micromap_shadow_view());
    }
    REQUIRE(two.skip.empty());
    REQUIRE(two.baked);
    REQUIRE(four.stats.level_cap == levels[i]);
    const u32 band = two_state_band_pixels(levels[i], four.frame);
    const TwoStateChange change = two_state_change(four.frame, two.frame, band);
    MESSAGE("level " << levels[i] << ": " << change.changed << " pixels changed, " << change.flipped
                     << " lit/shadowed flips, " << change.outside << " of them outside the " << band
                     << "-pixel band, " << four.stats.micro_unknown << " unknown micro-triangles");
    CHECK(change.ids_changed == 0);
    CHECK(change.outside == 0);
    changed_at[i] = change.changed;
  }
  // The flag reaches the traversal (the coarser level's centre answers differ from the loop's
  // somewhere along the checker's edges), and a finer level moves fewer pixels.
  CHECK(changed_at[0] > 0);
  CHECK(changed_at[1] <= changed_at[0]);

  // The ray path: its primary rays never carry the flag, so its ids are the same words.
  {
    TunableScope level("renderer.rt.micromap_level=5", "renderer.rt.micromap_level=6");
    MicromapShot four;
    {
      TunableScope set("renderer.shadow.micromap_2state=false",
                       "renderer.shadow.micromap_2state=true");
      four = shoot_micromaps(disc, traced, micromap_shadow_view());
    }
    MicromapShot two;
    {
      TunableScope set("renderer.shadow.micromap_2state=true",
                       "renderer.shadow.micromap_2state=true");
      two = shoot_micromaps(disc, traced, micromap_shadow_view());
    }
    if (four.skip.empty() && two.skip.empty()) {
      const TwoStateChange change =
          two_state_change(four.frame, two.frame, two_state_band_pixels(5, four.frame));
      MESSAGE("ray path: " << change.ids_changed << " ids changed, " << change.changed
                           << " pixels changed");
      CHECK(change.ids_changed == 0);
      CHECK(change.outside == 0);
    }
  }

  // A device without the extension bakes nothing and never sees the flag: what it drew.
  {
    MicromapShot without;
    {
      TunableScope set("renderer.shadow.micromap_2state=true",
                       "renderer.shadow.micromap_2state=true");
      without = shoot_micromaps(disc, rays, micromap_shadow_view(), true);
    }
    MicromapShot off;
    {
      TunableScope set("renderer.shadow.micromaps=false", "renderer.shadow.micromaps=true");
      off = shoot_micromaps(disc, rays, micromap_shadow_view());
    }
    REQUIRE(without.skip.empty());
    CHECK_FALSE(without.baked);
    CHECK(differing_pixels(without.frame, off.frame) == 0);
  }
}

// **On by default where the device has them** (R120, batch 53; renderer.md, "Opacity micromaps"):
// a device with VK_EXT_opacity_micromap beside the cluster extension resolves to micromaps on, the
// shadow rays' two-state answer, level 6 and the 1,024 MiB bake budget with no tunable named; the
// scene's chain budget is the device's sixteenth scaled by the micromaps' slot growth, so it holds
// as many clusters as it would without them, and a named budget is left as named; the defaults
// draw what every flag set by hand draws; a device without the extension resolves to off.
TEST_CASE(
    "alpha mask: micromaps are the default where the device has them, and the budget follows") {
  Fixture fx;
  RenderSettings rays = base_settings(RasterMode::Hardware);
  rays.shadows = ShadowMode::RayTraced;
  gfx::Device device;
  std::string error;
  if (!device.create(renderer_test::device_options(), &error)) {
    MESSAGE("skipped: device unavailable: " << error);
    return;
  }
  if (!device.features().cluster_acceleration_structure || !device.features().ray_query) {
    MESSAGE("skipped: no cluster acceleration structures and ray queries on this device");
    device.destroy();
    return;
  }
  SceneData data;
  SceneDesc desc;
  desc.meshes.push_back(fx.caster_masked);
  desc.cache = false;
  REQUIRE_MESSAGE(load_scene(desc, data, error), "scene: " << error);
  const bool has = device.features().opacity_micromap;
  ResolvedSettings resolved;
  resolve_settings(rays, device.features(), &data, resolved);
  CHECK(resolved.shadow_micromaps == has);
  CHECK(resolved.micromap_2state == has);
  CHECK(resolved.micromap_level == 6);
  CHECK(resolved.micromap_budget_bytes == (u64{1024} << 20));
  gfx::MemoryBudget memory;
  REQUIRE(device.memory_budget(memory));
  {
    GpuScene scene;
    REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), "gpu scene: " << error);
    REQUIRE(scene.rt_bytes_per_cluster() > 0);
    MESSAGE("a cluster of capacity: "
            << scene.rt_bytes_per_cluster() << " bytes, " << scene.rt_bytes_per_cluster_plain()
            << " without the micromap flag; the budget " << scene.rt_budget_mib() << " MiB ("
            << std::string(rt_budget_source_name(scene.rt_budget_source())) << ")");
    if (has) {
      CHECK(scene.rt_budget_source() == RtBudgetSource::DeviceMicromaps);
      CHECK(scene.rt_bytes_per_cluster() > scene.rt_bytes_per_cluster_plain());
      CHECK(scene.rt_budget_mib() == rt_device_budget_mib(memory.device_local_bytes,
                                                          scene.rt_bytes_per_cluster(),
                                                          scene.rt_bytes_per_cluster_plain()));
      // The same clusters as the unscaled budget would hold without the flag, to a MiB's worth.
      const u64 with = u64{scene.rt_budget_mib()} * 1024 * 1024 / scene.rt_bytes_per_cluster();
      const u64 without = u64{rt_device_budget_mib(memory.device_local_bytes)} * 1024 * 1024 /
                          scene.rt_bytes_per_cluster_plain();
      CHECK(with + 1024 * 1024 / scene.rt_bytes_per_cluster() + 1 >= without);
      CHECK(with <= without);
    } else {
      CHECK(scene.rt_budget_source() == RtBudgetSource::Device);
      CHECK(scene.rt_bytes_per_cluster() == scene.rt_bytes_per_cluster_plain());
    }
    scene.destroy();
  }
  {
    RenderSettings named = rays;
    named.rt_budget_mib = 64;
    ResolvedSettings resolved_named;
    resolve_settings(named, device.features(), &data, resolved_named);
    GpuScene scene;
    REQUIRE_MESSAGE(scene.create(device, data, resolved_named, &error), "gpu scene: " << error);
    CHECK(scene.rt_budget_source() == RtBudgetSource::Setting);
    CHECK(scene.rt_budget_mib() == 64);
    scene.destroy();
  }
  device.destroy();

  // A device without the extension resolves to off, whatever the tunables say.
  {
    gfx::Device bare;
    gfx::DeviceOptions options = renderer_test::device_options();
    options.overrides.absent.push_back("VK_EXT_opacity_micromap");
    REQUIRE_MESSAGE(bare.create(options, &error), error);
    ResolvedSettings off;
    resolve_settings(rays, bare.features(), &data, off);
    CHECK_FALSE(off.shadow_micromaps);
    CHECK_FALSE(off.micromap_2state);
    bare.destroy();
  }

  // The defaults draw what every flag set by hand draws.
  const MicromapShot defaults = shoot_micromaps(fx.caster_masked, rays, micromap_shadow_view());
  MicromapShot by_hand;
  {
    TunableScope maps("renderer.shadow.micromaps=true", "renderer.shadow.micromaps=true");
    TunableScope two("renderer.shadow.micromap_2state=true",
                     "renderer.shadow.micromap_2state=true");
    TunableScope level("renderer.rt.micromap_level=6", "renderer.rt.micromap_level=6");
    TunableScope budget("renderer.rt.micromap_mib=1024", "renderer.rt.micromap_mib=1024");
    by_hand = shoot_micromaps(fx.caster_masked, rays, micromap_shadow_view());
  }
  REQUIRE(defaults.skip.empty());
  REQUIRE(by_hand.skip.empty());
  CHECK(defaults.baked == has);
  CHECK(by_hand.baked == has);
  CHECK(differing_pixels(defaults.frame, by_hand.frame) == 0);
}

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

#include <core/math/math.h>
#include <domain/gfx/device.h>
#include <foundation/image/png.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/reference.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
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
  if (!device.create(gfx::DeviceOptions{}, &error)) {
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

TEST_CASE("alpha mask: the reference path tracer sees through the holes the rasterizer leaves") {
  // The masked quad through the reference (k_pt_alpha_masks: its camera rays decide masked
  // candidates by the mask) and through the real-time frame: a pixel the rasterizer leaves empty is
  // the sky in the reference, to the bit, and a pixel it covers is not. The sky is the furnace's
  // one colour, so a hole reads exactly the background a pixel off the quad reads.
  Fixture fx;
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
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

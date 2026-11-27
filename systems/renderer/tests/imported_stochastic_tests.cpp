// Stochastic sampling an imported material asks for (roadmap T150; renderer.md, "Stochastic mesh
// materials"; assets.md, "Stochastic sampling an imported material asks for"): a glTF written at
// test time, a large plane repeating one noise texture eight times, loaded through `load_scene` as
// any mesh is — the file's own `extras.sampling`, or the scene's `Mesh.sampling` for a file that
// does not say — and drawn in the albedo view. The repeat is measured, not looked at: how much of
// the picture is the picture one texture period along. Skipped with a message on a machine with no
// Vulkan device, as every renderer test is; the scene-file case needs no device.
#include "test_device.h"

#include <core/math/math.h>
#include <domain/geometry/material_sampling.h>
#include <domain/gfx/device.h>
#include <domain/gfx/visibility_resolve.h>
#include <foundation/image/png.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/early_pipelines.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr u32 k_size = 256;                       // the frame, square
constexpr u32 k_repeats = 8;                      // the texture's repeats across the plane
constexpr u32 k_texture = 64;                     // its side in texels
constexpr u32 k_period = 2 * k_size / k_repeats;  // its period on screen: 64 pixels, one a texel
constexpr f32 k_fov = 0.9599310886f;              // Camera's default, radians(55)
constexpr u32 k_albedo_view = static_cast<u32>(gfx::ResolveMode::Albedo);

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

// Hashed noise, a value per texel and channel: no structure for a repeat to hide in.
u8 noise(u32 x, u32 y, u32 c) {
  u32 h = x * 73856093u ^ y * 19349663u ^ c * 83492791u;
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  return static_cast<u8>(40 + (h & 0xffu) * 175 / 255);
}

// A plane of half-width 1 in the XY plane facing +z, its UVs running 0 to `k_repeats`, one material
// whose base colour is `image.png` beside it (repeat, linear: no sampler) and whose `extras` are
// `extras` (empty: none). `material` false: a file with no materials at all, whose primitive the
// builders give the default they append.
bool write_plane(const std::filesystem::path& dir, const std::string& extras,
                 bool material = true) {
  std::filesystem::create_directories(dir);
  std::vector<u8> bin;
  constexpr f32 r = static_cast<f32>(k_repeats);
  const f32 positions[12] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
  const f32 normals[12] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  const f32 uvs[8] = {0, r, r, r, r, 0, 0, 0};
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
  if (!write_file((dir / "plane.bin").string(), bin.data(), bin.size())) return false;
  std::vector<u8> rgba(k_texture * k_texture * 4);
  for (u32 y = 0; y < k_texture; ++y) {
    for (u32 x = 0; x < k_texture; ++x) {
      u8* t = &rgba[(y * k_texture + x) * 4];
      for (u32 c = 0; c < 3; ++c)
        t[c] = noise(x, y, c);
      t[3] = 255;
    }
  }
  Vector<u8> png;
  if (!image::encode_png(k_texture, k_texture, 4, std::span<const u8>(rgba.data(), rgba.size()),
                         png))
    return false;
  if (!write_file((dir / "image.png").string(), png.data(), png.size())) return false;
  const std::string materials =
      material ? "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},"
                 "\"metallicFactor\":0,\"roughnessFactor\":1}" +
                     (extras.empty() ? std::string() : ",\"extras\":" + extras) +
                     "}],"
                     "\"textures\":[{\"source\":0}],\"images\":[{\"uri\":\"image.png\"}],"
               : std::string();
  const std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
      "\"TEXCOORD_0\":2},\"indices\":3" +
      std::string(material ? ",\"material\":0" : "") + "}]}]," + materials +
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
      "\"buffers\":[{\"uri\":\"plane.bin\",\"byteLength\":140}]}";
  return write_file((dir / "plane.gltf").string(), json.data(), json.size());
}

// One plane's scene, through the derived-data root in the test's scratch directory (built
// textures: the stochastic path takes its means from their small mips), in the albedo view.
// `sampling`: the scene's `Mesh.sampling` entries, none for a scene that says nothing.
struct PlaneRig {
  SceneData data;
  GpuScene scene;
  SceneRenderer renderer;
  bool made = false;

  void build(const gfx::Device& device, const std::string& mesh, const std::string& ddc,
             const Vector<SceneMeshInfo::MaterialSampling>& sampling = {}) {
    std::string error;
    SceneDesc desc;
    desc.meshes.push_back(mesh);
    desc.cache = true;
    desc.ddc = ddc;
    if (!sampling.empty()) {
      SceneMeshInfo info;
      info.name = "plane";
      info.sampling = sampling;
      desc.mesh_info.push_back(std::move(info));
    }
    REQUIRE_MESSAGE(load_scene(desc, data, error), "scene: " << error);
    RenderSettings settings;
    settings.shadows = ShadowMode::Off;
    settings.view_mode = k_albedo_view;
    ResolvedSettings resolved;
    resolve_settings(settings, device.features(), &data, resolved);
    REQUIRE(check_availability(resolved, device.features()) == RenderAvailability::Ok);
    REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), "gpu scene: " << error);
    SceneRenderer::Desc rd;
    rd.width = k_size;
    rd.height = k_size;
    REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), "renderer: " << error);
    made = true;
  }

  // Straight on from the distance at which the plane's width is twice the frame's, so a texture
  // period is `k_period` pixels everywhere on it (a plane square to the view projects affinely).
  CapturedFrame shot() {
    FrameDesc frame;
    const f32 distance = 0.5f / std::tan(0.5f * k_fov);
    frame.camera.position = absolute(WorldPos::origin(), Vec3{0.0f, 0.0f, distance});
    frame.camera.target = absolute(WorldPos::origin(), Vec3{0.0f, 0.0f, 0.0f});
    frame.camera.fov_y = k_fov;
    frame.view_mode = k_albedo_view;
    CaptureChannels channels;
    channels.color = true;
    CapturedFrame out;
    std::string error;
    REQUIRE_MESSAGE(renderer.capture(frame, channels, out, &error), error);
    return out;
  }

  ~PlaneRig() {
    if (!made) return;
    renderer.destroy();
    scene.destroy();
  }
};

// **The repeat, by count**: of the pixel pairs one texture period apart along x and along y, the
// share whose three channels all lie within 4 codes of each other, and the normalized correlation
// of their luminance. A tiled plane is the same picture a period along; a stochastic one is not.
struct Repeat {
  f64 same = 0.0;
  f64 correlation = 0.0;
  f64 mean[3] = {0.0, 0.0, 0.0};  // linear, through the display transform's inverse (1/2.2)
};

Repeat measure(const CapturedFrame& f) {
  REQUIRE(f.width == k_size);
  REQUIRE(f.height == k_size);
  const auto luma = [&](u32 x, u32 y) {
    const u8* p = &f.color[(y * k_size + x) * 4];
    return 0.25 * p[0] + 0.5 * p[1] + 0.25 * p[2];
  };
  Repeat r;
  u64 pairs = 0;
  u64 same = 0;
  f64 sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      const u8* p = &f.color[(y * k_size + x) * 4];
      for (u32 c = 0; c < 3; ++c)  // in linear light, where the blend keeps the mean
        r.mean[c] += std::pow(static_cast<f64>(p[c]) / 255.0, 2.2);
      for (u32 axis = 0; axis < 2; ++axis) {
        const u32 x2 = axis == 0 ? x + k_period : x;
        const u32 y2 = axis == 1 ? y + k_period : y;
        if (x2 >= k_size || y2 >= k_size) continue;
        const u8* q = &f.color[(y2 * k_size + x2) * 4];
        int d = 0;
        for (u32 c = 0; c < 3; ++c)
          d = std::max(d, std::abs(int{p[c]} - int{q[c]}));
        ++pairs;
        if (d <= 4) ++same;
        const f64 a = luma(x, y);
        const f64 b = luma(x2, y2);
        sa += a;
        sb += b;
        saa += a * a;
        sbb += b * b;
        sab += a * b;
      }
    }
  }
  for (u32 c = 0; c < 3; ++c)
    r.mean[c] /= static_cast<f64>(k_size * k_size);
  const f64 n = static_cast<f64>(pairs);
  r.same = static_cast<f64>(same) / n;
  const f64 cov = sab / n - (sa / n) * (sb / n);
  const f64 va = saa / n - (sa / n) * (sa / n);
  const f64 vb = sbb / n - (sb / n) * (sb / n);
  r.correlation = va > 0.0 && vb > 0.0 ? cov / std::sqrt(va * vb) : 0.0;
  return r;
}

bool same_bytes(const CapturedFrame& a, const CapturedFrame& b) {
  return a.width == b.width && a.height == b.height && a.color.size() == b.color.size() &&
         std::memcmp(a.color.data(), b.color.data(), a.color.size()) == 0;
}

}  // namespace

// **The file asks, or the scene asks for it, and the repeat is gone; nothing asked, nothing
// changed.** Five loads of one plane: no extras, `"tiled"`, `"stochastic"` (twice: the first load
// builds the derived-data entry, the second reads it back), the plain file with the scene's
// `Mesh.sampling` asking, and the stochastic file with the scene's word `Tiled`. The plain plane
// repeats (nearly every pair a period apart the same); the stochastic one does not, and keeps the
// plain one's mean colour. A load that asks for nothing draws through the plain module, and
// `"tiled"` and the scene's `Tiled` draw its bytes; the scene's `Stochastic` draws the file's.
TEST_CASE("renderer: an imported material that asks for stochastic sampling loses its repeat") {
  const test::TempDir tmp("engine_renderer_imported_stochastic");
  const std::filesystem::path root = tmp.native();
  const std::string ddc = (root / "ddc").string();
  REQUIRE(write_plane(root / "plain", ""));
  REQUIRE(write_plane(root / "tiled", "{\"sampling\":\"tiled\"}"));
  REQUIRE(write_plane(root / "asked", "{\"sampling\":\"stochastic\"}"));
  const auto mesh = [&](const char* name) { return (root / name / "plane.gltf").string(); };

  gfx::Device device;
  std::string error;
  if (!device.create(renderer_test::device_options(), &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  std::string last_cache;  // the derived-data cache's word on the last draw's mesh
  const auto draw = [&](const std::string& path,
                        const Vector<SceneMeshInfo::MaterialSampling>& sampling, bool& stochastic) {
    PlaneRig rig;
    rig.build(device, path, ddc, sampling);
    stochastic = rig.scene.has_stochastic_materials();
    last_cache = rig.data.sources.empty() ? std::string() : rig.data.sources[0].cache;
    return rig.shot();
  };
  bool plain_module = true;
  bool tiled_module = true;
  bool asked_module = false;
  bool asked_again_module = false;
  bool scene_module = false;
  bool scene_tiled_module = true;
  const CapturedFrame plain = draw(mesh("plain"), {}, plain_module);
  const CapturedFrame tiled = draw(mesh("tiled"), {}, tiled_module);
  const CapturedFrame asked = draw(mesh("asked"), {}, asked_module);
  CHECK(last_cache == "miss");  // the first load builds the entry
  const CapturedFrame asked_again = draw(mesh("asked"), {}, asked_again_module);
  CHECK(last_cache == "hit");  // and the second reads it back
  const CapturedFrame by_scene = draw(mesh("plain"), {{-1, true}}, scene_module);
  const CapturedFrame scene_tiled = draw(mesh("asked"), {{0, false}}, scene_tiled_module);

  CHECK_FALSE(plain_module);  // nothing asked: the plain resolve, the pipeline it always compiled
  CHECK_FALSE(tiled_module);
  CHECK(asked_module);
  CHECK(asked_again_module);
  CHECK(scene_module);
  CHECK_FALSE(scene_tiled_module);

  const Repeat p = measure(plain);
  const Repeat s = measure(asked);
  MESSAGE("plain: " << p.same * 100.0 << "% of pairs a period apart within 4 codes, correlation "
                    << p.correlation << "; stochastic: " << s.same * 100.0 << "%, correlation "
                    << s.correlation << "; means (" << p.mean[0] << ", " << p.mean[1] << ", "
                    << p.mean[2] << ") and (" << s.mean[0] << ", " << s.mean[1] << ", " << s.mean[2]
                    << ")");
  CHECK(p.same > 0.9);
  CHECK(p.correlation > 0.9);
  CHECK(s.same < 0.2);
  CHECK(s.correlation < 0.3);
  for (u32 c = 0; c < 3; ++c)
    CHECK(std::fabs(s.mean[c] - p.mean[c]) < 0.03 * p.mean[c]);

  CHECK(same_bytes(plain, tiled));        // "tiled" is the plain material
  CHECK(same_bytes(asked, asked_again));  // the cache's hit draws what its miss drew
  CHECK(same_bytes(asked, by_scene));     // the scene's word reaches the file's variant
  CHECK(same_bytes(plain, scene_tiled));  // and wins over the file's
  device.destroy();
}

// **The scene file's word, read**: `Mesh.sampling` through `read_scene_file` and `load_scene`, with
// no device — the entries in order, a later one over an earlier, an index past the file's
// materials refused by name, and a load that names none keeping what the file says. **Every leg
// three ways** (T150's review, finding 1): with the cache off, through a fresh derived-data root
// (a miss, which writes the entry) and through it again (a hit, which reads it back), and the
// three give one answer: the file's own materials are the bound and what -1 reaches, never the
// default a build appends for a primitive with none. The material's `stochastic` says what its
// flag says after every load (finding 5).
TEST_CASE("renderer: a scene's Mesh.sampling sets its materials' sampling over the file's") {
  const test::TempDir tmp("engine_renderer_scene_sampling");
  const std::filesystem::path root = tmp.native();
  REQUIRE(write_plane(root / "plain", ""));
  REQUIRE(write_plane(root / "asked", "{\"sampling\":\"stochastic\"}"));
  REQUIRE(write_plane(root / "bare", "", false));
  const u32 k_on = geometry::k_material_sampling_stochastic;
  u32 roots = 0;
  // One load: the flags of every mesh's own materials, or empty with `error`.
  const auto load_once = [&](const std::string& meshes, const std::string& ddc,
                             const char* expect_cache, std::string& error) {
    const std::string scene_path = (root / "scene.json").string();
    const std::string json = "{\"meshes\":[" + meshes + "]}";
    REQUIRE(write_file(scene_path, json.data(), json.size()));
    SceneDesc desc;
    REQUIRE_MESSAGE(read_scene_file(scene_path, desc, error), error);
    desc.cache = !ddc.empty();
    desc.ddc = ddc;
    SceneData data;
    Vector<u32> flags;
    if (!load_scene(desc, data, error)) return flags;
    for (const SourceMesh& source : data.sources) {
      CHECK(std::string(source.cache) == expect_cache);
      const u32 own = std::min<u32>(source.file_materials, source.data.materials.size());
      for (u32 k = 0; k < source.data.materials.size(); ++k) {
        const u32 f =
            k < source.material_sampling_flags.size() ? source.material_sampling_flags[k] : 0u;
        CHECK(source.data.materials[k].stochastic == ((f & k_on) != 0));
        if (k < own) {
          flags.push_back(f);
        } else {
          CHECK(f == 0u);  // the appended default: no entry reaches it
        }
      }
    }
    return flags;
  };
  // The same leg three ways; the answer and the error (empty when it loads) must agree.
  const auto flags_of = [&](const std::string& meshes, std::string& error) {
    std::string off_error;
    std::string miss_error;
    std::string hit_error;
    const Vector<u32> off = load_once(meshes, "", "none", off_error);
    const std::string ddc = (root / ("ddc" + std::to_string(roots++))).string();
    const Vector<u32> miss = load_once(meshes, ddc, "miss", miss_error);
    const Vector<u32> hit = load_once(meshes, ddc, "hit", hit_error);
    CHECK(miss_error == off_error);
    CHECK(hit_error == off_error);
    REQUIRE(miss.size() == off.size());
    REQUIRE(hit.size() == off.size());
    for (u32 k = 0; k < off.size(); ++k) {
      CHECK(miss[k] == off[k]);
      CHECK(hit[k] == off[k]);
    }
    error = off_error;
    return off;
  };
  std::string error;
  Vector<u32> f = flags_of(
      "{\"name\":\"a\",\"path\":\"plain/plane.gltf\"},"
      "{\"name\":\"b\",\"path\":\"asked/plane.gltf\"}",
      error);
  REQUIRE_MESSAGE(f.size() == 2, error);
  CHECK(f[0] == 0u);
  CHECK(f[1] == k_on);
  f = flags_of(
      "{\"name\":\"a\",\"path\":\"plain/plane.gltf\",\"sampling\":[{}]},"
      "{\"name\":\"b\",\"path\":\"asked/plane.gltf\","
      "\"sampling\":[{\"material\":0,\"sampling\":\"Tiled\"}]}",
      error);
  REQUIRE_MESSAGE(f.size() == 2, error);
  CHECK(f[0] == k_on);  // an entry's defaults: every material, stochastic
  CHECK(f[1] == 0u);
  f = flags_of(
      "{\"name\":\"a\",\"path\":\"plain/plane.gltf\",\"sampling\":["
      "{\"sampling\":\"Stochastic\"},{\"material\":-1,\"sampling\":\"Tiled\"}]}",
      error);
  REQUIRE_MESSAGE(f.size() == 1, error);
  CHECK(f[0] == 0u);  // the later entry wins
  error.clear();
  f = flags_of("{\"name\":\"a\",\"path\":\"plain/plane.gltf\",\"sampling\":[{\"material\":1}]}",
               error);
  CHECK(f.empty());
  CHECK(error.find("sampling names material 1 of 1") != std::string::npos);
  // A file with no materials: material 0 is refused on every route (the hit's container holds the
  // default, which is not the file's), and -1 asks for nothing there is.
  error.clear();
  f = flags_of("{\"name\":\"a\",\"path\":\"bare/plane.gltf\",\"sampling\":[{\"material\":0}]}",
               error);
  CHECK(f.empty());
  CHECK(error.find("sampling names material 0 of 0") != std::string::npos);
  error.clear();
  f = flags_of("{\"name\":\"a\",\"path\":\"bare/plane.gltf\",\"sampling\":[{}]}", error);
  CHECK(error.empty());
  CHECK(f.empty());

  // **Validated where the bound is** (finding 4): a `SceneDesc` built in code, which no scene
  // file's reader checked, is refused for an index below -1.
  {
    SceneDesc desc;
    desc.meshes.push_back((root / "plain" / "plane.gltf").string());
    desc.cache = false;  // nothing written outside the test's scratch directory
    SceneMeshInfo info;
    info.name = "coded";
    info.sampling.push_back({-2, true});
    desc.mesh_info.push_back(std::move(info));
    SceneData data;
    std::string why;
    CHECK_FALSE(load_scene(desc, data, why));
    CHECK_MESSAGE(why.find("coded: sampling names material -2") != std::string::npos, why);
  }

  // **The early resolve's prediction reads a container's own word** (finding 3): the entries the
  // two loads above wrote are `.clusters` files, peeked without a decode — the asked plane's flag
  // predicts the stochastic module, the plain one's does not, and the scene's word over the
  // container's is read in order as the load reads it.
  {
    SceneDesc built;
    built.meshes.push_back((root / "asked" / "plane.gltf").string());
    built.cache = true;
    built.ddc = (root / "ddc-predict").string();
    SceneData data;
    std::string why;
    REQUIRE_MESSAGE(load_scene(built, data, why), why);
    REQUIRE(data.sources.size() == 1);
    const std::string asked_container = data.sources[0].container;
    REQUIRE_FALSE(asked_container.empty());
    built.meshes[0] = (root / "plain" / "plane.gltf").string();
    SceneData plain_data;
    REQUIRE_MESSAGE(load_scene(built, plain_data, why), why);
    const std::string plain_container = plain_data.sources[0].container;
    REQUIRE_FALSE(plain_container.empty());
    ResolvedSettings resolved;
    const auto predicts = [&](const std::string& path,
                              const Vector<SceneMeshInfo::MaterialSampling>& sampling) {
      SceneDesc desc;
      desc.meshes.push_back(path);
      desc.materials = (root / "no-pack").string();
      SceneMeshInfo info;
      info.sampling = sampling;
      desc.mesh_info.push_back(std::move(info));
      return predict_resolve_variant(desc, resolved, gfx::Format::R8G8B8A8Unorm).stochastic;
    };
    CHECK(predicts(asked_container, {}));
    CHECK_FALSE(predicts(plain_container, {}));
    CHECK_FALSE(predicts(asked_container, {{0, false}}));
    CHECK(predicts(plain_container, {{0, true}}));
    CHECK_FALSE(predicts(plain_container, {{-1, true}, {0, false}}));
    CHECK_FALSE(predicts(plain_container, {{1, true}}));  // refused by the load: past its one
  }
}

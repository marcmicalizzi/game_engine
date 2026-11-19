// An imported asset's own vertex streams in the renderer (roadmap G2; gfx.md, "The author's tangent
// frame"; assets.md, "What G2 reads"), end to end: a glTF written at test time, loaded through
// `load_scene` as any mesh is, drawn shaded by each raster path the device has — the vertex path
// (the baseline tier), the mesh path, the ray path — and the pictures compared with each other and
// with what the mathematics says they must be. Skipped with a message on a machine with no Vulkan
// device, as every renderer test is.
#include "test_device.h"

#include <core/math/math.h>
#include <domain/gfx/device.h>
#include <foundation/image/png.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
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

constexpr u32 k_size = 128;

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

// A quad of half-width 1 in the XY plane facing +z, UV (0, 1) at the bottom left and (1, 0) at the
// top right as glTF's top-left origin puts an image on it, one material whose normal map is a
// constant `normal.png` beside it of tangent-space direction (128, green, 218): green 218 leans the
// normal 45 degrees along the bitangent, green 37 the same the other way. With `tangents` the
// primitive carries `TANGENT` as an exporter writes it for that origin: T = +x, w = +1, so glTF's
// bitangent cross(N, T) * w is +y — **the opposite of the frame the resolve derives from the UVs**,
// whose bitangent is dP/dv = -y, as the first leg measured on nearly every corner of five of the
// Khronos samples (assets.md, "What G2 reads").
bool write_normal_quad(const std::filesystem::path& dir, bool tangents, u8 green) {
  std::filesystem::create_directories(dir);
  std::vector<u8> bin;
  const f32 positions[12] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
  const f32 normals[12] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  const f32 uvs[8] = {0, 1, 1, 1, 1, 0, 0, 0};
  const f32 tangent_values[16] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
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
  bin.push_back(0);
  bin.push_back(0);  // to 144, so the tangents start on a 4-byte boundary
  for (const f32 v : tangent_values)
    put_f32(bin, v);
  if (!write_file((dir / "quad.bin").string(), bin.data(), bin.size())) return false;
  std::vector<u8> rgba(4 * 4 * 4);
  for (u32 i = 0; i < 16; ++i) {
    rgba[i * 4 + 0] = 128;
    rgba[i * 4 + 1] = green;
    rgba[i * 4 + 2] = 218;
    rgba[i * 4 + 3] = 255;
  }
  Vector<u8> png;
  if (!image::encode_png(4, 4, 4, std::span<const u8>(rgba.data(), rgba.size()), png)) return false;
  if (!write_file((dir / "normal.png").string(), png.data(), png.size())) return false;
  const std::string json =
      std::string(
          "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
          "\"nodes\":[{\"mesh\":0}],"
          "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
          "\"TEXCOORD_0\":2") +
      (tangents ? ",\"TANGENT\":4" : "") +
      "},\"indices\":3,\"material\":0}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.6,0.3,1],"
      "\"metallicFactor\":0,\"roughnessFactor\":0.6},\"normalTexture\":{\"index\":0}}],"
      "\"samplers\":[{\"magFilter\":9728,\"minFilter\":9728}],"
      "\"textures\":[{\"source\":0,\"sampler\":0}],"
      "\"images\":[{\"uri\":\"normal.png\"}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\","
      "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"},"
      "{\"bufferView\":4,\"componentType\":5126,\"count\":4,\"type\":\"VEC4\"}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":32},"
      "{\"buffer\":0,\"byteOffset\":128,\"byteLength\":12},"
      "{\"buffer\":0,\"byteOffset\":144,\"byteLength\":64}],"
      "\"buffers\":[{\"uri\":\"quad.bin\",\"byteLength\":208}]}";
  return write_file((dir / "quad.gltf").string(), json.data(), json.size());
}

// One quad's scene, drawn shaded by one raster path with no shadows (a shadow is not what the
// paths or the frames have to agree about).
struct StreamRig {
  SceneData data;
  GpuScene scene;
  SceneRenderer renderer;
  bool made = false;

  // False with `skip` set when the device has no such path.
  bool build(const gfx::Device& device, const std::string& mesh, RasterMode mode,
             std::string& skip) {
    std::string error;
    SceneDesc desc;
    desc.meshes.push_back(mesh);
    desc.cache = false;
    REQUIRE_MESSAGE(load_scene(desc, data, error), "scene: " << error);
    RenderSettings settings;
    settings.raster = mode;
    settings.shadows = ShadowMode::Off;
    ResolvedSettings resolved;
    resolve_settings(settings, device.features(), &data, resolved);
    const RenderAvailability availability = check_availability(resolved, device.features());
    if (availability != RenderAvailability::Ok) {
      skip = unavailable_reason(availability, device);
      return false;
    }
    const bool as_asked = mode == RasterMode::Vertex ? resolved.vertex_path
                          : mode == RasterMode::RayTrace
                              ? resolved.ray_path
                              : !resolved.vertex_path && !resolved.ray_path;
    if (!as_asked) {
      skip = "the device resolves this path to another";
      return false;
    }
    REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), "gpu scene: " << error);
    SceneRenderer::Desc rd;
    rd.width = k_size;
    rd.height = k_size;
    REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), "renderer: " << error);
    made = true;
    return true;
  }

  CapturedFrame shot() {
    FrameDesc frame;
    frame.camera.position = absolute(WorldPos::origin(), Vec3{0.0f, 0.0f, 3.0f});
    frame.camera.target = absolute(WorldPos::origin(), Vec3{0.0f, 0.0f, 0.0f});
    CaptureChannels channels;
    channels.color = true;
    channels.ids = true;
    CapturedFrame out;
    std::string error;
    REQUIRE_MESSAGE(renderer.capture(frame, channels, out, &error), error);
    return out;
  }

  ~StreamRig() {
    if (!made) return;
    renderer.destroy();
    scene.destroy();
  }
};

struct Comparison {
  u32 both = 0;   // pixels both frames cover with the same triangle
  int worst = 0;  // their largest channel difference
  u32 apart = 0;  // of those, the pixels more than 8 codes apart
};

Comparison compare(const CapturedFrame& a, const CapturedFrame& b) {
  REQUIRE(a.width == b.width);
  REQUIRE(a.height == b.height);
  Comparison c;
  for (u32 p = 0; p < a.width * a.height; ++p) {
    const u32 w = p * k_id_words;
    if (a.ids[w] == k_no_id || b.ids[w] == k_no_id) continue;
    bool same = true;
    for (u32 i = 0; i < k_id_words; ++i)
      same = same && a.ids[w + i] == b.ids[w + i];
    if (!same) continue;
    ++c.both;
    int d = 0;
    for (u32 ch = 0; ch < 3; ++ch)
      d = std::max(d, std::abs(int{a.color[p * 4 + ch]} - int{b.color[p * 4 + ch]}));
    c.worst = std::max(c.worst, d);
    if (d > 8) ++c.apart;
  }
  return c;
}

}  // namespace

// **The author's tangent frame draws the picture its mathematics says, on every path.** Three
// quads: the exporter's tangents with the map leaning +45 degrees along their bitangent; no
// tangents and the same map; no tangents and the map leaning the other way. The author's bitangent
// is the derived one turned over, so the first and the third must be one picture and the second a
// picture apart — on the vertex path, the mesh path and the ray path alike, and the three paths
// must agree with each other wherever they drew the same triangle. A scene whose meshes carry no
// tangent uploads no imported stream at all.
TEST_CASE("renderer: an imported tangent frame draws the same picture on every raster path") {
  const test::TempDir tmp("engine_renderer_imported_tangents");
  const std::filesystem::path root = tmp.native();
  const std::string authored = (root / "authored" / "quad.gltf").string();
  const std::string derived = (root / "derived" / "quad.gltf").string();
  const std::string flipped = (root / "flipped" / "quad.gltf").string();
  REQUIRE(write_normal_quad(root / "authored", true, 218));
  REQUIRE(write_normal_quad(root / "derived", false, 218));
  REQUIRE(write_normal_quad(root / "flipped", false, 37));

  gfx::Device device;
  std::string error;
  if (!device.create(renderer_test::device_options(), &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  CapturedFrame reference;  // the vertex path's authored picture, which every path is held to
  bool have_reference = false;
  for (const RasterMode mode : {RasterMode::Vertex, RasterMode::Hardware, RasterMode::RayTrace}) {
    const char* name = mode == RasterMode::Vertex     ? "vertex"
                       : mode == RasterMode::Hardware ? "mesh"
                                                      : "ray";
    INFO(name);
    CapturedFrame a;
    CapturedFrame d;
    CapturedFrame f;
    {
      StreamRig rig;
      std::string skip;
      if (!rig.build(device, authored, mode, skip)) {
        MESSAGE("the " << name << " path is not here: " << skip);
        continue;
      }
      CHECK(rig.scene.extra_vertices == rig.data.lod.mesh.vertices.size());
      CHECK(rig.scene.extra.address != 0);
      a = rig.shot();
    }
    {
      StreamRig rig;
      std::string skip;
      REQUIRE(rig.build(device, derived, mode, skip));
      CHECK(rig.scene.extra_vertices == 0);  // nothing imported, nothing uploaded
      CHECK(rig.scene.extra.address == 0);
      d = rig.shot();
    }
    {
      StreamRig rig;
      std::string skip;
      REQUIRE(rig.build(device, flipped, mode, skip));
      f = rig.shot();
    }
    const Comparison same = compare(a, f);
    const Comparison differ = compare(a, d);
    MESSAGE(name << " path: the author's frame against the derived one with the map turned over, "
                 << same.both << " pixels, worst " << same.worst
                 << "; against the derived one with the same map, " << differ.apart << " of "
                 << differ.both << " pixels more than 8 apart (worst " << differ.worst << ")");
    CHECK(same.both > 2000);
    CHECK(same.worst <= 1);
    CHECK(differ.apart > differ.both / 2);
    if (!have_reference) {
      reference = a;
      have_reference = true;
    } else {
      const Comparison paths = compare(reference, a);
      MESSAGE(name << " path against the vertex path: " << paths.both << " pixels on the same "
                   << "triangle, worst " << paths.worst);
      CHECK(paths.both > 2000);
      CHECK(paths.worst <= 1);
    }
  }
  device.destroy();
}

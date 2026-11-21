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
// bitangent cross(N, T) * w is +y, -dP/dv — **the frame the resolve derives from the UVs**, which
// is glTF's handedness (docs/experiments/tangent-space-convention.md), and what the first leg
// measured on nearly every corner of five of the Khronos samples (assets.md, "What G2 reads").
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
  for (u32 i = 0; i < 4; ++i)
    bin.push_back(0);  // 140 to 144, so the tangents start on a 4-byte boundary
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

// A curved patch: a quarter of a cylinder of radius 1.5 about the y axis bulging towards +z, eight
// columns from -45 to +45 degrees and two rows from y = 1 down to y = -1, UV (0, 0) at its top
// left as glTF's origin puts an image on it, its vertex normals the cylinder's and its tangents
// what an exporter writes for them: T = d/dtheta, (cos, 0, -sin), w = +1, so the bitangent
// cross(N, T) is +y. Across a triangle the author's frame turns with the interpolated tangent
// while the derived one is the chord's, so the two differ a little, by the facet; `green` is the
// map's lean along the bitangent (191 one way, 64 the other) with a fixed lean along the tangent.
bool write_normal_patch(const std::filesystem::path& dir, bool tangents, u8 green) {
  std::filesystem::create_directories(dir);
  constexpr u32 k_columns = 9;
  constexpr u32 k_rows = 3;
  constexpr f32 k_radius = 1.5f;
  std::vector<u8> bin;
  std::vector<f32> positions;
  std::vector<f32> normals;
  std::vector<f32> uvs;
  std::vector<f32> tangent_values;
  for (u32 j = 0; j < k_rows; ++j) {
    for (u32 i = 0; i < k_columns; ++i) {
      const f32 theta = radians(-45.0f + 90.0f * static_cast<f32>(i) / (k_columns - 1));
      const f32 s = std::sin(theta);
      const f32 c = std::cos(theta);
      const f32 y = 1.0f - 2.0f * static_cast<f32>(j) / (k_rows - 1);
      positions.insert(positions.end(), {k_radius * s, y, k_radius * c - k_radius});
      normals.insert(normals.end(), {s, 0.0f, c});
      uvs.insert(uvs.end(), {static_cast<f32>(i) / (k_columns - 1),
                             static_cast<f32>(j) / (k_rows - 1)});
      tangent_values.insert(tangent_values.end(), {c, 0.0f, -s, 1.0f});
    }
  }
  std::vector<u16> indices;
  for (u32 j = 0; j + 1 < k_rows; ++j) {
    for (u32 i = 0; i + 1 < k_columns; ++i) {
      const u16 a = static_cast<u16>(j * k_columns + i);
      const u16 b = static_cast<u16>(a + 1);
      const u16 d = static_cast<u16>(a + k_columns);
      const u16 e = static_cast<u16>(d + 1);
      indices.insert(indices.end(), {a, d, e, a, e, b});
    }
  }
  for (const f32 v : positions)
    put_f32(bin, v);
  for (const f32 v : normals)
    put_f32(bin, v);
  for (const f32 v : uvs)
    put_f32(bin, v);
  const u32 index_at = static_cast<u32>(bin.size());
  for (const u16 i : indices) {
    bin.push_back(static_cast<u8>(i & 0xff));
    bin.push_back(static_cast<u8>(i >> 8));
  }
  while (bin.size() % 4 != 0)
    bin.push_back(0);
  const u32 tangent_at = static_cast<u32>(bin.size());
  for (const f32 v : tangent_values)
    put_f32(bin, v);
  if (!write_file((dir / "patch.bin").string(), bin.data(), bin.size())) return false;
  std::vector<u8> rgba(4 * 4 * 4);
  for (u32 i = 0; i < 16; ++i) {
    rgba[i * 4 + 0] = 170;
    rgba[i * 4 + 1] = green;
    rgba[i * 4 + 2] = 218;
    rgba[i * 4 + 3] = 255;
  }
  Vector<u8> png;
  if (!image::encode_png(4, 4, 4, std::span<const u8>(rgba.data(), rgba.size()), png)) return false;
  if (!write_file((dir / "normal.png").string(), png.data(), png.size())) return false;
  const u32 vertex_count = k_columns * k_rows;
  const std::string n = std::to_string(vertex_count);
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
      "{\"bufferView\":0,\"componentType\":5126,\"count\":" + n +
      ",\"type\":\"VEC3\",\"min\":[-1.0607,-1,-0.4394],\"max\":[1.0607,1,0]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":" + n + ",\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":" + n + ",\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5123,\"count\":" + std::to_string(indices.size()) +
      ",\"type\":\"SCALAR\"},"
      "{\"bufferView\":4,\"componentType\":5126,\"count\":" + n + ",\"type\":\"VEC4\"}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      std::to_string(vertex_count * 12) + "},"
      "{\"buffer\":0,\"byteOffset\":" + std::to_string(vertex_count * 12) +
      ",\"byteLength\":" + std::to_string(vertex_count * 12) + "},"
      "{\"buffer\":0,\"byteOffset\":" + std::to_string(vertex_count * 24) +
      ",\"byteLength\":" + std::to_string(vertex_count * 8) + "},"
      "{\"buffer\":0,\"byteOffset\":" + std::to_string(index_at) +
      ",\"byteLength\":" + std::to_string(indices.size() * 2) + "},"
      "{\"buffer\":0,\"byteOffset\":" + std::to_string(tangent_at) +
      ",\"byteLength\":" + std::to_string(vertex_count * 16) + "}],"
      "\"buffers\":[{\"uri\":\"patch.bin\",\"byteLength\":" + std::to_string(bin.size()) + "}]}";
  return write_file((dir / "patch.gltf").string(), json.data(), json.size());
}

// One quad's scene, drawn shaded by one raster path with no shadows (a shadow is not what the
// paths or the frames have to agree about).
struct StreamRig {
  SceneData data;
  GpuScene scene;
  SceneRenderer renderer;
  bool made = false;

  // False with `skip` set when the device has no such path. `stream`: the scene laid out in pages
  // and streamed, which uploads no imported stream (gpu_scene.cpp, `upload_geometry`).
  bool build(const gfx::Device& device, const std::string& mesh, RasterMode mode,
             std::string& skip, bool stream = false) {
    std::string error;
    SceneDesc desc;
    desc.meshes.push_back(mesh);
    desc.cache = false;
    desc.stream = stream;
    REQUIRE_MESSAGE(load_scene(desc, data, error), "scene: " << error);
    RenderSettings settings;
    settings.raster = mode;
    settings.shadows = ShadowMode::Off;
    settings.stream = stream;
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
    if (scene.streamed()) {
      // Frames until nothing is queued, staged or on its way, four in a row (streaming_tests.cpp,
      // `converge`), so the capture draws every page the cut asked for.
      std::string error;
      u32 quiet = 0;
      for (u32 f = 0; f < 64 && quiet < 4; ++f) {
        frame.frame_index = f;
        REQUIRE_MESSAGE(renderer.render_offscreen(frame, &error), error);
        const StreamStats& stream = renderer.streamer().stats();
        quiet = stream.pending == 0 && !renderer.streamer().has_uploads() &&
                        stream.loads_in_flight == 0
                    ? quiet + 1
                    : 0;
      }
      CHECK(quiet >= 4);
      frame.frame_index = 64;
    }
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
// tangents and the same map; no tangents and the map leaning the other way. The author's frame is
// the derived one, so the first and the second must be one picture and the third a picture
// apart — on the vertex path, the mesh path and the ray path alike, and the three paths
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
    const std::string name = mode == RasterMode::Vertex     ? "vertex"
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
    const Comparison same = compare(a, d);
    const Comparison differ = compare(a, f);
    MESSAGE(name << " path: the author's frame against the derived one with the same map, "
                 << same.both << " pixels, worst " << same.worst
                 << "; against the derived one with the map turned over, " << differ.apart << " of "
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

// **The picture is a function of the asset, not of how it reached the scene** (G2): one curved
// patch whose file states its tangents, loaded with the scene — the imported stream uploaded, the
// author's interpolated frame drawn — and loaded streamed, which uploads no stream, so the resolve
// derives its frame per triangle from the paged positions and UVs. The two must show the same
// relief with the same sign: they may differ only by the facet, the author's frame turning across
// a triangle where the derived one is the chord's, and that difference is bounded here. The same
// patch streamed with its map's green turned over says how far apart a sign is.
TEST_CASE("renderer: an asset draws one relief whether its tangents reach the scene or not") {
  const test::TempDir tmp("engine_renderer_tangent_routes");
  const std::filesystem::path root = tmp.native();
  const std::string authored = (root / "authored" / "patch.gltf").string();
  const std::string flipped = (root / "flipped" / "patch.gltf").string();
  REQUIRE(write_normal_patch(root / "authored", true, 191));
  REQUIRE(write_normal_patch(root / "flipped", true, 64));

  gfx::Device device;
  std::string error;
  if (!device.create(renderer_test::device_options(), &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  u32 ran = 0;
  for (const RasterMode mode : {RasterMode::Vertex, RasterMode::Hardware}) {
    const std::string name = mode == RasterMode::Vertex ? "vertex" : "mesh";
    INFO(name);
    CapturedFrame whole;
    CapturedFrame paged;
    CapturedFrame paged_flipped;
    {
      StreamRig rig;
      std::string skip;
      if (!rig.build(device, authored, mode, skip)) {
        MESSAGE("the " << name << " path is not here: " << skip);
        continue;
      }
      REQUIRE_FALSE(rig.scene.streamed());
      CHECK(rig.scene.extra_vertices != 0);
      whole = rig.shot();
    }
    {
      StreamRig rig;
      std::string skip;
      if (!rig.build(device, authored, mode, skip, true)) {
        MESSAGE("the " << name << " path does not stream here: " << skip);
        continue;
      }
      REQUIRE(rig.scene.streamed());
      CHECK(rig.scene.extra_vertices == 0);  // a streamed scene carries no imported stream
      CHECK(rig.scene.extra.address == 0);
      paged = rig.shot();
    }
    {
      StreamRig rig;
      std::string skip;
      REQUIRE(rig.build(device, flipped, mode, skip, true));
      paged_flipped = rig.shot();
    }
    ++ran;
    // The ids differ between a whole and a paged scene (its clusters are renumbered), so the
    // pictures are compared where both drew the patch.
    u32 both = 0;
    int worst = 0;
    u32 apart_same = 0;
    u32 apart_flipped = 0;
    u64 sum_same = 0;
    for (u32 p = 0; p < whole.width * whole.height; ++p) {
      const u32 w = p * k_id_words;
      if (whole.ids[w] == k_no_id || paged.ids[w] == k_no_id || paged_flipped.ids[w] == k_no_id)
        continue;
      ++both;
      int d = 0;
      int f = 0;
      for (u32 ch = 0; ch < 3; ++ch) {
        d = std::max(d, std::abs(int{whole.color[p * 4 + ch]} - int{paged.color[p * 4 + ch]}));
        f = std::max(f,
                     std::abs(int{whole.color[p * 4 + ch]} - int{paged_flipped.color[p * 4 + ch]}));
      }
      worst = std::max(worst, d);
      sum_same += static_cast<u64>(d);
      if (d > 8) ++apart_same;
      if (f > 8) ++apart_flipped;
    }
    MESSAGE(name << " path, the curved patch: " << both << " pixels; loaded whole against "
                 << "streamed, worst " << worst << " of 255, mean "
                 << (both != 0 ? static_cast<f64>(sum_same) / both : 0.0) << ", " << apart_same
                 << " more than 8 apart; against streamed with the green turned over, "
                 << apart_flipped << " more than 8 apart");
    CHECK(both > 3000);
    // The facet's difference, measured on the RTX 5090 at 1 of 255 on the vertex and the mesh
    // path (mean 0.001; R202): the bound with a code's margin. A sign turned over is up to 128
    // codes, and more than 8 on every pixel of the patch.
    CHECK(worst <= 2);
    CHECK(apart_same == 0);
    CHECK(apart_flipped > both / 2);
  }
  CHECK(ran > 0);
  device.destroy();
}

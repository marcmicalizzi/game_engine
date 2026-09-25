// Scenes, camera paths and flythroughs (docs/plan/09-testing-profiling.md §9.4,
// docs/subsystems/renderer.md "Scenes, camera paths and flythroughs"). The cases in the first half
// need no device: the camera path's arithmetic, the terrain's height function, the scene file's
// grammar — its oldest spelling, placements on the ground, scatters, fits, content hashes and the
// overlay — and the percentiles a run is summarized by. The second half flies a small scene built
// here (a terrain with a ridge, cubes hidden behind it, a path that rises over it) and holds the
// two properties the corpus leans on: the same path gives the same visible pairs frame by frame,
// and occlusion culling changes no pixel anywhere on it while it does cull the hidden cubes.
#include <core/math/math.h>
#include <domain/assets/gltf.h>
#include <domain/gfx/device.h>
#include <domain/texture/texture_build.h>
#include <systems/renderer/camera_path.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/flythrough.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/terrain.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
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

void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  put_u32(out, bits);
}

std::string n(u32 v) { return std::to_string(v); }

// A unit cube as a GLB with normals, one material and 12 triangles wound counter-clockwise seen
// from outside: the renderer tests' fixture, minus the morph target.
bool write_cube_glb(const std::string& path) {
  const Vec3 normals[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                           Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
  const Vec3 tangents[6] = {Vec3{0, 1, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1},
                            Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{1, 0, 0}};
  std::vector<u8> bin;
  for (u32 f = 0; f < 6; ++f) {
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
  const u32 index_offset = static_cast<u32>(bin.size());
  for (u32 f = 0; f < 6; ++f) {
    const u32 base = f * 4;
    const u32 tris[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
    for (const u32 i : tris)
      put_u32(bin, i);
  }
  const u32 index_bytes = static_cast<u32>(bin.size()) - index_offset;
  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},"
      "\"indices\":2,\"material\":0}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.5,0.2,1],"
      "\"metallicFactor\":0,\"roughnessFactor\":0.6}}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\","
      "\"min\":[-0.5,-0.5,-0.5],\"max\":[0.5,0.5,0.5]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5125,\"count\":36,\"type\":\"SCALAR\"}],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      n(normal_offset) + "},{\"buffer\":0,\"byteOffset\":" + n(normal_offset) +
      ",\"byteLength\":" + n(index_offset - normal_offset) +
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

// Two flat grids of 24 x 24 quads side by side, x < 0 and x > 0, as two primitives with a
// material each: a mesh with a real DAG (nine or ten leaves a primitive, and the levels above
// them) whose every cluster's material can be told from where its vertices are.
bool write_two_material_glb(const std::string& path) {
  constexpr u32 k_quads = 24;
  constexpr u32 k_side = k_quads + 1;
  constexpr u32 k_vertices = k_side * k_side;
  constexpr u32 k_indices = k_quads * k_quads * 6;
  std::vector<u8> bin;
  u32 position_offset[2] = {};
  u32 normal_offset[2] = {};
  u32 index_offset[2] = {};
  for (u32 p = 0; p < 2; ++p) {
    const f32 x0 = p == 0 ? -2.05f : 0.05f;
    position_offset[p] = static_cast<u32>(bin.size());
    for (u32 z = 0; z < k_side; ++z) {
      for (u32 x = 0; x < k_side; ++x) {
        put_f32(bin, x0 + 2.0f * static_cast<f32>(x) / k_quads);
        put_f32(bin, 0.0f);
        put_f32(bin, -1.0f + 2.0f * static_cast<f32>(z) / k_quads);
      }
    }
    normal_offset[p] = static_cast<u32>(bin.size());
    for (u32 v = 0; v < k_vertices; ++v) {
      put_f32(bin, 0.0f);
      put_f32(bin, 1.0f);
      put_f32(bin, 0.0f);
    }
    index_offset[p] = static_cast<u32>(bin.size());
    for (u32 z = 0; z < k_quads; ++z) {
      for (u32 x = 0; x < k_quads; ++x) {
        const u32 a = z * k_side + x;
        for (const u32 i : {a, a + k_side, a + 1, a + 1, a + k_side, a + k_side + 1})
          put_u32(bin, i);
      }
    }
  }
  const u32 position_bytes = k_vertices * 12;
  const u32 index_bytes = k_indices * 4;
  std::string views;
  std::string accessors;
  for (u32 p = 0; p < 2; ++p) {
    const f32 x0 = p == 0 ? -2.05f : 0.05f;
    if (p != 0) {
      views += ",";
      accessors += ",";
    }
    views += "{\"buffer\":0,\"byteOffset\":" + n(position_offset[p]) +
             ",\"byteLength\":" + n(position_bytes) +
             "},{\"buffer\":0,\"byteOffset\":" + n(normal_offset[p]) +
             ",\"byteLength\":" + n(position_bytes) +
             "},{\"buffer\":0,\"byteOffset\":" + n(index_offset[p]) +
             ",\"byteLength\":" + n(index_bytes) + "}";
    accessors += "{\"bufferView\":" + n(p * 3) +
                 ",\"componentType\":5126,\"count\":" + n(k_vertices) +
                 ",\"type\":\"VEC3\",\"min\":[" + std::to_string(x0) + ",0,-1],\"max\":[" +
                 std::to_string(x0 + 2.0f) + ",0,1]},{\"bufferView\":" + n(p * 3 + 1) +
                 ",\"componentType\":5126,\"count\":" + n(k_vertices) +
                 ",\"type\":\"VEC3\"},{\"bufferView\":" + n(p * 3 + 2) +
                 ",\"componentType\":5125,\"count\":" + n(k_indices) + ",\"type\":\"SCALAR\"}";
  }
  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":["
      "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":2,\"material\":0},"
      "{\"attributes\":{\"POSITION\":3,\"NORMAL\":4},\"indices\":5,\"material\":1}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.5,0.2,1]}},"
      "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.2,0.5,0.8,1]}}],"
      "\"accessors\":[" +
      accessors + "],\"bufferViews\":[" + views +
      "],\"buffers\":[{\"byteLength\":" + n(static_cast<u32>(bin.size())) + "}]}";
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

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

std::string slashes(const std::filesystem::path& p) {
  std::string s = p.string();
  for (char& c : s) {
    if (c == '\\') c = '/';
  }
  return s;
}

bool near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }
bool near(const Vec3& a, const Vec3& b, f32 eps = 1e-4f) {
  return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

// The small world the GPU cases fly over: a 64 m terrain with an 8 m ridge across it at z = 0,
// three cubes (fitted to 3 m) standing on the ground 8 m behind it, and a camera that holds low
// behind the ridge for a quarter of a second and then rises over it. From the low eye the cubes
// sit 4 degrees up and the ridge 25, so they are hidden; from the high one they are in view.
constexpr const char* k_scene_json = R"({
  "format": "engine.scene.v1",
  "name": "ridge-and-cubes",
  "meshes": [{"name": "cube", "path": "cube.glb", "fit": {"height": 3}}],
  "instances": [
    {"mesh": 0, "translation": [-6, 0, -8], "ground": true},
    {"mesh": 0, "translation": [0, 0, -8], "ground": true, "yaw_deg": 30},
    {"mesh": 0, "translation": [6, 0, -8], "ground": true}
  ],
  "terrain": {
    "size": 65, "extent": 32, "seed": 3, "dune_height": 0.3, "dune_wavelength": 8,
    "ridges": [{"from": [-30, 0], "to": [30, 0], "height": 8, "width": 6, "roughness": 0.1}]
  }
})";

constexpr const char* k_path_json = R"({
  "format": "engine.camera-path.v1",
  "name": "over-the-ridge",
  "interpolation": "Smooth",
  "fps": 24,
  "keys": [
    {"time": 0, "position": [0, 1.5, 14], "ground": true, "target": [0, 2, -8]},
    {"time": 0.25, "position": [0, 1.5, 14], "ground": true, "target": [0, 2, -8]},
    {"time": 1.0, "position": [0, 14, 7], "target": [0, 1, -8]}
  ],
  "markers": [{"frame": 3, "name": "hidden"}, {"frame": 24, "name": "in view"}]
})";

struct World {
  test::TempDir tmp{"engine_renderer_flythrough"};
  std::string scene_file;
  SceneDesc desc;
  std::string error;

  bool write() {
    const std::filesystem::path dir = tmp.native();
    scene_file = slashes(dir / "scene.json");
    if (!write_cube_glb(slashes(dir / "cube.glb")) || !write_text(scene_file, k_scene_json)) {
      error = "cannot write the fixture";
      return false;
    }
    if (!read_scene_file(scene_file, desc, error)) return false;
    desc.ddc = slashes(dir / "ddc");
    return true;
  }
};

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
      error = "unavailable";
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = width;
    rd.height = height;
    return renderer.create(device, scene, resolved, rd, &error);
  }
};

// The path flown the way a benchmark flies it — frames kept in flight, `Stats::last` read after
// every `begin_frame()`, and the frames in flight drained at the end — returning each frame's
// visible pairs in path order. `frames_seen` is the frame index of each record in arrival order.
bool fly(Rig& rig, const CameraPath& path, Vector<u32>& pairs, Vector<u64>& frames_seen,
         std::string& error) {
  const u32 frames = path.frame_count();
  pairs.assign(frames, ~u32{0});
  frames_seen.clear();
  rig.renderer.reset_stats();
  u64 folded = rig.renderer.stats().folded;
  constexpr u32 k_drain = 2;  // SceneRenderer::Desc::frames_in_flight
  for (u32 i = 0; i < frames + k_drain; ++i) {
    rig.renderer.begin_frame();
    const Stats& stats = rig.renderer.stats();
    if (stats.folded != folded) {
      folded = stats.folded;
      if (stats.last.submission < frames) {
        pairs[static_cast<u32>(stats.last.submission)] = stats.last.visible_pairs();
        frames_seen.push_back(stats.last.frame);
      }
    }
    FrameDesc frame;
    const u32 f = i < frames ? i : frames - 1;
    frame.camera = camera_path_frame(path, f, frames);
    frame.frame_index = f;
    if (rig.renderer.submit_frame(frame, &error) == 0) return false;
  }
  rig.renderer.wait_idle();
  return true;
}

}  // namespace

TEST_CASE("camera path: keys are hit exactly, and linear and smooth differ only between them") {
  const char* text = R"({"name":"p","interpolation":"Linear","fps":10,"znear":0.25,
    "keys":[{"time":0,"position":[0,0,0],"target":[0,0,-10],"fov_deg":50},
            {"time":1,"position":[10,0,0],"target":[10,0,-10],"fov_deg":70},
            {"time":3,"position":[10,0,20],"rotation":[0,0,0,1],"fov_deg":70}],
    "markers":[{"frame":30,"name":"end"}]})";
  CameraPath path;
  std::string error;
  REQUIRE_MESSAGE(parse_camera_path(text, nullptr, path, error), error);
  CHECK(path.frame_count() == 31);
  CHECK(path.duration() == doctest::Approx(3.0));
  CHECK(path.znear == doctest::Approx(0.25f));
  CHECK(path.hash != 0);
  // An orientation key becomes a point 100 m ahead of it, down -z.
  CHECK(near(path.keys[2].target, Vec3{10, 0, -80}));
  const Camera half = sample_camera_path(path, 0.5);
  CHECK(near(half.position, Vec3{5, 0, 0}));
  CHECK(near(half.fov_y, radians(60.0f)));
  // Frame f of the native count is time f / fps.
  CHECK(near(camera_path_frame(path, 5, path.frame_count()).position, Vec3{5, 0, 0}));
  CHECK(near(camera_path_frame(path, 30, path.frame_count()).position, Vec3{10, 0, 20}));

  CameraPath smooth;
  std::string smooth_text = text;
  smooth_text.replace(smooth_text.find("Linear"), 6, "Smooth");
  REQUIRE_MESSAGE(parse_camera_path(smooth_text, nullptr, smooth, error), error);
  for (const CameraPathKey& key : smooth.keys) {
    const Camera at = sample_camera_path(smooth, key.time);
    CHECK(near(at.position, key.position));
    CHECK(near(at.target, key.target));
  }
  // Between the keys the cubic leaves the straight line, because it has to arrive at the second
  // key already turning towards the third.
  CHECK(std::fabs(sample_camera_path(smooth, 0.5).position.z) > 0.01f);
  // Past either end the camera holds.
  CHECK(near(sample_camera_path(smooth, -1.0).position, Vec3{0, 0, 0}));
  CHECK(near(sample_camera_path(smooth, 9.0).position, Vec3{10, 0, 20}));

  // What is refused, each with a sentence.
  CHECK_FALSE(parse_camera_path(R"({"keys":[]})", nullptr, path, error));
  CHECK_FALSE(
      parse_camera_path(R"({"keys":[{"time":1,"position":[0,0,0]},{"time":1,"position":[1,0,0]}]})",
                        nullptr, path, error));
  CHECK(error.find("increase") != std::string::npos);
  CHECK_FALSE(parse_camera_path(R"({"keys":[{"time":0,"position":[0,0,0],"ground":true}]})",
                                nullptr, path, error));
  CHECK(error.find("terrain") != std::string::npos);
  CHECK_FALSE(parse_camera_path(
      R"({"keys":[{"time":0,"position":[0,0,0]}],"markers":[{"frame":5,"name":"late"}]})", nullptr,
      path, error));
  CHECK_FALSE(parse_camera_path(R"({"keys":[{"time":0,"position":[0,0,0],"bogus":1}]})", nullptr,
                                path, error));
}

TEST_CASE("terrain: the height is a function of the fields, and the hash names them") {
  TerrainDesc desc;
  desc.enabled = true;
  desc.size = 33;
  desc.extent = 100.0f;
  desc.seed = 7;
  desc.dune_height = 2.0f;
  desc.ridges.push_back(TerrainRidge{Vec2{-50, 0}, Vec2{50, 0}, 30.0f, 20.0f, 0.0f});
  desc.basins.push_back(TerrainBasin{Vec2{0, 60}, 25.0f, 6.0f});
  // Deterministic, and the features are where the fields put them: the ridge's crest stands at
  // its height over the (damped) dunes, the basin's floor at its depth under them.
  CHECK(terrain_height(desc, 3.0f, 4.0f) == terrain_height(desc, 3.0f, 4.0f));
  CHECK(terrain_height(desc, 0.0f, 0.0f) > 25.0f);
  CHECK(terrain_height(desc, 0.0f, 0.0f) < 36.0f);
  CHECK(terrain_height(desc, 0.0f, 60.0f) < -4.0f);
  CHECK(std::fabs(terrain_height(desc, 80.0f, -80.0f)) < 3.0f);  // dunes alone
  CHECK(terrain_material(desc, 0.0f, 0.0f) == 1u);               // rock
  CHECK(terrain_material(desc, 0.0f, 60.0f) == 3u);              // the basin's floor
  CHECK(terrain_material(desc, 80.0f, -80.0f) == 0u);            // sand
  // The dunes are dunes: over a few wavelengths away from every feature the ground moves by a
  // good part of `dune_height`, and it is not one plane.
  f32 lo = 1e30f;
  f32 hi = -1e30f;
  for (u32 i = 0; i < 200; ++i) {
    const f32 h = terrain_height(desc, -100.0f + static_cast<f32>(i), -90.0f);
    lo = std::min(lo, h);
    hi = std::max(hi, h);
  }
  MESSAGE("dune relief over 200 m: " << lo << " .. " << hi);
  CHECK(hi - lo > 0.5f * desc.dune_height);
  const u64 hash = terrain_hash(desc);
  TerrainDesc other = desc;
  other.seed = 8;
  CHECK(terrain_hash(other) != hash);
  other = desc;
  other.ridges[0].height = 31.0f;
  CHECK(terrain_hash(other) != hash);
  CHECK(terrain_hash(desc) == hash);

  Vector<Vec3> positions;
  Vector<u32> indices;
  Vector<Vec2> uvs;
  std::string error;
  REQUIRE(build_terrain_mesh(desc, positions, indices, uvs, &error));
  CHECK(positions.size() == 33u * 33u);
  CHECK(indices.size() == 32u * 32u * 6u);
  CHECK(near(positions[0], Vec3{-100, terrain_height(desc, -100, -100), -100}));
  // Counter-clockwise seen from +y: the first triangle's normal points up.
  const Vec3 a = positions[indices[0]];
  const Vec3 b = positions[indices[1]];
  const Vec3 c = positions[indices[2]];
  CHECK(cross(b - a, c - a).y > 0.0f);
  desc.size = 1;
  CHECK_FALSE(build_terrain_mesh(desc, positions, indices, uvs, &error));
}

TEST_CASE("terrain: a sampler's heights are the direct function's and the mesh's, bit for bit") {
  // The scene read, the ruins it assembles and a camera path ask the ground through one
  // `TerrainSampler` rather than `terrain_height`, which draws the dune field on every call; a
  // height that moved by one ulp would move a building's base centimetre and every hash pinned on
  // it, so the two are compared as bits over a grid that crosses a ridge, a basin and plain dunes,
  // and against the mesh's own vertices.
  TerrainDesc desc;
  desc.enabled = true;
  desc.size = 65;
  desc.extent = 120.0f;
  desc.seed = 23;
  desc.dune_height = 8.0f;
  desc.dune_wavelength = 110.0f;
  desc.ridges.push_back(TerrainRidge{Vec2{-100, 20}, Vec2{90, -30}, 55.0f, 40.0f, 0.2f});
  desc.basins.push_back(TerrainBasin{Vec2{30, 60}, 45.0f, 9.0f});
  const TerrainSampler sampler(desc);
  u32 differing = 0;
  for (i32 zi = -60; zi <= 60; ++zi) {
    for (i32 xi = -60; xi <= 60; ++xi) {
      const f32 x = static_cast<f32>(xi) * 2.1f + 0.37f;
      const f32 z = static_cast<f32>(zi) * 1.9f - 0.61f;
      differing +=
          std::bit_cast<u32>(sampler.height(x, z)) != std::bit_cast<u32>(terrain_height(desc, x, z))
              ? 1u
              : 0u;
      differing += std::bit_cast<u32>(sampler.ridge_weight(x, z)) !=
                           std::bit_cast<u32>(terrain_ridge_weight(desc, x, z))
                       ? 1u
                       : 0u;
      differing += std::bit_cast<u32>(sampler.basin_weight(x, z)) !=
                           std::bit_cast<u32>(terrain_basin_weight(desc, x, z))
                       ? 1u
                       : 0u;
    }
  }
  CHECK(differing == 0u);
  Vector<Vec3> positions;
  Vector<u32> indices;
  Vector<Vec2> uvs;
  std::string error;
  REQUIRE(build_terrain_mesh(desc, positions, indices, uvs, &error));
  u32 off_mesh = 0;
  for (const Vec3& p : positions)
    off_mesh += std::bit_cast<u32>(sampler.height(p.x, p.z)) != std::bit_cast<u32>(p.y) ? 1u : 0u;
  CHECK(off_mesh == 0u);
}

TEST_CASE("terrain: the ground's maps are its surface, blended across each feature's edge") {
  // The terrain's colour is a function of the point, baked into two maps over its UVs, and no
  // longer a material voted per cluster (terrain.h, `terrain_surface`;
  // docs/experiments/second-interactive-session-2026-09-25.md): well inside a feature the surface
  // is that material's own values, across a feature's edge it changes over a band rather than at
  // a line, and every texel of the maps is the surface at the texel's centre.
  TerrainDesc desc;
  desc.enabled = true;
  desc.size = 33;
  desc.extent = 100.0f;
  desc.seed = 7;
  desc.dune_height = 2.0f;
  desc.ridges.push_back(TerrainRidge{Vec2{-50, 0}, Vec2{50, 0}, 30.0f, 20.0f, 0.0f});
  desc.basins.push_back(TerrainBasin{Vec2{0, 60}, 25.0f, 6.0f});
  const TerrainSampler field(desc);
  const TerrainSurface rock = terrain_surface(field, 0.0f, 0.0f);
  const TerrainSurface floor = terrain_surface(field, 0.0f, 60.0f);
  const TerrainSurface sand = terrain_surface(field, 80.0f, -80.0f);
  CHECK(near(rock.albedo, Vec3{0.47f, 0.39f, 0.32f}));
  CHECK(rock.roughness == doctest::Approx(0.78f));
  CHECK(near(floor.albedo, Vec3{0.36f, 0.40f, 0.22f}));
  CHECK(floor.roughness == doctest::Approx(0.85f));
  CHECK(near(sand.albedo, Vec3{0.84f, 0.69f, 0.47f}));
  CHECK(sand.roughness == doctest::Approx(0.92f));
  // From the ridge's crest out to the plain in 5 cm steps: the colour goes all the way from rock
  // to sand, and no step moves it by more than a small part of that.
  f32 largest_step = 0.0f;
  TerrainSurface previous = terrain_surface(field, 0.0f, 0.0f);
  for (u32 i = 1; i <= 400; ++i) {
    const TerrainSurface s = terrain_surface(field, 0.0f, -0.05f * static_cast<f32>(i));
    largest_step = std::max(largest_step, std::fabs(s.albedo.x - previous.albedo.x));
    previous = s;
  }
  CHECK(near(previous.albedo, sand.albedo, 1e-3f));
  MESSAGE("largest step of the red channel over 5 cm across the ridge's edge: " << largest_step);
  // Of a change of 0.37, which a border would make in one step. The band is 1.4 m wide on this
  // 20 m ridge and the smoothstep's steepest slope 1.5 times the band's mean, so about 0.02.
  CHECK(largest_step < 0.05f);

  const u32 side = terrain_map_side(desc);
  CHECK(side == 32u);
  Vector<u8> base_color;
  Vector<u8> metallic_roughness;
  bake_terrain_maps(desc, side, base_color, metallic_roughness);
  REQUIRE(base_color.size() == side * side * 4);
  REQUIRE(metallic_roughness.size() == side * side * 4);
  u32 wrong = 0;
  for (u32 row = 0; row < side; ++row) {
    for (u32 col = 0; col < side; ++col) {
      const f32 x = -desc.extent + 2.0f * desc.extent * (static_cast<f32>(col) + 0.5f) / side;
      const f32 z = -desc.extent + 2.0f * desc.extent * (static_cast<f32>(row) + 0.5f) / side;
      const TerrainSurface s = terrain_surface(field, x, z);
      const u32 o = (row * side + col) * 4;
      wrong += base_color[o] != texture::linear_to_srgb8(s.albedo.x) ? 1u : 0u;
      wrong += base_color[o + 2] != texture::linear_to_srgb8(s.albedo.z) ? 1u : 0u;
      wrong += base_color[o + 3] != 255 ? 1u : 0u;
      wrong += metallic_roughness[o] != 255 ? 1u : 0u;
      wrong += metallic_roughness[o + 1] != static_cast<u8>(s.roughness * 255.0f + 0.5f) ? 1u : 0u;
      wrong += metallic_roughness[o + 2] != 0 ? 1u : 0u;
    }
  }
  CHECK(wrong == 0u);
}

TEST_CASE(
    "scene file: the oldest spelling reads as it did; the new one grounds, scatters and fits") {
  const test::TempDir tmp("engine_renderer_scene_file");
  const std::filesystem::path dir = tmp.native();
  REQUIRE(write_cube_glb(slashes(dir / "cube.glb")));

  // The spelling engine-view's own tests have always written.
  const std::string old_file = slashes(dir / "old.json");
  REQUIRE(
      write_text(old_file,
                 R"({"meshes":[{"path":"cube.glb"}],"instances":[{"mesh":0,"translation":[-1,0,0]},
      {"mesh":0,"translation":[1,0,0],"rotation":[0,0,0,1],"scale":[1,2,1],
       "animation":{"phase":0.25}}]})"));
  SceneDesc old_desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(old_file, old_desc, error), error);
  REQUIRE(old_desc.meshes.size() == 1);
  CHECK(old_desc.meshes[0] == slashes(dir / "cube.glb"));
  REQUIRE(old_desc.instances.size() == 2);
  CHECK(near(old_desc.instances[1].transform.scale, Vec3{1, 2, 1}));
  CHECK(old_desc.instances[1].animation.play);
  CHECK(old_desc.instances[1].animation.phase == doctest::Approx(0.25f));
  CHECK_FALSE(old_desc.instances[0].animation.play);
  CHECK_FALSE(old_desc.terrain.enabled);
  CHECK(old_desc.file_hash != 0);
  CHECK(old_desc.camera_path.empty());  // a scene that names no path: the orbit

  // A typo is an error with the field's path, not a placement that quietly floats.
  const std::string typo = slashes(dir / "typo.json");
  REQUIRE(write_text(typo, R"({"meshes":[{"path":"cube.glb"}],"instances":[{"grund":true}]})"));
  SceneDesc refused;
  CHECK_FALSE(read_scene_file(typo, refused, error));
  CHECK(error.find("grund") != std::string::npos);

  // The cube's own bytes, which the scene may name and the load then insists on.
  u64 cube_hash = 0;
  REQUIRE(assets::source_mesh_hash(slashes(dir / "cube.glb"), cube_hash, &error));
  const std::string scene_file = slashes(dir / "scene.json");
  REQUIRE(write_text(scene_file, std::string(R"({"format":"engine.scene.v1","name":"t",
      "meshes":[{"name":"cube","path":"cube.glb","hash":")") +
                                     hash_hex(cube_hash) + R"(","fit":{"height":4}},
                {"name":"stand-in","path":"cube.glb","overlay":"00000000000000aa","fit":{"extent":2,"ground":false}}],
      "instances":[{"mesh":0,"translation":[10,1,-10],"ground":true,"yaw_deg":90},
                   {"mesh":1,"translation":[0,5,0]}],
      "scatters":[{"mesh":0,"center":[20,20],"radius_min":2,"radius_max":6,"count":5,"seed":9,
                   "scale_min":0.5,"scale_max":1.5}],
      "camera_path":"paths/over.json",
      "terrain":{"size":17,"extent":40,"seed":2,"ridges":[{"from":[-5,0],"to":[5,0],"height":6,"width":10}]}})"));
  SceneDesc desc;
  REQUIRE_MESSAGE(read_scene_file(scene_file, desc, error), error);
  CHECK(desc.name == "t");
  // The scene's own camera path, resolved against the scene file like a mesh path.
  CHECK(desc.camera_path == slashes(dir / "paths" / "over.json"));
  REQUIRE(desc.meshes.size() == 3);  // two files and the terrain, last
  CHECK(desc.meshes[2].empty());
  REQUIRE(desc.mesh_info.size() == 3);
  CHECK(desc.mesh_info[0].hash == cube_hash);
  CHECK(std::string(desc.mesh_info[1].origin) == "scene");
  CHECK(std::string(desc.mesh_info[2].origin) == "terrain");
  REQUIRE(desc.instances.size() == 2 + 5 + 1);
  const SceneInstance& grounded = desc.instances[0];
  CHECK(near(grounded.transform.position.y, 1.0f + terrain_height(desc.terrain, 10.0f, -10.0f)));
  CHECK(near(rotate(grounded.transform.rotation, Vec3{1, 0, 0}), Vec3{0, 0, -1}));
  for (u32 k = 2; k < 7; ++k) {
    const Vec3 p = desc.instances[k].transform.position;
    const f32 r = std::sqrt((p.x - 20.0f) * (p.x - 20.0f) + (p.z - 20.0f) * (p.z - 20.0f));
    CHECK(r >= 2.0f - 1e-3f);
    CHECK(r <= 6.0f + 1e-3f);
    CHECK(near(p.y, terrain_height(desc.terrain, p.x, p.z)));
    CHECK(desc.instances[k].transform.scale.x >= 0.5f);
    CHECK(desc.instances[k].transform.scale.x <= 1.5f);
  }
  CHECK(desc.instances[7].mesh == 2);
  // A second read places the scatter in exactly the same places.
  SceneDesc again;
  REQUIRE(read_scene_file(scene_file, again, error));
  for (u32 k = 0; k < desc.instances.size(); ++k)
    CHECK(desc.instances[k].transform.position == again.instances[k].transform.position);

  // Loading it fits both cubes: 4 m tall standing on its point, and 2 m wide centred on its own.
  desc.ddc = slashes(dir / "ddc");
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  CHECK(data.sources[0].source_hash == cube_hash);
  CHECK(data.sources[2].source_hash == terrain_hash(desc.terrain));
  CHECK(data.terrain.enabled);
  const gfx::InstanceDesc& tall = data.instances[0];
  CHECK(tall.scale_max == doctest::Approx(4.0f));
  const Vec3 base = transform_point(tall.world, Vec3{0.0f, -0.5f, 0.0f});
  CHECK(near(base, grounded.transform.position, 1e-3f));
  const gfx::InstanceDesc& wide = data.instances[1];
  CHECK(wide.scale_max == doctest::Approx(2.0f));
  CHECK(near(transform_point(wide.world, Vec3{0, 0, 0}), Vec3{0, 5, 0}, 1e-3f));

  // A hash the bytes do not have is refused, naming both.
  SceneDesc wrong = desc;
  wrong.mesh_info[0].hash = cube_hash ^ 1u;
  SceneData refused_data;
  CHECK_FALSE(load_scene(wrong, refused_data, error));
  CHECK(error.find(hash_hex(cube_hash)) != std::string::npos);

  // The overlay: a manifest naming the stand-in's overlay hash puts that file in its place, and
  // the load then insists on the manifest's hash — which this file does not have, so it fails,
  // and the file that does is accepted.
  const std::string manifest = slashes(dir / "overlay.json");
  REQUIRE(write_text(manifest, R"({"format":"engine.scene-overlay.v1","entries":[
      {"hash":"00000000000000aa","path":"landmark.glb","name":"landmark"}]})"));
  REQUIRE(write_cube_glb(slashes(dir / "landmark.glb")));
  SceneFileOptions options;
  options.overlay = manifest;
  SceneDesc overlaid;
  REQUIRE_MESSAGE(read_scene_file(scene_file, options, overlaid, error), error);
  CHECK(std::string(overlaid.mesh_info[1].origin) == "overlay");
  CHECK(overlaid.meshes[1] == slashes(dir / "landmark.glb"));
  overlaid.ddc = slashes(dir / "ddc");
  SceneData overlaid_data;
  CHECK_FALSE(load_scene(overlaid, overlaid_data, error));
  CHECK(error.find("00000000000000aa") != std::string::npos);
  REQUIRE(write_text(manifest, std::string(R"({"entries":[{"hash":")") + hash_hex(cube_hash) +
                                   R"(","path":"landmark.glb"}]})"));
  SceneDesc by_hash;
  REQUIRE(read_scene_file(scene_file, options, by_hash, error));
  CHECK(std::string(by_hash.mesh_info[1].origin) == "scene");  // its overlay hash is not listed
  CHECK(parse_hash_hex(hash_hex(cube_hash), cube_hash));
  u64 bad = 0;
  CHECK_FALSE(parse_hash_hex("xyz", bad));
  CHECK_FALSE(parse_hash_hex("00000000000000AA", bad));
}

TEST_CASE("flythrough: percentiles and the per-frame summary") {
  const f64 values[] = {5, 1, 4, 2, 3};
  const scene::Percentiles p = percentiles(std::span<const f64>(values, 5));
  CHECK(p.median == doctest::Approx(3.0));
  CHECK(p.max == doctest::Approx(5.0));
  CHECK(p.mean == doctest::Approx(3.0));
  CHECK(p.p95 == doctest::Approx(4.8));
  CHECK(percentiles({}).median == 0.0);

  CameraPath path;
  std::string error;
  REQUIRE(parse_camera_path(R"({"fps":2,"keys":[{"time":0,"position":[0,0,0]},
      {"time":1,"position":[1,0,0]}],"markers":[{"frame":2,"name":"end"}]})",
                            nullptr, path, error));
  REQUIRE(path.frame_count() == 3);
  // Three frames, three repeats; frame 1's second repeat is an outlier the median ignores, and
  // frame 2's third repeat drew a different cut, which is what `deterministic` exists to say.
  Vector<scene::FrameRecord> records;
  for (u32 r = 0; r < 3; ++r) {
    for (u32 f = 0; f < 3; ++f) {
      scene::FrameRecord record;
      record.repeat = r;
      record.frame = f;
      record.gpu_ms.total = 1.0 + f + (f == 1 && r == 1 ? 100.0 : 0.0);
      record.visible_pairs = 10 + f + (f == 2 && r == 2 ? 1u : 0u);
      record.uploads = 1;
      // The CPU's and the wall's milliseconds summarize like a pass does; ticks add up.
      record.cpu_ms = 0.5 * (f + 1);
      record.frame_ms = 16.0 + (f == 0 && r == 0 ? 50.0 : 0.0);
      record.ticks = 4;
      records.push_back(record);
    }
  }
  scene::FlythroughSummary summary;
  summarize_frames(std::span<const scene::FrameRecord>(records.data(), records.size()), 3, 3, path,
                   summary);
  CHECK(summary.gpu_ms.total.median == doctest::Approx(2.0));
  CHECK(summary.gpu_ms.total.max == doctest::Approx(3.0));
  CHECK_FALSE(summary.deterministic);
  CHECK(summary.mismatched_frames == 1);
  CHECK(summary.uploads == 9);
  CHECK(summary.cpu_ms.median == doctest::Approx(1.0));
  CHECK(summary.cpu_ms.max == doctest::Approx(1.5));
  CHECK(summary.frame_ms.max == doctest::Approx(16.0));  // one slow repeat is not the median
  CHECK(summary.ticks == 36);
  REQUIRE(summary.markers.size() == 1);
  CHECK(summary.markers[0].frame == 2);
  CHECK(summary.markers[0].total_ms == doctest::Approx(3.0));
  // A resampled run puts the marker on the frame that shares its time.
  CHECK(marker_frame(path, 2, 5) == 4);
  CHECK(marker_frame(path, 1, 5) == 2);
}

TEST_CASE("flythrough: a terrain comes back from its cache entry as the terrain it was built as") {
  World world;
  REQUIRE_MESSAGE(world.write(), world.error);
  SceneData first;
  REQUIRE_MESSAGE(load_scene(world.desc, first, world.error), world.error);
  const u32 terrain = first.parts.size() - 1;
  CHECK(std::string(first.sources[terrain].cache) == "miss");
  CHECK_FALSE(first.sources[terrain].container.empty());
  SceneData second;
  REQUIRE_MESSAGE(load_scene(world.desc, second, world.error), world.error);
  CHECK(std::string(second.sources[terrain].cache) == "hit");
  REQUIRE(first.cluster_count() == second.cluster_count());
  REQUIRE(first.lod.mesh.vertices.size() == second.lod.mesh.vertices.size());
  CHECK(std::memcmp(first.lod.mesh.vertices.data(), second.lod.mesh.vertices.data(),
                    first.lod.mesh.vertices.size() * sizeof(Vec3)) == 0);
  CHECK(first.sources[terrain].part_of_cluster == second.sources[terrain].part_of_cluster);
  // One material over the two baked maps, the images carried in the entry with their texture
  // records, so a hit samples the same maps the miss built.
  const SourceMesh& hit = second.sources[terrain];
  REQUIRE(hit.data.materials.size() == 1);
  CHECK(hit.data.materials[0].base_color_image == 0);
  CHECK(hit.data.materials[0].metallic_roughness_image == 1);
  REQUIRE(hit.data.images.size() == 2);
  CHECK(hit.data.images[0].bytes == first.sources[terrain].data.images[0].bytes);
  CHECK(hit.data.images[1].bytes == first.sources[terrain].data.images[1].bytes);
  CHECK(hit.textures.size() == 2);
}

TEST_CASE("scene: every cluster keeps its own material through the scene's merge") {
  // `merge_cluster_meshes` puts each mesh's leaves first, and a mesh that arrives in pages — every
  // glTF and container that goes through the cache — arrives leaves last, so the merge reorders
  // it. Until 2026-09-24 the per-cluster material map was not reordered with it and clusters drew
  // with other clusters' materials: patches of rock on the dunes and of metal on the
  // FlightHelmet's leather, changing whenever the LOD cut moved
  // (docs/experiments/first-interactive-session-2026-09-24.md). The fixture is a glTF of two
  // primitives side by side, so every cluster's material is known from where its vertices are,
  // and it is checked on both a cache miss and a hit. (The terrain was this test's fixture until
  // 2026-09-25, when its four per-cluster materials became one material over baked maps.)
  World world;
  REQUIRE_MESSAGE(world.write(), world.error);
  const std::string two = slashes(world.tmp.native() / "two.glb");
  REQUIRE(write_two_material_glb(two));
  SceneDesc desc;
  desc.ddc = world.desc.ddc;
  desc.meshes.push_back(slashes(world.tmp.native() / "cube.glb"));
  desc.meshes.push_back(two);
  desc.instances.push_back(SceneInstance{0, Transform3{}});
  desc.instances.push_back(SceneInstance{1, Transform3{}});
  for (u32 pass = 0; pass < 2; ++pass) {
    SceneData data;
    REQUIRE_MESSAGE(load_scene(desc, data, world.error), world.error);
    REQUIRE(data.parts.size() == 2);  // the cube and the two grids: the merge runs
    const geometry::ClusterMeshPart& part = data.parts[1];
    const SourceMesh& source = data.sources[1];
    CAPTURE(std::string(source.cache));
    CHECK(std::string(source.cache) == (pass == 0 ? "miss" : "hit"));
    REQUIRE(source.part_of_cluster.size() == part.cluster_count);
    REQUIRE(source.part_material.size() == 2);
    // The merge did reorder it: its run starts with its leaves, and the container's does not.
    CHECK(data.lod.lod[part.first_cluster].level == 0);
    u32 wrong = 0;
    u32 second = 0;
    u32 above_leaves = 0;
    for (u32 i = 0; i < part.cluster_count; ++i) {
      const geometry::ClusterDesc& c = data.lod.mesh.clusters[part.first_cluster + i];
      f32 x = 0.0f;
      for (u32 v = 0; v < c.vertex_count; ++v)
        x += data.lod.mesh.vertices[c.vertex_offset + v].x;
      const i32 expected = x > 0.0f ? 1 : 0;
      wrong += source.part_material[source.part_of_cluster[i]] != expected ? 1u : 0u;
      second += expected == 1 ? 1u : 0u;
      above_leaves += data.lod.lod[part.first_cluster + i].level > 0 ? 1u : 0u;
    }
    MESSAGE("clusters: " << part.cluster_count << ", second material " << second
                         << ", above the leaves " << above_leaves << ", wrong " << wrong);
    // Both materials and more than one level, or the check would prove nothing.
    CHECK(second > 0);
    CHECK(second < part.cluster_count);
    CHECK(above_leaves > 0);
    CHECK(wrong == 0);
  }
}

TEST_CASE("flythrough: the same path draws the same pairs, frame by frame, and says which frame") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  World world;
  REQUIRE_MESSAGE(world.write(), world.error);
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;  // occlusion culling on, which is what makes pairs history-
                                       // dependent and so the case worth asserting
  CameraPath path;
  Rig first;
  REQUIRE_MESSAGE(first.build(gpu.device, world.desc, settings, 160, 96), first.error);
  REQUIRE(first.resolved.occlusion);
  REQUIRE_MESSAGE(parse_camera_path(k_path_json, &first.data.terrain, path, world.error),
                  world.error);
  REQUIRE(path.frame_count() == 25);
  Vector<u32> a;
  Vector<u64> seen;
  REQUIRE_MESSAGE(fly(first, path, a, seen, world.error), world.error);
  // Every frame came back, in order, carrying its own frame index.
  REQUIRE(seen.size() == path.frame_count());
  for (u32 f = 0; f < seen.size(); ++f)
    CHECK(seen[f] == f);
  for (const u32 pairs : a)
    CHECK(pairs != ~u32{0});
  // A second rig, built from scratch, flies the same path to the same pairs.
  Rig second;
  REQUIRE_MESSAGE(second.build(gpu.device, world.desc, settings, 160, 96), second.error);
  Vector<u32> b;
  REQUIRE_MESSAGE(fly(second, path, b, seen, world.error), world.error);
  u32 differing = 0;
  for (u32 f = 0; f < a.size(); ++f)
    differing += a[f] != b[f] ? 1u : 0u;
  MESSAGE("visible pairs at frame 0 / 12 / 24: " << a[0] << " / " << a[12] << " / " << a[24]);
  CHECK(differing == 0);
  // And the same rig flown again draws them again, once the history of the first flight is gone:
  // the pairs of a frame are a function of the path, not of what the renderer drew before it.
  Vector<u32> c;
  REQUIRE_MESSAGE(fly(first, path, c, seen, world.error), world.error);
  u32 again = 0;
  for (u32 f = 3; f < a.size(); ++f)  // the first frames still see the last flight's history
    again += a[f] != c[f] ? 1u : 0u;
  CHECK(again == 0);
}

TEST_CASE("flythrough: occlusion culling changes no pixel on the path, and culls what is hidden") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  World world;
  REQUIRE_MESSAGE(world.write(), world.error);
  RenderSettings on;
  on.shadows = ShadowMode::Off;
  RenderSettings off = on;
  off.occlusion = false;
  constexpr u32 k_width = 170;  // a multiple of neither the 32-pixel tile nor anything else
  constexpr u32 k_height = 98;
  Rig with;
  Rig without;
  REQUIRE_MESSAGE(with.build(gpu.device, world.desc, on, k_width, k_height), with.error);
  REQUIRE_MESSAGE(without.build(gpu.device, world.desc, off, k_width, k_height), without.error);
  REQUIRE(with.resolved.occlusion);
  REQUIRE_FALSE(without.resolved.occlusion);
  CameraPath path;
  REQUIRE_MESSAGE(parse_camera_path(k_path_json, &with.data.terrain, path, world.error),
                  world.error);
  VisibleCensus census_with;
  VisibleCensus census_without;
  REQUIRE_MESSAGE(census_with.create(gpu.device, with.scene, &world.error), world.error);
  REQUIRE_MESSAGE(census_without.create(gpu.device, without.scene, &world.error), world.error);
  CaptureChannels channels;
  channels.ids = true;
  const u32 frames = path.frame_count();
  u32 frames_differing = 0;
  u64 surface = 0;
  u64 colour = 0;
  u32 hidden_with = 0;
  u32 hidden_without = 0;
  u32 cubes_last = 0;
  for (u32 f = 0; f < frames; ++f) {
    FrameDesc frame;
    frame.camera = camera_path_frame(path, f, frames);
    frame.frame_index = f;
    CapturedFrame a;
    CapturedFrame b;
    REQUIRE_MESSAGE(with.renderer.capture(frame, channels, a, &world.error), world.error);
    REQUIRE_MESSAGE(without.renderer.capture(frame, channels, b, &world.error), world.error);
    Vector<u32> levels;
    Vector<u32> meshes_with;
    Vector<u32> meshes_without;
    REQUIRE_MESSAGE(census_with.count(with.data, with.scene, with.renderer.stats(), levels,
                                      meshes_with, &world.error),
                    world.error);
    REQUIRE_MESSAGE(census_without.count(without.data, without.scene, without.renderer.stats(),
                                         levels, meshes_without, &world.error),
                    world.error);
    // Frames 3 to 6: the eye holds behind the ridge and the history has settled.
    if (f >= 3 && f <= 6) {
      hidden_with += meshes_with[0];
      hidden_without += meshes_without[0];
    }
    if (f + 1 == frames) cubes_last = meshes_with[0];
    u64 frame_surface = 0;
    u64 frame_colour = 0;
    for (u32 p = 0; p < k_width * k_height; ++p) {
      for (u32 c = 0; c < 4; ++c)
        frame_colour += a.color[p * 4 + c] != b.color[p * 4 + c] ? 1u : 0u;
      for (u32 w = 0; w < 2; ++w)
        frame_surface += a.ids[p * k_id_words + w] != b.ids[p * k_id_words + w] ? 1u : 0u;
    }
    frames_differing += frame_surface + frame_colour > 0 ? 1u : 0u;
    surface += frame_surface;
    colour += frame_colour;
  }
  MESSAGE(frames << " frames: " << frames_differing << " differ (" << surface << " surface, "
                 << colour << " colour); cube pairs while hidden " << hidden_with << " with, "
                 << hidden_without << " without; in view at the end " << cubes_last);
  CHECK(frames_differing == 0);
  CHECK(surface == 0);
  CHECK(colour == 0);
  // The case is only worth something if the cubes really are behind the ridge and really come
  // into view: culled while hidden, drawn without occlusion culling, and drawn at the end.
  CHECK(hidden_with < hidden_without);
  CHECK(cubes_last > 0);
}

namespace {

// The owner's terrain of the second session (fly-ashlar-100.json: seed 23, 8 m dunes, three
// ridges and the basin at the oasis) cropped to 1.28 km at 5 m a cell, which keeps the basin,
// the near ridge and the ground every marker looked at while building in a second, and the six
// cameras he pressed the marker key at, read from the recording's trajectory (tick 11,413 to
// 14,899; position, yaw and pitch in the fly camera's conventions, apps.md).
constexpr const char* k_basin_scene_json = R"({
  "format": "engine.scene.v1",
  "name": "basin-crop",
  "terrain": {
    "size": 257, "extent": 640, "seed": 23, "dune_height": 8, "dune_wavelength": 110,
    "ridges": [
      {"from": [-2200, 420], "to": [-100, 330], "height": 75, "width": 150, "roughness": 0.18},
      {"from": [100, 330], "to": [2200, 460], "height": 70, "width": 150, "roughness": 0.18},
      {"from": [-700, -260], "to": [520, -340], "height": 55, "width": 110, "roughness": 0.2}],
    "basins": [{"center": [0, 60], "radius": 200, "depth": 9}]
  }
})";

struct MarkerCamera {
  Vec3 position;
  f32 yaw;
  f32 pitch;
};

constexpr MarkerCamera k_owner_markers[6] = {
    {{-209.26375f, 26.024311f, -9.5677338f}, -1.8348248f, -0.18861327f},
    {{-162.20723f, 16.719624f, 3.1532950f}, -1.8348248f, -0.18861327f},
    {{-180.93501f, 20.422749f, -1.9094846f}, -1.8348248f, -0.18861327f},
    {{-161.49605f, 16.578999f, 3.3455529f}, -1.8348248f, -0.18861327f},
    {{-45.882092f, 9.8684425f, 37.036621f}, -1.8392248f, -0.036813259f},
    {{-29.020290f, 9.2243690f, 41.674911f}, -1.8392248f, -0.036813259f},
};

// The fly camera's orientation (yaw about +y, pitch about the right axis, forward -z at zero) as
// the renderer's position-and-target camera.
Camera marker_camera(const MarkerCamera& m) {
  Camera camera;
  camera.position = m.position;
  const Vec3 forward{-std::sin(m.yaw) * std::cos(m.pitch), std::sin(m.pitch),
                     -std::cos(m.yaw) * std::cos(m.pitch)};
  camera.target = m.position + forward * 100.0f;
  camera.znear = 0.05f;
  return camera;
}

}  // namespace

TEST_CASE("terrain: the ground's colour does not move when the LOD cut does") {
  // The second owner session (docs/experiments/second-interactive-session-2026-09-25.md): "green
  // and grey ground textures shift on different position thresholds". The terrain drew each
  // cluster with the material most of its vertices were, and a coarser cluster votes over a
  // larger patch, so the borders of the ridge's rock and the basin's green floor moved by whole
  // clusters whenever the cut changed. Its colour is now a map over its UVs, which every level
  // interpolates from the same source vertices, so a cut change may move the ground's colour
  // only as far as it moves the ground: the cluster LOD's declared error, a few pixels.
  //
  // At each of the owner's marker cameras the unlit albedo is drawn twice, from the cut of a
  // 1-pixel error threshold and of a 3-pixel one, and compared pixel by pixel where both frames
  // show the terrain. The cut must really change (else the case proves nothing), and the colour
  // may move by more than 8 of 255 only where the cut's own error moves the surface across a
  // blend — a sliver of the picture, against the patches the per-cluster materials gave.
  //
  // Measured on 2026-09-25 (RTX 5090): the cut changes under 8.7% of the terrain's pixels over
  // the six cameras. With the per-cluster vote put back, the colour moved on 1.32% of them —
  // 15% of the pixels whose cluster changed, 2.3% of the worst camera's terrain; with the maps,
  // on 0.013% — 0.15% of those whose cluster changed, 0.037% of the worst camera's. The bounds
  // sit a decade from each side.
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("engine_renderer_terrain_colour");
  const std::string scene_file = slashes(tmp.native() / "scene.json");
  REQUIRE(write_text(scene_file, k_basin_scene_json));
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(scene_file, desc, error), error);
  desc.ddc = slashes(tmp.native() / "ddc");
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  constexpr u32 k_width = 480;
  constexpr u32 k_height = 270;
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, desc, settings, k_width, k_height), rig.error);
  CaptureChannels channels;
  channels.ids = true;
  constexpr u32 k_empty = 0xFFFFFFFFu;
  u64 terrain_pixels = 0;
  u64 cut_changed = 0;
  u64 colour_moved = 0;
  f64 worst_marker = 0.0;
  for (u32 m = 0; m < 6; ++m) {
    CapturedFrame shot[2];
    const f32 thresholds[2] = {1.0f, 3.0f};
    for (u32 t = 0; t < 2; ++t) {
      FrameDesc frame;
      frame.camera = marker_camera(k_owner_markers[m]);
      frame.frame_index = 0;
      frame.view_mode = static_cast<u32>(gfx::ResolveMode::Albedo);
      frame.lod_px = thresholds[t];
      REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot[t], &error), error);
    }
    u64 covered = 0;
    u64 changed = 0;
    u64 moved = 0;
    for (u32 p = 0; p < k_width * k_height; ++p) {
      if (shot[0].ids[p * k_id_words] == k_empty || shot[1].ids[p * k_id_words] == k_empty)
        continue;
      ++covered;
      changed += shot[0].ids[p * k_id_words + 1] != shot[1].ids[p * k_id_words + 1] ? 1u : 0u;
      i32 largest = 0;
      for (u32 c = 0; c < 3; ++c) {
        largest = std::max(largest, std::abs(static_cast<i32>(shot[0].color[p * 4 + c]) -
                                             static_cast<i32>(shot[1].color[p * 4 + c])));
      }
      moved += largest > 8 ? 1u : 0u;
    }
    MESSAGE("marker " << m + 1 << ": " << covered << " terrain pixels, the cluster changed under "
                      << changed << ", the colour moved by more than 8 on " << moved);
    terrain_pixels += covered;
    cut_changed += changed;
    colour_moved += moved;
    if (covered > 0)
      worst_marker = std::max(worst_marker, static_cast<f64>(moved) / static_cast<f64>(covered));
  }
  REQUIRE(terrain_pixels > 0);
  const f64 changed_share = static_cast<f64>(cut_changed) / static_cast<f64>(terrain_pixels);
  const f64 moved_share = static_cast<f64>(colour_moved) / static_cast<f64>(terrain_pixels);
  MESSAGE("all six: the cut changed under " << changed_share * 100.0 << "% of the terrain, the "
                                            << "colour moved on " << moved_share * 100.0
                                            << "% (worst marker " << worst_marker * 100.0 << "%)");
  CHECK(changed_share > 0.04);  // the two thresholds really are two cuts
  CHECK(static_cast<f64>(colour_moved) / static_cast<f64>(cut_changed) < 0.015);
  CHECK(worst_marker < 0.002);
}

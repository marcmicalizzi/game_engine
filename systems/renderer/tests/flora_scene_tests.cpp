// A scene's `.flora.json` mesh (docs/subsystems/flora.md, "In a scene"; scene_flora.cpp): built by
// the mesh generator its name gives through the derived-data cache, a cache hit the second time,
// refused with a sentence naming the capability where the build has none; and the oasis scene's
// palms drawn offscreen — green where the crowns stand and brown below.
//
// Compiled in every configuration: with the flora capability it links it, as a host does, so its
// registrar is what makes the name one the reader knows; without it, the refusal is what is tested.
#include <domain/gfx/device.h>
#include <foundation/image/png.h>
#include <systems/renderer/camera_path.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#if ENGINE_RENDERER_TESTS_TILESET
#include <domain/tileset/tileset.h>
#endif

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

// A small palm, so a debug build solves and clusters it in a moment.
constexpr const char* k_small_palm =
    R"({"species":"date_palm","palm":{"seed":9,"height_m":5,"frond_count":8,)"
    R"("leaflet_count":12,"dead_skirt_fronds":2,"triangle_budget":8000}})";

SceneDesc one_mesh(const std::string& path, const std::string& ddc) {
  SceneDesc desc;
  desc.meshes.push_back(path);
  desc.ddc = ddc;
  desc.cache = true;
  return desc;
}

}  // namespace

#if ENGINE_RENDERER_TESTS_FLORA

TEST_CASE("flora scene: a .flora.json mesh is built through the cache and read back from it") {
  const test::TempDir tmp("renderer_flora_scene");
  REQUIRE(tmp.ok());
  const std::string palm = tmp.file("small.flora.json");
  REQUIRE(write_text(palm, k_small_palm));
  const std::string ddc = tmp.file("ddc");

  SceneData first;
  std::string error;
  REQUIRE_MESSAGE(load_scene(one_mesh(palm, ddc), first, error), error);
  REQUIRE(first.sources.size() == 1);
  CHECK(std::string(first.sources[0].cache) == "miss");
  CHECK(first.sources[0].source_hash != 0);
  CHECK_FALSE(first.sources[0].container.empty());
  // The three materials, the fronds two-sided.
  REQUIRE(first.sources[0].data.materials.size() == 3);
  CHECK_FALSE(first.sources[0].data.materials[0].double_sided);
  CHECK(first.sources[0].data.materials[1].double_sided);
  CHECK(first.parts[0].cluster_count > 0);

  // The second load finds the entry the first wrote: the same clusters, from the cache.
  SceneData second;
  REQUIRE_MESSAGE(load_scene(one_mesh(palm, ddc), second, error), error);
  CHECK(std::string(second.sources[0].cache) == "hit");
  CHECK(second.parts[0].cluster_count == first.parts[0].cluster_count);
  CHECK(second.sources[0].source_hash == first.sources[0].source_hash);

  // And with no cache, the same mesh built in memory.
  SceneDesc uncached = one_mesh(palm, ddc);
  uncached.cache = false;
  SceneData third;
  REQUIRE_MESSAGE(load_scene(uncached, third, error), error);
  CHECK(std::string(third.sources[0].cache) == "none");
  CHECK(third.parts[0].cluster_count == first.parts[0].cluster_count);

  // A file the generator refuses is a load error naming the field.
  const std::string bad = tmp.file("bad.flora.json");
  REQUIRE(write_text(bad, R"({"species":"date_palm","palm":{"frond_count":0}})"));
  SceneData refused;
  CHECK_FALSE(load_scene(one_mesh(bad, ddc), refused, error));
  CHECK(error.find("frond_count") != std::string::npos);
}

namespace {

// A 64 x 64 stand-in map of the test's own — vertical stripes, `a` and `b` — so every image has
// bytes of its own and the texture step builds each once.
bool write_stripes(const std::string& path, u8 a, u8 b) {
  Vector<u8> pixels(64u * 64u * 4u);
  for (u32 y = 0; y < 64; ++y) {
    for (u32 x = 0; x < 64; ++x) {
      const u8 v = (x / 8u) % 2u == 0 ? a : b;
      u8* p = pixels.data() + (y * 64u + x) * 4u;
      p[0] = v;
      p[1] = static_cast<u8>(v / 2u + 64u);
      p[2] = 128u;
      p[3] = 255u;
    }
  }
  Vector<u8> png;
  if (!image::encode_png(64, 64, 4, std::span<const u8>(pixels.data(), pixels.size()), png))
    return false;
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  return f.good();
}

u32 files_in(const std::string& dir) {
  std::error_code ec;
  u32 n = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.is_regular_file()) ++n;
  }
  return n;
}

}  // namespace

// docs/subsystems/materials.md: a recipe's set for the bark, a material pack's for the fronds and
// nothing for the skirt; every image the parts sample is built into the cache's textures, as a
// glTF's are, and the container names them.
TEST_CASE("flora scene: a palm's material sets are built into the cache as textures") {
  const test::TempDir tmp("renderer_flora_materials");
  REQUIRE(tmp.ok());
  std::filesystem::create_directories(tmp.native_file("sets"));
  std::filesystem::create_directories(tmp.native_file("pack/palm/frond"));
  REQUIRE(write_stripes(tmp.file("sets/bark_c.png"), 90, 140));
  REQUIRE(write_stripes(tmp.file("sets/bark_n.png"), 120, 136));
  REQUIRE(write_stripes(tmp.file("sets/bark_r.png"), 200, 230));
  const char* maps[] = {"base_color", "normal", "roughness", "metallic", "height"};
  std::string pack =
      R"({"format":"engine.materials.pack/1","name":"stand-in","version":"1",)"
      R"("roles":{"palm/frond":{"tile_m":0.25,"normal_convention":"opengl","maps":{)";
  for (u32 i = 0; i < 5; ++i) {
    REQUIRE(write_stripes(tmp.file(std::string("pack/palm/frond/") + maps[i] + ".png"),
                          static_cast<u8>(10 + 30 * i), static_cast<u8>(20 + 30 * i)));
    pack += std::string(i > 0 ? "," : "") + "\"" + maps[i] + "\":{\"file\":\"palm/frond/" +
            maps[i] + ".png\"}";
  }
  pack += "}}}}";
  REQUIRE(write_text(tmp.file("pack/pack.json"), pack));
  const std::string palm = tmp.file("small.flora.json");
  REQUIRE(write_text(
      palm, R"({"species":"date_palm","palm":{"seed":9,"height_m":5,"frond_count":8,)"
            R"("leaflet_count":12,"dead_skirt_fronds":2,"triangle_budget":8000},)"
            R"("materials":{"bark":{"base_color":"sets/bark_c.png","normal":"sets/bark_n.png",)"
            R"("roughness":"sets/bark_r.png","tile_m":0.5}}})"));

  SceneDesc desc = one_mesh(palm, tmp.file("ddc"));
  desc.materials = tmp.file("pack");
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  REQUIRE(data.sources.size() == 1);
  const assets::MeshData& mesh = data.sources[0].data;
  REQUIRE(mesh.materials.size() == 3);
  CHECK(mesh.materials[0].base_color_image >= 0);
  CHECK(mesh.materials[1].normal_image >= 0);
  CHECK(mesh.materials[2].base_color_image == -1);  // the skirt: no set, the flat colour
  // Bark three, frond three (metallic and height are carried, not sampled).
  CHECK(mesh.images.size() == 6);
  CHECK(files_in(tmp.file("ddc/textures")) == 6);
}

#if ENGINE_RENDERER_TESTS_FLORA_TERRAIN

namespace {

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

// Whether a mesh is a palm: three materials, the second and third two-sided (a container carries
// no material names, so a palm is told by its materials' shape).
bool is_palm(const SourceMesh& source) {
  const auto& m = source.data.materials;
  return m.size() == 3 && !m[0].double_sided && m[1].double_sided && m[2].double_sided;
}

// The palm material a pixel's cluster draws with — 0 bark, 1 frond, 2 dead frond — or -1 for the
// ground and the sky.
i32 palm_material_of(const SceneData& data, u32 instance, u32 cluster) {
  if (instance >= data.instances.size()) return -1;
  const u32 mesh = data.instances[instance].mesh;
  if (mesh >= data.parts.size() || !is_palm(data.sources[mesh])) return -1;
  const geometry::ClusterMeshPart& part = data.parts[mesh];
  if (cluster < part.first_cluster || cluster >= part.first_cluster + part.cluster_count) return -1;
  const SourceMesh& source = data.sources[mesh];
  const u32 local = cluster - part.first_cluster;
  if (local >= source.part_of_cluster.size()) return -1;
  return source.part_material[source.part_of_cluster[local]];
}

}  // namespace

TEST_CASE("flora scene: the oasis's palms are green crowns over brown trunks") {
  const std::string scene_path =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/oasis-palms/scene.json",
                      "content/test-scenes/oasis-palms/scene.json");
  if (!test::path_exists(scene_path)) {
    MESSAGE("skipped: " << scene_path << " is not here");
    return;
  }
  const test::TempDir tmp("renderer_flora_oasis");
  REQUIRE(tmp.ok());
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(scene_path, desc, error), error);
  REQUIRE(desc.meshes.size() == 6);  // the five palms and the terrain
  desc.ddc = tmp.file("ddc");
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  u32 palms = 0;
  for (const SourceMesh& source : data.sources) {
    if (is_palm(source)) ++palms;
  }
  CHECK(palms == 5);

  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  RenderSettings settings;
  ResolvedSettings resolved;
  resolve_settings(settings, gpu.device.features(), &data, resolved);
  REQUIRE(check_availability(resolved, gpu.device.features()) == RenderAvailability::Ok);
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(gpu.device, data, resolved, &error), error);
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = 640;
  rd.height = 400;
  REQUIRE_MESSAGE(renderer.create(gpu.device, scene, resolved, rd, &error), error);

  // The path's first frame: the whole grove from 38 m, crowns against the sky. With
  // ENGINE_FLORA_KEEP_CAPTURE set, it and the path's walk under the crowns are left as PNGs in the
  // scratch directory for a person to look at.
  CameraPath path;
  REQUIRE_MESSAGE(read_camera_path(desc.camera_path, &desc.terrain, path, error), error);
  CaptureChannels channels;
  channels.ids = true;
  CapturedFrame shot;
  const bool keep = !test::detail::environment("ENGINE_FLORA_KEEP_CAPTURE").empty();
  for (const f64 time : {40.0, 34.0, 0.0}) {
    if (!keep && time != 0.0) continue;
    FrameDesc frame;
    frame.camera = sample_camera_path(path, time);
    REQUIRE_MESSAGE(renderer.render_offscreen(frame, &error), error);
    REQUIRE_MESSAGE(renderer.capture(frame, channels, shot, &error), error);
    if (keep) {
      CaptureFiles files;
      write_capture(tmp.path(), "oasis-" + std::to_string(static_cast<int>(time)), shot, files,
                    error);
      tmp.keep();
    }
  }
  REQUIRE(shot.ids.size() == u64{shot.width} * shot.height * k_id_words);

  // Every pixel a frond covers against every pixel a trunk covers: their colour and their rows.
  u32 frond = 0;
  u32 frond_green = 0;
  u32 bark = 0;
  u32 bark_brown = 0;
  f64 frond_row = 0.0;
  f64 bark_row = 0.0;
  for (u32 y = 0; y < shot.height; ++y) {
    for (u32 x = 0; x < shot.width; ++x) {
      const u32 p = y * shot.width + x;
      const i32 material =
          palm_material_of(data, shot.ids[p * k_id_words], shot.ids[p * k_id_words + 1]);
      const int r = shot.color[p * 4];
      const int g = shot.color[p * 4 + 1];
      const int b = shot.color[p * 4 + 2];
      if (material == 1) {
        ++frond;
        frond_row += y;
        if (g > r && g > b) ++frond_green;
      } else if (material == 0) {
        ++bark;
        bark_row += y;
        if (r > b && r >= g) ++bark_brown;
      }
    }
  }
  MESSAGE("oasis frame 0: " << frond << " frond pixels (" << frond_green << " green), " << bark
                            << " bark pixels (" << bark_brown << " brown)");
  REQUIRE(frond > 1000);
  REQUIRE(bark > 1000);
  CHECK(frond_green * 10 >= frond * 8);        // the crowns read green
  CHECK(bark_brown * 10 >= bark * 8);          // the trunks brown
  CHECK(frond_row / frond < bark_row / bark);  // and the crowns stand above the trunks
}

// G201 (flora.md, "The trunk's skirt"; docs/experiments/tree-base-on-slopes.md): the tree-bases
// scene's spruces stand on ramps of 0, 15, 30 and 45 degrees at eight yaws, and none shows a gap
// under its trunk. Each tree is seen across the slope from 3 m, the eye 0.2 m over its foot and
// looking level, so the ground under the downhill rim falls away from the sight line and a gap is
// what lies beyond: the sky, or another ramp. Read from the ids alone: in every column the tree
// draws in the lower part of the frame, every pixel from the tree's lowest down to the first of
// its own ramp's is the tree's or the ramp's. On the mesh before the skirt (main at 33ff2824) the
// 30 degree ramp's trees fail this; with it, every ramp to 45 passes.
TEST_CASE("flora scene: no tree on a ramp of up to 45 degrees shows a gap under its trunk") {
  const std::string scene_path =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/tree-bases/scene.json",
                      "content/test-scenes/tree-bases/scene.json");
  if (!test::path_exists(scene_path)) {
    MESSAGE("skipped: " << scene_path << " is not here");
    return;
  }
  const test::TempDir tmp("renderer_tree_bases");
  REQUIRE(tmp.ok());
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(scene_path, desc, error), error);
  desc.ddc = tmp.file("ddc");
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  // The four ramps first, then eight spruces a ramp down its fall line (scene.json).
  REQUIRE(data.instances.size() == 4u + 4u * 8u);

  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  RenderSettings settings;
  ResolvedSettings resolved;
  resolve_settings(settings, gpu.device.features(), &data, resolved);
  REQUIRE(check_availability(resolved, gpu.device.features()) == RenderAvailability::Ok);
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(gpu.device, data, resolved, &error), error);
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = 640;
  rd.height = 400;
  REQUIRE_MESSAGE(renderer.create(gpu.device, scene, resolved, rd, &error), error);
  CaptureChannels channels;
  channels.ids = true;
  const bool keep = !test::detail::environment("ENGINE_FLORA_KEEP_CAPTURE").empty();

  constexpr f64 k_ramp_x[4] = {-45.0, -15.0, 15.0, 45.0};
  constexpr f64 k_ramp_tan[4] = {0.0, 0.2679491924311227, 0.5773502691896257, 1.0};
  for (u32 ramp = 0; ramp < 4; ++ramp) {
    u32 gap = 0;
    u32 columns = 0;
    for (u32 k = 0; k < 8; ++k) {
      const u32 tree = 4 + ramp * 8 + k;
      const f64 z = -21.0 + 6.0 * k;
      const f64 y = -k_ramp_tan[ramp] * z;
      FrameDesc frame;
      frame.camera.position = WorldPos{k_ramp_x[ramp] + 3.0, y + 0.2, z};
      frame.camera.target = WorldPos{k_ramp_x[ramp], y + 0.2, z};
      frame.camera.znear = 0.05f;
      CapturedFrame shot;
      REQUIRE_MESSAGE(renderer.render_offscreen(frame, &error), error);
      REQUIRE_MESSAGE(renderer.capture(frame, channels, shot, &error), error);
      REQUIRE(shot.ids.size() == u64{shot.width} * shot.height * k_id_words);
      if (keep) {
        CaptureFiles files;
        write_capture(tmp.path(), "tree-bases-" + std::to_string(ramp) + "-" + std::to_string(k),
                      shot, files, error);
        tmp.keep();
      }
      const auto instance = [&](u32 x, u32 row) {
        return shot.ids[(u64{row} * shot.width + x) * k_id_words];
      };
      for (u32 x = 0; x < shot.width; ++x) {
        u32 lowest = shot.height;
        for (u32 row = 0; row < shot.height; ++row)
          if (instance(x, row) == tree) lowest = row;
        if (lowest == shot.height || lowest < shot.height * 2 / 5) continue;  // a branch's column
        ++columns;
        for (u32 row = lowest + 1; row < shot.height; ++row) {
          const u32 id = instance(x, row);
          if (id == ramp) break;  // the ground it stands on
          if (id != tree) ++gap;
        }
      }
    }
    MESSAGE("ramp of " << 15 * ramp << " degrees: " << gap << " gap pixels under " << columns
                       << " columns of trunk, eight yaws");
    CHECK(columns > 0);
    CHECK(gap == 0);
  }
}

#endif  // ENGINE_RENDERER_TESTS_FLORA_TERRAIN

// `Gpu` is the dunes' block's, above.
#if ENGINE_RENDERER_TESTS_TILESET && ENGINE_RENDERER_TESTS_FLORA_TERRAIN

// G201 stage 2 (renderer.md, "A tree seated by its footprint"; content/test-scenes/tree-bases/
// seat.json): spruces at eight yaws on the edge of a tread raised 1.2 m over flat ground and on the
// lip of a cutting 1.5 m deep, their rims hanging over faces steeper than the skirt reaches. Each
// is seen along the edge from 3 m, the eye 0.2 m over its foot, so a gap under its downhill rim
// shows the ground beyond it, far behind. Read from the ids and the depth: in every column the
// tree draws low in the frame, every pixel from its lowest down is the tree's, until the ground
// within 0.6 m of the trunk's distance — never the sky, another tree, or ground far behind. On the
// base (b7681ac9: the skirt, no seat) the faces' trees fail it; seated, every one passes.
TEST_CASE("flora scene: a tree on a tread's edge or a cutting's lip is seated with no gap") {
  const std::string spruce =
      test::data_path(ENGINE_SOURCE_DIR "/content/flora/spruce-tall.flora.json",
                      "content/flora/spruce-tall.flora.json");
  if (!test::path_exists(spruce)) {
    MESSAGE("skipped: " << spruce << " is not here");
    return;
  }
  const test::TempDir tmp("renderer_tree_seats");
  REQUIRE(tmp.ok());
  std::string error;
  const tileset::Heightfield field =
      tileset::synth_heightfield(tileset::SynthKind::flat, 257, 250, 0, -32000, -32000);
  REQUIRE_MESSAGE(tileset::write_heightfield(tmp.file("seat-ground.json"), field, error), error);
  std::string spruce_path = spruce;
  for (char& c : spruce_path)
    if (c == '\\') c = '/';
  // The committed scene's instances and terrain (tree-bases/seat.json), its paths this test's.
  constexpr f64 k_edge_x[2] = {-4.6, 8.1};  // 0.1 m inside the tread's edge; 0.1 m off the lip
  std::string text = R"({"format":"engine.scene.v1","name":"tree-seats","meshes":[)"
                     R"({"name":"spruce","path":")" +
                     spruce_path + R"("}],"instances":[)";
  for (u32 row = 0; row < 2; ++row) {
    for (u32 k = 0; k < 8; ++k) {
      char line[160];
      std::snprintf(line, sizeof(line),
                    R"(%s{"mesh":0,"translation":[%.1f,0,%d],"yaw_deg":%u,"ground":true})",
                    row + k == 0 ? "" : ",", k_edge_x[row], -21 + 6 * static_cast<i32>(k), 45u * k);
      text += line;
    }
  }
  text += R"(],"terrain":{"size":257,"extent":32,"provider":"tiles","tiles":{)"
          R"("heightfield":"seat-ground.json","edits":[)"
          R"({"name":"tread","mode":"Bench","points":[[-6,1.2,-30],[-6,1.2,30]],)"
          R"("width_m":[3,3],"fill_slope":3,"fill_m":2,"bank_round":0.1},)"
          R"({"name":"cutting","mode":"Cut","points":[[6,0,-30],[6,0,30]],"width_m":[4,4],)"
          R"("depth_m":[1.5,1.5],"bank_slope":4,"bank_round":0.1}]}}})";
  REQUIRE(write_text(tmp.file("seat.json"), text));
  SceneDesc desc;
  desc.ddc = tmp.file("ddc");  // the host's root, before the read: the tile set is built there
  REQUIRE_MESSAGE(read_scene_file(tmp.file("seat.json"), desc, error), error);
  const Vector<SceneInstance> unseated = desc.instances;  // the trees' feet as the read stood them
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  REQUIRE(data.instances.size() == 16u + 1u);
  MESSAGE("seated " << data.load.seated << ", lowered " << data.load.lowered << ", the most "
                    << data.load.lowered_max_m << " m, in " << data.load.seat_ms << " ms");
  CHECK(data.load.seated == 16u);
  CHECK(data.load.lowered == 16u);
  const u32 terrain = 16;

  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  RenderSettings settings;
  ResolvedSettings resolved;
  resolve_settings(settings, gpu.device.features(), &data, resolved);
  REQUIRE(check_availability(resolved, gpu.device.features()) == RenderAvailability::Ok);
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(gpu.device, data, resolved, &error), error);
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = 640;
  rd.height = 400;
  REQUIRE_MESSAGE(renderer.create(gpu.device, scene, resolved, rd, &error), error);
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  const bool keep = !test::detail::environment("ENGINE_FLORA_KEEP_CAPTURE").empty();
  constexpr f64 k_distance = 3.0;
  constexpr f32 k_znear = 0.05f;
  for (u32 row = 0; row < 2; ++row) {
    u32 gap = 0;
    u32 columns = 0;
    for (u32 k = 0; k < 8; ++k) {
      const u32 tree = row * 8 + k;
      const WorldPos foot = unseated[tree].origin;
      FrameDesc frame;
      frame.camera.position = WorldPos{foot.x, foot.y + 0.2, foot.z - k_distance};
      frame.camera.target = WorldPos{foot.x, foot.y + 0.2, foot.z};
      frame.camera.znear = k_znear;
      CapturedFrame shot;
      REQUIRE_MESSAGE(renderer.render_offscreen(frame, &error), error);
      REQUIRE_MESSAGE(renderer.capture(frame, channels, shot, &error), error);
      REQUIRE(shot.ids.size() == u64{shot.width} * shot.height * k_id_words);
      REQUIRE(shot.depth.size() == u64{shot.width} * shot.height);
      if (keep) {
        CaptureFiles files;
        write_capture(tmp.path(), "tree-seats-" + std::to_string(row) + "-" + std::to_string(k),
                      shot, files, error);
        tmp.keep();
      }
      const auto pixel = [&](u32 x, u32 y) { return u64{y} * shot.width + x; };
      for (u32 x = 0; x < shot.width; ++x) {
        u32 lowest = shot.height;
        for (u32 y = 0; y < shot.height; ++y)
          if (shot.ids[pixel(x, y) * k_id_words] == tree) lowest = y;
        if (lowest == shot.height || lowest < shot.height * 2 / 5) continue;  // a branch's column
        ++columns;
        for (u32 y = lowest + 1; y < shot.height; ++y) {
          const u32 id = shot.ids[pixel(x, y) * k_id_words];
          const f32 depth = shot.depth[pixel(x, y)];
          if (id == tree) continue;
          // The ground at the foot: the trunk's distance and its rim's.
          if (id == terrain && depth > 0.0f &&
              static_cast<f64>(k_znear / depth) <= k_distance + 0.6)
            break;
          ++gap;  // the sky, another tree, or the ground far behind, seen under the trunk
        }
      }
    }
    MESSAGE((row == 0 ? "the tread's edge: " : "the cutting's lip: ")
            << gap << " gap pixels under " << columns << " columns of trunk, eight yaws");
    CHECK(columns > 0);
    CHECK(gap == 0);
  }
}

#endif  // ENGINE_RENDERER_TESTS_TILESET && ENGINE_RENDERER_TESTS_FLORA_TERRAIN

#else

TEST_CASE("flora scene: a .flora.json mesh is refused with a sentence naming the capability") {
  const test::TempDir tmp("renderer_flora_off");
  REQUIRE(tmp.ok());
  const std::string palm = tmp.file("small.flora.json");
  REQUIRE(write_text(palm, k_small_palm));
  SceneData data;
  std::string error;
  CHECK_FALSE(load_scene(one_mesh(palm, tmp.file("ddc")), data, error));
  CHECK(error.find("ENGINE_WITH_FLORA") != std::string::npos);
}

#endif

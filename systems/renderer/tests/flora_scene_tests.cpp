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

#include <algorithm>
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

#endif  // ENGINE_RENDERER_TESTS_FLORA_TERRAIN

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

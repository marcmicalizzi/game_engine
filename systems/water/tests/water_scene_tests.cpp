// The water block through the renderer's scene reader (docs/subsystems/water.md, "Testing"): a
// body read as one mesh and one instance before the terrain, its mesh's info carrying the body and
// its shading, a scene without the block the scene it was, a refusal with a sentence; and on a GPU
// the oasis pool's captures — with and without the block they differ only where the pool is, the
// same frame twice is the same bytes, and the sun two hours on moves what the pool shows.
#include <core/json/json.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/gfx/device.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/terrain.h>
#include <systems/water/water.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace engine;
using namespace engine::renderer;

namespace {

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
  return static_cast<bool>(out);
}

// The renderer's own ground (the waves) and a pool over it, or none.
std::string scene_text(const std::string& water) {
  return std::string(R"({
  "format": "engine.scene.v1",
  "name": "water-test",
  "terrain": {"size": 65, "extent": 32, "seed": 4, "dune_height": 0.6, "dune_wavelength": 12})") +
         (water.empty() ? std::string() : ",\n  \"water\": " + water) + "\n}\n";
}

constexpr const char* k_pool = R"([{"name": "pool", "level_m": 0.2,
  "shape": {"circle": {"center": [3, 0, -2], "radius_m": 6}}, "wind_follow": false,
  "direction_deg": 90, "shallow": [0.2, 0.5, 0.45]}])";

}  // namespace

TEST_CASE("water: a scene's block is one mesh and one instance a body, before the terrain") {
  const test::TempDir tmp("water_scene");
  const std::string path = tmp.file("scene.json");
  REQUIRE(write_text(path, scene_text(k_pool)));
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  // The pool's mesh, then the terrain, last as always.
  REQUIRE(desc.meshes.size() == 2);
  REQUIRE(desc.mesh_info.size() == 2);
  CHECK(desc.meshes[0].find("water-") != std::string::npos);
  CHECK(io::exists(desc.meshes[0]));
  CHECK(std::string_view(desc.mesh_info[0].origin) == "water");
  CHECK(desc.mesh_info[0].name == "water/pool");
  REQUIRE(desc.mesh_info[0].water.has_value());
  CHECK_FALSE(desc.mesh_info[1].water.has_value());
  CHECK(std::string_view(desc.mesh_info[1].origin) == "terrain");
  const SceneWater& water = *desc.mesh_info[0].water;
  CHECK(water.body.name == "pool");
  CHECK(water.shading.shallow == Vec3{0.2f, 0.5f, 0.45f});
  CHECK(water.shading.deep == gfx::WaterShading{}.deep);  // absent: the default
  // A fixed direction of 90 degrees: +z.
  CHECK(water.shading.direction.x == doctest::Approx(0.0f).epsilon(1e-6));
  CHECK(water.shading.direction.y == doctest::Approx(1.0f));
  // The body's depth map, beside its mesh, as the renderer's reader mirrors the capability's
  // layout (water.md, "The depth map"): the same frame and texels the capability reads back.
  {
    const std::string& glb = desc.meshes[0];
    const std::string map_path = glb.substr(0, glb.size() - 4) + ".depth";
    std::string bytes;
    REQUIRE(io::read_file(map_path, bytes) == io::Status::Ok);
    water::DepthMap map;
    REQUIRE_MESSAGE(
        water::read_depth_map(
            std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size()), map,
            &error),
        error);
    const SceneWaterDepth& depth = water.depth;
    REQUIRE(depth.size == map.size);
    CHECK(depth.size >= water::k_depth_map_min);
    CHECK(depth.origin == map.origin);
    CHECK(depth.texel_m == map.texel_m);
    CHECK(depth.depth_max == map.depth_max);
    REQUIRE(depth.texels.size() == map.texels.size());
    CHECK(std::equal(depth.texels.begin(), depth.texels.end(), map.texels.begin()));
    CHECK(depth.depth_max > 0.0f);
  }
  REQUIRE(desc.instances.size() == 2);
  CHECK(desc.instances[0].mesh == 0u);
  CHECK(desc.instances[0].origin == WorldPos{3.0, 0.2, -2.0});
  CHECK(desc.instances[0].transform == Transform3::identity());
  CHECK(desc.instances[1].mesh == 1u);

  // The load carries the body to the scene a GPU scene is made from.
  SceneData data;
  desc.ddc = tmp.file("ddc");
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  REQUIRE(data.mesh_info.size() == 2);
  CHECK(data.mesh_info[0].water.has_value());
  CHECK(data.terrain_mesh == 1u);

  // Without the block, and with an empty one: the scene it was, mesh for mesh.
  for (const char* water_text : {"", "[]"}) {
    const std::string bare_path =
        tmp.file(std::string("bare") + (*water_text ? "-empty" : "") + ".json");
    REQUIRE(write_text(bare_path, scene_text(water_text)));
    SceneDesc bare;
    REQUIRE_MESSAGE(read_scene_file(bare_path, bare, error), error);
    REQUIRE(bare.meshes.size() == 1);
    CHECK(bare.meshes[0].empty());
    REQUIRE(bare.instances.size() == 1);
    CHECK_FALSE(bare.mesh_info[0].water.has_value());
  }
}

// The host's derived-data root (`--ddc`, `SceneDesc::ddc` set before the file is read, which the
// reader hands the generator as `Context::ddc`): the pool's mesh is written under `<root>/water/`
// and nothing beside the scene, so a game repository's root is honoured wherever its scenes are.
// With none named, the scene's own root by the one rule (T119; scene_gen.md, "The derived-data
// root"): outside any checkout, `<scene dir>/ddc/water/`, and nothing at `<scene dir>/water/`.
TEST_CASE("water: the meshes go under the host's derived-data root, else beside the scene") {
  const test::TempDir tmp("water_scene_ddc");
  const std::string path = tmp.file("scene.json");
  REQUIRE(write_text(path, scene_text(k_pool)));
  const std::string root = tmp.file("game-ddc");
  SceneDesc desc;
  desc.ddc = root;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  REQUIRE(desc.meshes.size() == 2);
  CHECK(io::parent_path(desc.meshes[0]) == io::join_path(root, "water"));
  CHECK(io::exists(desc.meshes[0]));
  CHECK_FALSE(io::exists(tmp.file("water")));

  SceneDesc bare;
  REQUIRE_MESSAGE(read_scene_file(path, bare, error), error);
  REQUIRE(bare.meshes.size() == 2);
  CHECK(io::parent_path(bare.meshes[0]) == io::join_path(io::parent_path(path), "ddc/water"));
  CHECK_FALSE(io::exists(io::join_path(io::parent_path(path), "water")));
  CHECK(io::exists(bare.meshes[0]));
  // The same body is the same bytes, so the same file name in either place.
  CHECK(io::file_name(bare.meshes[0]) == io::file_name(desc.meshes[0]));
}

TEST_CASE("water: a scene's polygon body, and a body the capability refuses") {
  const test::TempDir tmp("water_scene_polygon");
  const std::string path = tmp.file("scene.json");
  REQUIRE(write_text(path, scene_text(R"([{"name": "pond", "level_m": 0.1, "shape": {"polygon":
    [[-4, 0, -3], [4, 0, -3], [5, 0, 2], [0, 0, 5], [-5, 0, 2]]}}])")));
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  REQUIRE(desc.instances.size() == 2);
  // At the polygon's centroid and the level.
  CHECK(desc.instances[0].origin.y == 0.1);
  CHECK(std::fabs(desc.instances[0].origin.x) < 1.0);
  const std::string bad = tmp.file("bad.json");
  REQUIRE(write_text(bad, scene_text(R"([{"name": "nothing", "shape": {}}])")));
  SceneDesc refused;
  CHECK_FALSE(read_scene_file(bad, refused, error));
  MESSAGE(error);
  CHECK(error.find("exactly one of a circle and a polygon") != std::string::npos);
  CHECK(error.find("nothing") != std::string::npos);
}

// **A water body is not an alpha-tested material** (R141; renderer.md, "Alpha-tested geometry";
// gfx's flag word, `k_material_common_flags`). The volume tier's bit was the alpha mask's bit (32)
// until 2026-11-09, so every scene with water at the default tier read as masked: the masked
// pipelines on, the ray-tracing templates off, the water's clusters in the shadow proxy's and the
// micromaps' lists, and the alpha test run on the water with its `shore_m` for the alpha and its
// `chop` for the cutoff. A pool whose chop is above its shore distance was therefore discarded
// whole at the high tier and drawn at the low one. Here: the scene is not masked at either tier,
// and the pool covers the same pixels at both.
TEST_CASE("water: a water body is not an alpha-tested material, at either tier") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("renderer unavailable here: " << error);
    return;
  }
  if (!device.features().buffer_int64_atomics) {
    MESSAGE("no 64-bit buffer atomics here");
    device.destroy();
    return;
  }
  const test::TempDir tmp("water_not_masked");
  const std::string path = tmp.file("scene.json");
  REQUIRE(write_text(path, scene_text(R"([{"name": "pool", "level_m": 0.2,
    "shape": {"circle": {"center": [3, 0, -2], "radius_m": 6}}, "wind_follow": false,
    "direction_deg": 90, "chop": 0.9, "shore_m": 0.5}])")));
  u32 pool_pixels[2] = {};
  bool masked[2] = {true, true};
  bool water[2] = {};
  const WaterQuality tiers[2] = {WaterQuality::High, WaterQuality::Low};
  for (u32 t = 0; t < 2; ++t) {
    SceneDesc desc;
    REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
    desc.ddc = tmp.file("ddc");
    SceneData data;
    REQUIRE_MESSAGE(load_scene(desc, data, error), error);
    RenderSettings settings;
    settings.water_quality = tiers[t];
    ResolvedSettings resolved;
    resolve_settings(settings, device.features(), &data, resolved);
    REQUIRE(check_availability(resolved, device.features()) == RenderAvailability::Ok);
    GpuScene scene;
    REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
    masked[t] = scene.has_masked();
    water[t] = scene.has_water();
    u32 water_instance = ~0u;
    for (u32 i = 0; i < data.instances.size(); ++i) {
      const u32 mesh = data.instances[i].mesh;
      if (mesh < data.mesh_info.size() && data.mesh_info[mesh].water.has_value())
        water_instance = i;
    }
    REQUIRE(water_instance != ~0u);
    SceneRenderer renderer;
    SceneRenderer::Desc rd;
    rd.width = 160;
    rd.height = 120;
    REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
    FrameDesc frame;
    frame.camera.position = WorldPos{3.0, 14.0, -12.0};
    frame.camera.target = WorldPos{3.0, 0.0, -2.0};
    CaptureChannels channels;
    channels.ids = true;
    CapturedFrame shot;
    REQUIRE_MESSAGE(renderer.capture(frame, channels, shot, &error), error);
    for (u32 p = 0; p < shot.width * shot.height; ++p)
      pool_pixels[t] += shot.ids[p * k_id_words] == water_instance ? 1u : 0u;
    renderer.destroy();
    scene.destroy();
  }
  device.destroy();
  MESSAGE("pool pixels: high " << pool_pixels[0] << ", low " << pool_pixels[1] << "; masked: high "
                               << masked[0] << ", low " << masked[1]);
  CHECK(water[0]);
  CHECK(water[1]);
  CHECK_FALSE(masked[0]);
  CHECK_FALSE(masked[1]);
  CHECK(pool_pixels[1] > 1000u);
  CHECK(pool_pixels[0] == pool_pixels[1]);
}

#if ENGINE_WATER_TESTS_DUNES
namespace {

// The oasis pool's file with its `water` block kept or emptied, in the test's own directory (its
// rocks are written beside it there, never in the tree).
std::string oasis_copy(const test::TempDir& tmp, bool water) {
  std::string text;
  const std::string source =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/oasis-pool/scene.json",
                      "content/test-scenes/oasis-pool/scene.json");
  REQUIRE(io::read_file(source, text) == io::Status::Ok);
  JsonValue root;
  REQUIRE(parse_json(text, root).ok);
  if (!water) root.set("water", JsonValue::array());
  // The wet band is on by default and darkens the sand round the pool; these cases isolate the
  // water block's own footprint, and the band's is held by count in shore_scene_tests.cpp.
  if (const JsonValue* bodies = root.find("water"); water && bodies != nullptr) {
    JsonValue list = JsonValue::array();
    for (usize i = 0; i < bodies->size(); ++i) {
      JsonValue body = (*bodies)[i];
      body.set("wet_m", JsonValue(0.0));
      list.push_back(std::move(body));
    }
    root.set("water", std::move(list));
  }
  const std::string out = tmp.file(water ? "oasis.json" : "dry.json");
  REQUIRE(write_text(out, write_json(root)));
  return out;
}

bool gpu_here() {
  gfx::Device probe;
  std::string why;
  if (!probe.create(gfx::DeviceOptions{}, &why)) {
    MESSAGE("renderer unavailable here: " << why);
    return false;
  }
  const bool atomics = probe.features().buffer_int64_atomics;
  probe.destroy();
  if (!atomics) MESSAGE("no 64-bit buffer atomics here");
  return atomics;
}

struct Shot {
  CapturedFrame frame;
  u32 water_instance = ~0u;
  bool has_water = false;  // the GPU scene's answer, which picks the resolve module
  u32 materials = 0;       // the GPU scene's material table's length
  u32 water_open = ~0u;    // the GPU scene's open body's instance (water.md, "To the horizon")
};

// One capture of a scene file from above the pool's west shore, towards the sun's reflection.
void shoot(const gfx::Device& device, const std::string& path, const std::string& ddc,
           f64 sun_time_s, u32 shots, Shot* out, WaterQuality quality = WaterQuality::High,
           WorldPos eye = WorldPos{-16.0, 9.0, -5.0}, WorldPos look = WorldPos{1.0, -1.0, 1.0},
           f64 world_time_s = -1.0) {
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  desc.ddc = ddc;
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  RenderSettings settings;
  settings.water_quality = quality;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  REQUIRE(check_availability(resolved, device.features()) == RenderAvailability::Ok);
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = 480;
  rd.height = 320;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
  FrameDesc frame;
  frame.camera.position = eye;
  frame.camera.target = look;
  frame.sky_time_s = sun_time_s;
  // The waves move with the sun here, as on the one clock, unless a world time is given (R88).
  frame.world_time_s = world_time_s >= 0.0 ? world_time_s : sun_time_s;
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;  // the open sea's case reads the surface's distance
  u32 water_instance = ~0u;
  for (u32 i = 0; i < data.instances.size(); ++i) {
    const u32 mesh = data.instances[i].mesh;
    if (mesh < data.mesh_info.size() && data.mesh_info[mesh].water.has_value()) water_instance = i;
  }
  for (u32 s = 0; s < shots; ++s) {
    REQUIRE_MESSAGE(renderer.capture(frame, channels, out[s].frame, &error), error);
    out[s].water_instance = water_instance;
    out[s].has_water = scene.has_water();
    out[s].materials = scene.material_count();
    out[s].water_open = scene.water_open_instance();
  }
}

}  // namespace

// **The scene's wind** (gfx.md, "The water reads the wind"; W136): a sky's fixed wind gives a body
// that follows it exactly the table, direction and fetch its own `wind_m_s` at that direction did;
// a track makes it follow a varying wind on one grid; a track out of order refuses the file.
TEST_CASE("water: a body follows the sky's wind, and a fixed one draws what its own wind drew") {
  const test::TempDir tmp("water_scene_wind");
  const auto read = [&](const char* name, const std::string& sky, const std::string& body,
                        SceneDesc& desc, std::string& error) {
    const std::string path = tmp.file(name);
    std::string text = scene_text(body);
    if (!sky.empty()) text.insert(text.rfind('}'), ",\n  \"sky\": " + sky);
    REQUIRE(write_text(path, text));
    return read_scene_file(path, desc, error);
  };
  const std::string open = R"([{"name": "sea", "level_m": 0.2, "open": true, "fetch_km": 40,
    "shape": {"circle": {"center": [3, 0, -2], "radius_m": 6}}, )";
  std::string error;
  SceneDesc own;
  REQUIRE_MESSAGE(
      read("own.json", "", open + R"("wind_follow": false, "wind_m_s": 7, "direction_deg": 30}])",
           own, error),
      error);
  SceneDesc fixed;
  REQUIRE_MESSAGE(read("fixed.json", R"({"wind": {"speed_m_s": 7, "to_deg": 30}})",
                       open + R"("wind_m_s": 2}])", fixed, error),
                  error);
  const SceneWater& a = *own.mesh_info[0].water;
  const SceneWater& b = *fixed.mesh_info[0].water;
  CHECK(fixed.wind.kind == gfx::WindKind::fixed);
  CHECK(b.scene_wind);
  CHECK_FALSE(b.grid.valid());
  CHECK(b.wind_m_s == a.wind_m_s);
  CHECK(b.fetch_m == a.fetch_m);
  CHECK(b.shading.direction == a.shading.direction);
  REQUIRE(a.waves.size() == gfx::k_water_waves);
  REQUIRE(b.waves.size() == a.waves.size());
  CHECK(std::memcmp(a.waves.data(), b.waves.data(), sizeof(gfx::WaterWave) * a.waves.size()) == 0);

  SceneDesc track;
  REQUIRE_MESSAGE(read("track.json",
                       R"({"wind": {"track": [{"hour": 0, "speed_m_s": 0, "to_deg": 30},
                         {"hour": 6, "speed_m_s": 18, "to_deg": 30}]}})",
                       open + R"("wind_m_s": 2}])", track, error),
                  error);
  const SceneWater& c = *track.mesh_info[0].water;
  CHECK(track.wind.kind == gfx::WindKind::track);
  CHECK(c.scene_wind);
  CHECK(c.grid.valid());
  CHECK(c.wind_m_s == doctest::Approx(18.0f).epsilon(0.02));  // the strongest wind sampled
  CHECK(c.wind_max_height_m > 0.5f);
  CHECK(c.waves.size() == gfx::k_water_waves);

  SceneDesc refused;
  CHECK_FALSE(
      read("refused.json",
           R"({"wind": {"track": [{"hour": 6, "speed_m_s": 5}, {"hour": 3, "speed_m_s": 5}]}})",
           open + R"("wind_m_s": 2}])", refused, error));
  MESSAGE(error);
  CHECK(error.find("sky.wind.track[1].hour") != std::string::npos);
}

// **What feeds a body's train** (roadmap W153; water.md, "What feeds the train";
// docs/experiments/fetch-limited-waves.md): a body's shore train is the wave its wind raises over
// its own fetch, by the fetch laws, and nothing else for a closed body. A pool 4 m across with no
// wind has none; under 20 m/s it has the ripples the laws give over its own reach (2026-11-21: 3.6
// m, 1.7 cm at 0.26 s, running up 1.3 cm on its steep bowl); under light air (1.2 m/s, under force
// 2's onset) none, its table still drawn; following a calm scene wind none, where until W153 it ran
// up its shore at the wind its own block named. A lake 500 m across under 7 m/s has the laws' 8 cm;
// the open sea's numbers are the laws' at its `fetch_km`, as before.
TEST_CASE("water: a body's shore train is its own fetch's under its wind, and none without one") {
  const test::TempDir tmp("water_scene_train");
  // A plain with one smooth bowl: 3 m in radius and a metre deep for the pool, 350 m and 10 m for
  // the lake and the sea; each body fills its bowl from the middle at its level.
  constexpr const char* k_small = R"({"size": 129, "extent": 64, "seed": 2, "dune_height": 0,
      "basins": [{"center": [0, 0], "radius": 3, "depth": 1}]})";
  constexpr const char* k_large = R"({"size": 129, "extent": 1024, "seed": 2, "dune_height": 0,
      "basins": [{"center": [0, 0], "radius": 350, "depth": 10}]})";
  const auto read = [&](const char* name, const char* terrain, const std::string& sky,
                        const std::string& body) {
    std::string text = std::string(R"({"format": "engine.scene.v1", "name": "train-test",
      "terrain": )") + terrain +
                       ",\n  \"water\": " + body;
    if (!sky.empty()) text += ",\n  \"sky\": " + sky;
    text += "\n}\n";
    const std::string path = tmp.file(name);
    REQUIRE(write_text(path, text));
    SceneDesc desc;
    std::string error;
    REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
    REQUIRE(desc.mesh_info.size() >= 1u);
    REQUIRE(desc.mesh_info[0].water.has_value());
    return *desc.mesh_info[0].water;
  };
  const std::string pool = R"([{"name": "pool", "level_m": -0.4, "wind_follow": false,
    "shape": {"fill": {"seed": [0, 0, 0], "max_extent_m": 10}}, "direction_deg": 30, )";
  const SceneWater still = read("still.json", k_small, "", pool + R"("wind_m_s": 0}])");
  const SceneWater gale = read("gale.json", k_small, "", pool + R"("wind_m_s": 20}])");
  const SceneWater light = read("light.json", k_small, "", pool + R"("wind_m_s": 1.2}])");
  const SceneWater calm = read("calm.json", k_small, R"({"wind": {"speed_m_s": 0, "to_deg": 30}})",
                               R"([{"name": "pool", "level_m": -0.4, "wind_m_s": 6,
               "shape": {"fill": {"seed": [0, 0, 0], "max_extent_m": 10}}}])");
  const std::string large = R"([{"level_m": -2, "wind_follow": false, "direction_deg": 30,
    "shape": {"fill": {"seed": [0, 0, 0], "max_extent_m": 900}}, "wind_m_s": 7, )";
  const SceneWater lake = read("lake.json", k_large, "", large + R"("name": "lake"}])");
  const SceneWater sea =
      read("sea.json", k_large, "", large + R"("name": "sea", "open": true, "fetch_km": 100}])");

  const WaterSwashNumbers n_still = water_swash_numbers(still);
  CHECK(n_still.wind == 0.0);
  CHECK(n_still.wave.height_m == 0.0);
  CHECK(n_still.run_up == 0.0);
  CHECK(still.waves.empty());  // the authored sines, not a table

  const WaterSwashNumbers n_gale = water_swash_numbers(gale);
  MESSAGE("a 4 m pool under 20 m/s: fetch " << n_gale.fetch_m << " m, H " << n_gale.wave.height_m
                                            << " m, T " << n_gale.wave.period_s << " s, run-up "
                                            << n_gale.run_up << " m");
  CHECK(n_gale.fetch_m > 1.5);
  CHECK(n_gale.fetch_m < 6.0);  // its own reach, not the block's 100 km
  CHECK(n_gale.wave.height_m == gfx::water_sea_height(20.0, n_gale.fetch_m));
  CHECK(n_gale.wave.period_s ==
        doctest::Approx(6.283185307179586 / gfx::jonswap_peak(20.0, n_gale.fetch_m)));
  CHECK(n_gale.wave.height_m > 0.005);  // centimetres, not decimetres
  CHECK(n_gale.wave.height_m < 0.04);
  CHECK(n_gale.wave.period_s < 0.5);
  CHECK(n_gale.run_up > 0.0);
  CHECK(n_gale.run_up < 0.02);

  const WaterSwashNumbers n_light = water_swash_numbers(light);
  CHECK(n_light.wave.height_m == 0.0);
  CHECK(n_light.run_up == 0.0);
  CHECK(light.waves.size() == gfx::k_water_waves);  // its ripples are still drawn

  const WaterSwashNumbers n_calm = water_swash_numbers(calm);
  CHECK(calm.scene_wind);
  CHECK(n_calm.wind == 0.0);
  CHECK(n_calm.run_up == 0.0);

  const WaterSwashNumbers n_lake = water_swash_numbers(lake);
  MESSAGE("a 500 m lake under 7 m/s: fetch " << n_lake.fetch_m << " m, H " << n_lake.wave.height_m
                                             << " m, T " << n_lake.wave.period_s << " s, run-up "
                                             << n_lake.run_up << " m");
  CHECK(n_lake.fetch_m > 350.0);
  CHECK(n_lake.fetch_m < 650.0);
  CHECK(n_lake.wave.height_m == gfx::water_sea_height(7.0, n_lake.fetch_m));
  CHECK(n_lake.wave.height_m > 0.05);
  CHECK(n_lake.wave.height_m < 0.12);

  const WaterSwashNumbers n_sea = water_swash_numbers(sea);
  const gfx::SwashWave laws = gfx::swash_wave(7.0, 100000.0);
  CHECK(n_sea.fetch_m == 100000.0);
  CHECK(n_sea.wave.height_m == laws.height_m);
  CHECK(n_sea.wave.period_s == laws.period_s);
  CHECK(n_sea.wave.height_m > 0.5);
}

TEST_CASE("water: the oasis pool stands in a hollow, its level below the ground round it") {
  const test::TempDir tmp("water_oasis_ground");
  const std::string wet = oasis_copy(tmp, true);
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(wet, desc, error), error);
  const TerrainSampler sampler(desc.terrain);
  const scene_gen::Ground ground = sampler.provider().view();
  const water::Body* body = nullptr;
  water::Body pool;
  for (const SceneMeshInfo& info : desc.mesh_info) {
    if (!info.water.has_value()) continue;
    REQUIRE(water::read_body(info.water->body, pool, &error));
    body = &pool;
  }
  REQUIRE(body != nullptr);
  const auto ring = [&](f64 r, f64& lo, f64& hi) {
    lo = 1e30;
    hi = -1e30;
    for (u32 k = 0; k < 64; ++k) {
      const f64 a = 6.283185307179586 * k / 64.0;
      const f64 h = ground.surface(
          WorldPos{body->center.x + r * std::cos(a), 0.0, body->center.z + r * std::sin(a)});
      lo = std::min(lo, h);
      hi = std::max(hi, h);
    }
  };
  const f64 centre = ground.surface(body->center);
  f64 edge_lo = 0.0, edge_hi = 0.0, rim_lo = 0.0, rim_hi = 0.0;
  ring(static_cast<f64>(body->radius_m), edge_lo, edge_hi);
  ring(25.0, rim_lo, rim_hi);
  const f32 deepest = water::depth_at(std::span<const water::Body>(body, 1), ground, body->center);
  MESSAGE("oasis ground: centre " << centre << ", at the edge " << edge_lo << ".." << edge_hi
                                  << ", 25 m out " << rim_lo << ".." << rim_hi << "; level "
                                  << body->level_m << ", deepest " << deepest);
  // Water two metres deep at the centre, the level under every crest 25 m out, and the edge dry all
  // round: the outline is everywhere where the ground has risen out of the water, so the surface
  // meets the sand at a shore inside it and never stands over the ground as a disc.
  CHECK(deepest > 2.0f);
  CHECK(body->level_m < rim_lo);
  CHECK(body->level_m < edge_lo);
}

TEST_CASE("water: the oasis pool drawn, only where the pool is, the same bytes twice") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_oasis");
  const std::string wet = oasis_copy(tmp, true);
  const std::string dry = oasis_copy(tmp, false);
  const std::string ddc = tmp.file("ddc");
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  Shot with[2];
  Shot without[1];
  Shot later[1];
  Shot waves[1];
  shoot(device, wet, ddc, 0.0, 2, with);
  shoot(device, dry, ddc, 0.0, 1, without);
  shoot(device, wet, ddc, 7200.0, 1, later);
  shoot(device, wet, ddc, 0.0, 1, waves, WaterQuality::High, WorldPos{-16.0, 9.0, -5.0},
        WorldPos{1.0, -1.0, 1.0}, 7.5);  // the sky held, the world 7.5 s on
  device.destroy();
  const CapturedFrame& a = with[0].frame;
  const CapturedFrame& b = without[0].frame;
  REQUIRE(a.width == b.width);
  REQUIRE(a.color.size() == b.color.size());
  REQUIRE(with[0].water_instance != ~0u);
  // The scene with the block draws with the resolve's water module and the one without it with the
  // plain module, which has no water branch to pay for (water.md, "Shading").
  CHECK(with[0].has_water);
  CHECK_FALSE(without[0].has_water);
  // The same frame twice: the same bytes.
  CHECK(std::equal(a.color.begin(), a.color.end(), with[1].frame.color.begin()));
  // With and without: every pixel that differs is a pixel of the pool, or within the sun's shadow
  // filter of one, and the pool is there. The band: on a device that shadows the sun with the
  // cascaded maps (the Titan Xp, which has no ray queries), the maps' 3x3 filter lets the pool's
  // own shadow touch the shore's last texels just past the pool's edge — gate b23 measured 33 such
  // pixels of 153,600 — where traced shadows stop at the edge itself. So pixels within
  // `k_shore_band` of a pool pixel count as the pool's here, and anything farther still may not
  // change.
  constexpr u32 k_shore_band = 3;
  const u32 pixels = a.width * a.height;
  std::vector<u8> by_pool(pixels, 0);
  for (u32 y = 0; y < a.height; ++y) {
    for (u32 x = 0; x < a.width; ++x) {
      if (a.ids[(y * a.width + x) * k_id_words] != with[0].water_instance) continue;
      const u32 y0 = y > k_shore_band ? y - k_shore_band : 0u;
      const u32 y1 = std::min(a.height - 1, y + k_shore_band);
      const u32 x0 = x > k_shore_band ? x - k_shore_band : 0u;
      const u32 x1 = std::min(a.width - 1, x + k_shore_band);
      for (u32 yy = y0; yy <= y1; ++yy) {
        for (u32 xx = x0; xx <= x1; ++xx)
          by_pool[yy * a.width + xx] = 1;
      }
    }
  }
  u32 pool = 0;
  u32 differ = 0;
  u32 differ_outside = 0;
  u32 moved = 0;
  u32 waved = 0;          // pool pixels the world clock alone moved
  u32 waved_outside = 0;  // and pixels it moved that are not the pool's
  for (u32 p = 0; p < pixels; ++p) {
    const bool in_pool = a.ids[p * k_id_words] == with[0].water_instance;
    pool += in_pool ? 1u : 0u;
    bool same = true;
    bool same_later = true;
    bool same_waves = true;
    for (u32 c = 0; c < 4; ++c) {
      same = same && a.color[p * 4 + c] == b.color[p * 4 + c];
      same_later = same_later && a.color[p * 4 + c] == later[0].frame.color[p * 4 + c];
      same_waves = same_waves && a.color[p * 4 + c] == waves[0].frame.color[p * 4 + c];
    }
    if (!same) {
      ++differ;
      if (by_pool[p] == 0) ++differ_outside;
    }
    if (in_pool && !same_later) ++moved;
    if (!same_waves) {
      if (in_pool) ++waved;
      if (by_pool[p] == 0) ++waved_outside;
    }
  }
  // **The waves are on the world clock, the sun on the sky's** (R88; water.md, "Shading"): with
  // the sky held at its hour and the world 7.5 s on, the pool moves and nothing else does — the
  // sun, the sky and the sand stand where they stood, whatever day length the session runs.
  MESSAGE(waved << " pool pixels move 7.5 world seconds on under a held sky, " << waved_outside
                << " outside it");
  CHECK(waved > pool / 4);
  CHECK(waved_outside == 0u);
  MESSAGE("pool " << pool << " pixels of " << pixels << "; " << differ << " differ with the block, "
                  << differ_outside << " of them outside it; " << moved
                  << " pool pixels change two hours on");
  // What a reader looks at when this fails (best effort, in the test's own directory).
  if (differ_outside != 0 || pool * 20 <= pixels) {
    tmp.keep();
    CaptureFiles files;
    std::string why;
    (void)write_capture(tmp.path(), "wet", a, files, why);
    (void)write_capture(tmp.path(), "dry", b, files, why);
    (void)write_capture(tmp.path(), "later", later[0].frame, files, why);
  }
  CHECK(pool * 20 > pixels);  // the pool is a twentieth of the picture or more
  CHECK(differ > pool / 2);
  CHECK(differ_outside == 0u);
  CHECK(moved > pool / 4);
}

// The water's two tiers (RenderSettings::water_quality; water.md, "Translucent by volume"): the
// two tables are the same length — the entry after the water's material carries the depth map at
// both tiers and the optics only at high (water.md, "The depth map") — high draws the same bytes
// twice, and every pixel the tiers differ in is a pixel of the pool or within the shadow filter's
// band of one, as with and without the block above.
TEST_CASE("water: the volume tier sees through the pool, and changes nothing but the pool") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_oasis_tiers");
  const std::string wet = oasis_copy(tmp, true);
  const std::string ddc = tmp.file("ddc");
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  Shot high[2];
  Shot low[1];
  shoot(device, wet, ddc, 0.0, 2, high, WaterQuality::High);
  shoot(device, wet, ddc, 0.0, 1, low, WaterQuality::Low);
  device.destroy();
  const CapturedFrame& a = high[0].frame;
  const CapturedFrame& b = low[0].frame;
  REQUIRE(a.color.size() == b.color.size());
  REQUIRE(high[0].water_instance != ~0u);
  CHECK(high[0].has_water);
  CHECK(low[0].has_water);
  CHECK(high[0].materials == low[0].materials);
  CHECK(std::equal(a.color.begin(), a.color.end(), high[1].frame.color.begin()));
  constexpr u32 k_shore_band = 3;
  const u32 pixels = a.width * a.height;
  std::vector<u8> by_pool(pixels, 0);
  u32 pool = 0;
  for (u32 y = 0; y < a.height; ++y) {
    for (u32 x = 0; x < a.width; ++x) {
      if (a.ids[(y * a.width + x) * k_id_words] != high[0].water_instance) continue;
      ++pool;
      for (u32 yy = y > k_shore_band ? y - k_shore_band : 0u;
           yy <= std::min(a.height - 1, y + k_shore_band); ++yy) {
        for (u32 xx = x > k_shore_band ? x - k_shore_band : 0u;
             xx <= std::min(a.width - 1, x + k_shore_band); ++xx)
          by_pool[yy * a.width + xx] = 1;
      }
    }
  }
  u32 differ = 0;
  u32 differ_outside = 0;
  for (u32 p = 0; p < pixels; ++p) {
    bool same = true;
    for (u32 c = 0; c < 4; ++c)
      same = same && a.color[p * 4 + c] == b.color[p * 4 + c];
    if (same) continue;
    ++differ;
    if (by_pool[p] == 0) ++differ_outside;
  }
  MESSAGE("pool " << pool << " pixels of " << pixels << "; " << differ
                  << " differ between the tiers, " << differ_outside << " of them outside it");
  if (differ_outside != 0 || differ <= pool / 2) {
    tmp.keep();
    CaptureFiles files;
    std::string why;
    (void)write_capture(tmp.path(), "high", a, files, why);
    (void)write_capture(tmp.path(), "low", b, files, why);
  }
  CHECK(pool * 20 > pixels);
  CHECK(differ > pool / 2);
  CHECK(differ_outside == 0u);
}

namespace {
// The bottom's colour across the water mesh's triangle edges, against the noise (`facet_contrast`).
// For every two neighbouring pixels of the pool, side by side or one above the other, the step in
// their colour (the sum of the three channels, signed, towards +x or +y), gathered by where the
// ids channel puts them: the pairs inside one triangle by (triangle, direction), and the pairs
// across an edge — a ring, a spoke of the fan or a quad's diagonal, as the LOD cut drew them — by
// (the two triangles, direction). An edge's **excess** is its pairs' mean step less the mean of
// its two triangles' own: what the colour does at the edge beyond the gradient it has either side.
// A bottom shaded per triangle has an excess at every edge; a bottom that follows the ground has
// none, and what is measured is the output's dither, a step of a unit or two in every pixel. So
// the excess is set against what the dither alone would give it — sqrt(2 / pi) times its standard
// error, from the scatter of the steps inside the triangles — and `ratio` is 1 within the noise
// and well above it when the triangles show. The pixels within `k_shore_px` of one that is not the
// pool are left out: the shore's own edge is not a facet.
struct FacetContrast {
  f64 excess = 0.0;  // the edges' mean |excess|, weighted by their pairs
  f64 noise = 0.0;   // what the dither alone would make of it
  u32 edges = 0;
  f64 ratio() const noexcept { return noise > 0.0 ? excess / noise : 0.0; }
};

FacetContrast facet_contrast(const Shot& shot) {
  constexpr u32 k_shore_px = 4;
  constexpr u32 k_min_pairs = 4;
  const CapturedFrame& f = shot.frame;
  const u32 w = f.width;
  const u32 h = f.height;
  const auto pool = [&](u32 x, u32 y) {
    return f.ids[(y * w + x) * k_id_words] == shot.water_instance;
  };
  // Pool pixels at least k_shore_px from anything that is not the pool.
  std::vector<u8> inner(w * h, 0);
  for (u32 y = k_shore_px; y + k_shore_px < h; ++y) {
    for (u32 x = k_shore_px; x + k_shore_px < w; ++x) {
      bool all = true;
      for (u32 yy = y - k_shore_px; all && yy <= y + k_shore_px; ++yy)
        for (u32 xx = x - k_shore_px; all && xx <= x + k_shore_px; ++xx)
          all = pool(xx, yy);
      inner[y * w + x] = all ? 1 : 0;
    }
  }
  const auto colour = [&](u32 p) {
    return static_cast<f64>(f.color[p * 4]) + static_cast<f64>(f.color[p * 4 + 1]) +
           static_cast<f64>(f.color[p * 4 + 2]);
  };
  const auto triangle = [&](u32 p) {
    return (u64{f.ids[p * k_id_words + 1]} << 8) | u64{f.ids[p * k_id_words + 2]};
  };
  struct Sum {
    f64 sum = 0.0;
    f64 squares = 0.0;
    u32 n = 0;
    f64 mean() const noexcept { return n != 0 ? sum / n : 0.0; }
  };
  // engine-lint: allow-std-container a test's tally
  std::map<std::pair<u64, u32>, Sum> within;
  // engine-lint: allow-std-container a test's tally
  std::map<std::tuple<u64, u64, u32>, Sum> across;
  for (u32 y = 0; y + 1 < h; ++y) {
    for (u32 x = 0; x + 1 < w; ++x) {
      const u32 p = y * w + x;
      if (inner[p] == 0) continue;
      for (u32 dir = 0; dir < 2; ++dir) {
        const u32 q = dir == 0 ? p + 1 : p + w;
        if (inner[q] == 0) continue;
        const f64 step = colour(q) - colour(p);
        Sum& s = triangle(p) == triangle(q) ? within[{triangle(p), dir}]
                                            : across[{triangle(p), triangle(q), dir}];
        s.sum += step;
        s.squares += step * step;
        ++s.n;
      }
    }
  }
  // The dither's scatter: the steps' variance about their triangle's mean, pooled.
  f64 scatter = 0.0;
  u32 scatter_n = 0;
  for (const auto& [key, s] : within) {
    if (s.n < k_min_pairs) continue;
    scatter += s.squares - s.sum * s.sum / s.n;
    scatter_n += s.n - 1;
  }
  const f64 variance = scatter_n != 0 ? scatter / scatter_n : 0.0;
  FacetContrast out;
  u32 pairs = 0;
  for (const auto& [key, s] : across) {
    const auto a = within.find({std::get<0>(key), std::get<2>(key)});
    const auto b = within.find({std::get<1>(key), std::get<2>(key)});
    if (s.n < k_min_pairs || a == within.end() || b == within.end() || a->second.n < k_min_pairs ||
        b->second.n < k_min_pairs)
      continue;
    const f64 excess = s.mean() - 0.5 * (a->second.mean() + b->second.mean());
    const f64 error = std::sqrt(variance * (1.0 / s.n + 0.25 / a->second.n + 0.25 / b->second.n));
    out.excess += std::abs(excess) * s.n;
    out.noise += std::sqrt(2.0 / 3.141592653589793) * error * s.n;
    pairs += s.n;
    ++out.edges;
  }
  if (pairs != 0) {
    out.excess /= pairs;
    out.noise /= pairs;
  }
  return out;
}
}  // namespace

// **No ring and no fan** (water.md, "Testing"): the oasis pool at high, its waves calmed so the
// bottom is all there is to see, from nine metres over its east shore looking down across it (the
// owner's view of the rings) and from straight over its middle, drawn once with its depth map and
// once with the map made unreadable — the reader then has none, and the branch falls back to the
// triangle's plane of vertex depths, which is the picture before the map. With the map the
// bottom's colour steps no more across the mesh's triangle edges than within its triangles;
// without it, it does.
TEST_CASE("water: the bottom seen through the pool shows no ring or fan of the mesh") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_oasis_rings");
  std::string text;
  REQUIRE(io::read_file(oasis_copy(tmp, true), text) == io::Status::Ok);
  const std::string calm = tmp.file("calm.json");
  {
    // The waves calmed: a mirror, so the reflection is the sky's and the same everywhere.
    const usize key = text.find("\"wave_amplitude_m\"");
    REQUIRE(key != std::string::npos);
    const usize at = text.find(':', key) + 1;
    const usize end = text.find_first_of(",}", at);
    text.replace(at, end - at, "0");
    REQUIRE(write_text(calm, text));
  }
  const std::string ddc = tmp.file("ddc");
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  constexpr u32 k_views = 2;
  const WorldPos eye[k_views] = {WorldPos{7.0, 9.0, 2.0}, WorldPos{0.0, 20.0, 0.5}};
  const WorldPos look[k_views] = {WorldPos{-3.0, -1.5, -1.0}, WorldPos{0.0, -0.45, 0.0}};
  Shot mapped[k_views];
  for (u32 v = 0; v < k_views; ++v)
    shoot(device, calm, ddc, 0.0, 1, &mapped[v], WaterQuality::High, eye[v], look[v]);
  // The map unreadable: the capability finds a file under its name and leaves it; the reader
  // refuses it and the body has no map.
  u32 maps = 0;
  for (const auto& entry : std::filesystem::directory_iterator(io::join_path(ddc, "water"))) {
    if (entry.path().extension() != ".depth") continue;
    REQUIRE(write_text(entry.path().string(), "not a depth map"));
    ++maps;
  }
  REQUIRE(maps == 1u);
  Shot planes[k_views];
  for (u32 v = 0; v < k_views; ++v)
    shoot(device, calm, ddc, 0.0, 1, &planes[v], WaterQuality::High, eye[v], look[v]);
  device.destroy();
  bool keep = false;
  for (u32 v = 0; v < k_views; ++v) {
    // The entry after the material is there either way: it carries the optics at high.
    CHECK(mapped[v].materials == planes[v].materials);
    const FacetContrast with = facet_contrast(mapped[v]);
    const FacetContrast without = facet_contrast(planes[v]);
    MESSAGE("view " << v << ": the bottom's step at the mesh's triangle edges over the dither's: "
                    << "with the map " << with.excess << " / " << with.noise << " = "
                    << with.ratio() << " over " << with.edges
                    << " edges; with the triangles' planes " << without.excess << " / "
                    << without.noise << " = " << without.ratio() << " over " << without.edges);
    CHECK(with.edges > 200u);
    CHECK(with.ratio() < 1.15);
    CHECK(without.ratio() > 1.4);
    keep = keep || !(with.ratio() < 1.15) || !(without.ratio() > 1.4);
  }
  if (keep) {
    tmp.keep();
    CaptureFiles files;
    std::string why;
    for (u32 v = 0; v < k_views; ++v) {
      (void)write_capture(tmp.path(), "mapped" + std::to_string(v), mapped[v].frame, files, why);
      (void)write_capture(tmp.path(), "planes" + std::to_string(v), planes[v].frame, files, why);
    }
  }
}
// **The sea to the horizon** (water.md, "To the horizon"; S93): a pool made open draws the same
// water past its outline wherever the ground is below its level and to the horizon past the
// ground's edge, and nowhere else. The camera stands 6 m up looking level along +x over the
// renderer's own dunes (32 m of them, then nothing), so every pixel's ray is known on the CPU: its
// rise is ndc_y tan(fov/2) per unit along the view axis, it meets the plane at the level
// (level - 6) / (ndc_y tan(fov/2)) along that axis, and the buffer's surface is znear / depth along
// it. Open against closed: the ids and the depth are the same (the plane is no geometry); a pixel
// changes only where the plane is nearer than the surface or there is none and the ray looks down,
// and every such pixel changes; the pool's own pixels do not change at all (its shore is 0, so
// the depth-faded shore of an open body and the outline's of a closed one are both none); nothing
// above the horizon changes; and the rows just under the horizon past the dunes are sea.
TEST_CASE("water: an open body's sea goes on past its outline to the horizon, and not over land") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_open_sea");
  constexpr const char* k_body = R"([{"name": "sea", "level_m": 0.2, "shore_m": 0,
    "shape": {"circle": {"center": [3, 0, -2], "radius_m": 6}}, "wind_follow": false,
    "direction_deg": 90, "shallow": [0.2, 0.5, 0.45], "open": OPEN}])";
  const auto body = [&](bool open) {
    std::string text = k_body;
    text.replace(text.find("OPEN"), 4, open ? "true" : "false");
    return text;
  };
  const std::string open_path = tmp.file("open.json");
  const std::string closed_path = tmp.file("closed.json");
  REQUIRE(write_text(open_path, scene_text(body(true))));
  REQUIRE(write_text(closed_path, scene_text(body(false))));
  const std::string ddc = tmp.file("ddc");
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  const WorldPos eye{-14.0, 6.0, -2.0};
  const WorldPos look{100.0, 6.0, -2.0};
  for (WaterQuality quality : {WaterQuality::High, WaterQuality::Low}) {
    Shot open[2];
    Shot closed[1];
    shoot(device, open_path, ddc, 0.0, 2, open, quality, eye, look);
    shoot(device, closed_path, ddc, 0.0, 1, closed, quality, eye, look);
    const CapturedFrame& a = open[0].frame;
    const CapturedFrame& b = closed[0].frame;
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    REQUIRE(a.depth.size() == static_cast<usize>(a.width) * a.height);
    REQUIRE(open[0].water_instance != ~0u);
    CHECK(open[0].water_open == open[0].water_instance);
    CHECK(closed[0].water_open == gfx::k_no_water_open);
    CHECK(std::equal(a.color.begin(), a.color.end(), open[1].frame.color.begin()));
    const Camera camera;  // the capture's: fov_y and znear at their defaults
    const f64 rise = std::tan(0.5 * static_cast<f64>(camera.fov_y));
    const f64 level = 0.2;
    u32 ids_differ = 0;
    u32 depth_differ = 0;
    u32 pool_changed = 0;
    u32 sea = 0;          // pixels the plane must draw, clear of the coast
    u32 sea_missing = 0;  // of those, unchanged
    u32 wrong = 0;        // changed where the plane is behind the surface, clear of the coast
    u32 above = 0;        // changed above the horizon
    u32 horizon = 0;      // sea pixels in the eight rows under the horizon with no surface
    for (u32 y = 0; y < a.height; ++y) {
      const f64 ndc_y = 1.0 - (static_cast<f64>(y) + 0.5) / static_cast<f64>(a.height) * 2.0;
      for (u32 x = 0; x < a.width; ++x) {
        const u32 p = y * a.width + x;
        for (u32 w = 0; w < k_id_words; ++w)
          ids_differ += a.ids[p * k_id_words + w] != b.ids[p * k_id_words + w] ? 1u : 0u;
        depth_differ += a.depth[p] != b.depth[p] ? 1u : 0u;
        bool same = true;
        for (u32 c = 0; c < 4; ++c)
          same = same && a.color[p * 4 + c] == b.color[p * 4 + c];
        if (a.ids[p * k_id_words] == open[0].water_instance) {
          pool_changed += same ? 0u : 1u;
          continue;
        }
        if (ndc_y >= 0.0) {
          above += same ? 0u : 1u;
          continue;
        }
        const f64 plane = (level - eye.y) / (ndc_y * rise);  // along the view axis
        const bool covered = a.depth[p] > 0.0f;
        const f64 surface =
            covered ? static_cast<f64>(camera.znear) / static_cast<f64>(a.depth[p]) : 1e30;
        // Clear of the coast: the plane and the surface more than 3% apart along the ray.
        if (plane < surface * 0.97) {
          ++sea;
          sea_missing += same ? 1u : 0u;
          if (!covered && y < a.height / 2 + 8) ++horizon;
        } else if (plane > surface * 1.03) {
          wrong += same ? 0u : 1u;
        }
      }
    }
    MESSAGE(std::string(quality == WaterQuality::High ? "high" : "low")
            << ": sea " << sea << " pixels (" << sea_missing << " unchanged), " << horizon
            << " under the horizon past the dunes; changed over land " << wrong
            << ", above the horizon " << above << ", in the pool " << pool_changed << "; "
            << a.covered << " pixels covered");
    if (sea_missing != 0 || wrong != 0 || above != 0 || pool_changed != 0) {
      tmp.keep();
      CaptureFiles files;
      std::string why;
      (void)write_capture(tmp.path(), "open", a, files, why);
      (void)write_capture(tmp.path(), "closed", b, files, why);
    }
    CHECK(ids_differ == 0u);
    CHECK(depth_differ == 0u);
    CHECK(pool_changed == 0u);
    CHECK(sea > a.width * 8);
    CHECK(sea_missing == 0u);
    CHECK(wrong == 0u);
    CHECK(above == 0u);
    CHECK(horizon > a.width);
  }
  device.destroy();
}

// S113 (world_sound.md, "Running water and the surf"; M3 of its review): the surf's power is the
// wave energy flux the field loses, where it loses it — read from the field the GPU scene draws its
// waves from (`water_surface_of`, the same function), so a record's arrival is the drawn train's at
// its texel. A still lake has no train and no record; a lake under 7 m/s has records along its
// shore, each in water where the field's `kept` share falls; the total does not depend on the
// cell; a field that loses nothing (unbroken, or a kept share that holds level, as behind a bar) is
// silent; and over a transect the records sum to the flux in less the flux out.
TEST_CASE("water: the surf records are the flux the drawn train loses, where it loses it") {
  const test::TempDir tmp("water_surf_records");
  constexpr const char* k_large = R"({"size": 129, "extent": 1024, "seed": 2, "dune_height": 0,
      "basins": [{"center": [0, 0], "radius": 350, "depth": 10}]})";
  const auto read = [&](const char* name, const char* wind) {
    const std::string text =
        std::string(R"({"format": "engine.scene.v1", "name": "surf-test", "terrain": )") + k_large +
        R"(, "water": [{"name": "lake", "level_m": -2, "wind_follow": false, "direction_deg": 30,
        "shape": {"fill": {"seed": [0, 0, 0], "max_extent_m": 900}}, "wind_m_s": )" +
        wind + "}]}\n";
    const std::string path = tmp.file(name);
    REQUIRE(write_text(path, text));
    SceneDesc desc;
    std::string error;
    REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
    REQUIRE(desc.mesh_info.size() >= 1u);
    REQUIRE(desc.mesh_info[0].water.has_value());
    return *desc.mesh_info[0].water;
  };
  Vector<SurfRecord> records;
  water_surf_records(read("still.json", "0"), 16.0, records);
  CHECK(records.empty());
  const SceneWater lake = read("lake.json", "7");
  water_surf_records(lake, 16.0, records);
  MESSAGE("a 700 m lake under 7 m/s: " << records.size() << " surf records");
  REQUIRE(!records.empty());
  // The same field, built as the GPU scene builds it: each record's arrival is its texel's, and
  // the scan over it gives the same records the building form did.
  gfx::WaterSurface surface;
  Vector<f32> field;
  water_surface_of(lake, water_swash_numbers(lake), surface, field);
  const std::span<const f32> view{field.data(), field.size()};
  Vector<SurfRecord> scanned;
  water_surf_records(lake, surface, view, 16.0, scanned);
  REQUIRE(scanned.size() == records.size());
  const SceneWaterDepth& map = lake.depth;
  const u32 size = map.size;
  const auto texel_of = [&](u32 i, u32 j) {
    return field.data() + u64{gfx::k_water_field_channels} * (u64{j} * size + i);
  };
  f64 total16 = 0.0;
  for (u32 k = 0; k < records.size(); ++k) {
    const SurfRecord& r = records[k];
    CHECK(r.power_w_m == scanned[k].power_w_m);
    const u32 i = static_cast<u32>(std::lround((r.at.x - map.origin.x) / map.texel_m));
    const u32 j = static_cast<u32>(std::lround((r.at.y - map.origin.y) / map.texel_m));
    REQUIRE(i < size);
    REQUIRE(j < size);
    const f32* t = texel_of(i, j);
    CHECK(r.arrival_s == t[1]);
    CHECK(t[0] > 0.0f);  // in the water
    // Where the field takes something: a kept share under one, and a neighbour that kept more.
    CHECK(t[6] < 1.0f);
    f32 most = 0.0f;
    if (i > 0) most = std::max(most, texel_of(i - 1, j)[6]);
    if (i + 1 < size) most = std::max(most, texel_of(i + 1, j)[6]);
    if (j > 0) most = std::max(most, texel_of(i, j - 1)[6]);
    if (j + 1 < size) most = std::max(most, texel_of(i, j + 1)[6]);
    CHECK(most > t[6]);
    CHECK(r.height_m > 0.0f);
    CHECK(r.height_m <= 0.78f * t[0] * 1.0001f);  // at most the breaking limit there
    CHECK(r.period_s == surface.period_s);
    CHECK(r.power_w_m > 0.0f);
    CHECK(std::fabs(r.along.x * r.along.x + r.along.y * r.along.y - 1.0f) < 1e-4f);
    total16 += static_cast<f64>(r.power_w_m) * static_cast<f64>(r.length_m);
  }
  // **The total does not depend on the cell**: half the cell, and twice, lose the same watts to
  // within a part in ten thousand (each texel's loss is counted once in whichever cell holds it;
  // what differs is the f32 a record keeps and the -60 dB floor a cell is silent under).
  for (const f64 cell : {8.0, 32.0}) {
    Vector<SurfRecord> other;
    water_surf_records(lake, surface, view, cell, other);
    f64 total = 0.0;
    for (const SurfRecord& r : other)
      total += static_cast<f64>(r.power_w_m) * static_cast<f64>(r.length_m);
    MESSAGE(cell << " m cells: " << other.size() << " records, " << total << " W against "
                 << total16 << " W at 16 m");
    CHECK(std::fabs(total - total16) <= 1e-4 * total16);
  }
  // **Silent where the field loses nothing**: the train unbroken everywhere, and a kept share
  // that holds level (what a trough behind a bar carries across it).
  Vector<f32> whole = field;
  for (u32 at = 0; at < size * size; ++at)
    whole[u64{gfx::k_water_field_channels} * at + 6] = 1.0f;
  water_surf_records(lake, surface, {whole.data(), whole.size()}, 16.0, scanned);
  CHECK(scanned.empty());
  for (u32 at = 0; at < size * size; ++at)
    whole[u64{gfx::k_water_field_channels} * at + 6] = 0.4f;
  water_surf_records(lake, surface, {whole.data(), whole.size()}, 16.0, scanned);
  CHECK(scanned.empty());
  // **A transect**: a plane beach of the lake's grid, every texel water, the train arriving along
  // +x (an arrival rising with x), nothing refracted, and the kept share falling from 1 to 0.3 over
  // the middle third. Each row's losses telescope: the records sum to the flux in less the flux
  // out, (1 - 0.09) of the sea's deep-water flux across the beach's width, at any cell.
  Vector<f32> beach = field;
  for (u32 j = 0; j < size; ++j) {
    for (u32 i = 0; i < size; ++i) {
      f32* t = beach.data() + u64{gfx::k_water_field_channels} * (u64{j} * size + i);
      const f64 x = static_cast<f64>(i) / static_cast<f64>(size - 1);
      t[0] = static_cast<f32>(10.0 - 9.5 * x);
      t[1] = static_cast<f32>(2.0 * i);
      t[5] = 1.0f;
      const f64 into = std::clamp((x - 1.0 / 3.0) * 3.0, 0.0, 1.0);
      t[6] = static_cast<f32>(1.0 - 0.7 * into);
    }
  }
  const f64 h0 = static_cast<f64>(surface.height_m);
  const f64 flux0 = 998.2 * 9.81 * 9.81 * h0 * h0 * static_cast<f64>(surface.period_s) /
                    (64.0 * 3.14159265358979323846);
  const f64 width = static_cast<f64>(size) * static_cast<f64>(map.texel_m);
  const f64 lost = flux0 * (1.0 - static_cast<f64>(0.3f) * static_cast<f64>(0.3f)) * width;
  for (const f64 cell : {8.0, 16.0, 64.0}) {
    water_surf_records(lake, surface, {beach.data(), beach.size()}, cell, scanned);
    f64 total = 0.0;
    for (const SurfRecord& r : scanned)
      total += static_cast<f64>(r.power_w_m) * static_cast<f64>(r.length_m);
    MESSAGE("the transect at " << cell << " m cells: " << total << " W against " << lost);
    CHECK(std::fabs(total - lost) <= 1e-4 * lost);
  }
}
#endif

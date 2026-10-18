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
#include <fstream>
#include <string>

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
};

// One capture of a scene file from above the pool's west shore, towards the sun's reflection.
void shoot(const gfx::Device& device, const std::string& path, const std::string& ddc,
           f64 sun_time_s, u32 shots, Shot* out) {
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  desc.ddc = ddc;
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  RenderSettings settings;
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
  frame.camera.position = WorldPos{-16.0, 9.0, -5.0};
  frame.camera.target = WorldPos{1.0, -1.0, 1.0};
  frame.sun_time_s = sun_time_s;
  CaptureChannels channels;
  channels.ids = true;
  u32 water_instance = ~0u;
  for (u32 i = 0; i < data.instances.size(); ++i) {
    const u32 mesh = data.instances[i].mesh;
    if (mesh < data.mesh_info.size() && data.mesh_info[mesh].water.has_value()) water_instance = i;
  }
  for (u32 s = 0; s < shots; ++s) {
    REQUIRE_MESSAGE(renderer.capture(frame, channels, out[s].frame, &error), error);
    out[s].water_instance = water_instance;
  }
}

}  // namespace

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
  shoot(device, wet, ddc, 0.0, 2, with);
  shoot(device, dry, ddc, 0.0, 1, without);
  shoot(device, wet, ddc, 7200.0, 1, later);
  device.destroy();
  const CapturedFrame& a = with[0].frame;
  const CapturedFrame& b = without[0].frame;
  REQUIRE(a.width == b.width);
  REQUIRE(a.color.size() == b.color.size());
  REQUIRE(with[0].water_instance != ~0u);
  // The same frame twice: the same bytes.
  CHECK(std::equal(a.color.begin(), a.color.end(), with[1].frame.color.begin()));
  // With and without: every pixel that differs is a pixel of the pool, and the pool is there.
  u32 pool = 0;
  u32 differ = 0;
  u32 differ_outside = 0;
  u32 moved = 0;
  const u32 pixels = a.width * a.height;
  for (u32 p = 0; p < pixels; ++p) {
    const bool in_pool = a.ids[p * k_id_words] == with[0].water_instance;
    pool += in_pool ? 1u : 0u;
    bool same = true;
    bool same_later = true;
    for (u32 c = 0; c < 4; ++c) {
      same = same && a.color[p * 4 + c] == b.color[p * 4 + c];
      same_later = same_later && a.color[p * 4 + c] == later[0].frame.color[p * 4 + c];
    }
    if (!same) {
      ++differ;
      if (!in_pool) ++differ_outside;
    }
    if (in_pool && !same_later) ++moved;
  }
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
#endif

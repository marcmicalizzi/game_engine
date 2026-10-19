// A scene naming a scatter layer, read by the renderer's scene reader as engine-view and
// engine-host read one (docs/subsystems/scatter.md, "Where it enters the renderer"): the generator
// is found in the registry by name, its procedural rock written once into the scene's directory and
// appended as one mesh, a few hundred instances of it standing on the terrain's own ground — to the
// bit against the ground's surface, and within a cell of the drawn grid's corners — and, on a GPU,
// the capture's ids channel naming most of them on the picture.
#include <core/json/json.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <core/schema/json_reflect.h>
#include <domain/gfx/device.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/view_set.h>
#include <systems/scatter/scatter.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::renderer;

namespace {

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

// A 64 m square of the renderer's own waves, 1 m a cell, and a layer of small rocks over all of it
// on 16 m tiles.
std::string scene_text() {
  return R"({"format":"engine.scene.v1","name":"scatter",)"
         R"("terrain":{"size":65,"extent":32,"seed":3,"dune_height":1.0,"dune_wavelength":20},)"
         R"("placements":[{"generator":"scatter","params":{"name":"pebbles","seed":11,)"
         R"("tile_size":16,"region":{"kind":"Tiles","tile_min":[-2,-2],"tile_max":[1,1]},)"
         R"("density":{"per_m2":0.08},"meshes":"rocks",)"
         R"("kinds":[{"name":"rock","spacing":1.5,"scale_min":0.8,"scale_max":1.2,)"
         R"("rock":{"seed":4,"size_x":0.45,"size_y":0.3,"size_z":0.4,"detail":2}}]}}]})";
}

bool read_json_file(const std::string& path, JsonValue& out) {
  std::string text;
  if (io::read_file(path, text) != io::Status::Ok) return false;
  return parse_json(text, out).ok;
}

// A variant of desert-endless with a layer: `scene.json` field for field but its name, its
// description and its placements — a second copy of the erg's bands and sand that nothing held
// equal would drift, as desert-endless's own copy of the erg's detail once did (its README) — and
// its one layer, opened as the generator opens it. Nothing is placed or written.
bool read_variant(const std::string& dir, const char* file, const JsonValue& scene,
                  scatter::Layer& layer, bool& stream) {
  JsonValue variant;
  if (!read_json_file(dir + "/" + file, variant)) return false;
  REQUIRE(variant.is_object());
  for (const auto& [key, value] : variant.as_object()) {
    if (key == "name" || key == "description" || key == "placements") continue;
    const JsonValue* original = scene.find(key);
    REQUIRE_MESSAGE(original != nullptr, file << " has a field scene.json has not: " << key);
    CHECK_MESSAGE(*original == value, file << "'s " << key << " is not scene.json's");
  }
  for (const auto& [key, value] : scene.as_object()) {
    (void)value;
    if (key == "name" || key == "description") continue;
    CHECK_MESSAGE(variant.find(key) != nullptr, file << " lacks scene.json's " << key);
  }
  const JsonValue* placements = variant.find("placements");
  REQUIRE(placements != nullptr);
  REQUIRE(placements->size() == 1);
  const JsonValue& entry = (*placements)[usize{0}];
  REQUIRE(entry.find("generator") != nullptr);
  CHECK(entry.find("generator")->as_string() == scatter::k_placement_generator);
  stream = entry.find("stream") == nullptr || entry.find("stream")->as_bool();
  schema::ReadContext ctx;
  REQUIRE(schema::from_json(layer, *entry.find("params"), ctx));
  REQUIRE(ctx.ok());
  return true;
}

}  // namespace

TEST_CASE("scatter: desert-endless's scattered variants are the scene and a layer") {
  const std::string dir = test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/desert-endless",
                                          "content/test-scenes/desert-endless");
  JsonValue scene;
  if (!read_json_file(dir + "/scene.json", scene)) {
    MESSAGE("the endless desert is not here; skipped");
    return;
  }
  REQUIRE(scene.is_object());
  // `scattered.json`, the one engine-view flies (since 2026-10-12): the layer streamed beside the
  // ground's tiles over the whole erg, the boulders to the second ring and the tufts in the first.
  scatter::Layer streamed;
  bool stream = false;
  REQUIRE(read_variant(dir, "scattered.json", scene, streamed, stream));
  CHECK(stream);
  CHECK(streamed.region.kind == scatter::RegionKind::Scene);
  REQUIRE(streamed.kinds.size() == 2);
  // `far` against the scene's rings (1.5, 8, 24, 64 tiles of 32 m): a ring places a kind when its
  // nearest tile is within the kind's `far` — the boulder in the first two rings (inner radii 0 and
  // 48 m), the tuft in the first alone.
  CHECK(streamed.kinds[0].far >= 48.0f);
  CHECK(streamed.kinds[0].far < 256.0f);
  CHECK(streamed.kinds[1].far > 0.0f);
  CHECK(streamed.kinds[1].far < 48.0f);
  scatter::Placer placer;
  std::string error;
  REQUIRE_MESSAGE(placer.open(streamed, dir, 7, &error), error);
  Vector<scene_gen::TileCoord> tiles;
  CHECK_FALSE(placer.region_tiles(tiles, &error));  // a region of the whole scene has no whole
  // `scattered-bounded.json`: the same layer read whole over a polygon along the camera path.
  scatter::Layer bounded;
  REQUIRE(read_variant(dir, "scattered-bounded.json", scene, bounded, stream));
  CHECK_FALSE(stream);
  CHECK(bounded.region.kind == scatter::RegionKind::Polygon);
  scatter::Placer whole;
  REQUIRE_MESSAGE(whole.open(bounded, dir, 7, &error), error);
  REQUIRE(whole.region_tiles(tiles, &error));
  MESSAGE("the bounded variant's layer covers " << tiles.size() << " tiles");
  CHECK(tiles.size() < 16000);
  // The two place the same kinds by the same rules: only the region, `stream` and `far` differ.
  REQUIRE(bounded.kinds.size() == streamed.kinds.size());
  for (u32 k = 0; k < bounded.kinds.size(); ++k) {
    CHECK(bounded.kinds[k].spacing == streamed.kinds[k].spacing);
    CHECK(bounded.kinds[k].weight == streamed.kinds[k].weight);
    CHECK(bounded.kinds[k].slope_max_deg == streamed.kinds[k].slope_max_deg);
  }
}

// The host's derived-data root (`--ddc`, `SceneDesc::ddc`, which the reader hands the generator as
// `scene_gen::Context::ddc`): a layer that names no `meshes` writes its procedural rock under
// `<root>/scatter/` and nothing beside the scene, so a game repository's root is honoured wherever
// its scenes are, as the water's meshes are. With no root named, the walk from the scene's
// directory, and outside any checkout `scatter/` beside the scene. Before the fix the rock went
// beside the scene whatever root the host named.
TEST_CASE("scatter: the procedural rock goes under the host's derived-data root, else the walk") {
  const test::TempDir tmp("scatter_scene_ddc");
  std::string text = scene_text();
  const std::string named = R"("meshes":"rocks",)";
  const usize at = text.find(named);
  REQUIRE(at != std::string::npos);
  text.erase(at, named.size());
  const std::string scene_path = tmp.file("scene.json");
  REQUIRE(write_text(scene_path, text));
  const std::string root = tmp.file("game-ddc");
  SceneDesc desc;
  desc.ddc = root;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(scene_path, desc, error), error);
  REQUIRE(desc.meshes.size() == 2);
  CHECK(io::parent_path(desc.meshes[0]) == io::join_path(root, "scatter"));
  CHECK(io::exists(desc.meshes[0]));
  CHECK_FALSE(io::exists(tmp.file("scatter")));

  SceneDesc bare;
  REQUIRE_MESSAGE(read_scene_file(scene_path, bare, error), error);
  REQUIRE(bare.meshes.size() == 2);
  CHECK(io::parent_path(bare.meshes[0]) == io::join_path(io::parent_path(scene_path), "scatter"));
  CHECK(io::exists(bare.meshes[0]));
  // The same rock is the same bytes, so the same file name in either place.
  CHECK(io::file_name(bare.meshes[0]) == io::file_name(desc.meshes[0]));
}

TEST_CASE("scatter: a scene's layer is read as instances of one rock standing on its ground") {
  const test::TempDir tmp("scatter_scene");
  const std::string scene_path = tmp.file("scene.json");
  REQUIRE(write_text(scene_path, scene_text()));
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(scene_path, desc, error), error);
  // The rock, then the terrain.
  REQUIRE(desc.meshes.size() == 2);
  CHECK(desc.meshes[0].find("rock-") != std::string::npos);
  CHECK(std::ifstream(desc.meshes[0], std::ios::binary).good());
  CHECK(desc.mesh_info[0].name.find("scatter/") == 0);
  const u32 placed = desc.placed_instances;
  MESSAGE(placed << " rocks placed");
  REQUIRE(placed > 200);
  REQUIRE(placed < 600);
  REQUIRE(desc.instances.size() == placed + 1);
  // Every rock on the ground the reader handed the generator, to the bit, and within the drawn
  // grid's cell: between the lowest and highest of its four corners, less what the waves curve
  // inside a metre (a centimetre and a bit at 1 m high and 20 m long).
  const TerrainSampler sampler(desc.terrain);
  const scene_gen::Ground ground = sampler.provider().view();
  for (u32 i = 0; i < placed; ++i) {
    const SceneInstance& rock = desc.instances[i];
    CHECK(rock.mesh == 0);
    CHECK(rock.origin.y == ground.surface(rock.origin));
    const f64 step = 1.0;
    const f64 gx = std::floor((rock.origin.x + 32.0) / step);
    const f64 gz = std::floor((rock.origin.z + 32.0) / step);
    f64 lo = 1e30;
    f64 hi = -1e30;
    for (f64 dz = 0.0; dz <= 1.0; dz += 1.0) {
      for (f64 dx = 0.0; dx <= 1.0; dx += 1.0) {
        const f64 h = static_cast<f64>(terrain_height(desc.terrain,
                                                      static_cast<f32>(-32.0 + (gx + dx) * step),
                                                      static_cast<f32>(-32.0 + (gz + dz) * step)));
        lo = std::min(lo, h);
        hi = std::max(hi, h);
      }
    }
    CHECK(rock.origin.y >= lo - 0.02);
    CHECK(rock.origin.y <= hi + 0.02);
  }
  // Read again: the same instances, to the bit.
  SceneDesc again;
  REQUIRE(read_scene_file(scene_path, again, error));
  REQUIRE(again.instances.size() == desc.instances.size());
  for (u32 i = 0; i < desc.instances.size(); ++i) {
    CHECK(again.instances[i].origin == desc.instances[i].origin);
    CHECK(again.instances[i].transform == desc.instances[i].transform);
  }

  // On a GPU: drawn from 40 m up, the capture's ids name most of the rocks (three in four on
  // the RTX 5090; the rest behind the waves' crests or under the frame's edges).
  {
    gfx::Device probe;
    std::string why;
    if (!probe.create(gfx::DeviceOptions{}, &why)) {
      MESSAGE("renderer unavailable here: " << why);
      return;
    }
    const bool atomics = probe.features().buffer_int64_atomics;
    probe.destroy();
    if (!atomics) {
      MESSAGE("no 64-bit buffer atomics here");
      return;
    }
  }
  desc.ddc = tmp.file("ddc");
  gfx::Device device;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  std::vector<u32> seen(placed, 0u);
  u32 rock_pixels = 0;
  {
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
    rd.width = 640;
    rd.height = 480;
    REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
    FrameDesc frame;
    frame.camera.position = WorldPos{0.0, 40.0, 22.0};
    frame.camera.target = WorldPos{0.0, 0.0, -2.0};
    CaptureChannels channels;
    channels.ids = true;
    CapturedFrame captured;
    REQUIRE_MESSAGE(renderer.capture(frame, channels, captured, &error), error);
    REQUIRE(captured.ids.size() == captured.width * captured.height * k_id_words);
    for (u32 p = 0; p < captured.width * captured.height; ++p) {
      const u32 instance = captured.ids[p * k_id_words];
      if (instance < placed) {
        ++seen[instance];
        ++rock_pixels;
      }
    }
  }
  device.destroy();
  const u32 visible =
      static_cast<u32>(std::count_if(seen.begin(), seen.end(), [](u32 n) { return n > 0; }));
  MESSAGE(visible << " of " << placed << " rocks on the picture, " << rock_pixels << " pixels");
  CHECK(visible * 5 >= placed * 3);
}

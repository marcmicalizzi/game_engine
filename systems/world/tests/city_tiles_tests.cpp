// Island City streamed by the world's placements consumer (placement_tiles.h; city.md, "Where it
// attaches"; scene_gen.md; ADR-0046), compiled where the city capability is and linked with it as a
// host is: a scene names the generator "city" with a plan reference; read whole it is the plan's
// proxies, and streamed the reader leaves the twelve proxy meshes resident and the ring streams
// each held tile's proxies — the per-tile query at the ring's detail — as the scene's tail. No
// device: the scene is read, not loaded, and its parts are laid out by hand, one cluster a mesh.
#include <domain/city/city.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/scene.h>
#include <systems/world/placement_tiles.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <fstream>
#include <string>

using namespace engine;
using namespace engine::world;

namespace {

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

// A small island's plan, written where the scene names it.
const city::Plan& write_plan(const engine::test::TempDir& tmp) {
  static const city::Plan plan = [] {
    city::IslandParams source = city::default_island_params();
    source.seed = 2026;
    source.radius = 700.0f;
    city::Params params;
    std::string error;
    REQUIRE_MESSAGE(city::params_from_schema(source, params, error), error);
    city::Plan made;
    REQUIRE_MESSAGE(city::generate_plan(params, made, error), error);
    return made;
  }();
  std::string error;
  REQUIRE(io::make_directories(tmp.file("island")) == io::Status::Ok);
  REQUIRE_MESSAGE(city::write_plan_file(tmp.file("island/plan.json"), plan, &error), error);
  return plan;
}

sim::ObserverSet at(f32 x, f32 z) {
  sim::ObserverSet set;
  set.add(Vec3{x, 30.0f, z}, 1.0f);
  return set;
}

struct Sink {
  Vector<renderer::SceneInstance> last;
  static bool take(void* c, std::span<const renderer::SceneInstance> tail, std::string*) {
    static_cast<Sink*>(c)->last.assign(tail.begin(), tail.end());
    return true;
  }
};

}  // namespace

TEST_CASE("world city: a scene names the city, and the ring streams its tiles' proxies") {
  const engine::test::TempDir tmp("world_city_tiles");
  const city::Plan& plan = write_plan(tmp);
  const f32 tile_m = static_cast<f32>(plan.params.tile_cm) / 100.0f;
  const std::string entry = R"({"generator":"city","params":{"plan":"island"}})";
  // Read whole: every building at massing, and the streets and parks.
  const std::string whole_path = tmp.file("whole.json");
  REQUIRE(write_text(whole_path, R"({"placements":[)" + entry + "]}"));
  renderer::SceneDesc whole;
  std::string error;
  REQUIRE_MESSAGE(renderer::read_scene_file(whole_path, whole, error), error);
  CHECK(whole.meshes.size() == city::k_proxy_meshes);
  CHECK(whole.placed_buildings > 50);
  CHECK(whole.placed_instances > whole.placed_buildings);

  // Streamed on the plan's own grid: the twelve boxes resident, nothing expanded.
  const std::string streamed_path = tmp.file("streamed.json");
  REQUIRE(write_text(streamed_path, R"({"placements":[)" + entry + R"(],"world":{"tile_size":)" +
                                        std::to_string(tile_m) +
                                        R"(,"rings":[{"radius":1.5},{"radius":4}]}})"));
  renderer::SceneDesc desc;
  REQUIRE_MESSAGE(renderer::read_scene_file(streamed_path, desc, error), error);
  REQUIRE(desc.streamed.size() == 1);
  CHECK(desc.streamed[0].meshes.size() == city::k_proxy_meshes);
  CHECK(desc.instances.empty());
  renderer::SceneData scene;
  scene.parts.resize(desc.meshes.size());
  for (u32 m = 0; m < desc.meshes.size(); ++m) {
    scene.parts[m].first_cluster = m;
    scene.parts[m].cluster_count = 1;
  }
  scene.streamed = desc.streamed;
  scene.world = desc.world;
  scene.dynamic = true;
  PlacementTilesConfig config;
  config.tile_size = tile_m;
  config.ring_count = 2;
  PlacementTiles placements;
  REQUIRE_MESSAGE(placements.create(scene, config, &error), error);
  Sink sink;
  placements.set_sink(&Sink::take, &sink);
  RingParams rings;
  rings.tile_size = tile_m;
  rings.ring_count = 2;
  rings.radius[0] = 1.5f;
  rings.radius[1] = 4.0f;
  rings.max_activations = 0;
  rings.max_deactivations = 0;
  World world(rings);
  world.add_consumer(placements.consumer());
  world.update(at(0.0f, 0.0f), 0);
  CHECK(placements.stats().buildings > 0);
  REQUIRE_FALSE(sink.last.empty());
  // Each held tile is the per-tile query at its ring's detail — rooms in the inner ring, floors in
  // the next — as proxy instances of the resident boxes.
  u32 checked = 0;
  for (u32 i = 0; i < world.ring().active_count(); ++i) {
    const TileCoord tile = tile_of_key(world.ring().active_keys()[i]);
    const u8 ring = world.ring().active_rings()[i];
    Vector<city::Proxy> proxies;
    REQUIRE(city::tile_proxies(plan, city::TileCoord{tile.x, tile.z},
                               ring == 0 ? city::Stage::Rooms : city::Stage::Floors, proxies,
                               &error));
    const std::span<const renderer::SceneInstance> held = placements.tile_instances(tile);
    REQUIRE(held.size() == proxies.size());
    for (u32 k = 0; k < proxies.size(); ++k) {
      CHECK(held[k].mesh == desc.streamed[0].meshes[static_cast<u32>(proxies[k].mesh)]);
      CHECK(held[k].origin == city::proxy_position(proxies[k]));
      CHECK(held[k].transform.position == Vec3{});
      CHECK(held[k].transform.scale == city::proxy_scale(proxies[k]));
    }
    checked += proxies.empty() ? 0u : 1u;
  }
  CHECK(checked > 4);
  // A city on another grid than the world's is refused when the scene is read.
  const std::string wrong_path = tmp.file("wrong.json");
  REQUIRE(write_text(wrong_path, R"({"placements":[)" + entry + R"(],"world":{"tile_size":13}})"));
  renderer::SceneDesc wrong;
  CHECK_FALSE(renderer::read_scene_file(wrong_path, wrong, error));
  CHECK(error.find("must be one grid") != std::string::npos);
}

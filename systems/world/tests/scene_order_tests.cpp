// The scene's generator consumers in their declared order (scene_consumers.h; world.md, "The
// order"; scene_gen.md, "The order rule"; ADR-0046, ADR-0040), with a ground and a placement
// generator of this test's own registered from this file and no capability linked — compiled in
// every configuration that has the world. The ground's tiles and the generator's tiles log what
// they are asked, in the order they are asked: a tile's ground is built before either placement
// entry is asked for the tile, the entries follow in the scene's order — swapping them swaps them
// and not the ground's place — and when the tile goes, its placements are gone before its ground
// is.
#include <core/json/json.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>
#include <systems/world/ground_tiles.h>
#include <systems/world/placement_tiles.h>
#include <systems/world/scene_consumers.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>

#include <memory>
#include <string>

using namespace engine;
using namespace engine::world;

namespace {

// What the test generators were asked, in order; and the placements consumer, so the ground can
// say whether a tile's placements are still there when the ground lets it go.
std::string g_log;
const PlacementTiles* g_placements = nullptr;

std::string coord(scene_gen::TileCoord t) {
  return std::to_string(t.x) + "," + std::to_string(t.z);
}

// A plane at 2 m whose tiles are nothing but a log.
bool tiles_activate(void*, scene_gen::TileCoord t, u8) {
  g_log += "ground " + coord(t) + ";";
  return true;
}
bool tiles_change(void*, scene_gen::TileCoord, u8) { return true; }
void tiles_deactivate(void*, scene_gen::TileCoord t) {
  const bool bare =
      g_placements == nullptr || g_placements->tile_instances(TileCoord{t.x, t.z}).empty();
  g_log += "-ground " + coord(t) + (bare ? " bare;" : " still built on;");
}
void tiles_take_built(void*, Vector<scene_gen::TileCoord>& out) { out.clear(); }
void tiles_set_time(void*, i64) {}
void tiles_destroy(void*) noexcept {}
constexpr scene_gen::GroundTilesOps k_tiles_ops{.destroy = &tiles_destroy,
                                                .activate = &tiles_activate,
                                                .change_ring = &tiles_change,
                                                .deactivate = &tiles_deactivate,
                                                .take_built = &tiles_take_built,
                                                .set_time = &tiles_set_time};

int g_tiles_state = 0;
f32 plane_height(const void*, f32, f32) noexcept { return 2.0f; }
void plane_destroy(void*) noexcept {}
bool plane_open_tiles(const void*, const scene_gen::TileRecords&, i64, scene_gen::GroundTiles& out,
                      std::string*) {
  out = scene_gen::GroundTiles(&k_tiles_ops, &g_tiles_state);
  return true;
}
constexpr scene_gen::GroundOps k_plane_ops{
    .destroy = &plane_destroy, .height = &plane_height, .open_tiles = &plane_open_tiles};
bool plane_make(const scene::Terrain&, const scene_gen::Context&, scene_gen::GroundProvider& out,
                std::string*) {
  out = scene_gen::GroundProvider(&k_plane_ops, nullptr);
  return true;
}
constexpr scene_gen::GroundProviderDesc k_plane{
    .name = "world-test-plane", .make = &plane_make, .flags = scene_gen::k_ground_tiles};
const scene_gen::Registrar k_plane_registrar{k_plane};

// One instance a tile, on the ground's floor at the tile's centre, tagged by the entry's name.
struct Row {
  std::string name;
};
bool row_open(const JsonValue& params, const scene_gen::Context&, void** state, std::string*) {
  auto* row = new Row;
  std::string_view name;
  const JsonValue* value = params.find("name");
  if (value != nullptr) value->get_string(name);
  row->name = std::string(name);
  *state = row;
  return true;
}
void row_close(void* state) noexcept { delete static_cast<Row*>(state); }
bool row_expand(void*, const scene_gen::Context&, scene_gen::Placements&, std::string*) {
  return true;
}
bool row_tile(void* state, scene_gen::TileCoord t, const scene_gen::Context& context,
              scene_gen::Placements& out, std::string*) {
  const Row& row = *static_cast<const Row*>(state);
  g_log += row.name + " " + coord(t) + ";";
  scene_gen::Placement p;
  p.mesh = 0;
  const f32 x = (static_cast<f32>(t.x) + 0.5f) * context.tile_size;
  const f32 z = (static_cast<f32>(t.z) + 0.5f) * context.tile_size;
  p.transform.position = Vec3{x, context.ground.floor(x, z), z};
  out.instances.push_back(p);
  out.things = 1;
  return true;
}
constexpr scene_gen::PlacementGeneratorDesc k_row{.name = "world-test-row",
                                                  .open = &row_open,
                                                  .close = &row_close,
                                                  .expand = &row_expand,
                                                  .tile = &row_tile};
const scene_gen::Registrar k_row_registrar{k_row};

JsonValue params_named(const char* name) {
  JsonValue v;
  REQUIRE(parse_json(std::string(R"({"name":")") + name + "\"}", v).ok);
  return v;
}

// A streamed scene standing on the test plane, with two row entries in the order given: one mesh
// each, one cluster a mesh.
renderer::SceneData scene_of(const char* first, const char* second) {
  renderer::SceneData scene;
  scene.terrain.enabled = true;
  scene.terrain.provider = "world-test-plane";
  scene.parts.resize(2);
  for (u32 m = 0; m < 2; ++m) {
    scene.parts[m].first_cluster = m;
    scene.parts[m].cluster_count = 1;
  }
  u32 mesh = 0;
  for (const char* name : {first, second}) {
    renderer::StreamedPlacements entry;
    entry.generator = "world-test-row";
    entry.params = params_named(name);
    entry.where = std::string("placements ") + std::to_string(mesh);
    entry.meshes.push_back(mesh++);
    scene.streamed.push_back(std::move(entry));
  }
  scene.world.tile_size = 32.0f;
  scene.dynamic = true;
  return scene;
}

// One tile's column: the ring's inner radius is half a tile, so an observer at a tile's centre
// holds that tile alone.
RingParams one_tile() {
  RingParams params;
  params.ring_count = 1;
  params.radius[0] = 0.5f;
  params.max_activations = 0;
  params.max_deactivations = 0;
  return params;
}

sim::ObserverSet at(f32 x, f32 z) {
  sim::ObserverSet set;
  set.add(Vec3{x, 0.0f, z}, 1.0f);
  return set;
}

// The log of one tile streamed in and then let go, for a scene with its entries in this order.
std::string stream_one_tile(const char* first, const char* second) {
  const renderer::SceneData scene = scene_of(first, second);
  renderer::TerrainSampler sampler(scene.terrain);
  REQUIRE_MESSAGE(sampler.ok(), sampler.error());
  GroundTiles ground;
  std::string error;
  REQUIRE_MESSAGE(ground.create(sampler.provider(), GroundTilesConfig{}, &error), error);
  PlacementTiles placements;
  PlacementTilesConfig config;
  config.ring_count = 1;
  REQUIRE_MESSAGE(placements.create(scene, config, &error), error);
  World world(one_tile());
  add_scene_consumers(world, &ground, &placements);
  REQUIRE(world.consumer_count() == 2);
  CHECK(std::string(world.consumer(0).name) == "ground");
  CHECK(std::string(world.consumer(1).name) == "placements");
  g_placements = &placements;
  g_log.clear();
  world.update(at(48.0f, -16.0f), 0);  // tile (1, -1)
  // Both entries' instances, standing on the plane.
  const std::span<const renderer::SceneInstance> held = placements.tile_instances(TileCoord{1, -1});
  REQUIRE(held.size() == 2);
  CHECK(held[0].transform.position.y == 2.0f);
  world.clear(1);
  g_placements = nullptr;
  return g_log;
}

}  // namespace

TEST_CASE("world order: a tile's ground comes first, then the placements in the scene's order") {
  CHECK(stream_one_tile("a", "b") == "ground 1,-1;a 1,-1;b 1,-1;-ground 1,-1 bare;");
  // Swapped in the scene: the entries swap, and the ground is still built before either and let go
  // after both.
  CHECK(stream_one_tile("b", "a") == "ground 1,-1;b 1,-1;a 1,-1;-ground 1,-1 bare;");
}

TEST_CASE("world order: a scene with no ground tiles or no placements registers what it has") {
  const renderer::SceneData scene = scene_of("a", "b");
  PlacementTiles placements;
  PlacementTilesConfig config;
  config.ring_count = 1;
  std::string error;
  REQUIRE_MESSAGE(placements.create(scene, config, &error), error);
  World placements_only(one_tile());
  add_scene_consumers(placements_only, nullptr, &placements);
  REQUIRE(placements_only.consumer_count() == 1);
  CHECK(std::string(placements_only.consumer(0).name) == "placements");
  // A ground consumer whose ground has no tiles is not registered.
  GroundTiles none;
  World empty(one_tile());
  add_scene_consumers(empty, &none, nullptr);
  CHECK(empty.consumer_count() == 0);
}

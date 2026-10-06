// Island City as the placement generator "city" (scene_generator.h; docs/subsystems/city.md, "Where
// it attaches"; scene_gen.md; ADR-0046), through the registry and with no scene reader: a
// district's expansion is the proxy fragment `engine-content city fragment --district` writes,
// instance for instance; a streamed tile is the per-tile query at the ring's detail (the plan's own
// tests hold every tile together to the whole city), each building counted by the one tile that
// owns its lot; the twelve proxy meshes are written beside the plan; and an entry on another grid
// than a streamed world's, or naming both a tile and a district, is refused.
#include "city_fixtures.h"

#include <core/json/json.h>
#include <domain/city/city.h>
#include <domain/city/scene_generator.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <schemas/scene.h>
#include <string>

using namespace engine;
using namespace engine::city;
using engine::city::test::small_plan;

namespace {

JsonValue entry_json(const std::string& text) {
  JsonValue v;
  REQUIRE(parse_json(text, v).ok);
  return v;
}

struct Opened {
  const scene_gen::PlacementGeneratorDesc* generator = nullptr;
  void* state = nullptr;
  ~Opened() {
    if (generator != nullptr && state != nullptr) generator->close(state);
  }
};

// The small island's plan, written where a scene would name it.
std::string write_plan(const engine::test::TempDir& tmp) {
  const std::string path = tmp.file("island/plan.json");
  std::string error;
  REQUIRE(io::make_directories(tmp.file("island")) == io::Status::Ok);
  REQUIRE_MESSAGE(write_plan_file(path, small_plan(), &error), error);
  return path;
}

// The fragment writes the scene file's float32 `translation` (`proxy_translation`, an ADR-0053 seam
// until the file's field is a `worldpos`) and the generator the f64 place (`proxy_position`): the
// same centimetres, so the two agree to a float32's rounding of the place (a part in 10^7 of it).
bool same_placement(const scene_gen::Placement& p, const scene::Instance& i) {
  const DVec3 off =
      p.position - WorldPos{static_cast<f64>(i.translation.x), static_cast<f64>(i.translation.y),
                            static_cast<f64>(i.translation.z)};
  return p.mesh == i.mesh && length(off) <= 1.0e-6 * (1.0 + length(p.position - WorldPos{})) &&
         i.scale.has_value() && p.scale == *i.scale && p.rotation == Quat::identity();
}

}  // namespace

TEST_CASE("city generator: a district's expansion is the district's proxy fragment") {
  const engine::test::TempDir tmp("city_scene_generator");
  write_plan(tmp);
  const scene_gen::PlacementGeneratorDesc* generator =
      scene_gen::GeneratorRegistry::global().find_placement(k_placement_generator);
  REQUIRE(generator != nullptr);
  scene_gen::Context context;
  context.dir = tmp.path();
  context.where = "test: placements 0";
  Opened opened{generator, nullptr};
  std::string error;
  REQUIRE_MESSAGE(generator->open(entry_json(R"({"plan":"island","district":1})"), context,
                                  &opened.state, &error),
                  error);
  scene_gen::Placements placed;
  REQUIRE_MESSAGE(generator->expand(opened.state, context, placed, &error), error);
  // The twelve boxes, written beside the plan.
  REQUIRE(placed.meshes.size() == k_proxy_meshes);
  for (u32 m = 0; m < k_proxy_meshes; ++m) {
    CHECK(placed.meshes[m].name ==
          std::string("city/") + proxy_mesh_name(static_cast<ProxyMesh>(m)));
    CHECK(io::exists(placed.meshes[m].path));
  }
  // The fragment the command writes for the same district, made the command's way.
  const Plan& plan = small_plan();
  Vector<u32> lots;
  for (u32 l = 0; l < plan.lots.size(); ++l) {
    if (plan.lots[l].district == 1 && plan.lots[l].use != LotUse::Park) lots.push_back(l);
  }
  CHECK(placed.things == lots.size());
  Vector<Building> buildings;
  REQUIRE(generate_buildings(plan, std::span<const u32>(lots.data(), lots.size()), Stage::Floors,
                             nullptr, buildings, &error));
  Vector<Proxy> proxies;
  for (const Building& b : buildings)
    append_building_proxies(plan, b, Stage::Floors, proxies);
  append_district_ground_proxies(plan, 1, proxies);
  scene::Scene fragment;
  make_fragment(proxies, tmp.file("island/proxy"), tmp.path(), "district 1", fragment);
  REQUIRE(placed.instances.size() == fragment.instances.size());
  u32 different = 0;
  for (u32 i = 0; i < placed.instances.size(); ++i)
    different += same_placement(placed.instances[i], fragment.instances[i]) ? 0u : 1u;
  CHECK(different == 0);
  CHECK(placed.instances.size() > 100);
}

TEST_CASE("city generator: a streamed city is the per-tile query, and its tiles are the whole") {
  const engine::test::TempDir tmp("city_scene_generator_tiles");
  write_plan(tmp);
  const Plan& plan = small_plan();
  const scene_gen::PlacementGeneratorDesc* generator =
      scene_gen::GeneratorRegistry::global().find_placement("city");
  REQUIRE(generator != nullptr);
  // One ring, so every tile is drawn at the innermost ring's detail (rooms), on the plan's grid.
  scene::WorldRings world;
  world.tile_size = static_cast<f32>(plan.params.tile_cm) / 100.0f;
  world.rings.push_back(scene::WorldRing{});
  world.rings[0].radius = 4.0f;
  scene_gen::Context context;
  context.dir = tmp.path();
  context.where = "test: placements 0";
  context.tile_size = world.tile_size;
  context.world = &world;
  context.ring = 0;
  Opened opened{generator, nullptr};
  std::string error;
  REQUIRE_MESSAGE(
      generator->open(entry_json(R"({"plan":"island/plan.json"})"), context, &opened.state, &error),
      error);
  Vector<scene_gen::PlacementMesh> resident;
  REQUIRE(generator->meshes(opened.state, context, resident, &error));
  CHECK(resident.size() == k_proxy_meshes);
  // Every tile the plan's lots touch: the per-tile query, placed.
  i32 lo_x = 1 << 30, lo_z = 1 << 30, hi_x = -(1 << 30), hi_z = -(1 << 30);
  for (const Lot& lot : plan.lots) {
    const TileCoord a = tile_of(plan, lot.rect.x0, lot.rect.z0);
    const TileCoord b = tile_of(plan, lot.rect.x1, lot.rect.z1);
    lo_x = std::min(lo_x, a.x);
    lo_z = std::min(lo_z, a.z);
    hi_x = std::max(hi_x, b.x);
    hi_z = std::max(hi_z, b.z);
  }
  u32 tiles = 0;
  u64 placements = 0;
  u32 buildings = 0;
  u32 different = 0;
  for (i32 z = lo_z; z <= hi_z; ++z) {
    for (i32 x = lo_x; x <= hi_x; ++x) {
      scene_gen::Placements one;
      REQUIRE(generator->tile(opened.state, scene_gen::TileCoord{x, z}, context, one, &error));
      Vector<Proxy> proxies;
      REQUIRE(tile_proxies(plan, TileCoord{x, z}, Stage::Rooms, proxies, &error));
      REQUIRE(one.instances.size() == proxies.size());
      for (u32 i = 0; i < proxies.size(); ++i) {
        different += one.instances[i].mesh == static_cast<u32>(proxies[i].mesh) &&
                             one.instances[i].position == proxy_position(proxies[i]) &&
                             one.instances[i].scale == proxy_scale(proxies[i])
                         ? 0u
                         : 1u;
      }
      tiles += one.instances.empty() ? 0u : 1u;
      placements += one.instances.size();
      buildings += one.things;
    }
  }
  CHECK(different == 0);
  CHECK(tiles > 4);
  // Each building counted once, by the tile that owns its lot.
  u32 lots = 0;
  for (const Lot& lot : plan.lots)
    lots += lot.use != LotUse::Park ? 1u : 0u;
  CHECK(buildings == lots);
  MESSAGE(placements << " proxies over " << tiles << " tiles, " << buildings << " buildings");
  // A tile far from the island holds nothing.
  CHECK_FALSE(generator->occupies(opened.state, scene_gen::TileCoord{hi_x + 1000, hi_z + 1000}));
}

TEST_CASE("city generator: an entry on another grid, or with a tile and a district, is refused") {
  const engine::test::TempDir tmp("city_scene_generator_refuse");
  write_plan(tmp);
  const scene_gen::PlacementGeneratorDesc* generator =
      scene_gen::GeneratorRegistry::global().find_placement("city");
  REQUIRE(generator != nullptr);
  scene_gen::Context context;
  context.dir = tmp.path();
  context.where = "scene.json: placements 3";
  void* state = nullptr;
  std::string error;
  CHECK_FALSE(generator->open(entry_json(R"({"district":1})"), context, &state, &error));
  CHECK(error == "scene.json: placements 3 names no plan");
  CHECK_FALSE(generator->open(entry_json(R"({"plan":"island","tile":[0,0],"district":1})"), context,
                              &state, &error));
  CHECK(error == "scene.json: placements 3 names both a tile and a district; name one");
  CHECK_FALSE(
      generator->open(entry_json(R"({"plan":"island","district":999})"), context, &state, &error));
  CHECK(error.find("no district 999") != std::string::npos);
  scene::WorldRings world;
  context.world = &world;
  context.tile_size = 13.0f;
  CHECK_FALSE(generator->open(entry_json(R"({"plan":"island"})"), context, &state, &error));
  CHECK(error.find("must be one grid") != std::string::npos);
}

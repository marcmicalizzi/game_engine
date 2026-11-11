// The placement generator through the registry: tiles agree with the whole read, in any order,
// and a far ring draws the strips.

#include "walls_test_support.h"

#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstdio>

using namespace engine;
using namespace engine::walls;

namespace {

struct Opened {
  const scene_gen::PlacementGeneratorDesc* desc = nullptr;
  void* state = nullptr;
  ~Opened() {
    if (state != nullptr) desc->close(state);
  }
};

bool same(const scene_gen::Placement& a, const scene_gen::Placement& b) {
  return walls::test::placement_hash(a) == walls::test::placement_hash(b);
}

}  // namespace

TEST_CASE("walls: the generator's tiles are its whole read, in any order") {
  engine::test::TempDir dir("walls");
  const Layer layer = walls::test::field_layer(-20, 10, 0.25f);
  const walls::test::TestPlane TestPlane{0.12, -0.08, 0.6};
  scene_gen::Context context;
  context.tile_size = 32.0f;
  context.ddc = dir.path();
  context.where = "test: walls 0";
  context.ground = walls::test::ground_of(TestPlane);
  Opened g;
  g.desc = scene_gen::GeneratorRegistry::global().find_placement("walls");
  REQUIRE(g.desc != nullptr);
  std::string error;
  REQUIRE(g.desc->open(schema::to_json(layer), context, &g.state, &error));
  Vector<scene_gen::PlacementMesh> meshes;
  REQUIRE(g.desc->meshes(g.state, context, meshes, &error));
  CHECK(meshes.size() == kit_size(layer.variants));
  for (const auto& m : meshes)
    CHECK(io::exists(m.path));
  scene_gen::Placements whole;
  REQUIRE(g.desc->expand(g.state, context, whole, &error));
  REQUIRE(whole.instances.size() > 50);
  // Every tile the walls touch, forwards and backwards; each placement found once, equal to the
  // whole read's by id.
  Vector<scene_gen::Placement> seen;
  seen.resize(whole.instances.size() + 1);
  Vector<u8> count;
  count.resize(whole.instances.size() + 1, 0);
  for (const bool backwards : {false, true}) {
    for (u32 n = 0; n < 16 * 16; ++n) {
      const u32 k = backwards ? 16 * 16 - 1 - n : n;
      const scene_gen::TileCoord t{static_cast<i32>(k % 16) - 8, static_cast<i32>(k / 16) - 8};
      scene_gen::Placements out;
      REQUIRE(g.desc->tile(g.state, t, context, out, &error));
      CHECK(out.instances.empty() == !g.desc->occupies(g.state, t));
      for (const auto& p : out.instances) {
        REQUIRE(p.id >= 1);
        REQUIRE(p.id <= whole.instances.size());
        if (backwards) {
          CHECK(same(seen[p.id], p));
        } else {
          seen[p.id] = p;
          ++count[p.id];
        }
      }
    }
  }
  for (u32 i = 0; i < whole.instances.size(); ++i) {
    const auto& p = whole.instances[i];
    CHECK(count[p.id] == 1);
    CHECK(same(seen[p.id], p));
  }
  // The far ring: strips, a strip over up to four modules.
  scene::WorldRings rings;
  scene_gen::Context far = context;
  far.world = &rings;
  far.ring = static_cast<u8>(layer.far_ring);
  CHECK(g.desc->representation(g.state, far) == 1);
  u32 strips = 0;
  for (u32 k = 0; k < 16 * 16; ++k) {
    const scene_gen::TileCoord t{static_cast<i32>(k % 16) - 8, static_cast<i32>(k / 16) - 8};
    scene_gen::Placements out;
    REQUIRE(g.desc->tile(g.state, t, far, out, &error));
    for (const auto& p : out.instances) {
      CHECK(p.mesh == kit_index(ModuleKind::strip, 0, 0, layer.variants));
      ++strips;
    }
  }
  CHECK(strips * 2 < whole.instances.size());
  CHECK(strips > 0);
}

TEST_CASE("walls: the generator refuses what it cannot build") {
  engine::test::TempDir dir("walls");
  scene_gen::Context context;
  context.ddc = dir.path();
  const auto* desc = scene_gen::GeneratorRegistry::global().find_placement("walls");
  REQUIRE(desc != nullptr);
  std::string error;
  void* state = nullptr;
  Layer layer = walls::test::field_layer();
  layer.lines[0].gateways_cm = {1};  // a gateway hanging off the line's start
  CHECK_FALSE(desc->open(schema::to_json(layer), context, &state, &error));
  CHECK(error.find("gateway") != std::string::npos);
  layer = walls::test::field_layer();
  layer.ruined = 0.95f;
  CHECK_FALSE(desc->open(schema::to_json(layer), context, &state, &error));
  JsonValue bad;
  REQUIRE(parse_json(R"({"lines": [], "colour": 3})", bad).ok);
  CHECK_FALSE(desc->open(bad, context, &state, &error));
  CHECK(state == nullptr);
}

TEST_CASE("walls: a hundred metres of wall is tens of instances, at every form") {
  // The want (roadmap G145): the proof of concept laid about 1,900 stone instances a hundred
  // metres. A straight 100 m wall on a 1 in 8 slope: its modules near, its strips far, and the
  // triangles each form of the kit carries.
  Layer layer;
  WallLine line;
  line.points_mm = {0, 0, 100000, 0};
  layer.lines.push_back(line);
  Plan plan;
  std::string why;
  REQUIRE(make_plan(layer, 32000, plan, why));
  Vector<Module> kit;
  kit_recipes(layer, kit);
  const walls::test::TestPlane plane{0.125, 0.0, 0.0};
  const scene_gen::Ground ground = walls::test::ground_of(plane);
  u64 near = 0, mid = 0, far = 0, pieces = 0;
  Vector<u32> near_of, mid_of, far_of;
  near_of.resize(kit.size(), 0);
  mid_of.resize(kit.size(), 0);
  far_of.resize(kit.size(), 0);
  for (const Slot& s : plan.slots) {
    const u32 k = stand_slot(plan, layer, s, ground).placement.mesh;
    if (near_of[k] == 0) {
      assets::MeshData mesh;
      ModuleReport report;
      REQUIRE(solve_module(kit[k], mesh, report, why));
      near_of[k] = report.near_triangles;
      mid_of[k] = report.mid_triangles;
      far_of[k] = report.far_triangles;
    }
    near += near_of[k];
    mid += mid_of[k];
    far += far_of[k];
  }
  {
    assets::MeshData mesh;
    ModuleReport report;
    REQUIRE(
        solve_module(kit[kit_index(ModuleKind::strip, 0, 0, layer.variants)], mesh, report, why));
    pieces = plan.pieces.size();
    std::printf(
        "walls 100 m: %u modules (near %llu, mid %llu, outline %llu triangles); %llu strips "
        "of %u triangles\n",
        plan.slots.size(), static_cast<unsigned long long>(near),
        static_cast<unsigned long long>(mid), static_cast<unsigned long long>(far),
        static_cast<unsigned long long>(pieces), report.near_triangles);
  }
  CHECK(plan.slots.size() <= 40);
  CHECK(pieces <= 10);
}

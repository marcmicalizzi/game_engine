#include "rock_checks.h"

#include <core/schema/json_reflect.h>
#include <domain/assets/mesh_generator.h>
#include <domain/rocks/deposits.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace engine;
using namespace engine::rocks;
namespace {
f64 cliff(const void* data, i64 x, i64 z) noexcept {
  const i64 offset = data ? *static_cast<const i64*>(data) : 0;
  const f64 local = static_cast<f64>(x - offset) / 1000.0;
  (void)z;
  return local < 16 ? 0 : (local > 24 ? 16 : (local - 16) * 2);
}
f64 flat(const void*, i64, i64) noexcept { return 0; }
f64 channel(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  return .3 * std::abs(xx - 16) + .06 * (32 - zz);
}
f64 invalid(const void*, i64, i64) noexcept { return std::numeric_limits<f64>::quiet_NaN(); }
DepositLayer layer() {
  DepositLayer p;
  p.region_max = {32, 32};
  p.patch.fragments = 8;
  p.patch.triangle_budget = 10000;
  return p;
}
scene_gen::Ground ground(scene_gen::HeightFn fn, const void* context = nullptr) {
  scene_gen::Ground g;
  g.surface_fn = fn;
  g.context = context;
  return g;
}
DepositPatch patch() {
  DepositPatch p;
  p.width = 5;
  p.depth = 5;
  p.fragments = 12;
  p.triangle_budget = 6000;
  for (u32 z = 0; z < 5; ++z)
    for (u32 x = 0; x < 5; ++x) {
      p.ground.push_back(static_cast<f32>(x) * .125f + static_cast<f32>(z) * .0625f);
      p.thickness.push_back(.125f);
      p.coarse.push_back(static_cast<f32>(x) * .25f);
    }
  return p;
}
struct Opened {
  const scene_gen::PlacementGeneratorDesc* desc =
      scene_gen::GeneratorRegistry::global().find_placement("deposits");
  void* state = nullptr;
  ~Opened() {
    if (state) desc->close(state);
  }
};
}  // namespace
TEST_CASE("deposits: connected foot talus with a finite runout and no uphill blanket") {
  DepositLayer p = layer();
  p.sediment_depth_m = 0;
  DepositPlan plan;
  std::string error;
  REQUIRE_MESSAGE(plan_deposits(p, ground(cliff), plan, error), error);
  const u32 row = 16 * plan.width;
  CHECK(plan.thickness[row + 15] > 0);
  CHECK(plan.thickness[row + 12] > 0);
  CHECK(plan.thickness[row + 26] == 0);
  CHECK(plan.thickness[row] == 0);
  bool found = false, ended = false;
  for (i32 x = 15; x >= 0; --x) {
    if (plan.thickness[row + static_cast<u32>(x)] > 0) {
      CHECK_FALSE(ended);
      found = true;
    } else if (found)
      ended = true;
  }
  CHECK(found);
  CHECK(ended);
  CHECK(plan.coarse[row + 12] > plan.coarse[row + 15]);
  // Francou/Mante's 34-36 degree modal band: the default envelope is 35 degrees.
  CHECK(static_cast<f64>(plan.thickness[row + 13] - plan.thickness[row + 12]) ==
        doctest::Approx(0.700207538).epsilon(1e-6));
  p.runout_m = 3;
  REQUIRE(plan_deposits(p, ground(cliff), plan, error));
  CHECK(plan.thickness[row + 10] == 0);
  REQUIRE(plan_deposits(p, ground(flat), plan, error));
  for (f32 v : plan.thickness)
    CHECK(v == 0);
}
TEST_CASE("deposits: channel sediment follows contributing area without needing a cliff") {
  auto p = layer();
  p.talus_depth_m = 0;
  DepositPlan plan;
  std::string error;
  REQUIRE_MESSAGE(plan_deposits(p, ground(channel), plan, error), error);
  const u32 center = 24 * plan.width + 16, bank = 24 * plan.width + 2;
  CHECK(plan.drainage_m2[center] > plan.drainage_m2[bank]);
  CHECK(plan.thickness[center] > 0);
  CHECK(plan.thickness[bank] == 0);
  CHECK(plan.coarse[center] == 0);
  p.sediment_depth_m = 0;
  REQUIRE(plan_deposits(p, ground(channel), plan, error));
  for (f32 v : plan.thickness)
    CHECK(v == 0);
}
TEST_CASE("deposits: finite world millimetres preserve translated terrain decisions") {
  const auto p = layer();
  DepositPlan near, far;
  std::string error;
  REQUIRE(plan_deposits(p, ground(cliff), near, error));
  for (i64 mm :
       {-99999000000ll, -10000000000ll, -419072000ll, 419072000ll, 10000000000ll, 99999000000ll}) {
    auto q = p;
    q.region_min[0] += static_cast<f64>(mm) / 1000.0;
    q.region_max[0] += static_cast<f64>(mm) / 1000.0;
    REQUIRE(plan_deposits(q, ground(cliff, &mm), far, error));
    REQUIRE(far.thickness.size() == near.thickness.size());
    for (u32 i = 0; i < near.thickness.size(); ++i) {
      CHECK(far.thickness[i] == near.thickness[i]);
      CHECK(far.coarse[i] == near.coarse[i]);
      CHECK(far.drainage_m2[i] == near.drainage_m2[i]);
    }
  }
}
TEST_CASE("deposits: external input and explicit budgets fail without partial output") {
  auto p = layer();
  DepositPlan plan;
  std::string error;
  CHECK_FALSE(plan_deposits(p, {}, plan, error));
  CHECK_FALSE(plan_deposits(p, ground(invalid), plan, error));
  CHECK(plan.ground.empty());
  auto decimal = layer();
  decimal.spacing_m = .3f;
  decimal.region_max = {30, 30};
  REQUIRE(plan_deposits(decimal, ground(cliff), plan, error));
  CHECK(plan.spacing_mm == 300);
  p.max_samples = 4;
  CHECK_FALSE(plan_deposits(p, ground(cliff), plan, error));
  p = layer();
  p.spacing_m = 0;
  CHECK_FALSE(plan_deposits(p, ground(cliff), plan, error));
  p = layer();
  p.region_max[0] = std::numeric_limits<f64>::infinity();
  CHECK_FALSE(plan_deposits(p, ground(cliff), plan, error));
  auto q = patch();
  assets::MeshData mesh;
  q.thickness[0] = -1;
  CHECK_FALSE(solve_deposit(q, mesh, error));
  CHECK(mesh.positions.empty());
  q = patch();
  q.triangle_budget = 500;
  CHECK_FALSE(solve_deposit(q, mesh, error));
  q = patch();
  q.ground.pop_back();
  CHECK_FALSE(solve_deposit(q, mesh, error));
  q = patch();
  q.material.surface_maps = RockSurfaceMaps{};
  CHECK_FALSE(solve_deposit(q, mesh, error));
}
TEST_CASE("deposits: continuous bed and angular fragments hold topology and the budget ladder") {
  auto p = patch();
  assets::MeshData mesh, again;
  std::string error;
  for (u32 budget : {3000u, 6000u, 24000u}) {
    p.triangle_budget = budget;
    REQUIRE_MESSAGE(solve_deposit(p, mesh, error), error);
    CHECK(mesh.indices.size() / 3 <= budget);
    checks::check_closed(mesh, 1 + p.fragments);
    REQUIRE(solve_deposit(p, again, error));
    CHECK(checks::mesh_hash(mesh) == checks::mesh_hash(again));
  }
  p = patch();
  p.fragments = 0;
  REQUIRE(solve_deposit(p, mesh, error));
  // Every top vertex follows its sampled support and depth; underside is buried.
  for (const auto& v : mesh.positions) {
    const f32 terrain = v.x * .125f + v.z * .0625f;
    CHECK((std::abs(v.y - (terrain + .124f)) < 1e-6f || std::abs(v.y - (terrain - .05f)) < 1e-6f));
  }
}
TEST_CASE("deposits: isolated wet samples form closed beds at diagonal contacts") {
  auto p = patch();
  p.fragments = 0;
  std::fill(p.thickness.begin(), p.thickness.end(), 0.0f);
  p.thickness[0] = .1f;
  p.thickness[12] = .1f;
  assets::MeshData mesh;
  std::string error;
  REQUIRE(solve_deposit(p, mesh, error));
  checks::check_closed(mesh, 1);
}
TEST_CASE("deposits: mesh identity and cluster container are deterministic") {
  const auto p = patch();
  std::string error;
  assets::MeshData mesh;
  REQUIRE(solve_deposit(p, mesh, error));
  const u64 hash = checks::mesh_hash(mesh);
  MESSAGE("deposit mesh pin: ", hash);
  // G128 initial MSVC pin; the existing rock solver pins remain unchanged.
  CHECK(hash == 6863964757150498499ull);
  geometry::ClusterFileData a, b;
  REQUIRE_MESSAGE(build_deposit_container(p, {}, 0, a, error), error);
  REQUIRE(build_deposit_container(p, {}, 0, b, error));
  MESSAGE("deposit container pin: ", geometry::cluster_file_hash(a));
  CHECK(geometry::cluster_file_hash(a) == 16297439944098904797ull);
  CHECK(geometry::cluster_file_hash(a) == geometry::cluster_file_hash(b));
  CHECK(a.source_hash == deposit_source_hash(p));
  CHECK(a.source_hash == b.source_hash);
  CHECK(a.generator == "deposit");
  CHECK_FALSE(a.mesh.mesh.clusters.empty());
  auto changed = p;
  changed.ground[0] += .01f;
  CHECK(deposit_source_hash(changed) != a.source_hash);
  changed = p;
  ++changed.seed;
  CHECK(deposit_source_hash(changed) != a.source_hash);
}
TEST_CASE("deposits: whole expansion equals reversed tile arrival with stable ids and exclusion") {
  test::TempDir scratch;
  scene_gen::Context c;
  const std::string dir = scratch.path();
  c.dir = dir;
  c.ddc = dir;
  c.ground = ground(cliff);
  c.tile_size = 16;
  Opened opened;
  REQUIRE(opened.desc != nullptr);
  std::string error;
  const auto p = layer();
  REQUIRE_MESSAGE(opened.desc->open(schema::to_json(p), c, &opened.state, &error), error);
  scene_gen::Placements whole, tiled;
  REQUIRE(opened.desc->expand(opened.state, c, whole, &error));
  REQUIRE_FALSE(whole.instances.empty());
  Vector<scene_gen::PlacementMesh> meshes;
  REQUIRE(opened.desc->meshes(opened.state, c, meshes, &error));
  CHECK(meshes.size() == whole.meshes.size());
  for (i32 z = 1; z >= 0; --z)
    for (i32 x = 1; x >= 0; --x)
      REQUIRE(opened.desc->tile(opened.state, {x, z}, c, tiled, &error));
  std::sort(tiled.instances.begin(), tiled.instances.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
  REQUIRE(tiled.instances.size() == whole.instances.size());
  for (u32 i = 0; i < whole.instances.size(); ++i) {
    CHECK(whole.instances[i].id != 0);
    CHECK(tiled.instances[i].id == whole.instances[i].id);
    CHECK(tiled.instances[i].mesh == whole.instances[i].mesh);
    CHECK(tiled.instances[i].position.x == whole.instances[i].position.x);
    CHECK(tiled.instances[i].position.y == whole.instances[i].position.y);
    CHECK(tiled.instances[i].position.z == whole.instances[i].position.z);
    CHECK(io::exists(whole.meshes[whole.instances[i].mesh].path));
  }
  const auto at = whole.instances[0].position;
  const scene_gen::Footprint footprint{scene_gen::nearest_mm(at.x + 1),
                                       scene_gen::nearest_mm(at.z + 1), .1};
  c.earlier = {&footprint, 1};
  scene_gen::Placements cleared;
  REQUIRE(opened.desc->expand(opened.state, c, cleared, &error));
  CHECK(cleared.instances.size() < whole.instances.size());
  for (const auto& instance : cleared.instances)
    CHECK(instance.id != whole.instances[0].id);
  c.stand = ground(flat);
  scene_gen::Placements moving_output;
  CHECK_FALSE(opened.desc->expand(opened.state, c, moving_output, &error));
  CHECK_FALSE(opened.desc->tile(opened.state, {0, 0}, c, moving_output, &error));
  Opened moving;
  CHECK_FALSE(moving.desc->open(schema::to_json(p), c, &moving.state, &error));
}

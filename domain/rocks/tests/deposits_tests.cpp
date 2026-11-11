#include "rock_checks.h"

#include <core/schema/json_reflect.h>
#include <domain/assets/mesh_generator.h>
#include <domain/geometry/material_sampling.h>
#include <domain/materials/materials.h>
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
// T108: a patch's bed and its loose stones are two primitives and materials; the closed-mesh checks
// read the whole mesh as one body list.
assets::MeshData as_one(const assets::MeshData& mesh) {
  assets::MeshData one = mesh;
  one.primitives.resize(1);
  one.primitives[0].first_index = 0;
  one.primitives[0].index_count = static_cast<u32>(one.indices.size());
  one.primitives[0].material = 0;
  one.materials.resize(1);
  return one;
}
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
  // An alpine wall's supply (G143's third pass moved the default to a temperate wall's 1.2 m, whose
  // lower talus puts these samples on its concave toe): the upper slope is the repose angle.
  p.supply_m = 3.5f;
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
  p.supply_m = 0;
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
      CHECK(far.drainage_m2[i] == near.drainage_m2[i]);
      // G143: the outline's ring is moved by noise anchored to the world, so a translated window
      // is another place there; inside the ring every decision is the same to the bit.
      if (near.thickness[i] < .5f) continue;
      CHECK(far.thickness[i] == near.thickness[i]);
      CHECK(far.coarse[i] == near.coarse[i]);
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
    checks::check_closed(as_one(mesh), 1 + p.fragments);
    REQUIRE(solve_deposit(p, again, error));
    CHECK(checks::mesh_hash(mesh) == checks::mesh_hash(again));
  }
  p = patch();
  p.fragments = 0;
  // Channel sediment (coarse share zero) has no clasts the relief draws (G143's third pass; the
  // relief's own case is in deposits_cone_tests.cpp), so the bed is its support plus its depth.
  std::fill(p.coarse.begin(), p.coarse.end(), 0.0f);
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
  checks::check_closed(as_one(mesh), 1);
}
TEST_CASE("deposits: mesh identity and cluster container are deterministic") {
  const auto p = patch();
  std::string error;
  assets::MeshData mesh;
  REQUIRE(solve_deposit(p, mesh, error));
  const u64 hash = checks::mesh_hash(mesh);
  MESSAGE("deposit mesh pin: ", hash);
  // G128 initial MSVC pin; the existing rock solver pins remain unchanged.
  // Re-taken for G143's normals from the top's gradient (2026-11-10; was 6863964757150498499), then
  // for fragments built at the rung their size needs (was 625669387871748774), then for G143's
  // third pass: the top drawn at 12.5 cm and raised by its clasts, the walls' own triangles, the
  // loose fragments of the largest band at 4 cm facets (was 2024239262398034445).
  CHECK(hash == 1738319574425829183ull);
  geometry::ClusterFileData a, b;
  REQUIRE_MESSAGE(build_deposit_container(p, {}, 0, a, error), error);
  REQUIRE(build_deposit_container(p, {}, 0, b, error));
  MESSAGE("deposit container pin: ", geometry::cluster_file_hash(a));
  // Rock solver v11 changes the source/build identity, not the deposit mesh above; so does
  // k_deposit_version 2 (G143, the bare rim's sink: was 17513793063602312493).
  // k_deposit_version 3 and the normals (G143, stage 2; was 14163044981722319397), then the
  // fragments' rungs (stage 3; was 13439161428700819251), then for the patch's border locked in
  // its cluster DAG (G143's third pass; was 7929702402516820737), then for the rubble and
  // k_deposit_version 4 (was 1421504051895875902), then 5 for the toe's clasts kept off the face
  // (was 13829166473420471729), then for T108's two primitives, the bed and the loose stones,
  // each its own DAG and material, and k_deposit_version 6 (was 10478160828047604680).
  CHECK(geometry::cluster_file_hash(a) == 16110835894214829114ull);
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
TEST_CASE("deposits: explicit and pack PBR sets have sampling records") {
  auto p = patch();
  p.fragments = 0;
  p.type = RockType::Sandstone;
  materials::Pack pack;
  std::string error;
  REQUIRE(materials::parse_pack(
      R"({"format":"engine.materials.pack/1","name":"stand-in","version":"1","roles":{)"
      R"("rock/sandstone":{"tile_m":0.5,"normal_scale":0.6,"normal_convention":"opengl","maps":{)"
      R"("base_color":{"file":"c.png"},"normal":{"file":"n.png"},)"
      R"("roughness":{"file":"r.png"},"metallic":{"file":"m.png"},"height":{"file":"h.png"}}}}})",
      "C:/stand-in", pack, error));
  std::string prefix;
  f32 normal_scale = 0;
  SUBCASE("explicit maps override the pack") {
    p.material.base_color = "own/c.png";
    p.material.normal = "own/n.png";
    p.material.roughness = "own/r.png";
    p.material.tile_m = 2.0f;
    p.material.normal_scale = .75f;
    prefix = "own/";
    normal_scale = .75f;
  }
  SUBCASE("pack supplies all maps") {
    prefix = "C:/stand-in/";
    normal_scale = .6f;
  }
  geometry::ClusterFileData data;
  REQUIRE_MESSAGE(build_deposit_container(p, {}, 0, data, error, &pack), error);
  REQUIRE(data.materials.size() == 1);
  REQUIRE(data.material_sampling.size() == data.materials.size());
  REQUIRE(data.image_paths.size() == 3);
  const auto& m = data.materials[0];
  REQUIRE(m.base_color_image >= 0);
  REQUIRE(m.normal_image >= 0);
  CHECK(data.image_paths[static_cast<u32>(m.base_color_image)] == prefix + "c.png");
  CHECK(data.image_paths[static_cast<u32>(m.normal_image)] == prefix + "n.png");
  CHECK(std::find(data.image_paths.begin(), data.image_paths.end(), prefix + "r.png") !=
        data.image_paths.end());
  CHECK(m.normal_scale == doctest::Approx(normal_scale));
  CHECK(m.metallic == 0);
  CHECK(m.roughness == 1);
  CHECK(data.textures.size() == data.image_paths.size());
  geometry::TextureSlotSampling slots[geometry::k_material_slots];
  f32 occlusion = 0;
  geometry::decode_material_sampling(data.material_sampling[0], slots, occlusion);
  for (const auto& slot : slots)
    CHECK(slot.transform.identity());  // UVs already carry the chosen repeat.
  CHECK(occlusion == 1);

  // Material setup must not rewrite the solved geometry or any vertex attributes/masks.
  auto expected = p;
  if (!p.material.base_color) expected.material.tile_m = pack.roles[0].set.tile_m;
  expected.material.base_color.reset();
  expected.material.normal.reset();
  expected.material.roughness.reset();
  geometry::ClusterFileData plain;
  REQUIRE(build_deposit_container(expected, {}, 0, plain, error));
  const auto same_bytes = [](const auto& a, const auto& b) {
    return a.size() == b.size() && hash_bytes(a.data(), a.size() * sizeof(a[0])) ==
                                       hash_bytes(b.data(), b.size() * sizeof(b[0]));
  };
  CHECK(same_bytes(data.mesh.mesh.vertices, plain.mesh.mesh.vertices));
  CHECK(same_bytes(data.mesh.mesh.attributes, plain.mesh.mesh.attributes));
  CHECK(same_bytes(data.cover_mask, plain.cover_mask));
}
TEST_CASE("deposits: a bed samples the pack's scree before its rock's") {
  // G143's third pass (rocks.md, "The bed is rubble"): a talus' surface is fresh clasts, so the
  // granular role comes first and the rock's own role is behind it. No pack on disk: two stand-in
  // packs parsed from text.
  auto p = patch();
  p.fragments = 0;
  p.type = RockType::Granite;
  const char* rock =
      R"("rock/granite":{"tile_m":1,"normal_convention":"opengl","maps":{"base_color":{"file":"g.png"},)"
      R"("normal":{"file":"gn.png"},"roughness":{"file":"gr.png"},"metallic":{"file":"gm.png"},)"
      R"("height":{"file":"gh.png"}}})";
  const char* scree =
      R"("rock/scree":{"tile_m":1,"normal_convention":"opengl","maps":{"base_color":{"file":"s.png"},)"
      R"("normal":{"file":"sn.png"},"roughness":{"file":"sr.png"},"metallic":{"file":"sm.png"},)"
      R"("height":{"file":"sh.png"}}})";
  const std::string head =
      R"({"format":"engine.materials.pack/1","name":"stand-in","version":"1","roles":{)";
  materials::Pack both, granite_only;
  std::string error;
  REQUIRE_MESSAGE(
      materials::parse_pack(head + rock + "," + scree + "}}", "C:/stand-in", both, error), error);
  REQUIRE(materials::parse_pack(head + rock + "}}", "C:/stand-in", granite_only, error));
  geometry::ClusterFileData data;
  REQUIRE_MESSAGE(build_deposit_container(p, {}, 0, data, error, &both), error);
  REQUIRE(data.materials[0].base_color_image >= 0);
  CHECK(data.image_paths[static_cast<u32>(data.materials[0].base_color_image)] ==
        "C:/stand-in/s.png");
  REQUIRE_MESSAGE(build_deposit_container(p, {}, 0, data, error, &granite_only), error);
  REQUIRE(data.materials[0].base_color_image >= 0);
  CHECK(data.image_paths[static_cast<u32>(data.materials[0].base_color_image)] ==
        "C:/stand-in/g.png");
  // A recipe that names its own image keeps it.
  p.material.base_color = "own/c.png";
  REQUIRE_MESSAGE(build_deposit_container(p, {}, 0, data, error, &both), error);
  CHECK(data.image_paths[static_cast<u32>(data.materials[0].base_color_image)] == "own/c.png");
}
TEST_CASE("deposits: a loose stone wears its rock's own surface and the bed the scree's") {
  // T108 (rocks.md, "A stone wears its rock"): a loose fragment or a shed block is one stone, so it
  // samples its rock's role (`rock/<type>`) at that set's own repeat, never the bed's granular set,
  // which is a picture of many stones. Two stand-in packs parsed from text: the rock's set repeats
  // every 2 m, the scree's every 0.5 m.
  auto p = patch();
  p.type = RockType::Granite;
  DepositBlock block;
  block.x_m = 1;
  block.z_m = 1;
  block.y_m = .3f;
  block.half = {.4f, .2f, .3f};
  block.buried = .3f;
  block.seed = 108;
  p.blocks.push_back(block);
  const char* rock =
      R"("rock/granite":{"tile_m":2,"normal_convention":"opengl","maps":{"base_color":{"file":"g.png"},)"
      R"("normal":{"file":"gn.png"},"roughness":{"file":"gr.png"},"metallic":{"file":"gm.png"},)"
      R"("height":{"file":"gh.png"}}})";
  const char* scree =
      R"("rock/scree":{"tile_m":0.5,"normal_convention":"opengl","maps":{"base_color":{"file":"s.png"},)"
      R"("normal":{"file":"sn.png"},"roughness":{"file":"sr.png"},"metallic":{"file":"sm.png"},)"
      R"("height":{"file":"sh.png"}}})";
  const std::string head =
      R"({"format":"engine.materials.pack/1","name":"stand-in","version":"1","roles":{)";
  materials::Pack both, granite_only;
  std::string error;
  REQUIRE_MESSAGE(
      materials::parse_pack(head + rock + "," + scree + "}}", "C:/stand-in", both, error), error);
  REQUIRE(materials::parse_pack(head + rock + "}}", "C:/stand-in", granite_only, error));
  auto image_of = [](const geometry::ClusterFileData& d, u32 m) {
    const i32 slot = d.materials[m].base_color_image;
    return slot >= 0 ? d.image_paths[static_cast<u32>(slot)] : std::string();
  };
  auto clusters_of = [](const geometry::ClusterFileData& d, u32 m) {
    return static_cast<u32>(std::count(d.cluster_material.begin(), d.cluster_material.end(), m));
  };
  geometry::ClusterFileData data;
  REQUIRE_MESSAGE(build_deposit_container(p, {}, 0, data, error, &both), error);
  // The bed is material 0 and wears the scree; the fragments and the block are material 1 and
  // wear the granite; each names its own clusters.
  REQUIRE(data.materials.size() == 2);
  REQUIRE(data.material_sampling.size() == 2);
  CHECK(image_of(data, 0) == "C:/stand-in/s.png");
  CHECK(image_of(data, 1) == "C:/stand-in/g.png");
  CHECK(clusters_of(data, 0) > 0u);
  CHECK(clusters_of(data, 1) > 0u);
  CHECK(clusters_of(data, 0) + clusters_of(data, 1) == data.mesh.mesh.clusters.size());
  // With only the rock's set, both wear it; with no pack, both are the type's flat colour.
  REQUIRE_MESSAGE(build_deposit_container(p, {}, 0, data, error, &granite_only), error);
  REQUIRE(data.materials.size() == 2);
  CHECK(image_of(data, 0) == "C:/stand-in/g.png");
  CHECK(image_of(data, 1) == "C:/stand-in/g.png");
  REQUIRE_MESSAGE(build_deposit_container(p, {}, 0, data, error), error);
  REQUIRE(data.materials.size() == 2);
  CHECK(data.materials[0].base_color_image < 0);
  CHECK(data.materials[1].base_color_image < 0);
  CHECK(data.material_sampling.empty());
  // The stones are charted at their own set's repeat and the bed at its own: the bed's UVs do not
  // move with the stones' set, and the stones' coordinates shrink as their set's repeat grows.
  assets::MeshData fine_rock, coarse_rock;
  REQUIRE(solve_deposit(p, fine_rock, error, .5));
  REQUIRE(solve_deposit(p, coarse_rock, error, 2.0));
  REQUIRE(fine_rock.primitives.size() == 2);
  REQUIRE(coarse_rock.primitives.size() == 2);
  CHECK(fine_rock.primitives[0].material == 0);
  CHECK(fine_rock.primitives[1].material == 1);
  REQUIRE(fine_rock.indices.size() == coarse_rock.indices.size());
  f64 span[2] = {};
  bool bed_same = true;
  for (u32 k = 0; k < 2; ++k) {
    const auto& part = fine_rock.primitives[k];
    for (u32 i = part.first_index; i < part.first_index + part.index_count; ++i) {
      const Vec2 a = fine_rock.uvs[fine_rock.indices[i]],
                 b = coarse_rock.uvs[coarse_rock.indices[i]];
      if (k == 0) bed_same = bed_same && a.x == b.x && a.y == b.y;
      if (k == 1) {
        span[0] = std::max(span[0], static_cast<f64>(std::max(std::abs(a.x), std::abs(a.y))));
        span[1] = std::max(span[1], static_cast<f64>(std::max(std::abs(b.x), std::abs(b.y))));
      }
    }
  }
  MESSAGE("the stones' widest coordinate at a 0.5 m and a 2 m repeat: ", span[0], ", ", span[1]);
  CHECK(bed_same);
  // Each chart sits in a cell of a whole, even number of repeats (rocks.md, "External tiled
  // maps"), so a quarter of the repeat at least doubles the widest coordinate.
  CHECK(span[0] >= 2.0 * span[1]);
  // A patch with no loose stone has one material: the bed's.
  auto bare = patch();
  bare.fragments = 0;
  REQUIRE_MESSAGE(build_deposit_container(bare, {}, 0, data, error, &both), error);
  CHECK(data.materials.size() == 1);
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

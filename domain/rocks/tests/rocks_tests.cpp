// The rock solver (docs/subsystems/rocks.md): closedness, the budget ladder, burial, the file and
// its key, the registry, and determinism.
#include "rock_checks.h"

#include <core/hash/hash.h>
#include <domain/assets/mesh_generator.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/material_sampling.h>
#include <domain/materials/materials.h>
#include <domain/rocks/rocks.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::rocks;
using namespace engine::rocks::checks;

namespace {

RockFile boulder(Boulder b = Boulder{}) {
  RockFile f;
  f.shape = RockShape::Boulder;
  f.boulder = b;
  return f;
}
RockFile outcrop(Outcrop o = Outcrop{}) {
  RockFile f;
  f.shape = RockShape::Outcrop;
  f.outcrop = o;
  return f;
}
RockFile scree(Scree s = Scree{}) {
  RockFile f;
  f.shape = RockShape::Scree;
  f.scree = s;
  return f;
}

assets::MeshData solve(const RockFile& f, RockReport* report = nullptr) {
  assets::MeshData mesh;
  std::string error;
  const bool ok = solve_rock(f, mesh, report, &error);
  REQUIRE_MESSAGE(ok, error);
  return mesh;
}

Boulder typed(RockType type) {
  Boulder b;
  b.type = type;
  return b;
}

}  // namespace

TEST_CASE("rocks: every shape and type is closed, two-manifold and wound outwards") {
  for (const RockType type :
       {RockType::Granite, RockType::Sandstone, RockType::Basalt, RockType::Limestone}) {
    CAPTURE(rock_type_name(type));
    RockReport report;
    const assets::MeshData mesh = solve(boulder(typed(type)), &report);
    check_closed(mesh, 1);
    CHECK(report.parts == 1);
    CHECK(report.triangles <= Boulder{}.triangle_budget);
  }
  {
    RockReport report;
    const assets::MeshData mesh = solve(outcrop(), &report);
    check_closed(mesh, Outcrop{}.count);
    CHECK(report.parts == Outcrop{}.count);
  }
  {
    RockReport report;
    const assets::MeshData mesh = solve(scree(), &report);
    check_closed(mesh, Scree{}.count);
    CHECK(report.parts == Scree{}.count);
  }
  // A layered sandstone, the sharpest basalt, the roundest limestone, and no joints at all.
  Boulder layered = typed(RockType::Sandstone);
  layered.layers = 24;
  check_closed(solve(boulder(layered)), 1);
  Boulder sharp = typed(RockType::Basalt);
  sharp.rounding = 0.0f;
  sharp.joint_sets = 4;
  check_closed(solve(boulder(sharp)), 1);
  Boulder round = typed(RockType::Limestone);
  round.rounding = 1.0f;
  round.joint_sets = 0;
  check_closed(solve(boulder(round)), 1);
}

TEST_CASE("rocks: the budget ladder holds every budget, and the coarsest rung is the same rock") {
  const RockFile top = boulder([] {
    Boulder b;
    b.triangle_budget = 500000;
    return b;
  }());
  RockReport finest;
  const assets::MeshData fine = solve(top, &finest);
  CHECK(finest.finest_grid == 192u);
  CHECK(finest.steps == 0u);
  CHECK(finest.triangles == triangles_of_grid(192));
  const Box fine_box = bounds(fine);
  const f64 fine_volume = volume_of(fine);
  u32 last = finest.triangles;
  for (const u32 budget : {200000u, 60000u, 20000u, 8000u, 3000u, 1200u, 500u}) {
    CAPTURE(budget);
    Boulder b;
    b.triangle_budget = budget;
    RockReport report;
    const assets::MeshData mesh = solve(boulder(b), &report);
    MESSAGE("budget " << budget << ": n = " << report.finest_grid << ", " << report.triangles
                      << " triangles, " << report.steps << " steps down");
    CHECK(report.triangles <= budget);
    CHECK(report.triangles <= last);
    last = report.triangles;
    check_closed(mesh, 1);
    // Still the rock: its box within a tenth of the finest's on every axis, its volume within 15%.
    const Box box = bounds(mesh);
    for (u32 axis = 0; axis < 3; ++axis) {
      const f32 want = fine_box.hi[axis] - fine_box.lo[axis];
      const f32 got = box.hi[axis] - box.lo[axis];
      CHECK(std::fabs(got - want) <= 0.1f * want);
    }
    CHECK(std::fabs(volume_of(mesh) - fine_volume) <= 0.15 * fine_volume);
  }
  // The outcrop spends its budget over its boulders and every one fits.
  Outcrop o;
  o.triangle_budget = 1000;
  o.count = 4;
  RockReport report;
  check_closed(solve(outcrop(o), &report), 4);
  CHECK(report.triangles <= 1000u);
  CHECK(report.coarsest_grid == 4u);
  // And a budget that cannot hold every part at the coarsest rung is refused with the count.
  o.count = 16;
  assets::MeshData mesh;
  std::string error;
  CHECK_FALSE(solve_rock(outcrop(o), mesh, nullptr, &error));
  CHECK(error.find("triangle_budget") != std::string::npos);
}

TEST_CASE("rocks: `buried` is the share of the box under the ground") {
  for (const f32 buried : {0.0f, 0.15f, 0.4f, 0.8f}) {
    CAPTURE(buried);
    Boulder b;
    b.buried = buried;
    const Box box = bounds(solve(boulder(b)));
    const f32 share = -box.lo.y / (box.hi.y - box.lo.y);
    CHECK(share == doctest::Approx(buried).epsilon(1e-4).scale(1.0));
  }
  // An outcrop's boulders each stand with that share under the bedding plane, so the whole stands
  // on it, the larger ones taller.
  const Box box = bounds(solve(outcrop()));
  CHECK(box.lo.y < 0.0f);
  CHECK(box.hi.y > 0.0f);
  CHECK(box.hi.y <= Outcrop{}.size_y * 1.25f);
  // Scree lies on the ground: nothing far above it.
  const Box pile = bounds(solve(scree()));
  CHECK(pile.hi.y < Scree{}.fragment_m * 1.5f);
  CHECK(pile.hi.x <= Scree{}.size_x + Scree{}.fragment_m);
}

TEST_CASE("rocks: physical surface detail filters away below its sampling scale") {
  Boulder detailed;
  detailed.triangle_budget = 500;
  Boulder plain = detailed;
  plain.surface.strength = 0.0f;
  CHECK(mesh_hash(solve(boulder(detailed))) == mesh_hash(solve(boulder(plain))));
  detailed.triangle_budget = plain.triangle_budget = 80000;
  const auto mesh = solve(boulder(detailed));
  const auto smooth = solve(boulder(plain));
  CHECK(mesh.indices.size() == smooth.indices.size());
  CHECK(mesh_hash(mesh) != mesh_hash(smooth));
  check_closed(mesh, 1);
  // Recesses remove material, without collapsing the body or spending extra triangles.
  CHECK(volume_of(mesh) < volume_of(smooth));
  CHECK(volume_of(mesh) > 0.8 * volume_of(smooth));
  plain.triangle_budget = 20000;
  // G62's default mesh: strength zero is also the exact baseline for the G89 capture pairs.
  CHECK(mesh_hash(solve(boulder(plain))) == 0xee8f6918ba577fabull);
}

TEST_CASE("rocks: surface lengths scale with the rock and validate as physical lengths") {
  Boulder b;
  const auto original = solve(boulder(b));
  b.size_x *= 2.0f;
  b.size_y *= 2.0f;
  b.size_z *= 2.0f;
  b.material.tile_m *= 2.0f;
  b.surface.joint_spacing_m *= 2.0f;
  b.surface.joint_width_m *= 2.0f;
  b.surface.joint_depth_m *= 2.0f;
  b.surface.wavelength_m *= 2.0f;
  b.surface.relief_m *= 2.0f;
  const auto enlarged = solve(boulder(b));
  REQUIRE(original.positions.size() == enlarged.positions.size());
  for (u32 i = 0; i < original.positions.size(); ++i) {
    CHECK(enlarged.positions[i].x == 2.0f * original.positions[i].x);
    CHECK(enlarged.positions[i].y == 2.0f * original.positions[i].y);
    CHECK(enlarged.positions[i].z == 2.0f * original.positions[i].z);
  }
  std::string error;
  for (const char* bad : {R"({"boulder":{"surface":{"strength":1.1}}})",
                          R"({"boulder":{"surface":{"joint_spacing_m":0}}})",
                          R"({"boulder":{"surface":{"joint_width_m":1}}})",
                          R"({"boulder":{"surface":{"joint_depth_m":-1}}})",
                          R"({"boulder":{"surface":{"wavelength_m":0}}})",
                          R"({"boulder":{"surface":{"relief_m":0.5}}})"}) {
    RockFile f;
    CHECK_FALSE(read_rock_file(bad, f, error));
    CHECK(error.find("surface.") != std::string::npos);
  }
}

TEST_CASE("rocks: strongest valid recesses retain closed shells across lithologies") {
  for (const RockType type :
       {RockType::Granite, RockType::Sandstone, RockType::Basalt, RockType::Limestone}) {
    Boulder b = typed(type);
    b.surface.joint_depth_m = 2.0f;
    b.surface.joint_width_m = 0.3f;
    b.surface.wavelength_m = 2.0f;
    b.surface.relief_m = 0.5f;
    b.triangle_budget = 80000;
    CAPTURE(rock_type_name(type));
    check_closed(solve(boulder(b)), 1);
  }
}

TEST_CASE("rocks: one seed is one rock, and another seed another") {
  CHECK(mesh_hash(solve(boulder())) == mesh_hash(solve(boulder())));
  Boulder other;
  other.seed = 2;
  CHECK(mesh_hash(solve(boulder())) != mesh_hash(solve(boulder(other))));
  // The type changes the rock, not only its colour.
  CHECK(mesh_hash(solve(boulder(typed(RockType::Granite)))) !=
        mesh_hash(solve(boulder(typed(RockType::Basalt)))));
}

TEST_CASE("rocks: the file is read, canonicalized and refused with the field's name") {
  RockFile a;
  std::string error;
  REQUIRE_MESSAGE(
      read_rock_file(R"({"shape":"Boulder","boulder":{"seed":4,"type":"Sandstone"}})", a, error),
      error);
  RockFile b;
  REQUIRE_MESSAGE(read_rock_file(R"({ "boulder" : { "type":"Sandstone", "seed": 4,
      "size_y": 0.8, "buried": 0.15 } })",
                                 b, error),
                  error);
  CHECK(canonical_rock_json(a) == canonical_rock_json(b));
  CHECK(rock_source_hash(a) == rock_source_hash(b));
  // A file naming no block holds its shape's defaults, and keys as the spelled-out defaults do.
  RockFile bare;
  REQUIRE_MESSAGE(read_rock_file(R"({"shape":"Scree"})", bare, error), error);
  REQUIRE(bare.scree.has_value());
  CHECK(rock_source_hash(bare) == rock_source_hash(scree()));
  CHECK(rock_source_hash(bare) != rock_source_hash(boulder()));

  const auto refused = [](const char* text, const char* field) {
    RockFile f;
    std::string why;
    CHECK_FALSE(read_rock_file(text, f, why));
    CHECK_MESSAGE(why.find(field) != std::string::npos, why);
  };
  refused(R"({"boulder":{"size_x":0.01}})", "size_x");
  refused(R"({"boulder":{"joint_sets":5}})", "joint_sets");
  refused(R"({"boulder":{"rounding":1.5}})", "rounding");
  refused(R"({"boulder":{"buried":0.9}})", "buried");
  refused(R"({"boulder":{"triangle_budget":100}})", "triangle_budget");
  refused(R"({"outcrop":{"count":1},"shape":"Outcrop"})", "count");
  refused(R"({"shape":"Boulder","scree":{}})", "scree");
  refused(R"({"boulder":{"material":{"normal_scale":5}}})", "material.normal_scale");
  refused(R"({"boulder":{"material":{"tile_m":0.01}}})", "material.tile_m");
  refused(R"({"boulder":{"colour":1}})", "colour");
}

TEST_CASE("rocks: the mesh generator \"rock\" builds what the capability builds") {
  CHECK(assets::mesh_generator_of("content/rocks/granite-boulder.rock.json") == "rock");
  const assets::MeshGeneratorDesc* generator = assets::MeshGeneratorRegistry::global().find("rock");
  REQUIRE(generator != nullptr);
  const std::string text = R"({"shape":"Boulder","boulder":{"seed":9,"triangle_budget":4000}})";
  const std::span<const u8> bytes(reinterpret_cast<const u8*>(text.data()), text.size());
  u64 hash = 0;
  std::string error;
  REQUIRE_MESSAGE(generator->source_hash(bytes, hash, error), error);
  RockFile file;
  REQUIRE(read_rock_file(text, file, error));
  CHECK(hash == rock_source_hash(file));
  const geometry::ClusterLodOptions lod{};
  const u32 page_bytes = geometry::ClusterPagesOptions{}.page_bytes;
  geometry::ClusterFileData through;
  REQUIRE_MESSAGE(
      generator->build(bytes, assets::MeshGeneratorInputs{}, lod, page_bytes, through, error),
      error);
  geometry::ClusterFileData direct;
  REQUIRE_MESSAGE(build_rock_container(file, lod, page_bytes, direct, nullptr, error), error);
  CHECK(geometry::cluster_file_hash(through) == geometry::cluster_file_hash(direct));
  CHECK(through.source_hash == hash);
  CHECK(through.build_key == rock_build_key(hash, lod, page_bytes));
  CHECK(through.generator == "rock");
  CHECK(through.generator_kind == "Boulder");
  CHECK(through.generator_params == canonical_rock_json(file));
  // Closed and one-sided: every cluster carries a cone.
  REQUIRE(!through.mesh.mesh.clusters.empty());
  u32 coneless = 0;
  for (const geometry::ClusterDesc& c : through.mesh.mesh.clusters)
    coneless += (c.cone >> 24) == 0x7fu ? 1u : 0u;
  MESSAGE(coneless << " of " << through.mesh.mesh.clusters.size() << " clusters without a cone");
  CHECK(coneless < through.mesh.mesh.clusters.size());
  // A file the solver refuses is refused through the registry with the same sentence.
  const std::string bad = R"({"boulder":{"joint_sets":9}})";
  CHECK_FALSE(generator->source_hash(
      std::span<const u8>(reinterpret_cast<const u8*>(bad.data()), bad.size()), hash, error));
  CHECK(error.find("joint_sets") != std::string::npos);
}

TEST_CASE("rocks: the default boulder is the committed bytes on every toolchain") {
  // Taken on MSVC; GCC and Clang at v3 and GCC at v2 must reproduce them. A change that moves
  // them bumps k_rocks_version and these numbers in the same commit.
  // G89, rocks version 2: 0xee8f6918ba577fab until physical surface detail and the 80k default.
  constexpr u64 k_mesh = 0xe7f94e4304b6ea53ull;
  // 0x7865d56fc786dbd1 until RockMaterial gained `normal_scale` (version 2), which the canonical
  // parameters in the generator section spell out; the mesh did not move.
  // G89 supersedes 0x420fa50ebc8196ff: new mesh, canonical surface fields and solver version.
  constexpr u64 k_container = 0xb2611b877ab5bd4eull;
  const assets::MeshData mesh = solve(boulder());
  geometry::ClusterFileData data;
  std::string error;
  REQUIRE_MESSAGE(
      build_rock_container(boulder(), geometry::ClusterLodOptions{},
                           geometry::ClusterPagesOptions{}.page_bytes, data, nullptr, error),
      error);
  char text[64];
  std::snprintf(text, sizeof(text), "%016llx %016llx",
                static_cast<unsigned long long>(mesh_hash(mesh)),
                static_cast<unsigned long long>(geometry::cluster_file_hash(data)));
  MESSAGE("default boulder: " << text);
  CHECK(mesh_hash(mesh) == k_mesh);
  CHECK(geometry::cluster_file_hash(data) == k_container);
}

TEST_CASE("rocks: the committed recipes are closed, hold their budgets, and are their bytes") {
  // content/rocks/: what the rock-field scene and its scatter layer name. Each recipe's mesh is
  // pinned like the default boulder's, so a solver change that moves a committed rock says so.
  struct Recipe {
    const char* file;
    u32 parts;
    u64 mesh;
  };
  constexpr Recipe k_recipes[] = {
      // G89 surface + budgets; old pins e943fb204d0466dc, b7abeb70a637033a,
      // 6a2ffb1198db5539, 511edd62e55ef986 respectively (G62).
      {"granite-boulder.rock.json", 1, 0x5fc64fb011e5cdfaull},
      {"sandstone-boulder.rock.json", 1, 0xa9cf86119ec92a02ull},
      {"basalt-outcrop.rock.json", 7, 0xc8a7870ce261e0acull},
      {"granite-scree.rock.json", 14, 0x74b4161bbefd5f1cull},
      {"jointed-pinnacles.rock.json", 8, 0x21756d998264c659ull},
      {"shore-rubble.rock.json", 48, 0x2eb5fb5c3e42b304ull},
  };
  const std::string dir = test::data_path(ENGINE_SOURCE_DIR "/content/rocks", "content/rocks");
  if (!test::path_exists(dir)) {
    MESSAGE("content/rocks is not here (a bundle without it): skipped");
    return;
  }
  for (const Recipe& recipe : k_recipes) {
    const std::string name = recipe.file;
    CAPTURE(name);
    std::ifstream in(dir + "/" + recipe.file, std::ios::binary);
    REQUIRE(in.good());
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    RockFile file;
    std::string error;
    REQUIRE_MESSAGE(read_rock_file(text, file, error), error);
    RockReport report;
    const assets::MeshData mesh = solve(file, &report);
    check_closed(mesh, recipe.parts);
    // The UVs inside the content build's range, so the build takes the rock unwarned.
    f32 widest = 0.0f;
    for (const Vec2& uv : mesh.uvs)
      widest = std::max(widest, std::max(std::fabs(uv.x), std::fabs(uv.y)));
    CHECK(widest <= 16.0f);
    char hash[24];
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(mesh_hash(mesh)));
    MESSAGE(std::string(recipe.file)
            << ": " << report.triangles << " triangles, " << report.parts << " parts, n "
            << report.coarsest_grid << ".." << report.finest_grid << ", " << report.cuts
            << " cuts, mesh " << hash);
    CHECK(mesh_hash(mesh) == recipe.mesh);
  }
}

// rocks.md, "UVs and materials": the block's set, else the pack's `rock/<type>`, else the flat
// colour; the charts in whole repeats of whichever set was chosen, so no transform.
TEST_CASE("rocks: a rock samples its block's set, else its type's role in the pack") {
  const char* k_pack =
      R"({"format":"engine.materials.pack/1","name":"stand-in","version":"1","roles":{)"
      R"("rock/sandstone":{"tile_m":0.5,"normal_scale":0.6,"normal_convention":"opengl","maps":{)"
      R"("base_color":{"file":"s/c.png"},"normal":{"file":"s/n.png"},"roughness":{"file":"s/r.png"},)"
      R"("metallic":{"file":"s/m.png"},"height":{"file":"s/h.png"}}}}})";
  materials::Pack pack;
  std::string error;
  REQUIRE_MESSAGE(materials::parse_pack(k_pack, "C:/stand-in", pack, error), error);
  CHECK(rock_role(RockType::Sandstone) == "rock/sandstone");
  CHECK(rock_role(RockType::Granite) == "rock/granite");
  const geometry::ClusterLodOptions lod{};
  const u32 page_bytes = geometry::ClusterPagesOptions{}.page_bytes;

  Boulder sandstone;
  sandstone.type = RockType::Sandstone;
  sandstone.triangle_budget = 4000;
  geometry::ClusterFileData plain;
  REQUIRE_MESSAGE(build_rock_container(boulder(sandstone), lod, page_bytes, plain, nullptr, error),
                  error);
  CHECK(plain.image_paths.empty());
  CHECK(plain.textures.empty());

  // The pack's set: its images, absolute, its normal strength, and the charts at its 0.5 m repeat —
  // twice the coordinates of the 1 m the recipe's default would have laid them at.
  geometry::ClusterFileData from_pack;
  REQUIRE_MESSAGE(
      build_rock_container(boulder(sandstone), lod, page_bytes, from_pack, nullptr, error, &pack),
      error);
  REQUIRE(from_pack.materials.size() == 1);
  REQUIRE(from_pack.materials[0].base_color_image >= 0);
  CHECK(from_pack.image_paths[static_cast<u32>(from_pack.materials[0].base_color_image)] ==
        "C:/stand-in/s/c.png");
  CHECK(from_pack.materials[0].normal_scale == doctest::Approx(0.6f));
  CHECK(from_pack.materials[0].metallic == 0.0f);
  CHECK(from_pack.textures.size() == from_pack.image_paths.size());
  CHECK(from_pack.build_key != plain.build_key);
  CHECK(from_pack.source_hash == plain.source_hash);
  geometry::TextureSlotSampling slots[geometry::k_material_slots];
  f32 occlusion = 0.0f;
  geometry::decode_material_sampling(from_pack.material_sampling[0], slots, occlusion);
  CHECK(slots[geometry::k_slot_base_color].transform.identity());
  RockFile at_half = boulder(sandstone);
  at_half.boulder->material.tile_m = 0.5f;
  CHECK(mesh_hash(solve(at_half)) != mesh_hash(solve(boulder(sandstone))));

  // A granite has no role in this pack: the flat colour.
  geometry::ClusterFileData granite;
  REQUIRE_MESSAGE(build_rock_container(boulder(), lod, page_bytes, granite, nullptr, error, &pack),
                  error);
  CHECK(granite.image_paths.empty());

  // The recipe's own images win, at the recipe's repeat, relative to the recipe as it spelled them.
  RockFile own = boulder(sandstone);
  own.boulder->material.base_color = "mine/c.png";
  own.boulder->material.tile_m = 2.0f;
  geometry::ClusterFileData mine;
  REQUIRE_MESSAGE(build_rock_container(own, lod, page_bytes, mine, nullptr, error, &pack), error);
  REQUIRE(mine.image_paths.size() == 1);
  CHECK(mine.image_paths[0] == "mine/c.png");
  CHECK(mine.materials[0].normal_image == -1);
}

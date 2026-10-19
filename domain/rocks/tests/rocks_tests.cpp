// The rock solver (docs/subsystems/rocks.md): closedness, the budget ladder, burial, the file and
// its key, the registry, and determinism.
#include <core/hash/hash.h>
#include <domain/assets/mesh_generator.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_pages.h>
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

u64 mesh_hash(const assets::MeshData& mesh) {
  u64 h = hash_bytes(mesh.positions.data(), mesh.positions.size() * sizeof(Vec3));
  h = hash_combine(h, hash_bytes(mesh.normals.data(), mesh.normals.size() * sizeof(Vec3)));
  h = hash_combine(h, hash_bytes(mesh.uvs.data(), mesh.uvs.size() * sizeof(Vec2)));
  h = hash_combine(h, hash_bytes(mesh.indices.data(), mesh.indices.size() * sizeof(u32)));
  return h;
}

struct Topology {
  std::vector<u32> weld;  // per vertex, its position's id
  u32 positions = 0;
  u32 components = 0;
};

// Welds by exact position (the solver's seams are the same bits on both sides by construction).
Topology weld(const assets::MeshData& mesh) {
  Topology t;
  const u32 n = mesh.positions.size();
  std::vector<u32> order(n);
  for (u32 i = 0; i < n; ++i)
    order[i] = i;
  const auto less = [&](u32 a, u32 b) {
    const Vec3 pa = mesh.positions[a];
    const Vec3 pb = mesh.positions[b];
    if (pa.x != pb.x) return pa.x < pb.x;
    if (pa.y != pb.y) return pa.y < pb.y;
    return pa.z < pb.z;
  };
  std::sort(order.begin(), order.end(), less);
  t.weld.assign(n, 0);
  for (u32 i = 0; i < n; ++i) {
    if (i > 0 && !less(order[i - 1], order[i]) && !less(order[i], order[i - 1])) {
      t.weld[order[i]] = t.weld[order[i - 1]];
    } else {
      t.weld[order[i]] = t.positions++;
    }
  }
  return t;
}

// Every validity property a solved rock has: finite, closed, two-manifold, consistently and
// outward wound, and `parts` connected components.
void check_closed(const assets::MeshData& mesh, u32 parts) {
  REQUIRE(!mesh.positions.empty());
  REQUIRE(mesh.normals.size() == mesh.positions.size());
  REQUIRE(mesh.uvs.size() == mesh.positions.size());
  REQUIRE(mesh.indices.size() % 3 == 0);
  REQUIRE(mesh.primitives.size() == 1);
  REQUIRE(mesh.materials.size() == 1);
  CHECK_FALSE(mesh.materials[0].double_sided);
  for (const Vec3& p : mesh.positions)
    REQUIRE((std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)));
  const Topology t = weld(mesh);
  // Directed edges, welded: each must appear exactly once, and its reverse exactly once.
  std::vector<u64> directed;
  directed.reserve(mesh.indices.size());
  u32 degenerate = 0;
  for (u32 i = 0; i < mesh.indices.size(); i += 3) {
    const u32 w[3] = {t.weld[mesh.indices[i]], t.weld[mesh.indices[i + 1]],
                      t.weld[mesh.indices[i + 2]]};
    if (w[0] == w[1] || w[1] == w[2] || w[0] == w[2]) ++degenerate;
    for (u32 e = 0; e < 3; ++e)
      directed.push_back(static_cast<u64>(w[e]) << 32 | w[(e + 1) % 3]);
  }
  CHECK(degenerate == 0);
  std::sort(directed.begin(), directed.end());
  u32 repeated = 0;
  u32 unmatched = 0;
  for (u32 i = 0; i < directed.size(); ++i) {
    if (i > 0 && directed[i] == directed[i - 1]) ++repeated;
    const u64 reverse = (directed[i] & 0xffffffffull) << 32 | directed[i] >> 32;
    if (!std::binary_search(directed.begin(), directed.end(), reverse)) ++unmatched;
  }
  CHECK_MESSAGE(repeated == 0, repeated << " edges used twice in one direction");
  CHECK_MESSAGE(unmatched == 0, unmatched << " edges with no opposite: a hole");
  // Components over welded edges.
  std::vector<u32> parent(t.positions);
  for (u32 i = 0; i < t.positions; ++i)
    parent[i] = i;
  const auto find = [&](u32 x) {
    while (parent[x] != x)
      x = parent[x] = parent[parent[x]];
    return x;
  };
  for (const u64 e : directed)
    parent[find(static_cast<u32>(e >> 32))] = find(static_cast<u32>(e & 0xffffffffull));
  u32 components = 0;
  for (u32 i = 0; i < t.positions; ++i)
    components += find(i) == i ? 1u : 0u;
  CHECK(components == parts);
  // Outward: the signed volume of every component is positive, and so is the whole's.
  f64 volume = 0.0;
  for (u32 i = 0; i < mesh.indices.size(); i += 3) {
    const Vec3 a = mesh.positions[mesh.indices[i]];
    const Vec3 b = mesh.positions[mesh.indices[i + 1]];
    const Vec3 c = mesh.positions[mesh.indices[i + 2]];
    volume += static_cast<f64>(dot(a, cross(b, c))) / 6.0;
  }
  CHECK(volume > 0.0);
  // And the faces agree with the normals the solver gave their corners.
  u32 against = 0;
  for (u32 i = 0; i < mesh.indices.size(); i += 3) {
    const Vec3 a = mesh.positions[mesh.indices[i]];
    const Vec3 fn =
        cross(mesh.positions[mesh.indices[i + 1]] - a, mesh.positions[mesh.indices[i + 2]] - a);
    const Vec3 vn = mesh.normals[mesh.indices[i]] + mesh.normals[mesh.indices[i + 1]] +
                    mesh.normals[mesh.indices[i + 2]];
    if (dot(fn, vn) <= 0.0f) ++against;
  }
  CHECK_MESSAGE(against == 0, against << " faces wound against their normals");
}

struct Box {
  Vec3 lo{1e30f, 1e30f, 1e30f};
  Vec3 hi{-1e30f, -1e30f, -1e30f};
};
Box bounds(const assets::MeshData& mesh) {
  Box b;
  for (const Vec3& p : mesh.positions) {
    b.lo = Vec3{std::min(b.lo.x, p.x), std::min(b.lo.y, p.y), std::min(b.lo.z, p.z)};
    b.hi = Vec3{std::max(b.hi.x, p.x), std::max(b.hi.y, p.y), std::max(b.hi.z, p.z)};
  }
  return b;
}

f64 volume_of(const assets::MeshData& mesh) {
  f64 v = 0.0;
  for (u32 i = 0; i < mesh.indices.size(); i += 3)
    v += static_cast<f64>(
             dot(mesh.positions[mesh.indices[i]],
                 cross(mesh.positions[mesh.indices[i + 1]], mesh.positions[mesh.indices[i + 2]]))) /
         6.0;
  return v;
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
  CHECK(finest.finest_grid == 128u);
  CHECK(finest.steps == 0u);
  CHECK(finest.triangles == triangles_of_grid(128));
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
  refused(R"({"boulder":{"material":{"base_color":"granite.png"}}})", "material.base_color");
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
  REQUIRE_MESSAGE(generator->build(bytes, lod, page_bytes, through, error), error);
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
  constexpr u64 k_mesh = 0xee8f6918ba577fabull;
  constexpr u64 k_container = 0x7865d56fc786dbd1ull;
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
      {"granite-boulder.rock.json", 1, 0xe943fb204d0466dcull},
      {"sandstone-boulder.rock.json", 1, 0xb7abeb70a637033aull},
      {"basalt-outcrop.rock.json", 7, 0x6a2ffb1198db5539ull},
      {"granite-scree.rock.json", 14, 0x511edd62e55ef986ull},
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

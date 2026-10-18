// The date palm's solver (docs/subsystems/flora.md): shape, ranges, budget and determinism.
#include <core/hash/hash.h>
#include <domain/flora/flora.h>
#include <domain/flora/palm.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_pages.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace engine;

namespace {

flora::Palm default_palm() { return flora::Palm{}; }

assets::MeshData solve(const flora::Palm& p, flora::PalmReport* report = nullptr) {
  assets::MeshData mesh;
  std::string error;
  const bool ok = flora::solve_palm(p, mesh, report, &error);
  REQUIRE_MESSAGE(ok, error);
  return mesh;
}

// The primitive a material index draws, or null.
const assets::Primitive* primitive_of(const assets::MeshData& mesh, i32 material) {
  for (const assets::Primitive& p : mesh.primitives)
    if (p.material == material) return &p;
  return nullptr;
}

f64 triangle_area(const assets::MeshData& mesh, u32 t) {
  const Vec3 a = mesh.positions[mesh.indices[t * 3 + 0]];
  const Vec3 b = mesh.positions[mesh.indices[t * 3 + 1]];
  const Vec3 c = mesh.positions[mesh.indices[t * 3 + 2]];
  const Vec3 n = cross(b - a, c - a);
  return 0.5 * std::sqrt(static_cast<f64>(dot(n, n)));
}

// Every stream the solver fills, hashed in order: the mesh's bytes, whatever the platform.
u64 mesh_hash(const assets::MeshData& mesh) {
  u64 h = hash_bytes(mesh.positions.data(), mesh.positions.size() * sizeof(Vec3));
  h = hash_combine(h, hash_bytes(mesh.normals.data(), mesh.normals.size() * sizeof(Vec3)));
  h = hash_combine(h, hash_bytes(mesh.uvs.data(), mesh.uvs.size() * sizeof(Vec2)));
  h = hash_combine(h, hash_bytes(mesh.indices.data(), mesh.indices.size() * sizeof(u32)));
  for (const assets::Primitive& p : mesh.primitives) {
    h = hash_combine(h, p.first_index);
    h = hash_combine(h, p.index_count);
    h = hash_combine(h, static_cast<u64>(static_cast<u32>(p.material)));
  }
  return h;
}

// Every validity property a solved palm has, whatever its parameters.
void check_valid(const flora::Palm& p, const assets::MeshData& mesh) {
  REQUIRE(!mesh.positions.empty());
  REQUIRE(mesh.normals.size() == mesh.positions.size());
  REQUIRE(mesh.uvs.size() == mesh.positions.size());
  REQUIRE(mesh.indices.size() % 3 == 0);
  CHECK(mesh.indices.size() / 3 <= p.triangle_budget);
  u32 covered = 0;
  for (const assets::Primitive& prim : mesh.primitives) {
    CHECK(prim.first_index == covered);
    covered += prim.index_count;
    REQUIRE(prim.material >= 0);
    REQUIRE(static_cast<u32>(prim.material) < mesh.materials.size());
  }
  CHECK(covered == mesh.indices.size());
  for (const Vec3& v : mesh.positions) {
    REQUIRE(std::isfinite(v.x));
    REQUIRE(std::isfinite(v.y));
    REQUIRE(std::isfinite(v.z));
  }
  u32 degenerate = 0;
  f64 smallest = 1e30;
  for (u32 t = 0; t < mesh.indices.size() / 3; ++t) {
    const u32 a = mesh.indices[t * 3];
    const u32 b = mesh.indices[t * 3 + 1];
    const u32 c = mesh.indices[t * 3 + 2];
    REQUIRE(a < mesh.positions.size());
    REQUIRE(b < mesh.positions.size());
    REQUIRE(c < mesh.positions.size());
    const f64 area = triangle_area(mesh, t);
    smallest = std::min(smallest, area);
    if (a == b || b == c || a == c || !(area > 1e-10)) ++degenerate;
  }
  CHECK_MESSAGE(degenerate == 0, degenerate << " degenerate triangles; smallest area " << smallest);
}

}  // namespace

TEST_CASE("flora: the default palm is valid, holds its budget and is the height asked") {
  flora::PalmReport report;
  const flora::Palm p = default_palm();
  const assets::MeshData mesh = solve(p, &report);
  check_valid(p, mesh);
  MESSAGE("default palm: " << report.triangles << " triangles (trunk " << report.trunk_triangles
                           << ", crown " << report.crown_triangles << ", skirt "
                           << report.skirt_triangles << "), " << report.vertices << " vertices, "
                           << report.rings << " rings, " << report.steps << " budget steps");
  CHECK(report.triangles == mesh.indices.size() / 3);
  CHECK(report.vertices == mesh.positions.size());
  CHECK(report.steps == 0);  // the default fits its own budget at full detail
  // The trunk is material 0, one-sided; the apex is the highest point of the trunk, at height_m.
  REQUIRE(mesh.materials.size() >= 1);
  CHECK_FALSE(mesh.materials[0].double_sided);
  const assets::Primitive* trunk = primitive_of(mesh, 0);
  REQUIRE(trunk != nullptr);
  f32 top = -1e30f;
  f32 bottom = 1e30f;
  for (u32 i = 0; i < trunk->index_count; ++i) {
    const Vec3 v = mesh.positions[mesh.indices[trunk->first_index + i]];
    top = std::max(top, v.y);
    bottom = std::min(bottom, v.y);
  }
  // The wall's last row stands at the height asked; the cap rises a third of a radius above it.
  const f32 cap = 0.35f * p.trunk_top_radius_m;
  // The base ring is square to the leaning spine, so its low side is in the ground by the radius
  // times the lean's sine and its high side above it by as much.
  const f32 dip = p.trunk_base_radius_m * std::sin(p.lean_deg * 3.14159265f / 180.0f);
  CHECK(bottom >= -1.01f * dip);
  CHECK(bottom <= 0.0f);
  CHECK(std::abs(top - cap - p.height_m) <= 0.01f * p.height_m);
}

TEST_CASE("flora: the trunk's radii are the radii asked, and every wall face looks outwards") {
  // Standing straight, the spine is the y axis, and a wall vertex's distance from it is its radius
  // there: the widest in the bottom ring and in the top ring is a boot's lip, where the radius is
  // the one asked.
  flora::Palm p = default_palm();
  p.lean_deg = 0.0f;
  const assets::MeshData mesh = solve(p);
  const assets::Primitive* trunk = primitive_of(mesh, 0);
  REQUIRE(trunk != nullptr);
  const auto widest = [&](f32 y0, f32 y1) {
    f64 r = 0.0;
    for (u32 i = 0; i < trunk->index_count; ++i) {
      const Vec3 v = mesh.positions[mesh.indices[trunk->first_index + i]];
      if (v.y >= y0 && v.y <= y1)
        r = std::max(r, std::hypot(static_cast<f64>(v.x), static_cast<f64>(v.z)));
    }
    return r;
  };
  const f64 base = widest(0.0f, p.ring_spacing_m);
  const f64 top = widest(p.height_m - p.ring_spacing_m, p.height_m);
  CHECK(std::abs(base - static_cast<f64>(p.trunk_base_radius_m)) <=
        0.01 * static_cast<f64>(p.trunk_base_radius_m));
  CHECK(std::abs(top - static_cast<f64>(p.trunk_top_radius_m)) <=
        0.01 * static_cast<f64>(p.trunk_top_radius_m));

  // Every face of the trunk agrees with its corners' normals, and those point away from the
  // spine: the wall's outward, the cap's up. A face wound the wrong way round would be dropped by
  // its cluster's cone seen from outside.
  // Which way is out, for the faces as a whole: the volume they enclose with the base's centre
  // (the open bottom's disc holds that point, so it adds nothing) is positive for outward faces
  // and close to the tapered column's own.
  u32 inward = 0;
  f64 volume = 0.0;
  for (u32 t = trunk->first_index / 3; t < (trunk->first_index + trunk->index_count) / 3; ++t) {
    const Vec3 a = mesh.positions[mesh.indices[t * 3]];
    const Vec3 b = mesh.positions[mesh.indices[t * 3 + 1]];
    const Vec3 c = mesh.positions[mesh.indices[t * 3 + 2]];
    const Vec3 n = cross(b - a, c - a);
    const Vec3 vn = mesh.normals[mesh.indices[t * 3]] + mesh.normals[mesh.indices[t * 3 + 1]] +
                    mesh.normals[mesh.indices[t * 3 + 2]];
    if (dot(n, vn) <= 0.0f) ++inward;
    volume += static_cast<f64>(dot(a, cross(b, c))) / 6.0;
  }
  CHECK(inward == 0);
  const f64 r0 = static_cast<f64>(p.trunk_base_radius_m);
  const f64 r1 = static_cast<f64>(p.trunk_top_radius_m);
  const f64 column =
      3.14159265358979 * static_cast<f64>(p.height_m) * (r0 * r0 + r0 * r1 + r1 * r1) / 3.0;
  CHECK(volume > 0.85 * column);
  CHECK(volume < 1.05 * column);
}

TEST_CASE("flora: each frond is one connected, consistently wound surface above the trunk") {
  const flora::Palm p = default_palm();
  const assets::MeshData mesh = solve(p);
  REQUIRE(mesh.materials.size() == 3);
  CHECK(mesh.materials[1].double_sided);
  CHECK(mesh.materials[2].double_sided);
  CHECK(mesh.materials[1].base_color.y > mesh.materials[1].base_color.x);  // green
  CHECK(mesh.materials[2].base_color.x > mesh.materials[2].base_color.z);  // tan
  for (const i32 material : {1, 2}) {
    CAPTURE(material);
    const assets::Primitive* part = primitive_of(mesh, material);
    REQUIRE(part != nullptr);
    // Connected components over shared vertices: one per frond, since a frond's leaflets stand on
    // its rachis's own edge vertices. A loose leaflet would be a component of its own, and the LOD
    // builder could never collapse it away.
    Vector<u32> parent(mesh.positions.size());
    for (u32 v = 0; v < parent.size(); ++v)
      parent[v] = v;
    const auto find = [&](u32 v) {
      while (parent[v] != v)
        v = parent[v] = parent[parent[v]];
      return v;
    };
    u32 inconsistent = 0;
    for (u32 i = part->first_index; i < part->first_index + part->index_count; i += 3) {
      const u32 a = mesh.indices[i];
      const u32 b = mesh.indices[i + 1];
      const u32 c = mesh.indices[i + 2];
      parent[find(a)] = find(b);
      parent[find(b)] = find(c);
      const Vec3 n =
          cross(mesh.positions[b] - mesh.positions[a], mesh.positions[c] - mesh.positions[a]);
      if (dot(n, mesh.normals[a] + mesh.normals[b] + mesh.normals[c]) <= 0.0f) ++inconsistent;
    }
    Vector<u32> roots;
    for (u32 i = part->first_index; i < part->first_index + part->index_count; ++i) {
      const u32 root = find(mesh.indices[i]);
      if (std::find(roots.begin(), roots.end(), root) == roots.end()) roots.push_back(root);
    }
    CHECK(roots.size() == (material == 1 ? p.frond_count : p.dead_skirt_fronds));
    // A face whose winding disagrees with its corners' area-weighted normals is a fold or a flip;
    // a few where a leaflet's base turns against the rachis are tolerated, not a whole side.
    CHECK(inconsistent * 100 <= part->index_count / 3);
  }
  // The crown stands at the apex: its highest point above the trunk's, and every dead frond's
  // highest point below the apex.
  const assets::Primitive* crown = primitive_of(mesh, 1);
  f32 crown_top = -1e30f;
  for (u32 i = crown->first_index; i < crown->first_index + crown->index_count; ++i)
    crown_top = std::max(crown_top, mesh.positions[mesh.indices[i]].y);
  CHECK(crown_top > p.height_m + 0.5f);
  const assets::Primitive* skirt = primitive_of(mesh, 2);
  f32 skirt_top = -1e30f;
  for (u32 i = skirt->first_index; i < skirt->first_index + skirt->index_count; ++i)
    skirt_top = std::max(skirt_top, mesh.positions[mesh.indices[i]].y);
  CHECK(skirt_top < p.height_m);
  // No skirt asked, no skirt primitive.
  flora::Palm bare = p;
  bare.dead_skirt_fronds = 0;
  const assets::MeshData bare_mesh = solve(bare);
  CHECK(primitive_of(bare_mesh, 2) == nullptr);
}

TEST_CASE("flora: a smaller budget walks down the ladder and holds") {
  for (const u32 budget : {40000u, 20000u, 8000u, 2000u}) {
    flora::Palm p = default_palm();
    p.triangle_budget = budget;
    flora::PalmReport report;
    const assets::MeshData mesh = solve(p, &report);
    CAPTURE(budget);
    check_valid(p, mesh);
    CHECK(report.steps > 0);
    CHECK(report.triangles <= budget);
    // The fronds are kept whatever the budget: the crown's outline is what a palm is.
    const assets::Primitive* crown = primitive_of(mesh, 1);
    REQUIRE(crown != nullptr);
    MESSAGE(budget << ": " << report.triangles << " triangles after " << report.steps
                   << " steps; leaflets " << report.leaflets_per_side << " of "
                   << report.leaflet_segments << " segments, " << report.trunk_segments
                   << " segments x " << report.rows_per_ring << " rows a ring");
  }
}

TEST_CASE("flora: one seed is one palm, and another seed is another") {
  const flora::Palm p = default_palm();
  const u64 a = mesh_hash(solve(p));
  const u64 b = mesh_hash(solve(p));
  CHECK(a == b);
  flora::Palm q = p;
  q.seed = 2;
  CHECK(mesh_hash(solve(q)) != a);
}

TEST_CASE("flora: every parameter at the ends of its range builds a valid palm in its budget") {
  struct Case {
    const char* name;
    void (*set)(flora::Palm&, bool high);
  };
  const Case cases[] = {
      {"height_m", [](flora::Palm& p, bool h) { p.height_m = h ? 30.0f : 1.0f; }},
      {"trunk_base_radius_m",
       [](flora::Palm& p, bool h) {
         p.trunk_base_radius_m = h ? 1.0f : 0.05f;
         p.trunk_top_radius_m = h ? 0.18f : 0.03f;
         p.ring_depth_m = h ? 0.014f : 0.0075f;
       }},
      {"trunk_top_radius_m",
       [](flora::Palm& p, bool h) { p.trunk_top_radius_m = h ? p.trunk_base_radius_m : 0.06f; }},
      {"lean_deg", [](flora::Palm& p, bool h) { p.lean_deg = h ? 40.0f : 0.0f; }},
      {"curve", [](flora::Palm& p, bool h) { p.curve = h ? 1.0f : 0.0f; }},
      {"ring_spacing_m", [](flora::Palm& p, bool h) { p.ring_spacing_m = h ? 1.0f : 0.04f; }},
      {"ring_depth_m", [](flora::Palm& p, bool h) { p.ring_depth_m = h ? 0.045f : 0.0f; }},
      {"frond_count", [](flora::Palm& p, bool h) { p.frond_count = h ? 64u : 1u; }},
      {"frond_length_m", [](flora::Palm& p, bool h) { p.frond_length_m = h ? 8.0f : 0.5f; }},
      {"frond_droop", [](flora::Palm& p, bool h) { p.frond_droop = h ? 1.0f : 0.0f; }},
      {"frond_spread_deg", [](flora::Palm& p, bool h) { p.frond_spread_deg = h ? 120.0f : 5.0f; }},
      {"leaflet_count", [](flora::Palm& p, bool h) { p.leaflet_count = h ? 160u : 2u; }},
      {"leaflet_width_m", [](flora::Palm& p, bool h) { p.leaflet_width_m = h ? 0.12f : 0.005f; }},
      {"leaflet_angle_deg",
       [](flora::Palm& p, bool h) { p.leaflet_angle_deg = h ? 85.0f : 10.0f; }},
      {"dead_skirt_fronds", [](flora::Palm& p, bool h) { p.dead_skirt_fronds = h ? 64u : 0u; }},
      {"triangle_budget", [](flora::Palm& p, bool h) { p.triangle_budget = h ? 2000000u : 2000u; }},
  };
  for (const Case& c : cases) {
    for (const bool high : {false, true}) {
      flora::Palm p = default_palm();
      c.set(p, high);
      CAPTURE(c.name);
      CAPTURE(high);
      std::string error;
      REQUIRE_MESSAGE(flora::palm_in_range(p, error), error);
      flora::PalmReport report;
      const assets::MeshData mesh = solve(p, &report);
      check_valid(p, mesh);
    }
  }
  // Everything at its most expensive at once, on the smallest budget: the ladder's bottom rung.
  flora::Palm worst = default_palm();
  worst.height_m = 30.0f;
  worst.ring_spacing_m = 0.04f;
  worst.frond_count = 64;
  worst.leaflet_count = 160;
  worst.dead_skirt_fronds = 64;
  worst.triangle_budget = 2000;
  flora::PalmReport report;
  const assets::MeshData mesh = solve(worst, &report);
  check_valid(worst, mesh);
  MESSAGE("worst case on 2,000 triangles: " << report.triangles << " after " << report.steps
                                            << " steps");
}

TEST_CASE("flora: a parameter out of range is refused with its name") {
  flora::Palm p = default_palm();
  p.height_m = 0.5f;
  std::string error;
  CHECK_FALSE(flora::palm_in_range(p, error));
  CHECK(error.find("height_m") != std::string::npos);
  p = default_palm();
  p.leaflet_angle_deg = std::nanf("");
  CHECK_FALSE(flora::palm_in_range(p, error));
  CHECK(error.find("leaflet_angle_deg") != std::string::npos);
  assets::MeshData mesh;
  CHECK_FALSE(flora::solve_palm(p, mesh, nullptr, &error));
}

// The default palm's container, as `engine-content flora` and the renderer build it: three
// primitives with their own cone settings, the identity, the generator section; and its content
// hash pinned beside the mesh's, since the cluster build is the other half of what a palm's bytes
// are and is held to the same rule (geometry.md, "The same bytes from every toolchain").
TEST_CASE(
    "flora: the default palm's container has its parts, its identity and its committed bytes") {
  flora::FloraFile file;
  geometry::ClusterFileData data;
  flora::PalmReport report;
  std::string error;
  const geometry::ClusterLodOptions lod;
  const u32 page_bytes = geometry::ClusterPagesOptions{}.page_bytes;
  REQUIRE_MESSAGE(flora::build_flora_container(file, lod, page_bytes, data, &report, error), error);
  CHECK(data.mesh.leaf_triangle_count == report.triangles);
  REQUIRE(data.materials.size() == 3);
  CHECK(data.cluster_material.size() == data.mesh.mesh.clusters.size());
  CHECK_FALSE(data.pages.pages.empty());
  CHECK(data.mesh.level_cluster_counts.size() > 3);
  // The trunk's clusters carry cones; a frond's never do (k_cone_none is a cutoff of 1).
  u32 coned[3] = {0, 0, 0};
  for (u32 c = 0; c < data.mesh.mesh.clusters.size(); ++c) {
    const geometry::NormalCone cone = geometry::decode_cone(data.mesh.mesh.clusters[c].cone);
    if (cone.cutoff < 1.0f) ++coned[data.cluster_material[c]];
  }
  CHECK(coned[0] > 0);
  CHECK(coned[1] == 0);
  CHECK(coned[2] == 0);
  CHECK(data.source_hash == flora::flora_source_hash(file));
  CHECK(data.build_key == flora::flora_build_key(data.source_hash, lod, page_bytes));
  CHECK(data.generator == "flora");
  CHECK(data.generator_kind == "date_palm");
  CHECK(data.generator_params == flora::canonical_flora_json(file));
  MESSAGE("default palm: " << data.mesh.mesh.clusters.size() << " clusters, "
                           << data.mesh.level_cluster_counts.size() << " levels, "
                           << data.pages.pages.size() << " pages");

  // Written and read back, it is the same container, generator section and all.
  const u64 hash = geometry::cluster_file_hash(data);
  char hex[32];
  std::snprintf(hex, sizeof(hex), "0x%016llxull", static_cast<unsigned long long>(hash));
  constexpr u64 k_golden = 0xab870afd905f4acdull;
  CHECK_MESSAGE(hash == k_golden, "the default palm's container hashes to " << hex);

  // The canonical form is the parameters, defaults filled in, whatever the file spelled.
  flora::FloraFile spelled;
  REQUIRE_MESSAGE(flora::read_flora_file("{ \"palm\" : { \"seed\" : 1 } }", spelled, error), error);
  CHECK(flora::canonical_flora_json(spelled) == flora::canonical_flora_json(file));
  CHECK(flora::flora_source_hash(spelled) == flora::flora_source_hash(file));
  CHECK_FALSE(flora::read_flora_file("{\"species\":\"oak\"}", spelled, error));
  CHECK(error.find("oak") != std::string::npos);
  CHECK_FALSE(flora::read_flora_file("{\"palm\":{\"frond_count\":0}}", spelled, error));
  CHECK(error.find("frond_count") != std::string::npos);
}

// The default palm's mesh, pinned (flora.md, "The same palm from every toolchain"): the solver's
// arithmetic is IEEE f64 with contraction off and the deterministic sines, so this number is the
// same on MSVC, GCC and Clang at either baseline. A change to the solver that moves it bumps
// `k_flora_version` and the number in the same commit.
TEST_CASE("flora: the default palm is the committed bytes on every toolchain") {
  const u64 got = mesh_hash(solve(default_palm()));
  constexpr u64 k_golden = 0x60d8345e83d0a6f1ull;
  char hex[32];
  std::snprintf(hex, sizeof(hex), "0x%016llxull", static_cast<unsigned long long>(got));
  CHECK_MESSAGE(got == k_golden, "the default palm's mesh hashes to " << hex);
}

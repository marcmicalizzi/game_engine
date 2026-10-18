// The date palm's solver (docs/subsystems/flora.md): shape, ranges, budget and determinism.
#include <core/hash/hash.h>
#include <domain/flora/flora.h>
#include <domain/flora/palm.h>

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
  CHECK(std::abs(base - p.trunk_base_radius_m) <= 0.01 * p.trunk_base_radius_m);
  CHECK(std::abs(top - p.trunk_top_radius_m) <= 0.01 * p.trunk_top_radius_m);

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
  const f64 r0 = p.trunk_base_radius_m;
  const f64 r1 = p.trunk_top_radius_m;
  const f64 column = 3.14159265358979 * p.height_m * (r0 * r0 + r0 * r1 + r1 * r1) / 3.0;
  CHECK(volume > 0.85 * column);
  CHECK(volume < 1.05 * column);
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

// The default palm's mesh, pinned (flora.md, "The same palm from every toolchain"): the solver's
// arithmetic is IEEE f64 with contraction off and the deterministic sines, so this number is the
// same on MSVC, GCC and Clang at either baseline. A change to the solver that moves it bumps
// `k_flora_version` and the number in the same commit.
TEST_CASE("flora: the default palm is the committed bytes on every toolchain") {
  const u64 got = mesh_hash(solve(default_palm()));
  constexpr u64 k_golden = 0x2031f9ed8096f6edull;
  char hex[32];
  std::snprintf(hex, sizeof(hex), "0x%016llxull", static_cast<unsigned long long>(got));
  CHECK_MESSAGE(got == k_golden, "the default palm's mesh hashes to " << hex);
}

// The caves capability (caves.h; docs/subsystems/caves.md; roadmap G134): a passage from a 6 m hole
// in and down 40 m with a bend — its shell two-manifold with the collar its one open edge, wound to
// face inward, the collar on the hole's rim, its profiles within the grounding's ranges, a ray down
// inside it finding the floor, its speleothems closed and within their budget — the same bytes
// every time, and the recipe's refusals.
#include <core/hash/hash.h>
#include <domain/caves/caves.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

using namespace engine;
using namespace engine::caves;

namespace {

// The test scene's passage: a 3 m shaft, then in along +x and down, a bend to +z, 40 m down in all.
Passage test_passage() {
  Passage p;
  p.format = std::string(k_passage_format);
  p.seed = 7;
  p.entrance_radius_m = 3.0f;
  p.shaft_m = 4.0f;
  p.points = {Vec3{6.0f, -8.0f, 0.0f}, Vec3{16.0f, -16.0f, 1.0f}, Vec3{24.0f, -26.0f, 8.0f},
              Vec3{26.0f, -34.0f, 18.0f}, Vec3{27.0f, -40.0f, 28.0f}};
  PassageSegment tube;
  tube.profile = PassageProfile::Phreatic;
  tube.width_m = 4.0f;
  PassageSegment keyhole;
  keyhole.profile = PassageProfile::Keyhole;
  keyhole.width_m = 3.5f;
  PassageSegment canyon;
  canyon.profile = PassageProfile::Vadose;
  canyon.width_m = 1.5f;
  p.segments = {tube, tube, keyhole, keyhole, canyon};
  p.speleothems = Speleothems{};
  return p;
}

u64 mesh_hash(const assets::MeshData& m) {
  u64 h = hash_bytes(m.positions.data(), m.positions.size() * sizeof(Vec3));
  h = hash_combine(h, hash_bytes(m.normals.data(), m.normals.size() * sizeof(Vec3)));
  h = hash_combine(h, hash_bytes(m.uvs.data(), m.uvs.size() * sizeof(Vec2)));
  return hash_combine(h, hash_bytes(m.indices.data(), m.indices.size() * sizeof(u32)));
}

// Welds by exact position, as the rocks' checks do (domain/rocks/tests/rock_checks.h).
std::vector<u32> weld(const assets::MeshData& m) {
  std::map<std::tuple<f32, f32, f32>, u32> ids;
  std::vector<u32> out(m.positions.size());
  for (u32 v = 0; v < m.positions.size(); ++v) {
    const Vec3 p = m.positions[v];
    out[v] =
        ids.emplace(std::make_tuple(p.x, p.y, p.z), static_cast<u32>(ids.size())).first->second;
  }
  return out;
}

// Ray (o, d) against triangle (a, b, c): the distance, or a negative number for a miss.
f32 ray_hit(Vec3 o, Vec3 d, Vec3 a, Vec3 b, Vec3 c) {
  const Vec3 e1 = b - a, e2 = c - a;
  const Vec3 p = cross(d, e2);
  const f32 det = dot(e1, p);
  if (std::fabs(det) < 1e-12f) return -1.0f;
  const Vec3 t = o - a;
  const f32 u = dot(t, p) / det;
  if (u < 0.0f || u > 1.0f) return -1.0f;
  const Vec3 q = cross(t, e1);
  const f32 v = dot(d, q) / det;
  if (v < 0.0f || u + v > 1.0f) return -1.0f;
  return dot(e2, q) / det;
}

}  // namespace

TEST_CASE("caves: a passage's shell is two-manifold, open only at the collar, wound inward") {
  const Passage passage = test_passage();
  assets::MeshData mesh;
  PassageReport report;
  std::string error;
  REQUIRE_MESSAGE(solve_passage(passage, mesh, &report, &error), error);
  MESSAGE("the passage: " << report.rings << " rings of " << report.ring_vertices << ", "
                          << report.shell_triangles << " shell triangles, " << report.length_m
                          << " m; " << report.drips << " drips: " << report.stalactites
                          << " stalactites, " << report.straws << " straws, " << report.stalagmites
                          << " stalagmites, " << report.columns << " columns, "
                          << report.speleothem_triangles << " triangles, "
                          << report.drips_over_budget << " over the budget");
  REQUIRE(mesh.normals.size() == mesh.positions.size());
  REQUIRE(mesh.uvs.size() == mesh.positions.size());
  REQUIRE(mesh.primitives.size() == 1);
  REQUIRE(mesh.materials.size() == 1);
  for (const Vec3& p : mesh.positions)
    REQUIRE((std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)));
  CHECK(report.length_m > 40.0f);

  // Directed edges, welded: each has its reverse exactly once, except the collar's, which are the
  // one open edge loop (the hole's rim is the ground's).
  const std::vector<u32> w = weld(mesh);
  std::map<std::pair<u32, u32>, u32> directed;
  for (u32 i = 0; i < mesh.indices.size(); i += 3) {
    const u32 t[3] = {w[mesh.indices[i]], w[mesh.indices[i + 1]], w[mesh.indices[i + 2]]};
    REQUIRE(t[0] != t[1]);
    REQUIRE(t[1] != t[2]);
    REQUIRE(t[0] != t[2]);
    for (u32 e = 0; e < 3; ++e)
      ++directed[{t[e], t[(e + 1) % 3]}];
  }
  u32 open = 0, twice = 0;
  for (const auto& [edge, count] : directed) {
    twice += count > 1 ? 1u : 0u;
    if (directed.find({edge.second, edge.first}) == directed.end()) {
      ++open;
      // An open edge is on the collar: both ends at the ground, on the rim.
      for (const u32 end : {edge.first, edge.second}) {
        bool on_collar = false;
        for (u32 v = report.collar_first; v < report.collar_first + report.collar_count; ++v)
          on_collar = on_collar || w[v] == end;
        CHECK(on_collar);
      }
    }
  }
  CHECK(twice == 0u);
  CHECK(open == report.ring_vertices);

  // Inward: the shell's signed volume about the entrance — on the collar's plane, so the collar's
  // closing fan adds nothing — is negative, and about the volume of a tube that long.
  f64 volume = 0.0;
  for (u32 i = 0; i < report.shell_triangles * 3; i += 3) {
    const Vec3 a = mesh.positions[mesh.indices[i]], b = mesh.positions[mesh.indices[i + 1]],
               c = mesh.positions[mesh.indices[i + 2]];
    volume += static_cast<f64>(dot(a, cross(b, c))) / 6.0;
  }
  MESSAGE("the shell's signed volume: " << volume << " m^3");
  CHECK(volume < -150.0);
  CHECK(volume > -2000.0);

  // The collar: on the hole's rim, at the ground, to a millimetre.
  f64 worst = 0.0;
  for (u32 v = report.collar_first; v < report.collar_first + report.collar_count; ++v) {
    const Vec3 p = mesh.positions[v];
    const f64 x = static_cast<f64>(p.x), z = static_cast<f64>(p.z);
    const f64 r = std::sqrt(x * x + z * z);
    worst = std::max({worst, std::fabs(r - 3.0), std::fabs(static_cast<f64>(p.y))});
  }
  MESSAGE("the collar stands off the hole's rim by " << worst * 1000.0 << " mm at most");
  CHECK(worst < 0.001);

  // A walker's query inside: straight down from the centreline at mid-passage meets the floor —
  // a shell triangle — below, within the passage's height.
  const Vec3 above{16.0f, -16.0f + 0.2f, 1.0f};
  f32 nearest = 1e30f;
  for (u32 i = 0; i < report.shell_triangles * 3; i += 3) {
    const f32 t = ray_hit(above, Vec3{0.0f, -1.0f, 0.0f}, mesh.positions[mesh.indices[i]],
                          mesh.positions[mesh.indices[i + 1]], mesh.positions[mesh.indices[i + 2]]);
    if (t > 0.0f) nearest = std::min(nearest, t);
  }
  MESSAGE("the floor under the centreline at (16, -16, 1): " << nearest << " m down");
  CHECK(nearest > 0.5f);
  CHECK(nearest < 4.0f);

  // Speleothems: there are some, and within their budget.
  CHECK(report.stalactites > 0u);
  CHECK(report.straws > 0u);
  CHECK(report.stalagmites > 0u);
  CHECK(report.speleothem_triangles <= passage.speleothems->triangle_budget);
}

TEST_CASE("caves: the same recipe is the same bytes; another seed is another passage") {
  const Passage passage = test_passage();
  assets::MeshData a, b;
  REQUIRE(solve_passage(passage, a, nullptr, nullptr));
  REQUIRE(solve_passage(passage, b, nullptr, nullptr));
  CHECK(mesh_hash(a) == mesh_hash(b));
  // Pinned: the solver is products, sums, square roots and an integer hash, so every toolchain
  // and instruction set reproduces it (the containers' run checks).
  MESSAGE("the test passage's mesh hash: " << mesh_hash(a));
  CHECK(mesh_hash(a) == 6604127554548133638ull);  // MSVC x64, v3, 2026-11-07
  Passage other = passage;
  other.seed = 8;
  assets::MeshData c;
  REQUIRE(solve_passage(other, c, nullptr, nullptr));
  CHECK(mesh_hash(c) != mesh_hash(a));
  CHECK(passage_source_hash(other) != passage_source_hash(passage));
  // The budget: none at 0, and the bare shell's triangles unchanged by it.
  Passage bare = passage;
  bare.speleothems->triangle_budget = 0;
  assets::MeshData d;
  PassageReport report;
  REQUIRE(solve_passage(bare, d, &report, nullptr));
  CHECK(report.speleothem_triangles == 0u);
  CHECK(report.drips_over_budget == report.drips);
  CHECK(report.drips > 0u);
}

TEST_CASE("caves: the profiles keep the grounding's proportions; the stalactite's 3/4 law") {
  constexpr u32 n = 32;
  const auto extent = [&](PassageProfile kind, f32 w, f32 h, f32& width, f32& height) {
    f32 x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (u32 k = 0; k < n; ++k) {
      const Vec2 p = profile_point(kind, w, h, k, n);
      x0 = std::min(x0, p.x);
      x1 = std::max(x1, p.x);
      y0 = std::min(y0, p.y);
      y1 = std::max(y1, p.y);
    }
    width = x1 - x0;
    height = y1 - y0;
  };
  PassageSegment s;
  f32 width = 0.0f, height = 0.0f;
  s.profile = PassageProfile::Phreatic;
  s.width_m = 4.0f;
  extent(s.profile, s.width_m, segment_height(s), width, height);
  CHECK(width == doctest::Approx(4.0f).epsilon(1e-4));
  CHECK(width / height == doctest::Approx(2.0f).epsilon(1e-4));  // a tube twice as wide as high
  s.profile = PassageProfile::Vadose;
  s.width_m = 1.5f;
  extent(s.profile, s.width_m, segment_height(s), width, height);
  CHECK(height / width == doctest::Approx(3.0f).epsilon(1e-4));  // a canyon three times as tall
  s.profile = PassageProfile::Keyhole;
  s.width_m = 3.5f;
  extent(s.profile, s.width_m, segment_height(s), width, height);
  CHECK(width == doctest::Approx(3.5f).epsilon(1e-4));
  CHECK(height == doctest::Approx(3.5f * 0.5f * 1.7f).epsilon(1e-3));  // half the tube + 1.2 below
  // The canyon under the keyhole's tube is a third of its width.
  f32 slot = 0.0f;
  for (u32 k = 3 * n / 4 - 1; k <= 3 * n / 4 + 1; ++k)
    slot = std::max(slot, std::fabs(profile_point(s.profile, 3.5f, segment_height(s), k, n).x));
  CHECK(slot < 3.5f * 0.35f * 0.5f + 1e-4f);

  // r(along) = tip + (base - tip) (along / length)^(3/4): the tip, the base, and a sixteenth of
  // the way an eighth of the way out.
  CHECK(stalactite_radius(0.0f, 1.0f, 0.12f, 0.0025f) == 0.0025f);
  CHECK(stalactite_radius(1.0f, 1.0f, 0.12f, 0.0025f) == doctest::Approx(0.12f));
  CHECK(stalactite_radius(1.0f / 16.0f, 1.0f, 0.12f, 0.0025f) ==
        doctest::Approx(0.0025f + (0.12f - 0.0025f) / 8.0f).epsilon(1e-5));
}

TEST_CASE("caves: a recipe outside the grounding's ranges is refused with its field") {
  std::string error;
  Passage p = test_passage();
  CHECK(passage_in_range(p, error));
  p.segments[0].width_m = 20.0f;
  CHECK_FALSE(passage_in_range(p, error));
  CHECK(error.find("width_m") != std::string::npos);
  p = test_passage();
  p.segments[0].height_m = 4.0f;  // a "tube" as high as wide
  CHECK_FALSE(passage_in_range(p, error));
  CHECK(error.find("height_m") != std::string::npos);
  p = test_passage();
  p.format = "engine.caves.cave";
  CHECK_FALSE(passage_in_range(p, error));
  p = test_passage();
  p.collar_heights_m = {0.0f, 1.0f};
  CHECK_FALSE(passage_in_range(p, error));
  CHECK(error.find("collar_heights_m") != std::string::npos);
  p = test_passage();
  p.speleothems->straw_radius_m = 0.01f;
  CHECK_FALSE(passage_in_range(p, error));
  CHECK(error.find("straw_radius_m") != std::string::npos);
  p = test_passage();
  p.points.clear();
  CHECK_FALSE(passage_in_range(p, error));
  Passage read;
  CHECK_FALSE(read_passage("{\"format\": \"engine.caves.passage\", \"nonsense\": 1}", read, error));
}

TEST_CASE("caves: the passage builds into a container, its collar locked through the DAG") {
  geometry::ClusterFileData data;
  PassageReport report;
  std::string error;
  REQUIRE_MESSAGE(build_passage_container(test_passage(), geometry::ClusterLodOptions{}, 0, data,
                                          &report, error),
                  error);
  CHECK(data.mesh.mesh.clusters.size() > 10u);
  CHECK(data.generator == "cave");
  CHECK(data.source_hash == passage_source_hash(test_passage()));
  MESSAGE("the passage's DAG: " << data.mesh.mesh.clusters.size() << " clusters, "
                                << data.mesh.leaf_triangle_count << " leaf triangles");
}

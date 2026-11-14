// The far form of a rigid mesh (far_form.h; geometry.md, "The far form"; G198).
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/far_form.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>

using namespace engine;
using namespace engine::geometry;

namespace {

// A 16 m sheet of 0.25 m quads with a gentle relief, and loose cubes on it: `small` of 5 cm (a
// diagonal under the first level's error), `mid` of 15 cm (over the first, under the third) and
// `large` of 60 cm (over every level's). What a talus patch is in shape, with known pieces.
struct Fixture {
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<Vec2> uvs;
  Vector<u32> indices;
  u32 border = 0;  // the sheet's open-border vertices, the first of `positions` that are
};

void cube(Fixture& f, Vec3 at, f32 side) {
  const u32 base = f.positions.size();
  for (u32 k = 0; k < 8; ++k) {
    const Vec3 p{at.x + ((k & 1u) ? side : 0.0f), at.y + ((k & 2u) ? side : 0.0f),
                 at.z + ((k & 4u) ? side : 0.0f)};
    f.positions.push_back(p);
    f.uvs.push_back(Vec2{p.x / 16.0f, p.z / 16.0f});
  }
  // Counter-clockwise seen from outside.
  const u32 faces[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                            {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
  for (const auto& t : faces)
    for (u32 k = 0; k < 3; ++k)
      f.indices.push_back(base + t[k]);
}

f32 relief(f32 x, f32 z) { return 0.3f * std::sin(x * 0.7f) * std::cos(z * 0.5f); }

Fixture talus() {
  Fixture f;
  constexpr u32 n = 65;  // 64 quads of 0.25 m
  // The border first, so `border` counts it; then the interior; the grid's index of each.
  Vector<u32> at(n * n);
  for (u32 pass = 0; pass < 2; ++pass)
    for (u32 z = 0; z < n; ++z)
      for (u32 x = 0; x < n; ++x) {
        const bool edge = x == 0 || z == 0 || x == n - 1 || z == n - 1;
        if (edge != (pass == 0)) continue;
        const f32 px = static_cast<f32>(x) * 0.25f, pz = static_cast<f32>(z) * 0.25f;
        at[z * n + x] = f.positions.size();
        f.positions.push_back(Vec3{px, relief(px, pz), pz});
        f.uvs.push_back(Vec2{px / 16.0f, pz / 16.0f});
        if (pass == 0) ++f.border;
      }
  for (u32 z = 0; z + 1 < n; ++z)
    for (u32 x = 0; x + 1 < n; ++x) {
      const u32 a = at[z * n + x], b = at[z * n + x + 1], c = at[(z + 1) * n + x],
                d = at[(z + 1) * n + x + 1];
      // Up-facing, counter-clockwise from above (y up, x right, z toward the viewer).
      f.indices.push_back(a);
      f.indices.push_back(c);
      f.indices.push_back(b);
      f.indices.push_back(b);
      f.indices.push_back(c);
      f.indices.push_back(d);
    }
  for (u32 k = 0; k < 6; ++k) {
    const f32 x = 1.0f + 2.5f * static_cast<f32>(k);
    cube(f, Vec3{x, 1.0f, 2.0f}, 0.05f);
    cube(f, Vec3{x, 1.0f, 6.0f}, 0.15f);
    cube(f, Vec3{x, 1.0f, 10.0f}, 0.6f);
  }
  compute_vertex_normals(std::span<const Vec3>(f.positions.data(), f.positions.size()),
                         std::span<const u32>(f.indices.data(), f.indices.size()), f.normals);
  return f;
}

ClusterFileData build(const Fixture& f) {
  ClusterFileData data;
  AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(f.normals.data(), f.normals.size());
  attributes.uvs = std::span<const Vec2>(f.uvs.data(), f.uvs.size());
  std::string error;
  REQUIRE_MESSAGE(build_cluster_lod(std::span<const Vec3>(f.positions.data(), f.positions.size()),
                                    std::span<const u32>(f.indices.data(), f.indices.size()),
                                    ClusterLodOptions{}, data.mesh, &error, attributes),
                  error);
  data.cluster_material.assign(data.mesh.mesh.clusters.size(), 0u);
  data.materials.push_back(ClusterFileMaterial{});
  return data;
}

}  // namespace

TEST_CASE(
    "far form: small pieces leave by level, the rest simplifies within its error, and the "
    "border stays") {
  const Fixture f = talus();
  ClusterFileData data = build(f);
  REQUIRE(far_form_applies(data));
  std::string error;
  REQUIRE_MESSAGE(ensure_far_form(data, &error), error);
  const FarForm& form = data.far_form;
  REQUIRE(form.levels.size() == k_far_form_levels);
  CHECK(form.header.pieces == 1u + 18u);
  CHECK(form.header.source_triangles == f.indices.size() / 3);
  const f32 e0 = form.header.radius / k_far_form_radius_px;
  MESSAGE("radius ", form.header.radius, " m, e0 ", e0, " m, source ", form.header.source_triangles,
          " triangles");
  // The pieces: the 5 cm cubes (diagonal 8.7 cm) are under e0 (8.9 cm); the 15 cm cubes (26 cm)
  // over 2 e0 and under 4 e0; the 60 cm ones over all three.
  REQUIRE(e0 > 0.0866f);
  REQUIRE(e0 * 2.0f < 0.2598f);
  REQUIRE(e0 * 4.0f > 0.2598f);
  const u32 dropped[3] = {6, 6, 12};
  u32 before = form.header.source_triangles;
  for (u32 l = 0; l < form.levels.size(); ++l) {
    const FarFormLevel& level = form.levels[l];
    MESSAGE("level ", l, ": error ", level.error_m, " m (simplifier ", level.simplify_m,
            ", largest piece left ", level.dropped_m, "), ", level.triangles, " triangles, ",
            level.kept_pieces, " pieces kept, ", level.dropped_pieces, " left out");
    CHECK(level.dropped_pieces == dropped[l]);
    CHECK(level.kept_pieces + level.dropped_pieces == form.header.pieces);
    // A level's steps add their errors, which a float sum may carry an ulp past.
    CHECK(FarForm::level_error(level) <= level.error_m * 1.000001f);
    CHECK(level.triangles < before);
    before = level.triangles;
  }
  CHECK(form.levels[0].triangles * 4 < form.header.source_triangles);
  // The sheet's open border is where a neighbour meets it: every border point is still a vertex
  // of every level.
  for (u32 l = 0; l < form.levels.size(); ++l) {
    Vector<u8> used(form.vertices.size(), 0);
    for (u32 p = form.levels[l].first_part;
         p < form.levels[l].first_part + form.levels[l].part_count; ++p)
      for (u32 i = 0; i < form.parts[p].index_count; ++i)
        used[form.indices[form.parts[p].first_index + i]] = 1;
    u32 found = 0;
    for (u32 b = 0; b < f.border; ++b) {
      const Vec3 want = f.positions[b];
      for (u32 v = 0; v < form.vertices.size(); ++v)
        if (used[v] != 0 && std::memcmp(&form.vertices[v].position, &want, sizeof(Vec3)) == 0) {
          ++found;
          break;
        }
    }
    CHECK(found == f.border);
  }
  // Where each level may stand in at the default camera at 1080p and one pixel.
  for (u32 l = 0; l < form.levels.size(); ++l)
    MESSAGE("level ", l, " from ", far_form_distance(form.levels[l], 1037.33f, 1.0f), " m");
}

TEST_CASE("far form: carried in the container, read back whole, the same bytes from every bake") {
  const Fixture f = talus();
  ClusterFileData data = build(f);
  REQUIRE(ensure_far_form(data));
  ClusterFileData again = build(f);
  REQUIRE(ensure_far_form(again));
  CHECK(far_form_hash(data.far_form) == far_form_hash(again.far_form));
  MESSAGE("far form hash ", far_form_hash(data.far_form));
  // Taken on MSVC 14.51, 2026-11-13, at k_far_form_version 1; GCC and Clang at v3 and GCC at v2
  // reproduce it (the containers' runs, G198's hand-back).
  CHECK(far_form_hash(data.far_form) == 8751553704110674218ull);
  test::TempDir scratch;
  const std::string path = scratch.path() + "/talus.clusters";
  std::string error;
  REQUIRE_MESSAGE(write_cluster_file(path, data, &error), error);
  ClusterFileData read;
  REQUIRE_MESSAGE(read_cluster_file(path, read, &error), error);
  CHECK(far_form_hash(read.far_form) == far_form_hash(data.far_form));
  // A container with none reads with none, and a reader that never knew the kinds skips them.
  ClusterFileData plain = build(f);
  REQUIRE(write_cluster_file(path, plain, &error));
  REQUIRE(read_cluster_file(path, read, &error));
  CHECK(read.far_form.empty());
  // A mesh that deforms or carries its own far form gets none.
  ClusterFileData swaying = build(f);
  swaying.motion_parts.push_back(ClusterFileMotionPart{});
  CHECK_FALSE(far_form_applies(swaying));
  CHECK_FALSE(ensure_far_form(swaying));
  CHECK(swaying.far_form.empty());
  ClusterFileData tree = build(f);
  tree.impostors.push_back(ClusterFileImpostor{});
  CHECK_FALSE(far_form_applies(tree));
}

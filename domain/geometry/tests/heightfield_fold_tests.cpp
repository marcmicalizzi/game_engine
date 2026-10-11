// Heightfield levels never fold (geometry.md, "Heightfield levels never fold"; roadmap F17).
#include <domain/geometry/cluster_lod.h>

#include <doctest/doctest.h>

#include <cstring>
#include <string>

using namespace engine;
using namespace engine::geometry;

namespace {

struct Field {
  Vector<Vec3> positions;
  Vector<Vec3> normals;
  Vector<Vec2> uvs;
  Vector<u32> indices;
};

// Integer hash noise in [-1, 1] micrometre units scaled by `amp_um`.
i64 noise_um(i64 i, i64 j, i64 amp_um) {
  u64 h = static_cast<u64>(i) * 0x9e3779b97f4a7c15ull ^ static_cast<u64>(j) * 0xc2b2ae3d27d4eb4full;
  h ^= h >> 29;
  h *= 0xbf58476d1ce4e5b9ull;
  h ^= h >> 32;
  return static_cast<i64>(h % static_cast<u64>(2 * amp_um + 1)) - amp_um;
}

// An asymmetric dune profile over one wavelength `l` (cells): a stoss slope up over `stoss` of it,
// a slip face down over the rest. Height in micrometres.
i64 dune_um(i64 t, i64 l, i64 stoss, i64 h_um) {
  i64 u = t % l;
  if (u < 0) u += l;
  if (u < stoss) return h_um * u / stoss;
  return h_um * (l - u) / (l - stoss);
}

// A tile of the erg's kind on a 1 m lattice, n x n vertices: a transverse dune whose crest bends,
// waves across it, and a grain of noise, all in integers so every coordinate is the same float on
// every toolchain. Wound counter-clockwise seen from +y, as the terrain builders wind theirs.
void make_erg_tile(u32 n, i64 noise_amp_um, Field& out) {
  out = Field{};
  const i64 half = static_cast<i64>(n / 2);
  Vector<i64> h(n * n);
  for (u32 j = 0; j < n; ++j) {
    for (u32 i = 0; i < n; ++i) {
      const i64 x = static_cast<i64>(i), z = static_cast<i64>(j);
      // The crest bends along z: a triangle wave of 14 cells over 120.
      const i64 bend = 14 - std::abs((z % 120) - 60) * 28 / 60;
      i64 y = dune_um(x + bend, 90, 62, 22'000'000);
      y += dune_um(x + z, 11, 7, 600'000);
      y += dune_um(x * 2 - z, 23, 15, 1'500'000);
      y += noise_um(x, z, noise_amp_um);
      h[j * n + i] = y;
    }
  }
  for (u32 j = 0; j < n; ++j) {
    for (u32 i = 0; i < n; ++i) {
      const f32 x = static_cast<f32>(static_cast<i64>(i) - half);
      const f32 z = static_cast<f32>(static_cast<i64>(j) - half);
      out.positions.push_back(Vec3{x, static_cast<f32>(static_cast<f64>(h[j * n + i]) / 1e6), z});
      const u32 i0 = i > 0 ? i - 1 : i, i1 = i + 1 < n ? i + 1 : i;
      const u32 j0 = j > 0 ? j - 1 : j, j1 = j + 1 < n ? j + 1 : j;
      const f64 dx = static_cast<f64>(h[j * n + i1] - h[j * n + i0]) / 1e6 / (i1 - i0);
      const f64 dz = static_cast<f64>(h[j1 * n + i] - h[j0 * n + i]) / 1e6 / (j1 - j0);
      const Vec3 normal{static_cast<f32>(-dx), 1.0f, static_cast<f32>(-dz)};
      out.normals.push_back(normalize(normal));
      out.uvs.push_back(Vec2{static_cast<f32>(i) / 64.0f, static_cast<f32>(j) / 64.0f});
    }
  }
  for (u32 j = 0; j + 1 < n; ++j) {
    for (u32 i = 0; i + 1 < n; ++i) {
      const u32 a = j * n + i, b = a + 1, c = a + n, d = c + 1;
      for (const u32 v : {a, c, b, b, c, d})
        out.indices.push_back(v);
    }
  }
}

LodFoldLevel total_of(const Vector<LodFoldLevel>& levels) {
  LodFoldLevel total;
  for (const LodFoldLevel& l : levels) {
    total.clusters += l.clusters;
    total.triangles += l.triangles;
    total.upside_down += l.upside_down;
    total.overlapping += l.overlapping;
    total.folded_clusters += l.folded_clusters;
  }
  return total;
}

bool build(const Field& field, const ClusterLodOptions& options, ClusterLodMesh& lod) {
  AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(field.normals.data(), field.normals.size());
  attributes.uvs = std::span<const Vec2>(field.uvs.data(), field.uvs.size());
  std::string error;
  const bool ok =
      build_cluster_lod(field.positions, field.indices, options, lod, &error, attributes);
  if (!ok) MESSAGE(error);
  return ok;
}

bool same_bytes(const ClusterLodMesh& a, const ClusterLodMesh& b) {
  const auto equal = [](const auto& x, const auto& y) {
    if (x.size() != y.size()) return false;
    return x.size() == 0 || std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])) == 0;
  };
  return equal(a.mesh.clusters, b.mesh.clusters) && equal(a.lod, b.lod) &&
         equal(a.mesh.vertices, b.mesh.vertices) && equal(a.mesh.triangles, b.mesh.triangles) &&
         equal(a.mesh.vertex_source, b.mesh.vertex_source) &&
         equal(a.mesh.quantized, b.mesh.quantized) && equal(a.mesh.attributes, b.mesh.attributes) &&
         equal(a.level_cluster_counts, b.level_cluster_counts) && a.group_count == b.group_count;
}

constexpr Vec3 k_up{0.0f, 1.0f, 0.0f};

}  // namespace

// The fixture is a tile of the erg's kind — a slip-faced dune whose crest bends, waves across it
// and a grain of noise, 129 x 129 on a metre lattice — and its plain build folds the way the erg's
// grid did (docs/experiments/shadow-shards-2026-10-09.md, "2026-10-11"): from the second level up
// a group's regular simplification turns triangles over their neighbours, and the sloppy fallback
// never runs. With `up` set the builder refuses every such result, and no level holds a triangle
// facing down or two triangles of one cluster over each other, on the floats or on the grid.
TEST_CASE("heightfield folds: an erg tile's levels fold without the guard and never with it") {
  Field field;
  make_erg_tile(129, 200'000, field);

  ClusterLodMesh plain;
  REQUIRE(build(field, ClusterLodOptions{}, plain));
  Vector<LodFoldLevel> floats, grid;
  REQUIRE(count_lod_folds(plain, k_up, false, floats));
  REQUIRE(count_lod_folds(plain, k_up, true, grid));
  const LodFoldLevel before = total_of(floats);
  const LodFoldLevel before_grid = total_of(grid);
  MESSAGE("without the guard: " << before.upside_down << " upside down and " << before.overlapping
                                << " overlapping in the floats, " << before_grid.upside_down
                                << " and " << before_grid.overlapping << " on the grid, of "
                                << plain.mesh.clusters.size() << " clusters");
  CHECK(floats[0].upside_down == 0);  // the source itself is a heightfield
  CHECK(floats[0].overlapping == 0);
  CHECK(before.upside_down > 0);
  CHECK(before.overlapping > 0);
  CHECK(before_grid.upside_down >= before.upside_down);

  // Who made them: the count-only build is the plain build byte for byte, and says which
  // simplifier's results turned a triangle over.
  ClusterLodOptions count_only;
  count_only.up = k_up;
  count_only.refuse_folds = false;
  ClusterLodMesh counted;
  REQUIRE(build(field, count_only, counted));
  CHECK(same_bytes(plain, counted));
  CHECK(counted.fold_guard.regular_folded > 0);
  CHECK(counted.fold_guard.sloppy == 0);
  CHECK(counted.fold_guard.refused == 0);

  ClusterLodOptions guarded;
  guarded.up = k_up;
  ClusterLodMesh held;
  REQUIRE(build(field, guarded, held));
  std::string error;
  CHECK_MESSAGE(validate_cluster_lod(held, field.indices, &error), error);
  REQUIRE(count_lod_folds(held, k_up, false, floats));
  REQUIRE(count_lod_folds(held, k_up, true, grid));
  REQUIRE(floats.size() == held.level_cluster_counts.size());
  for (u32 level = 0; level < floats.size(); ++level) {
    CAPTURE(level);
    CHECK(floats[level].upside_down == 0);
    CHECK(floats[level].overlapping == 0);
    CHECK(grid[level].upside_down == 0);
    CHECK(grid[level].overlapping == 0);
  }
  CHECK(held.fold_guard.refused > 0);
  CHECK(held.fold_guard.unsimplified == 0);
  // What it costs: a refused result is simplified again with the fold's corners locked, so the
  // level above holds a few more triangles there. Within a few per cent of the plain build.
  MESSAGE("with the guard: " << held.fold_guard.refused << " results refused, "
                             << held.fold_guard.retries << " simplifications again, "
                             << held.mesh.clusters.size() << " clusters, "
                             << held.level_cluster_counts.size() << " levels");
  CHECK(held.level_cluster_counts.size() == plain.level_cluster_counts.size());
  CHECK(held.mesh.clusters.size() * 100 <= plain.mesh.clusters.size() * 103);
  CHECK(held.mesh.clusters.size() * 100 >= plain.mesh.clusters.size() * 97);
}

// A guard that changes a mesh it has nothing to refuse in is a guard nobody can turn on: a gentle
// heightfield, which no simplification folds, builds the same bytes with `up` set and without —
// the property that keeps every terrain that never folded the mesh it was, and the check that the
// guarded build loop is still clusterlod's when meshoptimizer is updated.
TEST_CASE("heightfield folds: a heightfield that never folds builds the same bytes guarded") {
  Field field;
  make_erg_tile(65, 0, field);
  for (Vec3& p : field.positions)
    p.y *= 0.0625f;  // the same dunes a sixteenth as tall: nothing steep enough to fold
  ClusterLodMesh plain;
  REQUIRE(build(field, ClusterLodOptions{}, plain));
  ClusterLodOptions guarded;
  guarded.up = k_up;
  ClusterLodMesh held;
  REQUIRE(build(field, guarded, held));
  REQUIRE(held.fold_guard.groups > 0);
  REQUIRE(held.fold_guard.regular_folded == 0);
  REQUIRE(held.fold_guard.sloppy_folded == 0);
  CHECK(same_bytes(plain, held));
  CHECK(plain.fold_guard.groups == 0);  // a mesh without `up` is not counted at all
}

// The census reads what it is given: a cluster holding one triangle turned over its neighbour is
// one upside down and two overlapping, on the floats and on the grid alike, and a skirt hung
// straight down from an edge is neither.
TEST_CASE("heightfield folds: the census counts a fold, and not a skirt") {
  ClusterLodMesh mesh;
  for (const Vec3 p : {Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 4.0f}, Vec3{4.0f, 0.0f, 0.0f},
                       Vec3{2.0f, 0.5f, 1.0f}, Vec3{4.0f, -1.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}})
    mesh.mesh.vertices.push_back(p);
  // (0, 1, 2) faces up; (2, 1, 3) is its neighbour folded back over it, sharing the edge 1-2;
  // (0, 5, 2) and (2, 5, 4) hang a skirt under the edge 0-2.
  for (const u32 t : {ClusterMesh::pack(0, 1, 2), ClusterMesh::pack(2, 1, 3),
                      ClusterMesh::pack(0, 2, 5), ClusterMesh::pack(2, 4, 5)})
    mesh.mesh.triangles.push_back(t);
  ClusterDesc desc;
  desc.vertex_count = 6;
  desc.triangle_count = 4;
  mesh.mesh.clusters.push_back(desc);
  mesh.lod.push_back(ClusterLodDesc{});
  mesh.level_cluster_counts.push_back(1);
  quantize_positions(mesh.mesh);
  for (const bool on_grid : {false, true}) {
    CAPTURE(on_grid);
    Vector<LodFoldLevel> levels;
    REQUIRE(count_lod_folds(mesh, k_up, on_grid, levels));
    REQUIRE(levels.size() == 1);
    CHECK(levels[0].triangles == 4);
    CHECK(levels[0].upside_down == 1);
    CHECK(levels[0].overlapping == 2);
    CHECK(levels[0].folded_clusters == 1);
  }
}

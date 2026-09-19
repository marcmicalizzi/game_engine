// What the LOD simplifier is given, measured rather than described.
//
// The defect these tests pin: `build_cluster_lod` used to hand meshoptimizer's cluster-LOD
// builder the **positions alone**, while that builder runs the simplifier in *permissive* mode,
// whose documented meaning is "a collapse may cross an attribute discontinuity unless the vertex
// is tagged". With no attributes in the error metric and no tags, a group of clusters spanning
// twenty atlas islands simplifies as though the atlas were continuous, and every triangle over a
// join paints one part of the model with another part's texture. It is invisible on the Khronos
// samples, whose atlases are a handful of large clean islands, and ruinous on the thousands of
// tiny islands an AI mesh generator or a photogrammetry scan produces.
//
// The measurement is `measure_lod_attribute_error`: sample the finest surface, find the closest
// point on a coarse cut, and compare the UVs the two would interpolate, in texels of a 4096
// atlas. It needs no GPU, so it is the gate; the picture tests are the corroboration.

#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/stress_mesh.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::geometry;

namespace {

ShreddedAtlasOptions fixture_options() {
  ShreddedAtlasOptions options;
  options.segments = 96;
  options.rings = 48;
  options.island_side = 4;
  return options;
}

u32 triangles_of(const ClusterLodMesh& mesh, const Vector<u32>& cut) {
  u32 total = 0;
  for (const u32 c : cut)
    total += mesh.mesh.clusters[c].triangle_count;
  return total;
}

// The comparison has to be at the same **triangle budget**, or it compares two different amounts
// of geometry rather than two simplifiers: attribute-aware simplification moves the errors, so
// one threshold is two different cuts. Sweeping the raw threshold geometrically and keeping the
// cut closest to the target is enough, because the cut's triangle count is monotone in the
// threshold (`validate_cluster_lod` asserts that).
u32 cut_near_triangles(const ClusterLodMesh& mesh, u32 target, Vector<u32>& out) {
  f32 best_threshold = 0.0f;
  u32 best_distance = ~u32{0};
  for (u32 step = 0; step <= 64; ++step) {
    const f32 threshold = std::pow(10.0f, -6.0f + 8.0f * static_cast<f32>(step) / 64.0f);
    Vector<u32> cut;
    select_lod_raw(mesh, threshold, cut);
    const u32 triangles = triangles_of(mesh, cut);
    const u32 distance = triangles > target ? triangles - target : target - triangles;
    if (distance < best_distance) {
      best_distance = distance;
      best_threshold = threshold;
    }
  }
  out.clear();
  select_lod_raw(mesh, best_threshold, out);
  return triangles_of(mesh, out);
}

ClusterLodOptions position_only() {
  ClusterLodOptions options;  // what the builder did before 2026-09-19
  options.normal_weight = 0.0f;
  options.uv_weight = 0.0f;
  options.uv_seams = SeamRule::none;
  options.normal_seams = SeamRule::none;
  options.skin_seams = SeamRule::none;
  return options;
}

struct Built {
  ClusterLodMesh mesh;
  AttributeError error;
  u32 cut_triangles = 0;
  u32 cut_clusters = 0;
};

Built build_and_measure(const ShreddedAtlasMesh& source, const ClusterLodOptions& options,
                        u32 target_triangles) {
  Built built;
  AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(source.normals.data(), source.normals.size());
  attributes.uvs = std::span<const Vec2>(source.uvs.data(), source.uvs.size());
  std::string error;
  REQUIRE_MESSAGE(
      build_cluster_lod(source.positions, source.indices, options, built.mesh, &error, attributes),
      error);
  REQUIRE_MESSAGE(validate_cluster_lod(built.mesh, source.indices, &error), error);
  Vector<u32> cut;
  built.cut_triangles = cut_near_triangles(built.mesh, target_triangles, cut);
  built.cut_clusters = cut.size();
  AttributeErrorOptions measure;
  REQUIRE_MESSAGE(measure_lod_attribute_error(built.mesh, cut, measure, built.error, &error),
                  error);
  return built;
}

}  // namespace

TEST_CASE("stress mesh: the shredded atlas is a torus with one island per patch of quads") {
  ShreddedAtlasMesh source;
  build_shredded_atlas_torus(fixture_options(), source);
  CHECK(source.island_count == (96 / 4) * (48 / 4));  // 288
  CHECK(source.positions.size() == source.island_count * 25);
  CHECK(source.normals.size() == source.positions.size());
  CHECK(source.uvs.size() == source.positions.size());
  CHECK(source.indices.size() == source.island_count * 4 * 4 * 6);
  CHECK(source.atlas_cells * source.atlas_cells >= source.island_count);
  // Every island border vertex is a seam vertex: 25 vertices an island, 9 of them interior.
  CHECK(source.seam_vertices == source.island_count * (25 - 9));
  for (const u32 index : source.indices)
    REQUIRE(index < source.positions.size());
  for (const Vec2& uv : source.uvs) {
    CHECK(uv.x >= 0.0f);
    CHECK(uv.x <= 1.0f);
    CHECK(uv.y >= 0.0f);
    CHECK(uv.y <= 1.0f);
  }
  // Every vertex is on the torus, and its normal is the exact surface normal there.
  for (u32 v = 0; v < source.positions.size(); ++v) {
    const Vec3 p = source.positions[v];
    const f32 ring = std::sqrt(p.x * p.x + p.z * p.z);
    const f32 du = ring - 1.0f;
    CHECK(std::fabs(std::sqrt(du * du + p.y * p.y) - 0.35f) < 1e-4f);
    CHECK(std::fabs(length(source.normals[v]) - 1.0f) < 1e-4f);
  }
  // Deterministic: the same options are the same mesh, bit for bit.
  ShreddedAtlasMesh again;
  build_shredded_atlas_torus(fixture_options(), again);
  REQUIRE(again.positions.size() == source.positions.size());
  for (u32 v = 0; v < source.positions.size(); ++v) {
    CHECK(again.uvs[v].x == source.uvs[v].x);
    CHECK(again.uvs[v].y == source.uvs[v].y);
  }
  MESSAGE(source.island_count << " islands in a " << source.atlas_cells << "x" << source.atlas_cells
                              << " atlas, " << source.positions.size() << " vertices of which "
                              << source.seam_vertices << " are seams, " << source.indices.size() / 3
                              << " triangles");

  // The probe texture: one hue a slot, so a UV that leaves its island reads a different colour.
  Vector<u8> texels;
  build_atlas_probe_texture(256, source.atlas_cells, texels);
  REQUIRE(texels.size() == 256u * 256u * 4u);
  const u32 cell_texels = 256 / source.atlas_cells;
  REQUIRE(cell_texels >= 4);
  // Two adjacent slots differ by far more than the within-slot gradient.
  auto texel_at = [&](u32 x, u32 y) {
    const u8* t = &texels[(y * 256 + x) * 4];
    return Vec3{static_cast<f32>(t[0]), static_cast<f32>(t[1]), static_cast<f32>(t[2])};
  };
  const Vec3 first = texel_at(cell_texels / 2, cell_texels / 2);
  const Vec3 second = texel_at(cell_texels + cell_texels / 2, cell_texels / 2);
  CHECK(length(first - second) > 60.0f);
}

TEST_CASE("cluster lod: a shredded atlas survives simplification only when the seams are told") {
  ShreddedAtlasMesh source;
  build_shredded_atlas_torus(fixture_options(), source);
  const u32 leaf_triangles = static_cast<u32>(source.indices.size() / 3);
  const u32 target = leaf_triangles / 5;  // a fifth of the geometry: a real, coarse cut

  // Before: positions only, permissive simplification, nothing tagged.
  const Built before = build_and_measure(source, position_only(), target);
  // After: the UVs weighted, and every island border protected (the normal weight is 0 by
  // default and, as the weight table in geometry.md shows, makes no difference to these numbers).
  const Built after = build_and_measure(source, ClusterLodOptions{}, target);

  MESSAGE("position-only: " << before.cut_clusters << " clusters, " << before.cut_triangles
                            << " triangles, UV error mean " << before.error.uv_mean_texels
                            << " p99 " << before.error.uv_p99_texels << " max "
                            << before.error.uv_max_texels << " texels of 4096, "
                            << before.error.uv_outlier_fraction() * 100.0f
                            << "% of samples wrong, normals mean " << before.error.normal_mean_deg
                            << " deg");
  MESSAGE("attribute-aware: " << after.cut_clusters << " clusters, " << after.cut_triangles
                              << " triangles, UV error mean " << after.error.uv_mean_texels
                              << " p99 " << after.error.uv_p99_texels << " max "
                              << after.error.uv_max_texels << " texels of 4096, "
                              << after.error.uv_outlier_fraction() * 100.0f
                              << "% of samples wrong, normals mean " << after.error.normal_mean_deg
                              << " deg");
  MESSAGE("levels before " << before.mesh.level_cluster_counts.size() << ", after "
                           << after.mesh.level_cluster_counts.size() << "; clusters before "
                           << before.mesh.mesh.clusters.size() << ", after "
                           << after.mesh.mesh.clusters.size());

  // The comparison is at the same triangle budget, give or take a cluster.
  CHECK(after.cut_triangles > before.cut_triangles / 2);
  CHECK(after.cut_triangles < before.cut_triangles * 2);
  CHECK(before.error.unmatched == 0);
  CHECK(after.error.unmatched == 0);

  // The defect, as a number: a position-only build drags a large share of the surface into some
  // other island of the atlas.
  CHECK(before.error.uv_outlier_fraction() > 0.10f);
  CHECK(before.error.uv_max_texels > 500.0f);
  // The fix: almost nothing moves, and what does moves by a few texels rather than half an atlas.
  // The number that says "never crossed an island" is the **maximum**: with 17 atlas slots a
  // side a slot is about 240 texels of 4096, so a worst sample under half of that cannot have
  // left the island it belongs to, however the rest of the distribution is read.
  CHECK(after.error.uv_max_texels < 120.0f);
  CHECK(after.error.uv_outlier_fraction() < 0.03f);
  CHECK(after.error.uv_mean_texels < before.error.uv_mean_texels / 10.0f);
  CHECK(after.error.uv_p99_texels < 32.0f);
  // Normals travel with the UVs: an island-crossing collapse moves both.
  CHECK(after.error.normal_mean_deg <= before.error.normal_mean_deg);
}

TEST_CASE("cluster lod: the seam rules trade triangles for the atlas, in the order expected") {
  ShreddedAtlasMesh source;
  build_shredded_atlas_torus(fixture_options(), source);
  AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(source.normals.data(), source.normals.size());
  attributes.uvs = std::span<const Vec2>(source.uvs.data(), source.uvs.size());

  // The cost of a seam rule is **not** the cluster total: a rule that gets stuck sooner produces
  // fewer clusters *and* a worse mesh, because the levels it never built are the cheap ones. The
  // honest cost is what the mesh reduces to when the camera is far enough away that the threshold
  // does not matter — the coarsest cut — and how many levels there are to get there.
  struct Cost {
    u32 levels = 0;
    u32 clusters = 0;
    u32 coarsest_triangles = 0;
  };
  auto cost_of = [&](const ClusterLodOptions& options) {
    ClusterLodMesh mesh;
    std::string error;
    REQUIRE_MESSAGE(
        build_cluster_lod(source.positions, source.indices, options, mesh, &error, attributes),
        error);
    REQUIRE_MESSAGE(validate_cluster_lod(mesh, source.indices, &error), error);
    Vector<u32> cut;
    select_lod_raw(mesh, 1e30f, cut);
    Cost cost;
    cost.levels = mesh.level_cluster_counts.size();
    cost.clusters = mesh.mesh.clusters.size();
    cost.coarsest_triangles = triangles_of(mesh, cut);
    return cost;
  };

  ClusterLodOptions untagged;
  untagged.uv_seams = SeamRule::none;
  const Cost none = cost_of(untagged);
  const Cost protect = cost_of(ClusterLodOptions{});
  ClusterLodOptions locked;
  locked.uv_seams = SeamRule::lock;
  const Cost lock = cost_of(locked);

  MESSAGE("seam rule none: " << none.clusters << " clusters, " << none.levels << " levels, "
                             << none.coarsest_triangles << " triangles at the coarsest cut");
  MESSAGE("seam rule protect: " << protect.clusters << " clusters, " << protect.levels
                                << " levels, " << protect.coarsest_triangles
                                << " triangles at the coarsest cut");
  MESSAGE("seam rule lock: " << lock.clusters << " clusters, " << lock.levels << " levels, "
                             << lock.coarsest_triangles << " triangles at the coarsest cut");
  // Protecting a seam keeps geometry an untagged collapse would have thrown away, and locking
  // one keeps far more — which is why `protect` is the default and `lock` is a knob.
  CHECK(protect.coarsest_triangles >= none.coarsest_triangles);
  CHECK(lock.coarsest_triangles > protect.coarsest_triangles);
  CHECK(lock.levels < protect.levels);
}

// The table that decided `ClusterLodOptions::normal_weight`, kept as code so the decision can be
// re-taken rather than re-argued (geometry.md, "What the weights are worth"). Skipped by default:
// it builds ten DAGs and proves nothing that the defaults do not already assert. Run it with
// `engine_geometry_tests -tc='cluster lod: what the attribute weights cost' -ns -s`.
TEST_CASE("cluster lod: what the attribute weights cost" * doctest::skip()) {
  auto terrain = [](u32 n, f32 extent, Vector<Vec3>& positions, Vector<u32>& indices,
                    Vector<Vec2>& uvs) {
    positions.clear();
    indices.clear();
    uvs.clear();
    for (u32 z = 0; z < n; ++z) {
      for (u32 x = 0; x < n; ++x) {
        const f32 fx = -extent + 2.0f * extent * static_cast<f32>(x) / (n - 1);
        const f32 fz = -extent + 2.0f * extent * static_cast<f32>(z) / (n - 1);
        const f32 h = 0.9f * std::sin(fx * 0.55f) * std::cos(fz * 0.4f) +
                      0.35f * std::sin(fx * 1.7f + fz * 1.1f) +
                      0.12f * std::cos(fx * 4.3f - fz * 3.7f);
        positions.push_back(Vec3{fx, h, fz});
        uvs.push_back(Vec2{(fx + extent) / (2.0f * extent), (fz + extent) / (2.0f * extent)});
      }
    }
    for (u32 z = 0; z + 1 < n; ++z) {
      for (u32 x = 0; x + 1 < n; ++x) {
        const u32 a = z * n + x;
        indices.push_back(a);
        indices.push_back(a + n);
        indices.push_back(a + 1);
        indices.push_back(a + 1);
        indices.push_back(a + n);
        indices.push_back(a + n + 1);
      }
    }
  };
  Vector<Vec3> positions;
  Vector<u32> indices;
  Vector<Vec2> uvs;
  terrain(129, 10.0f, positions, indices, uvs);
  AttributeSource attributes;
  attributes.uvs = std::span<const Vec2>(uvs.data(), uvs.size());

  ShreddedAtlasMesh torus;
  build_shredded_atlas_torus(fixture_options(), torus);
  AttributeSource torus_attributes;
  torus_attributes.normals = std::span<const Vec3>(torus.normals.data(), torus.normals.size());
  torus_attributes.uvs = std::span<const Vec2>(torus.uvs.data(), torus.uvs.size());

  LodView view;
  view.proj_scale = 1.0f / std::tan(radians(60.0f) * 0.5f) * 1080.0f * 0.5f;
  view.threshold_px = 1.0f;

  MESSAGE("nw    uw    | terrain levels  cut@30  cut@120 | torus levels  cut@6  uvmax  uv>8px");
  for (const f32 nw : {0.0f, 0.05f, 0.1f, 0.25f, 0.5f}) {
    for (const f32 uw : {0.0f, 0.5f}) {
      ClusterLodOptions options;
      options.normal_weight = nw;
      options.uv_weight = uw;
      std::string error;
      ClusterLodMesh t;
      REQUIRE(build_cluster_lod(positions, indices, options, t, &error, attributes));
      view.camera = Vec3{0.0f, 30.0f, 0.0f};
      Vector<u32> cut30;
      select_lod(t, view, cut30);
      view.camera = Vec3{0.0f, 120.0f, 0.0f};
      Vector<u32> cut120;
      select_lod(t, view, cut120);

      ClusterLodMesh g;
      REQUIRE(
          build_cluster_lod(torus.positions, torus.indices, options, g, &error, torus_attributes));
      view.camera = Vec3{0.0f, 6.0f, 0.0f};
      Vector<u32> cutg;
      select_lod(g, view, cutg);
      AttributeError measured;
      AttributeErrorOptions measure_options;
      REQUIRE(measure_lod_attribute_error(g, cutg, measure_options, measured, &error));
      MESSAGE(nw << "  " << uw << "  | " << t.level_cluster_counts.size() << "  "
                 << triangles_of(t, cut30) << "  " << triangles_of(t, cut120) << " | "
                 << g.level_cluster_counts.size() << "  " << triangles_of(g, cutg) << "  "
                 << measured.uv_max_texels << "  " << measured.uv_outlier_fraction() * 100.0f
                 << "%");
    }
  }
}

TEST_CASE("cluster lod: the attribute metric agrees with itself and refuses what it cannot do") {
  ShreddedAtlasMesh source;
  ShreddedAtlasOptions small = fixture_options();
  small.segments = 48;
  small.rings = 24;
  build_shredded_atlas_torus(small, source);
  AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(source.normals.data(), source.normals.size());
  attributes.uvs = std::span<const Vec2>(source.uvs.data(), source.uvs.size());
  ClusterLodMesh mesh;
  std::string error;
  REQUIRE_MESSAGE(build_cluster_lod(source.positions, source.indices, ClusterLodOptions{}, mesh,
                                    &error, attributes),
                  error);

  // The finest cut is the surface itself, so every sample lands on the triangle it came from and
  // the only error left is the half-float UV encoding — under two texels of 4096.
  Vector<u32> leaves;
  for (u32 c = 0; c < mesh.mesh.clusters.size(); ++c) {
    if (mesh.lod[c].level == 0) leaves.push_back(c);
  }
  AttributeError finest;
  AttributeErrorOptions measure;
  REQUIRE_MESSAGE(measure_lod_attribute_error(mesh, leaves, measure, finest, &error), error);
  CHECK(finest.samples > 0);
  CHECK(finest.unmatched == 0);
  CHECK(finest.uv_max_texels < 4.0f);
  CHECK(finest.normal_max_deg < 1.0f);
  CHECK(finest.uv_outliers == 0);
  MESSAGE("the finest cut against itself: " << finest.uv_max_texels << " texels, "
                                            << finest.normal_max_deg << " deg at worst");

  // Two runs over the same mesh give the same answer, and the stride is a function of the mesh.
  AttributeError again;
  REQUIRE(measure_lod_attribute_error(mesh, leaves, measure, again, &error));
  CHECK(again.samples == finest.samples);
  CHECK(again.uv_mean_texels == finest.uv_mean_texels);

  // Refusals, each with its own sentence.
  AttributeError ignored;
  CHECK_FALSE(measure_lod_attribute_error(mesh, {}, measure, ignored, &error));
  CHECK(error.find("cut is empty") != std::string::npos);
  const u32 out_of_range[1] = {mesh.mesh.clusters.size()};
  CHECK_FALSE(measure_lod_attribute_error(mesh, out_of_range, measure, ignored, &error));
  CHECK(error.find("out of range") != std::string::npos);
  ClusterLodMesh bare = mesh;
  bare.mesh.attributes.clear();
  CHECK_FALSE(measure_lod_attribute_error(bare, leaves, measure, ignored, &error));
  CHECK(error.find("no attributes") != std::string::npos);
}

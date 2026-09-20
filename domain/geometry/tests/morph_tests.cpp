// Morph channels as a geometry stream (docs/subsystems/geometry.md, "Morph channels"): the
// quantization and what it costs against float, the weld key that sees a vertex's deltas, the
// directory travelling through `vertex_source` into every LOD level and through both merges and
// the page layout, the container round trip, and what `validate_clusters` refuses.
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/stress_mesh.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

using namespace engine;
using namespace engine::geometry;

namespace {

MorphFixtureOptions face_rig() {
  MorphFixtureOptions options;
  options.segments = 48;
  options.rings = 24;
  options.channels = 6;
  options.falloff = 0.25f;
  return options;
}

// The whole surface moving, which is the other shape the chain has to be right on.
MorphFixtureOptions whole_sphere() {
  MorphFixtureOptions options = face_rig();
  options.channels = 1;
  options.falloff = 4.0f;
  return options;
}

AttributeSource source_of(const MorphFixtureMesh& mesh) {
  AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(mesh.normals.data(), mesh.normals.size());
  attributes.uvs = std::span<const Vec2>(mesh.uvs.data(), mesh.uvs.size());
  attributes.morph = std::span<const MorphChannelSource>(mesh.morph.data(), mesh.morph.size());
  return attributes;
}

// Every (cluster, local vertex) of the built mesh, looked up against the source channel, so the
// comparison is "what the format stored" against "what the source said" rather than against
// another copy of the same code.
struct DeltaError {
  f64 position_sum = 0.0;
  f32 position_max = 0.0f;
  f32 normal_max = 0.0f;
  u32 compared = 0;
  u32 missing = 0;  // a source delta the built mesh does not carry for a vertex it holds
};

DeltaError compare_deltas(const ClusterMesh& mesh, const MorphFixtureMesh& source) {
  DeltaError out;
  for (u32 c = 0; c < mesh.clusters.size(); ++c) {
    const ClusterDesc& cluster = mesh.clusters[c];
    for (u32 local = 0; local < cluster.vertex_count; ++local) {
      const u32 vertex = mesh.vertex_source[cluster.vertex_offset + local];
      for (u32 k = 0; k < source.morph.size(); ++k) {
        const MorphChannelSource& channel = source.morph[k];
        const auto at = std::lower_bound(channel.vertices.begin(), channel.vertices.end(), vertex);
        const bool present = at != channel.vertices.end() && *at == vertex;
        Vec3 position_delta;
        Vec3 normal_delta;
        const bool stored = morph_delta_at(mesh, c, k, local, position_delta, normal_delta);
        if (!present) {
          if (stored) ++out.missing;
          continue;
        }
        if (!stored) {
          ++out.missing;
          continue;
        }
        const u32 i = static_cast<u32>(at - channel.vertices.begin());
        const Vec3 want = channel.position_deltas[i];
        const f32 error = length(position_delta - want);
        out.position_sum += static_cast<f64>(error);
        out.position_max = std::max(out.position_max, error);
        ++out.compared;
        if (channel.normal_deltas.size() == channel.vertices.size()) {
          out.normal_max =
              std::max(out.normal_max, length(normal_delta - channel.normal_deltas[i]));
        }
      }
    }
  }
  return out;
}

f32 largest_delta(const MorphFixtureMesh& source) {
  f32 largest = 0.0f;
  for (const MorphChannelSource& channel : source.morph) {
    for (const Vec3& d : channel.position_deltas)
      largest = std::max(largest, length(d));
  }
  return largest;
}

}  // namespace

TEST_CASE("morph fixture: channels are sparse, deterministic, and disagree across the seam") {
  MorphFixtureMesh a;
  MorphFixtureMesh b;
  build_morph_sphere(face_rig(), a);
  build_morph_sphere(face_rig(), b);
  REQUIRE(a.positions.size() == b.positions.size());
  REQUIRE(a.morph.size() == 6);
  for (u32 k = 0; k < a.morph.size(); ++k) {
    CHECK(a.morph[k].vertices.size() == b.morph[k].vertices.size());
    for (u32 i = 0; i < a.morph[k].vertices.size(); ++i) {
      CHECK(a.morph[k].vertices[i] == b.morph[k].vertices[i]);
      CHECK(a.morph[k].position_deltas[i].x == b.morph[k].position_deltas[i].x);
    }
  }

  // Sparsity is the whole reason for the layout: a face channel touches a small region.
  u32 total = 0;
  for (const MorphChannelSource& channel : a.morph)
    total += channel.vertices.size();
  const f32 fraction =
      static_cast<f32>(total) / static_cast<f32>(a.positions.size() * a.morph.size());
  MESSAGE("face rig: " << a.positions.size() << " vertices, " << a.morph.size() << " channels, "
                       << total << " deltas, " << (fraction * 100.0f)
                       << "% of a dense table, seam vertices " << a.morph_seam_vertices);
  CHECK(fraction < 0.25f);
  CHECK(a.morph_seam_vertices > 0);

  MorphFixtureMesh sphere;
  build_morph_sphere(whole_sphere(), sphere);
  REQUIRE(sphere.morph.size() == 1);
  CHECK(sphere.morph[0].vertices.size() == sphere.positions.size());
}

TEST_CASE("morph: 16 bits with a per-channel scale costs far less than the position grid") {
  MorphFixtureMesh source;
  build_morph_sphere(face_rig(), source);
  ClusterMesh mesh;
  std::string error;
  REQUIRE_MESSAGE(build_clusters(source.positions, source.indices, ClusterBuildOptions{}, mesh,
                                 &error, source_of(source)),
                  error);
  REQUIRE(mesh.morph_channels.size() == source.morph.size());
  REQUIRE(mesh.morph_delta_count > 0);

  const DeltaError delta = compare_deltas(mesh, source);
  CHECK(delta.missing == 0);
  REQUIRE(delta.compared > 0);
  const f32 mean = static_cast<f32>(delta.position_sum / delta.compared);
  // The number that says the quantization is free: half a step of the *mesh's* 16-bit position
  // grid, which every position already lives on, against the worst error a delta picked up.
  const f32 position_step = mesh.quant_scale;
  MESSAGE("morph quantization: " << delta.compared << " deltas, mean " << mean << ", max "
                                 << delta.position_max
                                 << " mesh units; the position grid's own step is " << position_step
                                 << " (max delta " << largest_delta(source) << ")");
  CHECK(delta.position_max < position_step * 0.5f);
  CHECK(delta.normal_max < 1.0e-3f);

  // And the scale really is the channel's own, not the mesh's.
  for (u32 k = 0; k < mesh.morph_channels.size(); ++k) {
    const MorphChannel& channel = mesh.morph_channels[k];
    CHECK(channel.position_scale > 0.0f);
    CHECK(channel.position_scale < position_step);
    CHECK(channel.max_displacement > 0.0f);
    CHECK(mesh.morph_names[k] == source.morph[k].name);
  }
  CHECK(validate_clusters(mesh, source.indices, ClusterBuildOptions{}, &error));
}

TEST_CASE("morph: the weld key sees a vertex's deltas") {
  MorphFixtureMesh source;
  build_morph_sphere(face_rig(), source);

  // The interning is exact: equal ids mean identical quantized tuples.
  Vector<u32> keys;
  const u32 distinct = morph_vertex_keys(
      std::span<const MorphChannelSource>(source.morph.data(), source.morph.size()),
      source.positions.size(), keys);
  REQUIRE(keys.size() == source.positions.size());
  CHECK(distinct > 1);
  u32 untouched = 0;
  for (const u32 key : keys)
    untouched += key == 0 ? 1u : 0u;
  CHECK(untouched > 0);  // a face rig leaves most of the sphere alone

  // Welding with the deltas in the key keeps the seam's two copies apart; welding without them
  // merges the pairs whose position, normal and UV also agree. The sphere's seam column shares a
  // position with the u = 0 column and *differs* in UV, so the count that moves here is the one
  // the `split_seam` option made disagree about the deltas at an otherwise identical vertex.
  auto weld_with = [&](bool with_morph) {
    Vector<Vec3> positions = source.positions;
    Vector<Vec3> normals = source.normals;
    Vector<Vec2> uvs = source.uvs;
    // Drop the UVs so that the seam's two copies differ in nothing *but* their morph deltas,
    // which is the case this key exists for.
    uvs.clear();
    Vector<u32> indices = source.indices;
    Vector<MorphChannelSource> morph = source.morph;
    return weld_vertices(positions, normals, uvs, std::span<u32>(indices.data(), indices.size()),
                         nullptr, with_morph ? &morph : nullptr);
  };
  const u32 without = weld_with(false);
  const u32 with = weld_with(true);
  MESSAGE("weld: " << source.positions.size() << " source vertices, " << without
                   << " without the morph key, " << with << " with it");
  CHECK(with > without);
  CHECK(with - without == source.morph_seam_vertices);
}

TEST_CASE("morph: the weld remaps a channel's vertices and the result still builds") {
  MorphFixtureMesh source;
  build_morph_sphere(face_rig(), source);
  Vector<Vec3> positions = source.positions;
  Vector<Vec3> normals = source.normals;
  Vector<Vec2> uvs = source.uvs;
  Vector<u32> indices = source.indices;
  Vector<MorphChannelSource> morph = source.morph;
  const u32 welded = weld_vertices(positions, normals, uvs,
                                   std::span<u32>(indices.data(), indices.size()), nullptr, &morph);
  REQUIRE(welded > 0);
  for (const MorphChannelSource& channel : morph) {
    CHECK(channel.vertices.size() == channel.position_deltas.size());
    CHECK(channel.vertices.size() == channel.normal_deltas.size());
    for (u32 i = 0; i < channel.vertices.size(); ++i) {
      CHECK(channel.vertices[i] < welded);
      if (i > 0) CHECK(channel.vertices[i] > channel.vertices[i - 1]);  // sorted, no repeats
    }
  }
  ClusterMesh mesh;
  std::string error;
  AttributeSource attributes;
  attributes.normals = std::span<const Vec3>(normals.data(), normals.size());
  attributes.uvs = std::span<const Vec2>(uvs.data(), uvs.size());
  attributes.morph = std::span<const MorphChannelSource>(morph.data(), morph.size());
  REQUIRE_MESSAGE(
      build_clusters(positions, indices, ClusterBuildOptions{}, mesh, &error, attributes), error);
  CHECK_MESSAGE(validate_clusters(mesh, indices, ClusterBuildOptions{}, &error), error);
}

TEST_CASE("morph: every LOD level carries the source vertex's deltas") {
  // A denser sphere with tighter channels, because the property being shown is that most clusters
  // are touched by nothing — which a 51-cluster mesh with channels spread over a whole sphere
  // cannot show.
  MorphFixtureOptions options = face_rig();
  options.segments = 128;
  options.rings = 64;
  options.falloff = 0.15f;
  MorphFixtureMesh source;
  build_morph_sphere(options, source);
  ClusterLodMesh dag;
  std::string error;
  REQUIRE_MESSAGE(build_cluster_lod(source.positions, source.indices, ClusterLodOptions{}, dag,
                                    &error, source_of(source)),
                  error);
  REQUIRE(dag.level_cluster_counts.size() > 2);
  const DeltaError delta = compare_deltas(dag.mesh, source);
  CHECK(delta.missing == 0);
  CHECK(delta.position_max < dag.mesh.quant_scale * 0.5f);
  CHECK_MESSAGE(validate_cluster_lod(dag, source.indices, &error), error);

  // The directory is only as big as it needs to be: a cluster no channel reaches holds no slice,
  // which is the whole point of keying it by cluster.
  u32 empty = 0;
  for (u32 c = 0; c < dag.mesh.clusters.size(); ++c) {
    if (dag.mesh.morph_cluster_slices[c] == dag.mesh.morph_cluster_slices[c + 1]) ++empty;
  }
  MESSAGE("LOD DAG: " << dag.mesh.clusters.size() << " clusters over "
                      << dag.level_cluster_counts.size() << " levels, " << empty
                      << " touched by no channel, " << dag.mesh.morph_slices.size() << " slices, "
                      << dag.mesh.morph_delta_count << " deltas");
  CHECK(empty > 0);
  // The directory is what a dense (cluster x channel) table is not: far smaller than the product.
  CHECK(dag.mesh.morph_slices.size() <
        u64{dag.mesh.clusters.size()} * dag.mesh.morph_channels.size());
}

TEST_CASE("morph: both merges concatenate the stream and the page layout permutes it") {
  MorphFixtureMesh source;
  build_morph_sphere(face_rig(), source);
  ClusterLodMesh dag;
  std::string error;
  REQUIRE_MESSAGE(build_cluster_lod(source.positions, source.indices, ClusterLodOptions{}, dag,
                                    &error, source_of(source)),
                  error);

  SUBCASE("parts of one mesh") {
    // Two DAGs of the same source: `merge_cluster_lod`'s contract is that they share the channel
    // array by index, which is what glTF guarantees for the primitives of one mesh.
    ClusterLodMesh second;
    REQUIRE(build_cluster_lod(source.positions, source.indices, ClusterLodOptions{}, second, &error,
                              source_of(source)));
    ClusterLodMesh merged;
    const ClusterLodMesh parts[2] = {dag, second};
    REQUIRE_MESSAGE(
        merge_cluster_lod(std::span<const ClusterLodMesh>(parts, 2), merged, nullptr, &error),
        error);
    CHECK(merged.mesh.morph_channels.size() == source.morph.size());
    CHECK(merged.mesh.morph_cluster_slices.size() == merged.mesh.clusters.size() + 1);
    CHECK(merged.mesh.morph_delta_count == dag.mesh.morph_delta_count * 2);
    CHECK(merged.mesh.morph_slices.size() == dag.mesh.morph_slices.size() * 2);
    // The per-cluster slice counts are the two parts' counts, as a multiset: the merge reorders
    // clusters (leaves first) but never moves a slice between clusters. (`compare_deltas` cannot
    // be used here: the merge shifts each part's `vertex_source` into its own range of a doubled
    // source, so a lookup against the single source's channels is meaningless by construction.)
    Vector<u32> want;
    Vector<u32> got;
    for (u32 pass = 0; pass < 2; ++pass) {
      for (u32 c = 0; c < dag.mesh.clusters.size(); ++c) {
        want.push_back(dag.mesh.morph_cluster_slices[c + 1] - dag.mesh.morph_cluster_slices[c]);
      }
    }
    for (u32 c = 0; c < merged.mesh.clusters.size(); ++c)
      got.push_back(merged.mesh.morph_cluster_slices[c + 1] - merged.mesh.morph_cluster_slices[c]);
    std::sort(want.begin(), want.end());
    std::sort(got.begin(), got.end());
    REQUIRE(want.size() == got.size());
    for (u32 i = 0; i < want.size(); ++i)
      REQUIRE(want[i] == got[i]);
    CHECK_MESSAGE(validate_cluster_lod(merged, {}, &error) == false, "a merged DAG needs indices");
  }

  SUBCASE("separate meshes of a scene") {
    MorphFixtureOptions other_options = face_rig();
    other_options.channels = 3;
    other_options.seed = 0x27d4eb2fu;
    MorphFixtureMesh other;
    build_morph_sphere(other_options, other);
    ClusterLodMesh other_dag;
    REQUIRE(build_cluster_lod(other.positions, other.indices, ClusterLodOptions{}, other_dag,
                              &error, source_of(other)));
    const ClusterLodMesh meshes[2] = {dag, other_dag};
    ClusterLodMesh scene;
    Vector<ClusterMeshPart> parts;
    REQUIRE_MESSAGE(
        merge_cluster_meshes(std::span<const ClusterLodMesh>(meshes, 2), scene, parts, &error),
        error);
    REQUIRE(parts.size() == 2);
    CHECK(parts[0].first_morph_channel == 0);
    CHECK(parts[0].morph_channel_count == 6);
    CHECK(parts[1].first_morph_channel == 6);
    CHECK(parts[1].morph_channel_count == 3);
    CHECK(scene.mesh.morph_channels.size() == 9);
    CHECK(scene.mesh.morph_names.size() == 9);
    CHECK(validate_morph_stream(scene.mesh, &error));
    // The second mesh's slices name channels 6..8, so nothing of one character's rig reaches the
    // other's.
    for (u32 c = parts[1].first_cluster; c < parts[1].first_cluster + parts[1].cluster_count; ++c) {
      for (u32 s = scene.mesh.morph_cluster_slices[c]; s < scene.mesh.morph_cluster_slices[c + 1];
           ++s) {
        CHECK(scene.mesh.morph_slices[s].channel >= 6);
      }
    }
  }

  SUBCASE("the page layout") {
    // The morph bytes are in the budget, and a page's slices come out contiguous because the
    // permutation walks the clusters in page order.
    ClusterLodMesh paged = dag;
    ClusterPages pages;
    ClusterPagesOptions options;
    options.page_bytes = 64 * 1024;
    Vector<u32> source_of_cluster;
    REQUIRE_MESSAGE(build_cluster_pages(paged, options, pages, &error, &source_of_cluster), error);
    CHECK(paged.mesh.morph_cluster_slices.size() == paged.mesh.clusters.size() + 1);
    CHECK(paged.mesh.morph_delta_count == dag.mesh.morph_delta_count);
    CHECK_MESSAGE(validate_cluster_lod(paged, source.indices, &error), error);
    // Cluster n of the paged mesh holds the slices cluster `source_of_cluster[n]` of the original
    // held, in the same order and with the same deltas.
    for (u32 n = 0; n < paged.mesh.clusters.size(); ++n) {
      const u32 from = source_of_cluster[n];
      const u32 want =
          dag.mesh.morph_cluster_slices[from + 1] - dag.mesh.morph_cluster_slices[from];
      const u32 got = paged.mesh.morph_cluster_slices[n + 1] - paged.mesh.morph_cluster_slices[n];
      REQUIRE(got == want);
    }
    const DeltaError delta = compare_deltas(paged.mesh, source);
    CHECK(delta.missing == 0);
    // A morphed cluster is heavier than the same cluster without a channel on it, and the
    // difference is exactly what the stream costs.
    ClusterLodMesh rigid = dag;
    rigid.mesh.morph_channels.clear();
    rigid.mesh.morph_cluster_slices.clear();
    rigid.mesh.morph_slices.clear();
    rigid.mesh.morph_indices.clear();
    rigid.mesh.morph_deltas.clear();
    rigid.mesh.morph_normal_deltas.clear();
    rigid.mesh.morph_delta_count = 0;
    u64 with = 0;
    u64 without = 0;
    for (u32 c = 0; c < dag.mesh.clusters.size(); ++c) {
      with += cluster_page_bytes(dag.mesh, c);
      without += cluster_page_bytes(rigid.mesh, c);
    }
    MESSAGE("page bytes: " << without << " rigid, " << with << " with "
                           << dag.mesh.morph_delta_count << " deltas over "
                           << dag.mesh.morph_channels.size() << " channels (+"
                           << (100.0 * static_cast<f64>(with - without) / static_cast<f64>(without))
                           << "%)");
    CHECK(with > without);
  }
}

TEST_CASE("morph: the container round-trips the stream and an older file reads as no channels") {
  const test::TempDir tmp("engine_morph_file");
  MorphFixtureMesh source;
  build_morph_sphere(face_rig(), source);
  ClusterFileData data;
  std::string error;
  REQUIRE_MESSAGE(build_cluster_lod(source.positions, source.indices, ClusterLodOptions{},
                                    data.mesh, &error, source_of(source)),
                  error);
  data.cluster_material.resize(data.mesh.mesh.clusters.size(), 0u);
  data.materials.resize(1);
  const std::string path = tmp.file("face.clusters");
  REQUIRE_MESSAGE(write_cluster_file(path, data, &error), error);

  ClusterFileData read;
  REQUIRE_MESSAGE(read_cluster_file(path, read, &error), error);
  const ClusterMesh& a = data.mesh.mesh;
  const ClusterMesh& b = read.mesh.mesh;
  REQUIRE(b.morph_channels.size() == a.morph_channels.size());
  CHECK(b.morph_delta_count == a.morph_delta_count);
  for (u32 k = 0; k < a.morph_channels.size(); ++k) {
    CHECK(b.morph_channels[k].position_scale == a.morph_channels[k].position_scale);
    CHECK(b.morph_channels[k].normal_scale == a.morph_channels[k].normal_scale);
    CHECK(b.morph_channels[k].max_displacement == a.morph_channels[k].max_displacement);
    CHECK(b.morph_names[k] == a.morph_names[k]);
  }
  REQUIRE(b.morph_cluster_slices.size() == a.morph_cluster_slices.size());
  for (u32 i = 0; i < a.morph_cluster_slices.size(); ++i)
    REQUIRE(b.morph_cluster_slices[i] == a.morph_cluster_slices[i]);
  REQUIRE(b.morph_slices.size() == a.morph_slices.size());
  for (u32 i = 0; i < a.morph_slices.size(); ++i) {
    REQUIRE(b.morph_slices[i].channel == a.morph_slices[i].channel);
    REQUIRE(b.morph_slices[i].first_delta == a.morph_slices[i].first_delta);
    REQUIRE(b.morph_slices[i].delta_count == a.morph_slices[i].delta_count);
  }
  for (u32 i = 0; i < a.morph_delta_count; ++i) {
    REQUIRE(b.morph_indices[i] == a.morph_indices[i]);
    for (u32 k = 0; k < 3; ++k) {
      REQUIRE(b.morph_deltas[i * 3 + k] == a.morph_deltas[i * 3 + k]);
      REQUIRE(b.morph_normal_deltas[i * 3 + k] == a.morph_normal_deltas[i * 3 + k]);
    }
  }
  CHECK_MESSAGE(validate_cluster_lod(read.mesh, source.indices, &error), error);

  // A mesh with no channels writes the seven sections empty, and reads back as one.
  ClusterFileData rigid;
  REQUIRE(
      build_cluster_lod(source.positions, source.indices, ClusterLodOptions{}, rigid.mesh, &error));
  rigid.cluster_material.resize(rigid.mesh.mesh.clusters.size(), 0u);
  rigid.materials.resize(1);
  const std::string rigid_path = tmp.file("rigid.clusters");
  REQUIRE_MESSAGE(write_cluster_file(rigid_path, rigid, &error), error);
  ClusterFileData rigid_read;
  REQUIRE_MESSAGE(read_cluster_file(rigid_path, rigid_read, &error), error);
  CHECK(rigid_read.mesh.mesh.morph_channels.empty());
  CHECK(rigid_read.mesh.mesh.morph_cluster_slices.empty());
  CHECK(rigid_read.mesh.mesh.morph_delta_count == 0);
}

TEST_CASE("morph: validate_clusters names what is wrong") {
  MorphFixtureMesh source;
  build_morph_sphere(face_rig(), source);
  ClusterMesh good;
  std::string error;
  REQUIRE(build_clusters(source.positions, source.indices, ClusterBuildOptions{}, good, &error,
                         source_of(source)));
  REQUIRE(validate_clusters(good, source.indices, ClusterBuildOptions{}, &error));

  SUBCASE("a directory that does not cover every cluster") {
    ClusterMesh mesh = good;
    mesh.morph_cluster_slices.pop_back();
    CHECK_FALSE(validate_clusters(mesh, source.indices, ClusterBuildOptions{}, &error));
    CHECK(error.find("cover every cluster") != std::string::npos);
  }
  SUBCASE("a slice naming a channel the mesh does not have") {
    ClusterMesh mesh = good;
    REQUIRE(!mesh.morph_slices.empty());
    mesh.morph_slices[0].channel = mesh.morph_channels.size();
    CHECK_FALSE(validate_clusters(mesh, source.indices, ClusterBuildOptions{}, &error));
    CHECK(error.find("channel the mesh does not have") != std::string::npos);
  }
  SUBCASE("a delta naming a vertex outside its cluster") {
    ClusterMesh mesh = good;
    REQUIRE(mesh.morph_delta_count > 0);
    mesh.morph_indices[0] = 254;
    CHECK_FALSE(validate_clusters(mesh, source.indices, ClusterBuildOptions{}, &error));
    CHECK(error.find("outside its cluster") != std::string::npos);
  }
  SUBCASE("a scale that is not finite") {
    ClusterMesh mesh = good;
    mesh.morph_channels[0].position_scale = std::numeric_limits<f32>::infinity();
    CHECK_FALSE(validate_clusters(mesh, source.indices, ClusterBuildOptions{}, &error));
    CHECK(error.find("not finite") != std::string::npos);
  }
  SUBCASE("deltas with no channels to belong to") {
    ClusterMesh mesh = good;
    mesh.morph_channels.clear();
    mesh.morph_names.clear();
    CHECK_FALSE(validate_clusters(mesh, source.indices, ClusterBuildOptions{}, &error));
    CHECK(error.find("names no channels") != std::string::npos);
  }
}

TEST_CASE("morph: the bounds padding is the sum of |weight| times the channel's reach") {
  MorphFixtureMesh source;
  build_morph_sphere(face_rig(), source);
  ClusterMesh mesh;
  std::string error;
  REQUIRE(build_clusters(source.positions, source.indices, ClusterBuildOptions{}, mesh, &error,
                         source_of(source)));

  Vector<f32> weights(mesh.morph_channels.size(), 0.0f);
  CHECK(morph_bounds_padding(mesh, std::span<const f32>(weights.data(), weights.size())) == 0.0f);

  weights[0] = 1.0f;
  const f32 one = morph_bounds_padding(mesh, std::span<const f32>(weights.data(), weights.size()));
  CHECK(one == doctest::Approx(mesh.morph_channels[0].max_displacement));
  weights[1] = -2.0f;
  const f32 two = morph_bounds_padding(mesh, std::span<const f32>(weights.data(), weights.size()));
  CHECK(two == doctest::Approx(one + 2.0f * mesh.morph_channels[1].max_displacement));

  // It really does bound the mesh: no vertex moves further than the padding says, at any weights.
  Vector<f32> played(mesh.morph_channels.size(), 0.0f);
  for (u32 k = 0; k < played.size(); ++k)
    played[k] = k % 2 == 0 ? 1.0f : -0.5f;
  const f32 bound = morph_bounds_padding(mesh, std::span<const f32>(played.data(), played.size()));
  f32 worst = 0.0f;
  for (u32 v = 0; v < source.positions.size(); ++v) {
    const Vec3 moved =
        morph_fixture_position(source, v, std::span<const f32>(played.data(), played.size()));
    worst = std::max(worst, length(moved - source.positions[v]));
  }
  MESSAGE("bounds padding: bound " << bound << ", worst vertex moved " << worst);
  CHECK(worst <= bound + 1.0e-5f);
}

#include <core/containers/hash_set.h>
#include <domain/geometry/cluster.h>

#include <algorithm>
#include <meshoptimizer.h>

namespace engine::geometry {

bool build_clusters(std::span<const Vec3> positions, std::span<const u32> indices,
                    const ClusterBuildOptions& options, ClusterMesh& out, std::string* error) {
  out = ClusterMesh{};
  if (positions.empty() || indices.empty() || indices.size() % 3 != 0) {
    if (error != nullptr) *error = "build_clusters: need vertices and a multiple of three indices";
    return false;
  }
  if (options.max_vertices == 0 || options.max_vertices > 255 || options.max_triangles == 0 ||
      options.max_triangles > 512 || options.max_triangles % 4 != 0) {
    if (error != nullptr) {
      *error =
          "build_clusters: max_vertices must be 1..255 and max_triangles a multiple of 4 up to 512";
    }
    return false;
  }
  for (const u32 index : indices) {
    if (index >= positions.size()) {
      if (error != nullptr) *error = "build_clusters: index out of range";
      return false;
    }
  }

  const usize bound =
      meshopt_buildMeshletsBound(indices.size(), options.max_vertices, options.max_triangles);
  Vector<meshopt_Meshlet> meshlets(static_cast<u32>(bound));
  Vector<unsigned int> meshlet_vertices(static_cast<u32>(bound * options.max_vertices));
  Vector<unsigned char> meshlet_triangles(static_cast<u32>(bound * options.max_triangles * 3));
  const usize count = meshopt_buildMeshlets(
      meshlets.data(), meshlet_vertices.data(), meshlet_triangles.data(), indices.data(),
      indices.size(), &positions[0].x, positions.size(), sizeof(Vec3), options.max_vertices,
      options.max_triangles, options.cone_weight);

  out.source_vertex_count = static_cast<u32>(positions.size());
  out.source_triangle_count = static_cast<u32>(indices.size() / 3);
  out.clusters.reserve(static_cast<u32>(count));
  for (usize m = 0; m < count; ++m) {
    meshopt_Meshlet& meshlet = meshlets[static_cast<u32>(m)];
    meshopt_optimizeMeshlet(&meshlet_vertices[meshlet.vertex_offset],
                            &meshlet_triangles[meshlet.triangle_offset], meshlet.triangle_count,
                            meshlet.vertex_count);
    const meshopt_Bounds bounds = meshopt_computeMeshletBounds(
        &meshlet_vertices[meshlet.vertex_offset], &meshlet_triangles[meshlet.triangle_offset],
        meshlet.triangle_count, &positions[0].x, positions.size(), sizeof(Vec3));

    ClusterDesc desc;
    desc.vertex_offset = out.vertices.size();
    desc.triangle_offset = out.triangles.size();
    desc.vertex_count = meshlet.vertex_count;
    desc.triangle_count = meshlet.triangle_count;
    desc.center = Vec3{bounds.center[0], bounds.center[1], bounds.center[2]};
    desc.radius = bounds.radius;
    for (u32 v = 0; v < meshlet.vertex_count; ++v) {
      const u32 source = meshlet_vertices[meshlet.vertex_offset + v];
      out.vertices.push_back(positions[source]);
      out.vertex_source.push_back(source);
    }
    for (u32 t = 0; t < meshlet.triangle_count; ++t) {
      const unsigned char* tri = &meshlet_triangles[meshlet.triangle_offset + t * 3];
      out.triangles.push_back(ClusterMesh::pack(tri[0], tri[1], tri[2]));
    }
    out.clusters.push_back(desc);
  }
  return true;
}

bool validate_clusters(const ClusterMesh& mesh, std::span<const u32> source_indices,
                       const ClusterBuildOptions& options, std::string* error) {
  auto fail = [&](const char* what) {
    if (error != nullptr) *error = what;
    return false;
  };
  if (source_indices.size() % 3 != 0) return fail("source index count is not a multiple of three");
  if (mesh.vertices.size() != mesh.vertex_source.size())
    return fail("vertex_source does not match vertices");

  // Every source triangle exactly once: compare sorted canonical corner triples.
  Vector<u64> expected;
  Vector<u64> found;
  auto key_of = [](u32 a, u32 b, u32 c) {
    u32 v[3] = {a, b, c};
    std::sort(v, v + 3);
    return (u64{v[0]} << 42) | (u64{v[1]} << 21) | u64{v[2]};
  };
  for (usize i = 0; i + 2 < source_indices.size(); i += 3) {
    expected.push_back(key_of(source_indices[i], source_indices[i + 1], source_indices[i + 2]));
  }
  u32 total_triangles = 0;
  for (u32 c = 0; c < mesh.clusters.size(); ++c) {
    const ClusterDesc& d = mesh.clusters[c];
    if (d.vertex_count == 0 || d.vertex_count > options.max_vertices)
      return fail("cluster vertex count out of limits");
    if (d.triangle_count == 0 || d.triangle_count > options.max_triangles)
      return fail("cluster triangle count out of limits");
    if (u64{d.vertex_offset} + d.vertex_count > mesh.vertices.size())
      return fail("cluster vertex range out of bounds");
    if (u64{d.triangle_offset} + d.triangle_count > mesh.triangles.size())
      return fail("cluster triangle range out of bounds");
    for (u32 v = 0; v < d.vertex_count; ++v) {
      const Vec3 p = mesh.vertices[d.vertex_offset + v];
      if (length(p - d.center) > d.radius * 1.001f + 1e-5f)
        return fail("vertex outside its cluster sphere");
    }
    for (u32 t = 0; t < d.triangle_count; ++t) {
      const u32 packed = mesh.triangles[d.triangle_offset + t];
      u32 corners[3];
      for (u32 k = 0; k < 3; ++k) {
        corners[k] = ClusterMesh::unpack(packed, k);
        if (corners[k] >= d.vertex_count) return fail("local index out of range");
      }
      found.push_back(key_of(mesh.vertex_source[d.vertex_offset + corners[0]],
                             mesh.vertex_source[d.vertex_offset + corners[1]],
                             mesh.vertex_source[d.vertex_offset + corners[2]]));
    }
    total_triangles += d.triangle_count;
  }
  if (total_triangles != mesh.source_triangle_count)
    return fail("triangle total does not match the source");
  std::sort(expected.begin(), expected.end());
  std::sort(found.begin(), found.end());
  if (expected.size() != found.size()) return fail("triangle count mismatch");
  for (u32 i = 0; i < expected.size(); ++i) {
    if (expected[i] != found[i]) return fail("a source triangle is missing or duplicated");
  }
  return true;
}

}  // namespace engine::geometry

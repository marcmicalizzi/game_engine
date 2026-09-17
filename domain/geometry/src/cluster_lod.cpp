#include <domain/geometry/cluster_lod.h>

#include <meshoptimizer.h>

// clusterlod.h is meshoptimizer's reference cluster LOD builder, header-only with the
// implementation compiled into this one translation unit.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#define CLUSTERLOD_IMPLEMENTATION
#include <clusterlod.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>

namespace engine::geometry {

namespace {

struct BuildContext {
  std::span<const Vec3> positions;
  ClusterLodMesh* out;
  bool normal_cones = true;
  Vector<clodBounds> group_bounds;  // simplified bounds per group, in output order
  Vector<unsigned int> local_vertices;
  Vector<unsigned char> local_triangles;
  bool failed = false;
};

int emit_group(void* context, clodGroup group, const clodCluster* clusters, size_t cluster_count) {
  auto* ctx = static_cast<BuildContext*>(context);
  ClusterLodMesh& out = *ctx->out;
  const int group_index = static_cast<int>(ctx->group_bounds.size());
  ctx->group_bounds.push_back(group.simplified);
  const u32 depth = group.depth >= 0 ? static_cast<u32>(group.depth) : 0;
  while (out.level_cluster_counts.size() <= depth)
    out.level_cluster_counts.push_back(0);

  for (size_t i = 0; i < cluster_count; ++i) {
    const clodCluster& cluster = clusters[i];
    if (cluster.vertex_count > 255 || cluster.index_count / 3 > 512) {
      ctx->failed = true;
      return group_index;
    }
    ctx->local_vertices.resize(static_cast<u32>(cluster.vertex_count));
    ctx->local_triangles.resize(static_cast<u32>(cluster.index_count));
    clodLocalIndices(ctx->local_vertices.data(), ctx->local_triangles.data(), cluster.indices,
                     cluster.index_count);

    ClusterDesc desc;
    desc.vertex_offset = out.mesh.vertices.size();
    desc.triangle_offset = out.mesh.triangles.size();
    desc.vertex_count = static_cast<u32>(cluster.vertex_count);
    desc.triangle_count = static_cast<u32>(cluster.index_count / 3);
    desc.center =
        Vec3{cluster.bounds.center[0], cluster.bounds.center[1], cluster.bounds.center[2]};
    desc.radius = cluster.bounds.radius;
    // clusterlod bounds carry no cone; meshoptimizer's meshlet bounds do, on the local indices.
    const meshopt_Bounds bounds = meshopt_computeMeshletBounds(
        ctx->local_vertices.data(), ctx->local_triangles.data(), cluster.index_count / 3,
        &ctx->positions[0].x, ctx->positions.size(), sizeof(Vec3));
    desc.cone_apex = Vec3{bounds.cone_apex[0], bounds.cone_apex[1], bounds.cone_apex[2]};
    if (ctx->normal_cones) {
      auto byte = [](signed char v) { return u32{static_cast<u8>(v)}; };
      desc.cone = byte(bounds.cone_axis_s8[0]) | (byte(bounds.cone_axis_s8[1]) << 8) |
                  (byte(bounds.cone_axis_s8[2]) << 16) | (byte(bounds.cone_cutoff_s8) << 24);
    }
    for (u32 v = 0; v < desc.vertex_count; ++v) {
      const u32 source = ctx->local_vertices[v];
      out.mesh.vertices.push_back(ctx->positions[source]);
      out.mesh.vertex_source.push_back(source);
    }
    for (u32 t = 0; t < desc.triangle_count; ++t) {
      const unsigned char* tri = &ctx->local_triangles[t * 3];
      out.mesh.triangles.push_back(ClusterMesh::pack(tri[0], tri[1], tri[2]));
    }
    out.mesh.clusters.push_back(desc);

    ClusterLodDesc lod;
    if (cluster.refined >= 0) {
      const clodBounds& own = ctx->group_bounds[static_cast<u32>(cluster.refined)];
      lod.own = Vec4{own.center[0], own.center[1], own.center[2], own.radius};
      lod.own_error = own.error;
    } else {
      lod.own = Vec4{desc.center.x, desc.center.y, desc.center.z, desc.radius};
      lod.own_error = 0.0f;
    }
    lod.parent = Vec4{group.simplified.center[0], group.simplified.center[1],
                      group.simplified.center[2], group.simplified.radius};
    lod.parent_error = group.simplified.error;
    lod.level = depth;
    lod.group = static_cast<u32>(group_index);
    out.lod.push_back(lod);
    ++out.level_cluster_counts[depth];
    if (depth == 0) out.leaf_triangle_count += desc.triangle_count;
  }
  return group_index;
}

}  // namespace

bool build_cluster_lod(std::span<const Vec3> positions, std::span<const u32> indices,
                       const ClusterLodOptions& options, ClusterLodMesh& out, std::string* error,
                       const AttributeSource& attributes) {
  out = ClusterLodMesh{};
  if (positions.empty() || indices.empty() || indices.size() % 3 != 0) {
    if (error != nullptr)
      *error = "build_cluster_lod: need vertices and a multiple of three indices";
    return false;
  }
  if (options.max_triangles < 4 || options.max_triangles > 256 || options.max_vertices == 0 ||
      options.max_vertices > 255) {
    if (error != nullptr)
      *error = "build_cluster_lod: max_triangles must be 4..256 and max_vertices 1..255";
    return false;
  }
  for (const u32 index : indices) {
    if (index >= positions.size()) {
      if (error != nullptr) *error = "build_cluster_lod: index out of range";
      return false;
    }
  }

  clodConfig config = options.ray_tracing ? clodDefaultConfigRT(options.max_triangles)
                                          : clodDefaultConfig(options.max_triangles);
  config.max_vertices = options.max_vertices;
  config.optimize_bounds = true;
  clodMesh mesh{};
  mesh.indices = indices.data();
  mesh.index_count = indices.size();
  mesh.vertex_count = positions.size();
  mesh.vertex_positions = &positions[0].x;
  mesh.vertex_positions_stride = sizeof(Vec3);

  BuildContext ctx;
  ctx.positions = positions;
  ctx.out = &out;
  ctx.normal_cones = options.normal_cones;
  out.mesh.source_vertex_count = static_cast<u32>(positions.size());
  out.mesh.source_triangle_count = static_cast<u32>(indices.size() / 3);
  const size_t produced = clodBuild(config, mesh, &ctx, &emit_group);
  if (ctx.failed) {
    if (error != nullptr) *error = "build_cluster_lod: a cluster exceeded the format limits";
    out = ClusterLodMesh{};
    return false;
  }
  if (produced != out.mesh.clusters.size() || out.mesh.clusters.empty()) {
    if (error != nullptr) *error = "build_cluster_lod: the builder produced no clusters";
    out = ClusterLodMesh{};
    return false;
  }
  out.group_count = ctx.group_bounds.size();
  fill_cluster_attributes(out.mesh, positions, indices, attributes);
  return true;
}

f32 projected_error(Vec4 sphere, f32 error, const LodView& view) noexcept {
  if (error >= k_lod_terminal_error) return k_lod_terminal_error;
  const Vec3 center{sphere.x, sphere.y, sphere.z};
  const f32 distance = length(center - view.camera) - sphere.w;
  const f32 clamped = distance > view.znear ? distance : view.znear;
  return error / clamped * view.proj_scale;
}

bool lod_selects(const ClusterLodDesc& lod, const LodView& view) noexcept {
  const f32 own = projected_error(lod.own, lod.own_error, view);
  const f32 parent = projected_error(lod.parent, lod.parent_error, view);
  return own <= view.threshold_px && parent > view.threshold_px;
}

u32 select_lod(const ClusterLodMesh& mesh, const LodView& view, Vector<u32>& out) {
  u32 count = 0;
  for (u32 i = 0; i < mesh.lod.size(); ++i) {
    if (lod_selects(mesh.lod[i], view)) {
      out.push_back(i);
      ++count;
    }
  }
  return count;
}

u32 select_lod_raw(const ClusterLodMesh& mesh, f32 threshold, Vector<u32>& out) {
  u32 count = 0;
  for (u32 i = 0; i < mesh.lod.size(); ++i) {
    const ClusterLodDesc& lod = mesh.lod[i];
    if (lod.own_error <= threshold && lod.parent_error > threshold) {
      out.push_back(i);
      ++count;
    }
  }
  return count;
}

bool validate_cluster_lod(const ClusterLodMesh& mesh, std::span<const u32> source_indices,
                          std::string* error) {
  auto fail = [&](const char* what) {
    if (error != nullptr) *error = what;
    return false;
  };
  if (mesh.mesh.clusters.size() != mesh.lod.size())
    return fail("lod table does not match clusters");
  if (mesh.level_cluster_counts.empty()) return fail("no levels");

  // Level 0 covers the source exactly once: reuse the plain validator on a level-0 view.
  ClusterMesh leaves;
  leaves.source_vertex_count = mesh.mesh.source_vertex_count;
  leaves.source_triangle_count = mesh.mesh.source_triangle_count;
  leaves.vertices = mesh.mesh.vertices;
  leaves.vertex_source = mesh.mesh.vertex_source;
  leaves.triangles = mesh.mesh.triangles;
  for (u32 i = 0; i < mesh.mesh.clusters.size(); ++i) {
    if (mesh.lod[i].level == 0) leaves.clusters.push_back(mesh.mesh.clusters[i]);
  }
  ClusterBuildOptions limits;
  limits.max_vertices = 255;
  limits.max_triangles = 512;
  std::string leaf_error;
  if (!validate_clusters(leaves, source_indices, limits, &leaf_error)) {
    if (error != nullptr) *error = "level 0: " + leaf_error;
    return false;
  }
  if (mesh.leaf_triangle_count != mesh.mesh.source_triangle_count)
    return fail("leaf triangle count mismatch");

  f32 max_finite_error = 0.0f;
  for (u32 i = 0; i < mesh.lod.size(); ++i) {
    const ClusterLodDesc& lod = mesh.lod[i];
    if (!(lod.own_error <= lod.parent_error)) return fail("own error exceeds parent error");
    if (lod.level == 0 && lod.own_error != 0.0f)
      return fail("level 0 cluster with nonzero own error");
    if (lod.group >= mesh.group_count) return fail("group index out of range");
    if (lod.level >= mesh.level_cluster_counts.size()) return fail("level out of range");
    if (lod.parent_error < k_lod_terminal_error && lod.parent_error > max_finite_error) {
      max_finite_error = lod.parent_error;
    }
  }

  // The raw cut is never empty and never grows with the threshold.
  Vector<u32> cut;
  u32 previous_triangles = ~u32{0};
  for (u32 step = 0; step <= 8; ++step) {
    cut.clear();
    const f32 threshold = max_finite_error * static_cast<f32>(step) / 8.0f * 1.001f;
    if (select_lod_raw(mesh, threshold, cut) == 0) return fail("empty cut");
    u32 triangles = 0;
    for (const u32 c : cut)
      triangles += mesh.mesh.clusters[c].triangle_count;
    if (triangles > previous_triangles) return fail("cut triangle count grew with the threshold");
    previous_triangles = triangles;
  }
  return true;
}

}  // namespace engine::geometry

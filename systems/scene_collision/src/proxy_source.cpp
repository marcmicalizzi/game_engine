// The collision's own copy of what its proxies are cut from (proxy_source.h;
// docs/subsystems/scene_collision.md, "The collision's own copy").

#include <core/log/log.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/flora_species.h>
#include <systems/renderer/flora_motion.h>
#include <systems/renderer/scene.h>
#include <systems/scene_collision/proxy_source.h>

#include <algorithm>
#include <cmath>

namespace engine::scene_collision {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_proxy_source, "scene_collision.copy");

// A margin on the largest scale a floor is set by: an instance's `scale_max` and the cut's own
// largest scale are the same column lengths, but not necessarily the same float operations, so the
// floor stands a hundredth below the threshold the largest placement asks for.
constexpr f32 k_floor_margin = 1.01f;

constexpr u16 k_unmapped = 0xFFFFu;

// The largest of a mesh fit's three column lengths: the scale every placement of the mesh carries
// before its own (`SceneData::mesh_fit`, which a streamed placement's linear part is multiplied
// by).
f32 fit_scale(const renderer::SceneData& scene, u32 index) noexcept {
  if (index >= scene.mesh_fit.size()) return 1.0f;
  const Mat4& fit = scene.mesh_fit[index];
  return std::max(length(fit.c[0].xyz()), std::max(length(fit.c[1].xyz()), length(fit.c[2].xyz())));
}

}  // namespace

void ProxySource::clear() noexcept {
  meshes_.clear();
  clusters_.clear();
  emptied_.clear();
  points_.clear();
  sources_.clear();
  triangles_.clear();
  parts_.clear();
  counts_ = Counts{};
}

u32 ProxySource::add(const geometry::ClusterLodMesh& lod, const geometry::ClusterMeshPart& part,
                     const renderer::SourceMesh* tree, f32 least) {
  const geometry::ClusterMesh& merged = lod.mesh;
  Mesh out;
  out.first_cluster = clusters_.size();
  out.first_emptied = emptied_.size();
  out.floor = least;
  out.copied = true;
  const bool has_lod = lod.lod.size() == merged.clusters.size();
  // **A tree collides by its wood** (scene_collision.md, "What gets no body"), decided here once
  // rather than at every cut: it depends on the mesh, never on the scale.
  const std::span<const geometry::ClusterFileStructuralPart> structure =
      tree != nullptr ? std::span<const geometry::ClusterFileStructuralPart>(
                            tree->structural_parts.data(), tree->structural_parts.size())
                      : std::span<const geometry::ClusterFileStructuralPart>();
  const auto* species = tree != nullptr ? geometry::flora_species_info(tree->species) : nullptr;
  const auto kind = species != nullptr ? species->motion : geometry::FloraMotionKind::none;
  auto wooden = [&](u32 v) {
    const u32 at = v - part.first_vertex;
    return at < tree->motion_vertices.size() &&
           renderer::flora_part_collides(structure, tree->motion_vertices[at].part, kind);
  };
  u16 map[256];
  for (u32 c = 0; c < part.cluster_count; ++c) {
    const u32 index = part.first_cluster + c;
    if (index >= merged.clusters.size()) break;
    Cluster copy;
    if (has_lod) {
      const geometry::ClusterLodDesc& d = lod.lod[index];
      copy.own_error = d.own_error;
      copy.parent_error = d.parent_error;
    } else {
      if (c < part.first_leaf_cluster || c >= part.first_leaf_cluster + part.leaf_cluster_count)
        continue;
      copy.own_error = 0.0f;  // the leaves, at every threshold, as the cut takes them
      copy.parent_error = geometry::k_lod_terminal_error;
    }
    const geometry::ClusterDesc& cluster = merged.clusters[index];
    ++counts_.scene_clusters;
    counts_.scene_triangles += cluster.triangle_count;
    // No threshold at or above the floor selects a cluster whose parent's error is not above it.
    if (!(copy.parent_error > least)) continue;
    // A scene whose streams were released has none to copy (its log line says so).
    if (u64{cluster.triangle_offset} + cluster.triangle_count > merged.triangles.size() ||
        u64{cluster.vertex_offset} + cluster.vertex_count > merged.vertices.size())
      continue;
    copy.first_point = points_.size();
    copy.first_triangle = triangles_.size();
    std::fill(map, map + 256, k_unmapped);
    for (u32 t = 0; t < cluster.triangle_count; ++t) {
      const u32 packed = merged.triangles[cluster.triangle_offset + t];
      const u32 local[3] = {packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu};
      if (tree != nullptr &&
          !(wooden(cluster.vertex_offset + local[0]) && wooden(cluster.vertex_offset + local[1]) &&
            wooden(cluster.vertex_offset + local[2]))) {
        ++copy.left_out;
        continue;
      }
      u32 kept = 0;
      for (u32 k = 0; k < 3; ++k) {
        if (map[local[k]] == k_unmapped) {
          const u32 v = cluster.vertex_offset + local[k];
          map[local[k]] = static_cast<u16>(points_.size() - copy.first_point);
          points_.push_back(merged.vertices[v]);
          sources_.push_back(v < merged.vertex_source.size() ? merged.vertex_source[v] : v);
        }
        kept |= u32{map[local[k]]} << (8 * k);
      }
      triangles_.push_back(kept);
      const u32 first = cluster.vertex_offset + local[0] - part.first_vertex;
      parts_.push_back(tree != nullptr && first < tree->motion_vertices.size()
                           ? tree->motion_vertices[first].part
                           : u16{0});
    }
    copy.triangle_count = triangles_.size() - copy.first_triangle;
    if (copy.triangle_count == 0) {
      // No points were copied for it: only its count, if a tree's wood left anything out.
      if (copy.left_out != 0) {
        emptied_.push_back(Emptied{copy.own_error, copy.parent_error, copy.left_out});
        ++counts_.emptied;
      }
      continue;
    }
    counts_.triangles += copy.triangle_count;
    ++counts_.clusters;
    clusters_.push_back(copy);
  }
  out.cluster_count = clusters_.size() - out.first_cluster;
  out.emptied_count = emptied_.size() - out.first_emptied;
  ++counts_.meshes;
  meshes_.push_back(out);
  return meshes_.size() - 1;
}

void ProxySource::build(const renderer::SceneData& scene, f32 mesh_error_m) {
  clear();
  const geometry::ClusterMesh& merged = scene.lod.mesh;
  const u32 count = scene.parts.size();
  // What can have a body, and the largest scale it is placed at: the scene's own instances, but a
  // skinned one (it moves); a streamed entry's meshes, at the headroom over their fit. The terrain,
  // a water body and a far forest's tile get no body (scene_collision.md, "What gets no body").
  Vector<f32> largest;
  largest.resize(count, 0.0f);
  Vector<u8> placed;
  placed.resize(count, u8{0});
  for (u32 i = 0; i < scene.instances.size(); ++i) {
    const gfx::InstanceDesc& instance = scene.instances[i];
    if (instance.mesh >= count) continue;
    if (i < scene.instance_joints.size() && scene.instance_joints[i] != 0) continue;
    placed[instance.mesh] = 1;
    largest[instance.mesh] = std::max(largest[instance.mesh], instance.scale_max);
  }
  for (const renderer::StreamedPlacements& source : scene.streamed) {
    for (const u32 m : source.meshes) {
      if (m >= count) continue;
      placed[m] = 1;
      largest[m] = std::max(largest[m], k_copy_scale_headroom * fit_scale(scene, m));
    }
  }
  if (scene.terrain_mesh < count) placed[scene.terrain_mesh] = 0;
  for (const renderer::FarForestTile& far : scene.far_forest.tiles)
    if (far.mesh < count) placed[far.mesh] = 0;
  for (u32 m = 0; m < count && m < scene.mesh_info.size(); ++m)
    if (scene.mesh_info[m].water.has_value()) placed[m] = 0;
  counts_.scene_float_bytes = u64{merged.vertices.size()} * sizeof(Vec3) +
                              u64{merged.triangles.size()} * sizeof(u32) +
                              u64{merged.vertex_source.size()} * sizeof(u32);
  for (u32 m = 0; m < count; ++m) {
    if (placed[m] == 0) {
      meshes_.push_back(Mesh{});
      continue;
    }
    const f32 scale = largest[m] > 0.0f ? largest[m] : k_copy_scale_headroom;
    const f32 least = mesh_error_m > 0.0f ? mesh_error_m / (scale * k_floor_margin) : 0.0f;
    const renderer::SourceMesh* tree =
        m < scene.sources.size() && scene.sources[m].has_motion() ? &scene.sources[m] : nullptr;
    (void)add(scene.lod, scene.parts[m], tree, least);
  }
  meshes_.shrink_to_fit();
  clusters_.shrink_to_fit();
  emptied_.shrink_to_fit();
  points_.shrink_to_fit();
  sources_.shrink_to_fit();
  triangles_.shrink_to_fit();
  parts_.shrink_to_fit();
  if (merged.vertices.empty() && !merged.clusters.empty()) {
    ENGINE_LOG_WARN(log_proxy_source, "the scene's float geometry was released before the copy",
                    log::field("meshes", count),
                    log::field("consequence", "placements have no collision"));
  }
  ENGINE_LOG_INFO(
      log_proxy_source, "the collision's own copy", log::field("bytes", bytes()),
      log::field("scene_float_bytes", counts_.scene_float_bytes),
      log::field("meshes", counts_.meshes), log::field("clusters", counts_.clusters),
      log::field("emptied", counts_.emptied), log::field("of_clusters", counts_.scene_clusters),
      log::field("triangles", counts_.triangles),
      log::field("of_triangles", counts_.scene_triangles), log::field("mesh_error_m", mesh_error_m),
      log::field("headroom", k_copy_scale_headroom));
}

u64 ProxySource::bytes() const noexcept {
  return u64{meshes_.capacity()} * sizeof(Mesh) + u64{clusters_.capacity()} * sizeof(Cluster) +
         u64{emptied_.capacity()} * sizeof(Emptied) + u64{points_.capacity()} * sizeof(Vec3) +
         u64{sources_.capacity()} * sizeof(u32) + u64{triangles_.capacity()} * sizeof(u32) +
         u64{parts_.capacity()} * sizeof(u16);
}

}  // namespace engine::scene_collision

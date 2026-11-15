// **What a place holds resident, by kind** (T138; docs/experiments/streaming-by-travel.md, row a;
// docs/subsystems/renderer.md, "What a place holds"). Counters only: every number is a buffer this
// scene already owns, read where it stands, so asking costs a walk over the members and no device
// call. Its own file so that the hub `gpu_scene.cpp` does not grow for a report.

#include <systems/renderer/gpu_scene.h>

#include <initializer_list>

namespace engine::renderer {

namespace {

u64 sum(std::initializer_list<const gfx::BufferResource*> buffers) noexcept {
  u64 total = 0;
  for (const gfx::BufferResource* b : buffers) total += b->size;
  return total;
}

}  // namespace

GpuScene::PlaceBytes GpuScene::place_bytes() const noexcept {
  PlaceBytes out;
  // The geometry buffers hold the load's meshes, then the arenas a world's tiles arrive into (the
  // mesh room and the terrain's rings, `vertex_capacity_`'s "scene + arenas"): the arenas are the
  // tiles' kind, the rest the load's.
  const u64 geometry_buffers =
      sum({&clusters, &quantized, &vertices, &triangles, &lods, &attributes, &meshes});
  const u64 arenas = mesh_room_.bytes + terrain_ring_bytes_;
  out.geometry = geometry_buffers > arenas ? geometry_buffers - arenas : 0u;
  out.companions = sum({&indices8, &skin, &lod_coverage, &shadow_coverage, &flora_motion_,
                        &morph_channels, &morph_directory, &morph_slices, &morph_indices,
                        &morph_deltas, &morph_normals, &static_cache, &flora_field_parts_,
                        &clas_templates.data});
  out.micromaps = micromap_bytes();
  out.materials = sum({&materials, &cluster_materials, &terrain_masks, &terrain_formation});
  out.textures = texture_bytes_;
  out.structures = sum({&clas_set.addresses, &clas_set.sizes, &blas_set.data, &blas_set.addresses,
                        &rt_scratch, &tlas.buffer, &rt_instances, &records, &hit_entries, &slots,
                        &instance_counts, &blas_records, &blas_dense, &blas_slots,
                        &clas_chunk_counts, &static_clas_.data, &static_blas_.data}) +
                   rt_cache_bytes_;
  for (const gfx::ClusterSet& chunk : clas_chunks_) out.structures += chunk.data.size;
  out.tiles = arenas + staging_ring_.size + terrain_table_.size;
  for (const TerrainLevel& level : terrain_)
    for (const gfx::BufferResource& field : level.fields) out.tiles += field.size;
  out.frame = sum({&instances, &fade, &visible, &pair_entries, &pair_table, &draw_args[0],
                   &draw_args[1], &sw_args, &shadow_args, &flags[0], &flags[1], &vertex_headers,
                   &vertex_records, &vertex_indices, &deform_pool, &deform_slots, &deform_blocks,
                   &deform_table, &joints, &deform_frames, &morph_weights, &deform_normals,
                   &flora_poses_, &ring_pairs_});
  out.pages = sum({&page_table, &page_of_cluster, &page_children, &residency, &page_used,
                   &page_requests, &page_stage});
  if (data_ != nullptr) {
    const geometry::ClusterMesh& mesh = data_->lod.mesh;
    out.host_geometry = u64{mesh.clusters.capacity()} * sizeof(geometry::ClusterDesc) +
                        u64{data_->lod.lod.capacity()} * sizeof(geometry::ClusterLodDesc) +
                        u64{mesh.vertices.capacity()} * sizeof(Vec3) +
                        u64{mesh.quantized.capacity()} * sizeof(u16) +
                        u64{mesh.attributes.capacity()} * sizeof(geometry::VertexAttributes) +
                        u64{mesh.triangles.capacity()} * sizeof(u32) +
                        u64{mesh.vertex_source.capacity()} * sizeof(u32) +
                        u64{mesh.vertex_ids.capacity()} * sizeof(u32) +
                        u64{mesh.skin.capacity()} * sizeof(geometry::SkinBinding);
  }
  return out;
}

}  // namespace engine::renderer

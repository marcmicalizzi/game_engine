// **What a place holds resident, by kind** (T138; docs/experiments/streaming-by-travel.md, row a;
// docs/subsystems/renderer.md, "What a place holds"). Counters only: every number is a buffer this
// scene already owns, read where it stands, so asking costs a walk over the members and no device
// call. Its own file so that the hub `gpu_scene.cpp` does not grow for a report.

#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/shadow_cascades.h>

#include <initializer_list>
#include <renderer_log.h>

namespace engine::renderer {

// **What the renderer reads from the host's float positions and triangles, measured at `create`**
// (T138; renderer.md, "The scene's host copy after the upload"), so that a host may give them back
// once `create` returns: the crowns' priors where the indirect light will read them, and the meshes
// that face the sky. And the load's clusters by the triangles each holds (renderer.md, "The ray
// tracing chain's memory"): what a second cluster set reserved for few triangles would take, by
// population, since the driver reserves a structure by the most triangles its set allows.
void GpuScene::measure_host_reads(const SceneData& data, const ResolvedSettings& resolved) {
  if (resolved.gi_mode != GiMode::Off && resolved.gi_crown_prior) {
    build_crown_priors(data, crown_priors_);
    crown_priors_built_ = true;
  }
  sky_facing_count_ = sky_facing_meshes(data.lod.mesh, data.parts, sky_facing_);
  u32 bins[5] = {};  // up to 32, 48, 64 and 96 triangles, and more
  for (const geometry::ClusterDesc& c : data.lod.mesh.clusters) {
    const u32 t = c.triangle_count;
    ++bins[t <= 32u ? 0 : t <= 48u ? 1 : t <= 64u ? 2 : t <= 96u ? 3 : 4];
  }
  ENGINE_LOG_INFO(log_renderer, "clusters by triangles", log::field("upto_32", bins[0]),
                  log::field("upto_48", bins[1]), log::field("upto_64", bins[2]),
                  log::field("upto_96", bins[3]), log::field("more", bins[4]));
}

// Every swaying mesh's sway bound, measured at the upload from the float positions (T138): a tail
// tree may first appear long after it, by when a host that uploads once has given them back
// (`release_uploaded_streams`). `flora_padding_of` only reads the table.
//
// **The table holds the calm rung's** (W154-2): main's padding to the bit, at the load the sway was
// held at before W154; a rung above it is that times the rung's load over it
// (`flora_rung_padding`), so a step reads no vertex. Measured here too, what the ladder's top is:
// the strongest load any of the scene's meshes can take, and the steepest law among its trees,
// which the ceiling's load is taken by.
void GpuScene::measure_flora_padding() {
  flora_padding_.assign(flora_meshes_.size(), 0.0f);
  flora_ladder_top_ = k_flora_calm_load;
  flora_streamline_max_ = 1.0f;
  flora_hold_max_ = 0.0f;
  flora_has_field_ = false;
  flora_field_hold_ = k_flora_max_wind;
  const geometry::ClusterMesh& mesh = data_->lod.mesh;
  for (u32 m = 0; m < flora_meshes_.size() && m < data_->sources.size(); ++m) {
    const FloraMesh& tree = flora_meshes_[m];
    if (tree.part_count() == 0) continue;
    const SourceMesh& source = data_->sources[m];
    flora_padding_[m] = flora_padding_at_load(
        tree,
        std::span<const Vec3>(mesh.vertices.data() + data_->parts[m].first_vertex,
                              source.motion_vertices.size()),
        std::span<const geometry::ClusterFileMotionVertex>(source.motion_vertices.data(),
                                                           source.motion_vertices.size()),
        k_flora_calm_load);
    if (tree.field) {
      flora_has_field_ = true;
      flora_field_hold_ = std::min(flora_field_hold_, tree.hold);
      flora_ladder_top_ = std::max(flora_ladder_top_, flora_field_load(tree.hold, tree.hold));
    } else {
      flora_streamline_max_ = std::max(flora_streamline_max_, tree.streamline);
      flora_hold_max_ = std::max(flora_hold_max_, tree.hold);
      flora_ladder_top_ =
          std::max(flora_ladder_top_, flora_storm_load(tree.hold, tree.streamline, tree.hold));
    }
  }
  flora_ladder_ = FloraLadder{};
  ladder_walking_ = false;
}

namespace {

u64 sum(std::initializer_list<const gfx::BufferResource*> buffers) noexcept {
  u64 total = 0;
  for (const gfx::BufferResource* b : buffers)
    total += b->size;
  return total;
}

}  // namespace

PlaceBytes GpuScene::place_bytes() const noexcept {
  PlaceBytes out;
  // The geometry buffers hold the load's meshes, then the arenas a world's tiles arrive into (the
  // mesh room and the terrain's rings, `vertex_capacity_`'s "scene + arenas"): the arenas are the
  // tiles' kind, the rest the load's.
  const u64 geometry_buffers =
      sum({&clusters, &quantized, &vertices, &triangles, &lods, &attributes, &meshes});
  const u64 arenas = mesh_room_.bytes + terrain_ring_bytes_;
  out.geometry = geometry_buffers > arenas ? geometry_buffers - arenas : 0u;
  out.companions =
      sum({&indices8, &skin, &lod_coverage, &shadow_coverage, &flora_motion_, &morph_channels,
           &morph_directory, &morph_slices, &morph_indices, &morph_deltas, &morph_normals,
           &static_cache, &flora_field_parts_, &clas_templates.data});
  out.micromaps = micromap_bytes();
  out.materials = sum({&materials, &cluster_materials, &terrain_masks, &terrain_formation});
  out.textures = texture_bytes_;
  // The traced chain by what sizes each part (R81): the same buffers `structures` always summed.
  StructureBytes& s = out.structure;
  for (const gfx::ClusterSet& chunk : clas_chunks_)
    s.frame_sets += chunk.data.size;
  s.frame_tables =
      sum({&records, &hit_entries, &clas_set.addresses, &clas_set.sizes, &clas_chunk_counts});
  s.bottom_level = sum({&blas_set.data, &blas_set.addresses});
  s.top_level = sum({&tlas.buffer, &rt_instances});
  s.scratch = rt_scratch.size;
  s.per_instance = sum({&slots, &instance_counts, &blas_records, &blas_dense, &blas_slots});
  s.cache = rt_cache_bytes_;
  s.cache_slots = rt_cache_buffers.slots.size;
  s.static_forms = sum({&static_clas_.data, &static_blas_.data});
  out.structures = s.total();
  out.tiles = arenas + staging_ring_.size + terrain_table_.size;
  for (const TerrainLevel& level : terrain_)
    for (const gfx::BufferResource& field : level.fields)
      out.tiles += field.size;
  out.frame =
      sum({&instances,     &fade,           &visible,        &pair_entries,   &pair_table,
           &draw_args[0],  &draw_args[1],   &sw_args,        &shadow_args,    &flags[0],
           &flags[1],      &vertex_headers, &vertex_records, &vertex_indices, &deform_pool,
           &deform_slots,  &deform_blocks,  &deform_table,   &joints,         &deform_frames,
           &morph_weights, &deform_normals, &flora_poses_,   &ring_pairs_});
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

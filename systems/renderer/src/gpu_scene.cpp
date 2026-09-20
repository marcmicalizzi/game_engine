#include <domain/geometry/cluster_lod.h>
#include <foundation/image/decode.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/gpu_scene.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <renderer_log.h>

namespace engine::renderer {

namespace {

constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
constexpr VkBufferUsageFlags k_address = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
constexpr VkBufferUsageFlags k_args = k_address | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
// The visible list is read back by a capture, which resolves a visibility id into the instance
// and cluster it names; that is the only reason it is a transfer source.
constexpr VkBufferUsageFlags k_readable = k_address | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
// The streaming feedback arrays: cleared by the frame's reset pass and copied back to the host
// one frame slot later, so both directions are transfers.
constexpr VkBufferUsageFlags k_transfer =
    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

// The budget, in vertices. `deform_pool_kib` is what the caller asked for (0 is the default); the
// clamp **down** to `whole_mesh_vertices` is what keeps a single character costing exactly what it
// cost before the suballocation existed, and makes overflow impossible on any scene small enough
// for the layout E25 built. Both ends are stated rather than hidden (ADR-0017): the summary
// reports the pool bytes and what the whole-mesh layout would have taken beside them.
u32 pool_budget_vertices(const ResolvedSettings& resolved, u64 whole_mesh_vertices) noexcept {
  u64 kib = resolved.settings.deform_pool_kib > 0 ? u64{resolved.settings.deform_pool_kib}
                                                  : u64{k_default_deform_pool_kib};
  if (kib < k_min_deform_pool_kib) kib = k_min_deform_pool_kib;
  u64 vertices = kib * 1024 / (3 * sizeof(f32));
  if (vertices < 1) vertices = 1;
  if (vertices > whole_mesh_vertices) vertices = whole_mesh_vertices;
  return static_cast<u32>(vertices);
}

}  // namespace

GpuScene::~GpuScene() { destroy(); }

bool GpuScene::create(const gfx::Device& device, const SceneData& data,
                      const ResolvedSettings& resolved, std::string* error) {
  destroy();
  device_ = &device;
  data_ = &data;
  ray_tracing_ = resolved.rt_chain;
  // A scene deforms when the settings say every instance does, or when any instance is skinned.
  // The two are independent: `--deform wave` on a scene with a skinned character deforms the
  // rigid instances procedurally and skins the skinned one, because a `DeformDesc` names one kind
  // per instance and the skinned instance's kind is skinning.
  skinned_ = data.skinned();
  skinned_instances_ = skinned_ ? data.skinned_instances : 0;
  max_joints_ = skinned_ ? data.max_joints : 0;
  deform_ = resolved.deform_pass;
  // `resolve_settings` has already refused streaming for a scene with no page table, but a caller
  // may hand the two apart; a scene that cannot be streamed is uploaded whole rather than half.
  streamed_ = resolved.stream && data.paged();
  page_count_ = streamed_ ? data.pages.pages.size() : 0u;
  view_count_ = resolved.view_count > 0 ? resolved.view_count : 1;
  cluster_count_ = data.cluster_count();
  leaf_count_ = data.leaf_count();
  instance_count_ = data.instances.size();
  pair_count_ = data.pair_count;
  triangles_per_cluster_ = geometry::ClusterLodOptions{}.max_triangles;
  instance_table_ = data.instances;

  if (!bindless_.create(device, gfx::BindlessConfig{}, error) ||
      !create_streaming(resolved, error) || !upload_geometry(resolved, error) ||
      !upload_materials(resolved, error) || !create_working_set(resolved, error) ||
      !create_ray_tracing(resolved, error)) {
    destroy();
    return false;
  }

  // One MeshDesc per mesh: its own grid, its range of the shared cluster array, the one
  // quantized stream every mesh of the scene indexes, and where a deformed instance's positions
  // come from instead — the frame's pool, the deform table, and this mesh's cluster templates.
  // It is uploaded last because those three addresses have to exist first.
  Vector<gfx::MeshDesc> mesh_descs;
  for (const geometry::ClusterMeshPart& part : data.parts) {
    gfx::MeshDesc desc{};
    desc.quant = Vec4{part.quant_origin, part.quant_scale};
    desc.quantized = quantized.address;
    desc.first_cluster = part.first_cluster;
    desc.cluster_count = part.cluster_count;
    desc.deform_pool = deform_pool.address;
    desc.deform_slots = deform_slots.address;
    desc.templates = clas_templates.addresses.address;
    desc.skin = skin.address;
    mesh_descs.push_back(desc);
  }
  if (!gfx::upload_buffer(device, mesh_descs.data(), mesh_descs.size() * sizeof(gfx::MeshDesc),
                          k_storage, meshes, error)) {
    destroy();
    return false;
  }
  return true;
}

bool GpuScene::upload_geometry(const ResolvedSettings& resolved, std::string* error) {
  const gfx::Device& device = *device_;
  const geometry::ClusterLodMesh& lod = data_->lod;
  // Positions go to the GPU on the mesh-wide 16-bit grid: six bytes a vertex instead of twelve.
  const u64 float_position_bytes = u64{lod.mesh.vertices.size()} * sizeof(Vec3);
  const u64 quantized_position_bytes =
      u64{lod.mesh.quantized.size()} * sizeof(u16) + sizeof(gfx::MeshDesc);
  // The descriptors are scene-sized whatever the budget — the cull pass tests every pair of every
  // frame — and under streaming the `ClusterDesc` array is also what a page upload **patches**:
  // when a page lands in a slot its clusters' `vertex_offset` and `triangle_offset` are rewritten
  // to point into that slot, so the copy that puts the array here is a transfer destination.
  const VkBufferUsageFlags cluster_usage =
      streamed_ ? (k_storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT) : k_storage;
  if (!gfx::upload_buffer(device, lod.mesh.clusters.data(),
                          u64{cluster_count_} * sizeof(geometry::ClusterDesc), cluster_usage,
                          clusters, error) ||
      !gfx::upload_buffer(device, lod.lod.data(),
                          u64{cluster_count_} * sizeof(geometry::ClusterLodDesc), k_storage, lods,
                          error)) {
    return false;
  }
  // Everything below this line is a *payload* stream: under streaming it is a page pool of
  // fixed-size slots that `create_streaming` already allocated, and nothing is uploaded here.
  if (!streamed_ && (!gfx::upload_buffer(device, lod.mesh.quantized.data(),
                                         u64{lod.mesh.quantized.size()} * sizeof(u16), k_storage,
                                         quantized, error) ||
                     !gfx::upload_buffer(device, lod.mesh.triangles.data(),
                                         u64{lod.mesh.triangles.size()} * sizeof(u32), k_storage,
                                         triangles, error))) {
    return false;
  }
  // The deformed-vertex pool, its per-instance table, and the per-entry allocation table. The
  // pool is device-local: nothing reads it back, and the cluster acceleration structure builds
  // take it as a build input.
  //
  // **The pool is a budget, not a sum over the population.** E25 gave every deformed instance a
  // block as long as its mesh's whole cluster-ordered vertex range — which is what made the pool
  // the binding memory constraint, 12.2 MB for 1,024 foxes and 1.7 GB if they had been
  // FlightHelmets, to write the ~1,300 clusters a frame draws. What is allocated here is
  // `RenderSettings::deform_pool_kib`, clamped **down** to what the whole-mesh layout would have
  // needed, so a single character costs what it always did and a scene small enough for that
  // layout can never overflow. `deform_alloc.slang` suballocates it per frame from the cull pass's
  // own visible list, one block per (instance, cluster) pair.
  if (deform_) {
    const u32 total_vertices = lod.mesh.vertices.size();
    u64 whole_mesh_vertices = 0;
    for (u32 i = 0; i < instance_count_; ++i) {
      // A skinned instance is deformed whatever the settings say; a rigid one only under
      // `--deform`, so a scene of one character and a hundred props allocates pool blocks for the
      // character alone and every prop stays on the instruction-for-instruction rigid path.
      const u32 instance_joints = skinned_ ? data_->instance_joints[i] : 0u;
      const geometry::ClusterMeshPart& part = data_->parts[instance_table_[i].mesh];
      if (instance_joints == 0 && part.morph_channel_count == 0 && !resolved.settings.deform)
        continue;
      const u32 next = instance_table_[i].mesh + 1 < data_->parts.size()
                           ? data_->parts[instance_table_[i].mesh + 1].first_vertex
                           : total_vertices;
      gfx::DeformDesc desc{};
      // The chain's stage mask. A skinned instance runs the skinning stage; a `--deform` one runs
      // the procedural stage with the kind the flag named. They are separate bits, so an instance
      // that is both runs both — which is the thing E25 said needed a wider field.
      desc.stages =
          (instance_joints > 0 ? gfx::k_deform_stage_skin : 0u) |
          (resolved.settings.deform ? gfx::k_deform_stage_procedural | resolved.settings.deform_kind
                                    : 0u) |
          (part.morph_channel_count > 0 ? gfx::k_deform_stage_static | gfx::k_deform_stage_pose
                                        : 0u);
      desc.first_vertex = part.first_vertex;
      desc.first_channel = part.first_morph_channel;
      desc.channel_count = part.morph_channel_count;
      deform_mesh_clusters_.push_back(part.cluster_count);
      // `joints` and `joint_count` stay zero in the *static* table: they are what a frame fills
      // in, in its own copy. A frame that hands over no matrices therefore leaves the instance at
      // its rest pose rather than reading an address from a previous frame.
      whole_mesh_vertices += next - part.first_vertex;
      instance_table_[i].deform = deform_descs_.size();
      deform_descs_.push_back(desc);
      deform_instance_.push_back(i);
    }
    deform_whole_mesh_bytes_ = whole_mesh_vertices * 3 * sizeof(f32);
    deform_pool_vertices_ = pool_budget_vertices(resolved, whole_mesh_vertices);
    deform_pool_bytes_ = u64{deform_pool_vertices_} * 3 * sizeof(f32);
    constexpr VkBufferUsageFlags k_pool_usage =
        k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    // One indirect dispatch block per (view, run): every view's pool pass covers its own cut, and
    // a cluster two views both draw gets a block in each, written twice with the same value.
    bool ok =
        gfx::create_buffer(device, deform_pool_bytes_, k_pool_usage, false, deform_pool, error) &&
        gfx::create_buffer(device, u64{visible_entries()} * sizeof(u32), k_address, false,
                           deform_slots, error) &&
        gfx::create_buffer(
            device, sizeof(gfx::DeformAlloc),
            k_address | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false,
            deform_alloc, error) &&
        gfx::upload_buffer(device, deform_descs_.data(),
                           deform_descs_.size() * sizeof(gfx::DeformDesc), k_storage, deform_table,
                           error) &&
        gfx::create_buffer(device, u64{gfx::k_draw_args_bytes} * k_visible_runs * view_count_,
                           k_args, false, deform_args, error);
    // The per-frame side of skinning: `k_joint_slots` regions of bone matrices and the same
    // number of copies of the deform table, both host-visible and persistently mapped, so a tick
    // is one memcpy of the span plus one rewrite of a table of 24-byte records. Nothing here is
    // touched again after `create`.
    if (ok && skinned_) {
      const u64 joint_region = joint_bytes();
      ok = gfx::create_buffer(device, joint_region * k_joint_slots, k_address, true, joints,
                              error) &&
           gfx::create_buffer(device,
                              u64{deform_descs_.size()} * sizeof(gfx::DeformDesc) * k_joint_slots,
                              k_address, true, deform_frames, error);
      if (ok) {
        // Every region starts as the static table, so a frame only ever rewrites the two words
        // that change and a slot that has never been written is still a valid rest pose.
        for (u32 slot = 0; slot < k_joint_slots; ++slot) {
          std::memcpy(deform_frame(slot), deform_descs_.data(),
                      deform_descs_.size() * sizeof(gfx::DeformDesc));
        }
        std::memset(joints.mapped, 0, joint_region * k_joint_slots);
      }
    }
    if (ok && !lod.mesh.morph_channels.empty()) ok = create_morph(resolved, error);
    if (!ok) return false;
    ENGINE_LOG_INFO(
        log_renderer, "deformed-vertex pool", log::field("mode", deform_name(resolved.settings)),
        log::field("instances", deform_descs_.size()),
        log::field("skinned_instances", skinned_instances_), log::field("max_joints", max_joints_),
        log::field("pool_vertices", deform_pool_vertices_),
        log::field("pool_bytes", deform_pool_bytes_),
        log::field("whole_mesh_bytes", deform_whole_mesh_bytes_),
        log::field("slot_bytes", u64{visible_entries()} * sizeof(u32)),
        log::field("joint_bytes", skinned_ ? joint_bytes() * k_joint_slots : u64{0}));
  }
  // The float positions stay only for the frames that build acceleration structures: the
  // cluster structure builds read them. Under streaming they are one more page-pool stream, for
  // the reason the patched offsets force: a CLAS record addresses a cluster's vertices as
  // `vertices + vertex_offset * 12`, and `vertex_offset` is slot-relative.
  if (ray_tracing_ && !streamed_ &&
      !gfx::upload_buffer(device, lod.mesh.vertices.data(), float_position_bytes,
                          k_storage | gfx::k_build_input_usage, vertices, error)) {
    return false;
  }
  ENGINE_LOG_INFO(log_renderer, "positions quantized",
                  log::field("vertices", lod.mesh.vertices.size()),
                  log::field("float_bytes", float_position_bytes),
                  log::field("quantized_bytes", quantized_position_bytes),
                  log::field("grid_step", lod.mesh.quant_scale));
  // The per-vertex binding stream, when the scene has one. It is the *mesh's* — cluster-ordered
  // and parallel to the positions, so one address serves every mesh of the scene exactly as the
  // quantized stream does, and a crowd of a hundred characters built from one mesh shares it.
  // Only the bone matrices are per instance (`DeformDesc::joints`).
  if (skinned_ && !gfx::upload_buffer(device, lod.mesh.skin.data(),
                                      u64{lod.mesh.skin.size()} * sizeof(geometry::SkinBinding),
                                      k_storage, skin, error)) {
    return false;
  }
  if (streamed_) return true;
  return gfx::upload_buffer(device, lod.mesh.attributes.data(),
                            u64{lod.mesh.attributes.size()} * sizeof(geometry::VertexAttributes),
                            k_storage, attributes, error);
}

// What one page costs the pool, stream by stream, which is what an upload copies and what the
// per-frame byte budget counts. The `ClusterDesc` records are in it because they are copied with
// the payload — they carry the slot-relative offsets and are useless without it.
//
// **Every sub-block is 16-byte aligned**, and the padding is counted, because this number is both
// the staging ring's layout and the floor the upload budget is raised to: a budget computed
// without the padding would be a byte or two short of the largest page, which is a page that never
// loads and a scene that never converges. One formula, used by both, is the only way those two
// cannot drift apart.
GpuScene::PageStage GpuScene::page_stage_layout(u32 page) const noexcept {
  PageStage out;
  if (!streamed_ || page >= page_count_) return out;
  const geometry::ClusterPageDesc& desc = data_->pages.pages[page];
  auto align16 = [](u64 value) { return (value + 15) & ~u64{15}; };
  out.ray_tracing = ray_tracing_;
  out.clusters = 0;
  out.quantized = align16(u64{desc.cluster_count} * sizeof(geometry::ClusterDesc));
  out.attributes = align16(out.quantized + u64{desc.vertex_count} * 3 * sizeof(u16));
  out.triangles =
      align16(out.attributes + u64{desc.vertex_count} * sizeof(geometry::VertexAttributes));
  out.total = align16(out.triangles + u64{desc.triangle_count} * sizeof(u32));
  if (!ray_tracing_) return out;
  // The float positions a CLAS build reads, and their 8-bit indices.
  out.vertices = out.total;
  out.indices8 = align16(out.vertices + u64{desc.vertex_count} * sizeof(Vec3));
  out.total = align16(out.indices8 + u64{desc.triangle_count} * 3);
  return out;
}

u32* GpuScene::residency_slot(u32 slot) noexcept {
  return static_cast<u32*>(residency.mapped) + u64{slot} * page_count_;
}

u64 GpuScene::residency_slot_address(u32 slot) const noexcept {
  return residency.address + u64{slot} * page_count_ * sizeof(u32);
}

u64 GpuScene::stream_params_address(u32 slot) const noexcept {
  return stream_params.address + u64{slot} * sizeof(gfx::StreamParams);
}

// The page pool and the tables the drawing rule reads. Runs before `upload_geometry`, because that
// is where the payload streams either go up whole or do not go up at all.
//
// **How many slots.** The budget is in page bytes, the pool is in slots, and the two have to agree
// on the worst case or a frame would admit a page with nowhere to put it. The bound is exact and
// cheap: sort the pages by size and take them smallest first until the budget is spent — no set of
// pages within the budget can be larger than that count. The pages of a real mesh are 85–95% full,
// so the pool is about a tenth larger than the budget it serves, which is the price of a
// fixed-size slot and is what makes an eviction a slot that can be reused without touching
// anything else.
// The morph stream and the chain's two morph stages (geometry.md, "Morph channels"; gfx.md, "The
// deform chain"). Three things are created here and only here:
//
//   - the stream's six arrays and the `gfx::MorphParams` block that names them. They are the
//     *scene's*: `merge_cluster_meshes` concatenated every mesh's channels and keyed the slices
//     by the global cluster index, so one block serves every mesh and every view of a frame.
//   - one weights region per frame slot, `2 * channels` floats per deformed instance. The static
//     half is written once here and again whenever the settings' weights change; the pose half is
//     what a frame writes from `FrameDesc::morph_weights`.
//   - the static shape caches, handed out in instance order until the budget runs out. An
//     instance that gets none keeps `DeformDesc::cache` at zero and runs its static stage every
//     frame over the cut — the same answer for more work, which is the same graceful degradation
//     the pool's `k_no_pool_slot` gives.
bool GpuScene::create_morph(const ResolvedSettings& resolved, std::string* error) {
  const gfx::Device& device = *device_;
  const geometry::ClusterMesh& mesh = data_->lod.mesh;
  morph_channel_count_ = mesh.morph_channels.size();
  if (morph_channel_count_ == 0 || deform_descs_.empty()) return true;

  if (!gfx::upload_buffer(device, mesh.morph_channels.data(),
                          u64{morph_channel_count_} * sizeof(geometry::MorphChannel), k_storage,
                          morph_channels, error) ||
      !gfx::upload_buffer(device, mesh.morph_cluster_slices.data(),
                          u64{mesh.morph_cluster_slices.size()} * sizeof(u32), k_storage,
                          morph_directory, error) ||
      !gfx::upload_buffer(device, mesh.morph_slices.data(),
                          u64{mesh.morph_slices.size()} * sizeof(geometry::MorphSlice), k_storage,
                          morph_slices, error) ||
      !gfx::upload_buffer(device, mesh.morph_indices.data(), mesh.morph_indices.size(), k_storage,
                          morph_indices, error) ||
      !gfx::upload_buffer(device, mesh.morph_deltas.data(),
                          u64{mesh.morph_deltas.size()} * sizeof(i16), k_storage, morph_deltas,
                          error)) {
    return false;
  }
  if (!mesh.morph_normal_deltas.empty() &&
      !gfx::upload_buffer(device, mesh.morph_normal_deltas.data(),
                          u64{mesh.morph_normal_deltas.size()} * sizeof(i16), k_storage,
                          morph_normals, error)) {
    return false;
  }
  gfx::MorphParams params{};
  params.channels = morph_channels.address;
  params.directory = morph_directory.address;
  params.slices = morph_slices.address;
  params.indices = morph_indices.address;
  params.deltas = morph_deltas.address;
  params.normal_deltas = mesh.morph_normal_deltas.empty() ? 0 : morph_normals.address;
  if (!gfx::upload_buffer(device, &params, sizeof(params), k_storage, morph_params, error))
    return false;

  // The deformed normal pool, parallel to the position pool: one octahedral word a vertex
  // against the position's twelve bytes, which is why carrying deformed normals costs a third of
  // what carrying deformed positions does.
  if (!gfx::create_buffer(device, u64{deform_pool_vertices_} * sizeof(u32), k_address, false,
                          deform_normals, error)) {
    return false;
  }
  const u64 weight_floats = morph_weight_floats();
  if (!gfx::create_buffer(device, weight_floats * sizeof(f32) * k_joint_slots, k_address, true,
                          morph_weights, error)) {
    return false;
  }
  std::memset(morph_weights.mapped, 0, weight_floats * sizeof(f32) * k_joint_slots);

  // The caches. Budget first, then hand out blocks in instance order.
  const u64 budget = u64{resolved.settings.static_shape_kib > 0 ? resolved.settings.static_shape_kib
                                                                : k_default_static_shape_kib} *
                     1024;
  const u32 total_vertices = data_->lod.mesh.vertices.size();
  Vector<u32> cache_offset(deform_descs_.size(), ~0u);
  u64 used = 0;
  for (u32 d = 0; d < deform_descs_.size(); ++d) {
    if ((deform_descs_[d].stages & gfx::k_deform_stage_static) == 0) continue;
    const u32 instance = deform_instance_[d];
    const geometry::ClusterMeshPart& part = data_->parts[instance_table_[instance].mesh];
    const u32 next = instance_table_[instance].mesh + 1 < data_->parts.size()
                         ? data_->parts[instance_table_[instance].mesh + 1].first_vertex
                         : total_vertices;
    const u64 bytes = u64{next - part.first_vertex} * sizeof(gfx::DeformCacheVertex);
    if (used + bytes > budget) continue;
    cache_offset[d] = static_cast<u32>(used / sizeof(gfx::DeformCacheVertex));
    used += bytes;
    ++static_cached_instances_;
  }
  static_cache_bytes_ = used;
  if (used > 0 && !gfx::create_buffer(device, used, k_address, false, static_cache, error)) {
    return false;
  }
  for (u32 d = 0; d < deform_descs_.size(); ++d) {
    gfx::DeformDesc& desc = deform_descs_[d];
    desc.weights = morph_weights.address;  // the frame overrides this with its own slot
    if (cache_offset[d] != ~0u) {
      desc.cache = static_cache.address + u64{cache_offset[d]} * sizeof(gfx::DeformCacheVertex);
    }
  }
  // The static weights, written into every slot's static half. They are the scene's, so they are
  // written once here and again only when the caller changes them.
  set_static_weights(resolved.settings.morph_static_weights);

  // **What a morphing instance needs from the cull pass**, exactly as a skinned one does: the
  // cluster spheres and both LOD spheres are the *rest* pose's, and a vertex a channel moves is
  // no longer inside them. `bounds_padding` is added to all three, so the bound here is
  // `geometry::morph_bounds_padding`'s — the sum over channels of |weight| x the channel's
  // largest displacement, which is the triangle inequality and nothing cleverer, because the
  // channels of a face move the same region in the same direction as often as not.
  //
  // The static half is known (the weights are the scene's); the **pose** half is not, so every
  // channel is priced at weight 1, which is the range a glTF weights track lives in. That is
  // conservative in the honest direction — a sphere too big draws a cluster that might have been
  // culled, a sphere too small drops a limb off the screen — and it is a per-instance float, so
  // tightening it later costs nothing to anyone.
  for (u32 d = 0; d < deform_descs_.size(); ++d) {
    const gfx::DeformDesc& desc = deform_descs_[d];
    if (desc.channel_count == 0) continue;
    f32 padding = 0.0f;
    for (u32 c = 0; c < desc.channel_count; ++c) {
      const geometry::MorphChannel& channel = mesh.morph_channels[desc.first_channel + c];
      const u32 at = desc.first_channel + c;
      const f32 weight = at < resolved.settings.morph_static_weights.size()
                             ? resolved.settings.morph_static_weights[at]
                             : 0.0f;
      const f32 magnitude = weight < 0.0f ? -weight : weight;
      padding += (magnitude + 1.0f) * channel.max_displacement;
    }
    gfx::InstanceDesc& instance = instance_table_[deform_instance_[d]];
    instance.bounds_padding += padding;
  }
  // Re-upload the table now that the caches and the weights are on it.
  gfx::destroy_buffer(device, deform_table);
  if (!gfx::upload_buffer(device, deform_descs_.data(),
                          deform_descs_.size() * sizeof(gfx::DeformDesc), k_storage, deform_table,
                          error)) {
    return false;
  }
  // A morphed scene needs the **per-frame** table even when nothing is skinned, because the pose
  // weights live in a per-slot region and the record has to point at this slot's. A skinned scene
  // already has one; this is the case where morphs alone force it.
  if (!has_frame_table() &&
      !gfx::create_buffer(device,
                          u64{deform_descs_.size()} * sizeof(gfx::DeformDesc) * k_joint_slots,
                          k_address, true, deform_frames, error)) {
    return false;
  }
  for (u32 slot = 0; slot < k_joint_slots; ++slot) {
    std::memcpy(deform_frame(slot), deform_descs_.data(),
                deform_descs_.size() * sizeof(gfx::DeformDesc));
  }
  ENGINE_LOG_INFO(log_renderer, "morph channels", log::field("channels", morph_channel_count_),
                  log::field("deltas", mesh.morph_delta_count),
                  log::field("slices", mesh.morph_slices.size()),
                  log::field("cached_instances", static_cached_instances_),
                  log::field("cache_bytes", static_cache_bytes_),
                  log::field("normal_pool_bytes", u64{deform_pool_vertices_} * sizeof(u32)));
  return true;
}

void GpuScene::set_static_weights(std::span<const f32> weights) {
  if (morph_channel_count_ == 0 || morph_weights.mapped == nullptr) return;
  const u64 per_slot = morph_weight_floats();
  for (u32 slot = 0; slot < k_joint_slots; ++slot) {
    f32* region = morph_weight_slot(slot);
    for (u32 d = 0; d < deform_descs_.size(); ++d) {
      const u32 first = deform_descs_[d].first_channel;
      const u32 count = deform_descs_[d].channel_count;
      f32* statics = region + u64{d} * 2 * morph_channel_count_;
      for (u32 c = 0; c < count; ++c)
        statics[c] = first + c < weights.size() ? weights[first + c] : 0.0f;
    }
    (void)per_slot;
  }
  static_cache_dirty_ = true;
}

bool GpuScene::create_streaming(const ResolvedSettings& resolved, std::string* error) {
  if (!streamed_) return true;
  const gfx::Device& device = *device_;
  const geometry::ClusterPages& table = data_->pages;
  // A group is the siblings a cut refines into together, and the drawing rule's residency test is
  // over the whole group; the GPU tests one cluster's page instead, which is the same thing only
  // while a group is in one page. `build_cluster_pages` guarantees it. Say so here rather than
  // draw a cracked surface if a future layout stops guaranteeing it.
  for (u32 c = 1; c < cluster_count_; ++c) {
    if (data_->lod.lod[c].group != data_->lod.lod[c - 1].group) continue;
    if (table.page_of_cluster[c] == table.page_of_cluster[c - 1]) continue;
    if (error != nullptr) {
      *error = "geometry streaming: group " + std::to_string(data_->lod.lod[c].group) +
               " spans two pages, and the GPU drawing rule tests a cluster's page for its group's";
    }
    return false;
  }
  u64 total_bytes = 0;
  Vector<u32> sizes(page_count_);
  for (u32 p = 0; p < page_count_; ++p) {
    const geometry::ClusterPageDesc& desc = table.pages[p];
    sizes[p] = desc.bytes;
    total_bytes += desc.bytes;
    slot_vertices_ = desc.vertex_count > slot_vertices_ ? desc.vertex_count : slot_vertices_;
    slot_triangles_ = desc.triangle_count > slot_triangles_ ? desc.triangle_count : slot_triangles_;
  }
  page_budget_bytes_ = resolved.settings.page_budget_bytes;
  if (page_budget_bytes_ == 0 || page_budget_bytes_ > total_bytes) page_budget_bytes_ = total_bytes;
  std::sort(sizes.begin(), sizes.end());
  u64 spent = 0;
  page_slots_ = 0;
  for (u32 p = 0; p < page_count_ && spent + sizes[p] <= page_budget_bytes_; ++p) {
    spent += sizes[p];
    ++page_slots_;
  }
  // The root pages are pinned whatever the budget says — a mesh missing one cannot be drawn at all
  // — so the pool always has room for them ([geometry](geometry.md), "The residency model").
  u32 roots = 0;
  for (u32 p = 0; p < page_count_; ++p)
    roots += (table.pages[p].flags & geometry::k_page_root) != 0 ? 1u : 0u;
  if (page_slots_ < roots) page_slots_ = roots;
  if (page_slots_ == 0) page_slots_ = 1;
  if (page_slots_ > page_count_) page_slots_ = page_count_;

  u64 largest_payload = 0;
  for (u32 p = 0; p < page_count_; ++p)
    largest_payload = std::max(largest_payload, page_payload_bytes(p));
  upload_budget_bytes_ = resolved.settings.upload_budget_bytes;
  if (upload_budget_bytes_ == 0) upload_budget_bytes_ = k_default_upload_budget;
  // A budget no page fits in would never converge, so it is raised to one page rather than
  // accepted and reported as a scene that never finishes loading.
  if (upload_budget_bytes_ < largest_payload)
    upload_budget_bytes_ = static_cast<u32>(largest_payload);
  max_requests_ = page_count_ < k_max_page_requests ? page_count_ : k_max_page_requests;

  const u64 slot_vertices = u64{page_slots_} * slot_vertices_;
  const u64 slot_triangles = u64{page_slots_} * slot_triangles_;
  constexpr VkBufferUsageFlags k_pool =
      k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  const VkBufferUsageFlags rt_pool = k_pool | gfx::k_build_input_usage;
  Vector<u32> page_index(cluster_count_);
  Vector<u32> child_ranges(u64{cluster_count_} * 2);
  for (u32 c = 0; c < cluster_count_; ++c) {
    page_index[c] = table.page_of_cluster[c];
    child_ranges[c * 2 + 0] = table.children[c].first_cluster;
    child_ranges[c * 2 + 1] = table.children[c].cluster_count;
  }
  bool ok =
      gfx::create_buffer(device, slot_vertices * 3 * sizeof(u16), k_pool, false, quantized,
                         error) &&
      gfx::create_buffer(device, slot_vertices * sizeof(geometry::VertexAttributes), k_pool, false,
                         attributes, error) &&
      gfx::create_buffer(device, slot_triangles * sizeof(u32), k_pool, false, triangles, error) &&
      gfx::upload_buffer(device, table.pages.data(),
                         u64{page_count_} * sizeof(geometry::ClusterPageDesc), k_storage,
                         page_table, error) &&
      gfx::upload_buffer(device, page_index.data(), u64{cluster_count_} * sizeof(u32), k_storage,
                         page_of_cluster, error) &&
      gfx::upload_buffer(device, child_ranges.data(), u64{cluster_count_} * 2 * sizeof(u32),
                         k_storage, page_children, error) &&
      // Host-visible, one region per frame slot: the host writes the residency the cull pass of
      // the *next* frame reads, and a slot is not reused until the GPU has finished the frame that
      // last had it — the same argument that makes the joint buffer safe.
      gfx::create_buffer(device, u64{page_count_} * sizeof(u32) * k_stream_slots, k_address, true,
                         residency, error) &&
      gfx::create_buffer(device, u64{page_count_} * sizeof(u32), k_address | k_transfer, false,
                         page_used, error) &&
      gfx::create_buffer(device, u64{max_requests_} * sizeof(geometry::PageRequest),
                         k_address | k_transfer, false, page_requests, error) &&
      gfx::create_buffer(device, sizeof(u32), k_address | k_transfer, false, request_count,
                         error) &&
      gfx::create_buffer(device, u64{page_count_} * sizeof(u32), k_address | k_transfer, false,
                         request_mask, error) &&
      gfx::create_buffer(device, sizeof(gfx::StreamParams) * k_stream_slots, k_address, true,
                         stream_params, error) &&
      gfx::create_buffer(device, u64{upload_budget_bytes_} * k_stream_slots,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, page_stage, error);
  if (ok && ray_tracing_) {
    ok =
        gfx::create_buffer(device, slot_vertices * sizeof(Vec3), rt_pool, false, vertices, error) &&
        gfx::create_buffer(device, slot_triangles * 3, rt_pool, false, indices8, error);
  }
  if (!ok) return false;
  std::memset(residency.mapped, 0, u64{page_count_} * sizeof(u32) * k_stream_slots);
  // Every address but the residency array is the same in all three blocks, which is the whole
  // reason there are three: one word of the block changes per frame and nothing else does.
  auto* blocks = static_cast<gfx::StreamParams*>(stream_params.mapped);
  for (u32 slot = 0; slot < k_stream_slots; ++slot) {
    gfx::StreamParams& block = blocks[slot];
    block = gfx::StreamParams{};
    block.pages = page_table.address;
    block.page_of_cluster = page_of_cluster.address;
    block.children = page_children.address;
    block.residency = residency_slot_address(slot);
    block.used = page_used.address;
    block.requests = page_requests.address;
    block.request_count = request_count.address;
    block.request_mask = request_mask.address;
  }
  const u64 pool_bytes = quantized.size + attributes.size + triangles.size + vertices.size +
                         indices8.size + page_stage.size;
  geometry_bytes_ = total_bytes;
  stream_bytes_ = pool_bytes + page_table.size + page_of_cluster.size + page_children.size;
  ENGINE_LOG_INFO(
      log_renderer, "geometry streaming", log::field("pages", page_count_),
      log::field("slots", page_slots_), log::field("page_bytes", total_bytes),
      log::field("budget_bytes", page_budget_bytes_), log::field("pool_bytes", pool_bytes),
      log::field("table_bytes", stream_bytes_ - pool_bytes),
      log::field("slot_vertices", slot_vertices_), log::field("slot_triangles", slot_triangles_),
      log::field("upload_budget", upload_budget_bytes_));
  return true;
}

bool GpuScene::upload_materials(const ResolvedSettings&, std::string* error) {
  const gfx::Device& device = *device_;
  const geometry::ClusterLodMesh& lod = data_->lod;
  if (!gfx::create_sampler(device, VK_FILTER_LINEAR, sampler_, error)) return false;
  const u32 sampler_slot = bindless_.add_sampler(sampler_);
  Vector<gfx::ResolveMaterial> material_table;
  Vector<u32> cluster_material(cluster_count_);
  Vector<u32> mesh_material_base(data_->parts.size(), 0u);
  if (data_->heightfield) {
    // A procedural ripple texture, linear-sampled through the bindless set.
    constexpr u32 k_texture_size = 256;
    Vector<u8> texels(k_texture_size * k_texture_size * 4);
    for (u32 y = 0; y < k_texture_size; ++y) {
      for (u32 x = 0; x < k_texture_size; ++x) {
        const f32 fx = static_cast<f32>(x);
        const f32 fy = static_cast<f32>(y);
        const f32 ripple = 0.5f + 0.5f * std::sin(fx * 0.25f + 2.0f * std::sin(fy * 0.08f));
        const f32 grain =
            0.5f + 0.5f * std::sin(fx * 1.7f + fy * 2.3f) * std::sin(fy * 1.1f - fx * 0.7f);
        const u8 v = static_cast<u8>((0.62f + 0.3f * ripple + 0.08f * grain) * 255.0f);
        u8* t = &texels[(y * k_texture_size + x) * 4];
        t[0] = t[1] = t[2] = v;
        t[3] = 255;
      }
    }
    if (!gfx::upload_image_2d(device, k_texture_size, k_texture_size, VK_FORMAT_R8G8B8A8_UNORM,
                              texels.data(), texels.size(), procedural_texture_, error) ||
        !gfx::create_image_view(device, procedural_texture_, procedural_view_, error)) {
      return false;
    }
    const u32 texture_slot =
        bindless_.add_sampled_image(procedural_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // Materials: a flat table indexed per cluster by the height band of the cluster's center.
    material_table.resize(3);
    material_table[0].albedo = Vec4{0.86f, 0.72f, 0.46f, 0.9f};  // sand
    material_table[1].albedo = Vec4{0.42f, 0.40f, 0.38f, 0.7f};  // rock
    material_table[2].albedo = Vec4{0.92f, 0.94f, 0.97f, 0.4f};  // snow
    for (u32 i = 0; i < 2; ++i) {  // sand and rock carry the ripple texture at different scales
      material_table[i].albedo_texture = texture_slot;
      material_table[i].sampler = sampler_slot;
      material_table[i].uv_scale = i == 0 ? 24.0f : 9.0f;
    }
    for (u32 i = 0; i < cluster_count_; ++i) {
      const f32 y = lod.mesh.clusters[i].center.y;
      cluster_material[i] = y < -0.15f ? 0u : y < 0.65f ? 1u : 2u;
    }
  } else {
    // Materials from the files, one mesh's table after the last: every instance adds its mesh's
    // base to the cluster's material index, so the clusters keep mesh-local indices. Images are
    // decoded on the CPU (embedded bytes or a file beside the glTF) and uploaded once each, into
    // one bindless slot every material of that mesh which names the image shares; an image that
    // fails to decode leaves its slot empty with a warning. Base color is color and goes up as
    // sRGB, so that sampling returns linear; metallic-roughness and normal maps are data, not
    // color, and go up UNORM. A glTF never gives one image both roles, so the format an image is
    // first asked for is the one it keeps.
    for (u32 m = 0; m < data_->sources.size(); ++m) {
      const SourceMesh& source_mesh = data_->sources[m];
      const assets::MeshData& mesh_data = source_mesh.data;
      mesh_material_base[m] = material_table.size();
      Vector<u32> image_slot(mesh_data.images.size(), gfx::k_no_texture);
      Vector<bool> image_tried(mesh_data.images.size(), false);
      const std::string& mesh_dir = source_mesh.image_dir;  // the glTF's or the container's
      auto texture_slot_of = [&](i32 image_index, VkFormat format) -> u32 {
        if (image_index < 0 || static_cast<u32>(image_index) >= mesh_data.images.size())
          return gfx::k_no_texture;
        const u32 index = static_cast<u32>(image_index);
        if (image_tried[index]) return image_slot[index];
        image_tried[index] = true;
        const assets::ImageRef& ref = mesh_data.images[index];
        image::Image decoded;
        std::string image_error;
        bool ok = false;
        if (!ref.bytes.empty()) {
          ok = image::decode_image(std::span<const u8>(ref.bytes.data(), ref.bytes.size()), decoded,
                                   4, &image_error);
        } else if (!ref.uri.empty()) {
          const std::string path = mesh_dir.empty() ? ref.uri : io::join_path(mesh_dir, ref.uri);
          ok = image::read_image(path, decoded, 4, &image_error) == io::Status::Ok;
        } else {
          image_error = "image has neither bytes nor a uri";
        }
        gfx::ImageResource uploaded;
        VkImageView view = VK_NULL_HANDLE;
        if (ok && (!gfx::upload_image_2d(device, decoded.width, decoded.height, format,
                                         decoded.pixels.data(), decoded.pixels.size(), uploaded,
                                         &image_error) ||
                   !gfx::create_image_view(device, uploaded, view, &image_error))) {
          if (uploaded.image != VK_NULL_HANDLE) gfx::destroy_image(device, uploaded);
          ok = false;
        }
        if (!ok) {
          ENGINE_LOG_WARN(log_renderer, "texture skipped", log::field("image", index),
                          log::field("name", ref.name), log::field("error", image_error));
          return gfx::k_no_texture;
        }
        textures_.push_back(uploaded);
        texture_views_.push_back(view);
        image_slot[index] =
            bindless_.add_sampled_image(view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return image_slot[index];
      };
      for (const assets::Material& source : mesh_data.materials) {
        gfx::ResolveMaterial material;
        material.albedo =
            Vec4{source.base_color.x, source.base_color.y, source.base_color.z, source.roughness};
        // The emissive factor goes through as a constant term. A material that modulates it with
        // an emissive texture is left unlit instead of glowing at full factor everywhere: the
        // resolve has no emissive slot yet, and too dark is a smaller lie than too bright.
        const Vec3 emissive = source.emissive_image < 0 ? source.emissive : Vec3{};
        material.emissive = Vec4{emissive, source.metallic};
        material.albedo_texture = texture_slot_of(source.base_color_image, VK_FORMAT_R8G8B8A8_SRGB);
        material.metallic_roughness_texture =
            texture_slot_of(source.metallic_roughness_image, VK_FORMAT_R8G8B8A8_UNORM);
        material.normal_texture = texture_slot_of(source.normal_image, VK_FORMAT_R8G8B8A8_UNORM);
        material.normal_scale = source.normal_scale;
        material.sampler = sampler_slot;
        material.uv_scale = 1.0f;
        material_table.push_back(material);
      }
      const u32 local_count = material_table.size() - mesh_material_base[m];
      gfx::ResolveMaterial plain;
      plain.albedo = Vec4{0.8f, 0.8f, 0.8f, 0.6f};
      material_table.push_back(plain);  // the default for a primitive that names no material
      const geometry::ClusterMeshPart& part = data_->parts[m];
      for (u32 i = 0; i < part.cluster_count; ++i) {
        const i32 material = source_mesh.part_material[source_mesh.part_of_cluster[i]];
        cluster_material[part.first_cluster + i] =
            material >= 0 && static_cast<u32>(material) < local_count ? static_cast<u32>(material)
                                                                      : local_count;
      }
    }
  }
  // Now that the tables are laid out, every instance knows where its mesh's materials start.
  for (gfx::InstanceDesc& instance : instance_table_)
    instance.material_base = mesh_material_base[instance.mesh];
  material_count_ = material_table.size();
  return gfx::upload_buffer(device, material_table.data(),
                            u64{material_count_} * sizeof(gfx::ResolveMaterial), k_storage,
                            materials, error) &&
         gfx::upload_buffer(device, cluster_material.data(), u64{cluster_count_} * sizeof(u32),
                            k_storage, cluster_materials, error) &&
         gfx::upload_buffer(device, instance_table_.data(),
                            u64{instance_count_} * sizeof(gfx::InstanceDesc), k_storage, instances,
                            error);
}

bool GpuScene::create_working_set(const ResolvedSettings&, std::string* error) {
  const gfx::Device& device = *device_;
  // One visible list for the whole frame, in three runs per view: the hardware pass 1, the
  // hardware pass 2, and the software rasterizer. A visibility id names an entry of the whole
  // list, so the resolve of any view reads one array however many draws of however many views
  // filled it, and each draw's `visible_offset` is where its run starts. Every run is as long as
  // the pair count, which is as many entries as any one draw can produce.
  //
  // Run-major (`visible_base`): run r of view v starts at `(r * views + v) * pair_count`, so the
  // first run of every view is one contiguous range at the front. That range is what the ray
  // tracing chain builds the union of the views' cuts from in a single dispatch.
  const u64 visible_entry_bytes = 2 * sizeof(u32);
  visible_run_bytes_ = u64{pair_count_} * visible_entry_bytes;
  bool ok = gfx::create_buffer(device, visible_run_bytes_ * k_visible_runs * view_count_,
                               k_readable, false, visible, error) &&
            gfx::create_buffer(device, u64{gfx::k_draw_args_bytes} * view_count_, k_args, false,
                               sw_args, error);
  for (u32 i = 0; i < 2; ++i) {
    ok = ok &&
         gfx::create_buffer(device, u64{gfx::k_draw_args_bytes} * view_count_, k_args, false,
                            draw_args[i], error) &&
         gfx::create_buffer(device, u64{pair_count_} * view_count_ * sizeof(u32),
                            k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, flags[i], error);
  }
  return ok;
}

bool GpuScene::create_ray_tracing(const ResolvedSettings& resolved, std::string* error) {
  if (!ray_tracing_) return true;
  const gfx::Device& device = *device_;
  const geometry::ClusterLodMesh& lod = data_->lod;
  // Every pair may be in some frame's cut, so the cluster acceleration structures are sized for
  // all of them, and a pair's base geometry index is its entry in the visible list, so the
  // largest geometry index is the last pair. One cluster bottom-level structure per instance,
  // sized for that instance's mesh; the top-level structure instances them with the world
  // transforms and a custom index that is the scene instance.
  //
  // **Times the view count.** The structures hold the union of the views' cuts, and in the worst
  // case every view draws every pair, so the capacity is `views * pair_count` and the largest
  // geometry index is the last entry of the views' first runs, which run-major ordering puts at
  // `views * pair_count - 1`. The capacity is what is allocated; what a frame actually builds is
  // `record_count`, and on a surround the views see nearly disjoint thirds of the world, so the
  // records built stay close to one view's while the allocation is three times it.
  const u32 union_clusters = pair_count_ * view_count_;
  Vector<u8> packed;
  // Under streaming the 8-bit indices are one more page-pool stream, filled a page at a time
  // beside the float positions, because a CLAS record addresses them at the same slot-relative
  // `triangle_offset` the rasterizers read.
  if (!streamed_) {
    gfx::pack_cluster_indices(
        std::span<const u32>(lod.mesh.triangles.data(), lod.mesh.triangles.size()), packed);
  }
  gfx::ClusterSetLimits limits;
  limits.max_clusters = union_clusters;
  limits.max_triangles_per_cluster = triangles_per_cluster_;
  limits.max_vertices_per_cluster = geometry::ClusterLodOptions{}.max_vertices;
  limits.max_geometry_index = union_clusters - 1;
  limits.instantiate = resolved.settings.rt_templates;
  constexpr VkBufferUsageFlags k_record_usage =
      k_address | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  bool ok = (streamed_ || gfx::upload_buffer(device, packed.data(), packed.size(),
                                             gfx::k_build_input_usage, indices8, error)) &&
            gfx::create_buffer(device, gfx::k_cluster_build_record_bytes * union_clusters,
                               k_record_usage, false, records, error) &&
            gfx::create_buffer(device, sizeof(u32), k_record_usage, false, record_count, error) &&
            gfx::create_buffer(device, u64{union_clusters} * sizeof(u32), k_address, false, slots,
                               error) &&
            gfx::create_buffer(device, u64{instance_count_} * sizeof(u32),
                               k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, instance_counts,
                               error) &&
            gfx::create_buffer(device, u64{instance_count_} * sizeof(u32), k_address, false,
                               instance_first, error) &&
            gfx::create_buffer(device, gfx::k_cluster_blas_record_bytes * instance_count_,
                               k_record_usage, false, blas_records, error) &&
            gfx::create_cluster_set(device, limits, clas_set, error) &&
            gfx::create_tlas(device, instance_count_, gfx::k_build_fast_trace, tlas, error) &&
            gfx::create_buffer(device, gfx::k_instance_record_bytes * instance_count_,
                               gfx::k_build_input_usage, true, rt_instances, error);
  u64 blas_bytes = 0;
  cluster_blas.resize(instance_count_);
  for (u32 i = 0; i < instance_count_ && ok; ++i) {
    ok = gfx::create_cluster_blas(device,
                                  data_->parts[instance_table_[i].mesh].cluster_count * view_count_,
                                  cluster_blas[i], error);
    if (ok) blas_bytes += cluster_blas[i].data.size;
  }
  // One template per cluster of the scene, built from the rest pose, with cluster id and base
  // geometry index zero so an instantiate record's offsets are the visible entry outright. Built
  // once below; instantiated from the pool every frame.
  if (ok && resolved.settings.rt_templates) {
    gfx::ClusterSetLimits template_limits = limits;
    template_limits.max_clusters = cluster_count_;
    template_limits.max_geometry_index = 0;
    template_limits.instantiate = false;
    ok = gfx::create_cluster_templates(device, template_limits, clas_templates, error) &&
         gfx::create_buffer(device, gfx::k_cluster_template_record_bytes * cluster_count_,
                            k_record_usage, true, template_records, error);
    if (ok) {
      Vector<gfx::ClusterBuildInput> template_inputs;
      for (u32 c = 0; c < cluster_count_; ++c) {
        const geometry::ClusterDesc& desc = lod.mesh.clusters[c];
        gfx::ClusterBuildInput in;
        in.cluster_id = 0;  // the instantiate record's offsets carry the visible entry
        in.triangle_count = desc.triangle_count;
        in.vertex_count = desc.vertex_count;
        in.vertices = vertices.address + u64{desc.vertex_offset} * sizeof(Vec3);
        in.indices = indices8.address + u64{desc.triangle_offset} * 3;
        template_inputs.push_back(in);
      }
      gfx::write_cluster_template_records(
          std::span<const gfx::ClusterBuildInput>(template_inputs.data(), template_inputs.size()),
          template_records.mapped);
    }
  }
  if (ok) {
    u64 scratch_bytes = clas_set.build_scratch_bytes;
    scratch_bytes = std::max(scratch_bytes, tlas.build_scratch_bytes);
    scratch_bytes = std::max(scratch_bytes, clas_templates.build_scratch_bytes);
    for (const gfx::ClusterBlas& blas : cluster_blas)
      scratch_bytes = std::max(scratch_bytes, blas.build_scratch_bytes);
    ok = gfx::create_scratch(device, scratch_bytes, rt_scratch, error);
    tlas_slot_ = bindless_.add_acceleration_structure(tlas.handle);
    if (tlas_slot_ == gfx::BindlessSet::k_invalid_slot) {
      ok = false;
      if (error != nullptr) *error = "no bindless slot for the top-level structure";
    }
  }
  // The templates are built once, before any frame: everything after them is per frame.
  if (ok && resolved.settings.rt_templates) {
    ok = gfx::submit_immediate(
        device,
        [&](VkCommandBuffer cb) {
          gfx::build_cluster_templates(cb, clas_templates, template_records.address, 0, rt_scratch);
          gfx::acceleration_build_barrier(
              cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        },
        error);
    if (ok) {
      const auto* sizes = static_cast<const u32*>(clas_templates.sizes.mapped);
      for (u32 c = 0; c < cluster_count_; ++c)
        template_bytes_ += sizes[c];
    }
  }
  if (!ok) return false;
  rt_bytes_ = clas_set.data.size + blas_bytes + rt_scratch.size + tlas.buffer.size + records.size +
              template_bytes_;
  ENGINE_LOG_INFO(log_renderer, "ray tracing ready", log::field("pairs", pair_count_),
                  log::field("views", view_count_), log::field("union_clusters", union_clusters),
                  log::field("instances", instance_count_),
                  log::field("clas_bytes", clas_set.data.size),
                  log::field("blas_bytes", blas_bytes),
                  log::field("scratch_bytes", rt_scratch.size), log::field("rt_bytes", rt_bytes_),
                  log::field("templates", resolved.settings.rt_templates ? cluster_count_ : 0u),
                  log::field("template_bytes", template_bytes_));
  return true;
}

void GpuScene::destroy() noexcept {
  if (device_ == nullptr) return;
  const gfx::Device& device = *device_;
  gfx::destroy_buffer(device, meshes);
  gfx::destroy_cluster_templates(device, clas_templates);
  gfx::destroy_buffer(device, template_records);
  gfx::destroy_acceleration_structure(device, tlas);
  for (gfx::ClusterBlas& blas : cluster_blas)
    gfx::destroy_cluster_blas(device, blas);
  cluster_blas.clear();
  gfx::destroy_cluster_set(device, clas_set);
  gfx::destroy_buffer(device, rt_scratch);
  gfx::destroy_buffer(device, rt_instances);
  gfx::destroy_buffer(device, blas_records);
  gfx::destroy_buffer(device, instance_first);
  gfx::destroy_buffer(device, instance_counts);
  gfx::destroy_buffer(device, slots);
  gfx::destroy_buffer(device, record_count);
  gfx::destroy_buffer(device, records);
  gfx::destroy_buffer(device, indices8);
  gfx::destroy_buffer(device, page_stage);
  gfx::destroy_buffer(device, stream_params);
  gfx::destroy_buffer(device, request_mask);
  gfx::destroy_buffer(device, request_count);
  gfx::destroy_buffer(device, page_requests);
  gfx::destroy_buffer(device, page_used);
  gfx::destroy_buffer(device, residency);
  gfx::destroy_buffer(device, page_children);
  gfx::destroy_buffer(device, page_of_cluster);
  gfx::destroy_buffer(device, page_table);
  gfx::destroy_buffer(device, deform_frames);
  gfx::destroy_buffer(device, joints);
  gfx::destroy_buffer(device, deform_args);
  gfx::destroy_buffer(device, deform_table);
  gfx::destroy_buffer(device, deform_alloc);
  gfx::destroy_buffer(device, deform_slots);
  gfx::destroy_buffer(device, deform_pool);
  for (u32 i = 0; i < 2; ++i) {
    gfx::destroy_buffer(device, flags[i]);
    gfx::destroy_buffer(device, draw_args[i]);
  }
  gfx::destroy_buffer(device, sw_args);
  gfx::destroy_buffer(device, visible);
  gfx::destroy_buffer(device, cluster_materials);
  gfx::destroy_buffer(device, materials);
  gfx::destroy_buffer(device, skin);
  gfx::destroy_buffer(device, attributes);
  gfx::destroy_buffer(device, lods);
  gfx::destroy_buffer(device, triangles);
  gfx::destroy_buffer(device, vertices);
  gfx::destroy_buffer(device, instances);
  gfx::destroy_buffer(device, quantized);
  gfx::destroy_buffer(device, clusters);
  for (VkImageView view : texture_views_)
    gfx::destroy_image_view(device, view);
  texture_views_.clear();
  for (gfx::ImageResource& image : textures_)
    gfx::destroy_image(device, image);
  textures_.clear();
  gfx::destroy_image_view(device, procedural_view_);
  procedural_view_ = VK_NULL_HANDLE;
  if (procedural_texture_.image != VK_NULL_HANDLE) gfx::destroy_image(device, procedural_texture_);
  procedural_texture_ = gfx::ImageResource{};
  gfx::destroy_sampler(device, sampler_);
  sampler_ = VK_NULL_HANDLE;
  bindless_.destroy();
  instance_table_.clear();
  deform_descs_.clear();
  deform_instance_.clear();
  device_ = nullptr;
  data_ = nullptr;
  cluster_count_ = leaf_count_ = instance_count_ = pair_count_ = material_count_ = 0;
  triangles_per_cluster_ = 0;
  view_count_ = 1;
  max_joints_ = skinned_instances_ = deform_pool_vertices_ = 0;
  visible_run_bytes_ = deform_pool_bytes_ = deform_whole_mesh_bytes_ = 0;
  template_bytes_ = rt_bytes_ = 0;
  tlas_slot_ = gfx::BindlessSet::k_invalid_slot;
  ray_tracing_ = deform_ = skinned_ = streamed_ = false;
  page_count_ = page_slots_ = slot_vertices_ = slot_triangles_ = max_requests_ = 0;
  upload_budget_bytes_ = 0;
  page_budget_bytes_ = stream_bytes_ = geometry_bytes_ = 0;
}

}  // namespace engine::renderer

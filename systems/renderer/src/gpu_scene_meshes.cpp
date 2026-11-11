// Meshes that come and go (docs/subsystems/renderer.md, "Meshes that come and go"; roadmap R142):
// the GPU scene's room for a tile's mesh, its admission and its retirement.
//
// **Why reserved room, in the scene's own buffers.** Every mesh of a scene lives in one set of
// buffers with scene-wide offsets baked into `geometry::ClusterDesc`, and every shader that reads
// geometry — the three rasterizers, the resolve, the pool pass, the ray tracing chain's records —
// takes those buffers' addresses once. A mesh that arrives after the load therefore goes where a
// terrain ring's chunk goes (renderer.md, "The rings in the scene"): into room the scene made when
// it was, by a copy the next frame records after every frame in flight and before anything that
// reads geometry. The two alternatives were weighed and left: a second set of buffers per mesh
// would put an address per stream in `gfx::MeshDesc`, which is full at 64 bytes and read per
// position, and in the chain's build records; relocation (growing the buffers and moving the
// scene) is a wait for the device and a copy of everything resident, on the frame that wanted a
// tile. Room costs its bytes whether or not a mesh is in it, which is why it is a budget
// (`renderer.meshes.budget_mib`) and why a scene none of whose generators hands a mesh with a tile
// (`SceneData::tile_meshes`) makes none.
//
// **One arena, laid out by clusters.** The cluster arena is first-fit over free runs; a cluster of
// the arena owns a fixed run of the vertex and triangle streams — the scene's per-cluster limits,
// `ClusterLodOptions::max_vertices` and `max_triangles` — so a mesh's vertices and triangles are at
// the offsets its cluster run gives and there is one free list to fragment, not three that
// fragment differently. What that costs is the slack of clusters that are not full, which the
// budget counts (`MeshRoom::cluster_bytes` is the worst case): a refusal is then a statement about
// clusters alone, which is what the world's residency decides by.

#include <core/base/assert.h>
#include <core/log/log.h>
#include <domain/geometry/cluster_lod.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/gpu_scene.h>

#include <algorithm>
#include <cstring>
#include <renderer_log.h>
#include <string>

namespace engine::renderer {

namespace {

tunables::Int meshes_budget_mib{
    "renderer.meshes.budget_mib", 256, 0, 16'384,
    "MiB of device memory a streamed scene reserves for meshes that arrive with their tiles "
    "(the cluster arena and the vertices and triangles it owns; 0: none, and a tile's mesh is "
    "refused). Only a scene with a generator that hands meshes with its tiles reserves it"};

tunables::Int meshes_slots{
    "renderer.meshes.slots", 4'096, 0, 65'536,
    "Meshes that arrived with their tiles a streamed scene holds at once, at most (64 bytes of "
    "the mesh table each)"};

tunables::Int meshes_max_clusters{
    "renderer.meshes.max_clusters", 4'096, 1, 1'048'576,
    "Clusters one mesh that arrives with a tile may have, at most (the ray tracing chain's "
    "per-instance tables are sized by it; a larger mesh is refused by name)"};

constexpr u64 aligned16(u64 v) noexcept { return (v + 15) & ~u64{15}; }

}  // namespace

bool GpuScene::lay_out_mesh_slots(std::string* error) {
  mesh_room_ = MeshRoom{};
  mesh_use_ = MeshUse{};
  mesh_slots_.clear();
  free_mesh_clusters_.clear();
  free_mesh_materials_.clear();
  if (!dynamic_ || streamed_ || !data_->tile_meshes) return true;
  const u64 budget = static_cast<u64>(meshes_budget_mib.get()) * 1024 * 1024;
  const u32 slot_count = static_cast<u32>(meshes_slots.get());
  if (budget == 0 || slot_count == 0) return true;
  mesh_cluster_vertices_ = std::max<u32>(data_->mesh_options.lod.max_vertices, 1u);
  mesh_cluster_triangles_ = std::max<u32>(data_->mesh_options.lod.max_triangles, 1u);
  const u64 cluster_bytes =
      sizeof(geometry::ClusterDesc) + sizeof(geometry::ClusterLodDesc) + sizeof(u32) +
      u64{mesh_cluster_vertices_} *
          (6 + sizeof(geometry::VertexAttributes) + (ray_tracing_ ? sizeof(Vec3) : 0)) +
      u64{mesh_cluster_triangles_} * (sizeof(u32) + (ray_tracing_ ? 3 : 0));
  u64 arena = budget / cluster_bytes;
  // A cluster's vertex and triangle offsets are 32-bit and scene-wide, and a cluster index too.
  const u64 offset_room = 0xffff'ffffull - 4;
  const u64 by_vertices = vertex_capacity_ < offset_room
                              ? (offset_room - vertex_capacity_) / mesh_cluster_vertices_
                              : 0;
  const u64 by_triangles = triangle_capacity_ < offset_room
                               ? (offset_room - triangle_capacity_) / mesh_cluster_triangles_
                               : 0;
  arena = std::min({arena, by_vertices, by_triangles, offset_room - cluster_count_});
  if (arena == 0) {
    if (error != nullptr) {
      *error = "renderer.meshes.budget_mib (" + std::to_string(meshes_budget_mib.get()) +
               ") holds no cluster of a mesh that arrives with a tile (" +
               std::to_string(cluster_bytes) + " bytes each)";
    }
    return false;
  }
  mesh_room_.first_mesh = data_->parts.size() + slot_parts_.size();
  mesh_room_.slots = slot_count;
  mesh_room_.clusters = static_cast<u32>(arena);
  mesh_room_.max_clusters =
      std::min(static_cast<u32>(meshes_max_clusters.get()), mesh_room_.clusters);
  // Its materials and the default after them: two a slot on average, a few dozen at least.
  mesh_room_.materials = std::max<u32>(64u, slot_count * 2u);
  mesh_room_.cluster_bytes = cluster_bytes;
  mesh_room_.bytes = arena * cluster_bytes + u64{slot_count} * sizeof(gfx::MeshDesc) +
                     u64{mesh_room_.materials} * sizeof(gfx::ResolveMaterial);
  mesh_part_base_ = slot_parts_.size();
  mesh_cluster_base_ = cluster_count_;
  mesh_vertex_base_ = vertex_capacity_;
  mesh_triangle_base_ = triangle_capacity_;
  // An empty slot is a mesh with no clusters at the arena's start: an instance cannot name it (the
  // tail refuses a mesh that is not resident), and a pair that did would be past its clusters.
  for (u32 s = 0; s < slot_count; ++s) {
    geometry::ClusterMeshPart part;
    part.first_cluster = mesh_cluster_base_;
    part.first_vertex = static_cast<u32>(mesh_vertex_base_);
    slot_parts_.push_back(part);
  }
  cluster_count_ += mesh_room_.clusters;
  vertex_capacity_ += arena * mesh_cluster_vertices_;
  triangle_capacity_ += arena * mesh_cluster_triangles_;
  mesh_slots_.resize(slot_count);
  // The free lists at the most they can hold (a run between every two resident meshes, and one
  // more while `give_range` inserts before it merges), so an admission or a retirement allocates
  // nothing for them; and a record per slot, off and on, as a ring's slots reserve.
  free_mesh_clusters_.reserve(slot_count + 2);
  free_mesh_materials_.reserve(slot_count + 2);
  free_mesh_clusters_.push_back(Range{mesh_cluster_base_, mesh_room_.clusters});
  pending_mesh_writes_.reserve(pending_mesh_writes_.capacity() + 2 * slot_count);
  ENGINE_LOG_INFO(log_renderer, "mesh room", log::field("slots", slot_count),
                  log::field("clusters", mesh_room_.clusters),
                  log::field("max_clusters", mesh_room_.max_clusters),
                  log::field("materials", mesh_room_.materials),
                  log::field("bytes", mesh_room_.bytes),
                  log::field("budget_mib", meshes_budget_mib.get()));
  return true;
}

GpuScene::MeshUse GpuScene::mesh_use() const noexcept {
  MeshUse use = mesh_use_;
  use.largest_free = 0;
  for (const Range& run : free_mesh_clusters_)
    use.largest_free = std::max(use.largest_free, static_cast<u32>(run.count));
  use.bytes = u64{use.clusters} * mesh_room_.cluster_bytes;
  return use;
}

bool GpuScene::mesh_fits(u32 cluster_count, u32 material_count) const noexcept {
  if (cluster_count == 0 || cluster_count > mesh_room_.max_clusters) return false;
  bool room = false;
  for (const Range& run : free_mesh_clusters_)
    room = room || run.count >= cluster_count;
  if (!room) return false;
  for (const Range& run : free_mesh_materials_)
    if (run.count >= material_count) return true;
  return false;
}

bool GpuScene::mesh_resident(u32 mesh) const noexcept {
  return mesh >= mesh_room_.first_mesh && mesh - mesh_room_.first_mesh < mesh_slots_.size() &&
         mesh_slots_[mesh - mesh_room_.first_mesh].loaded;
}

u32 GpuScene::mesh_first_cluster(u32 mesh) const noexcept {
  return mesh < data_->parts.size() + slot_parts_.size() ? part_of(mesh).first_cluster : 0u;
}

u32 GpuScene::mesh_cluster_count(u32 mesh) const noexcept {
  return mesh < data_->parts.size() + slot_parts_.size() ? part_of(mesh).cluster_count : 0u;
}

bool GpuScene::stage_bytes(u64 bytes, gfx::BufferResource& staging, u64& at, std::string* error) {
  at = 0;
  if (ring_take(bytes, at)) {
    staging = staging_ring_;
    return true;
  }
  // No room in the ring (a whole fill at once), or none made: a buffer of its own, which the frame
  // that copies from it retires.
  if (!gfx::create_buffer(*device_, bytes, gfx::BufferUsage::TransferSrc, true, staging, error))
    return false;
  if (staging_ring_.mapped != nullptr) {
    ++ring_overflows_;
    ring_overflow_bytes_ += bytes;
  }
  pending_staging_.push_back(staging);
  return true;
}

bool GpuScene::admit_mesh(u32 mesh, const PlacementMeshData& data, std::string* error) {
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = std::move(sentence);
    return false;
  };
  const std::string& name = data.source.path;
  if (mesh_room_.slots == 0) {
    return fail(name + ": the scene has no room for a mesh that arrives with a tile (" +
                (streamed_
                     ? std::string("it is drawn from geometry pages")
                     : std::string("renderer.meshes.budget_mib or renderer.meshes.slots is 0, "
                                   "or no generator of the scene hands one")) +
                ")");
  }
  if (mesh < mesh_room_.first_mesh || mesh - mesh_room_.first_mesh >= mesh_slots_.size())
    return fail(name + ": mesh " + std::to_string(mesh) + " is not one of the scene's mesh slots");
  const u32 s = mesh - mesh_room_.first_mesh;
  MeshSlot& slot = mesh_slots_[s];
  if (slot.loaded) return fail(name + ": mesh slot " + std::to_string(s) + " holds a mesh");
  const geometry::ClusterMesh& source = data.lod.mesh;
  const u32 nc = source.clusters.size();
  const u64 nv = source.vertices.size();
  const u64 nt = source.triangles.size();
  const u32 nm = mesh_materials(data);
  if (nc == 0) return fail(name + ": a mesh with no clusters");
  if (nc > mesh_room_.max_clusters) {
    return fail(name + ": " + std::to_string(nc) +
                " clusters, more than a mesh that arrives with " + "a tile may have (" +
                std::to_string(mesh_room_.max_clusters) +
                "; renderer.meshes.max_clusters, within renderer.meshes.budget_mib)");
  }
  if (nv > u64{nc} * mesh_cluster_vertices_ || nt > u64{nc} * mesh_cluster_triangles_ ||
      source.quantized.size() < nv * 3 || source.attributes.size() != nv ||
      data.lod.lod.size() != nc || data.source.part_of_cluster.size() != nc) {
    return fail(name + ": its clusters are not the scene's (" + std::to_string(nv) +
                " vertices and " + std::to_string(nt) + " triangles in " + std::to_string(nc) +
                " clusters of at most " + std::to_string(mesh_cluster_vertices_) + " and " +
                std::to_string(mesh_cluster_triangles_) + ")");
  }
  if (!take_range(free_mesh_clusters_, nc, slot.clusters)) {
    return fail(name + ": the mesh room has no run of " + std::to_string(nc) +
                " clusters free (renderer.meshes.budget_mib)");
  }
  if (!take_range(free_mesh_materials_, nm, slot.materials)) {
    give_range(free_mesh_clusters_, slot.clusters);
    slot.clusters = Range{};
    return fail(name + ": the mesh room has no run of " + std::to_string(nm) +
                " materials free (renderer.meshes.slots)");
  }
  const u64 c0 = slot.clusters.offset;
  const u64 v0 = mesh_vertex_base_ + (c0 - mesh_cluster_base_) * mesh_cluster_vertices_;
  const u64 t0 = mesh_triangle_base_ + (c0 - mesh_cluster_base_) * mesh_cluster_triangles_;
  // One staging run for the mesh, its streams one after another at 16-byte offsets, as a ring's
  // chunk is staged (`stage_chunk`), with its material indices and its materials after them.
  const u64 at_clusters = 0;
  const u64 at_lods = aligned16(at_clusters + u64{nc} * sizeof(geometry::ClusterDesc));
  const u64 at_quant = aligned16(at_lods + u64{nc} * sizeof(geometry::ClusterLodDesc));
  const u64 at_attributes = aligned16(at_quant + nv * 6);
  const u64 at_triangles = aligned16(at_attributes + nv * sizeof(geometry::VertexAttributes));
  const u64 at_vertices = aligned16(at_triangles + nt * sizeof(u32));
  const u64 at_indices = aligned16(at_vertices + (ray_tracing_ ? nv * sizeof(Vec3) : 0));
  const u64 at_cluster_materials = aligned16(at_indices + (ray_tracing_ ? nt * 3 : 0));
  const u64 at_materials = aligned16(at_cluster_materials + u64{nc} * sizeof(u32));
  const u64 total = aligned16(at_materials + u64{nm} * sizeof(gfx::ResolveMaterial));
  gfx::BufferResource staging;
  u64 ring_at = 0;
  if (!stage_bytes(total, staging, ring_at, error)) {
    give_range(free_mesh_clusters_, slot.clusters);
    give_range(free_mesh_materials_, slot.materials);
    slot.clusters = slot.materials = Range{};
    return false;
  }
  u8* base = static_cast<u8*>(staging.mapped) + ring_at;
  auto* clusters_out = reinterpret_cast<geometry::ClusterDesc*>(base + at_clusters);
  for (u32 c = 0; c < nc; ++c) {
    clusters_out[c] = source.clusters[c];
    clusters_out[c].vertex_offset += static_cast<u32>(v0);
    clusters_out[c].triangle_offset += static_cast<u32>(t0);
  }
  std::memcpy(base + at_lods, data.lod.lod.data(), u64{nc} * sizeof(geometry::ClusterLodDesc));
  std::memcpy(base + at_quant, source.quantized.data(), nv * 6);
  std::memcpy(base + at_attributes, source.attributes.data(),
              nv * sizeof(geometry::VertexAttributes));
  std::memcpy(base + at_triangles, source.triangles.data(), nt * sizeof(u32));
  if (ray_tracing_) {
    std::memcpy(base + at_vertices, source.vertices.data(), nv * sizeof(Vec3));
    u8* indices = base + at_indices;  // `gfx::pack_cluster_indices`' bytes, as a chunk's are
    for (u64 t = 0; t < nt; ++t) {
      const u32 triangle = source.triangles[static_cast<u32>(t)];
      indices[t * 3 + 0] = static_cast<u8>(triangle & 0xffu);
      indices[t * 3 + 1] = static_cast<u8>((triangle >> 8) & 0xffu);
      indices[t * 3 + 2] = static_cast<u8>((triangle >> 16) & 0xffu);
    }
  }
  // Each cluster's material, local to the mesh's run of the table as the load writes it: the
  // material of the cluster's part, or the default after the mesh's own for a part that names none.
  auto* cluster_material = reinterpret_cast<u32*>(base + at_cluster_materials);
  const u32 own = nm - 1;
  for (u32 c = 0; c < nc; ++c) {
    const u32 part = data.source.part_of_cluster[c];
    const i32 material =
        part < data.source.part_material.size() ? data.source.part_material[part] : -1;
    cluster_material[c] =
        material >= 0 && static_cast<u32>(material) < own ? static_cast<u32>(material) : own;
  }
  // **The materials' constants** — base colour, roughness, metallic, emissive — and none of their
  // textures, which are resident by the load alone until textures have a residency of their own
  // (renderer.md, "Meshes that come and go", what does not arrive). An alpha-tested material is
  // therefore drawn opaque: its cutout is its texture's.
  auto* materials_out = reinterpret_cast<gfx::ResolveMaterial*>(base + at_materials);
  for (u32 m = 0; m < own; ++m) {
    const assets::Material& from = data.source.data.materials[m];
    gfx::ResolveMaterial material;
    material.albedo = Vec4{from.base_color.x, from.base_color.y, from.base_color.z, from.roughness};
    material.emissive = Vec4{from.emissive, from.metallic};
    material.normal_scale = from.normal_scale;
    material.occlusion_strength = from.occlusion_strength;
    materials_out[m] = material;
  }
  gfx::ResolveMaterial plain;
  plain.albedo = Vec4{0.8f, 0.8f, 0.8f, 0.6f};  // the load's default for a part with no material
  materials_out[own] = plain;

  const auto copy = [&](const gfx::BufferResource& dst, u64 src, u64 dst_offset, u64 size) {
    if (size == 0) return;
    pending_geometry_.push_back(TerrainUpdate::GeometryCopy{
        staging.buffer, dst.buffer, gfx::BufferCopy{ring_at + src, dst_offset, size}});
  };
  copy(clusters, at_clusters, c0 * sizeof(geometry::ClusterDesc),
       u64{nc} * sizeof(geometry::ClusterDesc));
  copy(lods, at_lods, c0 * sizeof(geometry::ClusterLodDesc),
       u64{nc} * sizeof(geometry::ClusterLodDesc));
  copy(quantized, at_quant, v0 * 6, nv * 6);
  copy(attributes, at_attributes, v0 * sizeof(geometry::VertexAttributes),
       nv * sizeof(geometry::VertexAttributes));
  copy(triangles, at_triangles, t0 * sizeof(u32), nt * sizeof(u32));
  if (ray_tracing_) {
    copy(vertices, at_vertices, v0 * sizeof(Vec3), nv * sizeof(Vec3));
    copy(indices8, at_indices, t0 * 3, nt * 3);
  }
  copy(cluster_materials, at_cluster_materials, c0 * sizeof(u32), u64{nc} * sizeof(u32));
  copy(materials, at_materials, slot.materials.offset * sizeof(gfx::ResolveMaterial),
       u64{nm} * sizeof(gfx::ResolveMaterial));
  pending_geometry_bytes_ += total;

  // The mesh's record: the scene's addresses (the slot's record has them from `create`), this
  // mesh's grid and its run of the cluster array. The frame that copies the streams writes it.
  geometry::ClusterMeshPart& part = slot_parts_[mesh_part_base_ + s];
  part = data.part;
  part.first_cluster = static_cast<u32>(c0);
  part.cluster_count = nc;
  part.first_vertex = static_cast<u32>(v0);
  gfx::MeshDesc& desc = mesh_descs_[mesh];
  desc.quant = Vec4{data.part.quant_origin, data.part.quant_scale};
  desc.first_cluster = static_cast<u32>(c0);
  desc.cluster_count = nc;
  pending_mesh_writes_.push_back(mesh);
  if (mesh_material_base_.size() <= mesh) mesh_material_base_.resize(mesh + 1, 0u);
  mesh_material_base_[mesh] = static_cast<u32>(slot.materials.offset);
  slot.payload = total;
  slot.loaded = true;
  ++mesh_use_.meshes;
  mesh_use_.clusters += nc;
  mesh_use_.materials += nm;
  mesh_use_.payload_bytes += total;
  mesh_use_.staged_bytes += total;
  ++mesh_use_.admitted;
  return true;
}

void GpuScene::retire_mesh(u32 mesh) noexcept {
  if (!mesh_resident(mesh)) return;
  const u32 s = mesh - mesh_room_.first_mesh;
  MeshSlot& slot = mesh_slots_[s];
  // No cluster from the next frame on; the frames in flight that drew it read streams nothing
  // overwrites before the copy that reuses them, which waits for those frames (as a ring's slot).
  geometry::ClusterMeshPart& part = slot_parts_[mesh_part_base_ + s];
  mesh_use_.clusters -= part.cluster_count;
  mesh_use_.materials -= static_cast<u32>(slot.materials.count);
  mesh_use_.payload_bytes -= slot.payload;
  --mesh_use_.meshes;
  ++mesh_use_.retired;
  part = geometry::ClusterMeshPart{};
  part.first_cluster = mesh_cluster_base_;
  part.first_vertex = static_cast<u32>(mesh_vertex_base_);
  gfx::MeshDesc& desc = mesh_descs_[mesh];
  desc.first_cluster = mesh_cluster_base_;
  desc.cluster_count = 0;
  pending_mesh_writes_.push_back(mesh);
  give_range(free_mesh_clusters_, slot.clusters);
  give_range(free_mesh_materials_, slot.materials);
  slot = MeshSlot{};
}

}  // namespace engine::renderer

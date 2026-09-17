#pragma once

// Cluster-level acceleration structures (VK_NV_cluster_acceleration_structure), experiment E2
// of docs/plan/04-renderer.md §4.4: the ray tracing geometry is built from the same clusters
// the rasterizer selected, so the two never disagree on LOD. One cluster acceleration structure
// (CLAS) per cluster is built in bulk from a device array of per-cluster records whose count may
// itself live on the device (the frame's GPU-selected cut), a cluster bottom-level structure
// references them, and the standard top-level structure of acceleration.h instances that. Each
// cluster's base geometry index is its cluster id, so a hit's GeometryIndex names the cluster
// and PrimitiveIndex the triangle without the vendor cluster-id intrinsic, and
// ray_visibility.slang traces both paths unchanged. NVIDIA only:
// DeviceFeatures::cluster_acceleration_structure; acceleration.h is the fallback.
//
//     ClusterSetLimits limits{max_clusters, ...};
//     ClusterSet set;           create_cluster_set(device, limits, set);
//     ClusterBlas blas;         create_cluster_blas(device, max_clusters, blas);
//     ...write ClusterBuildInput records for the cut into a buffer with k_build_input_usage...
//     build_cluster_set(cb, set, infos_address, count_or_0, scratch);
//     acceleration_build_barrier(cb, ACCELERATION_STRUCTURE_BUILD, ACCELERATION_STRUCTURE_READ);
//     build_cluster_blas(cb, blas, set.addresses.address, cluster_count, scratch);
//     ...blas.address is the structure's device address for the TLAS instance record...
//
// Per frame from the GPU's own cut: clas_records.slang turns the cull pass's visible list into
// the CLAS records, the record count, and the bottom-level record, then build_cluster_set with
// the count's address and build_cluster_blas_indirect follow with no CPU round trip.

#include <core/base/types.h>
#include <domain/gfx/acceleration.h>

#include <span>
#include <string>

namespace engine::gfx {

// What the device allows per cluster and how big the arrays may get.
struct ClusterSetLimits {
  u32 max_clusters = 0;                 // records per build
  u32 max_triangles_per_cluster = 124;  // at most ClusterAsProperties::max_triangles_per_cluster
  u32 max_vertices_per_cluster = 64;
  u32 max_geometry_index = 0;  // largest cluster id used as a geometry index
};

struct ClusterAsProperties {
  u32 max_vertices_per_cluster = 0;
  u32 max_triangles_per_cluster = 0;
  u32 max_geometry_index = 0;
  u32 cluster_alignment = 0;
  u32 scratch_alignment = 0;
  u32 bottom_level_alignment = 0;
};
bool cluster_as_properties(const Device& device, ClusterAsProperties& out) noexcept;

// One cluster for the CLAS build: float3 positions (stride 12) and tightly packed 8-bit local
// indices, three per triangle (pack_cluster_indices() produces them from the cluster format).
struct ClusterBuildInput {
  u32 cluster_id = 0;      // reported as the cluster's ClusterID and used as its geometry index
  u32 triangle_count = 0;  // at most 256
  u32 vertex_count = 0;    // at most 256
  VkDeviceAddress vertices = 0;
  VkDeviceAddress indices = 0;  // u8[3 * triangle_count]
};
inline constexpr u64 k_cluster_build_record_bytes =
    64;  // one VkClusterAccelerationStructureBuildTriangleClusterInfoNV
// Appends three bytes per packed cluster triangle (i0 | i1 << 8 | i2 << 16).
void pack_cluster_indices(std::span<const u32> packed, Vector<u8>& out);
// Writes the device records the build consumes, k_cluster_build_record_bytes each.
void write_cluster_build_records(std::span<const ClusterBuildInput> clusters, void* out) noexcept;

// Storage for up to max_clusters CLAS built in one command (implicit destinations): the packed
// structures, the resulting address of each, and its size.
struct ClusterSet {
  BufferResource data;       // every CLAS, packed by the driver
  BufferResource addresses;  // u64[max_clusters], written by the build; host visible
  BufferResource sizes;      // u32[max_clusters], written by the build; host visible
  u64 build_scratch_bytes = 0;
  u32 max_clusters = 0;
  ClusterSetLimits limits;
  u32 alignment = 0;
};
bool create_cluster_set(const Device& device, const ClusterSetLimits& limits, ClusterSet& out,
                        std::string* error = nullptr);
// Builds one CLAS per record: `records` holds ClusterBuildInput records (see
// write_cluster_build_records) and `count` is a device address of a u32 count, or 0 to build
// limits.max_clusters records.
void build_cluster_set(VkCommandBuffer commands, const ClusterSet& set, VkDeviceAddress records,
                       VkDeviceAddress count, const BufferResource& scratch);
void destroy_cluster_set(const Device& device, ClusterSet& set) noexcept;

// The 64-byte CLAS build record as a shader writes it (clas_records.slang mirrors this), in
// order: u32 cluster_id; u32 cluster_flags; u32 counts, which packs triangle_count, then
// vertex_count << 9, position_truncate << 18, and index_type << 24; u32 geometry, the base
// geometry index in 24 bits with the geometry flags << 29 (4 = opaque); u32 strides0, the index
// stride (1) with the vertex stride (12) << 16; u32 strides1, the geometry-index stride with the
// opacity-micromap index stride << 16 (both 0); then five u64 addresses: index_buffer,
// vertex_buffer, geometry_buffer, omm_array, omm_indices.
inline constexpr u32 k_cluster_index_type_8bit = 1;  // ..._INDEX_FORMAT_8BIT_NV
inline constexpr u32 k_cluster_geometry_opaque = 4;  // ..._GEOMETRY_OPAQUE_BIT_NV

// The 16-byte record of a cluster bottom-level build: u32 reference count, u32 stride (8), u64
// address of the CLAS address array. A GPU-driven cut writes it from a shader.
inline constexpr u64 k_cluster_blas_record_bytes = 16;

// Push constants of clas_records.slang (records_main, 64 threads per group): one thread per
// entry of the cull pass's visible list writes that cluster's CLAS record; thread 0 writes the
// record count and the bottom-level record. Mirrors RecordParams in the shader. 80 bytes.
struct ClusterRecordParams {
  u64 clusters = 0;        // geometry::ClusterDesc[]
  u64 vertices = 0;        // float3[]: cluster-ordered positions
  u64 indices8 = 0;        // u8[]: pack_cluster_indices of every cluster, in triangle order
  u64 visible = 0;         // u32[]: the visible list
  u64 visible_count = 0;   // u32: the cull pass's count word
  u64 records = 0;         // ClusterBuildInput records out, k_cluster_build_record_bytes each
  u64 record_count = 0;    // u32 out: what build_cluster_set reads as `count`
  u64 blas_record = 0;     // ClusterBlas::record.address
  u64 clas_addresses = 0;  // ClusterSet::addresses.address
  u32 max_clusters = 0;
  u32 pad = 0;
};
static_assert(sizeof(ClusterRecordParams) == 80);
inline constexpr u32 k_cluster_records_workgroup = 64;

// A bottom-level structure over CLAS references (the addresses a ClusterSet build wrote). Built
// to an explicit destination, so its address is known when it is created and a top-level
// instance record can be written once.
struct ClusterBlas {
  BufferResource data;
  BufferResource record;        // k_cluster_blas_record_bytes; host visible
  BufferResource destination;   // u64: the explicit destination the build is told to use
  VkDeviceAddress address = 0;  // where the structure lives after any build (InstanceDesc::blas)
  u64 build_scratch_bytes = 0;
  u32 max_clusters = 0;
  u32 alignment = 0;
};
bool create_cluster_blas(const Device& device, u32 max_clusters, ClusterBlas& out,
                         std::string* error = nullptr);
// Writes the record on the host (`references`: a u64 array of CLAS addresses; `count` how
// many) and records the build.
void build_cluster_blas(VkCommandBuffer commands, const ClusterBlas& blas,
                        VkDeviceAddress references, u32 count, const BufferResource& scratch);
// Records the build with whatever the record holds: for a record a shader wrote on the GPU.
void build_cluster_blas_indirect(VkCommandBuffer commands, const ClusterBlas& blas,
                                 const BufferResource& scratch);
void destroy_cluster_blas(const Device& device, ClusterBlas& blas) noexcept;

}  // namespace engine::gfx

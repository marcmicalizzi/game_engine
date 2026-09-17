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
//     ...blas.address_out holds the structure's device address for the TLAS instance record...

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

// A bottom-level structure over CLAS references (the addresses a ClusterSet build wrote).
struct ClusterBlas {
  BufferResource data;
  BufferResource record;       // the one build record: count, stride, reference address
  BufferResource address_out;  // u64: the structure's device address after the build; host visible
  u64 build_scratch_bytes = 0;
  u32 max_clusters = 0;
  u32 alignment = 0;
};
bool create_cluster_blas(const Device& device, u32 max_clusters, ClusterBlas& out,
                         std::string* error = nullptr);
// `references` is a u64 array of CLAS addresses and `count` how many; the record is written on
// the host before recording, so the count is a CPU value here (a GPU-driven cut writes the
// record from a shader instead).
void build_cluster_blas(VkCommandBuffer commands, const ClusterBlas& blas,
                        VkDeviceAddress references, u32 count, const BufferResource& scratch);
// The address the last completed build wrote, for InstanceDesc::blas.
VkDeviceAddress cluster_blas_address(const ClusterBlas& blas) noexcept;
void destroy_cluster_blas(const Device& device, ClusterBlas& blas) noexcept;

}  // namespace engine::gfx

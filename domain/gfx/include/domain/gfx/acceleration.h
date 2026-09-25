#pragma once

// Acceleration structures (VK_KHR_acceleration_structure) over the cluster format, the ray
// tracing side of docs/plan/04-renderer.md §4.4. A bottom-level structure takes one geometry
// per cluster, so a hit's GeometryIndex names the cluster within the set that was built and
// PrimitiveIndex the triangle within it: the same (cluster, triangle) pair the rasterizer writes
// into the visibility buffer, which is what lets the RT and raster pictures be compared word for
// word. A top-level structure holds instances of bottom-level ones. Sizes come from the device,
// builds record into a command buffer, and the caller owns the scratch buffer so builds batch.
//
//     Vector<ClusterGeometry> geometries = ...one per cluster of the cut...;
//     AccelerationStructure blas;
//     create_blas(device, geometries, k_build_fast_trace, blas);
//     BufferResource scratch;  create_scratch(device, blas.build_scratch_bytes, scratch);
//     ...record: build_blas(cb, blas, geometries, k_build_fast_trace, scratch);
//                acceleration_build_barrier(cb, ...TLAS build or ray query stage...);
//
// Cluster-level acceleration structures (VK_NV_cluster_acceleration_structure, experiment E2)
// are the next step; this is the first-class fallback the plan keeps in any case.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/gfx/commands.h>
#include <domain/gfx/device.h>
#include <domain/gfx/resources.h>
#include <domain/gfx/rhi.h>

#include <span>
#include <string>

namespace engine::gfx {

struct AccelerationStructure {
  AccelerationStructureHandle handle;
  BufferResource buffer;        // backing storage
  DeviceAddress address = 0;    // for instance records
  u64 build_scratch_bytes = 0;  // what create_scratch() needs for the build
  u32 scratch_alignment = 256;  // the device's minimum scratch offset alignment
  u32 geometry_count = 0;       // BLAS: geometries; TLAS: instance capacity
};

// One cluster as a triangle geometry: tightly packed float3 positions and 16-bit local indices,
// three per triangle. The cluster format packs three 8-bit indices per u32, which no builder
// accepts, so expand_packed_triangles() widens them.
struct ClusterGeometry {
  DeviceAddress vertices = 0;
  u32 vertex_count = 0;
  DeviceAddress indices = 0;  // u16[3 * triangle_count]
  u32 triangle_count = 0;
};

// One instance of a bottom-level structure in a top-level one.
struct TlasInstance {
  Mat4 transform;          // affine; the top three rows go into the record
  u32 custom_index = 0;    // 24 bits: InstanceID() in shaders
  u32 mask = 0xff;         // 8 bits
  DeviceAddress blas = 0;  // AccelerationStructure::address
  GeometryInstanceFlags flags = GeometryInstanceFlags::TriangleFacingCullDisable;
};

inline constexpr AccelerationBuildFlags k_build_fast_trace =
    AccelerationBuildFlags::PreferFastTrace;
inline constexpr AccelerationBuildFlags k_build_fast_build =
    AccelerationBuildFlags::PreferFastBuild;
inline constexpr u64 k_instance_record_bytes = 64;  // one instance record, as the builder reads it

// Buffer usage for geometry (vertices, indices) and instance records the builder reads.
inline constexpr BufferUsage k_build_input_usage =
    BufferUsage::AccelerationBuildInput | BufferUsage::ShaderDeviceAddress;

// Appends three u16 per packed cluster triangle (i0 | i1 << 8 | i2 << 16).
void expand_packed_triangles(std::span<const u32> packed, Vector<u16>& out);

// Creates the bottom-level structure and its buffer for `clusters`, one geometry each, and
// computes the build sizes; nothing is recorded. Fails without DeviceFeatures::
// acceleration_structure.
bool create_blas(const Device& device, std::span<const ClusterGeometry> clusters,
                 AccelerationBuildFlags flags, AccelerationStructure& out,
                 std::string* error = nullptr);
// Records the build of `blas` from the same `clusters` and `flags` it was created with.
void build_blas(CommandList commands, const AccelerationStructure& blas,
                std::span<const ClusterGeometry> clusters, AccelerationBuildFlags flags,
                const BufferResource& scratch);

// Creates a top-level structure for up to `instance_count` instances.
bool create_tlas(const Device& device, u32 instance_count, AccelerationBuildFlags flags,
                 AccelerationStructure& out, std::string* error = nullptr);
// Writes instance records (k_instance_record_bytes each) to `out`, which the TLAS build reads
// from a buffer with k_build_input_usage.
void write_instances(std::span<const TlasInstance> instances, void* out) noexcept;
void build_tlas(CommandList commands, const AccelerationStructure& tlas, DeviceAddress instances,
                u32 instance_count, AccelerationBuildFlags flags, const BufferResource& scratch);

// Device-local scratch with the alignment the builder requires; reusable across builds that do
// not overlap in the same command buffer without a barrier.
bool create_scratch(const Device& device, u64 bytes, BufferResource& out,
                    std::string* error = nullptr);

// Between a build and whatever reads its result: another build (a BLAS feeding a TLAS,
// PipelineStage::AccelerationBuild and MemoryAccess::AccelerationStructureRead) or the shaders
// that trace (PipelineStage::ComputeShader and MemoryAccess::AccelerationStructureRead). The
// render graph derives this barrier for every build it records (Access::AccelerationBuildWrite);
// this is for the tests and tools that record a build outside a graph.
void acceleration_build_barrier(CommandList commands, PipelineStage dst_stage,
                                MemoryAccess dst_access) noexcept;

void destroy_acceleration_structure(const Device& device, AccelerationStructure& as) noexcept;

}  // namespace engine::gfx

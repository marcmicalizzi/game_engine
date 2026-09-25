#include <domain/gfx/acceleration.h>
#include <domain/gfx/backend/vulkan/vulkan.h>

#include <cstring>

namespace engine::gfx {

namespace {

void fail(std::string* error, const char* what, VkResult r) {
  if (error != nullptr) *error = std::string(what) + ": " + result_name(r);
}

u32 scratch_alignment(const Device& device) {
  VkPhysicalDeviceAccelerationStructurePropertiesKHR as_props{};
  as_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
  VkPhysicalDeviceProperties2 props{};
  props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props.pNext = &as_props;
  vkGetPhysicalDeviceProperties2(device.handles().physical, &props);
  return as_props.minAccelerationStructureScratchOffsetAlignment > 0
             ? as_props.minAccelerationStructureScratchOffsetAlignment
             : 256u;
}

VkDeviceAddress align_up(VkDeviceAddress address, u64 alignment) noexcept {
  return (address + alignment - 1) / alignment * alignment;
}

struct BlasBuild {
  Vector<VkAccelerationStructureGeometryKHR> geometries;
  Vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
  Vector<u32> primitive_counts;
  VkAccelerationStructureBuildGeometryInfoKHR info{};
};

void fill_blas_build(std::span<const ClusterGeometry> clusters, AccelerationBuildFlags flags,
                     BlasBuild& build) {
  const u32 count = static_cast<u32>(clusters.size());
  build.geometries.resize(count);
  build.ranges.resize(count);
  build.primitive_counts.resize(count);
  for (u32 i = 0; i < count; ++i) {
    const ClusterGeometry& cluster = clusters[i];
    VkAccelerationStructureGeometryKHR& geometry = build.geometries[i];
    geometry = VkAccelerationStructureGeometryKHR{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    VkAccelerationStructureGeometryTrianglesDataKHR& triangles = geometry.geometry.triangles;
    triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = cluster.vertices;
    triangles.vertexStride = sizeof(f32) * 3;
    triangles.maxVertex = cluster.vertex_count > 0 ? cluster.vertex_count - 1 : 0;
    triangles.indexType = VK_INDEX_TYPE_UINT16;
    triangles.indexData.deviceAddress = cluster.indices;
    build.ranges[i] = VkAccelerationStructureBuildRangeInfoKHR{cluster.triangle_count, 0, 0, 0};
    build.primitive_counts[i] = cluster.triangle_count;
  }
  build.info = VkAccelerationStructureBuildGeometryInfoKHR{};
  build.info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  build.info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  build.info.flags = vk::native(flags);
  build.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.info.geometryCount = count;
  build.info.pGeometries = build.geometries.data();
}

void fill_tlas_build(VkDeviceAddress instances, AccelerationBuildFlags flags,
                     VkAccelerationStructureGeometryKHR& geometry,
                     VkAccelerationStructureBuildGeometryInfoKHR& info) {
  geometry = VkAccelerationStructureGeometryKHR{};
  geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geometry.geometry.instances.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  geometry.geometry.instances.arrayOfPointers = VK_FALSE;
  geometry.geometry.instances.data.deviceAddress = instances;
  info = VkAccelerationStructureBuildGeometryInfoKHR{};
  info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  info.flags = vk::native(flags);
  info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  info.geometryCount = 1;
  info.pGeometries = &geometry;
}

// Buffer, handle, and address for a structure of `bytes`; `out.buffer` is created here.
bool create_structure(const Device& device, VkAccelerationStructureTypeKHR type,
                      const VkAccelerationStructureBuildSizesInfoKHR& sizes,
                      AccelerationStructure& out, std::string* error) {
  const Handles& h = device.handles();
  if (!create_buffer(device, sizes.accelerationStructureSize,
                     BufferUsage::AccelerationStorage | BufferUsage::ShaderDeviceAddress, false,
                     out.buffer, error)) {
    return false;
  }
  VkAccelerationStructureCreateInfoKHR create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
  create_info.buffer = vk::native(out.buffer.buffer);
  create_info.size = sizes.accelerationStructureSize;
  create_info.type = type;
  VkAccelerationStructureKHR structure = VK_NULL_HANDLE;
  if (const VkResult r =
          vkCreateAccelerationStructureKHR(h.device, &create_info, nullptr, &structure);
      r != VK_SUCCESS) {
    fail(error, "vkCreateAccelerationStructureKHR", r);
    destroy_buffer(device, out.buffer);
    return false;
  }
  out.handle = vk::wrap(structure);
  VkAccelerationStructureDeviceAddressInfoKHR address_info{};
  address_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
  address_info.accelerationStructure = structure;
  out.address = vkGetAccelerationStructureDeviceAddressKHR(h.device, &address_info);
  const u32 alignment = scratch_alignment(device);
  out.scratch_alignment = alignment;
  out.build_scratch_bytes = sizes.buildScratchSize + alignment;
  return true;
}

}  // namespace

void expand_packed_triangles(std::span<const u32> packed, Vector<u16>& out) {
  out.reserve(out.size() + static_cast<u32>(packed.size()) * 3);
  for (const u32 triangle : packed) {
    out.push_back(static_cast<u16>(triangle & 0xffu));
    out.push_back(static_cast<u16>((triangle >> 8) & 0xffu));
    out.push_back(static_cast<u16>((triangle >> 16) & 0xffu));
  }
}

bool create_blas(const Device& device, std::span<const ClusterGeometry> clusters,
                 AccelerationBuildFlags flags, AccelerationStructure& out, std::string* error) {
  out = AccelerationStructure{};
  if (!device.features().acceleration_structure) {
    if (error != nullptr) *error = "create_blas: the device has no acceleration structures";
    return false;
  }
  if (clusters.empty()) {
    if (error != nullptr) *error = "create_blas: no geometry";
    return false;
  }
  BlasBuild build;
  fill_blas_build(clusters, flags, build);
  VkAccelerationStructureBuildSizesInfoKHR sizes{};
  sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
  vkGetAccelerationStructureBuildSizesKHR(device.handles().device,
                                          VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                          &build.info, build.primitive_counts.data(), &sizes);
  if (!create_structure(device, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizes, out,
                        error)) {
    return false;
  }
  out.geometry_count = static_cast<u32>(clusters.size());
  return true;
}

void build_blas(CommandList commands, const AccelerationStructure& blas,
                std::span<const ClusterGeometry> clusters, AccelerationBuildFlags flags,
                const BufferResource& scratch) {
  BlasBuild build;
  fill_blas_build(clusters, flags, build);
  build.info.dstAccelerationStructure = vk::native(blas.handle);
  build.info.scratchData.deviceAddress = align_up(scratch.address, blas.scratch_alignment);
  const VkAccelerationStructureBuildRangeInfoKHR* ranges = build.ranges.data();
  vkCmdBuildAccelerationStructuresKHR(vk::native(commands), 1, &build.info, &ranges);
}

bool create_tlas(const Device& device, u32 instance_count, AccelerationBuildFlags flags,
                 AccelerationStructure& out, std::string* error) {
  out = AccelerationStructure{};
  if (!device.features().acceleration_structure) {
    if (error != nullptr) *error = "create_tlas: the device has no acceleration structures";
    return false;
  }
  if (instance_count == 0) {
    if (error != nullptr) *error = "create_tlas: no instances";
    return false;
  }
  VkAccelerationStructureGeometryKHR geometry;
  VkAccelerationStructureBuildGeometryInfoKHR info;
  fill_tlas_build(0, flags, geometry, info);
  VkAccelerationStructureBuildSizesInfoKHR sizes{};
  sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
  vkGetAccelerationStructureBuildSizesKHR(device.handles().device,
                                          VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
                                          &instance_count, &sizes);
  if (!create_structure(device, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, sizes, out, error))
    return false;
  out.geometry_count = instance_count;
  return true;
}

void write_instances(std::span<const TlasInstance> instances, void* out) noexcept {
  auto* bytes = static_cast<u8*>(out);
  for (u32 i = 0; i < instances.size(); ++i) {
    const TlasInstance& desc = instances[i];
    VkAccelerationStructureInstanceKHR record{};
    for (u32 row = 0; row < 3; ++row) {
      for (u32 col = 0; col < 4; ++col)
        record.transform.matrix[row][col] = desc.transform.at(row, col);
    }
    record.instanceCustomIndex = desc.custom_index & 0xffffffu;
    record.mask = desc.mask & 0xffu;
    record.instanceShaderBindingTableRecordOffset = 0;
    record.flags = static_cast<u32>(desc.flags) & 0xffu;
    record.accelerationStructureReference = desc.blas;
    static_assert(sizeof(record) == k_instance_record_bytes);
    std::memcpy(bytes + u64{i} * k_instance_record_bytes, &record, sizeof(record));
  }
}

void build_tlas(CommandList commands, const AccelerationStructure& tlas, DeviceAddress instances,
                u32 instance_count, AccelerationBuildFlags flags, const BufferResource& scratch) {
  VkAccelerationStructureGeometryKHR geometry;
  VkAccelerationStructureBuildGeometryInfoKHR info;
  fill_tlas_build(instances, flags, geometry, info);
  info.dstAccelerationStructure = vk::native(tlas.handle);
  info.scratchData.deviceAddress = align_up(scratch.address, tlas.scratch_alignment);
  const VkAccelerationStructureBuildRangeInfoKHR range{instance_count, 0, 0, 0};
  const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
  vkCmdBuildAccelerationStructuresKHR(vk::native(commands), 1, &info, &ranges);
}

bool create_scratch(const Device& device, u64 bytes, BufferResource& out, std::string* error) {
  return create_buffer(device, bytes, BufferUsage::Storage | BufferUsage::ShaderDeviceAddress,
                       false, out, error);
}

void acceleration_build_barrier(CommandList commands, PipelineStage dst_stage,
                                MemoryAccess dst_access) noexcept {
  VkMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  barrier.dstStageMask = vk::native(dst_stage);
  barrier.dstAccessMask = vk::native(dst_access);
  VkDependencyInfo dependency{};
  dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(vk::native(commands), &dependency);
}

void destroy_acceleration_structure(const Device& device, AccelerationStructure& as) noexcept {
  if (as.handle)
    vkDestroyAccelerationStructureKHR(device.handles().device, vk::native(as.handle), nullptr);
  destroy_buffer(device, as.buffer);
  as = AccelerationStructure{};
}

}  // namespace engine::gfx

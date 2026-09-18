#include <domain/gfx/cluster_acceleration.h>

#include <cstring>

namespace engine::gfx {

namespace {

VkDeviceAddress align_up(VkDeviceAddress address, u64 alignment) noexcept {
  return alignment == 0 ? address : (address + alignment - 1) / alignment * alignment;
}

void set_message(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
}

VkClusterAccelerationStructureTriangleClusterInputNV triangle_input(
    const ClusterSetLimits& limits) {
  VkClusterAccelerationStructureTriangleClusterInputNV input{};
  input.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_TRIANGLE_CLUSTER_INPUT_NV;
  input.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
  input.maxGeometryIndexValue = limits.max_geometry_index;
  input.maxClusterUniqueGeometryCount = 1;
  input.maxClusterTriangleCount = limits.max_triangles_per_cluster;
  input.maxClusterVertexCount = limits.max_vertices_per_cluster;
  input.maxTotalTriangleCount = limits.max_triangles_per_cluster * limits.max_clusters;
  input.maxTotalVertexCount = limits.max_vertices_per_cluster * limits.max_clusters;
  input.minPositionTruncateBitCount = 0;
  return input;
}

VkClusterAccelerationStructureInputInfoNV op_input(
    const ClusterSetLimits& limits, VkClusterAccelerationStructureOpTypeNV op,
    VkClusterAccelerationStructureTriangleClusterInputNV& triangles) {
  VkClusterAccelerationStructureInputInfoNV info{};
  info.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV;
  info.maxAccelerationStructureCount = limits.max_clusters;
  info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  info.opType = op;
  info.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_IMPLICIT_DESTINATIONS_NV;
  info.opInput.pTriangleClusters = &triangles;
  return info;
}

// The op a ClusterSet's build runs: clusters from scratch, or templates instantiated with new
// positions. Both write CLAS into the set's storage and both report addresses and sizes.
VkClusterAccelerationStructureOpTypeNV set_op(const ClusterSetLimits& limits) {
  return limits.instantiate
             ? VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_INSTANTIATE_TRIANGLE_CLUSTER_NV
             : VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_NV;
}

u64 set_record_bytes(const ClusterSetLimits& limits) {
  return limits.instantiate ? k_cluster_instantiate_record_bytes : k_cluster_build_record_bytes;
}

VkClusterAccelerationStructureInputInfoNV set_input(
    const ClusterSetLimits& limits,
    VkClusterAccelerationStructureTriangleClusterInputNV& triangles) {
  return op_input(limits, set_op(limits), triangles);
}

VkClusterAccelerationStructureInputInfoNV blas_input(
    u32 max_clusters, VkClusterAccelerationStructureClustersBottomLevelInputNV& clusters) {
  clusters = VkClusterAccelerationStructureClustersBottomLevelInputNV{};
  clusters.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_CLUSTERS_BOTTOM_LEVEL_INPUT_NV;
  clusters.maxTotalClusterCount = max_clusters;
  clusters.maxClusterCountPerAccelerationStructure = max_clusters;
  VkClusterAccelerationStructureInputInfoNV info{};
  info.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV;
  info.maxAccelerationStructureCount = 1;
  info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  info.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_CLUSTERS_BOTTOM_LEVEL_NV;
  info.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_IMPLICIT_DESTINATIONS_NV;
  info.opInput.pClustersBottomLevel = &clusters;
  return info;
}

constexpr VkBufferUsageFlags k_structure_usage =
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
constexpr VkBufferUsageFlags k_output_usage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;

}  // namespace

bool cluster_as_properties(const Device& device, ClusterAsProperties& out) noexcept {
  out = ClusterAsProperties{};
  if (!device.features().cluster_acceleration_structure) return false;
  VkPhysicalDeviceClusterAccelerationStructurePropertiesNV cluster{};
  cluster.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_PROPERTIES_NV;
  VkPhysicalDeviceProperties2 props{};
  props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props.pNext = &cluster;
  vkGetPhysicalDeviceProperties2(device.handles().physical, &props);
  out.max_vertices_per_cluster = cluster.maxVerticesPerCluster;
  out.max_triangles_per_cluster = cluster.maxTrianglesPerCluster;
  out.max_geometry_index = cluster.maxClusterGeometryIndex;
  out.cluster_alignment = cluster.clusterByteAlignment;
  out.scratch_alignment = cluster.clusterScratchByteAlignment;
  out.bottom_level_alignment = cluster.clusterBottomLevelByteAlignment;
  out.template_alignment = cluster.clusterTemplateByteAlignment;
  return true;
}

void pack_cluster_indices(std::span<const u32> packed, Vector<u8>& out) {
  out.reserve(out.size() + static_cast<u32>(packed.size()) * 3);
  for (const u32 triangle : packed) {
    out.push_back(static_cast<u8>(triangle & 0xffu));
    out.push_back(static_cast<u8>((triangle >> 8) & 0xffu));
    out.push_back(static_cast<u8>((triangle >> 16) & 0xffu));
  }
}

namespace {

// The cluster build record and the template build record share their first 64 bytes field for
// field; the template's are followed by one more address. One function fills both.
template <typename Record>
void fill_triangle_record(Record& record, const ClusterBuildInput& c) noexcept {
  record.clusterID = c.cluster_id;
  record.clusterFlags = 0;
  record.triangleCount = c.triangle_count & 0x1ffu;
  record.vertexCount = c.vertex_count & 0x1ffu;
  record.positionTruncateBitCount = 0;
  record.indexType = VK_CLUSTER_ACCELERATION_STRUCTURE_INDEX_FORMAT_8BIT_NV;
  record.opacityMicromapIndexType = 0;
  record.baseGeometryIndexAndGeometryFlags.geometryIndex = c.cluster_id & 0xffffffu;
  record.baseGeometryIndexAndGeometryFlags.geometryFlags =
      VK_CLUSTER_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT_NV;
  record.indexBufferStride = 1;
  record.vertexBufferStride = sizeof(f32) * 3;
  record.geometryIndexAndFlagsBufferStride = 0;
  record.opacityMicromapIndexBufferStride = 0;
  record.indexBuffer = c.indices;
  record.vertexBuffer = c.vertices;
  record.geometryIndexAndFlagsBuffer = 0;
  record.opacityMicromapArray = 0;
  record.opacityMicromapIndexBuffer = 0;
}

}  // namespace

void write_cluster_build_records(std::span<const ClusterBuildInput> clusters, void* out) noexcept {
  static_assert(sizeof(VkClusterAccelerationStructureBuildTriangleClusterInfoNV) ==
                k_cluster_build_record_bytes);
  auto* bytes = static_cast<u8*>(out);
  for (u32 i = 0; i < clusters.size(); ++i) {
    VkClusterAccelerationStructureBuildTriangleClusterInfoNV record{};
    fill_triangle_record(record, clusters[i]);
    std::memcpy(bytes + u64{i} * k_cluster_build_record_bytes, &record, sizeof(record));
  }
}

void write_cluster_template_records(std::span<const ClusterBuildInput> clusters,
                                    void* out) noexcept {
  static_assert(sizeof(VkClusterAccelerationStructureBuildTriangleClusterTemplateInfoNV) ==
                k_cluster_template_record_bytes);
  auto* bytes = static_cast<u8*>(out);
  for (u32 i = 0; i < clusters.size(); ++i) {
    VkClusterAccelerationStructureBuildTriangleClusterTemplateInfoNV record{};
    fill_triangle_record(record, clusters[i]);
    record.instantiationBoundingBoxLimit = 0;  // no authored limit on how far a vertex moves
    std::memcpy(bytes + u64{i} * k_cluster_template_record_bytes, &record, sizeof(record));
  }
}

void write_cluster_instantiate_records(std::span<const ClusterInstantiateInput> clusters,
                                       void* out) noexcept {
  static_assert(sizeof(VkClusterAccelerationStructureInstantiateClusterInfoNV) ==
                k_cluster_instantiate_record_bytes);
  auto* bytes = static_cast<u8*>(out);
  for (u32 i = 0; i < clusters.size(); ++i) {
    const ClusterInstantiateInput& c = clusters[i];
    VkClusterAccelerationStructureInstantiateClusterInfoNV record{};
    record.clusterIdOffset = c.cluster_id;
    record.geometryIndexOffset = c.geometry_index & 0xffffffu;
    record.reserved = 0;
    record.clusterTemplateAddress = c.cluster_template;
    record.vertexBuffer.startAddress = c.vertices;
    record.vertexBuffer.strideInBytes = sizeof(f32) * 3;
    std::memcpy(bytes + u64{i} * k_cluster_instantiate_record_bytes, &record, sizeof(record));
  }
}

bool create_cluster_set(const Device& device, const ClusterSetLimits& limits, ClusterSet& out,
                        std::string* error) {
  out = ClusterSet{};
  ClusterAsProperties props;
  if (!cluster_as_properties(device, props)) {
    set_message(error, "create_cluster_set: the device has no cluster acceleration structures");
    return false;
  }
  if (limits.max_clusters == 0 ||
      limits.max_triangles_per_cluster > props.max_triangles_per_cluster ||
      limits.max_vertices_per_cluster > props.max_vertices_per_cluster ||
      limits.max_geometry_index > props.max_geometry_index) {
    set_message(error, "create_cluster_set: limits exceed what the device allows");
    return false;
  }
  VkClusterAccelerationStructureTriangleClusterInputNV triangles = triangle_input(limits);
  const VkClusterAccelerationStructureInputInfoNV input = set_input(limits, triangles);
  VkAccelerationStructureBuildSizesInfoKHR sizes{};
  sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
  vkGetClusterAccelerationStructureBuildSizesNV(device.handles().device, &input, &sizes);
  out.alignment = props.cluster_alignment;
  out.limits = limits;
  out.max_clusters = limits.max_clusters;
  out.build_scratch_bytes = sizes.buildScratchSize + props.scratch_alignment;
  if (!create_buffer(device, sizes.accelerationStructureSize + props.cluster_alignment,
                     k_structure_usage, false, out.data, error) ||
      !create_buffer(device, u64{limits.max_clusters} * sizeof(u64), k_output_usage, true,
                     out.addresses, error) ||
      !create_buffer(device, u64{limits.max_clusters} * sizeof(u32), k_output_usage, true,
                     out.sizes, error)) {
    destroy_cluster_set(device, out);
    return false;
  }
  return true;
}

void build_cluster_set(VkCommandBuffer commands, const ClusterSet& set, VkDeviceAddress records,
                       VkDeviceAddress count, const BufferResource& scratch) {
  VkClusterAccelerationStructureTriangleClusterInputNV triangles = triangle_input(set.limits);
  VkClusterAccelerationStructureCommandsInfoNV info{};
  info.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV;
  info.input = set_input(set.limits, triangles);
  info.dstImplicitData = align_up(set.data.address, set.alignment);
  info.scratchData = align_up(scratch.address, set.alignment);
  info.dstAddressesArray = {set.addresses.address, sizeof(u64), set.addresses.size};
  info.dstSizesArray = {set.sizes.address, sizeof(u32), set.sizes.size};
  const u64 stride = set_record_bytes(set.limits);
  info.srcInfosArray = {records, stride, stride * set.max_clusters};
  info.srcInfosCount = count;
  info.addressResolutionFlags = 0;
  vkCmdBuildClusterAccelerationStructureIndirectNV(commands, &info);
}

void instantiate_cluster_templates(VkCommandBuffer commands, const ClusterSet& set,
                                   VkDeviceAddress records, VkDeviceAddress count,
                                   const BufferResource& scratch) {
  build_cluster_set(commands, set, records, count, scratch);  // the op comes from set.limits
}

void destroy_cluster_set(const Device& device, ClusterSet& set) noexcept {
  destroy_buffer(device, set.data);
  destroy_buffer(device, set.addresses);
  destroy_buffer(device, set.sizes);
  set = ClusterSet{};
}

bool create_cluster_templates(const Device& device, const ClusterSetLimits& limits,
                              ClusterTemplateSet& out, std::string* error) {
  out = ClusterTemplateSet{};
  ClusterAsProperties props;
  if (!cluster_as_properties(device, props)) {
    set_message(error,
                "create_cluster_templates: the device has no cluster acceleration structures");
    return false;
  }
  if (limits.max_clusters == 0 ||
      limits.max_triangles_per_cluster > props.max_triangles_per_cluster ||
      limits.max_vertices_per_cluster > props.max_vertices_per_cluster ||
      limits.max_geometry_index > props.max_geometry_index) {
    set_message(error, "create_cluster_templates: limits exceed what the device allows");
    return false;
  }
  VkClusterAccelerationStructureTriangleClusterInputNV triangles = triangle_input(limits);
  const VkClusterAccelerationStructureInputInfoNV input =
      op_input(limits, VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_TEMPLATE_NV,
               triangles);
  VkAccelerationStructureBuildSizesInfoKHR sizes{};
  sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
  vkGetClusterAccelerationStructureBuildSizesNV(device.handles().device, &input, &sizes);
  out.alignment = props.template_alignment;
  out.limits = limits;
  out.max_clusters = limits.max_clusters;
  out.build_scratch_bytes = sizes.buildScratchSize + props.scratch_alignment;
  if (!create_buffer(device, sizes.accelerationStructureSize + props.template_alignment,
                     k_structure_usage, false, out.data, error) ||
      !create_buffer(device, u64{limits.max_clusters} * sizeof(u64), k_output_usage, true,
                     out.addresses, error) ||
      !create_buffer(device, u64{limits.max_clusters} * sizeof(u32), k_output_usage, true,
                     out.sizes, error)) {
    destroy_cluster_templates(device, out);
    return false;
  }
  return true;
}

void build_cluster_templates(VkCommandBuffer commands, const ClusterTemplateSet& set,
                             VkDeviceAddress records, VkDeviceAddress count,
                             const BufferResource& scratch) {
  VkClusterAccelerationStructureTriangleClusterInputNV triangles = triangle_input(set.limits);
  VkClusterAccelerationStructureCommandsInfoNV info{};
  info.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV;
  info.input = op_input(
      set.limits, VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_TEMPLATE_NV,
      triangles);
  info.dstImplicitData = align_up(set.data.address, set.alignment);
  info.scratchData = align_up(scratch.address, set.alignment);
  info.dstAddressesArray = {set.addresses.address, sizeof(u64), set.addresses.size};
  info.dstSizesArray = {set.sizes.address, sizeof(u32), set.sizes.size};
  info.srcInfosArray = {records, k_cluster_template_record_bytes,
                        k_cluster_template_record_bytes * set.max_clusters};
  info.srcInfosCount = count;
  info.addressResolutionFlags = 0;
  vkCmdBuildClusterAccelerationStructureIndirectNV(commands, &info);
}

void destroy_cluster_templates(const Device& device, ClusterTemplateSet& set) noexcept {
  destroy_buffer(device, set.data);
  destroy_buffer(device, set.addresses);
  destroy_buffer(device, set.sizes);
  set = ClusterTemplateSet{};
}

bool create_cluster_blas(const Device& device, u32 max_clusters, ClusterBlas& out,
                         std::string* error) {
  out = ClusterBlas{};
  ClusterAsProperties props;
  if (!cluster_as_properties(device, props)) {
    set_message(error, "create_cluster_blas: the device has no cluster acceleration structures");
    return false;
  }
  if (max_clusters == 0) {
    set_message(error, "create_cluster_blas: no clusters");
    return false;
  }
  VkClusterAccelerationStructureClustersBottomLevelInputNV clusters;
  VkClusterAccelerationStructureInputInfoNV input = blas_input(max_clusters, clusters);
  input.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_EXPLICIT_DESTINATIONS_NV;
  VkAccelerationStructureBuildSizesInfoKHR sizes{};
  sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
  vkGetClusterAccelerationStructureBuildSizesNV(device.handles().device, &input, &sizes);
  out.alignment = props.bottom_level_alignment;
  out.max_clusters = max_clusters;
  out.build_scratch_bytes = sizes.buildScratchSize + props.scratch_alignment;
  static_assert(sizeof(VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV) ==
                k_cluster_blas_record_bytes);
  if (!create_buffer(device, sizes.accelerationStructureSize + props.bottom_level_alignment,
                     k_structure_usage, false, out.data, error) ||
      !create_buffer(device, k_cluster_blas_record_bytes, k_output_usage, true, out.record,
                     error) ||
      !create_buffer(device, sizeof(u64), k_output_usage, true, out.destination, error)) {
    destroy_cluster_blas(device, out);
    return false;
  }
  // Explicit destination: the structure always lands at the aligned start of `data`.
  out.address = align_up(out.data.address, props.bottom_level_alignment);
  std::memcpy(out.destination.mapped, &out.address, sizeof(out.address));
  std::memset(out.record.mapped, 0, k_cluster_blas_record_bytes);
  return true;
}

void build_cluster_blas_indirect(VkCommandBuffer commands, const ClusterBlas& blas,
                                 const BufferResource& scratch, VkDeviceAddress record_address) {
  VkClusterAccelerationStructureClustersBottomLevelInputNV clusters;
  VkClusterAccelerationStructureCommandsInfoNV info{};
  info.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV;
  info.input = blas_input(blas.max_clusters, clusters);
  info.input.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_EXPLICIT_DESTINATIONS_NV;
  info.dstImplicitData = 0;
  info.scratchData = align_up(scratch.address, blas.alignment);
  info.dstAddressesArray = {blas.destination.address, sizeof(u64), sizeof(u64)};
  info.dstSizesArray = {0, 0, 0};
  info.srcInfosArray = {record_address != 0 ? record_address : blas.record.address,
                        k_cluster_blas_record_bytes, k_cluster_blas_record_bytes};
  info.srcInfosCount = 0;
  info.addressResolutionFlags = 0;
  vkCmdBuildClusterAccelerationStructureIndirectNV(commands, &info);
}

void build_cluster_blas(VkCommandBuffer commands, const ClusterBlas& blas,
                        VkDeviceAddress references, u32 count, const BufferResource& scratch) {
  VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV record{};
  record.clusterReferencesCount = count;
  record.clusterReferencesStride = sizeof(u64);
  record.clusterReferences = references;
  std::memcpy(blas.record.mapped, &record, sizeof(record));
  build_cluster_blas_indirect(commands, blas, scratch);
}

void destroy_cluster_blas(const Device& device, ClusterBlas& blas) noexcept {
  destroy_buffer(device, blas.data);
  destroy_buffer(device, blas.record);
  destroy_buffer(device, blas.destination);
  blas = ClusterBlas{};
}

}  // namespace engine::gfx

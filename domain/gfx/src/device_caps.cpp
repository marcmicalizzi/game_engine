// The Vulkan half of domain/gfx/requirements.h: one query that fills `DeviceCaps`.
//
// It is a file of its own so that the table, the verdict and the clamping stay Vulkan-free and
// testable without a driver, and so that `enumerate_adapters` and `Device::create` read a device
// through exactly one function. Everything is queried **behind its own version or extension
// gate**: handing a VkPhysicalDeviceVulkan13Features to a 1.2 device is invalid usage, and a 1.2
// device — a 2014 Kepler laptop, say — is precisely the machine this report exists for, so the
// query that describes it must not be the thing that breaks on it.

#include <core/containers/vector.h>
#include <domain/gfx/vulkan.h>

#include <cstring>

namespace engine::gfx {

namespace {

u32 flag(VkBool32 value) noexcept { return value == VK_TRUE ? 1u : 0u; }

}  // namespace

void read_device_caps(VkPhysicalDevice physical, DeviceCaps& out) {
  out = DeviceCaps{};
  if (physical == VK_NULL_HANDLE) return;

  VkPhysicalDeviceProperties base_properties{};
  vkGetPhysicalDeviceProperties(physical, &base_properties);
  const u32 api = base_properties.apiVersion;
  out.api_version = VK_API_VERSION_MAJOR(api) * 1000u + VK_API_VERSION_MINOR(api);

  // ---- extensions ----
  u32 extension_count = 0;
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, nullptr);
  Vector<VkExtensionProperties> extensions(extension_count);
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, extensions.data());
  auto has = [&](const char* name) {
    for (const VkExtensionProperties& e : extensions) {
      if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
  };
  const bool ext_swapchain = has("VK_KHR_swapchain");
  const bool ext_mesh = has("VK_EXT_mesh_shader");
  const bool ext_deferred = has("VK_KHR_deferred_host_operations");
  const bool ext_as = ext_deferred && has("VK_KHR_acceleration_structure");
  const bool ext_rt = ext_as && has("VK_KHR_ray_tracing_pipeline");
  const bool ext_rq = ext_as && has("VK_KHR_ray_query");
  const bool ext_cluster = ext_as && has("VK_NV_cluster_acceleration_structure");
  const bool ext_descriptor_buffer = has("VK_EXT_descriptor_buffer");
  const bool ext_decompression = has("VK_EXT_memory_decompression");
  out.swapchain = flag(ext_swapchain ? VK_TRUE : VK_FALSE);
  out.deferred_host_operations = flag(ext_deferred ? VK_TRUE : VK_FALSE);
  out.memory_budget = flag(has("VK_EXT_memory_budget") ? VK_TRUE : VK_FALSE);
  out.fragment_shading_rate = flag(has("VK_KHR_fragment_shading_rate") ? VK_TRUE : VK_FALSE);
  out.index_type_uint8 =
      flag((has("VK_EXT_index_type_uint8") || has("VK_KHR_index_type_uint8")) ? VK_TRUE : VK_FALSE);

  // ---- features ----
  VkPhysicalDeviceVulkan11Features v11{};
  v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
  VkPhysicalDeviceVulkan12Features v12{};
  v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  VkPhysicalDeviceVulkan13Features v13{};
  v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  VkPhysicalDeviceMeshShaderFeaturesEXT mesh{};
  mesh.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
  VkPhysicalDeviceAccelerationStructureFeaturesKHR as{};
  as.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
  VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt{};
  rt.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
  VkPhysicalDeviceRayQueryFeaturesKHR rq{};
  rq.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
  VkPhysicalDeviceDescriptorBufferFeaturesEXT db{};
  db.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
#if defined(VK_NV_cluster_acceleration_structure)
  VkPhysicalDeviceClusterAccelerationStructureFeaturesNV cluster{};
  cluster.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_FEATURES_NV;
#endif
#if defined(VK_EXT_memory_decompression)
  VkPhysicalDeviceMemoryDecompressionFeaturesEXT decompression{};
  decompression.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_DECOMPRESSION_FEATURES_EXT;
#endif

  VkPhysicalDeviceFeatures2 f2{};
  f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  void** tail = &f2.pNext;
  auto link = [&](VkBaseOutStructure* node) {
    *tail = node;
    tail = reinterpret_cast<void**>(&node->pNext);
  };
  if (api >= VK_API_VERSION_1_1) link(reinterpret_cast<VkBaseOutStructure*>(&v11));
  if (api >= VK_API_VERSION_1_2) link(reinterpret_cast<VkBaseOutStructure*>(&v12));
  if (api >= VK_API_VERSION_1_3) link(reinterpret_cast<VkBaseOutStructure*>(&v13));
  if (ext_mesh) link(reinterpret_cast<VkBaseOutStructure*>(&mesh));
  if (ext_as) link(reinterpret_cast<VkBaseOutStructure*>(&as));
  if (ext_rt) link(reinterpret_cast<VkBaseOutStructure*>(&rt));
  if (ext_rq) link(reinterpret_cast<VkBaseOutStructure*>(&rq));
  if (ext_descriptor_buffer) link(reinterpret_cast<VkBaseOutStructure*>(&db));
#if defined(VK_NV_cluster_acceleration_structure)
  if (ext_cluster) link(reinterpret_cast<VkBaseOutStructure*>(&cluster));
#endif
#if defined(VK_EXT_memory_decompression)
  if (ext_decompression) link(reinterpret_cast<VkBaseOutStructure*>(&decompression));
#endif
  vkGetPhysicalDeviceFeatures2(physical, &f2);

  out.multi_draw_indirect = flag(f2.features.multiDrawIndirect);
  out.shader_int64 = flag(f2.features.shaderInt64);
  out.fragment_stores_and_atomics = flag(f2.features.fragmentStoresAndAtomics);
  out.sampler_anisotropy = flag(f2.features.samplerAnisotropy);
  out.fill_mode_non_solid = flag(f2.features.fillModeNonSolid);
  out.shader_int16 = flag(f2.features.shaderInt16);
  out.shader_storage_image_write_without_format =
      flag(f2.features.shaderStorageImageWriteWithoutFormat);
  out.shader_storage_image_read_without_format =
      flag(f2.features.shaderStorageImageReadWithoutFormat);
  out.vertex_pipeline_stores_and_atomics = flag(f2.features.vertexPipelineStoresAndAtomics);
  out.geometry_shader = flag(f2.features.geometryShader);
  out.full_draw_index_uint32 = flag(f2.features.fullDrawIndexUint32);

  out.shader_draw_parameters = flag(v11.shaderDrawParameters);
  out.storage_buffer_16bit_access = flag(v11.storageBuffer16BitAccess);

  out.buffer_device_address = flag(v12.bufferDeviceAddress);
  out.descriptor_indexing = flag(v12.descriptorIndexing);
  out.runtime_descriptor_array = flag(v12.runtimeDescriptorArray);
  out.descriptor_binding_partially_bound = flag(v12.descriptorBindingPartiallyBound);
  out.descriptor_binding_variable_descriptor_count =
      flag(v12.descriptorBindingVariableDescriptorCount);
  out.descriptor_binding_sampled_image_update_after_bind =
      flag(v12.descriptorBindingSampledImageUpdateAfterBind);
  out.descriptor_binding_storage_image_update_after_bind =
      flag(v12.descriptorBindingStorageImageUpdateAfterBind);
  out.descriptor_binding_storage_buffer_update_after_bind =
      flag(v12.descriptorBindingStorageBufferUpdateAfterBind);
  out.shader_sampled_image_array_non_uniform_indexing =
      flag(v12.shaderSampledImageArrayNonUniformIndexing);
  out.shader_storage_buffer_array_non_uniform_indexing =
      flag(v12.shaderStorageBufferArrayNonUniformIndexing);
  out.timeline_semaphore = flag(v12.timelineSemaphore);
  out.scalar_block_layout = flag(v12.scalarBlockLayout);
  out.host_query_reset = flag(v12.hostQueryReset);
  out.draw_indirect_count = flag(v12.drawIndirectCount);
  out.shader_buffer_int64_atomics = flag(v12.shaderBufferInt64Atomics);
  out.shader_int8 = flag(v12.shaderInt8);
  out.storage_buffer_8bit_access = flag(v12.storageBuffer8BitAccess);
  out.shader_float16 = flag(v12.shaderFloat16);
  out.sampler_filter_minmax = flag(v12.samplerFilterMinmax);

  out.dynamic_rendering = flag(v13.dynamicRendering);
  out.synchronization2 = flag(v13.synchronization2);
  out.maintenance4 = flag(v13.maintenance4);
  out.shader_demote_to_helper_invocation = flag(v13.shaderDemoteToHelperInvocation);
  out.subgroup_size_control = flag(v13.subgroupSizeControl);
  out.compute_full_subgroups = flag(v13.computeFullSubgroups);

  // An extension's row is the extension *and* the feature bit the engine would enable with it,
  // because an extension advertised with its feature off is not a capability.
  out.mesh_shader = ext_mesh ? flag(mesh.meshShader) : 0u;
  out.task_shader = ext_mesh ? flag(mesh.taskShader) : 0u;
  out.acceleration_structure = ext_as ? flag(as.accelerationStructure) : 0u;
  out.ray_tracing_pipeline =
      ext_rt && out.acceleration_structure != 0 ? flag(rt.rayTracingPipeline) : 0u;
  out.ray_query = ext_rq && out.acceleration_structure != 0 ? flag(rq.rayQuery) : 0u;
  out.descriptor_buffer = ext_descriptor_buffer ? flag(db.descriptorBuffer) : 0u;
#if defined(VK_NV_cluster_acceleration_structure)
  out.cluster_acceleration_structure = ext_cluster && out.acceleration_structure != 0
                                           ? flag(cluster.clusterAccelerationStructure)
                                           : 0u;
#else
  out.cluster_acceleration_structure = 0u;
#endif
#if defined(VK_EXT_memory_decompression)
  out.memory_decompression = ext_decompression ? flag(decompression.memoryDecompression) : 0u;
#else
  out.memory_decompression = 0u;
#endif

  // ---- limits ----
  VkPhysicalDeviceSubgroupProperties subgroup{};
  subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
  VkPhysicalDeviceDescriptorIndexingProperties indexing{};
  indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
  VkPhysicalDeviceAccelerationStructurePropertiesKHR as_properties{};
  as_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
  VkPhysicalDeviceProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  tail = &properties.pNext;
  if (api >= VK_API_VERSION_1_1) link(reinterpret_cast<VkBaseOutStructure*>(&subgroup));
  if (api >= VK_API_VERSION_1_2) link(reinterpret_cast<VkBaseOutStructure*>(&indexing));
  if (ext_as) link(reinterpret_cast<VkBaseOutStructure*>(&as_properties));
  vkGetPhysicalDeviceProperties2(physical, &properties);

  const VkPhysicalDeviceLimits& limits = properties.properties.limits;
  out.max_push_constants_size = limits.maxPushConstantsSize;
  out.max_compute_workgroup_invocations = limits.maxComputeWorkGroupInvocations;
  out.max_compute_workgroup_size_x = limits.maxComputeWorkGroupSize[0];
  out.max_compute_workgroup_size_y = limits.maxComputeWorkGroupSize[1];
  out.max_compute_shared_memory_size = limits.maxComputeSharedMemorySize;
  out.max_memory_allocation_count = limits.maxMemoryAllocationCount;
  out.max_storage_buffer_range = limits.maxStorageBufferRange;
  out.subgroup_size = subgroup.subgroupSize;
  out.max_per_stage_uab_sampled_images = indexing.maxPerStageDescriptorUpdateAfterBindSampledImages;
  out.max_per_stage_uab_storage_images = indexing.maxPerStageDescriptorUpdateAfterBindStorageImages;
  out.max_per_stage_uab_samplers = indexing.maxPerStageDescriptorUpdateAfterBindSamplers;
  out.max_per_stage_uab_resources = indexing.maxPerStageUpdateAfterBindResources;
  out.max_set_uab_sampled_images = indexing.maxDescriptorSetUpdateAfterBindSampledImages;
  out.max_set_uab_storage_images = indexing.maxDescriptorSetUpdateAfterBindStorageImages;
  out.max_set_uab_samplers = indexing.maxDescriptorSetUpdateAfterBindSamplers;
  out.max_set_uab_acceleration_structures =
      ext_as ? as_properties.maxDescriptorSetUpdateAfterBindAccelerationStructures : 0u;

  // ---- queues ----
  u32 family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
  Vector<VkQueueFamilyProperties> families(family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
  for (const VkQueueFamilyProperties& family : families) {
    const bool graphics = (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
    const bool compute = (family.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
    if (graphics && compute) ++out.graphics_compute_families;
  }
}

}  // namespace engine::gfx

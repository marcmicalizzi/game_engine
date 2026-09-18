#include <core/base/assert.h>
#include <core/containers/vector.h>
#include <core/log/log.h>
#include <core/memory/memory.h>
#include <domain/gfx/device.h>
#include <domain/gfx/vulkan.h>

#include <cstdint>
#include <cstring>
#include <memory>

namespace engine::gfx {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_device, "gfx.device");
ENGINE_LOG_CATEGORY_DEFINE(log_validation, "gfx.validation");

constexpr const char* k_validation_layer = "VK_LAYER_KHRONOS_validation";

struct ExtensionSet {
  Vector<VkExtensionProperties> available;
  Vector<const char*> enabled;

  bool has(const char* name) const noexcept {
    for (const VkExtensionProperties& e : available) {
      if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
  }
  // Enables the extension when available; reports whether it did.
  bool enable_if_available(const char* name) {
    if (!has(name)) return false;
    enabled.push_back(name);
    return true;
  }
};

bool has_layer(const char* name) {
  u32 count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  Vector<VkLayerProperties> layers(count);
  vkEnumerateInstanceLayerProperties(&count, layers.data());
  for (const VkLayerProperties& l : layers) {
    if (std::strcmp(l.layerName, name) == 0) return true;
  }
  return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void*) {
  const std::string_view message = data->pMessage != nullptr ? data->pMessage : "";
  const std::string_view id = data->pMessageIdName != nullptr ? data->pMessageIdName : "";
  if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
    ENGINE_LOG_ERROR(log_validation, "validation error", log::field("id", id),
                     log::field("message", message));
  } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0 &&
             id != "Loader Message") {
    ENGINE_LOG_WARN(log_validation, "validation warning", log::field("id", id),
                    log::field("message", message));
  } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
    // Loader policy notes about third-party layers installed on the machine (overlays) are
    // not about this engine; keep them visible at debug only.
    ENGINE_LOG_DEBUG(log_validation, "loader message", log::field("message", message));
  } else {
    ENGINE_LOG_DEBUG(log_validation, "validation info", log::field("id", id),
                     log::field("message", message));
  }
  return VK_FALSE;
}

void set_error(std::string* error, const char* what, VkResult result) {
  if (error == nullptr) return;
  error->assign(what);
  error->append(": ");
  error->append(result_name(result));
}

}  // namespace

const char* result_name(VkResult result) noexcept {
  switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    default: return "VkResult(other)";
  }
}

// ---- Device ----------------------------------------------------------------------------------

struct Device::Impl {
  Handles handles;
  AdapterInfo adapter;
  DeviceFeatures features;
  u32 graphics_family = 0;
  u32 compute_family = 0;
  u32 transfer_family = 0;
};

Device::~Device() { destroy(); }

const AdapterInfo& Device::adapter() const noexcept {
  ENGINE_ASSERT(impl_ != nullptr, "Device::adapter: no device");
  return impl_->adapter;
}
const DeviceFeatures& Device::features() const noexcept {
  ENGINE_ASSERT(impl_ != nullptr, "Device::features: no device");
  return impl_->features;
}
u32 Device::graphics_family() const noexcept {
  return impl_ != nullptr ? impl_->graphics_family : 0;
}
u32 Device::compute_family() const noexcept { return impl_ != nullptr ? impl_->compute_family : 0; }
u32 Device::transfer_family() const noexcept {
  return impl_ != nullptr ? impl_->transfer_family : 0;
}
const Handles& Device::handles() const noexcept {
  ENGINE_ASSERT(impl_ != nullptr, "Device::handles: no device");
  return impl_->handles;
}

bool Device::memory_budget(MemoryBudget& out) const noexcept {
  if (impl_ == nullptr || impl_->handles.physical == VK_NULL_HANDLE) return false;
  VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
  budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
  VkPhysicalDeviceMemoryProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
  if (impl_->features.memory_budget) properties.pNext = &budget;
  vkGetPhysicalDeviceMemoryProperties2(impl_->handles.physical, &properties);

  out = MemoryBudget{};
  out.valid = impl_->features.memory_budget;
  const VkPhysicalDeviceMemoryProperties& mem = properties.memoryProperties;
  // Device-local heaps only: host memory is the system's and is reported elsewhere. On an
  // integrated GPU every heap is device-local and the totals are the shared pool, which is the
  // honest answer there.
  for (u32 i = 0; i < mem.memoryHeapCount; ++i) {
    if ((mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) continue;
    out.device_local_bytes += mem.memoryHeaps[i].size;
    if (out.valid) {
      out.budget_bytes += budget.heapBudget[i];
      out.used_bytes += budget.heapUsage[i];
    }
  }
  return true;
}

void Device::wait_idle() noexcept {
  if (impl_ != nullptr && impl_->handles.device != VK_NULL_HANDLE)
    vkDeviceWaitIdle(impl_->handles.device);
}

bool Device::create(const DeviceOptions& options, std::string* error) {
  ENGINE_VERIFY(impl_ == nullptr, "Device::create: already created");

  Vector<AdapterInfo> adapters;
  if (!enumerate_adapters(adapters, error)) return false;
  if (options.adapter_index >= adapters.size()) {
    if (error != nullptr) *error = "no adapter at index " + std::to_string(options.adapter_index);
    return false;
  }

  auto* impl = static_cast<Impl*>(mem::allocate(sizeof(Impl), alignof(Impl)));
  std::construct_at(impl);
  impl->adapter = adapters[options.adapter_index];
  Handles& h = impl->handles;
  auto fail = [&](const char* what, VkResult result) {
    set_error(error, what, result);
    impl_ = impl;
    destroy();
    return false;
  };

  // ---- instance ----
  Vector<const char*> layers;
  if (options.validation) {
    if (has_layer(k_validation_layer)) {
      layers.push_back(k_validation_layer);
      impl->features.validation = true;
    } else {
      ENGINE_LOG_WARN(log_device,
                      "validation requested but VK_LAYER_KHRONOS_validation is not installed");
    }
  }
  u32 instance_extension_count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &instance_extension_count, nullptr);
  ExtensionSet instance_extensions;
  instance_extensions.available.resize(instance_extension_count);
  vkEnumerateInstanceExtensionProperties(nullptr, &instance_extension_count,
                                         instance_extensions.available.data());
  const bool debug_utils =
      options.debug_messenger && instance_extensions.enable_if_available("VK_EXT_debug_utils");
  for (u32 i = 0; i < options.instance_extension_count; ++i) {
    const char* name = options.instance_extensions[i];
    if (!instance_extensions.enable_if_available(name)) {
      if (error != nullptr) *error = std::string("instance extension not available: ") + name;
      impl_ = impl;
      destroy();
      return false;
    }
  }

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "game_engine";
  app.pEngineName = "game_engine";
  app.apiVersion = VK_API_VERSION_1_3;
  VkDebugUtilsMessengerCreateInfoEXT messenger_info{};
  messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
  messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  messenger_info.pfnUserCallback = &debug_callback;
  VkInstanceCreateInfo instance_info{};
  instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instance_info.pApplicationInfo = &app;
  instance_info.enabledLayerCount = layers.size();
  instance_info.ppEnabledLayerNames = layers.data();
  instance_info.enabledExtensionCount = instance_extensions.enabled.size();
  instance_info.ppEnabledExtensionNames = instance_extensions.enabled.data();
  if (debug_utils) instance_info.pNext = &messenger_info;  // covers create/destroy too
  if (const VkResult r = vkCreateInstance(&instance_info, nullptr, &h.instance); r != VK_SUCCESS) {
    return fail("vkCreateInstance", r);
  }
  volkLoadInstanceOnly(h.instance);
  if (debug_utils) {
    vkCreateDebugUtilsMessengerEXT(h.instance, &messenger_info, nullptr, &h.messenger);
  }

  // ---- physical device: match the chosen adapter by ids and name ----
  u32 count = 0;
  vkEnumeratePhysicalDevices(h.instance, &count, nullptr);
  Vector<VkPhysicalDevice> physicals(count);
  vkEnumeratePhysicalDevices(h.instance, &count, physicals.data());
  for (VkPhysicalDevice p : physicals) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(p, &props);
    if (props.vendorID == impl->adapter.vendor_id && props.deviceID == impl->adapter.device_id &&
        impl->adapter.name == props.deviceName) {
      h.physical = p;
      break;
    }
  }
  if (h.physical == VK_NULL_HANDLE)
    return fail("adapter disappeared between enumeration and creation",
                VK_ERROR_INITIALIZATION_FAILED);

  // ---- queues ----
  u32 family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(h.physical, &family_count, nullptr);
  Vector<VkQueueFamilyProperties> families(family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(h.physical, &family_count, families.data());
  u32 graphics = UINT32_MAX;
  u32 compute = UINT32_MAX;
  u32 transfer = UINT32_MAX;
  for (u32 i = 0; i < family_count; ++i) {
    const VkQueueFlags flags = families[i].queueFlags;
    const bool g = (flags & VK_QUEUE_GRAPHICS_BIT) != 0;
    const bool c = (flags & VK_QUEUE_COMPUTE_BIT) != 0;
    const bool t = (flags & VK_QUEUE_TRANSFER_BIT) != 0;
    if (g && c && graphics == UINT32_MAX) graphics = i;
    if (c && !g && compute == UINT32_MAX) compute = i;
    if (t && !g && !c && transfer == UINT32_MAX) transfer = i;
  }
  if (graphics == UINT32_MAX)
    return fail("no graphics+compute queue family", VK_ERROR_INITIALIZATION_FAILED);
  if (compute == UINT32_MAX) compute = graphics;
  if (transfer == UINT32_MAX) transfer = compute;
  impl->graphics_family = graphics;
  impl->compute_family = compute;
  impl->transfer_family = transfer;

  const float priority = 1.0f;
  Vector<VkDeviceQueueCreateInfo> queue_infos;
  for (const u32 family : {graphics, compute, transfer}) {
    bool seen = false;
    for (const VkDeviceQueueCreateInfo& q : queue_infos)
      seen = seen || q.queueFamilyIndex == family;
    if (seen) continue;
    VkDeviceQueueCreateInfo q{};
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = family;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    queue_infos.push_back(q);
  }

  // ---- extensions ----
  ExtensionSet device_extensions;
  u32 device_extension_count = 0;
  vkEnumerateDeviceExtensionProperties(h.physical, nullptr, &device_extension_count, nullptr);
  device_extensions.available.resize(device_extension_count);
  vkEnumerateDeviceExtensionProperties(h.physical, nullptr, &device_extension_count,
                                       device_extensions.available.data());
  impl->features.presentation = device_extensions.enable_if_available("VK_KHR_swapchain");
  const bool ext_mesh = device_extensions.enable_if_available("VK_EXT_mesh_shader");
  const bool ext_deferred =
      device_extensions.enable_if_available("VK_KHR_deferred_host_operations");
  const bool ext_as =
      ext_deferred && device_extensions.enable_if_available("VK_KHR_acceleration_structure");
  const bool ext_rt =
      ext_as && device_extensions.enable_if_available("VK_KHR_ray_tracing_pipeline");
  const bool ext_rq = ext_as && device_extensions.enable_if_available("VK_KHR_ray_query");
  if (ext_as) {
    device_extensions.enable_if_available("VK_KHR_ray_tracing_maintenance1");
    device_extensions.enable_if_available("VK_KHR_ray_tracing_position_fetch");
  }
  const bool ext_cluster =
      ext_as && device_extensions.enable_if_available("VK_NV_cluster_acceleration_structure");
  const bool ext_descriptor_buffer =
      device_extensions.enable_if_available("VK_EXT_descriptor_buffer");
  const bool ext_decompression =
      device_extensions.enable_if_available("VK_EXT_memory_decompression");
  // No feature struct and no device functions of its own: enabling it is what makes
  // vkGetPhysicalDeviceMemoryProperties2 fill VkPhysicalDeviceMemoryBudgetPropertiesEXT.
  impl->features.memory_budget = device_extensions.enable_if_available("VK_EXT_memory_budget");

  // ---- features: query, then enable what is supported ----
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

  // Chain: only structs whose extension is enabled may be queried or passed.
  VkPhysicalDeviceFeatures2 f2{};
  f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  // Every feature struct starts with sType and pNext, so a VkBaseOutStructure view links them.
  void** tail = &f2.pNext;
  auto link = [&](VkBaseOutStructure* node) {
    *tail = node;
    tail = reinterpret_cast<void**>(&node->pNext);
  };
  link(reinterpret_cast<VkBaseOutStructure*>(&v11));
  link(reinterpret_cast<VkBaseOutStructure*>(&v12));
  link(reinterpret_cast<VkBaseOutStructure*>(&v13));
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
  vkGetPhysicalDeviceFeatures2(h.physical, &f2);

  // Required core features.
  const bool required = v13.dynamicRendering && v13.synchronization2 && v13.maintenance4 &&
                        v12.bufferDeviceAddress && v12.descriptorIndexing &&
                        v12.runtimeDescriptorArray && v12.descriptorBindingPartiallyBound &&
                        v12.timelineSemaphore && v12.scalarBlockLayout && v12.hostQueryReset &&
                        v12.drawIndirectCount && f2.features.multiDrawIndirect;
  if (!required)
    return fail("a required Vulkan 1.2/1.3 feature is missing", VK_ERROR_FEATURE_NOT_PRESENT);

  // Enable exactly what the renderer uses: rebuild the structs with the supported subset.
  VkPhysicalDeviceFeatures base{};
  base.multiDrawIndirect = VK_TRUE;
  base.samplerAnisotropy = f2.features.samplerAnisotropy;
  base.shaderInt64 = f2.features.shaderInt64;
  base.fillModeNonSolid = f2.features.fillModeNonSolid;
  base.shaderInt16 = f2.features.shaderInt16;
  base.fragmentStoresAndAtomics = f2.features.fragmentStoresAndAtomics;
  base.vertexPipelineStoresAndAtomics = f2.features.vertexPipelineStoresAndAtomics;
  base.shaderStorageImageWriteWithoutFormat = f2.features.shaderStorageImageWriteWithoutFormat;
  base.shaderStorageImageReadWithoutFormat = f2.features.shaderStorageImageReadWithoutFormat;
  impl->features.sampler_anisotropy = f2.features.samplerAnisotropy == VK_TRUE;
  impl->features.shader_int64 = f2.features.shaderInt64 == VK_TRUE;

  VkPhysicalDeviceVulkan11Features e11{};
  e11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
  e11.shaderDrawParameters = v11.shaderDrawParameters;
  e11.storageBuffer16BitAccess = v11.storageBuffer16BitAccess;
  VkPhysicalDeviceVulkan12Features e12{};
  e12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  e12.bufferDeviceAddress = VK_TRUE;
  e12.descriptorIndexing = VK_TRUE;
  e12.runtimeDescriptorArray = VK_TRUE;
  e12.descriptorBindingPartiallyBound = VK_TRUE;
  e12.descriptorBindingVariableDescriptorCount = v12.descriptorBindingVariableDescriptorCount;
  e12.descriptorBindingSampledImageUpdateAfterBind =
      v12.descriptorBindingSampledImageUpdateAfterBind;
  e12.descriptorBindingStorageBufferUpdateAfterBind =
      v12.descriptorBindingStorageBufferUpdateAfterBind;
  e12.descriptorBindingStorageImageUpdateAfterBind =
      v12.descriptorBindingStorageImageUpdateAfterBind;
  e12.shaderSampledImageArrayNonUniformIndexing = v12.shaderSampledImageArrayNonUniformIndexing;
  e12.shaderStorageBufferArrayNonUniformIndexing = v12.shaderStorageBufferArrayNonUniformIndexing;
  e12.timelineSemaphore = VK_TRUE;
  e12.scalarBlockLayout = VK_TRUE;
  e12.hostQueryReset = VK_TRUE;
  e12.drawIndirectCount = VK_TRUE;
  e12.shaderInt8 = v12.shaderInt8;
  e12.storageBuffer8BitAccess = v12.storageBuffer8BitAccess;
  e12.shaderFloat16 = v12.shaderFloat16;
  e12.samplerFilterMinmax = v12.samplerFilterMinmax;
  e12.shaderBufferInt64Atomics = v12.shaderBufferInt64Atomics;
  impl->features.buffer_int64_atomics = v12.shaderBufferInt64Atomics == VK_TRUE;
  VkPhysicalDeviceVulkan13Features e13{};
  e13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  e13.dynamicRendering = VK_TRUE;
  e13.synchronization2 = VK_TRUE;
  e13.maintenance4 = VK_TRUE;
  e13.shaderDemoteToHelperInvocation = v13.shaderDemoteToHelperInvocation;
  e13.subgroupSizeControl = v13.subgroupSizeControl;
  e13.computeFullSubgroups = v13.computeFullSubgroups;

  VkPhysicalDeviceFeatures2 enable{};
  enable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  enable.features = base;
  tail = &enable.pNext;
  link(reinterpret_cast<VkBaseOutStructure*>(&e11));
  link(reinterpret_cast<VkBaseOutStructure*>(&e12));
  link(reinterpret_cast<VkBaseOutStructure*>(&e13));
  if (ext_mesh && mesh.meshShader) {
    mesh.pNext = nullptr;
    mesh.multiviewMeshShader = VK_FALSE;
    mesh.primitiveFragmentShadingRateMeshShader = VK_FALSE;
    mesh.meshShaderQueries = VK_FALSE;
    link(reinterpret_cast<VkBaseOutStructure*>(&mesh));
    impl->features.mesh_shader = true;
  }
  if (ext_as && as.accelerationStructure) {
    as.pNext = nullptr;
    as.accelerationStructureCaptureReplay = VK_FALSE;
    as.accelerationStructureIndirectBuild = VK_FALSE;
    as.accelerationStructureHostCommands = VK_FALSE;
    link(reinterpret_cast<VkBaseOutStructure*>(&as));
    impl->features.acceleration_structure = true;
  }
  if (impl->features.acceleration_structure && ext_rt && rt.rayTracingPipeline) {
    rt.pNext = nullptr;
    rt.rayTracingPipelineShaderGroupHandleCaptureReplay = VK_FALSE;
    rt.rayTracingPipelineShaderGroupHandleCaptureReplayMixed = VK_FALSE;
    link(reinterpret_cast<VkBaseOutStructure*>(&rt));
    impl->features.ray_tracing_pipeline = true;
  }
  if (impl->features.acceleration_structure && ext_rq && rq.rayQuery) {
    rq.pNext = nullptr;
    link(reinterpret_cast<VkBaseOutStructure*>(&rq));
    impl->features.ray_query = true;
  }
  if (ext_descriptor_buffer && db.descriptorBuffer) {
    db.pNext = nullptr;
    db.descriptorBufferCaptureReplay = VK_FALSE;
    db.descriptorBufferImageLayoutIgnored = VK_FALSE;
    db.descriptorBufferPushDescriptors = VK_FALSE;
    link(reinterpret_cast<VkBaseOutStructure*>(&db));
    impl->features.descriptor_buffer = true;
  }
#if defined(VK_NV_cluster_acceleration_structure)
  if (impl->features.acceleration_structure && ext_cluster &&
      cluster.clusterAccelerationStructure) {
    cluster.pNext = nullptr;
    link(reinterpret_cast<VkBaseOutStructure*>(&cluster));
    impl->features.cluster_acceleration_structure = true;
  }
#endif
#if defined(VK_EXT_memory_decompression)
  if (ext_decompression && decompression.memoryDecompression) {
    decompression.pNext = nullptr;
    link(reinterpret_cast<VkBaseOutStructure*>(&decompression));
    impl->features.memory_decompression = true;
  }
#endif
  *tail = nullptr;

  VkDeviceCreateInfo device_info{};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.pNext = &enable;
  device_info.queueCreateInfoCount = queue_infos.size();
  device_info.pQueueCreateInfos = queue_infos.data();
  device_info.enabledExtensionCount = device_extensions.enabled.size();
  device_info.ppEnabledExtensionNames = device_extensions.enabled.data();
  if (const VkResult r = vkCreateDevice(h.physical, &device_info, nullptr, &h.device);
      r != VK_SUCCESS) {
    return fail("vkCreateDevice", r);
  }
  volkLoadDevice(h.device);
  vkGetDeviceQueue(h.device, graphics, 0, &h.graphics_queue);
  vkGetDeviceQueue(h.device, compute, 0, &h.compute_queue);
  vkGetDeviceQueue(h.device, transfer, 0, &h.transfer_queue);

  // ---- allocator ----
  VmaVulkanFunctions functions{};
  functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
  functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
  VmaAllocatorCreateInfo allocator_info{};
  allocator_info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
  allocator_info.physicalDevice = h.physical;
  allocator_info.device = h.device;
  allocator_info.instance = h.instance;
  allocator_info.vulkanApiVersion = VK_API_VERSION_1_3;
  allocator_info.pVulkanFunctions = &functions;
  if (const VkResult r = vmaCreateAllocator(&allocator_info, &h.allocator); r != VK_SUCCESS) {
    return fail("vmaCreateAllocator", r);
  }

  // ---- immediate command pool ----
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags =
      VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = graphics;
  if (const VkResult r = vkCreateCommandPool(h.device, &pool_info, nullptr, &h.immediate_pool);
      r != VK_SUCCESS) {
    return fail("vkCreateCommandPool", r);
  }

  impl_ = impl;
  ENGINE_LOG_INFO(log_device, "device created",
                  log::field("adapter", std::string_view(impl->adapter.name)),
                  log::field("api", std::string_view(impl->adapter.api_version)),
                  log::field("mesh_shader", impl->features.mesh_shader),
                  log::field("ray_tracing", impl->features.ray_tracing_pipeline),
                  log::field("cluster_as", impl->features.cluster_acceleration_structure),
                  log::field("descriptor_buffer", impl->features.descriptor_buffer),
                  log::field("validation", impl->features.validation));
  return true;
}

void Device::destroy() noexcept {
  if (impl_ == nullptr) return;
  Handles& h = impl_->handles;
  if (h.device != VK_NULL_HANDLE) vkDeviceWaitIdle(h.device);
  if (h.immediate_pool != VK_NULL_HANDLE) vkDestroyCommandPool(h.device, h.immediate_pool, nullptr);
  if (h.allocator != nullptr) vmaDestroyAllocator(h.allocator);
  if (h.device != VK_NULL_HANDLE) vkDestroyDevice(h.device, nullptr);
  if (h.messenger != VK_NULL_HANDLE)
    vkDestroyDebugUtilsMessengerEXT(h.instance, h.messenger, nullptr);
  if (h.instance != VK_NULL_HANDLE) vkDestroyInstance(h.instance, nullptr);
  std::destroy_at(impl_);
  mem::deallocate(impl_, sizeof(Impl), alignof(Impl));
  impl_ = nullptr;
}

// ---- resources -------------------------------------------------------------------------------

bool create_buffer(const Device& device, u64 size, VkBufferUsageFlags usage, bool host_visible,
                   BufferResource& out, std::string* error) {
  const Handles& h = device.handles();
  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = size;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  if (host_visible) {
    alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    alloc.requiredFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  }
  VmaAllocationInfo result_info{};
  const VkResult r =
      vmaCreateBuffer(h.allocator, &info, &alloc, &out.buffer, &out.allocation, &result_info);
  if (r != VK_SUCCESS) {
    set_error(error, "vmaCreateBuffer", r);
    return false;
  }
  out.size = size;
  out.mapped = result_info.pMappedData;
  out.address = 0;
  if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0) {
    VkBufferDeviceAddressInfo address_info{};
    address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address_info.buffer = out.buffer;
    out.address = vkGetBufferDeviceAddress(h.device, &address_info);
  }
  return true;
}

void destroy_buffer(const Device& device, BufferResource& buffer) noexcept {
  if (buffer.buffer == VK_NULL_HANDLE) return;
  vmaDestroyBuffer(device.handles().allocator, buffer.buffer, buffer.allocation);
  buffer = BufferResource{};
}

bool create_image_2d(const Device& device, u32 width, u32 height, VkFormat format,
                     VkImageUsageFlags usage, ImageResource& out, std::string* error) {
  const Handles& h = device.handles();
  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  const VkResult r =
      vmaCreateImage(h.allocator, &info, &alloc, &out.image, &out.allocation, nullptr);
  if (r != VK_SUCCESS) {
    set_error(error, "vmaCreateImage", r);
    return false;
  }
  out.format = format;
  out.width = width;
  out.height = height;
  return true;
}

void destroy_image(const Device& device, ImageResource& image) noexcept {
  if (image.image == VK_NULL_HANDLE) return;
  vmaDestroyImage(device.handles().allocator, image.image, image.allocation);
  image = ImageResource{};
}

// ---- commands --------------------------------------------------------------------------------

bool submit_immediate(const Device& device, RecordFn record, void* context, std::string* error) {
  const Handles& h = device.handles();
  VkCommandBufferAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc.commandPool = h.immediate_pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer commands = VK_NULL_HANDLE;
  if (const VkResult r = vkAllocateCommandBuffers(h.device, &alloc, &commands); r != VK_SUCCESS) {
    set_error(error, "vkAllocateCommandBuffers", r);
    return false;
  }
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(commands, &begin);
  record(commands, context);
  vkEndCommandBuffer(commands);

  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(h.device, &fence_info, nullptr, &fence);
  VkCommandBufferSubmitInfo command_info{};
  command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  command_info.commandBuffer = commands;
  VkSubmitInfo2 submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &command_info;
  VkResult r = vkQueueSubmit2(h.graphics_queue, 1, &submit, fence);
  if (r == VK_SUCCESS) r = vkWaitForFences(h.device, 1, &fence, VK_TRUE, UINT64_MAX);
  vkDestroyFence(h.device, fence, nullptr);
  vkFreeCommandBuffers(h.device, h.immediate_pool, 1, &commands);
  if (r != VK_SUCCESS) {
    set_error(error, "submit_immediate", r);
    return false;
  }
  return true;
}

void image_barrier(VkCommandBuffer commands, VkImage image, VkImageLayout old_layout,
                   VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                   VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                   VkAccessFlags2 dst_access) noexcept {
  VkImageMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  barrier.srcStageMask = src_stage;
  barrier.srcAccessMask = src_access;
  barrier.dstStageMask = dst_stage;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo dependency{};
  dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(commands, &dependency);
}

}  // namespace engine::gfx

// ---- shaders and pipelines -------------------------------------------------------------------

namespace engine::gfx {

VkShaderModule create_shader_module(const Device& device, const unsigned char* spirv, usize bytes,
                                    std::string* error) {
  if (bytes == 0 || (bytes % 4) != 0 || (reinterpret_cast<std::uintptr_t>(spirv) % 4) != 0) {
    if (error != nullptr) *error = "SPIR-V must be a non-empty, 4-byte-aligned multiple of 4 bytes";
    return VK_NULL_HANDLE;
  }
  VkShaderModuleCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = bytes;
  info.pCode = reinterpret_cast<const u32*>(spirv);
  VkShaderModule module = VK_NULL_HANDLE;
  const VkResult r = vkCreateShaderModule(device.handles().device, &info, nullptr, &module);
  if (r != VK_SUCCESS) {
    set_error(error, "vkCreateShaderModule", r);
    return VK_NULL_HANDLE;
  }
  return module;
}

void destroy_shader_module(const Device& device, VkShaderModule module) noexcept {
  if (module != VK_NULL_HANDLE) vkDestroyShaderModule(device.handles().device, module, nullptr);
}

bool create_compute_pipeline(const Device& device, VkShaderModule module, const char* entry,
                             std::span<const VkDescriptorSetLayout> set_layouts,
                             u32 push_constant_bytes, ComputePipeline& out, std::string* error) {
  const Handles& h = device.handles();
  VkPushConstantRange range{};
  range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  range.offset = 0;
  range.size = push_constant_bytes;
  VkPipelineLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.setLayoutCount = static_cast<u32>(set_layouts.size());
  layout_info.pSetLayouts = set_layouts.data();
  layout_info.pushConstantRangeCount = push_constant_bytes > 0 ? 1 : 0;
  layout_info.pPushConstantRanges = push_constant_bytes > 0 ? &range : nullptr;
  if (const VkResult r = vkCreatePipelineLayout(h.device, &layout_info, nullptr, &out.layout);
      r != VK_SUCCESS) {
    set_error(error, "vkCreatePipelineLayout", r);
    return false;
  }
  VkComputePipelineCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  info.stage.module = module;
  info.stage.pName = entry;
  info.layout = out.layout;
  if (const VkResult r =
          vkCreateComputePipelines(h.device, VK_NULL_HANDLE, 1, &info, nullptr, &out.pipeline);
      r != VK_SUCCESS) {
    set_error(error, "vkCreateComputePipelines", r);
    vkDestroyPipelineLayout(h.device, out.layout, nullptr);
    out.layout = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

void destroy_compute_pipeline(const Device& device, ComputePipeline& pipeline) noexcept {
  const Handles& h = device.handles();
  if (pipeline.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(h.device, pipeline.pipeline, nullptr);
  if (pipeline.layout != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(h.device, pipeline.layout, nullptr);
  pipeline = ComputePipeline{};
}

}  // namespace engine::gfx

// ---- graphics pipelines ----------------------------------------------------------------------

namespace engine::gfx {

bool create_graphics_pipeline(const Device& device, const GraphicsPipelineDesc& desc,
                              VkPipeline& out, std::string* error) {
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = desc.vertex;
  stages[0].pName = desc.vertex_entry;
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = desc.fragment;
  stages[1].pName = desc.fragment_entry;

  VkPipelineVertexInputStateCreateInfo vertex_input{};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo assembly{};
  assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = desc.cull;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth{};
  depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth.depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE;
  depth.depthWriteEnable = desc.depth_write ? VK_TRUE : VK_FALSE;
  depth.depthCompareOp = desc.depth_compare;
  VkPipelineColorBlendAttachmentState blend_attachment{};
  blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = desc.color_format != VK_FORMAT_UNDEFINED ? 1 : 0;
  blend.pAttachments = &blend_attachment;
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  VkPipelineRenderingCreateInfo rendering{};
  rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering.colorAttachmentCount = blend.attachmentCount;
  rendering.pColorAttachmentFormats = &desc.color_format;
  rendering.depthAttachmentFormat = desc.depth_format;

  VkGraphicsPipelineCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.pNext = &rendering;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = desc.layout;
  const VkResult r =
      vkCreateGraphicsPipelines(device.handles().device, VK_NULL_HANDLE, 1, &info, nullptr, &out);
  if (r != VK_SUCCESS) {
    set_error(error, "vkCreateGraphicsPipelines", r);
    out = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

void destroy_pipeline(const Device& device, VkPipeline pipeline) noexcept {
  if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device.handles().device, pipeline, nullptr);
}

}  // namespace engine::gfx

// ---- mesh pipelines --------------------------------------------------------------------------

namespace engine::gfx {

bool create_mesh_pipeline(const Device& device, const MeshPipelineDesc& desc, VkPipeline& out,
                          std::string* error) {
  if (!device.features().mesh_shader) {
    if (error != nullptr) *error = "create_mesh_pipeline: the device has no mesh shader support";
    out = VK_NULL_HANDLE;
    return false;
  }
  VkPipelineShaderStageCreateInfo stages[3]{};
  u32 stage_count = 0;
  if (desc.task != VK_NULL_HANDLE) {
    stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stage_count].stage = VK_SHADER_STAGE_TASK_BIT_EXT;
    stages[stage_count].module = desc.task;
    stages[stage_count].pName = desc.task_entry;
    ++stage_count;
  }
  stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[stage_count].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
  stages[stage_count].module = desc.mesh;
  stages[stage_count].pName = desc.mesh_entry;
  ++stage_count;
  stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[stage_count].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[stage_count].module = desc.fragment;
  stages[stage_count].pName = desc.fragment_entry;
  ++stage_count;

  VkPipelineViewportStateCreateInfo viewport{};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = desc.cull;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth{};
  depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth.depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE;
  depth.depthWriteEnable = desc.depth_write ? VK_TRUE : VK_FALSE;
  depth.depthCompareOp = desc.depth_compare;
  VkPipelineColorBlendAttachmentState blend_attachment{};
  blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = desc.color_format != VK_FORMAT_UNDEFINED ? 1 : 0;
  blend.pAttachments = &blend_attachment;
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  VkPipelineRenderingCreateInfo rendering{};
  rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering.colorAttachmentCount = blend.attachmentCount;
  rendering.pColorAttachmentFormats = &desc.color_format;
  rendering.depthAttachmentFormat = desc.depth_format;

  VkGraphicsPipelineCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.pNext = &rendering;
  info.stageCount = stage_count;
  info.pStages = stages;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = desc.layout;
  const VkResult r =
      vkCreateGraphicsPipelines(device.handles().device, VK_NULL_HANDLE, 1, &info, nullptr, &out);
  if (r != VK_SUCCESS) {
    set_error(error, "vkCreateGraphicsPipelines(mesh)", r);
    out = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

}  // namespace engine::gfx

// ---- uploads ---------------------------------------------------------------------------------

namespace engine::gfx {

bool upload_buffer(const Device& device, const void* data, u64 bytes, VkBufferUsageFlags usage,
                   BufferResource& out, std::string* error) {
  out = BufferResource{};
  if (data == nullptr || bytes == 0) {
    if (error != nullptr) *error = "upload_buffer: nothing to upload";
    return false;
  }
  if (!create_buffer(
          device, bytes,
          usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
          false, out, error)) {
    return false;
  }
  BufferResource staging;
  if (!create_buffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging, error)) {
    destroy_buffer(device, out);
    return false;
  }
  std::memcpy(staging.mapped, data, static_cast<usize>(bytes));
  const bool ok = submit_immediate(
      device,
      [&](VkCommandBuffer commands) {
        const VkBufferCopy copy{0, 0, bytes};
        vkCmdCopyBuffer(commands, staging.buffer, out.buffer, 1, &copy);
      },
      error);
  destroy_buffer(device, staging);
  if (!ok) destroy_buffer(device, out);
  return ok;
}

}  // namespace engine::gfx

namespace engine::gfx {

bool upload_image_2d(const Device& device, u32 width, u32 height, VkFormat format,
                     const void* pixels, u64 bytes, ImageResource& out, std::string* error) {
  out = ImageResource{};
  if (pixels == nullptr || bytes == 0 || width == 0 || height == 0) {
    if (error != nullptr) *error = "upload_image_2d: nothing to upload";
    return false;
  }
  if (!create_image_2d(device, width, height, format,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, out, error)) {
    return false;
  }
  BufferResource staging;
  if (!create_buffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging, error)) {
    destroy_image(device, out);
    return false;
  }
  std::memcpy(staging.mapped, pixels, static_cast<usize>(bytes));
  const bool ok = submit_immediate(
      device,
      [&](VkCommandBuffer commands) {
        image_barrier(commands, out.image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
                      VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(commands, staging.buffer, out.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        image_barrier(commands, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT);
      },
      error);
  destroy_buffer(device, staging);
  if (!ok) destroy_image(device, out);
  return ok;
}

}  // namespace engine::gfx

#include <core/log/log.h>
#include <domain/gfx/adapter.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <volk.h>

namespace engine::gfx {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_gfx, "gfx");

constexpr const char* k_extensions[] = {
    "VK_KHR_acceleration_structure",
    "VK_KHR_ray_tracing_pipeline",
    "VK_KHR_ray_query",
    "VK_KHR_ray_tracing_position_fetch",
    "VK_KHR_ray_tracing_maintenance1",
    "VK_EXT_mesh_shader",
    "VK_NV_cluster_acceleration_structure",
    "VK_NV_partitioned_acceleration_structure",
    "VK_EXT_memory_decompression",
    "VK_NV_memory_decompression",
    "VK_EXT_descriptor_buffer",
    "VK_KHR_fragment_shading_rate",
    "VK_EXT_fragment_density_map",
    "VK_KHR_swapchain",
    "VK_EXT_shader_object",
    "VK_NV_cooperative_matrix",
};

const char* result_name(VkResult r) noexcept {
  switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    default: return "VkResult";
  }
}

std::string version_text(u32 v) {
  return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) +
         "." + std::to_string(VK_API_VERSION_PATCH(v));
}

std::string vendor_text(u32 id) {
  switch (id) {
    case 0x10DE: return "NVIDIA";
    case 0x1002: return "AMD";
    case 0x8086: return "Intel";
    case 0x13B5: return "ARM";
    case 0x5143: return "Qualcomm";
    case 0x106B: return "Apple";
    case 0x10005: return "Mesa";
    default: {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "0x%04X", id);
      return buf;
    }
  }
}

AdapterType adapter_type(VkPhysicalDeviceType t) noexcept {
  switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return AdapterType::IntegratedGpu;
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return AdapterType::DiscreteGpu;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return AdapterType::VirtualGpu;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return AdapterType::Cpu;
    default: return AdapterType::Other;
  }
}

// volkInitialize loads the loader library once per process.
bool loader_ready(std::string* error) {
  static std::once_flag once;
  static VkResult result = VK_ERROR_INITIALIZATION_FAILED;
  std::call_once(once, [] { result = volkInitialize(); });
  if (result != VK_SUCCESS) {
    if (error != nullptr) {
      *error = "no Vulkan loader library (vulkan-1.dll or libvulkan.so.1); install a GPU driver";
    }
    return false;
  }
  const u32 version = volkGetInstanceVersion();
  if (version < VK_API_VERSION_1_3) {
    if (error != nullptr)
      *error = "Vulkan loader supports only " + version_text(version) + "; 1.3 is required";
    return false;
  }
  return true;
}

void fill_adapter(VkPhysicalDevice device, AdapterInfo& info) {
  VkPhysicalDeviceDriverProperties driver{};
  driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
  VkPhysicalDeviceProperties2 props{};
  props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props.pNext = &driver;
  vkGetPhysicalDeviceProperties2(device, &props);
  info.name = props.properties.deviceName;
  info.vendor_id = props.properties.vendorID;
  info.vendor = vendor_text(props.properties.vendorID);
  info.device_id = props.properties.deviceID;
  info.type = adapter_type(props.properties.deviceType);
  info.api_version = version_text(props.properties.apiVersion);
  info.driver_name = driver.driverName;
  info.driver_info = driver.driverInfo;

  VkPhysicalDeviceMemoryProperties memory{};
  vkGetPhysicalDeviceMemoryProperties(device, &memory);
  info.device_local_bytes = 0;
  for (u32 i = 0; i < memory.memoryHeapCount; ++i) {
    if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
      info.device_local_bytes += memory.memoryHeaps[i].size;
    }
  }

  u32 extension_count = 0;
  vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr);
  Vector<VkExtensionProperties> extensions(extension_count);
  vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, extensions.data());
  for (const char* wanted : k_extensions) {
    bool present = false;
    for (const VkExtensionProperties& e : extensions) {
      if (std::strcmp(e.extensionName, wanted) == 0) {
        present = true;
        break;
      }
    }
    info.extensions.insert_or_assign(std::string(wanted), present);
  }

  u32 family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
  Vector<VkQueueFamilyProperties> families(family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());
  info.queue_families.clear();
  for (u32 i = 0; i < family_count; ++i) {
    QueueFamilyInfo q;
    q.index = i;
    q.count = families[i].queueCount;
    q.graphics = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
    q.compute = (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
    q.transfer = (families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;
    info.queue_families.push_back(q);
  }

  auto has = [&](const char* name) {
    const bool* v = info.extensions.find_value(std::string_view(name));
    return v != nullptr && *v;
  };
  const bool rt = has("VK_KHR_acceleration_structure") && has("VK_KHR_ray_tracing_pipeline");
  info.tier = rt ? (has("VK_NV_cluster_acceleration_structure") ? "rt-cluster" : "rt") : "raster";
}

}  // namespace

std::span<const char* const> extensions_of_interest() noexcept {
  return {k_extensions, sizeof(k_extensions) / sizeof(k_extensions[0])};
}

bool vulkan_available(std::string* error) { return loader_ready(error); }

bool enumerate_adapters(Vector<AdapterInfo>& out, std::string* error) {
  out.clear();
  if (!loader_ready(error)) return false;

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "game_engine";
  app.pEngineName = "game_engine";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo create{};
  create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  const VkResult created = vkCreateInstance(&create, nullptr, &instance);
  if (created != VK_SUCCESS) {
    if (error != nullptr) {
      *error = created == VK_ERROR_INCOMPATIBLE_DRIVER
                   ? std::string("no Vulkan 1.3 driver (ICD) is installed behind the loader")
                   : std::string("vkCreateInstance failed: ") + result_name(created);
    }
    return false;
  }
  volkLoadInstanceOnly(instance);

  u32 count = 0;
  VkResult r = vkEnumeratePhysicalDevices(instance, &count, nullptr);
  Vector<VkPhysicalDevice> devices(count);
  if (r == VK_SUCCESS && count > 0)
    r = vkEnumeratePhysicalDevices(instance, &count, devices.data());
  if (r != VK_SUCCESS) {
    if (error != nullptr)
      *error = std::string("vkEnumeratePhysicalDevices failed: ") + result_name(r);
    vkDestroyInstance(instance, nullptr);
    return false;
  }
  for (u32 i = 0; i < count; ++i) {
    AdapterInfo info;
    fill_adapter(devices[i], info);
    out.push_back(std::move(info));
  }
  vkDestroyInstance(instance, nullptr);

  std::sort(out.begin(), out.end(), [](const AdapterInfo& a, const AdapterInfo& b) {
    const bool da = a.type == AdapterType::DiscreteGpu;
    const bool db = b.type == AdapterType::DiscreteGpu;
    if (da != db) return da;
    return a.device_local_bytes > b.device_local_bytes;
  });
  ENGINE_LOG_INFO(
      log_gfx, "enumerated adapters", log::field("count", static_cast<u64>(out.size())),
      log::field("first", out.empty() ? std::string_view{} : std::string_view(out[0].name)),
      log::field("tier", out.empty() ? std::string_view{} : std::string_view(out[0].tier)));
  return true;
}

std::string describe_adapters(std::span<const AdapterInfo> adapters) {
  std::string text;
  for (const AdapterInfo& a : adapters) {
    text.append(a.name);
    text.append(" (");
    text.append(a.vendor);
    text.append(", Vulkan ");
    text.append(a.api_version);
    text.append(", driver ");
    text.append(a.driver_name);
    text.push_back(' ');
    text.append(a.driver_info);
    text.append(", ");
    text.append(std::to_string(a.device_local_bytes >> 20));
    text.append(" MB device-local, tier ");
    text.append(a.tier);
    text.append(")\n");
  }
  return text;
}

}  // namespace engine::gfx

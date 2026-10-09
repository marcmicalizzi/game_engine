// The display probe (display_probe.h; E39's first measurement). Windows only: DXGI for the outputs
// and their luminances, the display configuration for the SDR white level, and a hidden Win32
// window per output for the surface Vulkan is asked about.
#include "volk_instance.h"

#include <core/log/log.h>
#include <domain/gfx/adapter.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/display.h>
#include <domain/gfx/display_probe.h>
#include <domain/gfx/rhi.h>

// The platform's headers last: windows.h defines `near` and `far` (and more) as macros, which the
// engine's headers above use as names.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: dxgi1_6.h and vulkan_win32.h need windows.h first
#include <windows.h>
#include <dxgi1_6.h>
#include <vulkan/vulkan_win32.h>
// clang-format on
#endif

#include <cstdio>
#include <cstring>

namespace engine::gfx {

const DisplayOutput* output_at(std::span<const DisplayOutput> outputs, i32 x, i32 y) noexcept {
  const DisplayOutput* origin = nullptr;
  for (const DisplayOutput& o : outputs) {
    if (o.desktop.size() != 4) continue;
    if (x >= o.desktop[0] && x < o.desktop[2] && y >= o.desktop[1] && y < o.desktop[3]) return &o;
    if (o.desktop[0] == 0 && o.desktop[1] == 0 && origin == nullptr) origin = &o;
  }
  if (origin != nullptr) return origin;
  return outputs.empty() ? nullptr : &outputs[0];
}

#if defined(_WIN32)

ENGINE_LOG_CATEGORY_DEFINE(log_display_probe, "gfx.display_probe");

namespace {

std::string narrow(const wchar_t* wide) {
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (bytes <= 1) return {};
  std::string out(static_cast<usize>(bytes - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), bytes, nullptr, nullptr);
  return out;
}

const char* dxgi_color_space_name(DXGI_COLOR_SPACE_TYPE space) {
  switch (space) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: return "rgb_full_g22_none_p709";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: return "rgb_full_g10_none_p709";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return "rgb_full_g2084_none_p2020";
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020: return "rgb_full_g22_none_p2020";
    default: return nullptr;
  }
}

// Windows' SDR white level for the output named `gdi_name` ("SDR content brightness"), nits: the
// display configuration reports it in thousandths of 80 nits, per target. A Surround group is one
// source with a target per panel; the first target's level is the group's (Windows sets one).
f32 sdr_white_nits(const wchar_t* gdi_name) {
  UINT32 path_count = 0;
  UINT32 mode_count = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) !=
      ERROR_SUCCESS) {
    return 0.0f;
  }
  Vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  Vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                         modes.data(), nullptr) != ERROR_SUCCESS) {
    return 0.0f;
  }
  for (u32 i = 0; i < path_count; ++i) {
    const DISPLAYCONFIG_PATH_INFO& path = paths[i];
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
    if (std::wcscmp(source.viewGdiDeviceName, gdi_name) != 0) continue;
    DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
    white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    white.header.size = sizeof(white);
    white.header.adapterId = path.targetInfo.adapterId;
    white.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0)
      return static_cast<f32>(white.SDRWhiteLevel) / 1000.0f * k_scrgb_unit_nits;
  }
  return 0.0f;
}

template <class T>
void release(T*& p) noexcept {
  if (p != nullptr) p->Release();
  p = nullptr;
}

const char* present_mode_name(VkPresentModeKHR mode) {
  switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo_relaxed";
    default: return nullptr;
  }
}

std::string number(u32 value) {
  char text[16];
  std::snprintf(text, sizeof(text), "%u", value);
  return text;
}

// One hidden window's surface, asked of every device.
void offers_on(VkInstance instance, PFN_vkCreateWin32SurfaceKHR create_surface,
               const Vector<VkPhysicalDevice>& devices, const DisplayOutput& output,
               Vector<SurfaceOffers>& out) {
  static const wchar_t* k_class = L"engine-display-probe";
  const HINSTANCE module = GetModuleHandleW(nullptr);
  WNDCLASSEXW cls{};
  cls.cbSize = sizeof(cls);
  cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = module;
  cls.lpszClassName = k_class;
  RegisterClassExW(&cls);  // fails harmlessly when already registered
  const i32 x = output.desktop.size() == 4 ? output.desktop[0] + 16 : 0;
  const i32 y = output.desktop.size() == 4 ? output.desktop[1] + 16 : 0;
  // WS_POPUP and never WS_VISIBLE: the window exists for the surface and is never shown.
  HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, k_class, L"", WS_POPUP, x, y, 64, 64, nullptr,
                                nullptr, module, nullptr);
  if (window == nullptr) return;
  VkWin32SurfaceCreateInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
  info.hinstance = module;
  info.hwnd = window;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (create_surface(instance, &info, nullptr, &surface) == VK_SUCCESS) {
    for (const VkPhysicalDevice physical : devices) {
      SurfaceOffers offers;
      VkPhysicalDeviceProperties props{};
      vkGetPhysicalDeviceProperties(physical, &props);
      offers.adapter = props.deviceName;
      u32 family_count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
      Vector<VkQueueFamilyProperties> families(family_count);
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
      for (u32 f = 0; f < family_count && !offers.supported; ++f) {
        if ((families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) continue;
        VkBool32 can = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(physical, f, surface, &can) == VK_SUCCESS)
          offers.supported = can == VK_TRUE;
      }
      if (offers.supported) {
        u32 format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, nullptr);
        Vector<VkSurfaceFormatKHR> formats(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, formats.data());
        for (u32 i = 0; i < format_count; ++i) {
          SurfaceOffer offer;
          offer.format = format_name(vk::wrap(formats[i].format));
          offer.format_value = static_cast<u32>(formats[i].format);
          offer.color_space = color_space_name(vk::wrap(formats[i].colorSpace));
          offer.color_space_value = static_cast<u32>(formats[i].colorSpace);
          offers.formats.push_back(std::move(offer));
        }
        u32 mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &mode_count, nullptr);
        Vector<VkPresentModeKHR> modes(mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &mode_count, modes.data());
        for (u32 i = 0; i < mode_count; ++i) {
          const char* name = present_mode_name(modes[i]);
          offers.present_modes.push_back(name != nullptr ? std::string(name)
                                                         : number(static_cast<u32>(modes[i])));
        }
        u32 ext_count = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &ext_count, nullptr);
        Vector<VkExtensionProperties> exts(ext_count);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &ext_count, exts.data());
        for (u32 i = 0; i < ext_count; ++i) {
          if (std::strcmp(exts[i].extensionName, "VK_EXT_hdr_metadata") == 0)
            offers.hdr_metadata = true;
        }
      }
      out.push_back(std::move(offers));
    }
    vkDestroySurfaceKHR(instance, surface, nullptr);
  }
  DestroyWindow(window);
}

}  // namespace

bool display_outputs(Vector<DisplayOutput>& out, std::string* error) {
  out.clear();
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    if (error != nullptr) *error = "CreateDXGIFactory1 failed: no DXGI to ask about the displays";
    return false;
  }
  IDXGIAdapter1* adapter = nullptr;
  for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
    DXGI_ADAPTER_DESC1 adapter_desc{};
    adapter->GetDesc1(&adapter_desc);
    IDXGIOutput* output = nullptr;
    for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
      DisplayOutput entry;
      entry.adapter = narrow(adapter_desc.Description);
      IDXGIOutput6* output6 = nullptr;
      DXGI_OUTPUT_DESC1 desc{};
      if (SUCCEEDED(
              output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6))) &&
          SUCCEEDED(output6->GetDesc1(&desc))) {
        entry.name = narrow(desc.DeviceName);
        entry.desktop = {static_cast<i32>(desc.DesktopCoordinates.left),
                         static_cast<i32>(desc.DesktopCoordinates.top),
                         static_cast<i32>(desc.DesktopCoordinates.right),
                         static_cast<i32>(desc.DesktopCoordinates.bottom)};
        entry.attached = desc.AttachedToDesktop != FALSE;
        entry.bits_per_color = desc.BitsPerColor;
        const char* space = dxgi_color_space_name(desc.ColorSpace);
        entry.color_space =
            space != nullptr ? std::string(space) : number(static_cast<u32>(desc.ColorSpace));
        entry.hdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        entry.min_luminance = desc.MinLuminance;
        entry.max_luminance = desc.MaxLuminance;
        entry.max_full_frame_luminance = desc.MaxFullFrameLuminance;
        entry.red = {desc.RedPrimary[0], desc.RedPrimary[1]};
        entry.green = {desc.GreenPrimary[0], desc.GreenPrimary[1]};
        entry.blue = {desc.BluePrimary[0], desc.BluePrimary[1]};
        entry.white = {desc.WhitePoint[0], desc.WhitePoint[1]};
        entry.sdr_white_nits = sdr_white_nits(desc.DeviceName);
      } else {
        // An output older than IDXGIOutput6 (Windows before 10 1803) has no luminances to give.
        DXGI_OUTPUT_DESC plain{};
        output->GetDesc(&plain);
        entry.name = narrow(plain.DeviceName);
        entry.desktop = {static_cast<i32>(plain.DesktopCoordinates.left),
                         static_cast<i32>(plain.DesktopCoordinates.top),
                         static_cast<i32>(plain.DesktopCoordinates.right),
                         static_cast<i32>(plain.DesktopCoordinates.bottom)};
        entry.attached = plain.AttachedToDesktop != FALSE;
      }
      release(output6);
      release(output);
      out.push_back(std::move(entry));
    }
    release(adapter);
  }
  release(factory);
  return true;
}

bool probe_displays(Vector<DisplayOutput>& out, bool& swapchain_colorspace, std::string* error) {
  swapchain_colorspace = false;
  if (!display_outputs(out, error)) return false;
  if (!vulkan_available(error)) return false;

  u32 available_count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &available_count, nullptr);
  Vector<VkExtensionProperties> available(available_count);
  vkEnumerateInstanceExtensionProperties(nullptr, &available_count, available.data());
  const auto has = [&](const char* name) {
    for (const VkExtensionProperties& e : available) {
      if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
  };
  if (!has("VK_KHR_surface") || !has("VK_KHR_win32_surface")) {
    if (error != nullptr) *error = "the Vulkan loader has no VK_KHR_win32_surface";
    return false;
  }
  Vector<const char*> enabled;
  enabled.push_back("VK_KHR_surface");
  enabled.push_back("VK_KHR_win32_surface");
  swapchain_colorspace = has("VK_EXT_swapchain_colorspace");
  if (swapchain_colorspace) enabled.push_back("VK_EXT_swapchain_colorspace");

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "game_engine";
  app.pEngineName = "game_engine";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo create{};
  create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create.pApplicationInfo = &app;
  create.enabledExtensionCount = enabled.size();
  create.ppEnabledExtensionNames = enabled.data();
  VkInstance instance = VK_NULL_HANDLE;
  if (const VkResult r = vkCreateInstance(&create, nullptr, &instance); r != VK_SUCCESS) {
    if (error != nullptr) *error = std::string("vkCreateInstance failed: ") + result_name(r);
    return false;
  }
  volkLoadInstanceOnly(instance);
  const auto create_surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
      vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
  u32 count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  Vector<VkPhysicalDevice> devices(count);
  if (count > 0) vkEnumeratePhysicalDevices(instance, &count, devices.data());
  if (create_surface != nullptr) {
    for (DisplayOutput& output : out)
      offers_on(instance, create_surface, devices, output, output.surfaces);
  }
  vkDestroyInstance(instance, nullptr);
  detail::reload_device_instance();  // volk_instance.h: a live device keeps its entry points
  ENGINE_LOG_INFO(log_display_probe, "probed displays",
                  log::field("outputs", static_cast<u64>(out.size())),
                  log::field("swapchain_colorspace", swapchain_colorspace));
  return true;
}

#else  // !_WIN32

bool display_outputs(Vector<DisplayOutput>& out, std::string* error) {
  out.clear();
  if (error != nullptr)
    *error =
        "the display probe reads DXGI and makes a Win32 window, so it runs on Windows only; "
        "engine-view's summary reports its own window's offers here";
  return false;
}

bool probe_displays(Vector<DisplayOutput>& out, bool& swapchain_colorspace, std::string* error) {
  swapchain_colorspace = false;
  return display_outputs(out, error);
}

#endif

}  // namespace engine::gfx

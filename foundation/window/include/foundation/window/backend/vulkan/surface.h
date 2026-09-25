#pragma once

// The window system's Vulkan glue (docs/subsystems/window.md, "The Vulkan surface"): the instance
// extensions a surface needs, and making and destroying a surface on a window. It is a backend
// header, under the same convention as gfx's (docs/subsystems/gfx.md, "The RHI surface and the
// backend surface"): `window.h` names no Vulkan type, and only a module that presents — today
// engine-view, and gfx's swapchain test — includes this. A second graphics backend brings its own
// beside it (a D3D12 swapchain wants the window's HWND, which `Window::native()` already gives).
//
//     gfx::DeviceOptions options;
//     const auto extensions = window::vulkan::instance_extensions();
//     options.instance_extensions = extensions.data();
//     options.instance_extension_count = static_cast<u32>(extensions.size());
//     ...device.create(options)...
//     VkSurfaceKHR surface = VK_NULL_HANDLE;
//     window::vulkan::create_surface(window, device.handles().instance, surface, &error);
//     ...a gfx::Swapchain on it...
//     window::vulkan::destroy_surface(device.handles().instance, surface);
//
// The two handle types are forward-declared the way SDL does it, so this header pulls in neither
// SDL nor the Vulkan headers and `foundation/window` stays below `domain/gfx`; on 64-bit targets
// the typedefs are exactly vulkan_core.h's, and a translation unit that includes both gets the
// same typedef twice, which C++ allows. SDL's own Vulkan entry points exist whatever display
// backends SDL was built with, so this compiles and links under ENGINE_WINDOW_BACKENDS=none too;
// there, `init()` fails first and nothing calls it.

#include <core/base/types.h>
#include <foundation/window/window.h>

#include <span>
#include <string>

#if !defined(VULKAN_CORE_H_)
typedef struct VkInstance_T* VkInstance;
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;
#endif

namespace engine::window::vulkan {

// Instance extensions the platform surface needs; empty (with SDL's reason logged) when Vulkan
// is unavailable to SDL. Valid after init().
std::span<const char* const> instance_extensions();

// A surface on `window` for `instance`. False, with `out` null, when there is no window or SDL
// refused.
bool create_surface(const Window& window, VkInstance instance, VkSurfaceKHR& out,
                    std::string* error = nullptr);
void destroy_surface(VkInstance instance, VkSurfaceKHR surface) noexcept;

}  // namespace engine::window::vulkan

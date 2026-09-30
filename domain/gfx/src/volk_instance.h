#pragma once

// Which Vulkan instance volk's process-wide instance table has to point at. Internal to gfx.
//
// volk keeps one table of instance-level entry points per process, and `volkLoadInstanceOnly`
// fills it from whichever instance it is handed (docs/subsystems/gfx.md, "One live device per
// process"). `Device::create` loads its own instance; `enumerate_adapters` creates a short-lived
// instance of its own, loads it, and destroys it — and until 2026-09-30 left the table pointing
// at that destroyed instance, so a live device's instance teardown
// (`vkDestroyDebugUtilsMessengerEXT`, `vkDestroyInstance`) went through stale entry points and
// crashed. engine-host did exactly that on every exit after `gpu.adapters` had run beside an open
// device, and the end-to-end test ignored the host's exit code; the GPU lock found it, because the
// crash left the lock behind (docs/subsystems/gpu_lock.md). So the live device's instance is
// recorded here, and whoever loads another instance puts this one back when it is done.

#include <domain/gfx/backend/vulkan/vulkan.h>

namespace engine::gfx::detail {

// The live Device's instance, or VK_NULL_HANDLE when no device is open. Set by Device::create
// once volk has loaded it, cleared by Device::destroy before the instance is destroyed.
void set_device_instance(VkInstance instance) noexcept;
VkInstance device_instance() noexcept;

// Points volk's instance table back at the live device's instance, if there is one. Call after
// destroying an instance of your own that you had loaded.
void reload_device_instance() noexcept;

}  // namespace engine::gfx::detail

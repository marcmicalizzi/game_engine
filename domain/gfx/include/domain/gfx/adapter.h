#pragma once

// Device discovery (docs/plan/04-renderer.md §4.1, ADR-0006): the first piece of the RHI.
// Enumerates every Vulkan physical device the loader exposes with the properties the renderer
// needs to choose a device and a capability tier: API version, driver, device-local memory,
// queue families, and the extensions behind each tier (ray tracing, mesh shaders, cluster
// acceleration structures, memory decompression, descriptor buffers).
//
// Works without the Vulkan SDK: volk loads the driver's loader library at run time. On a
// machine without one, enumerate_adapters returns false with a message and nothing else fails,
// so build and test machines without GPUs keep working.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <schemas/gfx.h>
#include <span>
#include <string>

namespace engine::gfx {

// The device extensions the renderer cares about, in the order they appear in
// AdapterInfo::extensions. Names, not header macros, so a header without an extension still
// compiles and simply reports it absent.
std::span<const char* const> extensions_of_interest() noexcept;

// True when a Vulkan loader library is present and supports instance version 1.3 or later.
bool vulkan_available(std::string* error = nullptr);

// Fills `out` with one entry per physical device. False (with `error`) when there is no loader,
// the instance cannot be created, or enumeration fails. Sorted: discrete GPUs first, then by
// device-local memory descending, so `out[0]` is the default choice.
bool enumerate_adapters(Vector<AdapterInfo>& out, std::string* error = nullptr);

// One line per adapter for logs and the CLI.
std::string describe_adapters(std::span<const AdapterInfo> adapters);

}  // namespace engine::gfx

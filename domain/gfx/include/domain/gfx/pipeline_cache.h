#pragma once

// The device's pipeline cache on disk (docs/subsystems/gfx.md, "The pipeline cache"; roadmap
// W75). Every pipeline the device makes goes through one `VkPipelineCache` the device owns, filled
// at creation from `<dir>/<vendor>-<device>-<driver>.bin` and written back when the device is
// destroyed if it grew, so the second launch of a build finds the driver's compiled pipelines
// where the first left them — 9.1 s of the forest's renderer stage on the first launch after a
// rebuild, 0.2 s after (docs/experiments/startup-trace-forest-2026-10-24.md).
//
// A file is used only when its header is this device's: `VkPipelineCacheHeaderVersionOne`'s
// header size, version, vendor, device and `pipelineCacheUUID`, checked here before the driver is
// shown a byte of it. A file that is someone else's (another driver, another card) or is not a
// cache at all is ignored with a log line and the device starts empty; the driver checks again
// and may still refuse, which is the same. **Nothing decides from a timestamp**: the UUID is the
// driver's statement of what it can read.
//
// API-neutral: the header's fields are plain integers here so a test can make and break one
// without a device.

#include <core/base/types.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::gfx {

// What a cache file must say to be read: the physical device's identity as the driver reports it.
struct PipelineCacheIdentity {
  u32 vendor_id = 0;
  u32 device_id = 0;
  u32 driver_version = 0;
  u8 uuid[16] = {};
};

// The 32-byte `VkPipelineCacheHeaderVersionOne` every cache begins with.
inline constexpr u32 k_pipeline_cache_header_bytes = 32;

// Whether `bytes` begins with a version-one header naming this device: false for anything shorter
// than the header, a header size other than 32, another version, vendor, device or UUID.
bool pipeline_cache_header_matches(std::span<const u8> bytes,
                                   const PipelineCacheIdentity& identity) noexcept;

// The file a device with `identity` reads and writes in `dir`: `<vendor>-<device>-<driver>.bin`,
// each in lower-case hexadecimal, so two drivers on one machine keep two files.
std::string pipeline_cache_file(std::string_view dir, const PipelineCacheIdentity& identity);

// What the device did with its cache, for the log and a summary.
struct PipelineCacheReport {
  bool enabled = false;  // a directory was given and ENGINE_PIPELINE_CACHE is not 0
  std::string path;      // the file, when enabled
  bool loaded = false;   // the file was there, matched and was handed to the driver
  u64 loaded_bytes = 0;  // its size
  std::string ignored;   // why a file that was there was not used; empty otherwise
  u64 saved_bytes = 0;   // what the last save wrote; 0 when it wrote nothing
};

}  // namespace engine::gfx

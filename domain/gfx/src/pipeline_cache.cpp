// The pipeline cache's file and header (pipeline_cache.h); the device's half is in device.cpp.
#include <domain/gfx/pipeline_cache.h>
#include <foundation/io/vfs.h>

#include <cstdio>
#include <cstring>

namespace engine::gfx {

namespace {

u32 read_u32(std::span<const u8> bytes, u32 at) noexcept {
  u32 value = 0;
  std::memcpy(&value, bytes.data() + at, sizeof(value));
  return value;
}

}  // namespace

bool pipeline_cache_header_matches(std::span<const u8> bytes,
                                   const PipelineCacheIdentity& identity) noexcept {
  if (bytes.size() < k_pipeline_cache_header_bytes) return false;
  // VkPipelineCacheHeaderVersionOne: headerSize, headerVersion (1), vendorID, deviceID, then the
  // sixteen bytes of pipelineCacheUUID — little-endian u32s, as every Vulkan platform lays them
  // out.
  if (read_u32(bytes, 0) != k_pipeline_cache_header_bytes) return false;
  if (read_u32(bytes, 4) != 1u) return false;
  if (read_u32(bytes, 8) != identity.vendor_id) return false;
  if (read_u32(bytes, 12) != identity.device_id) return false;
  return std::memcmp(bytes.data() + 16, identity.uuid, sizeof(identity.uuid)) == 0;
}

std::string pipeline_cache_file(std::string_view dir, const PipelineCacheIdentity& identity) {
  char name[48];
  std::snprintf(name, sizeof(name), "%04x-%04x-%08x.bin", identity.vendor_id, identity.device_id,
                identity.driver_version);
  return io::join_path(dir, name);
}

}  // namespace engine::gfx

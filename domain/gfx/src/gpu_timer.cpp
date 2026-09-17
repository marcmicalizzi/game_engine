#include <core/base/assert.h>
#include <domain/gfx/device.h>
#include <domain/gfx/gpu_timer.h>

#include <cstring>

namespace engine::gfx {

GpuTimer::~GpuTimer() { destroy(); }

bool GpuTimer::create(const Device& device, u32 frames_in_flight, u32 max_zones,
                      std::string* error) {
  ENGINE_VERIFY(device_ == nullptr, "GpuTimer::create: already created");
  if (!device.valid() || frames_in_flight == 0 || max_zones == 0) {
    if (error != nullptr) *error = "GpuTimer needs a valid device, frames in flight, and zones";
    return false;
  }
  const Handles& h = device.handles();
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(h.physical, &properties);
  period_ns_ = static_cast<f64>(properties.limits.timestampPeriod);
  if (period_ns_ <= 0.0) {
    if (error != nullptr) *error = "GpuTimer: the device does not support timestamps";
    return false;
  }
  max_zones_ = max_zones;
  slots_.resize(frames_in_flight);
  for (Slot& slot : slots_) {
    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = max_zones * 2;
    if (const VkResult r = vkCreateQueryPool(h.device, &info, nullptr, &slot.pool);
        r != VK_SUCCESS) {
      if (error != nullptr) *error = std::string("vkCreateQueryPool: ") + result_name(r);
      device_ = &device;
      destroy();
      return false;
    }
    // Queries must be reset before their first use; host reset is a required feature.
    vkResetQueryPool(h.device, slot.pool, 0, max_zones * 2);
  }
  device_ = &device;
  return true;
}

void GpuTimer::destroy() noexcept {
  if (device_ == nullptr) return;
  const Handles& h = device_->handles();
  for (Slot& slot : slots_) {
    if (slot.pool != VK_NULL_HANDLE) vkDestroyQueryPool(h.device, slot.pool, nullptr);
  }
  slots_.clear();
  results_.clear();
  device_ = nullptr;
  open_ = false;
}

void GpuTimer::begin_frame(VkCommandBuffer, u32 slot_index) {
  ENGINE_VERIFY(device_ != nullptr && slot_index < slots_.size(),
                "GpuTimer::begin_frame: bad slot");
  ENGINE_VERIFY(!open_, "GpuTimer::begin_frame: a zone is still open");
  Slot& slot = slots_[slot_index];
  const Handles& h = device_->handles();
  results_.clear();
  if (slot.used > 0) {
    Vector<u64> stamps(slot.used);
    const VkResult r = vkGetQueryPoolResults(h.device, slot.pool, 0, slot.used,
                                             slot.used * sizeof(u64), stamps.data(), sizeof(u64),
                                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (r == VK_SUCCESS) {
      for (u32 z = 0; z < slot.used / 2; ++z) {
        const u64 begin_stamp = stamps[z * 2];
        const u64 end_stamp = stamps[z * 2 + 1];
        Zone zone;
        zone.name = slot.names[z];
        zone.ms = end_stamp > begin_stamp
                      ? static_cast<f64>(end_stamp - begin_stamp) * period_ns_ / 1.0e6
                      : 0.0;
        results_.push_back(zone);
      }
    }
    vkResetQueryPool(h.device, slot.pool, 0, slot.used);
  }
  slot.used = 0;
  slot.names.clear();
  current_ = slot_index;
}

void GpuTimer::begin(VkCommandBuffer commands, const char* name) {
  ENGINE_VERIFY(device_ != nullptr && !open_, "GpuTimer::begin: not created or a zone is open");
  Slot& slot = slots_[current_];
  if (slot.used + 2 > max_zones_ * 2) return;  // out of queries: silently drop the zone
  slot.names.push_back(name);
  vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot.pool, slot.used);
  ++slot.used;
  open_ = true;
}

void GpuTimer::end(VkCommandBuffer commands) {
  if (!open_) return;
  Slot& slot = slots_[current_];
  vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot.pool, slot.used);
  ++slot.used;
  open_ = false;
}

f64 GpuTimer::total_ms() const noexcept {
  f64 total = 0.0;
  for (const Zone& z : results_)
    total += z.ms;
  return total;
}

f64 GpuTimer::ms(const char* name) const noexcept {
  f64 total = 0.0;
  for (const Zone& z : results_) {
    if (std::strcmp(z.name, name) == 0) total += z.ms;
  }
  return total;
}

}  // namespace engine::gfx

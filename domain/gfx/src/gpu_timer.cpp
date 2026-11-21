#include <core/base/assert.h>
#include <core/time/time.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
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
  // The pool holds every zone's two queries and, after them, the frame's two marks (W155).
  const u32 queries = max_zones * 2 + 2;
  slots_.resize(frames_in_flight);
  // Everything a frame touches is sized here, for the most zones a slot holds, so `begin_frame`
  // and `begin` allocate nothing: until 2026-10-07 every frame's read-back made a vector of its
  // own, the one engine allocation a frame the flythrough's frame-loop count found on every scene.
  stamps_.resize(static_cast<usize>(max_zones) * 2);
  results_.reserve(max_zones);
  for (Slot& slot : slots_) {
    slot.names.reserve(max_zones);
    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = queries;
    VkQueryPool pool = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateQueryPool(h.device, &info, nullptr, &pool); r != VK_SUCCESS) {
      if (error != nullptr) *error = std::string("vkCreateQueryPool: ") + result_name(r);
      device_ = &device;
      destroy();
      return false;
    }
    slot.pool = vk::wrap(pool);
    // Queries must be reset before their first use; host reset is a required feature.
    vkResetQueryPool(h.device, pool, 0, queries);
  }
  device_ = &device;
  calibrated_ = h.calibrated_timestamps && calibrate();
  return true;
}

// One device timestamp against the host's clock, the host's read either side of the call and the
// middle taken (as `Swapchain::calibrate` does): the call takes microseconds, so the error is a
// fraction of what a frame's record says in milliseconds.
bool GpuTimer::calibrate() noexcept {
  VkCalibratedTimestampInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
  info.timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
  u64 ticks = 0;
  u64 deviation = 0;
  const i64 before = time::monotonic_ns();
  const VkResult r =
      vkGetCalibratedTimestampsKHR(device_->handles().device, 1, &info, &ticks, &deviation);
  const i64 after = time::monotonic_ns();
  if (r != VK_SUCCESS) return false;
  calibration_ticks_ = ticks;
  calibration_ns_ = before + (after - before) / 2;
  return true;
}

i64 GpuTimer::to_host_ns(u64 stamp) const noexcept {
  const f64 from = static_cast<f64>(static_cast<i64>(stamp - calibration_ticks_)) * period_ns_;
  return calibration_ns_ + static_cast<i64>(from);
}

void GpuTimer::destroy() noexcept {
  if (device_ == nullptr) return;
  const Handles& h = device_->handles();
  for (Slot& slot : slots_) {
    if (slot.pool) vkDestroyQueryPool(h.device, vk::native(slot.pool), nullptr);
  }
  slots_.clear();
  results_.clear();
  stamps_.clear();
  device_ = nullptr;
  open_ = false;
}

void GpuTimer::begin_frame(CommandList commands, u32 slot_index) {
  ENGINE_VERIFY(device_ != nullptr && slot_index < slots_.size(),
                "GpuTimer::begin_frame: bad slot");
  ENGINE_VERIFY(!open_, "GpuTimer::begin_frame: a zone is still open");
  Slot& slot = slots_[slot_index];
  const Handles& h = device_->handles();
  results_.clear();
  if (slot.used > 0) {
    const u64* stamps = stamps_.data();
    const VkResult r = vkGetQueryPoolResults(h.device, vk::native(slot.pool), 0, slot.used,
                                             slot.used * sizeof(u64), stamps_.data(), sizeof(u64),
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
    vkResetQueryPool(h.device, vk::native(slot.pool), 0, slot.used);
  }
  // The frame's marks (W155), when both were written: the GPU's first and last commands of it.
  span_ = FrameSpan{};
  const u32 mark = max_zones_ * 2;
  if (slot.marks == 3) {
    u64 marks[2] = {};
    const VkResult r = vkGetQueryPoolResults(h.device, vk::native(slot.pool), mark, 2,
                                             sizeof(marks), marks, sizeof(u64),
                                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (r == VK_SUCCESS && marks[1] >= marks[0]) {
      span_.valid = true;
      span_.span_ms = static_cast<f64>(marks[1] - marks[0]) * period_ns_ / 1.0e6;
      if (calibrated_) {
        // Calibrated again every half second: the tick rate does not move, so this only keeps the
        // two clocks' drift out of a long session's numbers.
        if (time::monotonic_ns() - calibration_ns_ > 500'000'000) (void)calibrate();
        span_.first_ns = to_host_ns(marks[0]);
        span_.last_ns = to_host_ns(marks[1]);
      }
    }
  }
  if (slot.marks != 0) vkResetQueryPool(h.device, vk::native(slot.pool), mark, 2);
  slot.marks = 0;
  slot.used = 0;
  slot.names.clear();
  current_ = slot_index;
  // The frame's first mark, at the head of its commands.
  vkCmdWriteTimestamp2(vk::native(commands), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       vk::native(slot.pool), mark);
  slot.marks = 1;
}

void GpuTimer::end_frame(CommandList commands) {
  if (device_ == nullptr || open_) return;
  Slot& slot = slots_[current_];
  if (slot.marks != 1) return;
  vkCmdWriteTimestamp2(vk::native(commands), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       vk::native(slot.pool), max_zones_ * 2 + 1);
  slot.marks = 3;
}

void GpuTimer::begin(CommandList commands, const char* name) {
  ENGINE_VERIFY(device_ != nullptr && !open_, "GpuTimer::begin: not created or a zone is open");
  Slot& slot = slots_[current_];
  if (slot.used + 2 > max_zones_ * 2) return;  // out of queries: silently drop the zone
  slot.names.push_back(name);
  vkCmdWriteTimestamp2(vk::native(commands), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       vk::native(slot.pool), slot.used);
  ++slot.used;
  open_ = true;
}

void GpuTimer::end(CommandList commands) {
  if (!open_) return;
  Slot& slot = slots_[current_];
  vkCmdWriteTimestamp2(vk::native(commands), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       vk::native(slot.pool), slot.used);
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

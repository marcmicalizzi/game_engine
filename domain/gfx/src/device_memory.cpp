#include <core/base/macros.h>
#include <core/time/time.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/device.h>
#include <domain/gfx/device_memory.h>

#include <atomic>

#if ENGINE_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <execinfo.h>
#endif

namespace engine::gfx {

namespace {

std::atomic<bool> g_on{false};
std::atomic<u64> g_floor{k_large_buffer_bytes};
std::atomic<bool> g_lock{false};
LargeBuffer g_buffers[k_large_buffer_slots];
u32 g_count = 0;  // under g_lock
std::atomic<u64> g_dropped{0};
// The events in order (W51-6), under g_lock: the first k_large_buffer_events since the log was on.
LargeBufferEvent g_events[k_large_buffer_events];
u32 g_event_count = 0;
std::atomic<u64> g_events_dropped{0};
std::atomic<i64> g_started_ns{0};

void push_event(const LargeBuffer& entry, bool made) noexcept {  // under g_lock
  if (g_event_count >= k_large_buffer_events) {
    g_events_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  LargeBufferEvent& e = g_events[g_event_count++];
  e.ns = time::monotonic_ns();
  e.bytes = entry.bytes;
  e.made = made;
  e.host_visible = entry.host_visible;
  e.depth = entry.depth;
  for (u32 f = 0; f < k_large_buffer_frames; ++f)
    e.stack[f] = entry.stack[f];
}

void lock() noexcept {
  while (g_lock.exchange(true, std::memory_order_acquire)) {
  }
}
void unlock() noexcept { g_lock.store(false, std::memory_order_release); }

}  // namespace

void set_large_buffer_log(bool on) noexcept {
  lock();
  if (!on) g_count = 0;
  g_event_count = 0;
  g_events_dropped.store(0, std::memory_order_relaxed);
  g_started_ns.store(on ? time::monotonic_ns() : 0, std::memory_order_relaxed);
  unlock();
  g_on.store(on, std::memory_order_relaxed);
}

u32 large_buffer_events(LargeBufferEvent* out, u32 capacity) noexcept {
  lock();
  const u32 count = g_event_count;
  for (u32 i = 0; i < count && i < capacity; ++i)
    out[i] = g_events[i];
  unlock();
  return count;
}

u64 large_buffer_events_dropped() noexcept {
  return g_events_dropped.load(std::memory_order_relaxed);
}

i64 large_buffer_log_started_ns() noexcept { return g_started_ns.load(std::memory_order_relaxed); }

u32 large_buffers(LargeBuffer* out, u32 capacity) noexcept {
  lock();
  const u32 count = g_count;
  for (u32 i = 0; i < count && i < capacity; ++i)
    out[i] = g_buffers[i];
  unlock();
  return count;
}

u64 large_buffers_dropped() noexcept { return g_dropped.load(std::memory_order_relaxed); }

void set_large_buffer_floor(u64 bytes) noexcept {
  lock();
  g_floor.store(bytes < k_large_buffer_min_bytes ? k_large_buffer_min_bytes : bytes,
                std::memory_order_relaxed);
  g_count = 0;
  g_event_count = 0;
  unlock();
}

u64 large_buffer_floor() noexcept { return g_floor.load(std::memory_order_relaxed); }

namespace detail {

bool large_buffer_log_on() noexcept { return g_on.load(std::memory_order_relaxed); }

bool large_buffer_logged(u64 bytes) noexcept {
  return bytes >= k_large_buffer_min_bytes && g_on.load(std::memory_order_relaxed) &&
         bytes >= g_floor.load(std::memory_order_relaxed);
}

void note_large_buffer(u64 key, u64 bytes, bool host_visible) noexcept {
  LargeBuffer entry{};
  entry.key = key;
  entry.bytes = bytes;
  entry.host_visible = host_visible;
#if ENGINE_PLATFORM_WINDOWS
  entry.depth = ::CaptureStackBackTrace(2, k_large_buffer_frames, entry.stack, nullptr);
#else
  const int depth = ::backtrace(entry.stack, static_cast<int>(k_large_buffer_frames));
  entry.depth = static_cast<u16>(depth > 0 ? depth : 0);
#endif
  lock();
  if (g_count < k_large_buffer_slots) {
    g_buffers[g_count++] = entry;
  } else {
    g_dropped.fetch_add(1, std::memory_order_relaxed);
  }
  push_event(entry, true);
  unlock();
}

void forget_large_buffer(u64 key) noexcept {
  lock();
  for (u32 i = 0; i < g_count; ++i) {
    if (g_buffers[i].key != key) continue;
    push_event(g_buffers[i], false);
    g_buffers[i] = g_buffers[--g_count];
    break;
  }
  unlock();
}

}  // namespace detail

bool device_memory_report(const Device& device, DeviceMemoryReport& out) noexcept {
  if (!device.valid()) return false;
  const Handles& h = device.handles();
  if (h.allocator == nullptr || h.physical == VK_NULL_HANDLE) return false;
  const bool have_budget = device.features().memory_budget;
  VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
  budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
  VkPhysicalDeviceMemoryProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
  if (have_budget) properties.pNext = &budget;
  vkGetPhysicalDeviceMemoryProperties2(h.physical, &properties);
  VmaBudget heaps[VK_MAX_MEMORY_HEAPS] = {};
  vmaGetHeapBudgets(h.allocator, heaps);

  out = DeviceMemoryReport{};
  out.valid = true;
  const VkPhysicalDeviceMemoryProperties& mem = properties.memoryProperties;
  for (u32 i = 0; i < mem.memoryHeapCount; ++i) {
    const bool local = (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
    const VmaStatistics& s = heaps[i].statistics;
    (local ? out.device_local_block_bytes : out.host_block_bytes) += s.blockBytes;
    (local ? out.device_local_allocation_bytes : out.host_allocation_bytes) += s.allocationBytes;
    if (have_budget)
      (local ? out.device_local_usage_bytes : out.host_usage_bytes) += budget.heapUsage[i];
    out.block_count += s.blockCount;
    out.allocation_count += s.allocationCount;
  }
  return true;
}

}  // namespace engine::gfx

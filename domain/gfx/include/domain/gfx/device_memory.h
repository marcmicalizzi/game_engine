#pragma once

// **What the device holds, by heap kind** (docs/subsystems/gfx.md, "What the device holds";
// docs/subsystems/platform.md, "Where the commit is"): for a memory report, the allocator's own
// count of what it took from the driver (`block`) and how much of that it handed to buffers and
// images (`allocation`), and the driver's own count for this process (`usage`,
// VK_EXT_memory_budget; zero without it), each for the device-local heaps and for the rest — the
// system memory the driver maps for the device (upload, readback, host-visible buffers), which
// lives in this process's address space. A cold query: a report's, never a frame's.

#include <core/base/types.h>

namespace engine::gfx {

class Device;

struct DeviceMemoryReport {
  bool valid = false;
  u64 device_local_block_bytes = 0;
  u64 device_local_allocation_bytes = 0;
  u64 device_local_usage_bytes = 0;
  u64 host_block_bytes = 0;
  u64 host_allocation_bytes = 0;
  u64 host_usage_bytes = 0;
  u32 block_count = 0;
  u32 allocation_count = 0;
};

// False with `out` untouched when the device is not valid.
bool device_memory_report(const Device& device, DeviceMemoryReport& out) noexcept;

// **The large-buffer log** (gfx.md, "What the device holds"): the device's counterpart of the
// engine heap's large-allocation log (core/memory/large_allocations.h). While it is on, every live
// buffer of `k_large_buffer_bytes` or more made by `create_buffer` is kept in a static table with
// its size, whether it is host-visible, and its call stack, so a memory report can say which line
// sized what the device holds — and on Windows every device-local byte is a byte of the process's
// commit as well (the driver's system-memory backing, measured by `tests/device_memory_tests.cpp`).
// Off — the default — a buffer that large pays one relaxed load and a smaller one nothing.
inline constexpr u64 k_large_buffer_bytes = u64{16} << 20;
inline constexpr u64 k_large_buffer_min_bytes = u64{1} << 20;
inline constexpr u32 k_large_buffer_slots = 8192;
inline constexpr u32 k_large_buffer_frames = 12;
struct LargeBuffer {
  u64 key = 0;  // the allocation's handle
  u64 bytes = 0;
  bool host_visible = false;
  u16 depth = 0;
  void* stack[k_large_buffer_frames]{};
};
void set_large_buffer_log(bool on) noexcept;
// The log's floor (W51-2): `k_large_buffer_bytes` unless a host lowers it, never under
// `k_large_buffer_min_bytes`; set before the log is on (it empties the table), as the engine
// heap's (core/memory/large_allocations.h, `set_large_allocation_floor`).
void set_large_buffer_floor(u64 bytes) noexcept;
u64 large_buffer_floor() noexcept;
// Copies the live large buffers (up to `capacity`) and returns how many there are.
u32 large_buffers(LargeBuffer* out, u32 capacity) noexcept;
u64 large_buffers_dropped() noexcept;

// **The large-buffer log's events, in order** (W51-6; gfx.md, "What the device holds"): every make
// and every free of a logged buffer since the log was turned on, with the monotonic time it
// happened, its size and the stack that made it (a free carries its make's), so a report can say
// what the device took and gave back between two moments — and which blocks it held twice. The
// first `k_large_buffer_events` are kept and the rest counted; turning the log on or setting its
// floor empties it. A load makes a few thousand at a 2 MiB floor; a steady frame makes none.
inline constexpr u32 k_large_buffer_events = 16384;
struct LargeBufferEvent {
  i64 ns = 0;  // core/time's monotonic clock
  u64 bytes = 0;
  bool made = false;  // false: a free
  bool host_visible = false;
  u16 depth = 0;
  void* stack[k_large_buffer_frames]{};
};
// Copies the events kept (up to `capacity`, oldest first) and returns how many were kept.
u32 large_buffer_events(LargeBufferEvent* out, u32 capacity) noexcept;
u64 large_buffer_events_dropped() noexcept;
// When the log was last turned on (0 while off): the events' times are read against it.
i64 large_buffer_log_started_ns() noexcept;

namespace detail {
bool large_buffer_log_on() noexcept;
// On, and `bytes` at or above the floor: whether a buffer of that size is the log's.
bool large_buffer_logged(u64 bytes) noexcept;
void note_large_buffer(u64 key, u64 bytes, bool host_visible) noexcept;
void forget_large_buffer(u64 key) noexcept;
}  // namespace detail

}  // namespace engine::gfx

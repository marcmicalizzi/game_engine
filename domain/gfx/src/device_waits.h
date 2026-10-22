#pragma once

// The calling thread's count of stops for the device (`gfx::device_waits_on_thread`). Internal to
// gfx: `submit_immediate` (device.cpp) and `FrameContext::wait_idle` (frame.cpp) both count into
// it.

namespace engine::gfx::detail {

void count_device_wait() noexcept;

}  // namespace engine::gfx::detail

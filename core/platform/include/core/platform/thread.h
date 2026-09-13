#pragma once

// Thread affinity, priority, naming, and identification. All functions act on the calling
// thread unless stated otherwise and return false when the OS refuses.

#include <core/base/types.h>
#include <core/platform/topology.h>

namespace engine::platform {

enum class ThreadPriority : u8 { Low, BelowNormal, Normal, AboveNormal, High };

// Pins the calling thread to one logical CPU (engine id).
bool pin_current_thread(u16 cpu);
// Pins the calling thread to any CPU in the set (must be non-empty).
bool pin_current_thread(const CpuSet& cpus);
bool set_current_thread_priority(ThreadPriority priority);
// Names are truncated to 63 characters.
bool set_current_thread_name(const char* name);

// Engine id of the CPU the calling thread is running on right now, or k_invalid_cpu.
u16 current_cpu();

// Monotonic per-process thread identity (cheap, thread-local).
u32 current_thread_index();

void yield_thread() noexcept;
void pause_cpu() noexcept;  // spin-wait hint
void sleep_ms(u32 milliseconds) noexcept;

}  // namespace engine::platform

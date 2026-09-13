#include <core/platform/thread.h>

#include <atomic>
#include <cstring>
#include <thread>

#if ENGINE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <immintrin.h>
#elif ENGINE_PLATFORM_LINUX
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <immintrin.h>
#endif
#endif

namespace engine::platform {

namespace {
std::atomic<u32> g_next_thread_index{0};
thread_local u32 t_thread_index = 0xFFFFFFFFu;
}  // namespace

u32 current_thread_index() {
  if (t_thread_index == 0xFFFFFFFFu) t_thread_index = g_next_thread_index.fetch_add(1, std::memory_order_relaxed);
  return t_thread_index;
}

#if ENGINE_PLATFORM_WINDOWS

bool pin_current_thread(u16 cpu) {
  const Topology& t = topology();
  if (cpu >= t.cpu_count()) return false;
  const LogicalCpu& c = t.cpus[cpu];
  GROUP_AFFINITY ga{};
  ga.Group = c.os_group;
  ga.Mask = static_cast<KAFFINITY>(u64{1} << c.os_index);
  return ::SetThreadGroupAffinity(::GetCurrentThread(), &ga, nullptr) != 0;
}

bool pin_current_thread(const CpuSet& cpus) {
  const Topology& t = topology();
  // A group affinity covers one processor group; use the group of the first CPU and every
  // requested CPU in that group.
  const u16 first = cpus.first();
  if (first == k_invalid_cpu || first >= t.cpu_count()) return false;
  const u16 group = t.cpus[first].os_group;
  GROUP_AFFINITY ga{};
  ga.Group = group;
  cpus.for_each([&](u16 id) {
    if (id < t.cpu_count() && t.cpus[id].os_group == group) ga.Mask |= static_cast<KAFFINITY>(u64{1} << t.cpus[id].os_index);
  });
  return ::SetThreadGroupAffinity(::GetCurrentThread(), &ga, nullptr) != 0;
}

bool set_current_thread_priority(ThreadPriority priority) {
  int p = THREAD_PRIORITY_NORMAL;
  switch (priority) {
    case ThreadPriority::Low: p = THREAD_PRIORITY_LOWEST; break;
    case ThreadPriority::BelowNormal: p = THREAD_PRIORITY_BELOW_NORMAL; break;
    case ThreadPriority::Normal: p = THREAD_PRIORITY_NORMAL; break;
    case ThreadPriority::AboveNormal: p = THREAD_PRIORITY_ABOVE_NORMAL; break;
    case ThreadPriority::High: p = THREAD_PRIORITY_HIGHEST; break;
  }
  return ::SetThreadPriority(::GetCurrentThread(), p) != 0;
}

bool set_current_thread_name(const char* name) {
  wchar_t wide[64];
  usize n = 0;
  for (; n < 63 && name[n] != '\0'; ++n) wide[n] = static_cast<wchar_t>(static_cast<unsigned char>(name[n]));
  wide[n] = L'\0';
  return SUCCEEDED(::SetThreadDescription(::GetCurrentThread(), wide));
}

u16 current_cpu() {
  PROCESSOR_NUMBER pn{};
  ::GetCurrentProcessorNumberEx(&pn);
  const LogicalCpu* c = topology().find(pn.Group, pn.Number);
  return c != nullptr ? c->id : k_invalid_cpu;
}

void yield_thread() noexcept { ::SwitchToThread(); }
void pause_cpu() noexcept { _mm_pause(); }
void sleep_ms(u32 milliseconds) noexcept { ::Sleep(milliseconds); }

#elif ENGINE_PLATFORM_LINUX

bool pin_current_thread(u16 cpu) {
  const Topology& t = topology();
  if (cpu >= t.cpu_count()) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(t.cpus[cpu].os_index, &set);
  return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
}

bool pin_current_thread(const CpuSet& cpus) {
  const Topology& t = topology();
  if (cpus.empty()) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  cpus.for_each([&](u16 id) {
    if (id < t.cpu_count()) CPU_SET(t.cpus[id].os_index, &set);
  });
  return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
}

bool set_current_thread_priority(ThreadPriority priority) {
  // Without elevated privileges only niceness can be lowered; treat everything at or above
  // Normal as success without change.
  int nice_value = 0;
  switch (priority) {
    case ThreadPriority::Low: nice_value = 15; break;
    case ThreadPriority::BelowNormal: nice_value = 5; break;
    default: return true;
  }
  return ::nice(nice_value) != -1;
}

bool set_current_thread_name(const char* name) {
  char buf[16];
  usize n = 0;
  for (; n < 15 && name[n] != '\0'; ++n) buf[n] = name[n];
  buf[n] = '\0';
  return ::pthread_setname_np(::pthread_self(), buf) == 0;
}

u16 current_cpu() {
  const int k = ::sched_getcpu();
  if (k < 0) return k_invalid_cpu;
  const LogicalCpu* c = topology().find(0, static_cast<u16>(k));
  return c != nullptr ? c->id : k_invalid_cpu;
}

void yield_thread() noexcept { ::sched_yield(); }
void pause_cpu() noexcept {
#if defined(__x86_64__)
  _mm_pause();
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}
void sleep_ms(u32 milliseconds) noexcept {
  timespec ts{static_cast<time_t>(milliseconds / 1000), static_cast<long>(milliseconds % 1000) * 1000000L};
  ::nanosleep(&ts, nullptr);
}

#endif

}  // namespace engine::platform

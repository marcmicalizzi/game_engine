#include <core/profiling/startup_trace.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <psapi.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#endif

namespace engine::profiling {

namespace detail {
std::atomic<bool> g_trace_on{false};
}

namespace {

// A slot is written by the thread that owns the event and read by whoever snapshots: the fields
// set at the begin are published by `ready`, the ones set at the end by `end_ns`.
struct Slot {
  const char* name = "";
  TraceKind kind = TraceKind::stage;
  u32 thread = 0;
  u32 parent = k_trace_none;
  i64 begin_ns = 0;
  i64 cpu_begin = 0;
  u64 read_begin = 0;
  i64 cpu_ns = 0;
  u64 read_bytes = 0;
  std::atomic<i64> end_ns{-1};
  std::atomic<i64> wait_ns{0};
  std::atomic<u64> items{0};
  std::atomic<u32> dep_count{0};
  std::atomic<u32> deps[k_trace_max_deps] = {};
  TraceSample sample;
  char detail[k_trace_detail_bytes] = {};
  std::atomic<bool> ready{false};
};

struct State {
  std::unique_ptr<Slot[]> slots;
  u32 capacity = 0;
  std::atomic<u32> count{0};
  std::atomic<u32> dropped{0};
  std::atomic<u32> refused{0};
  std::atomic<u32> threads{0};
  i64 origin_ns = 0;
  u32 sample_ms = 0;
  std::thread sampler;
  std::mutex mutex;
  std::condition_variable wake;
  bool stopping = false;
};

State& state() {
  static State s;
  return s;
}

thread_local u32 t_thread = k_trace_none;
thread_local u32 t_current = k_trace_none;

u32 this_thread_index() noexcept {
  if (t_thread == k_trace_none) t_thread = state().threads.fetch_add(1, std::memory_order_relaxed);
  return t_thread;
}

void copy_detail(char (&out)[k_trace_detail_bytes], std::string_view detail) noexcept {
  const usize n = std::min<usize>(detail.size(), k_trace_detail_bytes - 1);
  if (n != 0) std::memcpy(out, detail.data(), n);
  out[n] = '\0';
}

// A slot for a new event, or null when the trace is off or full.
Slot* claim(u32& id) noexcept {
  State& s = state();
  if (!s.slots) return nullptr;
  const u32 index = s.count.fetch_add(1, std::memory_order_relaxed);
  if (index >= s.capacity) {
    s.count.store(s.capacity, std::memory_order_relaxed);
    s.dropped.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  id = index;
  return &s.slots[index];
}

u64 process_read_bytes() noexcept;  // below, per platform: the read counter alone

void sampler_main() {
  State& s = state();
  std::unique_lock<std::mutex> lock(s.mutex);
  while (!s.stopping) {
    lock.unlock();
    u32 id = 0;
    if (Slot* slot = claim(id)) {
      slot->name = "process";
      slot->kind = TraceKind::sample;
      slot->thread = this_thread_index();
      slot->begin_ns = trace_now_ns() - s.origin_ns;
      slot->sample = sample_process();
      slot->end_ns.store(slot->begin_ns, std::memory_order_relaxed);
      slot->ready.store(true, std::memory_order_release);
    }
    lock.lock();
    s.wake.wait_for(lock, std::chrono::milliseconds(s.sample_ms), [&] { return s.stopping; });
  }
}

}  // namespace

i64 trace_now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

#if defined(_WIN32)

namespace {
u64 filetime_100ns(const FILETIME& t) noexcept {
  return (static_cast<u64>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}
}  // namespace

i64 process_start_ns() noexcept {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
  FILETIME now{};
  GetSystemTimePreciseAsFileTime(&now);
  const i64 steady = trace_now_ns();
  const i64 ago = static_cast<i64>(filetime_100ns(now) - filetime_100ns(created)) * 100;
  return ago > 0 ? steady - ago : steady;
}

i64 thread_cpu_ns() noexcept {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
  return static_cast<i64>(filetime_100ns(kernel) + filetime_100ns(user)) * 100;
}

TraceSample sample_process() noexcept {
  TraceSample out;
  HANDLE self = GetCurrentProcess();
  FILETIME created{}, exited{}, kernel{}, user{};
  if (GetProcessTimes(self, &created, &exited, &kernel, &user))
    out.cpu_ns = (filetime_100ns(kernel) + filetime_100ns(user)) * 100;
  IO_COUNTERS io{};
  if (GetProcessIoCounters(self, &io)) {
    out.read_bytes = io.ReadTransferCount;
    out.read_ops = io.ReadOperationCount;
  }
  // Mapped files and the OS's own read-ahead are not in these: they arrive as page faults.
  PROCESS_MEMORY_COUNTERS_EX memory{};
  memory.cb = sizeof(memory);
  if (K32GetProcessMemoryInfo(self, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                              sizeof(memory))) {
    out.working_set_bytes = memory.WorkingSetSize;
    out.private_bytes = memory.PrivateUsage;
    out.page_faults = memory.PageFaultCount;
  }
  return out;
}

namespace {
u64 process_read_bytes() noexcept {
  IO_COUNTERS io{};
  return GetProcessIoCounters(GetCurrentProcess(), &io) ? io.ReadTransferCount : 0;
}
}  // namespace

#elif defined(__linux__)

namespace {
// Reads a small /proc file into `buffer` with no allocation; the length read, 0 on failure.
usize read_proc(const char* path, char* buffer, usize size) noexcept {
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  usize total = 0;
  while (total + 1 < size) {
    const ssize_t n = ::read(fd, buffer + total, size - 1 - total);
    if (n <= 0) break;
    total += static_cast<usize>(n);
  }
  ::close(fd);
  buffer[total] = '\0';
  return total;
}

u64 field_after(const char* text, const char* key) noexcept {
  const char* at = std::strstr(text, key);
  if (at == nullptr) return 0;
  at += std::strlen(key);
  while (*at == ' ' || *at == ':' || *at == '\t')
    ++at;
  u64 value = 0;
  while (*at >= '0' && *at <= '9')
    value = value * 10 + static_cast<u64>(*at++ - '0');
  return value;
}

i64 clock_ns(clockid_t clock) noexcept {
  timespec t{};
  if (clock_gettime(clock, &t) != 0) return 0;
  return static_cast<i64>(t.tv_sec) * 1000000000 + t.tv_nsec;
}
}  // namespace

i64 process_start_ns() noexcept {
  char text[1024];
  if (read_proc("/proc/self/stat", text, sizeof(text)) == 0) return 0;
  // Field 22 (starttime, clock ticks after boot), counted from the last ')' (the name may hold
  // spaces and parentheses).
  const char* at = std::strrchr(text, ')');
  if (at == nullptr) return 0;
  u32 field = 2;
  u64 start_ticks = 0;
  for (++at; *at != '\0' && field < 22; ++at) {
    if (*at == ' ') {
      ++field;
      if (field == 22) {
        ++at;
        while (*at >= '0' && *at <= '9')
          start_ticks = start_ticks * 10 + static_cast<u64>(*at++ - '0');
        break;
      }
    }
  }
  const long hz = sysconf(_SC_CLK_TCK);
  if (hz <= 0 || start_ticks == 0) return 0;
  const i64 since_boot = clock_ns(CLOCK_BOOTTIME);
  const i64 started = static_cast<i64>(start_ticks) * (1000000000 / hz);
  const i64 steady = trace_now_ns();
  return since_boot > started ? steady - (since_boot - started) : steady;
}

i64 thread_cpu_ns() noexcept { return clock_ns(CLOCK_THREAD_CPUTIME_ID); }

TraceSample sample_process() noexcept {
  TraceSample out;
  out.cpu_ns = static_cast<u64>(clock_ns(CLOCK_PROCESS_CPUTIME_ID));
  char text[1024];
  if (read_proc("/proc/self/io", text, sizeof(text)) != 0) {
    out.read_bytes = field_after(text, "rchar");
    out.read_ops = field_after(text, "syscr");
  }
  if (read_proc("/proc/self/statm", text, sizeof(text)) != 0) {
    // size resident shared text lib data dt, in pages
    u64 values[7] = {};
    const char* at = text;
    for (u64& v : values) {
      while (*at == ' ')
        ++at;
      while (*at >= '0' && *at <= '9')
        v = v * 10 + static_cast<u64>(*at++ - '0');
    }
    const u64 page = static_cast<u64>(sysconf(_SC_PAGESIZE));
    out.working_set_bytes = values[1] * page;
    out.private_bytes = values[5] * page;
  }
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0)
    out.page_faults = static_cast<u64>(usage.ru_minflt) + static_cast<u64>(usage.ru_majflt);
  return out;
}

namespace {
u64 process_read_bytes() noexcept {
  char text[1024];
  return read_proc("/proc/self/io", text, sizeof(text)) != 0 ? field_after(text, "rchar") : 0;
}
}  // namespace

#else

i64 process_start_ns() noexcept { return 0; }
i64 thread_cpu_ns() noexcept { return 0; }
TraceSample sample_process() noexcept { return {}; }

namespace {
u64 process_read_bytes() noexcept { return 0; }
}  // namespace

#endif

bool trace_start(const TraceOptions& options) {
  State& s = state();
  if (detail::g_trace_on.load(std::memory_order_relaxed) || s.sampler.joinable()) return false;
  s.capacity = std::max<u32>(options.capacity, 16u);
  s.slots = std::make_unique<Slot[]>(s.capacity);
  s.count.store(0, std::memory_order_relaxed);
  s.dropped.store(0, std::memory_order_relaxed);
  s.refused.store(0, std::memory_order_relaxed);
  s.origin_ns = options.origin_ns;
  s.sample_ms = options.sample_ms;
  s.stopping = false;
  detail::g_trace_on.store(true, std::memory_order_release);
  if (s.sample_ms != 0) s.sampler = std::thread(sampler_main);
  return true;
}

void trace_stop() {
  State& s = state();
  detail::g_trace_on.store(false, std::memory_order_release);
  if (s.sampler.joinable()) {
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      s.stopping = true;
    }
    s.wake.notify_all();
    s.sampler.join();
  }
}

void trace_reset() {
  trace_stop();
  State& s = state();
  s.slots.reset();
  s.capacity = 0;
  s.count.store(0, std::memory_order_relaxed);
  t_current = k_trace_none;
}

i64 trace_origin_ns() noexcept { return state().origin_ns; }

u32 trace_begin(const char* name, std::string_view detail) noexcept {
  if (!trace_enabled()) return k_trace_none;
  u32 id = k_trace_none;
  Slot* slot = claim(id);
  if (slot == nullptr) return k_trace_none;
  slot->name = name != nullptr ? name : "";
  slot->kind = TraceKind::stage;
  slot->thread = this_thread_index();
  slot->parent = t_current;
  copy_detail(slot->detail, detail);
  slot->read_begin = process_read_bytes();
  slot->cpu_begin = thread_cpu_ns();
  slot->begin_ns = trace_now_ns() - state().origin_ns;
  slot->ready.store(true, std::memory_order_release);
  t_current = id;
  return id;
}

void trace_end(u32 stage) noexcept {
  State& s = state();
  if (stage == k_trace_none || !s.slots || stage >= s.capacity) return;
  Slot& slot = s.slots[stage];
  if (slot.end_ns.load(std::memory_order_relaxed) >= 0) return;
  const i64 end = trace_now_ns() - s.origin_ns;
  slot.cpu_ns = thread_cpu_ns() - slot.cpu_begin;
  const u64 read = process_read_bytes();
  slot.read_bytes = read >= slot.read_begin ? read - slot.read_begin : 0;
  slot.end_ns.store(end, std::memory_order_release);
  if (t_current == stage) t_current = slot.parent;
}

bool trace_depends(u32 stage, u32 on) noexcept {
  State& s = state();
  if (stage == k_trace_none || on == k_trace_none || !s.slots || stage >= s.capacity ||
      on >= stage) {
    if (stage != k_trace_none && on != k_trace_none)
      s.refused.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  Slot& slot = s.slots[stage];
  const u32 at = slot.dep_count.fetch_add(1, std::memory_order_relaxed);
  if (at >= k_trace_max_deps) {
    slot.dep_count.store(k_trace_max_deps, std::memory_order_relaxed);
    s.refused.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  slot.deps[at].store(on, std::memory_order_relaxed);
  return true;
}

void trace_wait(i64 ns) noexcept {
  if (t_current == k_trace_none || ns <= 0) return;
  State& s = state();
  if (!s.slots || t_current >= s.capacity) return;
  s.slots[t_current].wait_ns.fetch_add(ns, std::memory_order_relaxed);
}

void trace_items(u64 n) noexcept {
  if (t_current == k_trace_none) return;
  State& s = state();
  if (!s.slots || t_current >= s.capacity) return;
  s.slots[t_current].items.fetch_add(n, std::memory_order_relaxed);
}

u32 trace_current() noexcept { return t_current; }

u32 trace_milestone(const char* name, std::string_view detail) noexcept {
  return trace_milestone_at(name, trace_now_ns(), detail);
}

u32 trace_milestone_at(const char* name, i64 at_ns, std::string_view detail) noexcept {
  if (!trace_enabled()) return k_trace_none;
  u32 id = k_trace_none;
  Slot* slot = claim(id);
  if (slot == nullptr) return k_trace_none;
  slot->name = name != nullptr ? name : "";
  slot->kind = TraceKind::milestone;
  slot->thread = this_thread_index();
  slot->parent = t_current;
  copy_detail(slot->detail, detail);
  slot->begin_ns = at_ns - state().origin_ns;
  slot->end_ns.store(slot->begin_ns, std::memory_order_relaxed);
  slot->sample = sample_process();
  slot->ready.store(true, std::memory_order_release);
  return id;
}

void trace_snapshot(Vector<TraceEvent>& out) {
  out.clear();
  State& s = state();
  if (!s.slots) return;
  const u32 n = std::min(s.count.load(std::memory_order_acquire), s.capacity);
  out.reserve(n);
  for (u32 i = 0; i < n; ++i) {
    const Slot& slot = s.slots[i];
    if (!slot.ready.load(std::memory_order_acquire)) continue;
    TraceEvent e;
    e.name = slot.name;
    e.kind = slot.kind;
    e.id = i;
    e.thread = slot.thread;
    e.parent = slot.parent;
    e.begin_ns = slot.begin_ns;
    e.end_ns = slot.end_ns.load(std::memory_order_acquire);
    if (e.end_ns >= 0) {
      e.cpu_ns = slot.cpu_ns;
      e.read_bytes = slot.read_bytes;
    }
    e.wait_ns = slot.wait_ns.load(std::memory_order_relaxed);
    e.items = slot.items.load(std::memory_order_relaxed);
    e.dep_count = std::min(slot.dep_count.load(std::memory_order_relaxed), k_trace_max_deps);
    for (u32 d = 0; d < e.dep_count; ++d)
      e.deps[d] = slot.deps[d].load(std::memory_order_relaxed);
    e.sample = slot.sample;
    std::memcpy(e.detail, slot.detail, sizeof(e.detail));
    out.push_back(e);
  }
}

u32 trace_dropped() noexcept { return state().dropped.load(std::memory_order_relaxed); }
u32 trace_refused_edges() noexcept { return state().refused.load(std::memory_order_relaxed); }

bool trace_acyclic(std::span<const TraceEvent> events) noexcept {
  // Every edge must point at an event that exists and has a smaller id; ids are allocated in
  // begin order, so no path can return to where it started.
  for (const TraceEvent& e : events) {
    for (u32 d = 0; d < e.dep_count && d < k_trace_max_deps; ++d) {
      const u32 on = e.deps[d];
      if (on >= e.id) return false;
      bool found = false;
      for (const TraceEvent& other : events) {
        if (other.id == on) {
          found = true;
          break;
        }
      }
      if (!found) return false;
    }
  }
  return true;
}

namespace {

void append_escaped(std::string& out, std::string_view text) {
  out += '"';
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

void append_us(std::string& out, i64 ns) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.3f", static_cast<f64>(ns) / 1000.0);
  out += buf;
}

void append_u64(std::string& out, u64 v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
  out += buf;
}

void append_i64(std::string& out, i64 v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
  out += buf;
}

void append_f64(std::string& out, f64 v) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.6g", v);
  out += buf;
}

const TraceEvent* find_event(std::span<const TraceEvent> events, u32 id) noexcept {
  // Snapshots are in id order; a binary search, then a scan for anything else.
  usize lo = 0, hi = events.size();
  while (lo < hi) {
    const usize mid = (lo + hi) / 2;
    if (events[mid].id < id)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < events.size() && events[lo].id == id) return &events[lo];
  for (const TraceEvent& e : events)
    if (e.id == id) return &e;
  return nullptr;
}

constexpr f64 k_mib = 1024.0 * 1024.0;

}  // namespace

std::string chrome_trace_json(std::span<const TraceEvent> events, i64 origin_ns, u32 dropped) {
  std::string out;
  out.reserve(256 + events.size() * 220);
  out += "{\"traceEvents\":[\n";
  bool first = true;
  auto sep = [&] {
    if (!first) out += ",\n";
    first = false;
  };
  u32 threads = 0;
  for (const TraceEvent& e : events)
    threads = std::max(threads, e.thread + 1);
  for (u32 t = 0; t < threads; ++t) {
    sep();
    out += "{\"ph\":\"M\",\"name\":\"thread_name\",\"pid\":1,\"tid\":";
    append_u64(out, t);
    out += ",\"args\":{\"name\":\"";
    if (t == 0)
      out += "main";
    else {
      out += "thread ";
      append_u64(out, t);
    }
    out += "\"}}";
  }
  const TraceEvent* previous_sample = nullptr;
  u64 flow = 0;
  for (const TraceEvent& e : events) {
    sep();
    if (e.kind == TraceKind::stage) {
      out += "{\"ph\":\"X\",\"cat\":\"startup\",\"name\":";
      append_escaped(out, e.name);
      out += ",\"pid\":1,\"tid\":";
      append_u64(out, e.thread);
      out += ",\"ts\":";
      append_us(out, e.begin_ns);
      out += ",\"dur\":";
      append_us(out, e.duration_ns());
      out += ",\"args\":{\"id\":";
      append_u64(out, e.id);
      out += ",\"parent\":";
      if (e.parent == k_trace_none)
        out += "null";
      else
        append_u64(out, e.parent);
      out += ",\"open\":";
      out += e.end_ns < 0 ? "true" : "false";
      out += ",\"cpu_ns\":";
      append_i64(out, e.cpu_ns);
      out += ",\"wait_ns\":";
      append_i64(out, e.wait_ns);
      out += ",\"read_bytes\":";
      append_u64(out, e.read_bytes);
      out += ",\"items\":";
      append_u64(out, e.items);
      out += ",\"depends\":[";
      for (u32 d = 0; d < e.dep_count; ++d) {
        if (d != 0) out += ',';
        append_u64(out, e.deps[d]);
      }
      out += "],\"detail\":";
      append_escaped(out, e.detail);
      out += "}}";
      // The edges as flow arrows, from the end of what was waited for to this stage's start.
      for (u32 d = 0; d < e.dep_count; ++d) {
        const TraceEvent* on = find_event(events, e.deps[d]);
        if (on == nullptr) continue;
        const i64 from = on->end_ns >= 0 ? std::max(on->begin_ns, on->end_ns - 1) : on->begin_ns;
        out += ",\n{\"ph\":\"s\",\"cat\":\"dependency\",\"name\":\"waits for\",\"id\":";
        append_u64(out, flow);
        out += ",\"pid\":1,\"tid\":";
        append_u64(out, on->thread);
        out += ",\"ts\":";
        append_us(out, from);
        out +=
            "},\n{\"ph\":\"f\",\"bp\":\"e\",\"cat\":\"dependency\",\"name\":\"waits for\",\"id\":";
        append_u64(out, flow);
        out += ",\"pid\":1,\"tid\":";
        append_u64(out, e.thread);
        out += ",\"ts\":";
        append_us(out, e.begin_ns);
        out += '}';
        ++flow;
      }
    } else if (e.kind == TraceKind::milestone) {
      out += "{\"ph\":\"i\",\"s\":\"g\",\"cat\":\"milestone\",\"name\":";
      append_escaped(out, e.name);
      out += ",\"pid\":1,\"tid\":";
      append_u64(out, e.thread);
      out += ",\"ts\":";
      append_us(out, e.begin_ns);
      out += ",\"args\":{\"id\":";
      append_u64(out, e.id);
      out += ",\"detail\":";
      append_escaped(out, e.detail);
      out += ",\"working_set_mib\":";
      append_f64(out, static_cast<f64>(e.sample.working_set_bytes) / k_mib);
      out += ",\"private_mib\":";
      append_f64(out, static_cast<f64>(e.sample.private_bytes) / k_mib);
      out += ",\"read_mib\":";
      append_f64(out, static_cast<f64>(e.sample.read_bytes) / k_mib);
      out += "}}";
    } else {
      // A counter track: rates over the interval since the last sample, and the levels.
      f64 cores = 0.0, read_rate = 0.0, faults_rate = 0.0;
      if (previous_sample != nullptr && e.begin_ns > previous_sample->begin_ns) {
        const f64 dt = static_cast<f64>(e.begin_ns - previous_sample->begin_ns);
        cores = static_cast<f64>(e.sample.cpu_ns -
                                 std::min(e.sample.cpu_ns, previous_sample->sample.cpu_ns)) /
                dt;
        read_rate =
            static_cast<f64>(e.sample.read_bytes -
                             std::min(e.sample.read_bytes, previous_sample->sample.read_bytes)) /
            k_mib / (dt * 1e-9);
        faults_rate =
            static_cast<f64>(e.sample.page_faults -
                             std::min(e.sample.page_faults, previous_sample->sample.page_faults)) /
            (dt * 1e-9);
      }
      previous_sample = &e;
      out += "{\"ph\":\"C\",\"name\":\"cpu cores\",\"pid\":1,\"ts\":";
      append_us(out, e.begin_ns);
      out += ",\"args\":{\"cores\":";
      append_f64(out, cores);
      out += "}},\n{\"ph\":\"C\",\"name\":\"read MiB/s\",\"pid\":1,\"ts\":";
      append_us(out, e.begin_ns);
      out += ",\"args\":{\"read\":";
      append_f64(out, read_rate);
      out += "}},\n{\"ph\":\"C\",\"name\":\"page faults/s\",\"pid\":1,\"ts\":";
      append_us(out, e.begin_ns);
      out += ",\"args\":{\"faults\":";
      append_f64(out, faults_rate);
      out += "}},\n{\"ph\":\"C\",\"name\":\"memory MiB\",\"pid\":1,\"ts\":";
      append_us(out, e.begin_ns);
      out += ",\"args\":{\"working_set\":";
      append_f64(out, static_cast<f64>(e.sample.working_set_bytes) / k_mib);
      out += ",\"private\":";
      append_f64(out, static_cast<f64>(e.sample.private_bytes) / k_mib);
      out += "}}";
    }
  }
  out +=
      "\n],\"displayTimeUnit\":\"ms\",\"otherData\":{\"format\":\"engine.startup_trace.v1\","
      "\"origin\":\"process launch\",\"origin_ns\":";
  append_i64(out, origin_ns);
  out += ",\"dropped\":";
  append_u64(out, dropped);
  out += "}}\n";
  return out;
}

bool write_chrome_trace(const std::string& path, std::span<const TraceEvent> events, i64 origin_ns,
                        u32 dropped, std::string* error) {
  const std::string text = chrome_trace_json(events, origin_ns, dropped);
  std::FILE* f = nullptr;
#if defined(_WIN32)
  if (fopen_s(&f, path.c_str(), "wb") != 0) f = nullptr;
#else
  f = std::fopen(path.c_str(), "wb");
#endif
  if (f == nullptr) {
    if (error != nullptr) *error = "cannot open " + path + " for writing";
    return false;
  }
  const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  const bool closed = std::fclose(f) == 0;
  if (!(ok && closed)) {
    if (error != nullptr) *error = "cannot write " + path;
    return false;
  }
  return true;
}

namespace {

f64 ms_of(i64 ns) { return static_cast<f64>(ns) / 1.0e6; }

// The last sample at or before `at`, or the first one after it; null with none.
const TraceSample* sample_at(std::span<const TraceEvent> events, i64 at) noexcept {
  const TraceSample* best = nullptr;
  const TraceSample* after = nullptr;
  for (const TraceEvent& e : events) {
    if (e.kind != TraceKind::sample && e.kind != TraceKind::milestone) continue;
    if (e.begin_ns <= at)
      best = &e.sample;
    else if (after == nullptr)
      after = &e.sample;
  }
  return best != nullptr ? best : after;
}

JsonValue process_json(const TraceSample& s) {
  JsonValue o = JsonValue::object();
  o.set("cpu_ms", ms_of(static_cast<i64>(s.cpu_ns)));
  o.set("read_mib", static_cast<f64>(s.read_bytes) / k_mib);
  o.set("read_ops", s.read_ops);
  o.set("working_set_mib", static_cast<f64>(s.working_set_bytes) / k_mib);
  o.set("private_mib", static_cast<f64>(s.private_bytes) / k_mib);
  o.set("page_faults", s.page_faults);
  return o;
}

}  // namespace

JsonValue startup_summary_json(std::span<const TraceEvent> events,
                               std::span<const char* const> milestones, const char* until,
                               u32 dropped) {
  JsonValue out = JsonValue::object();
  out.set("format", "engine.startup.v1");
  out.set("origin", "process launch");
  out.set("dropped_events", dropped);
  // ---- the milestones, monotone in the order given -----------------------------------------
  JsonValue list = JsonValue::array();
  i64 floor_ns = 0;
  i64 until_ns = -1;
  u32 reordered = 0;
  u32 main_thread = k_trace_none;
  for (const char* name : milestones) {
    const TraceEvent* found = nullptr;
    for (const TraceEvent& e : events) {
      if (e.kind == TraceKind::milestone && std::strcmp(e.name, name) == 0) {
        found = &e;
        break;
      }
    }
    JsonValue m = JsonValue::object();
    m.set("name", name);
    if (found == nullptr) {
      m.set("ms", JsonValue::null());
    } else {
      if (main_thread == k_trace_none) main_thread = found->thread;
      i64 at = found->begin_ns;
      if (at < floor_ns) {
        // Reached before a milestone listed ahead of it: it cannot be said to have been reached
        // until that one was (a player is not controllable before the frame that shows them).
        at = floor_ns;
        ++reordered;
        m.set("reached_ms", ms_of(found->begin_ns));
      }
      floor_ns = at;
      m.set("ms", ms_of(at));
      if (found->detail[0] != '\0') m.set("detail", std::string_view(found->detail));
      if (until != nullptr && std::strcmp(name, until) == 0) until_ns = at;
    }
    list.push_back(std::move(m));
  }
  out.set("milestones", std::move(list));
  out.set("milestones_reordered", reordered);
  if (main_thread == k_trace_none) main_thread = 0;
  i64 end_ns = until_ns;
  if (end_ns < 0) {
    for (const TraceEvent& e : events)
      end_ns = std::max(end_ns, std::max(e.begin_ns, e.end_ns));
  }
  out.set("until", until != nullptr ? JsonValue(until) : JsonValue::null());
  out.set("until_reached", until_ns >= 0);
  out.set("until_ms", ms_of(std::max<i64>(end_ns, 0)));
  // ---- the main thread's top-level stages: the serial critical path ------------------------
  JsonValue stages = JsonValue::array();
  i64 staged_ns = 0;
  for (const TraceEvent& e : events) {
    if (e.kind != TraceKind::stage || e.thread != main_thread || e.parent != k_trace_none) continue;
    if (e.begin_ns > end_ns) continue;
    JsonValue s = JsonValue::object();
    s.set("name", e.name);
    if (e.detail[0] != '\0') s.set("detail", std::string_view(e.detail));
    s.set("begin_ms", ms_of(e.begin_ns));
    s.set("ms", ms_of(e.duration_ns()));
    s.set("open", e.end_ns < 0);
    s.set("cpu_ms", ms_of(e.cpu_ns));
    s.set("wait_ms", ms_of(e.wait_ns));
    s.set("read_mib", static_cast<f64>(e.read_bytes) / k_mib);
    if (e.items != 0) s.set("items", e.items);
    staged_ns += std::min(e.end_ns >= 0 ? e.end_ns : end_ns, end_ns) - e.begin_ns;
    // What it nested, longest first (three), and what it waited for anywhere beneath it.
    Vector<const TraceEvent*> children;
    JsonValue waits = JsonValue::array();
    for (const TraceEvent& c : events) {
      if (c.kind != TraceKind::stage || c.id <= e.id) continue;
      // Beneath `e`: walk the parent chain up to it.
      u32 p = c.parent;
      bool beneath = false;
      for (u32 guard = 0; p != k_trace_none && guard < 64; ++guard) {
        if (p == e.id) {
          beneath = true;
          break;
        }
        const TraceEvent* up = find_event(events, p);
        if (up == nullptr) break;
        p = up->parent;
      }
      if (!beneath) continue;
      if (c.parent == e.id) children.push_back(&c);
      for (u32 d = 0; d < c.dep_count; ++d) {
        const TraceEvent* on = find_event(events, c.deps[d]);
        JsonValue w = JsonValue::object();
        w.set("stage", c.name);
        w.set("ms", ms_of(c.duration_ns()));
        w.set("on", on != nullptr ? JsonValue(on->name) : JsonValue::null());
        if (on != nullptr) w.set("on_thread", on->thread);
        waits.push_back(std::move(w));
      }
    }
    std::sort(children.begin(), children.end(), [](const TraceEvent* a, const TraceEvent* b) {
      return a->duration_ns() > b->duration_ns();
    });
    JsonValue inner = JsonValue::array();
    for (u32 i = 0; i < children.size() && i < 3; ++i) {
      JsonValue c = JsonValue::object();
      c.set("name", children[i]->name);
      c.set("ms", ms_of(children[i]->duration_ns()));
      c.set("cpu_ms", ms_of(children[i]->cpu_ns));
      c.set("read_mib", static_cast<f64>(children[i]->read_bytes) / k_mib);
      inner.push_back(std::move(c));
    }
    if (inner.size() != 0) s.set("longest", std::move(inner));
    JsonValue deps = JsonValue::array();
    for (u32 d = 0; d < e.dep_count; ++d) {
      const TraceEvent* on = find_event(events, e.deps[d]);
      if (on != nullptr) deps.push_back(JsonValue(on->name));
    }
    if (deps.size() != 0) s.set("depends", std::move(deps));
    if (waits.size() != 0) s.set("waits", std::move(waits));
    stages.push_back(std::move(s));
  }
  out.set("stages", std::move(stages));
  out.set("unstaged_ms", ms_of(std::max<i64>(end_ns - staged_ns, 0)));
  // ---- the other threads ------------------------------------------------------------------
  u32 threads = 0;
  for (const TraceEvent& e : events)
    threads = std::max(threads, e.thread + 1);
  JsonValue per_thread = JsonValue::array();
  for (u32 t = 0; t < threads; ++t) {
    i64 busy = 0, cpu = 0, wait = 0;
    u32 count = 0;
    for (const TraceEvent& e : events) {
      if (e.kind != TraceKind::stage || e.thread != t || e.begin_ns > end_ns) continue;
      // Top-level on its thread: its parent is none or on another thread (never, by the rule).
      if (e.parent != k_trace_none) continue;
      busy += std::min(e.end_ns >= 0 ? e.end_ns : end_ns, end_ns) - e.begin_ns;
      cpu += e.cpu_ns;
      wait += e.wait_ns;
      ++count;
    }
    if (count == 0) continue;
    JsonValue o = JsonValue::object();
    o.set("thread", t);
    o.set("stages", count);
    o.set("busy_ms", ms_of(busy));
    o.set("cpu_ms", ms_of(cpu));
    o.set("wait_ms", ms_of(wait));
    per_thread.push_back(std::move(o));
  }
  out.set("threads", std::move(per_thread));
  // ---- the process, before ready, from the samples ----------------------------------------
  const TraceSample* first = sample_at(events, 0);
  const TraceSample* last = sample_at(events, end_ns);
  JsonValue before = JsonValue::object();
  if (first != nullptr && last != nullptr) {
    before.set("cpu_ms",
               ms_of(static_cast<i64>(last->cpu_ns - std::min(last->cpu_ns, first->cpu_ns))));
    before.set("read_mib", static_cast<f64>(last->read_bytes) / k_mib);
    before.set("read_ops", last->read_ops);
    before.set("page_faults", last->page_faults);
  }
  // What the samples say the process was doing, interval by interval: on a CPU (at least half a
  // core busy), reading (4 MiB/s or more while less than half a core is busy), or neither —
  // waiting on something that is not this process's CPU or its file reads (the GPU, the driver,
  // a lock, a page fault storm counted on its own).
  f64 cpu_ms = 0.0, io_ms = 0.0, quiet_ms = 0.0, peak_ws = 0.0, peak_private = 0.0;
  const TraceEvent* previous = nullptr;
  for (const TraceEvent& e : events) {
    if (e.kind != TraceKind::sample) continue;
    if (e.begin_ns > end_ns) break;
    peak_ws = std::max(peak_ws, static_cast<f64>(e.sample.working_set_bytes) / k_mib);
    peak_private = std::max(peak_private, static_cast<f64>(e.sample.private_bytes) / k_mib);
    if (previous != nullptr && e.begin_ns > previous->begin_ns) {
      const f64 dt = static_cast<f64>(e.begin_ns - previous->begin_ns);
      const f64 cores =
          static_cast<f64>(e.sample.cpu_ns - std::min(e.sample.cpu_ns, previous->sample.cpu_ns)) /
          dt;
      const f64 rate =
          static_cast<f64>(e.sample.read_bytes -
                           std::min(e.sample.read_bytes, previous->sample.read_bytes)) /
          k_mib / (dt * 1e-9);
      if (cores >= 0.5)
        cpu_ms += dt / 1e6;
      else if (rate >= 4.0)
        io_ms += dt / 1e6;
      else
        quiet_ms += dt / 1e6;
    }
    previous = &e;
  }
  before.set("on_cpu_ms", cpu_ms);
  before.set("reading_ms", io_ms);
  before.set("neither_ms", quiet_ms);
  before.set("peak_working_set_mib", peak_ws);
  before.set("peak_private_mib", peak_private);
  out.set("before_ready", std::move(before));
  // The process at each milestone.
  JsonValue at = JsonValue::object();
  for (const char* name : milestones) {
    for (const TraceEvent& e : events) {
      if (e.kind == TraceKind::milestone && std::strcmp(e.name, name) == 0) {
        at.set(name, process_json(e.sample));
        break;
      }
    }
  }
  out.set("process_at", std::move(at));
  return out;
}

}  // namespace engine::profiling

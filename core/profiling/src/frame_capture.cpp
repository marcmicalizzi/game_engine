#include <core/profiling/frame_capture.h>
#include <core/profiling/startup_trace.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <thread>

// The buffer is a plain array of slots and a claim counter; an event is written in place and then
// published by its slot's `ready` word, so a snapshot taken while recording copies only what is
// whole. `capture_stop` turns the switch off and then waits for the writers in flight: every
// writer enters by bumping `g_writers` before it reads the switch again (sequentially consistent
// on both sides, the store-load order a Dekker pair needs), so once the count reads zero after the
// switch is off, nobody can still be inside the buffer and `capture_reset` may free it.

namespace engine::profiling {

namespace detail {
std::atomic<bool> g_capture_on{false};
}  // namespace detail

namespace {

struct Slot {
  CaptureEvent event;
  std::atomic<u32> ready{0};  // the generation that published it
};

std::unique_ptr<Slot[]> g_slots;
u32 g_capacity = 0;
std::atomic<u64> g_claimed{0};
std::atomic<u64> g_dropped{0};
std::atomic<u32> g_generation{0};  // 0 is never a capture's
std::atomic<u32> g_writers{0};
std::atomic<u32> g_threads{0};
i64 g_origin_ns = 0;
std::atomic<i64> g_stopped_ns{0};
std::atomic<i64> g_full_ns{-1};  // when the first event was refused, from the origin; -1: never
char g_thread_names[k_capture_max_threads][k_capture_thread_name_bytes] = {};

struct ThreadState {
  u32 generation = 0;  // the capture the index below belongs to
  u16 index = 0;
  char name[k_capture_thread_name_bytes] = {};
};
thread_local ThreadState t_thread;

void copy_name(char* to, const char* from) noexcept {
  usize n = 0;
  if (from != nullptr)
    while (n + 1 < k_capture_thread_name_bytes && from[n] != '\0') {
      to[n] = from[n];
      ++n;
    }
  to[n] = '\0';
}

// The calling thread's index in capture `generation`, given one on its first event.
u16 thread_index(u32 generation) noexcept {
  ThreadState& t = t_thread;
  if (t.generation != generation) {
    const u32 index = g_threads.fetch_add(1, std::memory_order_relaxed);
    const u32 kept = std::min(index, k_capture_max_threads - 1);  // the last index is shared
    if (index < k_capture_max_threads) copy_name(g_thread_names[index], t.name);
    t.index = static_cast<u16>(kept);
    t.generation = generation;
  }
  return t.index;
}

// Claims a slot and writes `event` into it, or counts a drop. The caller holds a writer's entry.
void publish(const CaptureEvent& event, u32 generation) noexcept {
  const u64 at = g_claimed.fetch_add(1, std::memory_order_relaxed);
  if (at >= g_capacity) {
    // The one claim that finds the buffer just full stamps the time, after its claim: every event
    // recorded was stamped before its own (earlier) claim, so none is later than this. The report
    // ends the capture's whole frames here (profiler.md, "A capture that filled").
    if (at == g_capacity) g_full_ns.store(trace_now_ns() - g_origin_ns, std::memory_order_relaxed);
    g_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Slot& slot = g_slots[static_cast<usize>(at)];
  slot.event = event;
  slot.event.thread = thread_index(generation);
  slot.ready.store(generation, std::memory_order_release);
}

// A writer's entry: true when the capture `generation` is still on, and then the caller must call
// `leave`. With `generation` 0, any running capture.
bool enter(u32& generation) noexcept {
  g_writers.fetch_add(1, std::memory_order_seq_cst);
  // The switch first, then the generation: `capture_start` stores the generation and then the
  // switch, so a writer that sees the switch on reads that start's generation or a later one.
  // Read the other way round, a writer racing a start could pass with the old generation and
  // publish an event the new capture never reads (a lost frame mark) under a stale thread index
  // (a phantom thread), as `capture_zone_begin_check` already avoids.
  const bool on = detail::g_capture_on.load(std::memory_order_seq_cst);
  const u32 current = g_generation.load(std::memory_order_seq_cst);
  if (!on || (generation != 0 && generation != current)) {
    g_writers.fetch_sub(1, std::memory_order_release);
    return false;
  }
  generation = current;
  return true;
}

void leave() noexcept { g_writers.fetch_sub(1, std::memory_order_release); }

}  // namespace

namespace detail {

i64 capture_clock_ns() noexcept { return trace_now_ns(); }

u32 capture_zone_begin_check() noexcept {
  if (!g_capture_on.load(std::memory_order_acquire)) return 0;
  return g_generation.load(std::memory_order_acquire);
}

void capture_zone_end(const char* name, i64 begin_ns, u32 generation) noexcept {
  const i64 end_ns = trace_now_ns();
  if (!enter(generation)) return;
  CaptureEvent e;
  e.name = name != nullptr ? name : "";
  e.kind = CaptureKind::zone;
  e.begin_ns = begin_ns - g_origin_ns;
  e.end_ns = end_ns - g_origin_ns;
  publish(e, generation);
  leave();
}

}  // namespace detail

bool capture_start(const CaptureOptions& options) {
  if (detail::g_capture_on.load(std::memory_order_acquire)) return false;
  // No writer can be inside: the last stop waited for them, and none enters while it is off.
  const u32 capacity = std::max<u32>(options.capacity, 1);
  if (g_capacity < capacity || !g_slots) {
    g_slots.reset(new Slot[capacity]);
    g_capacity = capacity;
  }
  g_claimed.store(0, std::memory_order_relaxed);
  g_dropped.store(0, std::memory_order_relaxed);
  g_threads.store(0, std::memory_order_relaxed);
  std::memset(g_thread_names, 0, sizeof(g_thread_names));
  g_origin_ns = trace_now_ns();
  g_stopped_ns.store(0, std::memory_order_relaxed);
  g_full_ns.store(-1, std::memory_order_relaxed);
  u32 next = g_generation.load(std::memory_order_relaxed) + 1;
  if (next == 0) next = 1;
  g_generation.store(next, std::memory_order_seq_cst);
  detail::g_capture_on.store(true, std::memory_order_seq_cst);
  return true;
}

void capture_stop() noexcept {
  if (!detail::g_capture_on.load(std::memory_order_acquire)) return;
  g_stopped_ns.store(trace_now_ns() - g_origin_ns, std::memory_order_relaxed);
  detail::g_capture_on.store(false, std::memory_order_seq_cst);
  while (g_writers.load(std::memory_order_seq_cst) != 0)
    std::this_thread::yield();
}

void capture_reset() noexcept {
  capture_stop();
  g_slots.reset();
  g_capacity = 0;
  g_claimed.store(0, std::memory_order_relaxed);
  g_dropped.store(0, std::memory_order_relaxed);
  g_threads.store(0, std::memory_order_relaxed);
}

namespace {
void frame_event(CaptureKind kind, u64 index) noexcept {
  if (!capture_enabled()) return;
  u32 generation = 0;
  if (!enter(generation)) return;
  CaptureEvent e;
  e.name = kind == CaptureKind::frame ? "frame" : "frame end";
  e.kind = kind;
  e.frame = index;
  e.begin_ns = trace_now_ns() - g_origin_ns;
  e.end_ns = e.begin_ns;
  publish(e, generation);
  leave();
}
}  // namespace

void capture_frame(u64 index) noexcept { frame_event(CaptureKind::frame, index); }
void capture_frame_end(u64 index) noexcept { frame_event(CaptureKind::frame_end, index); }

void capture_value(CaptureTrack track, const char* name, u64 index, f64 ms) noexcept {
  if (!capture_enabled()) return;
  u32 generation = 0;
  if (!enter(generation)) return;
  CaptureEvent e;
  e.name = name != nullptr ? name : "";
  e.kind = CaptureKind::value;
  e.track = track;
  e.frame = index;
  e.ms = ms;
  e.begin_ns = trace_now_ns() - g_origin_ns;
  e.end_ns = e.begin_ns;
  publish(e, generation);
  leave();
}

void capture_name_thread(const char* name) noexcept {
  ThreadState& t = t_thread;
  copy_name(t.name, name);
  // Named after its first event of a running capture: the table follows.
  u32 generation = t.generation;
  if (generation == 0 || !enter(generation)) return;
  if (t.index < k_capture_max_threads - 1) copy_name(g_thread_names[t.index], t.name);
  leave();
}

u32 capture_snapshot(std::span<CaptureEvent> out, CaptureInfo& info) noexcept {
  const bool running = detail::g_capture_on.load(std::memory_order_acquire);
  const u32 generation = g_generation.load(std::memory_order_acquire);
  const u64 claimed = std::min<u64>(g_claimed.load(std::memory_order_acquire), g_capacity);
  u32 n = 0;
  for (u64 i = 0; i < claimed && n < out.size(); ++i) {
    const Slot& slot = g_slots[static_cast<usize>(i)];
    if (slot.ready.load(std::memory_order_acquire) != generation) continue;
    out[n++] = slot.event;
  }
  info.origin_ns = g_origin_ns;
  info.stopped_ns =
      running ? trace_now_ns() - g_origin_ns : g_stopped_ns.load(std::memory_order_relaxed);
  info.dropped = g_dropped.load(std::memory_order_relaxed);
  info.full_ns = g_full_ns.load(std::memory_order_relaxed);
  info.threads = std::min(g_threads.load(std::memory_order_relaxed), k_capture_max_threads);
  info.running = running;
  return n;
}

u32 capture_count() noexcept {
  return static_cast<u32>(std::min<u64>(g_claimed.load(std::memory_order_acquire), g_capacity));
}

const char* capture_thread_name(u32 thread) noexcept {
  return thread < k_capture_max_threads ? g_thread_names[thread] : "";
}

}  // namespace engine::profiling

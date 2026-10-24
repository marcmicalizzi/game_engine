#pragma once

// The startup trace (docs/subsystems/profiling.md, "The startup trace"): what a process did between
// its launch and the moment it was ready, as named stages on the threads that ran them — wall time,
// the thread's own CPU time over it, the bytes the process read while it ran, the time it waited
// and on what — plus milestones and the process's own counters sampled beside them. It is written
// as Chrome trace JSON (loadable in chrome://tracing and Perfetto) and summarized as a JSON block.
//
// **Off, it costs one relaxed atomic load per stage** and allocates nothing: every entry point
// returns at once while `trace_enabled()` is false, which it is until `trace_start`. On, the
// events go into a buffer allocated once by `trace_start` (a stage, a milestone, a sample each
// take one slot; a full buffer counts what it dropped), so recording never allocates and a frame
// loop with the trace on allocates nothing either.
//
// **Stages nest per thread and name what they waited for.** A stage's parent is the stage open on
// its thread when it began; a dependency edge (`trace_depends`, or a `TraceWait` scope) points from
// a stage to one that *began before it* — refused otherwise — so the graph is acyclic by
// construction and a summary that walks it never sums two overlapping stages as if one followed
// the other.
//
// Names are string literals (static storage): the trace keeps the pointer. A stage's detail is
// copied (`k_trace_detail_bytes`, truncated), so a path or a reason may come from anywhere.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/profiling/profile.h>  // ENGINE_PROFILING

#include <atomic>
#include <span>
#include <string>
#include <string_view>

namespace engine::profiling {

inline constexpr u32 k_trace_none = ~0u;
inline constexpr u32 k_trace_max_deps = 6;
inline constexpr u32 k_trace_detail_bytes = 96;
inline constexpr u32 k_trace_default_capacity = 1u << 15;

enum class TraceKind : u8 { stage, milestone, sample };

// One sample of the process (the sampler thread's, every `TraceOptions::sample_ms`): what a flat
// memory curve cannot say on its own — CPU busy or idle, bytes read, pages faulted.
struct TraceSample {
  u64 cpu_ns = 0;      // process user + kernel time, summed over threads
  u64 read_bytes = 0;  // bytes the process read through the OS (files, cumulative)
  u64 read_ops = 0;    // read calls (cumulative)
  u64 working_set_bytes = 0;
  u64 private_bytes = 0;  // committed
  u64 page_faults = 0;    // cumulative, hard and soft
};

// An event as `trace_snapshot` returns it. Times are nanoseconds from the trace's origin.
struct TraceEvent {
  const char* name = "";
  TraceKind kind = TraceKind::stage;
  u32 id = k_trace_none;
  u32 thread = 0;             // the trace's own thread index, in order of first use (0 = first)
  u32 parent = k_trace_none;  // the stage open on this thread when this one began
  i64 begin_ns = 0;
  i64 end_ns = -1;     // -1: still open at the snapshot
  i64 cpu_ns = 0;      // the thread's CPU time over the stage
  i64 wait_ns = 0;     // time the stage reported blocked (`trace_wait`, `TraceWait`)
  u64 read_bytes = 0;  // the process's OS reads over the stage (all threads)
  u64 items = 0;       // a count the stage reported (`trace_items`)
  u32 deps[k_trace_max_deps] = {};
  u32 dep_count = 0;
  TraceSample sample;  // kind == sample
  char detail[k_trace_detail_bytes] = {};

  i64 duration_ns() const noexcept { return end_ns >= begin_ns ? end_ns - begin_ns : 0; }
};

struct TraceOptions {
  // The time zero of every event, on `trace_now_ns`'s clock: the process's launch
  // (`process_start_ns`) for a startup trace.
  i64 origin_ns = 0;
  u32 capacity = k_trace_default_capacity;
  u32 sample_ms = 20;  // 0: no sampler thread
};

namespace detail {
extern std::atomic<bool> g_trace_on;
}

inline bool trace_enabled() noexcept { return detail::g_trace_on.load(std::memory_order_relaxed); }

// The clock every event is on (std::chrono::steady_clock, the same as `time::monotonic_ns`).
i64 trace_now_ns() noexcept;
// When the OS says this process was created, on `trace_now_ns`'s clock; 0 where it cannot say.
i64 process_start_ns() noexcept;
// The calling thread's CPU time (user + kernel); 0 where the platform cannot say.
i64 thread_cpu_ns() noexcept;
// The process's counters now (`TraceSample`); zeros where the platform cannot say.
TraceSample sample_process() noexcept;

// Starts recording: allocates the buffer and starts the sampler. False if already on.
bool trace_start(const TraceOptions& options = {});
// Stops recording and joins the sampler; the events stay readable until `trace_reset`.
void trace_stop();
// Frees the buffer. Recording must be stopped and no stage may be open on any thread.
void trace_reset();
i64 trace_origin_ns() noexcept;

// A stage begins: its id, or `k_trace_none` when the trace is off or full. `detail` is copied.
u32 trace_begin(const char* name, std::string_view detail = {}) noexcept;
void trace_end(u32 stage) noexcept;
// `stage` waited for `on`; refused (false) unless `on` began before `stage` — the edge that keeps
// the graph acyclic — or when either is `k_trace_none`, or the stage has `k_trace_max_deps` edges.
bool trace_depends(u32 stage, u32 on) noexcept;
// Blocked time and items, added to this thread's innermost open stage (nothing when none is open).
void trace_wait(i64 ns) noexcept;
void trace_items(u64 n) noexcept;
// The stage open on this thread, or `k_trace_none`.
u32 trace_current() noexcept;
// An instant: its id. A summary reads the first milestone of each name.
u32 trace_milestone(const char* name, std::string_view detail = {}) noexcept;
// The same at a time already past (`trace_now_ns`'s clock): a milestone that is only known once
// it has held for a while ("stable for N frames") is dated from when it began to hold, and one
// reached before the trace started (the process's `main`) from when that was.
u32 trace_milestone_at(const char* name, i64 at_ns, std::string_view detail = {}) noexcept;

// What was recorded (any time; a stage another thread has not ended reads as open).
void trace_snapshot(Vector<TraceEvent>& out);
u32 trace_dropped() noexcept;
u32 trace_refused_edges() noexcept;

// Whether every edge in `events` points at an event that began earlier and exists — the graph
// `trace_depends` promises is acyclic. A trace read back from elsewhere is checked by the same.
bool trace_acyclic(std::span<const TraceEvent> events) noexcept;
// The Chrome trace JSON (the "JSON Array/Object Format": complete events, instants, counters,
// flow arrows for the dependency edges), with every field above in each event's `args`.
std::string chrome_trace_json(std::span<const TraceEvent> events, i64 origin_ns, u32 dropped = 0);
bool write_chrome_trace(const std::string& path, std::span<const TraceEvent> events, i64 origin_ns,
                        u32 dropped, std::string* error);

// **The summary** (apps.md, "--startup-trace"): `milestones` in the order given, each the first
// milestone of that name in milliseconds from the origin (null when never reached), required to be
// monotone in that order — a later milestone reached before an earlier one is reported at the
// earlier one's time and counted in `milestones_reordered`; the top-level stages of the thread
// that recorded the first milestone (its serial critical path) with wall, CPU, read bytes, waits,
// their dependencies' names and the stages nested in them that took longest; the other threads'
// busy time; the process's counters at each milestone. `until` names the milestone the
// "before ready" totals stop at.
JsonValue startup_summary_json(std::span<const TraceEvent> events,
                               std::span<const char* const> milestones, const char* until,
                               u32 dropped = 0);

// A scoped stage, and the Tracy zone of the same name in a Tracy build.
class TraceStage {
 public:
  explicit TraceStage(const char* name, std::string_view detail = {}) noexcept
      : id_(trace_enabled() ? trace_begin(name, detail) : k_trace_none) {}
  ~TraceStage() {
    if (id_ != k_trace_none) trace_end(id_);
  }
  TraceStage(const TraceStage&) = delete;
  TraceStage& operator=(const TraceStage&) = delete;
  u32 id() const noexcept { return id_; }
  void depends(u32 on) noexcept {
    if (id_ != k_trace_none) (void)trace_depends(id_, on);
  }
  // Ends the stage before the scope does.
  void end() noexcept {
    if (id_ != k_trace_none) trace_end(id_);
    id_ = k_trace_none;
  }

 private:
  u32 id_;
};

// A wait: a stage named `name` that depends on `on`, whose whole duration counts as the enclosing
// stage's wait time.
class TraceWait {
 public:
  TraceWait(const char* name, u32 on) noexcept
      : start_(trace_enabled() ? trace_now_ns() : 0),
        id_(trace_enabled() ? trace_begin(name) : k_trace_none) {
    if (id_ != k_trace_none) (void)trace_depends(id_, on);
  }
  ~TraceWait() {
    if (id_ == k_trace_none) return;
    const i64 waited = trace_now_ns() - start_;
    trace_end(id_);
    trace_wait(waited);
  }
  TraceWait(const TraceWait&) = delete;
  TraceWait& operator=(const TraceWait&) = delete;

 private:
  i64 start_;
  u32 id_;
};

}  // namespace engine::profiling

#define ENGINE_TRACE_CONCAT_INNER(a, b) a##b
#define ENGINE_TRACE_CONCAT(a, b) ENGINE_TRACE_CONCAT_INNER(a, b)
// The Tracy zone beside a stage, named per line so that two stages may share a scope.
#if ENGINE_PROFILING
#define ENGINE_STARTUP_TRACY_ZONE(name) \
  ZoneNamedN(ENGINE_TRACE_CONCAT(engine_startup_zone_, __LINE__), name, true)
#else
#define ENGINE_STARTUP_TRACY_ZONE(name) ((void)0)
#endif
// A startup stage for the rest of the scope, with a Tracy zone of the same literal name.
#define ENGINE_STARTUP_STAGE(name) \
  ENGINE_STARTUP_TRACY_ZONE(name); \
  ::engine::profiling::TraceStage ENGINE_TRACE_CONCAT(engine_startup_stage_, __LINE__)(name)
#define ENGINE_STARTUP_STAGE_DETAIL(name, detail) \
  ENGINE_STARTUP_TRACY_ZONE(name);                \
  ::engine::profiling::TraceStage ENGINE_TRACE_CONCAT(engine_startup_stage_, __LINE__)(name, detail)

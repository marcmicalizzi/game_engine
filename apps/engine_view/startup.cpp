#include "startup.h"

#include <core/profiling/startup_trace.h>

#include <cstring>

namespace engine::view {

namespace {
u32 milestone_index(const char* name) noexcept {
  for (u32 i = 0; i < k_startup_milestone_count; ++i)
    if (std::strcmp(k_startup_milestones[i], name) == 0) return i;
  return k_startup_milestone_count;
}
}  // namespace

StartupTrace& startup_trace() {
  static StartupTrace trace;
  return trace;
}

bool StartupTrace::start(const std::string& path, i64 main_ns, std::string* error) {
  profiling::TraceOptions options;
  const i64 launched = profiling::process_start_ns();
  // Where the OS cannot say when the process was made, the trace starts at `main`.
  options.origin_ns = launched > 0 && launched <= main_ns ? launched : main_ns;
  if (!profiling::trace_start(options)) {
    if (error != nullptr) *error = "the startup trace is already recording";
    return false;
  }
  on_ = true;
  path_ = path;
  (void)profiling::trace_milestone_at("main", main_ns);
  reached_[0] = true;
  return true;
}

void StartupTrace::milestone(const char* name) noexcept {
  // The live progress (W79) follows the milestones whether or not the trace records them.
  if (!on_ && !profiling::progress_enabled()) return;
  const u32 i = milestone_index(name);
  if (i < k_startup_milestone_count) {
    if (reached_[i]) return;
    reached_[i] = true;
  }
  if (on_) {
    (void)profiling::trace_milestone(name);  // which feeds the progress too
  } else {
    profiling::progress_milestone(name);
  }
}

bool StartupTrace::reached(const char* name) const noexcept {
  const u32 i = milestone_index(name);
  return i < k_startup_milestone_count && reached_[i];
}

void StartupTrace::frame(const Readiness& r) noexcept {
  if (!on_ && !profiling::progress_enabled()) return;
  if (r.frame_done) milestone("first_frame");
  if (r.frame_done && r.controllable) milestone("player_controllable");
  if (r.frame_done && r.collision) milestone("ground_collision_ready");
  const u32 stable = milestone_index("nearby_stable");
  if (reached_[stable]) return;
  const bool holds = r.frame_done && r.controllable && r.collision && r.world_settled &&
                     r.pages_settled && r.ground_settled;
  if (!holds) {
    stable_ = 0;
    return;
  }
  if (stable_ == 0) stable_since_ = profiling::trace_now_ns();
  if (++stable_ == k_stable_frames) {
    reached_[stable] = true;
    if (on_) {
      (void)profiling::trace_milestone_at("nearby_stable", stable_since_, "held 30 frames");
    } else {
      profiling::progress_milestone("nearby_stable");
    }
  }
}

JsonValue StartupTrace::finish(std::string* error) {
  if (!on_) return JsonValue::null();
  if (finished_) return block_;
  finished_ = true;
  profiling::trace_stop();
  Vector<profiling::TraceEvent> events;
  profiling::trace_snapshot(events);
  const std::span<const profiling::TraceEvent> all(events.data(), events.size());
  block_ = profiling::startup_summary_json(
      all, std::span<const char* const>(k_startup_milestones, k_startup_milestone_count),
      "nearby_stable", profiling::trace_dropped());
  block_.set("trace", path_);
  block_.set("stable_frames", k_stable_frames);
  block_.set("acyclic", profiling::trace_acyclic(all));
  block_.set("refused_edges", profiling::trace_refused_edges());
  std::string why;
  if (!profiling::write_chrome_trace(path_, all, profiling::trace_origin_ns(),
                                     profiling::trace_dropped(), &why)) {
    block_.set("trace_error", why);
    if (error != nullptr) *error = why;
  }
  return block_;
}

}  // namespace engine::view

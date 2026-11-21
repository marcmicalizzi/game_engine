#pragma once

// **A startup trace shaped like a streamed session's**, made without a clock (T210): a load, then
// frames, each with the stages engine-view records a frame — a top-level `frame` on the main
// thread holding the world's update and its tile change and a wait for a collision build, a
// terrain field and its cluster read on one of four workers, and the sampler's sample — until the
// set holds `events`. `nearby_stable` is reached at `ready_frame` (dated thirty frames earlier, as
// the host dates it) or never. What the readers' cost depends on is the count and the shape, so
// the tests hold that cost by a count and the bench times it (tests/startup_trace_tests.cpp,
// bench/startup_trace_bench.cpp). Test and bench code only.

#include <core/profiling/startup_trace.h>

#include <cstdio>
#include <cstring>

namespace engine::profiling::synthetic {

inline constexpr const char* k_milestones[] = {"main",
                                               "device",
                                               "scene_loaded",
                                               "gpu_scene",
                                               "first_frame",
                                               "player_controllable",
                                               "ground_collision_ready",
                                               "nearby_stable"};
inline constexpr u32 k_milestone_count = sizeof(k_milestones) / sizeof(k_milestones[0]);
inline constexpr u32 k_never = ~0u;
inline constexpr u32 k_workers = 4;
inline constexpr u32 k_sampler_thread = k_workers + 1;
inline constexpr i64 k_ms = 1'000'000;
inline constexpr i64 k_frame_ns = 16 * k_ms;
// The deepest parent chain the shape has: a tile change in a world update in a frame.
inline constexpr u32 k_max_depth = 2;

struct SessionShape {
  u32 events = 100'000;
  u32 ready_frame = 100;  // k_never: the load is never stable (a walk that never settles)
  u32 load_meshes = 64;
};

class SessionWriter {
 public:
  explicit SessionWriter(Vector<TraceEvent>& out) : out_(out) {}

  u32 stage(const char* name, u32 thread, u32 parent, i64 begin, i64 end, const char* detail = "") {
    TraceEvent& e = push(name, TraceKind::stage, thread, parent, begin);
    e.end_ns = end;
    e.cpu_ns = (end - begin) * 3 / 4;
    e.read_bytes = static_cast<u64>(e.id) * 4096u % 1'048'576u;
    e.items = e.id % 7u;
    std::snprintf(e.detail, sizeof(e.detail), "%s", detail);
    return e.id;
  }
  void depends(u32 stage, u32 on) {
    TraceEvent& e = out_[stage];
    if (e.dep_count < k_trace_max_deps) e.deps[e.dep_count++] = on;
  }
  void wait(u32 stage, i64 ns) { out_[stage].wait_ns += ns; }
  u32 milestone(const char* name, u32 thread, u32 parent, i64 at) {
    TraceEvent& e = push(name, TraceKind::milestone, thread, parent, at);
    e.end_ns = at;
    e.sample = sample_at(at);
    return e.id;
  }
  u32 sample(i64 at) {
    TraceEvent& e = push("process", TraceKind::sample, k_sampler_thread, k_trace_none, at);
    e.end_ns = at;
    e.sample = sample_at(at);
    return e.id;
  }
  u32 size() const noexcept { return static_cast<u32>(out_.size()); }

 private:
  TraceEvent& push(const char* name, TraceKind kind, u32 thread, u32 parent, i64 begin) {
    TraceEvent e;
    e.name = name;
    e.kind = kind;
    e.id = static_cast<u32>(out_.size());
    e.thread = thread;
    e.parent = parent;
    e.begin_ns = begin;
    out_.push_back(e);
    return out_[out_.size() - 1];
  }
  static TraceSample sample_at(i64 at) {
    TraceSample s;
    const u64 t = static_cast<u64>(at < 0 ? 0 : at);
    s.cpu_ns = t * 3u / 2u;  // a core and a half busy on average
    s.read_bytes = t / 64u;
    s.read_ops = t / 1'000'000u;
    s.working_set_bytes = (2048u + t / 10'000'000u) * 1'048'576u;
    s.private_bytes = (3072u + t / 8'000'000u) * 1'048'576u;
    s.page_faults = t / 100'000u;
    return s;
  }

  Vector<TraceEvent>& out_;
};

// The session, in id order (a snapshot's order).
inline Vector<TraceEvent> streamed_session(const SessionShape& shape) {
  Vector<TraceEvent> out;
  out.reserve(shape.events + 64);
  SessionWriter w(out);
  w.milestone("main", 0, k_trace_none, 40 * k_ms);
  // ---- the load: the device, the scene's meshes on the workers, the GPU scene ----------------
  i64 t = 50 * k_ms;
  w.stage("device", 0, k_trace_none, t, t + 300 * k_ms);
  w.milestone("device", 0, k_trace_none, t + 300 * k_ms);
  t += 300 * k_ms;
  const u32 load = w.stage("load scene", 0, k_trace_none, t, t + 2000 * k_ms, "scene.json");
  const u32 meshes = w.stage("meshes", 0, load, t + 10 * k_ms, t + 1900 * k_ms);
  u32 last_mesh = k_trace_none;
  for (u32 m = 0; m < shape.load_meshes; ++m) {
    const i64 at = t + 10 * k_ms + static_cast<i64>(m) * 25 * k_ms;
    last_mesh = w.stage("mesh", 1 + m % k_workers, k_trace_none, at, at + 90 * k_ms, "tree.gltf");
    w.stage("cluster read", 1 + m % k_workers, last_mesh, at + k_ms, at + 30 * k_ms);
    if (m % 4 == 0) w.sample(at);
  }
  // The load's wait for its last mesh: an edge points at a stage that began earlier.
  const u32 mesh_wait = w.stage("mesh waits", 0, meshes, t + 1800 * k_ms, t + 1890 * k_ms);
  if (last_mesh != k_trace_none) w.depends(mesh_wait, last_mesh);
  w.milestone("scene_loaded", 0, k_trace_none, t + 2000 * k_ms);
  t += 2000 * k_ms;
  w.stage("gpu scene", 0, k_trace_none, t, t + 800 * k_ms);
  w.milestone("gpu_scene", 0, k_trace_none, t + 800 * k_ms);
  t += 800 * k_ms;
  // ---- the frames: seven events each, until the set is full -------------------------------
  u32 previous_field = k_trace_none;
  for (u32 f = 0; w.size() + 10 <= shape.events; ++f) {
    const i64 t0 = t + static_cast<i64>(f) * k_frame_ns;
    w.sample(t0);
    const u32 frame = w.stage("frame", 0, k_trace_none, t0, t0 + 14 * k_ms);
    if (f == 0) w.milestone("first_frame", 0, frame, t0 + 13 * k_ms);
    if (f == 1) w.milestone("player_controllable", 0, frame, t0 + 13 * k_ms);
    if (f == 2) w.milestone("ground_collision_ready", 0, frame, t0 + 13 * k_ms);
    const u32 update = w.stage("world update", 0, frame, t0 + k_ms / 10, t0 + 3 * k_ms);
    w.stage("tile change", 0, update, t0 + k_ms / 5, t0 + 2 * k_ms, "tile 12_34");
    const u32 field = w.stage("terrain field", 1 + f % k_workers, k_trace_none, t0 + k_ms / 2,
                              t0 + 8 * k_ms, "ring 2");
    w.stage("cluster read", 1 + f % k_workers, field, t0 + 6 * k_ms / 10, t0 + 2 * k_ms);
    const u32 wait = w.stage("collision waits", 0, frame, t0 + 9 * k_ms, t0 + 10 * k_ms);
    if (previous_field != k_trace_none) w.depends(wait, previous_field);
    w.wait(frame, k_ms);
    previous_field = field;
    if (shape.ready_frame != k_never && f == shape.ready_frame)
      w.milestone("nearby_stable", 0, frame, t0 - 30 * k_frame_ns);
  }
  return out;
}

}  // namespace engine::profiling::synthetic

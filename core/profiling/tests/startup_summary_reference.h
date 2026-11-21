#pragma once

// **The startup trace's readers as they were before T210**, kept as the reference the rewrite is
// held to byte for byte (tests/startup_trace_tests.cpp) and the "before" the bench times
// (bench/startup_trace_bench.cpp). The code is core/profiling/src/startup_trace.cpp's at 4a51ab021,
// unchanged but for the namespace and its comments: the summary walked every event once per
// top-level stage and climbed each one's parents with a lookup that scanned every event on a miss,
// and the acyclic check scanned every event per edge — quadratic in the events, which a five-minute
// walk made twenty minutes at exit (docs/subsystems/profiling.md, "The readers are linear in the
// events"). Test and bench code only; nothing in the engine calls it.

#include <core/json/json_value.h>
#include <core/profiling/startup_trace.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace engine::profiling::reference {

inline bool trace_acyclic(std::span<const TraceEvent> events) noexcept {
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

namespace detail {

inline void append_escaped(std::string& out, std::string_view text) {
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

inline void append_us(std::string& out, i64 ns) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.3f", static_cast<f64>(ns) / 1000.0);
  out += buf;
}

inline void append_u64(std::string& out, u64 v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
  out += buf;
}

inline void append_i64(std::string& out, i64 v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
  out += buf;
}

inline void append_f64(std::string& out, f64 v) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.6g", v);
  out += buf;
}

inline const TraceEvent* find_event(std::span<const TraceEvent> events, u32 id) noexcept {
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

inline f64 ms_of(i64 ns) { return static_cast<f64>(ns) / 1.0e6; }

inline const TraceSample* sample_at(std::span<const TraceEvent> events, i64 at) noexcept {
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

inline JsonValue process_json(const TraceSample& s) {
  JsonValue o = JsonValue::object();
  o.set("cpu_ms", ms_of(static_cast<i64>(s.cpu_ns)));
  o.set("read_mib", static_cast<f64>(s.read_bytes) / k_mib);
  o.set("read_ops", s.read_ops);
  o.set("working_set_mib", static_cast<f64>(s.working_set_bytes) / k_mib);
  o.set("private_mib", static_cast<f64>(s.private_bytes) / k_mib);
  o.set("page_faults", s.page_faults);
  return o;
}

}  // namespace detail

inline std::string chrome_trace_json(std::span<const TraceEvent> events, i64 origin_ns,
                                     u32 dropped) {
  using namespace detail;
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

inline JsonValue startup_summary_json(std::span<const TraceEvent> events,
                                      std::span<const char* const> milestones, const char* until,
                                      u32 dropped) {
  using namespace detail;
  JsonValue out = JsonValue::object();
  out.set("format", "engine.startup.v1");
  out.set("origin", "process launch");
  out.set("dropped_events", dropped);
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
    Vector<const TraceEvent*> children;
    JsonValue waits = JsonValue::array();
    for (const TraceEvent& c : events) {
      if (c.kind != TraceKind::stage || c.id <= e.id) continue;
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
  u32 threads = 0;
  for (const TraceEvent& e : events)
    threads = std::max(threads, e.thread + 1);
  JsonValue per_thread = JsonValue::array();
  for (u32 t = 0; t < threads; ++t) {
    i64 busy = 0, cpu = 0, wait = 0;
    u32 count = 0;
    for (const TraceEvent& e : events) {
      if (e.kind != TraceKind::stage || e.thread != t || e.begin_ns > end_ns) continue;
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

}  // namespace engine::profiling::reference

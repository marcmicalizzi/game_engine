#include <core/json/json.h>
#include <core/profiling/startup_trace.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <thread>

using namespace engine;
using namespace engine::profiling;

namespace {

const TraceEvent* by_name(const Vector<TraceEvent>& events, const char* name) {
  for (const TraceEvent& e : events)
    if (std::strcmp(e.name, name) == 0) return &e;
  return nullptr;
}

// Keeps a stage busy long enough to have a measurable duration on any clock.
void spin_us(i64 us) {
  const i64 until = trace_now_ns() + us * 1000;
  while (trace_now_ns() < until) {
  }
}

struct Recording {
  Recording(u32 capacity = k_trace_default_capacity, u32 sample_ms = 0) {
    TraceOptions options;
    options.origin_ns = trace_now_ns();
    options.capacity = capacity;
    options.sample_ms = sample_ms;
    REQUIRE(trace_start(options));
  }
  ~Recording() { trace_reset(); }
};

}  // namespace

TEST_CASE("the startup trace is off by default and records nothing while off") {
  CHECK_FALSE(trace_enabled());
  CHECK(trace_begin("never") == k_trace_none);
  CHECK(trace_milestone("never") == k_trace_none);
  {
    ENGINE_STARTUP_STAGE("off");
    TraceStage stage("off too");
    CHECK(stage.id() == k_trace_none);
    trace_wait(1000);
    trace_items(3);
  }
  Vector<TraceEvent> events;
  trace_snapshot(events);
  CHECK(events.empty());
  CHECK(trace_current() == k_trace_none);
}

TEST_CASE("stages nest per thread, and a wait names what it waited for") {
  Recording recording;
  u32 worker_stage = k_trace_none;
  u32 outer = k_trace_none;
  {
    ENGINE_STARTUP_STAGE_DETAIL("load", "scene.json");
    outer = trace_current();
    REQUIRE(outer != k_trace_none);
    std::thread worker([&] {
      TraceStage decode("decode");
      worker_stage = decode.id();
      trace_items(7);
      spin_us(200);
    });
    worker.join();
    {
      TraceWait wait("wait for decode", worker_stage);
      spin_us(100);
    }
    {
      ENGINE_STARTUP_STAGE("upload");
      spin_us(50);
    }
    trace_milestone("first_frame", "frame 0");
  }
  CHECK(trace_current() == k_trace_none);
  Vector<TraceEvent> events;
  trace_snapshot(events);
  const TraceEvent* load = by_name(events, "load");
  const TraceEvent* decode = by_name(events, "decode");
  const TraceEvent* wait = by_name(events, "wait for decode");
  const TraceEvent* upload = by_name(events, "upload");
  const TraceEvent* frame = by_name(events, "first_frame");
  REQUIRE(load != nullptr);
  REQUIRE(decode != nullptr);
  REQUIRE(wait != nullptr);
  REQUIRE(upload != nullptr);
  REQUIRE(frame != nullptr);
  CHECK(std::strcmp(load->detail, "scene.json") == 0);
  CHECK(load->parent == k_trace_none);
  CHECK(decode->parent == k_trace_none);  // a thread's own first stage: nothing open on it
  CHECK(decode->thread != load->thread);
  CHECK(decode->items == 7);
  CHECK(wait->parent == load->id);
  CHECK(upload->parent == load->id);
  REQUIRE(wait->dep_count == 1);
  CHECK(wait->deps[0] == decode->id);
  CHECK(load->wait_ns >= wait->duration_ns());  // the wait counts against the stage it is in
  CHECK(frame->kind == TraceKind::milestone);
  CHECK(frame->parent == load->id);
  CHECK(load->end_ns >= upload->end_ns);
  CHECK(decode->duration_ns() >= 200000);
  CHECK(trace_acyclic(std::span<const TraceEvent>(events.data(), events.size())));
}

TEST_CASE("a dependency on a stage that began later is refused, so the graph stays acyclic") {
  Recording recording;
  const u32 first = trace_begin("first");
  const u32 second = trace_begin("second");
  CHECK_FALSE(trace_depends(first, second));  // forward: refused
  CHECK_FALSE(trace_depends(first, first));   // a loop on itself
  CHECK(trace_depends(second, first));
  CHECK(trace_refused_edges() == 2);
  trace_end(second);
  trace_end(first);
  Vector<TraceEvent> events;
  trace_snapshot(events);
  CHECK(trace_acyclic(std::span<const TraceEvent>(events.data(), events.size())));
  // A set read from elsewhere with an edge forward (or to nothing) is caught by the same check.
  Vector<TraceEvent> cycle = events;
  cycle[0].deps[0] = cycle[1].id;
  cycle[0].dep_count = 1;
  CHECK_FALSE(trace_acyclic(std::span<const TraceEvent>(cycle.data(), cycle.size())));
  Vector<TraceEvent> dangling = events;
  dangling[1].deps[0] = 77;
  CHECK_FALSE(trace_acyclic(std::span<const TraceEvent>(dangling.data(), dangling.size())));
}

TEST_CASE("a full buffer drops and counts, and never writes past its end") {
  Recording recording(16);
  for (u32 i = 0; i < 20; ++i) {
    TraceStage stage("s");
  }
  Vector<TraceEvent> events;
  trace_snapshot(events);
  CHECK(events.size() == 16);
  CHECK(trace_dropped() == 4);
}

TEST_CASE("the summary's milestones are monotone in the order they are listed") {
  Recording recording;
  {
    ENGINE_STARTUP_STAGE("prepare");
    spin_us(100);
  }
  trace_milestone("first_frame");
  spin_us(100);
  // Reached before first_frame would have been, in wall time — listed after it, so reported then.
  trace_milestone("controllable");
  spin_us(100);
  trace_milestone("ready");
  Vector<TraceEvent> events;
  trace_snapshot(events);
  // Swap the two milestones' times so that "controllable" comes first in time.
  TraceEvent* frame = nullptr;
  TraceEvent* control = nullptr;
  for (TraceEvent& e : events) {
    if (std::strcmp(e.name, "first_frame") == 0) frame = &e;
    if (std::strcmp(e.name, "controllable") == 0) control = &e;
  }
  REQUIRE(frame != nullptr);
  REQUIRE(control != nullptr);
  std::swap(frame->begin_ns, control->begin_ns);
  const char* order[] = {"first_frame", "controllable", "ground", "ready"};
  const JsonValue summary = startup_summary_json(
      std::span<const TraceEvent>(events.data(), events.size()), order, "ready");
  const JsonValue* list = summary.find("milestones");
  REQUIRE(list != nullptr);
  REQUIRE(list->size() == 4);
  f64 last = 0.0;
  u32 reached = 0;
  for (const JsonValue& m : list->as_array()) {
    const JsonValue* ms = m.find("ms");
    REQUIRE(ms != nullptr);
    f64 value = 0.0;
    if (!ms->get_f64(value)) continue;  // "ground" was never reached: null
    CHECK(value >= last);
    last = value;
    ++reached;
  }
  CHECK(reached == 3);
  u64 reordered = 0;
  REQUIRE(summary.find("milestones_reordered")->get_u64(reordered));
  CHECK(reordered == 1);
  bool until = false;
  REQUIRE(summary.find("until_reached")->get_bool(until));
  CHECK(until);
  const JsonValue* stages = summary.find("stages");
  REQUIRE(stages != nullptr);
  REQUIRE(stages->size() == 1);
}

TEST_CASE("the chrome trace round-trips a recorded set") {
  Recording recording(k_trace_default_capacity, 1);
  u32 worker_stage = k_trace_none;
  {
    ENGINE_STARTUP_STAGE_DETAIL("scene", "a \"quoted\" path\\with\nlines");
    std::thread worker([&] {
      TraceStage tiles("tiles");
      worker_stage = tiles.id();
      trace_items(1804);
      spin_us(300);
    });
    worker.join();
    TraceWait wait("collision waits", worker_stage);
    trace_wait(5);
    spin_us(2000);  // long enough for the sampler to have run
  }
  trace_milestone("ready");
  trace_stop();
  Vector<TraceEvent> events;
  trace_snapshot(events);
  const std::string text =
      chrome_trace_json(std::span<const TraceEvent>(events.data(), events.size()), 42, 3);
  JsonValue doc;
  REQUIRE(parse_json(text, doc).ok);
  const JsonValue* list = doc.find("traceEvents");
  REQUIRE(list != nullptr);
  u64 dropped = 0;
  REQUIRE(doc.find("otherData")->find("dropped")->get_u64(dropped));
  CHECK(dropped == 3);
  u32 stages = 0, milestones = 0, samples = 0, flows = 0;
  for (const JsonValue& e : list->as_array()) {
    std::string_view ph, name;
    REQUIRE(e.find("ph")->get_string(ph));
    if (ph == "C") {
      ++samples;
      continue;
    }
    if (ph == "s" || ph == "f") {
      ++flows;
      continue;
    }
    if (ph == "M") continue;
    REQUIRE(e.find("name")->get_string(name));
    const JsonValue* args = e.find("args");
    REQUIRE(args != nullptr);
    u64 id = 0;
    REQUIRE(args->find("id")->get_u64(id));
    const TraceEvent* original = nullptr;
    for (const TraceEvent& o : events)
      if (o.id == id) original = &o;
    REQUIRE(original != nullptr);
    CHECK(name == std::string_view(original->name));
    f64 ts = 0.0;
    REQUIRE(e.find("ts")->get_f64(ts));
    CHECK(std::llround(ts * 1000.0) == original->begin_ns);
    u64 tid = 0;
    REQUIRE(e.find("tid")->get_u64(tid));
    CHECK(tid == original->thread);
    std::string_view detail;
    REQUIRE(args->find("detail")->get_string(detail));
    CHECK(detail == std::string_view(original->detail));
    if (ph == "X") {
      ++stages;
      f64 dur = 0.0;
      REQUIRE(e.find("dur")->get_f64(dur));
      CHECK(std::llround(dur * 1000.0) == original->duration_ns());
      i64 cpu = 0, wait = 0;
      u64 read = 0, items = 0;
      REQUIRE(args->find("cpu_ns")->get_i64(cpu));
      REQUIRE(args->find("wait_ns")->get_i64(wait));
      REQUIRE(args->find("read_bytes")->get_u64(read));
      REQUIRE(args->find("items")->get_u64(items));
      CHECK(cpu == original->cpu_ns);
      CHECK(wait == original->wait_ns);
      CHECK(read == original->read_bytes);
      CHECK(items == original->items);
      const JsonValue* parent = args->find("parent");
      u64 p = 0;
      if (original->parent == k_trace_none)
        CHECK(parent->is_null());
      else {
        REQUIRE(parent->get_u64(p));
        CHECK(p == original->parent);
      }
      const JsonValue* deps = args->find("depends");
      REQUIRE(deps->size() == original->dep_count);
      for (u32 d = 0; d < original->dep_count; ++d) {
        u64 on = 0;
        REQUIRE(deps->as_array()[d].get_u64(on));
        CHECK(on == original->deps[d]);
      }
    } else {
      CHECK(ph == "i");
      ++milestones;
    }
  }
  CHECK(stages == 3);
  CHECK(milestones == 1);
  CHECK(flows == 2);    // one edge: its start and its end
  CHECK(samples >= 4);  // four counter tracks a sample, at least one sample
}

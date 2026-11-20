#include "startup_summary_reference.h"
#include "synthetic_session.h"

#include <core/json/json.h>
#include <core/profiling/startup_trace.h>

#include <doctest/doctest.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

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

// Waits until the sampler thread has recorded a sample, under a minute's deadline. The sampler is
// an OS thread on its own wait (`TraceOptions::sample_ms`) with no tick a test can drive, so the
// case waits for the event it reads rather than for a time: a 2 ms spin stood in for it and failed
// on a saturated machine (batch 70's stack, GCC), where the thread was not scheduled within it.
bool sampler_has_run(i64 deadline_ns = 60'000'000'000) {
  const i64 until = trace_now_ns() + deadline_ns;
  Vector<TraceEvent> events;
  while (trace_now_ns() < until) {
    trace_snapshot(events);
    for (const TraceEvent& e : events)
      if (e.kind == TraceKind::sample) return true;
    std::this_thread::yield();
  }
  return false;
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
    spin_us(2000);  // the wait's own duration
    REQUIRE_MESSAGE(sampler_has_run(), "the sampler thread recorded no sample in a minute");
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

// ---- the readers are linear in the events (T210)
// ----------------------------------------------------------------------

namespace {

std::string summary_text(const Vector<TraceEvent>& events, const char* until,
                         TraceWalkCost* cost = nullptr) {
  return write_json(startup_summary_json(std::span<const TraceEvent>(events.data(), events.size()),
                                         synthetic::k_milestones, until, 5, cost),
                    JsonWriteOptions{.pretty = false});
}

std::string reference_summary_text(const Vector<TraceEvent>& events, const char* until) {
  return write_json(
      reference::startup_summary_json(std::span<const TraceEvent>(events.data(), events.size()),
                                      synthetic::k_milestones, until, 5),
      JsonWriteOptions{.pretty = false});
}

// The three readers against the copies of them as they were before T210, byte for byte.
void check_same_as_before(const Vector<TraceEvent>& events) {
  const std::span<const TraceEvent> all(events.data(), events.size());
  const char* const untils[] = {"nearby_stable", "first_frame", "never named", nullptr};
  for (const char* until : untils) {
    CAPTURE(until);
    CHECK(summary_text(events, until) == reference_summary_text(events, until));
  }
  CHECK(chrome_trace_json(all, 42, 3) == reference::chrome_trace_json(all, 42, 3));
  CHECK(trace_acyclic(all) == reference::trace_acyclic(all));
}

// A deterministic shuffle: a set read back from elsewhere need not be in id order.
Vector<TraceEvent> shuffled(const Vector<TraceEvent>& events) {
  Vector<TraceEvent> out = events;
  u64 state = 0x9e3779b97f4a7c15ull;
  for (u32 i = out.size(); i > 1; --i) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    std::swap(out[i - 1], out[static_cast<u32>((state >> 33) % i)]);
  }
  return out;
}

u64 bit_width(u64 n) {
  u64 bits = 0;
  while (n != 0) {
    ++bits;
    n >>= 1;
  }
  return bits;
}

}  // namespace

TEST_CASE("the trace's readers say what they said before T210, on a recorded trace") {
  // A recorded session in small: frames on this thread, each with a world update and its tile
  // change, a worker on a thread of its own every frame (so the trace has a thread index for each,
  // as a session that makes threads as it goes does), a wait for the last frame's worker, the
  // sampler every millisecond, and the ladder's milestones with `nearby_stable` dated back.
  Recording recording(1u << 14, 1);
  {
    ENGINE_STARTUP_STAGE("device");
    spin_us(100);
  }
  {
    ENGINE_STARTUP_STAGE_DETAIL("load scene", "scene.json");
    spin_us(200);
  }
  trace_milestone("scene_loaded");
  u32 previous_worker = k_trace_none;
  i64 stable_since = 0;
  for (u32 f = 0; f < 160; ++f) {
    ENGINE_STARTUP_STAGE("frame");
    {
      ENGINE_STARTUP_STAGE("world update");
      ENGINE_STARTUP_STAGE_DETAIL("tile change", "tile 3_4");
      spin_us(20);
    }
    u32 worker = k_trace_none;
    std::thread field([&] {
      TraceStage stage("terrain field", "ring 1");
      worker = stage.id();
      ENGINE_STARTUP_STAGE("cluster read");
      trace_items(3);
      spin_us(30);
    });
    field.join();
    if (previous_worker != k_trace_none) {
      TraceWait wait("collision waits", previous_worker);
      spin_us(10);
    }
    previous_worker = worker;
    if (f == 0) trace_milestone("first_frame");
    if (f == 1) trace_milestone("player_controllable");
    if (f == 2) trace_milestone("ground_collision_ready");
    if (f == 70) stable_since = trace_now_ns();
    if (f == 100) trace_milestone_at("nearby_stable", stable_since, "held 30 frames");
  }
  REQUIRE_MESSAGE(sampler_has_run(), "the sampler thread recorded no sample in a minute");
  trace_stop();
  Vector<TraceEvent> events;
  trace_snapshot(events);
  REQUIRE(events.size() > 900);  // six stages a frame, and the samples
  CHECK(trace_dropped() == 0);
  check_same_as_before(events);
}

TEST_CASE("the trace's readers say what they said before T210, on a streamed session's shape") {
  // Small enough for the quadratic copies to finish in a debug build: ready at frame 100, and
  // never ready (every frame then a top-level stage of the summary's).
  for (const u32 ready : {100u, synthetic::k_never}) {
    CAPTURE(ready);
    const Vector<TraceEvent> events = synthetic::streamed_session({3'000, ready});
    REQUIRE(events.size() > 2'900);
    check_same_as_before(events);
  }
  // Out of id order, as a set read back from elsewhere may be: the copies' lookups scan on every
  // miss there, so the set is smaller still.
  check_same_as_before(shuffled(synthetic::streamed_session({600, 20})));
  // And the shape is a graph the trace could have recorded: every edge points back.
  const Vector<TraceEvent> shape = synthetic::streamed_session({3'000, 100});
  CHECK(trace_acyclic(std::span<const TraceEvent>(shape.data(), shape.size())));
}

TEST_CASE("the trace's readers cost visits in proportion to the events, at 10^5 and 10^6") {
  // The bound, from the readers' passes (startup_trace.cpp): the summary reads every event once
  // in each of nine passes and once per milestone in two more ((2M + 9) n), and looks up by id
  // every parent of every stage (stages x depth) and every edge twice (beneath its top, and as a
  // top's own), each lookup a binary search of at most bit_width(n) + 1 steps. Before T210 the
  // summary visited every event once per top-level stage before ready, climbing each one's
  // parents, and the acyclic check every event per edge: quadratic, and twenty minutes at exit
  // after a five-minute walk.
  const u64 milestones = synthetic::k_milestone_count;
  for (const u32 n : {100'000u, 1'000'000u}) {
    for (const u32 ready : {100u, synthetic::k_never}) {
      // The never-ready shape's summary lists every frame; at 10^6 that is a JSON document of
      // 140,000 stages, which says nothing the 10^5 one does not.
      if (n == 1'000'000u && ready == synthetic::k_never) continue;
      CAPTURE(n);
      CAPTURE(ready);
      const Vector<TraceEvent> events = synthetic::streamed_session({n, ready});
      const std::span<const TraceEvent> all(events.data(), events.size());
      u64 stages = 0, edges = 0;
      for (const TraceEvent& e : events) {
        stages += e.kind == TraceKind::stage ? 1u : 0u;
        edges += e.dep_count;
      }
      const u64 size = events.size();
      const u64 lookup = bit_width(size) + 1;
      TraceWalkCost summary;
      (void)summary_text(events, "nearby_stable", &summary);
      const u64 summary_bound =
          (2 * milestones + 9) * size + (stages * synthetic::k_max_depth + 2 * edges) * lookup;
      MESSAGE("summary visits " << summary.visits << " for " << size << " events, "
                                << static_cast<f64>(summary.visits) / static_cast<f64>(size)
                                << " an event; bound " << summary_bound);
      CHECK(summary.visits <= summary_bound);
      TraceWalkCost acyclic;
      CHECK(trace_acyclic(all, &acyclic));
      CHECK(acyclic.visits <= 2 * size + edges * lookup);
      // The writer's text is a quarter of a kilobyte an event: at 10^6, a quarter of a gigabyte
      // to say what 10^5 says.
      if (n == 100'000u) {
        TraceWalkCost chrome;
        (void)chrome_trace_json(all, 0, 0, &chrome);
        CHECK(chrome.visits <= 3 * size + edges * lookup);
      }
    }
  }
}

// ---- the live progress (W79)
// ----------------------------------------------------------------------

namespace {
constexpr const char* k_ladder[] = {"main", "device", "scene_loaded", "ready"};

ProgressPlan three_stages(bool from_previous) {
  ProgressPlan plan;
  plan.from_previous = from_previous;
  REQUIRE(plan.add_stage("read", from_previous ? 2.0 : 1.0));
  REQUIRE(plan.add_stage("build", from_previous ? 6.0 : 1.0));
  REQUIRE(plan.add_stage("upload", from_previous ? 2.0 : 1.0));
  CHECK_FALSE(plan.add_stage("read", 1.0));  // named already
  for (const char* m : k_ladder)
    REQUIRE(plan.add_milestone(m));
  return plan;
}

struct Following {
  explicit Following(const ProgressPlan& plan) { REQUIRE(progress_start(plan)); }
  ~Following() { progress_stop(); }
};
}  // namespace

TEST_CASE("progress follows the plan's stages, rises monotonically and reaches every milestone") {
  CHECK_FALSE(progress_enabled());
  {
    ENGINE_STARTUP_STAGE("read");
  }  // off: a stage feeds nothing
  for (const bool previous : {false, true}) {
    CAPTURE(previous);
    Following following(three_stages(previous));
    CHECK(progress_enabled());
    LoadProgress p;
    progress_read(p);
    CHECK(p.active);
    CHECK(p.stages_expected == 3);
    CHECK(p.stages_done == 0);
    CHECK(p.fraction == 0.0f);
    CHECK(p.milestone_count == 4);
    f32 last = 0.0f;
    const auto look = [&] {
      progress_read(p);
      CHECK(p.fraction >= last);
      last = p.fraction;
    };
    progress_milestone("main");
    progress_milestone("device");
    {
      ENGINE_STARTUP_STAGE_DETAIL("read", "scene.json");
      look();
      CHECK(std::string(p.stage) == "read");
      CHECK(std::string(p.detail) == "scene.json");
      {
        ENGINE_STARTUP_STAGE_DETAIL("cluster read", "tree.clusters");  // not the plan's: detail
        trace_items(5);
        spin_us(2000);
        look();
        CHECK(std::string(p.stage) == "read");
        CHECK(std::string(p.detail) == "cluster read: tree.clusters");
        CHECK(p.items == 5);
      }
    }
    look();
    CHECK(p.stages_done == 1);
    CHECK(p.fraction == doctest::Approx(previous ? 0.2 : 1.0 / 3.0).epsilon(0.001));
    progress_milestone("scene_loaded");
    {
      ENGINE_STARTUP_STAGE("build");
      spin_us(4000);  // 4 of the previous run's 6 ms: a share of the stage, under 0.9 of it
      look();
      if (previous) {
        CHECK(p.fraction > 0.2f);
        CHECK(p.fraction <= 0.2f + 0.6f * 0.9f + 1e-4f);
      } else {
        CHECK(p.fraction == doctest::Approx(1.0 / 3.0).epsilon(0.001));  // equal weights: no share
      }
      CHECK(p.stage_elapsed_ns >= 4'000'000);
    }
    look();
    CHECK(p.stages_done == 2);
    {
      ENGINE_STARTUP_STAGE("upload");
    }
    look();
    CHECK(p.stages_done == 3);
    CHECK(p.fraction == doctest::Approx(1.0));
    progress_milestone("not on the ladder");
    progress_milestone("ready");
    progress_finish();
    look();
    CHECK(p.done);
    CHECK(p.fraction == 1.0f);
    CHECK(p.milestones == 0b1111u);  // every rung, each its own bit in ladder order
    // What the next run's plan is made of: the three, measured.
    ProgressPlan measured;
    progress_measured(measured);
    CHECK(measured.from_previous);
    REQUIRE(measured.stage_count == 3);
    CHECK(std::string(measured.stages[1]) == "build");
    CHECK(measured.weights[1] >= 4.0);
    CHECK(measured.milestone_count == 4);
  }
  CHECK_FALSE(progress_enabled());
}

TEST_CASE("a progress token from an earlier plan finishes nothing in the next") {
  u32 stale = k_trace_none;
  {
    Following first(three_stages(false));
    stale = progress_stage_begin("read", {});
    CHECK(stale != k_trace_none);
  }
  Following second(three_stages(false));
  progress_stage_end(stale);
  LoadProgress p;
  progress_read(p);
  CHECK(p.stages_done == 0);
  progress_milestone("scene_loaded");
  progress_read(p);
  CHECK(p.milestones == 0b0100u);
}

TEST_CASE("progress reads are consistent while stages begin and end on other threads") {
  ProgressPlan plan;
  REQUIRE(plan.add_stage("alpha", 1.0));
  REQUIRE(plan.add_stage("beta", 1.0));
  Following following(plan);
  std::atomic<bool> stop{false};
  std::thread writers[3];
  for (std::thread& w : writers) {
    w = std::thread([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        ENGINE_STARTUP_STAGE_DETAIL("alpha", "aaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        ENGINE_STARTUP_STAGE_DETAIL("beta", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
      }
    });
  }
  u32 torn = 0;
  f32 last = 0.0f;
  for (u32 i = 0; i < 20000; ++i) {
    LoadProgress p;
    progress_read(p);
    const std::string stage(p.stage);
    const std::string detail(p.detail);
    // A stage and its own detail, never one stage with the other's half-written.
    if (!((stage == "alpha" && (detail.empty() || detail == "aaaaaaaaaaaaaaaaaaaaaaaaaaaa")) ||
          (stage == "beta" && detail == "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")))
      ++torn;
    if (p.fraction < last) ++torn;
    last = p.fraction;
  }
  stop = true;
  for (std::thread& w : writers)
    w.join();
  CHECK(torn == 0);
}

namespace {
struct PumpCounter {
  u32 calls = 0;
  static void pump(void* user) {
    ++static_cast<PumpCounter*>(user)->calls;
    ENGINE_STARTUP_STAGE("inside the pump");  // a nested pump is ignored
    progress_pump();
  }
};
}  // namespace

TEST_CASE("the pump runs on its own thread's stage boundaries and nowhere else") {
  PumpCounter counter;
  ProgressPlan plan;
  REQUIRE(plan.add_stage("long", 1.0));
  {
    Following following(plan);
    progress_set_pump(&PumpCounter::pump, &counter);
    {
      ENGINE_STARTUP_STAGE("long");  // its begin: 1
      for (u32 slice = 0; slice < 4; ++slice)
        progress_pump();                // 4
      ENGINE_STARTUP_STAGE("a slice");  // its begin and its end: 2
    }  // the long stage's end: 1
    CHECK(counter.calls == 8);
    std::thread other([] {
      ENGINE_STARTUP_STAGE("elsewhere");
      progress_pump();
    });
    other.join();
    CHECK(counter.calls == 8);
    progress_set_pump(nullptr, nullptr);
    progress_pump();
    CHECK(counter.calls == 8);
  }
  progress_set_pump(&PumpCounter::pump, &counter);
  {
    ENGINE_STARTUP_STAGE("long");
  }  // progress off: nothing pumps at a boundary
  CHECK(counter.calls == 8);
  progress_set_pump(nullptr, nullptr);
}

// Experiment E7, the Luau half (docs/experiments/e7-luau-spike.md): what a Luau host costs the
// engine, measured on the shapes the plan uses scripts for (docs/plan/08-toolchain.md §8.2).
//
//   scripting.call.empty           C++ -> Luau -> C++ with nothing passed or returned
//   scripting.call.roundtrip       one number in, the same number back as a JsonValue
//   scripting.read.baseline        the read benches' call with no property read, to subtract
//   scripting.read.ten             ten scalar properties of a bound schema object, summed
//   scripting.read.mixed           a string, an enum, a vector, a nested struct's field, a bool
//   scripting.tick.shared/N        on_tick(self, dt) over N instances sharing one VM
//   scripting.tick.separate/N      the same over N instances with one VM each
//   scripting.compile.200_lines    compile, load and run the top level of a 200-line script
//   scripting.reload.poll_idle     poll_changes() over 16 watched files, none changed
//   scripting.reload.poll_hit      poll_changes() when one file changed: stat, read, compile, swap
//   scripting.context.create       create and destroy a context (sandbox included)
//   scripting.gc.pauses/N          a frame allocating N small tables into a retained ring
//
// Memory per instance, the VM floor, and the GC pause distribution are not times, so those
// benches print one JSON line each (prefixed `{"e7":`) beside the harness's own table. Take the
// numbers from a release build on a quiet machine:
//
//   engine_scripting_bench.exe --require-quiet --json=e7.jsonl
//   engine_scripting_bench.exe --filter=scripting.gc.* --repeats=1 --warmup=0 --require-quiet
//
// Debug builds only smoke-run benchmarks, so every fixture here shrinks under --smoke.

#include <core/base/assert.h>
#include <core/containers/vector.h>
#include <core/log/log.h>
#include <core/memory/memory.h>
#include <core/time/time.h>
#include <foundation/bench/bench.h>
#include <foundation/scripting/scripting.h>

#include <test_temp_dir.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <schemas/scripting_test.h>
#include <string>
#include <string_view>

using namespace engine;
using scripting::Arg;
using scripting::ScriptContext;
using scripting::ScriptId;
using scripting::Status;

namespace {

ScriptId must_load(ScriptContext& context, std::string_view name, std::string_view source) {
  ScriptId id;
  const Status status = context.load(name, source, id);
  ENGINE_VERIFY(status == Status::Ok, "scripting bench: a fixture script failed to load");
  return id;
}

scripting::FunctionId must_find(ScriptContext& context, ScriptId id, std::string_view name) {
  scripting::FunctionId fn;
  ENGINE_VERIFY(context.find_function(id, name, fn) == Status::Ok,
                "scripting bench: a fixture function is missing");
  return fn;
}

void must_call(ScriptContext& context, scripting::FunctionId fn, std::span<const Arg> args,
               JsonValue* out) {
  const Status status = context.call(fn, args, out);
  ENGINE_VERIFY(status == Status::Ok, "scripting bench: a fixture call failed");
}

// Prints a result JSON line once per (bench, argument), however often the harness calls the body.
bool report_once(const char* bench, i64 arg) {
  static const char* seen_names[64];
  static i64 seen_args[64];
  static u32 count = 0;
  for (u32 i = 0; i < count; ++i)
    if (seen_names[i] == bench && seen_args[i] == arg) return false;
  if (count < 64) {
    seen_names[count] = bench;
    seen_args[count] = arg;
    ++count;
  }
  return true;
}

// ---- calls -------------------------------------------------------------------------------------

constexpr std::string_view k_calls = R"(
function empty() end
function echo(x) return x end
function base(o) return 0 end
function ten(o) return o.a + o.b + o.c + o.d + o.e + (if o.f then 1 else 0) + o.g + o.h + o.i + o.j end
function mixed(npc)
  local n = #npc.name
  if npc.faction == "Drifters" then n += 1 end
  n += npc.position.x
  n += npc.attributes.strength
  if npc.alive then n += 1 end
  return n
end
)";

ENGINE_BENCH(call_empty, "scripting.call.empty") {
  ScriptContext context;
  const ScriptId id = must_load(context, "calls.luau", k_calls);
  const scripting::FunctionId fn = must_find(context, id, "empty");
  while (state.keep_running()) {
    must_call(context, fn, {}, nullptr);
  }
  state.set_items(1);
}

ENGINE_BENCH(call_roundtrip, "scripting.call.roundtrip") {
  ScriptContext context;
  const ScriptId id = must_load(context, "calls.luau", k_calls);
  const scripting::FunctionId fn = must_find(context, id, "echo");
  JsonValue out;
  f64 x = 0.5;
  while (state.keep_running()) {
    const Arg args[] = {Arg::number(x)};
    must_call(context, fn, args, &out);
    f64 back = 0;
    out.get_f64(back);
    x = back + 1.0;
    bench::keep(x);
  }
  state.set_items(1);
}

template <class T>
void read_bench(bench::State& state, std::string_view function, const T& object, u64 items) {
  ScriptContext context;
  const ScriptId id = must_load(context, "calls.luau", k_calls);
  const scripting::FunctionId fn = must_find(context, id, function);
  const scripting::BindingId binding = context.bind(scripting::object_ref(object));
  const Arg args[] = {Arg::binding(binding)};
  JsonValue out;
  while (state.keep_running()) {
    must_call(context, fn, args, &out);
    bench::keep(out);
  }
  state.set_items(items);
}

ENGINE_BENCH(read_baseline, "scripting.read.baseline") {
  const scripting_test::Ten ten;
  read_bench(state, "base", ten, 1);
}

ENGINE_BENCH(read_ten, "scripting.read.ten") {
  const scripting_test::Ten ten;
  read_bench(state, "ten", ten, 10);
}

ENGINE_BENCH(read_mixed, "scripting.read.mixed") {
  scripting_test::Character npc;
  npc.name = "Ada";
  npc.faction = scripting_test::Faction::Drifters;
  npc.position = Vec3{1.0f, 2.0f, 3.0f};
  read_bench(state, "mixed", npc, 5);
}

// ---- on_tick over many instances ---------------------------------------------------------------

// A routine of the size the plan means by content logic: read a few properties, branch, return
// nothing on the common path.
constexpr std::string_view k_routine = R"(
function on_tick(self, dt)
  if not self.alive then return nil end
  local threat = self.health < 30 and self.faction ~= "Ironbound"
  if threat and self.position.y < 0 then
    return { action = "flee" }
  end
  return nil
end
)";

Vector<scripting_test::Character> make_npcs(u32 count) {
  Vector<scripting_test::Character> npcs;
  npcs.resize(count);
  for (u32 i = 0; i < count; ++i) {
    npcs[i].name = "npc";
    npcs[i].health = static_cast<f32>(40 + (i % 60));
    npcs[i].faction = static_cast<scripting_test::Faction>(i % 3);
    npcs[i].position = Vec3{static_cast<f32>(i), 1.0f, 0.0f};
  }
  return npcs;
}

u32 instance_count(bench::State& state) {
  return bench::smoke_mode() ? 8u : static_cast<u32>(state.arg());
}

ENGINE_BENCH_ARGS(tick_shared, "scripting.tick.shared", 1000, 10000) {
  const u32 n = instance_count(state);
  const Vector<scripting_test::Character> npcs = make_npcs(n);
  const mem::Stats host_before = mem::total_stats();
  ScriptContext context;
  const ScriptId id = must_load(context, "routine.luau", k_routine);
  const scripting::FunctionId on_tick = must_find(context, id, "on_tick");
  context.collect_garbage();
  const scripting::ContextStats before = context.stats();
  scripting::ScriptingSystem system(context);
  for (u32 i = 0; i < n; ++i)
    system.add(on_tick, context.bind(scripting::object_ref(npcs[i])));
  context.collect_garbage();
  const scripting::ContextStats after = context.stats();
  const mem::Stats host_after = mem::total_stats();
  u64 tick = 0;
  while (state.keep_running()) {
    system.begin_tick(tick++, 1.0 / 60.0);
    system.tick(1.0f / 60.0f);
    system.end_tick();
    bench::keep(system.calls_last_tick());
  }
  ENGINE_VERIFY(system.failures_last_tick() == 0, "scripting bench: an on_tick failed");
  state.set_items(n);
  if (report_once("scripting.tick.shared", n)) {
    // Per instance: what binding it added to the VM (a view and a registry reference), as pages
    // and as Luau's live bytes, and everything the process allocated for it (the context, its
    // pages, the slot and the tick driver's record).
    std::printf(
        "{\"e7\":\"tick.shared\",\"instances\":%u,\"vm_page_bytes_per_instance\":%.1f,"
        "\"vm_live_bytes_per_instance\":%.1f,\"process_bytes_per_instance\":%.1f,"
        "\"vm_heap_bytes\":%llu}\n",
        n, (static_cast<f64>(after.heap_bytes) - static_cast<f64>(before.heap_bytes)) / n,
        (static_cast<f64>(after.live_bytes) - static_cast<f64>(before.live_bytes)) / n,
        (static_cast<f64>(host_after.bytes_current) - static_cast<f64>(host_before.bytes_current)) /
            n,
        static_cast<unsigned long long>(after.heap_bytes));
  }
}

ENGINE_BENCH_ARGS(tick_separate, "scripting.tick.separate", 1000, 10000) {
  const u32 n = instance_count(state);
  const Vector<scripting_test::Character> npcs = make_npcs(n);
  const mem::Stats host_before = mem::total_stats();
  Vector<std::unique_ptr<ScriptContext>> contexts;
  Vector<scripting::FunctionId> functions;
  Vector<scripting::BindingId> bindings;
  contexts.reserve(n);
  functions.reserve(n);
  bindings.reserve(n);
  u64 vm_heap = 0;
  u64 vm_live = 0;
  for (u32 i = 0; i < n; ++i) {
    contexts.push_back(std::make_unique<ScriptContext>());
    ScriptContext& context = *contexts.back();
    const ScriptId id = must_load(context, "routine.luau", k_routine);
    functions.push_back(must_find(context, id, "on_tick"));
    bindings.push_back(context.bind(scripting::object_ref(npcs[i])));
    context.collect_garbage();
    vm_heap += context.stats().heap_bytes;
    vm_live += context.stats().live_bytes;
  }
  const mem::Stats host_after = mem::total_stats();
  JsonValue out;
  while (state.keep_running()) {
    for (u32 i = 0; i < n; ++i) {
      const Arg args[] = {Arg::binding(bindings[i]), Arg::number(1.0 / 60.0)};
      must_call(*contexts[i], functions[i], args, &out);
    }
    bench::keep(out);
  }
  state.set_items(n);
  if (report_once("scripting.tick.separate", n)) {
    std::printf(
        "{\"e7\":\"tick.separate\",\"instances\":%u,\"vm_page_bytes_per_instance\":%.1f,"
        "\"vm_live_bytes_per_instance\":%.1f,\"process_bytes_per_instance\":%.1f}\n",
        n, static_cast<f64>(vm_heap) / n, static_cast<f64>(vm_live) / n,
        (static_cast<f64>(host_after.bytes_current) - static_cast<f64>(host_before.bytes_current)) /
            n);
  }
}

// ---- compile and reload ------------------------------------------------------------------------

// Exactly 200 lines of the kind of code a content script holds: small functions over tables,
// loops, string building and conditionals.
std::string script_200_lines() {
  std::string out;
  u32 lines = 0;
  auto line = [&](const std::string& text) {
    out += text;
    out += '\n';
    ++lines;
  };
  u32 fn = 0;
  while (lines + 10 <= 200) {
    const std::string n = std::to_string(fn++);
    line("function routine_" + n + "(npc, dt)");
    line("  local score = 0");
    line("  for i = 1, 8 do");
    line("    if npc.health > i * " + n + " then score += i else score -= 1 end");
    line("  end");
    line("  local label = \"routine_" + n + ":\" .. tostring(score)");
    line("  local plan = { name = label, weight = score * dt, tags = { \"a\", \"b\" } }");
    line("  if #plan.tags > 1 and score > 0 then plan.go = true end");
    line("  return plan");
    line("end");
  }
  while (lines < 200)
    line("-- padding");
  return out;
}

ENGINE_BENCH(compile_200, "scripting.compile.200_lines") {
  const std::string source = script_200_lines();
  ScriptContext context;
  const ScriptId id = must_load(context, "big.luau", source);
  while (state.keep_running()) {
    ENGINE_VERIFY(context.reload(id, source) == Status::Ok, "scripting bench: reload failed");
  }
  state.set_items(1);
  state.set_bytes(source.size());
}

void write_file(const std::filesystem::path& path, std::string_view text, int seconds) {
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
  }
  std::error_code ignored;
  std::filesystem::last_write_time(
      path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(seconds), ignored);
}

ENGINE_BENCH(reload_idle, "scripting.reload.poll_idle") {
  engine::test::TempDir dir("scripting-bench-idle");
  ScriptContext context;
  for (int i = 0; i < 16; ++i) {
    const std::string name = "s" + std::to_string(i) + ".luau";
    write_file(dir.native_file(name), "function f() return " + std::to_string(i) + " end\n", 0);
    ScriptId id;
    ENGINE_VERIFY(context.load_file(dir.file(name), id) == Status::Ok,
                  "scripting bench: a watched file failed to load");
  }
  while (state.keep_running()) {
    bench::keep(context.poll_changes());
  }
  state.set_items(16);
}

ENGINE_BENCH(reload_hit, "scripting.reload.poll_hit") {
  engine::test::TempDir dir("scripting-bench-hit");
  const std::string body = script_200_lines();
  const std::filesystem::path path = dir.native_file("hot.luau");
  write_file(path, body, 0);
  ScriptContext context;
  ScriptId id;
  ENGINE_VERIFY(context.load_file(dir.file("hot.luau"), id) == Status::Ok,
                "scripting bench: the hot file failed to load");
  // A reload logs one info line; writing it to the console would be most of what is measured.
  log::Category* category = log::find_category("scripting");
  const log::Level level = category != nullptr ? category->min_level() : log::Level::Trace;
  if (category != nullptr) category->set_min_level(log::Level::Warn);
  int version = 0;
  while (state.keep_running()) {
    state.pause_timing();
    ++version;
    write_file(path, body + "-- edit " + std::to_string(version) + "\n", version);
    state.resume_timing();
    const u32 reloaded = context.poll_changes();
    ENGINE_VERIFY(reloaded == 1, "scripting bench: the edit was not picked up");
  }
  if (category != nullptr) category->set_min_level(level);
  state.set_items(1);
}

// ---- the VM floor ------------------------------------------------------------------------------

ENGINE_BENCH(context_create, "scripting.context.create") {
  u64 floor = 0;
  while (state.keep_running()) {
    ScriptContext context;
    floor = context.stats().heap_bytes;
    bench::keep(floor);
  }
  state.set_items(1);
  if (report_once("scripting.context.create", 0)) {
    // The floor three ways: the VM's pages, the objects Luau counts as live in them, and
    // everything the process allocated for the context (the pages, the context's own tables and
    // the field-name atoms of every registered schema type).
    const mem::Stats process_before = mem::total_stats();
    ScriptContext context;
    context.collect_garbage();
    const scripting::ContextStats empty = context.stats();
    const mem::Stats process_empty = mem::total_stats();
    must_load(context, "routine.luau", k_routine);
    context.collect_garbage();
    const scripting::ContextStats loaded = context.stats();
    std::printf(
        "{\"e7\":\"context.floor\",\"vm_page_bytes\":%llu,\"vm_live_bytes\":%llu,"
        "\"process_bytes\":%lld,\"with_routine_page_bytes\":%llu,\"with_routine_live_bytes\":%llu}"
        "\n",
        static_cast<unsigned long long>(empty.heap_bytes),
        static_cast<unsigned long long>(empty.live_bytes),
        static_cast<long long>(process_empty.bytes_current) -
            static_cast<long long>(process_before.bytes_current),
        static_cast<unsigned long long>(loaded.heap_bytes),
        static_cast<unsigned long long>(loaded.live_bytes));
  }
}

// ---- GC pauses under a steady allocation load --------------------------------------------------

// Each call is a frame: `n` small tables, each with a string, written into a ring of 20,000 slots
// that the script keeps, so the live set is steady and every table a frame makes replaces one that
// becomes garbage. What is measured is what the incremental collector costs the frames it runs in.
constexpr std::string_view k_churn = R"(
local ring = table.create(20000, false)
local cursor = 1
function frame(n)
  for i = 1, n do
    ring[cursor] = { x = i, y = i * 2, name = "n" .. i, tags = { i } }
    cursor = cursor % 20000 + 1
  end
end
)";

u64 percentile(Vector<u64>& sorted, f64 p) {
  if (sorted.empty()) return 0;
  const usize index = static_cast<usize>(p * static_cast<f64>(sorted.size() - 1) + 0.5);
  return sorted[static_cast<u32>(std::min<usize>(index, sorted.size() - 1))];
}

ENGINE_BENCH_ARGS(gc_pauses, "scripting.gc.pauses", 500, 5000) {
  const u32 per_frame = bench::smoke_mode() ? 10u : static_cast<u32>(state.arg());
  const u32 frames = bench::smoke_mode() ? 4u : 2000u;
  scripting::ScriptLimits limits;
  limits.memory_bytes = u64{512} << 20;
  limits.step_budget = 0;
  ScriptContext context(limits);
  const ScriptId id = must_load(context, "churn.luau", k_churn);
  const scripting::FunctionId frame = must_find(context, id, "frame");
  const Arg args[] = {Arg::number(static_cast<f64>(per_frame))};
  // Fill the ring first, so the measured frames run against the steady live set.
  for (u32 i = 0; i < 20000 / std::max(per_frame, 1u) + 1; ++i)
    must_call(context, frame, args, nullptr);
  context.collect_garbage();

  Vector<u64> pauses;
  pauses.reserve(1u << 20);
  Vector<u64> frame_ns;
  frame_ns.reserve(frames);
  u64 allocated = 0;
  i64 elapsed = 0;
  while (state.keep_running()) {
    pauses.clear();
    frame_ns.clear();
    context.record_gc_pauses(&pauses);
    const u64 allocated_before = context.stats().allocated_bytes;
    const i64 start = time::monotonic_ns();
    for (u32 f = 0; f < frames; ++f) {
      const i64 t0 = time::monotonic_ns();
      must_call(context, frame, args, nullptr);
      frame_ns.push_back(static_cast<u64>(time::monotonic_ns() - t0));
    }
    elapsed = time::monotonic_ns() - start;
    allocated = context.stats().allocated_bytes - allocated_before;
    context.record_gc_pauses(nullptr);
  }
  state.set_items(frames);
  std::sort(pauses.begin(), pauses.end());
  std::sort(frame_ns.begin(), frame_ns.end());
  u64 gc_total = 0;
  for (const u64 p : pauses)
    gc_total += p;
  // Printed on every call of the body: run it with --repeats=1 --warmup=0 and the last line is the
  // measured run.
  std::printf(
      "{\"e7\":\"gc.pauses\",\"tables_per_frame\":%u,\"frames\":%u,\"steps\":%u,"
      "\"alloc_bytes_per_frame\":%.0f,\"alloc_mb_per_s\":%.1f,\"live_heap_mb\":%.2f,"
      "\"pause_p50_us\":%.2f,\"pause_p90_us\":%.2f,\"pause_p99_us\":%.2f,\"pause_max_us\":%.2f,"
      "\"gc_share\":%.3f,\"frame_p50_us\":%.2f,\"frame_p99_us\":%.2f}\n",
      per_frame, frames, pauses.size(), static_cast<f64>(allocated) / frames,
      elapsed > 0 ? static_cast<f64>(allocated) / (static_cast<f64>(elapsed) * 1e-9) / 1e6 : 0.0,
      static_cast<f64>(context.stats().heap_bytes) / (1024.0 * 1024.0),
      static_cast<f64>(percentile(pauses, 0.50)) * 1e-3,
      static_cast<f64>(percentile(pauses, 0.90)) * 1e-3,
      static_cast<f64>(percentile(pauses, 0.99)) * 1e-3,
      pauses.empty() ? 0.0 : static_cast<f64>(pauses.back()) * 1e-3,
      elapsed > 0 ? static_cast<f64>(gc_total) / static_cast<f64>(elapsed) : 0.0,
      static_cast<f64>(percentile(frame_ns, 0.50)) * 1e-3,
      static_cast<f64>(percentile(frame_ns, 0.99)) * 1e-3);
}

}  // namespace

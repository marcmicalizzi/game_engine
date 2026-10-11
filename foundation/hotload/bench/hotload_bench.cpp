// Experiment E7, the C++ hot-reload arm (docs/experiments/e7-luau-spike.md, "The C++ hot-reload
// arm"): the Luau arm's rows (foundation/scripting/bench/scripting_bench.cpp) measured on the same
// feature written as a hot-reloadable game module, game/sandbox_npc.
//
//   hotload.call.empty         host -> module -> host, nothing passed or returned
//   hotload.call.roundtrip     one number in, the same number back
//   hotload.read.baseline      the read benches' call with no property read, to subtract
//   hotload.read.ten           ten scalar fields of a bound `Ten` through the host's views
//   hotload.read.mixed         a string, an enum, a vector, a nested struct's field, a bool
//   hotload.tick.module/N      on_tick(self, dt) over N bound characters, through the module
//   hotload.tick.static/N      the same routine compiled into the bench, reading the struct:
//                              what the module boundary and the views cost against none
//   hotload.reload.poll_idle   poll_changes() on the watched library, unchanged
//   hotload.reload.swap        reload(): copy, load, describe, check, migrate, on_load, swap,
//                              unload the old build — everything but the compile
//   hotload.migrate/N          N states through the old and new descriptors (per state)
//
// The module's size is printed as one `{"e7":...}` line. Its rebuild time is not a bench: it is a
// compiler run, measured with the build tool on the module target (the write-up has how). Take
// the numbers from a release build on a quiet machine:
//
//   engine_hotload_bench.exe --require-quiet --json=e7-cpp.jsonl
//
// Debug builds only smoke-run benchmarks, so every fixture here shrinks under --smoke.

#include <core/base/assert.h>
#include <core/base/macros.h>
#include <core/containers/vector.h>
#include <core/log/log.h>
#include <core/memory/memory.h>
#include <foundation/bench/bench.h>
#include <foundation/hotload/module_host.h>

#include <test_temp_dir.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <schemas/hotload_test.h>
#include <string>

using namespace engine;
using namespace engine::hotload;

namespace {

#if defined(ENGINE_SANDBOX_NPC_PATH)
constexpr const char* k_module = ENGINE_SANDBOX_NPC_PATH;
#else
constexpr const char* k_module = "";
#endif

// One host with the sandbox module loaded from a copy in the bench's own scratch directory.
struct Loaded {
  engine::test::TempDir dir{"hotload-bench"};
  std::string live;
  std::unique_ptr<ModuleHost> host;

  Loaded() {
    const std::filesystem::path from(k_module);
    live = dir.file("sandbox_npc" + from.extension().string());
    std::error_code ec;
    std::filesystem::copy_file(from, std::filesystem::path(live),
                               std::filesystem::copy_options::overwrite_existing, ec);
    ENGINE_VERIFY(!ec, "hotload bench: cannot copy the sandbox module (is it built?)");
    host = std::make_unique<ModuleHost>(HostConfig{.shadow_directory = dir.file("shadow")});
    const Status status = host->load(live);
    if (status != Status::Ok) std::fprintf(stderr, "%s\n", host->last_error().c_str());
    ENGINE_VERIFY(status == Status::Ok, "hotload bench: the sandbox module did not load");
  }

  template <class F>
  F find(const char* name) {
    FunctionId id;
    ENGINE_VERIFY(host->find<F>(name, id) == Status::Ok, "hotload bench: a fixture is missing");
    return host->function<F>(id);
  }
};

bool report_once(const char* bench, i64 arg) {
  static const char* seen_names[32];
  static i64 seen_args[32];
  static u32 count = 0;
  for (u32 i = 0; i < count; ++i)
    if (seen_names[i] == bench && seen_args[i] == arg) return false;
  if (count < 32) {
    seen_names[count] = bench;
    seen_args[count] = arg;
    ++count;
  }
  return true;
}

// A reload logs one info line; writing it to the console would be most of what is measured.
struct QuietHotloadLog {
  log::Category* category = log::find_category("hotload");
  log::Level level = category != nullptr ? category->min_level() : log::Level::Trace;
  QuietHotloadLog() {
    if (category != nullptr) category->set_min_level(log::Level::Warn);
  }
  ~QuietHotloadLog() {
    if (category != nullptr) category->set_min_level(level);
  }
};

// ---- calls
// ---------------------------------------------------------------------------------------

ENGINE_BENCH(call_empty, "hotload.call.empty") {
  Loaded m;
  const VoidFn empty = m.find<VoidFn>("empty");
  ModuleContext* context = m.host->context();
  while (state.keep_running()) {
    empty(context);
  }
  state.set_items(1);
}

ENGINE_BENCH(call_roundtrip, "hotload.call.roundtrip") {
  Loaded m;
  const NumberFn echo = m.find<NumberFn>("echo");
  ModuleContext* context = m.host->context();
  f64 x = 0.5;
  while (state.keep_running()) {
    x = echo(context, x) + 1.0;
    bench::keep(x);
  }
  state.set_items(1);
}

template <class T>
void read_bench(bench::State& state, const char* function, const T& object, u64 items) {
  Loaded m;
  const ViewNumberFn fn = m.find<ViewNumberFn>(function);
  ModuleContext* context = m.host->context();
  const View view = m.host->bind(object);
  f64 out = 0.0;
  while (state.keep_running()) {
    out = fn(context, view);
    bench::keep(out);
  }
  state.set_items(items);
}

ENGINE_BENCH(read_baseline, "hotload.read.baseline") {
  const hotload_test::Ten ten;
  read_bench(state, "base", ten, 1);
}

ENGINE_BENCH(read_ten, "hotload.read.ten") {
  const hotload_test::Ten ten;
  read_bench(state, "ten", ten, 10);
}

ENGINE_BENCH(read_mixed, "hotload.read.mixed") {
  hotload_test::Character npc;
  npc.name = "Ada";
  npc.faction = hotload_test::Faction::Drifters;
  npc.position = Vec3{1.0f, 2.0f, 3.0f};
  read_bench(state, "mixed", npc, 5);
}

// ---- on_tick over many instances
// -----------------------------------------------------------------

Vector<hotload_test::Character> make_npcs(u32 count) {
  Vector<hotload_test::Character> npcs;
  npcs.resize(count);
  for (u32 i = 0; i < count; ++i) {
    npcs[i].name = "npc";
    npcs[i].health = static_cast<f32>(40 + (i % 60));
    npcs[i].faction = static_cast<hotload_test::Faction>(i % 3);
    npcs[i].position = Vec3{static_cast<f32>(i), 1.0f, 0.0f};
  }
  return npcs;
}

u32 instance_count(bench::State& state) {
  return bench::smoke_mode() ? 8u : static_cast<u32>(state.arg());
}

ENGINE_BENCH_ARGS(tick_module, "hotload.tick.module", 1000, 10000) {
  const u32 n = instance_count(state);
  const Vector<hotload_test::Character> npcs = make_npcs(n);
  Loaded m;
  FunctionId on_tick;
  ENGINE_VERIFY(m.host->find<TickFn>("on_tick", on_tick) == Status::Ok, "hotload bench: on_tick");
  const mem::Stats before = mem::total_stats();
  Vector<View> views;
  views.reserve(n);
  for (u32 i = 0; i < n; ++i)
    views.push_back(m.host->bind(npcs[i]));
  const mem::Stats after = mem::total_stats();
  ModuleContext* context = m.host->context();
  const u64 allocations_before = mem::allocation_counter();
  while (state.keep_running()) {
    // The table is read once a tick: a reload happens between ticks, never inside one.
    const TickFn fn = m.host->function<TickFn>(on_tick);
    for (u32 i = 0; i < n; ++i)
      fn(context, views[i], 1.0f / 60.0f);
    bench::keep(views.data());
  }
  const u64 allocations = mem::allocation_counter() - allocations_before;
  ENGINE_VERIFY(allocations == 0, "hotload bench: a steady-state tick allocated");
  state.set_items(n);
  if (report_once("hotload.tick.module", n)) {
    std::printf(
        "{\"e7\":\"hotload.tick.module\",\"instances\":%u,\"host_bytes_per_instance\":%.1f}\n", n,
        (static_cast<f64>(after.bytes_current) - static_cast<f64>(before.bytes_current)) / n);
  }
}

// The routine as engine code would write it, with the struct in hand and no boundary.
ENGINE_NO_INLINE void static_on_tick(const hotload_test::Character& self, f32 dt, u64& flee) {
  (void)dt;
  if (!self.alive) return;
  const bool threat = self.health < 30.0f && self.faction != hotload_test::Faction::Ironbound;
  if (threat && self.position.y < 0.0f) ++flee;
}

ENGINE_BENCH_ARGS(tick_static, "hotload.tick.static", 1000, 10000) {
  const u32 n = instance_count(state);
  const Vector<hotload_test::Character> npcs = make_npcs(n);
  u64 flee = 0;
  while (state.keep_running()) {
    for (u32 i = 0; i < n; ++i)
      static_on_tick(npcs[i], 1.0f / 60.0f, flee);
    bench::keep(flee);
  }
  state.set_items(n);
}

// ---- reload
// --------------------------------------------------------------------------------------

ENGINE_BENCH(reload_poll_idle, "hotload.reload.poll_idle") {
  Loaded m;
  while (state.keep_running()) {
    bench::keep(m.host->poll_changes());
  }
  state.set_items(1);
}

ENGINE_BENCH(reload_swap, "hotload.reload.swap") {
  Loaded m;
  const QuietHotloadLog quiet;
  while (state.keep_running()) {
    ENGINE_VERIFY(m.host->reload() == Status::Ok, "hotload bench: reload failed");
  }
  state.set_items(1);
  if (report_once("hotload.size", 0)) {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(std::filesystem::path(k_module), ec);
    std::printf("{\"e7\":\"hotload.size\",\"module_bytes\":%llu,\"state_bytes\":%llu}\n",
                static_cast<unsigned long long>(ec ? 0 : bytes),
                static_cast<unsigned long long>(m.host->last_report().state_bytes));
  }
}

ENGINE_BENCH_ARGS(migrate, "hotload.migrate", 1, 1000) {
  const u32 n = bench::smoke_mode() ? 2u : static_cast<u32>(state.arg());
  Loaded m;
  const schema::TypeInfo& type = *m.host->state_type();
  // N states of the module's own type, each moved through the descriptors into a fresh one.
  Vector<u8> from;
  Vector<u8> to;
  from.resize(static_cast<u32>(type.size * n));
  to.resize(static_cast<u32>(type.size * n));
  for (u32 i = 0; i < n; ++i) {
    type.ops->construct(from.data() + static_cast<usize>(i) * type.size);
    type.ops->construct(to.data() + static_cast<usize>(i) * type.size);
  }
  ReloadReport report;
  std::string error;
  while (state.keep_running()) {
    for (u32 i = 0; i < n; ++i) {
      const bool ok = migrate_state(type, from.data() + static_cast<usize>(i) * type.size, type,
                                    to.data() + static_cast<usize>(i) * type.size, report, error);
      ENGINE_VERIFY(ok, "hotload bench: migration failed");
    }
  }
  state.set_items(n);
}

}  // namespace

// The hot-reloadable game module host (docs/subsystems/hotload.md): load, call, read through views,
// poll, swap a rebuilt module in with its state carried through the schema descriptors, refuse a
// build that cannot take over and keep the old one running, and leave nothing loaded or locked.
//
// The modules are real shared libraries built by engine_game_module() (tests/modules/), and one
// test compiles a new build from source with the helper's exported command. Every library the
// host loads is a copy in the test's own scratch directory.
#include "hotload_test_support.h"

#include <foundation/hotload/module_host.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <fstream>
#include <schemas/hotload_test.h>
#include <string>

#if defined(__linux__)
#include <dlfcn.h>
#endif

using namespace engine;
using namespace engine::hotload;
using engine::hotload::test::build_module;
using engine::hotload::test::in_bundle;
using engine::hotload::test::place;
using engine::hotload::test::write_text;

namespace {

struct Counter {
  ModuleHost host;
  FunctionId bump, get, build, read_health;

  explicit Counter(const engine::test::TempDir& dir)
      : host(HostConfig{.shadow_directory = dir.file("shadow")}) {}

  Status load(const std::string& path) {
    const Status status = host.load(path);
    if (status != Status::Ok) return status;
    REQUIRE(host.find<VoidFn>("bump", bump) == Status::Ok);
    REQUIRE(host.find<NumberFn>("get", get) == Status::Ok);
    REQUIRE(host.find<NumberFn>("build", build) == Status::Ok);
    REQUIRE(host.find<ViewNumberFn>("read_health", read_health) == Status::Ok);
    return status;
  }
  void call_bump() { host.function<VoidFn>(bump)(host.context()); }
  f64 call_get() { return host.function<NumberFn>(get)(host.context(), 0.0); }
  f64 call_build() { return host.function<NumberFn>(build)(host.context(), 0.0); }
  f64 call_read_health(View view) {
    return host.function<ViewNumberFn>(read_health)(host.context(), view);
  }
};

bool contains(const std::string& text, std::string_view part) {
  return text.find(part) != std::string::npos;
}

bool listed(const Vector<std::string>& names, std::string_view name) {
  for (const std::string& n : names)
    if (n == name) return true;
  return false;
}

}  // namespace

TEST_CASE("hotload: load, call, read through a view, and a poll that finds no change") {
  if (in_bundle()) return;
  engine::test::TempDir dir("hotload-load");
  const std::string live = place(dir, HOTLOAD_MODULE_COUNTER_V1, "live");
  Counter counter(dir);
  REQUIRE(counter.load(live) == Status::Ok);
  CHECK(counter.host.generation() == 1);
  CHECK(counter.host.module_name() == "counter");
  CHECK(counter.call_build() == 1.0);
  counter.call_bump();
  counter.call_bump();
  CHECK(counter.call_get() == 2.0);

  hotload_test::Character npc;
  npc.health = 42.0f;
  const View view = counter.host.bind(npc);
  CHECK(counter.call_read_health(view) == 42.0);

  CHECK(counter.host.poll_changes() == 0);
  CHECK(counter.host.generation() == 1);

  FunctionId wrong;
  CHECK(counter.host.find<VoidFn>("get", wrong) == Status::SignatureMismatch);
  CHECK(counter.host.find<VoidFn>("no_such_function", wrong) == Status::NotFound);

  JsonValue state;
  REQUIRE(counter.host.state_json(state));
  u64 count = 0;
  REQUIRE(state.find("count") != nullptr);
  CHECK(state.find("count")->get_u64(count));
  CHECK(count == 2);
}

TEST_CASE(
    "hotload: a rebuilt module is swapped in with its state carried, a field added and one "
    "removed") {
  if (in_bundle()) return;
  engine::test::TempDir dir("hotload-swap");
  const std::string live = place(dir, HOTLOAD_MODULE_COUNTER_V1, "live");
  Counter counter(dir);
  REQUIRE(counter.load(live) == Status::Ok);
  const std::string first_copy = counter.host.loaded_copy_path();
  for (int i = 0; i < 3; ++i)
    counter.call_bump();
  hotload_test::Character npc;
  npc.health = 21.0f;
  const View view = counter.host.bind(npc);

  // The build writes the watched file in place; the host is running a copy.
  place(dir, HOTLOAD_MODULE_COUNTER_V2, "live", 10);
  REQUIRE(counter.host.poll_changes() == 1);
  INFO(counter.host.last_error());
  CHECK(counter.host.generation() == 2);
  CHECK(counter.call_build() == 2.0);
  // count (3) carried, scale widened from f32 to f64, bonus added at its default (5).
  CHECK(counter.call_get() == 8.0);
  CHECK(counter.call_read_health(view) == 42.0);  // the new code, through the same view
  const ReloadReport& report = counter.host.last_report();
  CHECK(listed(report.added, "bonus"));
  CHECK(listed(report.removed, "label"));
  CHECK(listed(report.converted, "scale"));
  CHECK(report.state_bytes > 0);
  CHECK_FALSE(std::filesystem::exists(first_copy));  // the old build is unloaded and its copy gone

  // And back: bonus dropped, label back at its default, the count still carried.
  counter.call_bump();
  place(dir, HOTLOAD_MODULE_COUNTER_V1, "live", 20);
  REQUIRE(counter.host.poll_changes() == 1);
  CHECK(counter.call_build() == 1.0);
  CHECK(counter.call_get() == 4.0);
  CHECK(listed(counter.host.last_report().removed, "bonus"));
  CHECK(listed(counter.host.last_report().added, "label"));
}

TEST_CASE(
    "hotload: an incompatible state change is refused, naming the field, and the old build runs "
    "on") {
  if (in_bundle()) return;
  engine::test::TempDir dir("hotload-retyped");
  const std::string live = place(dir, HOTLOAD_MODULE_COUNTER_V1, "live");
  Counter counter(dir);
  REQUIRE(counter.load(live) == Status::Ok);
  counter.call_bump();
  counter.call_bump();

  place(dir, HOTLOAD_MODULE_COUNTER_RETYPED, "live", 10);
  CHECK(counter.host.poll_changes() == 0);
  const std::string error = counter.host.last_error();
  MESSAGE("refused: " << error);
  CHECK(contains(error, "StateIncompatible"));
  CHECK(contains(error, "'count'"));
  CHECK(counter.host.generation() == 1);
  CHECK(counter.call_build() == 1.0);
  counter.call_bump();
  CHECK(counter.call_get() == 3.0);
  // A refused build is not retried until the file changes again.
  CHECK(counter.host.poll_changes() == 0);
  CHECK(counter.host.stats().failed_loads == 1);
}

TEST_CASE("hotload: builds the host must refuse, first and as a reload") {
  if (in_bundle()) return;
  struct Case {
    const char* name;
    const char* path;
    Status first;   // loading it alone
    Status reload;  // loading it over a running counter
    const char* says;
  };
  const Case cases[] = {
      {"no entry point", HOTLOAD_MODULE_REFUSAL_NO_ENTRY, Status::NoEntryPoint,
       Status::NoEntryPoint, "engine_hotload_describe"},
      {"describe fails", HOTLOAD_MODULE_REFUSAL_DESCRIBE, Status::DescribeFailed,
       Status::DescribeFailed, "refuses on purpose"},
      {"another ABI", HOTLOAD_MODULE_REFUSAL_ABI, Status::AbiMismatch, Status::AbiMismatch,
       "rebuild the module"},
      {"paths that do not resolve", HOTLOAD_MODULE_REFUSAL_PATHS, Status::LoadHookFailed,
       Status::LoadHookFailed, "'helth'"},
      {"drops a held function", HOTLOAD_MODULE_REFUSAL_DROPS, Status::Ok, Status::MissingFunction,
       "'get'"},
  };
  for (const Case& c : cases) {
    SUBCASE(c.name) {
      engine::test::TempDir dir("hotload-refusal");
      {
        ModuleHost alone(HostConfig{.shadow_directory = dir.file("shadow-alone")});
        CHECK(alone.load(place(dir, c.path, "alone")) == c.first);
        if (c.first != Status::Ok) {
          CHECK_FALSE(alone.loaded());
          CHECK(contains(alone.last_error(), c.says));
        }
      }
      const std::string live = place(dir, HOTLOAD_MODULE_COUNTER_V1, "live");
      Counter counter(dir);
      REQUIRE(counter.load(live) == Status::Ok);
      counter.call_bump();
      place(dir, c.path, "live", 10);
      CHECK(counter.host.reload() == c.reload);
      MESSAGE(std::string(c.name) << ": " << counter.host.last_error());
      CHECK(contains(counter.host.last_error(), c.says));
      CHECK(counter.host.generation() == 1);
      CHECK(counter.call_get() == 1.0);
    }
  }
  SUBCASE("every unresolved path is listed at once") {
    engine::test::TempDir dir("hotload-paths");
    ModuleHost host(HostConfig{.shadow_directory = dir.file("shadow")});
    CHECK(host.load(place(dir, HOTLOAD_MODULE_REFUSAL_PATHS, "paths")) == Status::LoadHookFailed);
    CHECK(contains(host.last_error(), "has no field 'helth'"));
    CHECK(contains(host.last_error(), "has no field 'strenght'"));
    CHECK(contains(host.last_error(), "is not an enum"));
  }
  SUBCASE("a file that is not a library") {
    engine::test::TempDir dir("hotload-garbage");
    write_text(dir.file("garbage.bin"), "this is not a shared library");
    ModuleHost host(HostConfig{.shadow_directory = dir.file("shadow")});
    CHECK(host.load(dir.file("garbage.bin")) == Status::LoadFailed);
    MESSAGE("not a library: " << host.last_error());
  }
}

TEST_CASE("hotload: unloading leaves no handle open") {
  if (in_bundle()) return;
  engine::test::TempDir dir("hotload-unload");
  const std::string live = place(dir, HOTLOAD_MODULE_COUNTER_V1, "live");
  std::string copy;
  {
    ModuleHost host(HostConfig{.shadow_directory = dir.file("shadow")});
    REQUIRE(host.load(live) == Status::Ok);
    copy = host.loaded_copy_path();
    CHECK(std::filesystem::exists(copy));
    // The watched file is not held while the module runs: the build can replace it.
    std::error_code ec;
    std::filesystem::remove(live, ec);
    CHECK_FALSE(ec);
    host.unload();
    CHECK_FALSE(host.loaded());
    CHECK_FALSE(std::filesystem::exists(copy));
  }
#if defined(__linux__) && defined(HOTLOAD_DLCLOSE_IS_NOOP)
  // Tracy's client defines dlclose as a no-op on Linux (TracyOverride.cpp), so that it can still
  // symbolize addresses in a library that was closed: in a build with ENGINE_TRACY every unloaded
  // build stays mapped, by the profiler's design (docs/subsystems/hotload.md).
  MESSAGE("ENGINE_TRACY: dlclose is Tracy's no-op here; the unloaded build stays mapped");
#elif defined(__linux__)
  // A dlclose that does not unload (a GNU-unique symbol pins a library) would leave the module's
  // code mapped and glibc still holding it. Only an executable mapping counts: a profiler's
  // symbolizer (Tracy's libbacktrace, in the release presets) may keep a read-only view of every
  // image it has seen, which is not the module staying loaded.
  std::ifstream maps("/proc/self/maps");
  std::string line;
  bool code_mapped = false;
  while (std::getline(maps, line)) {
    if (line.find(copy) == std::string::npos) continue;
    MESSAGE("still mapped after unload: " << line);
    const usize space = line.find(' ');
    if (space != std::string::npos && line.size() > space + 3 && line[space + 3] == 'x')
      code_mapped = true;
  }
  CHECK_FALSE(code_mapped);
  // RTLD_NOLOAD finds a loaded object by name before touching the (deleted) file.
  void* still = dlopen(copy.c_str(), RTLD_NOW | RTLD_NOLOAD);
  CHECK(still == nullptr);
  if (still != nullptr) dlclose(still);
#endif
}

TEST_CASE("hotload: views read through the host's descriptors, and go stale") {
  if (in_bundle()) return;
  ModuleHost host;
  hotload_test::Character a;
  a.name = "Ada";
  a.health = 12.5f;
  a.faction = hotload_test::Faction::Drifters;
  a.position = Vec3{1.0f, -2.0f, 3.0f};
  a.attributes.strength = 17;
  a.inventory.push_back(Id128{1, 2});
  hotload_test::Character b;
  b.health = 99.0f;
  hotload_test::Ten ten;

  FieldRef health, strength, faction, name, position, inventory, home, bad;
  const char* type = "engine.hotload_test.Character";
  REQUIRE(host.resolve(type, "health", health) == Result::Ok);
  REQUIRE(host.resolve(type, "attributes.strength", strength) == Result::Ok);
  REQUIRE(host.resolve(type, "faction", faction) == Result::Ok);
  REQUIRE(host.resolve(type, "name", name) == Result::Ok);
  REQUIRE(host.resolve(type, "position", position) == Result::Ok);
  REQUIRE(host.resolve(type, "inventory", inventory) == Result::Ok);
  REQUIRE(host.resolve(type, "home", home) == Result::Ok);
  CHECK(host.resolve(type, "position.x", bad) == Result::NotFound);  // a vec3 is not a struct
  CHECK(host.resolve(type, "helth", bad) == Result::NotFound);
  CHECK(host.resolve("engine.hotload_test.Nobody", "health", bad) == Result::NotFound);
  FieldRef again;
  REQUIRE(host.resolve(type, "health", again) == Result::Ok);
  CHECK(again.index == health.index);  // one entry per path, however often it is resolved

  View view = host.bind(a);
  Value v;
  REQUIRE(host.read(view, health, v) == Result::Ok);
  CHECK(v.kind == ValueKind::Float);
  CHECK(v.f == 12.5);
  REQUIRE(host.read(view, strength, v) == Result::Ok);
  CHECK(v.kind == ValueKind::Int);
  CHECK(v.i == 17);
  REQUIRE(host.read(view, faction, v) == Result::Ok);
  CHECK(v.kind == ValueKind::Enum);
  CHECK(v.i == 2);
  CHECK(std::string(v.str.data, v.str.length) == "Drifters");
  REQUIRE(host.read(view, name, v) == Result::Ok);
  CHECK(std::string(v.str.data, v.str.length) == "Ada");
  REQUIRE(host.read(view, position, v) == Result::Ok);
  CHECK(v.count == 3);
  CHECK(v.v[1] == -2.0f);
  REQUIRE(host.read(view, inventory, v) == Result::Ok);
  CHECK(v.kind == ValueKind::Array);
  CHECK(v.count == 1);
  REQUIRE(host.read(view, home, v) == Result::Ok);
  CHECK(v.kind == ValueKind::Nil);

  // The wrong type, a rebind and an unbind are refusals, never memory.
  const View ten_view = host.bind(ten);
  CHECK(host.read(ten_view, health, v) == Result::WrongType);
  const View before = view;
  host.rebind(view, &b);
  CHECK(host.read(before, health, v) == Result::StaleView);
  REQUIRE(host.read(view, health, v) == Result::Ok);
  CHECK(v.f == 99.0);
  host.unbind(view);
  CHECK(host.read(view, health, v) == Result::StaleView);
  CHECK(v.kind == ValueKind::Nil);
  CHECK(host.stats().refused_reads == 3);
}

TEST_CASE("hotload: a module's state holds fixed-layout values only, and migrates field by field") {
  std::string error;
  CHECK(check_state_type(schema::type_of<hotload_test::Ten>(), error));
  CHECK_FALSE(check_state_type(schema::type_of<hotload_test::Character>(), error));
  MESSAGE("Character as module state: " << error);
  CHECK(contains(error, "'name'"));

  hotload_test::Ten from;
  from.a = 11.0f;
  from.j = 200;
  hotload_test::Ten to;
  ReloadReport report;
  REQUIRE(migrate_state(schema::type_of<hotload_test::Ten>(), &from,
                        schema::type_of<hotload_test::Ten>(), &to, report, error));
  CHECK(to.a == 11.0f);
  CHECK(to.j == 200);
  CHECK(report.added.empty());
  CHECK(report.removed.empty());
  // Two different types are not one state across builds.
  hotload_test::Attributes other;
  CHECK_FALSE(migrate_state(schema::type_of<hotload_test::Ten>(), &from,
                            schema::type_of<hotload_test::Attributes>(), &other, report, error));
}

TEST_CASE("hotload: a build compiled from source onto the watched file is swapped in") {
  if (in_bundle()) return;
  engine::test::TempDir dir("hotload-rebuild");
  const std::string live = place(dir, HOTLOAD_MODULE_COUNTER_V1, "live");
  Counter counter(dir);
  REQUIRE(counter.load(live) == Status::Ok);
  for (int i = 0; i < 4; ++i)
    counter.call_bump();

  // A third build, written here and compiled with the configured compiler: the state gains a
  // field, `get` changes, and the output is the file the host watches.
  const std::string src = dir.file("src");
  std::filesystem::create_directories(src);
  write_text(src + "/counter.schema", R"(namespace engine.hotload_counter
struct CounterState @version(3) {
  count: u32 = 0
  scale: f32 = 1
  label: u16 = 7
  extra: f32 = 0.5
}
)");
  write_text(src + "/counter.cpp", R"(#include <foundation/hotload/module_state.h>
#include <schemas/counter.h>
using namespace engine;
using namespace engine::hotload;
using State = hotload_counter::CounterState;
namespace {
void bump(ModuleContext* c) { state<State>(c).count += 10; }
f64 get(ModuleContext* c, f64 x) {
  const State& s = state<State>(c);
  return static_cast<f64>(s.count) * static_cast<f64>(s.scale) + static_cast<f64>(s.extra) + 1000.0 + x;
}
f64 build(ModuleContext*, f64) { return 3.0; }
f64 read_health(ModuleContext*, View) { return 0.0; }
}  // namespace
ENGINE_HOTLOAD_DESCRIBE(api, out) {
  (void)api;
  static const Export exports[] = {
      export_function<VoidFn>("bump", &bump),
      export_function<NumberFn>("get", &get),
      export_function<NumberFn>("build", &build),
      export_function<ViewNumberFn>("read_health", &read_health),
  };
  out->abi_version = k_abi_version;
  out->desc_size = static_cast<u32>(sizeof(ModuleDesc));
  out->name = "counter";
  out->exports = exports;
  out->export_count = 4;
  describe_state<State>(out);
  return 0;
}
)");
  std::string output;
  const bool built = build_module(src, live, output);
  INFO(output);
  REQUIRE(built);
  REQUIRE(counter.host.poll_changes() == 1);
  CHECK(counter.call_build() == 3.0);
  CHECK(counter.call_get() == 4.0 + 0.5 + 1000.0);
  counter.call_bump();
  CHECK(counter.call_get() == 14.0 + 0.5 + 1000.0);
  CHECK(listed(counter.host.last_report().added, "extra"));
}

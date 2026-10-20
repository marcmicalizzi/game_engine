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
  CHECK(counter.host.module_abi() == k_abi_version);
}

TEST_CASE("hotload: modules built against ABI 3, 4 and 5 load on an ABI 6 host and run unchanged") {
  // ABI 4, 5 and 6 appended to the table and moved nothing (module_abi.h, `k_abi_oldest`): a build
  // that declares 3, 4 or 5 is what a module compiled against that header hands the host. It loads,
  // its exports run, it reads through a view, and a rebuild against 6 takes over from it with its
  // state.
  if (in_bundle()) return;
  struct Old {
    const char* path;
    u32 abi;
  };
  for (const Old old : {Old{HOTLOAD_MODULE_COUNTER_ABI3, 3}, Old{HOTLOAD_MODULE_COUNTER_ABI4, 4},
                        Old{HOTLOAD_MODULE_COUNTER_ABI5, 5}}) {
    INFO("ABI " << old.abi);
    engine::test::TempDir dir("hotload-old-abi");
    const std::string live = place(dir, old.path, "live");
    Counter counter(dir);
    REQUIRE_MESSAGE(counter.load(live) == Status::Ok, counter.host.last_error());
    CHECK(counter.host.module_abi() == old.abi);
    counter.call_bump();
    CHECK(counter.call_get() == 1.0);
    hotload_test::Character npc;
    npc.health = 7.0f;
    CHECK(counter.call_read_health(counter.host.bind(npc)) == 7.0);
    place(dir, HOTLOAD_MODULE_COUNTER_V1, "live", 10);
    REQUIRE_MESSAGE(counter.host.reload() == Status::Ok, counter.host.last_error());
    CHECK(counter.host.module_abi() == k_abi_version);
    CHECK(counter.call_get() == 1.0);  // the state carried
  }
}

TEST_CASE(
    "hotload: ABI 6's images and fonts forward, and an older element arrives at the defaults") {
  ModuleHost host;
  const HostApi* api = host.context()->api;
  CHECK(api->size == sizeof(HostApi));
  CHECK(ENGINE_HOTLOAD_HAS(api, hud_image));
  CHECK(ENGINE_HOTLOAD_HAS(api, hud_font));
  // A table as an ABI 5 host built it ends before the images: the helpers do not call them.
  HostApi v5 = *api;
  v5.size = static_cast<u32>(offsetof(HostApi, hud_image));
  CHECK_FALSE(ENGINE_HOTLOAD_HAS(&v5, hud_image));
  CHECK(ENGINE_HOTLOAD_HAS(&v5, ground_height));
  ModuleContext old_host{&v5, nullptr};
  HudImage panel;
  panel.id = 2;
  panel.path = "hud/panel.png";
  u32 font = 9;
  CHECK(hud_image(&old_host, panel) == Result::Unsupported);
  CHECK(hud_font(&old_host, "hud/font.ttf", 24.0f, font) == Result::Unsupported);
  CHECK(font == 9);

  ModuleContext context{api, nullptr};
  CHECK(hud_image(&context, panel) == Result::Unsupported);  // no game services
  CHECK(hud_font(&context, "hud/font.ttf", 24.0f, font) == Result::Unsupported);
  // The host's own checks, before anything is forwarded.
  HudImage small = panel;
  small.size = 40;
  CHECK(api->hud_image(api->host, &small) == Result::InvalidArgument);
  CHECK(api->hud_image(api->host, nullptr) == Result::InvalidArgument);
  CHECK(api->hud_font(api->host, "a.ttf", 5, 24.0f, nullptr) == Result::InvalidArgument);

  struct Seen {
    u32 image = 0;
    u64 path_length = 0;
    std::string font_path;
    f32 font_px = 0.0f;
    u32 element_size = 0;
    u32 element_font = 0;
    f32 spacing = -1.0f;
    u8 shadow_alpha = 0;
  } seen;
  GameServices services;
  services.user = &seen;
  services.hud_image = [](void* user, const HudImage& i) {
    static_cast<Seen*>(user)->image = i.id;
    static_cast<Seen*>(user)->path_length = i.path_length;
    return Result::Ok;
  };
  services.hud_font = [](void* user, std::string_view path, f32 size_px, u32* out) {
    static_cast<Seen*>(user)->font_path = std::string(path);
    static_cast<Seen*>(user)->font_px = size_px;
    *out = 1;
    return Result::Ok;
  };
  services.hud_element = [](void* user, const HudElement& e) {
    Seen& s = *static_cast<Seen*>(user);
    s.element_size = e.size;
    s.element_font = e.font;
    s.spacing = e.letter_spacing_px;
    s.shadow_alpha = e.shadow_color[3];
    return Result::Ok;
  };
  host.set_game_services(services);
  CHECK(hud_image(&context, panel) == Result::Ok);
  CHECK(seen.image == 2);
  CHECK(seen.path_length == 13);  // the helper measured the C string
  CHECK(hud_font(&context, "hud/font.ttf", 24.0f, font) == Result::Ok);
  CHECK(font == 1);
  CHECK(seen.font_path == "hud/font.ttf");
  CHECK(seen.font_px == 24.0f);

  // An element as an ABI 5 module built it: ABI 4's 72 bytes, followed in its memory by whatever
  // the module had there. The host reads the 72 and nothing past them, so the new fields arrive
  // at their defaults.
  struct Older {
    HudElement element;
  } older;
  older.element.id = 1;
  older.element.text = "[E] Pick up";
  older.element.text_length = 11;
  older.element.font = 7;  // what an ABI 5 module's memory held past its struct
  older.element.letter_spacing_px = 99.0f;
  older.element.shadow_color[3] = 1;
  older.element.size = k_hud_element_v4_size;
  CHECK(api->hud_element(api->host, &older.element) == Result::Ok);
  CHECK(seen.element_size == k_hud_element_v4_size);
  CHECK(seen.element_font == 0);
  CHECK(seen.spacing == 0.0f);
  CHECK(seen.shadow_alpha == 160);
  // And one of ABI 6's own size arrives whole.
  HudElement styled;
  styled.id = 3;
  styled.font = 1;
  styled.letter_spacing_px = 2.0f;
  CHECK(hud_element(&context, styled) == Result::Ok);
  CHECK(seen.element_size == sizeof(HudElement));
  CHECK(seen.element_font == 1);
  CHECK(seen.spacing == 2.0f);
  // Smaller than ABI 4's is refused, as before.
  older.element.size = k_hud_element_v4_size - 8;
  CHECK(api->hud_element(api->host, &older.element) == Result::InvalidArgument);
}

TEST_CASE("hotload: ABI 5's actor entries and the ground forward, and answer Unsupported without") {
  ModuleHost host;
  const HostApi* api = host.context()->api;
  CHECK(api->size == sizeof(HostApi));
  CHECK(ENGINE_HOTLOAD_HAS(api, actor_find));
  CHECK(ENGINE_HOTLOAD_HAS(api, ground_height));
  // A table as an ABI 4 host built it ends before the actors: the helpers do not call them.
  HostApi v4 = *api;
  v4.size = static_cast<u32>(offsetof(HostApi, actor_find));
  CHECK_FALSE(ENGINE_HOTLOAD_HAS(&v4, actor_find));
  CHECK(ENGINE_HOTLOAD_HAS(&v4, request_graphics));
  ModuleContext old_host{&v4, nullptr};
  u32 actor = 7;
  f64 height = -1.0;
  const f64 at[3] = {1.0, 2.0, 3.0};
  ActorState state;
  CHECK(actor_find(&old_host, "bar", actor) == Result::Unsupported);
  CHECK(actor_place(&old_host, 0, at, 0.0f) == Result::Unsupported);
  CHECK(actor_state(&old_host, 0, state) == Result::Unsupported);
  CHECK(ground_height(&old_host, 0.0, 0.0, height) == Result::Unsupported);
  CHECK(actor == 7);

  // No game services: every entry answers Unsupported, and a lookup is not remembered.
  ModuleContext context{api, nullptr};
  CHECK(actor_find(&context, "bar", actor) == Result::Unsupported);
  CHECK(actor_spawn(&context, "bar", at, 0.0f, actor) == Result::Unsupported);
  CHECK(actor_play(&context, 0, "bend", 0.0f, true) == Result::Unsupported);
  CHECK(actor_stop(&context, 0, 0.5f) == Result::Unsupported);
  CHECK(actor_speed(&context, 0, 2.0f) == Result::Unsupported);
  CHECK(ground_height(&context, 0.0, 0.0, height) == Result::Unsupported);
  // The host's own checks, before anything is forwarded.
  ActorState small;
  small.size = 16;
  CHECK(api->actor_state(api->host, 0, &small) == Result::InvalidArgument);
  CHECK(api->actor_play(api->host, 0, "be\0nd", 5, 0.0f, 1) == Result::InvalidArgument);
  CHECK(api->actor_place(api->host, 0, nullptr, 0.0f) == Result::InvalidArgument);
  CHECK(api->ground_height(api->host, 0.0, 0.0, nullptr) == Result::InvalidArgument);

  struct Seen {
    std::string clip;
    f32 fade = 0.0f;
    bool looping = false;
    f64 position[3] = {0.0, 0.0, 0.0};
  } seen;
  GameServices services;
  services.user = &seen;
  services.actor_find = [](void*, std::string_view name, u32* out, std::string* why) {
    if (name == "bar") {
      *out = 3;
      return Result::Ok;
    }
    if (why != nullptr) *why = "no actor '" + std::string(name) + "' (the scene names bar)";
    return Result::NotFound;
  };
  services.actor_place = [](void* user, u32, const f64* p, f32) {
    for (u32 k = 0; k < 3; ++k)
      static_cast<Seen*>(user)->position[k] = p[k];
    return Result::Ok;
  };
  services.actor_play = [](void* user, u32, std::string_view clip, f32 fade, bool looping) {
    static_cast<Seen*>(user)->clip = std::string(clip);
    static_cast<Seen*>(user)->fade = fade;
    static_cast<Seen*>(user)->looping = looping;
    return Result::Ok;
  };
  services.actor_state = [](void*, u32 a, ActorState& out) {
    out.instance = a;
    out.pose_hash = 0x1234;
    out.clip[0] = 'b';
    return Result::Ok;
  };
  services.ground_height = [](void*, f64 x, f64 z, f64* out) {
    *out = x + z;
    return Result::Ok;
  };
  host.set_game_services(services);
  CHECK(actor_find(&context, "bar", actor) == Result::Ok);
  CHECK(actor == 3);
  CHECK(actor_find(&context, "tortoise", actor) == Result::NotFound);  // at a tick: an answer
  CHECK(actor_place(&context, 3, at, 0.5f) == Result::Ok);
  CHECK(seen.position[2] == 3.0);
  CHECK(actor_play(&context, 3, "bend", 0.25f, false) == Result::Ok);
  CHECK(seen.clip == "bend");  // the helper measured the C string
  CHECK(seen.fade == 0.25f);
  CHECK_FALSE(seen.looping);
  CHECK(actor_state(&context, 3, state) == Result::Ok);
  CHECK(state.instance == 3);
  CHECK(state.pose_hash == 0x1234);
  CHECK(state.size == sizeof(ActorState));
  CHECK(ground_height(&context, 2.0, 0.5, height) == Result::Ok);
  CHECK(height == 2.5);
  CHECK(actor_stop(&context, 3, 0.0f) == Result::Unsupported);  // not forwarded: no service
  CHECK(actor_spawn(&context, "bar", at, 0.0f, actor) == Result::Unsupported);
}

TEST_CASE("hotload: ABI 4's entries forward to the game services, and answer Unsupported without") {
  // No module: the table the host hands every module, called as one would call it.
  ModuleHost host;
  const HostApi* api = host.context()->api;
  CHECK(api->abi_version == k_abi_version);
  CHECK(api->size == sizeof(HostApi));
  CHECK(ENGINE_HOTLOAD_HAS(api, hud_element));
  CHECK(ENGINE_HOTLOAD_HAS(api, request_graphics));
  // A table as an ABI 3 host built it ends before the new entries: the helpers do not call them.
  HostApi v3 = *api;
  v3.size = static_cast<u32>(offsetof(HostApi, hud_element));
  CHECK_FALSE(ENGINE_HOTLOAD_HAS(&v3, hud_element));
  CHECK(ENGINE_HOTLOAD_HAS(&v3, sound_stop));
  ModuleContext old_host{&v3, nullptr};
  HudElement prompt;
  prompt.id = 1;
  prompt.text = "[E] Pick up";
  CHECK(hud_element(&old_host, prompt) == Result::Unsupported);
  CHECK(hud_remove(&old_host, 1) == Result::Unsupported);

  ModuleContext context{api, nullptr};
  CHECK(hud_element(&context, prompt) == Result::Unsupported);  // no game services
  GraphicsState state;
  CHECK(graphics_state(&context, state) == Result::Unsupported);
  // A struct smaller than ABI 4's is refused before anything is forwarded.
  HudElement small = prompt;
  small.size = 16;
  CHECK(api->hud_element(api->host, &small) == Result::InvalidArgument);
  CHECK(api->hud_element(api->host, nullptr) == Result::InvalidArgument);

  struct Seen {
    u32 id = 0;
    u64 length = 0;
    u32 removed = 0;
  } seen;
  GameServices services;
  services.user = &seen;
  services.hud_element = [](void* user, const HudElement& e) {
    static_cast<Seen*>(user)->id = e.id;
    static_cast<Seen*>(user)->length = e.text_length;
    return Result::Ok;
  };
  services.hud_remove = [](void* user, u32 id) {
    static_cast<Seen*>(user)->removed = id;
    return Result::Ok;
  };
  services.graphics_state = [](void*, GraphicsState& out) {
    out.width = 1920;
    out.raster[0] = 'm';
    return Result::Ok;
  };
  host.set_game_services(services);
  CHECK(hud_element(&context, prompt) == Result::Ok);
  CHECK(seen.id == 1);
  CHECK(seen.length == 11);  // the helper measured the C string
  CHECK(hud_remove(&context, 1) == Result::Ok);
  CHECK(seen.removed == 1);
  CHECK(graphics_state(&context, state) == Result::Ok);
  CHECK(state.width == 1920);
  CHECK(state.size == sizeof(GraphicsState));
  CHECK(request_graphics(&context, GraphicsRequest{}) == Result::Unsupported);  // not forwarded
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

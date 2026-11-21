// walk_poc_wind: the fixture of a module that reads and sets the session's wind (hotload ABI 13,
// its second line; roadmap W151; docs/subsystems/apps.md, "The wind a module reads and sets").
// engine-view's end-to-end case (apps/engine_view/tests/wind_view_tests.cpp) runs it in a
// session of its own and holds what it hands `end_run` against the frames' own records.
//
// On its ticks, counted from its first: at 1 it reads the wind as the session starts (the scene's,
// not yet live); at 10 it asks two sets the host must refuse (a negative mean, one past
// `k_wind_set_max_m_s`) and then sets 20 m/s blowing to 90 degrees, and reads it back at 11; at 20
// it holds a gust, which it reads one second in (260) and at 740, where it lets it go; and at 980,
// a second after, it reads the gust dying and ends the run with every answer. A fixture: it keeps
// no state (a reload starts it again).

#include <foundation/hotload/module_abi.h>

#include <cstdio>

using namespace engine;
using namespace engine::hotload;

namespace {

constexpr u32 k_set_tick = 10;
constexpr u32 k_gust_tick = 20;
constexpr u32 k_held_tick = k_gust_tick + 240;     // a second into the gust at 240 Hz
constexpr u32 k_release_tick = k_gust_tick + 720;  // three seconds held
constexpr u32 k_end_tick = k_release_tick + 240;   // a second after it was let go

u32 g_ticks = 0;
bool g_done = false;

// The answer, built in a fixed buffer: a module allocates nothing on a tick.
char g_json[4096];
u32 g_at = 0;
void put(const char* text) {
  while (*text != '\0' && g_at + 1 < sizeof(g_json))
    g_json[g_at++] = *text++;
  g_json[g_at] = '\0';
}

// One reading as JSON: `"name":{"result":r,"tick":t,"flags":f,"mean_m_s":...}`.
void read(ModuleContext* context, const char* name) {
  WindState wind;
  const Result r = wind_state(context, wind);
  char line[384];
  std::snprintf(line, sizeof line,
                "%s\"%s\":{\"result\":%u,\"tick\":%u,\"flags\":%u,\"mean_m_s\":%.17g,"
                "\"to_deg\":%.17g,\"gust\":%.17g,\"speed_m_s\":%.17g}",
                g_at > 1 ? "," : "", name, static_cast<u32>(r), g_ticks, wind.flags, wind.mean_m_s,
                wind.to_deg, wind.gust, wind.speed_m_s);
  put(line);
}

void said(const char* name, Result r) {
  char line[96];
  std::snprintf(line, sizeof line, ",\"%s\":%u", name, static_cast<u32>(r));
  put(line);
}

i32 on_load(ModuleContext*) {
  g_ticks = 0;
  g_done = false;
  g_at = 0;
  put("{");
  return 0;
}

void game_tick(ModuleContext* context, View, f32) {
  if (g_done) return;
  ++g_ticks;
  if (g_ticks == 1) read(context, "start");
  if (g_ticks == k_set_tick) {
    said("set_negative", wind_set(context, -1.0, 90.0));
    said("set_past_max", wind_set(context, k_wind_set_max_m_s + 1.0, 90.0));
    said("set", wind_set(context, 20.0, 90.0));
  }
  if (g_ticks == k_set_tick + 1) read(context, "after_set");
  if (g_ticks == k_gust_tick) said("gust_on", wind_gust(context, true));
  if (g_ticks == k_held_tick) read(context, "held");
  if (g_ticks == k_release_tick) {
    read(context, "released");
    said("gust_off", wind_gust(context, false));
  }
  if (g_ticks == k_end_tick) {
    read(context, "after");
    put("}");
    g_done = true;
    context->api->end_run(context->api->host, g_json, g_at);
  }
}

}  // namespace

ENGINE_HOTLOAD_DESCRIBE(api, out) {
  (void)api;
  static const Export exports[] = {
      export_function<TickFn>("game_tick", &game_tick),
  };
  out->abi_version = k_abi_version;
  out->desc_size = static_cast<u32>(sizeof(ModuleDesc));
  out->name = "walk_poc_wind";
  out->exports = exports;
  out->export_count = static_cast<u32>(sizeof(exports) / sizeof(exports[0]));
  out->on_load = &on_load;
  out->state_type = nullptr;
  out->type_info_size = 0;
  return 0;
}

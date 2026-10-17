// @@NAME@@: a game module for the engine's walking session (engine-view --game).
//
// A game module is a shared library the engine loads, ticks, watches and rebuilds under a running
// session. It is three things: one struct of state (@@NAME@@.schema, beside this file), `on_load`,
// which resolves the fields of the tick it reads and the actions it listens to, and `game_tick`,
// called every tick of the session with a view of that tick. Everything the module knows about
// the world comes through that view and the services below; everything it keeps is its state.
//
// The contract — the fields of `engine.game.GameTick`, the services (`hud_text`, `hud_meter`,
// `log`, `end_run`, `nearest_placement`, `game_action`) and the determinism rules — is documented
// in the engine's tree at the commit this repository pins (engine-sdk.json):
//   docs/game-authoring.md                       the author's guide
//   docs/subsystems/apps.md, "The game contract"  every field and service, and what each costs
// and the headers it is written against are the SDK's include/foundation/hotload/module_abi.h and
// module_state.h. This file is a starting point: a day counter, one HUD line, one meter, and a
// run that ends after a declared number of game days. Replace it.

#include <foundation/hotload/module_state.h>

#include <cmath>
#include <cstdio>
#include <schemas/@@NAME@@.h>  // generated from @@NAME@@.schema by the SDK's schemac at build time

using namespace engine;
using namespace engine::hotload;
using Run = @@NAME@@::Run;

namespace {

// The declared numbers. Game time runs at the session's sun rate (60 by default: a game day in 24
// real minutes), so three game days is a little over an hour of play.
constexpr f64 k_days_to_survive = 3.0;
constexpr f64 k_seconds_a_day = 86'400.0;
constexpr const char* k_tick = "engine.game.GameTick";

// The tick's fields this module reads, resolved once by name in on_load, and the bit of the one
// game action it listens to. A field that does not resolve refuses the load naming it, which is
// how an engine-side rename reaches a game: loudly, never as a wrong read.
struct Fields {
  FieldRef dt;               // this tick's length in game seconds (read this, not the f32 argument)
  FieldRef world_time_s;     // the world's clock, game seconds
  FieldRef actions_pressed;  // the game actions pressed on this tick, as a mask of bits
  u32 interact = 0;          // the bit `game_action` hands out for "interact" (E on the keyboard)
} g_fields;

// Called when the module is loaded, and again after every rebuild. Resolve, declare, return 0.
i32 on_load(ModuleContext* context) {
  resolve(context, k_tick, "dt", g_fields.dt);
  resolve(context, k_tick, "world_time_s", g_fields.world_time_s);
  resolve(context, k_tick, "actions_pressed", g_fields.actions_pressed);
  game_action(context, "interact", g_fields.interact);  // interact, use, game_1..game_4 exist
  return 0;  // a path or an action that did not resolve refuses the load by itself
}

// A numeric field of the tick, whatever its declared type, as f64.
f64 number(ModuleContext* context, View view, FieldRef field) {
  Value v;
  read(context, view, field, v);
  switch (v.kind) {
    case ValueKind::Float: return v.f;
    case ValueKind::Int: return static_cast<f64>(v.i);
    case ValueKind::UInt: return static_cast<f64>(v.u);
    case ValueKind::Bool: return v.b ? 1.0 : 0.0;
    default: return 0.0;
  }
}

// Called every tick (240 a second by default) with a view of the tick. Rules that keep a run
// replayable: read the clock from the tick, never from the machine; allocate nothing (the HUD text
// goes through a stack buffer the host copies); keep everything that matters in the state.
void game_tick(ModuleContext* context, View tick, f32 /*dt_f32*/) {
  Run& run = state<Run>(context);
  const f64 dt = number(context, tick, g_fields.dt);
  const f64 world_s = number(context, tick, g_fields.world_time_s);

  if (run.start_world_s < 0.0) run.start_world_s = world_s;  // the run's first tick
  run.seconds += dt;

  Value pressed;
  read(context, tick, g_fields.actions_pressed, pressed);
  if (action_set(pressed, g_fields.interact)) {
    ++run.interactions;
    log(context, LogLevel::Info, "@@NAME@@: interact");  // a line in the engine's log, not the HUD
  }

  // Whole game days since the run began, and how far through the current one we are.
  const f64 days = (world_s - run.start_world_s) / k_seconds_a_day;
  const u32 day = static_cast<u32>(std::floor(days));

  // The HUD is cleared by the host at the start of every tick: say each tick what it shows.
  // 16 lines of 95 characters and 8 meters at most; more is dropped.
  char line[96];
  std::snprintf(line, sizeof(line), "day %u of %.0f   %.0f s survived   E pressed %u times",
                day + 1, k_days_to_survive, run.seconds, run.interactions);
  hud_text(context, 0, line);
  hud_meter(context, "days", days < k_days_to_survive ? days : k_days_to_survive,
            k_days_to_survive);

  // Ending the run: one JSON object as the result. The host draws this tick's frame, ends the
  // session, and puts the object in the report's `game.result`; game_tick is not called again.
  if (days >= k_days_to_survive) {
    std::snprintf(line, sizeof(line), "{\"days\":%.0f,\"seconds\":%.3f,\"interactions\":%u}",
                  k_days_to_survive, run.seconds, run.interactions);
    end_run(context, line);
  }
}

}  // namespace

// The one exported symbol: how the host learns the module's name, its ABI, its exports, its load
// hook and its state type. `game_tick` is the export the game runner calls; the rest is
// boilerplate.
ENGINE_HOTLOAD_DESCRIBE(api, out) {
  (void)api;
  static const Export exports[] = {
      export_function<TickFn>("game_tick", &game_tick),
  };
  out->abi_version = k_abi_version;
  out->desc_size = static_cast<u32>(sizeof(ModuleDesc));
  out->name = "@@NAME@@";
  out->exports = exports;
  out->export_count = static_cast<u32>(sizeof(exports) / sizeof(exports[0]));
  out->on_load = &on_load;
  describe_state<Run>(out);
  return 0;
}

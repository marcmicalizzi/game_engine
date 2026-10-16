// walk_poc: the fixture game module that proves engine-view's game runner (game/walk_poc/README.md,
// docs/subsystems/apps.md "--game: running a game module"). A fixture and a demonstration of the
// contract — not Desert Survival, which lives in its own repository (ADR-0047, roadmap P18).
//
// The rules: thirst drains while the walker walks (faster at a sprint), refills while it stands
// within a few metres of water — the nearest ruin (the proof of concept's "water at the ruins"),
// or, on a scene with none near, a spring fixed 25 m ahead of where the run began, so the fixture
// works on any scene — and the run ends when thirst reaches zero, with the days, seconds and metres
// it lasted. `interact` (E) says a line. The HUD shows the meter, the score and where the water is.
//
// Everything the module knows comes through `GameTick`'s view and the HostApi; everything it keeps
// is its state (walk_poc.schema), so a rebuild carries it and `--game-state` saves it. `game_tick`
// allocates nothing: the HUD text is formatted into a stack buffer and copied by the host.

#include <foundation/hotload/module_state.h>

#include <cmath>
#include <cstdio>
#include <schemas/walk_poc.h>

using namespace engine;
using namespace engine::hotload;
using Survivor = walk_poc::Survivor;

#ifndef WALK_POC_TEST
#define WALK_POC_TEST 0  // 1: declares an action no map has; 2: fails at tick 30 (the tests')
#endif

namespace {

// The declared numbers. Per second of walking; a full meter is 100.
constexpr f64 k_drain_walk = 0.25;    // 400 s of walking from full
constexpr f64 k_drain_sprint = 0.75;  // three times as fast at a sprint
constexpr f64 k_refill = 10.0;        // per second at the water
constexpr f64 k_water_radius = 6.0;   // metres from a ruin or the spring that count as at it
constexpr f64 k_spring_ahead = 25.0;  // the spring's distance ahead of the first tick's feet
constexpr f64 k_look_radius = 400.0;  // how far the HUD looks for the nearest ruin
constexpr const char* k_tick = "engine.game.GameTick";

struct Fields {
  FieldRef tick, dt, feet, heading, walking, sprinting, moved, world_time, day_fraction;
  FieldRef pressed;
  u32 interact = 0;
  u32 use = 0;
  // The HUD's "nearest ruin" line, refreshed once a second: cosmetic, so not state.
  f64 ruin_distance = -1.0;
} g_fields;

i32 on_load(ModuleContext* context) {
  Fields& f = g_fields;
  resolve(context, k_tick, "tick", f.tick);
  resolve(context, k_tick, "dt", f.dt);
  resolve(context, k_tick, "feet", f.feet);
  resolve(context, k_tick, "heading", f.heading);
  resolve(context, k_tick, "walking", f.walking);
  resolve(context, k_tick, "sprinting", f.sprinting);
  resolve(context, k_tick, "moved_m", f.moved);
  resolve(context, k_tick, "world_time_s", f.world_time);
  resolve(context, k_tick, "day_fraction", f.day_fraction);
  resolve(context, k_tick, "actions_pressed", f.pressed);
  game_action(context, "interact", f.interact);
  game_action(context, "use", f.use);
#if WALK_POC_TEST == 1
  u32 dance = 0;
  game_action(context, "dance", dance);  // no such action: the load is refused, naming it
#endif
  return 0;  // a path or an action that did not resolve refuses the load by itself
}

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

void game_tick(ModuleContext* context, View tick, f32) {
  Survivor& s = state<Survivor>(context);
  const Fields& f = g_fields;
  // The tick's length from the view, in f64: TickFn's own f32 `dt` is 1/240 to seven digits, and a
  // run's seconds summed from it drift by a part in ten million a tick.
  const f64 dt = number(context, tick, f.dt);
  Value v;
  read(context, tick, f.tick, v);
  const u64 tick_index = v.u;
  read(context, tick, f.feet, v);
  const f64 feet[3] = {v.d[0], v.d[1], v.d[2]};
  const f64 world_s = number(context, tick, f.world_time);

#if WALK_POC_TEST == 2
  if (tick_index == 30) {
    fail(context, "walk_poc: the test build's tick 30 fails on purpose");
    return;
  }
#endif

  if (!s.started) {
    s.started = true;
    s.start_world_s = world_s;
    const f64 heading = number(context, tick, f.heading);
    // Ahead along the look's heading: 0 looks along -z, and the heading turns counter-clockwise
    // from above.
    s.spring.x = feet[0] - std::sin(heading) * k_spring_ahead;
    s.spring.y = feet[1];
    s.spring.z = feet[2] - std::cos(heading) * k_spring_ahead;
  }
  if (s.over) return;

  // Water: a ruin within reach, or the spring.
  f64 ruin[3] = {0.0, 0.0, 0.0};
  f64 ruin_distance = 0.0;
  bool at_water =
      nearest_placement(context, "ruins", k_water_radius, ruin, ruin_distance) == Result::Ok;
  const f64 dx = s.spring.x - feet[0];
  const f64 dz = s.spring.z - feet[2];
  const f64 spring_distance = std::sqrt(dx * dx + dz * dz);
  if (spring_distance <= k_water_radius) at_water = true;
  if (tick_index % 240 == 1) {
    f64 far[3];
    f64 far_distance = 0.0;
    g_fields.ruin_distance =
        nearest_placement(context, "ruins", k_look_radius, far, far_distance) == Result::Ok
            ? far_distance
            : -1.0;
  }

  // Thirst.
  const bool walking = number(context, tick, f.walking) != 0.0;
  const f64 moved = number(context, tick, f.moved);
  if (walking && moved > 0.0) {
    const bool sprinting = number(context, tick, f.sprinting) != 0.0;
    s.thirst -= (sprinting ? k_drain_sprint : k_drain_walk) * dt;
    s.walked_m += moved;
  }
  if (at_water) {
    s.thirst += k_refill * dt;
    ++s.drinking_ticks;
  }
  if (s.thirst > 100.0) s.thirst = 100.0;
  s.seconds += dt;
  const f64 start_day = std::floor(s.start_world_s / 86'400.0);
  const f64 day = std::floor(world_s / 86'400.0);
  s.days = day > start_day ? static_cast<u32>(day - start_day) : 0u;

  read(context, tick, f.pressed, v);
  if (action_set(v, f.interact)) {
    ++s.interactions;
    log(context, LogLevel::Info, at_water ? "walk_poc: you drink" : "walk_poc: nothing here");
  }

  // The HUD, said again every tick.
  char line[96];
  hud_meter(context, "thirst", s.thirst > 0.0 ? s.thirst : 0.0, 100.0);
  std::snprintf(line, sizeof(line), "day %u   %.0f s survived   %.0f m walked", s.days + 1,
                s.seconds, s.walked_m);
  hud_text(context, 0, line);
  if (at_water) {
    hud_text(context, 1, "at the water: drinking");
  } else if (g_fields.ruin_distance >= 0.0) {
    std::snprintf(line, sizeof(line), "water: a ruin %.0f m away, the spring %.0f m",
                  g_fields.ruin_distance, spring_distance);
    hud_text(context, 1, line);
  } else {
    std::snprintf(line, sizeof(line), "water: the spring, %.0f m away", spring_distance);
    hud_text(context, 1, line);
  }
  if (s.interactions > 0) {
    std::snprintf(line, sizeof(line), "E pressed %u times", s.interactions);
    hud_text(context, 2, line);
  }

  if (s.thirst <= 0.0) {
    s.thirst = 0.0;
    s.over = true;
    std::snprintf(line, sizeof(line), "{\"days\":%u,\"seconds\":%.3f,\"distance\":%.3f}", s.days,
                  s.seconds, s.walked_m);
    end_run(context, line);
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
  out->name = "walk_poc";
  out->exports = exports;
  out->export_count = static_cast<u32>(sizeof(exports) / sizeof(exports[0]));
  out->on_load = &on_load;
  describe_state<Survivor>(out);
  return 0;
}

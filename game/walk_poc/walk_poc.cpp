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
// **Acting on the world** (ABI 3, roadmap P43), when the scene declares the props `can` and
// `marker`: a water can is placed at the spring on the first tick, picked up (removed) with
// `interact` within two metres, which fills the meter; `use` (Q) places a marker where the walker
// stands; and below half thirst the walker slows, to half speed at zero (`walker_set`). On a scene
// that declares no props the fixture places nothing and runs as it did.
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
// 1: declares an action no map has; 2: fails at tick 30; 3: places no props and touches no walker;
// 4: named walk_poc_teleport, teleports the walker 10 m ahead at tick 50; 5: named walk_poc_actor,
// walks the scene's actor `bar` round a circle on the ground (the tests').
#define WALK_POC_TEST 0
#endif

namespace {

// The declared numbers. Per second of walking; a full meter is 100.
constexpr f64 k_drain_walk = 0.25;    // 400 s of walking from full
constexpr f64 k_drain_sprint = 0.75;  // three times as fast at a sprint
constexpr f64 k_refill = 10.0;        // per second at the water
constexpr f64 k_water_radius = 6.0;   // metres from a ruin or the spring that count as at it
constexpr f64 k_spring_ahead = 25.0;  // the spring's distance ahead of the first tick's feet
constexpr f64 k_look_radius = 400.0;  // how far the HUD looks for the nearest ruin
constexpr f64 k_reach = 2.0;          // metres within which the can can be picked up
constexpr f64 k_slow_below = 50.0;    // thirst under which the walker slows, to half speed at 0
constexpr const char* k_tick = "engine.game.GameTick";

struct Fields {
  FieldRef tick, dt, feet, heading, pitch, walking, sprinting, moved, world_time, day_fraction;
  FieldRef pressed;
  u32 interact = 0;
  u32 use = 0;
  // The props, when the scene declares them (`prop_mesh` answered Ok for both).
  bool props = false;
  u32 can_mesh = 0;
  u32 marker_mesh = 0;
  // The HUD's "nearest ruin" line, refreshed once a second: cosmetic, so not state. The last walk
  // scale set, so the walker is told only when it changes: a reload tells it again once.
  f64 ruin_distance = -1.0;
  f64 scale_set = -1.0;
  // The actor build's `bar` (ABI 5), when the scene names one.
  bool has_bar = false;
  u32 bar = 0;
  // The game's own HUD art (ABI 6), when its content directory has it: a font for the title line,
  // and the prompt's panel image, not asked for again once the host said it is not there.
  bool has_font = false;
  u32 font = 0;
  bool no_panel = false;
} g_fields;

#if WALK_POC_TEST == 5
// **The actor build** (apps.md "Actors: the characters a game moves"): the scene's actor `bar`
// walks a 3 m circle round where it stood, a turn every two seconds, standing on the ground
// (`ground_height`) and facing along its way; it plays `bend` from its first tick, at twice the
// rate from tick 120, and fades to its rest pose over half a second from tick 240. What it saw
// last (`actor_state`'s pose hash) is in the state.
constexpr f64 k_circle_radius = 3.0;
constexpr u64 k_circle_ticks = 480;
constexpr f64 k_two_pi = 6.283185307179586;

void walk_actor(ModuleContext* context, Survivor& s, u64 tick) {
  const u32 bar = g_fields.bar;
  ActorState seen;
  if (!s.actor_started) {
    if (actor_state(context, bar, seen) != Result::Ok) return;
    s.actor_started = true;
    s.actor_centre.x = seen.position[0];
    s.actor_centre.y = seen.position[1];
    s.actor_centre.z = seen.position[2];
    actor_play(context, bar, "bend", 0.0f, true);
  }
  const f64 angle =
      k_two_pi * static_cast<f64>(tick % k_circle_ticks) / static_cast<f64>(k_circle_ticks);
  f64 at[3] = {s.actor_centre.x + k_circle_radius * std::cos(angle), s.actor_centre.y,
               s.actor_centre.z + k_circle_radius * std::sin(angle)};
  f64 ground = 0.0;
  if (ground_height(context, at[0], at[2], ground) == Result::Ok) at[1] = ground;
  // Along the circle: the way (-sin a, cos a) in x and z, and a yaw turns +x to (cos y, -sin y).
  const f64 yaw = std::atan2(-std::cos(angle), -std::sin(angle));
  actor_place(context, bar, at, static_cast<f32>(yaw));
  if (tick == 120) actor_speed(context, bar, 2.0f);
  if (tick == 240) actor_stop(context, bar, 0.5f);
  if (actor_state(context, bar, seen) == Result::Ok) s.actor_pose = seen.pose_hash;
  ++s.actor_ticks;
}
#endif

constexpr u32 k_prompt_id = 1;  // the pick-up prompt's HUD element
constexpr u32 k_panel_id = 2;   // the image behind it (ABI 6)
constexpr u32 k_title_id = 3;   // the title line in the game's font (ABI 6)

i32 on_load(ModuleContext* context) {
  Fields& f = g_fields;
  resolve(context, k_tick, "tick", f.tick);
  resolve(context, k_tick, "dt", f.dt);
  resolve(context, k_tick, "feet", f.feet);
  resolve(context, k_tick, "heading", f.heading);
  resolve(context, k_tick, "pitch", f.pitch);
  resolve(context, k_tick, "walking", f.walking);
  resolve(context, k_tick, "sprinting", f.sprinting);
  resolve(context, k_tick, "moved_m", f.moved);
  resolve(context, k_tick, "sky_time_s",
          f.world_time);  // the sky's hour: the days are the sky's (R88)
  resolve(context, k_tick, "day_fraction", f.day_fraction);
  resolve(context, k_tick, "actions_pressed", f.pressed);
  game_action(context, "interact", f.interact);
  game_action(context, "use", f.use);
#if WALK_POC_TEST == 1
  u32 dance = 0;
  game_action(context, "dance", dance);  // no such action: the load is refused, naming it
#endif
#if WALK_POC_TEST != 3
  // The props: `Unsupported` is a scene with none (the fixture then places nothing); `NotFound` —
  // a scene with props and not these — refuses the load by itself, naming the prop.
  f.props = prop_mesh(context, "can", f.can_mesh) == Result::Ok &&
            prop_mesh(context, "marker", f.marker_mesh) == Result::Ok;
#endif
  f.scale_set = -1.0;
  // The game's font (ABI 6), when its content directory ships one: a plain answer either way.
  f.has_font = hud_font(context, "hud/font.ttf", 56.0f, f.font) == Result::Ok;
  f.no_panel = false;
#if WALK_POC_TEST == 5
  // `Unsupported`: a session with no actors, and the build walks nothing; `NotFound` — a scene
  // with actors and no `bar` — refuses the load by itself, naming it.
  f.has_bar = actor_find(context, "bar", f.bar) == Result::Ok;
#endif
  return 0;  // a path, an action or a prop that did not resolve refuses the load by itself
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
  const f64 heading = number(context, tick, f.heading);

#if WALK_POC_TEST == 2
  if (tick_index == 30) {
    fail(context, "walk_poc: the test build's tick 30 fails on purpose");
    return;
  }
#endif
#if WALK_POC_TEST == 4
  if (tick_index == 50) {
    // Ten metres ahead along the look, standing: the camera with it, looking the same way.
    const f64 ahead[3] = {feet[0] - std::sin(heading) * 10.0, feet[1],
                          feet[2] - std::cos(heading) * 10.0};
    walker_teleport(context, ahead, static_cast<f32>(heading),
                    static_cast<f32>(number(context, tick, f.pitch)));
  }
#endif

  if (tick_index == 1) {
    // **The graphics** (ABI 4): what the session draws with, said once in the log, and the two
    // kinds of answer a request gets — one the host records for its next restart (the sun's
    // cascaded maps, which every device draws), and one it cannot make (a raster path it has not).
    GraphicsState graphics;
    if (graphics_state(context, graphics) == Result::Ok) {
      char said[96];
      std::snprintf(said, sizeof(said), "walk_poc: drawn with %s, shadows %s", graphics.raster,
                    graphics.shadows);
      log(context, LogLevel::Info, said);
    }
    GraphicsRequest maps;
    std::snprintf(maps.shadows, sizeof(maps.shadows), "csm");
    request_graphics(context, maps);
    GraphicsRequest unknown;
    std::snprintf(unknown.raster, sizeof(unknown.raster), "warp");
    request_graphics(context, unknown);  // Unsupported: no such path
  }
  if (!s.started) {
    s.started = true;
    s.start_world_s = world_s;
    // Ahead along the look's heading: 0 looks along -z, and the heading turns counter-clockwise
    // from above.
    s.spring.x = feet[0] - std::sin(heading) * k_spring_ahead;
    s.spring.y = feet[1];
    s.spring.z = feet[2] - std::cos(heading) * k_spring_ahead;
  }
  // The can at the spring, once, at the height of the walker's feet on its first tick (a module
  // has no ground query; a scene whose spring lies up a dune buries it, which is the fixture's
  // business and not the contract's).
  if (f.props && !s.can_placed) {
    s.can_placed = true;
    const f64 at[3] = {s.spring.x, s.spring.y, s.spring.z};
    if (prop_place(context, f.can_mesh, at, 0.0f, 1.0f, s.can) != Result::Ok) s.can = 0;
  }
#if WALK_POC_TEST == 5
  if (f.has_bar) walk_actor(context, s, tick_index);
#endif
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
  // The can within reach: `interact` picks it up and the meter is full.
  u32 near_can = 0;
  f64 can_at[3] = {0.0, 0.0, 0.0};
  f64 can_distance = 0.0;
  const bool can_in_reach =
      s.can != 0 &&
      nearest_prop(context, f.can_mesh, k_reach, near_can, can_at, can_distance) == Result::Ok &&
      near_can == s.can;

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
    if (can_in_reach && prop_remove(context, s.can) == Result::Ok) {
      s.can = 0;
      s.can_taken = true;
      s.thirst = 100.0;
      log(context, LogLevel::Info, "walk_poc: you pick up the can and drink it dry");
    } else {
      log(context, LogLevel::Info, at_water ? "walk_poc: you drink" : "walk_poc: nothing here");
    }
  }
  if (action_set(v, f.use) && f.props) {
    u32 marker = 0;
    if (prop_place(context, f.marker_mesh, feet, static_cast<f32>(heading), 1.0f, marker) ==
        Result::Ok) {
      ++s.markers;
    }
  }

#if WALK_POC_TEST != 3
  // Below half thirst the walker slows, to half speed at zero: told only when the scale moves, so
  // a fresh walker's speed is the recording's own to the bit until thirst falls that far.
  const f64 scale =
      s.thirst >= k_slow_below ? 1.0 : 0.5 + 0.5 * (s.thirst > 0.0 ? s.thirst : 0.0) / k_slow_below;
  if (scale != g_fields.scale_set) {
    g_fields.scale_set = scale;
    walker_set(context, static_cast<f32>(scale), static_cast<f32>(scale));
  }
#endif

  // The HUD, said again every tick.
  char line[96];
  hud_meter(context, "thirst", s.thirst > 0.0 ? s.thirst : 0.0, 100.0);
  std::snprintf(line, sizeof(line), "day %u   %.0f s survived   %.0f m walked", s.days + 1,
                s.seconds, s.walked_m);
  hud_text(context, 0, line);
  if (can_in_reach) {
    hud_text(context, 1, "a water can: E to pick it up");
  } else if (at_water) {
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
  if (s.can_taken || s.markers > 0) {
    std::snprintf(line, sizeof(line), "%s%u marker%s placed", s.can_taken ? "can taken   " : "",
                  s.markers, s.markers == 1 ? "" : "s");
    hud_text(context, 3, line);
  }

  // **The pick-up prompt** (ABI 4, a structured element): outlined text with no panel, below the
  // picture's centre, shown while the can is in reach. The host keeps an element until it is
  // removed; said every tick under its id it is replaced in place, and `visible` hides it. Said
  // only once the can stands, so a scene with no props has no element at all.
  // **The game's own art** (ABI 6): with `hud/panel.png` in its content directory (beside the
  // scene, or beside the module), a nine-sliced panel behind the prompt — placed before the prompt
  // the first time, so it is drawn under it — and with `hud/font.ttf`, a title line in that font
  // with a drop shadow and letter spacing at the top of the picture. Without the files the host
  // answers NotFound and the HUD is what it was.
  if (s.can_placed && !g_fields.no_panel) {
    HudImage panel;
    panel.id = k_panel_id;
    panel.anchor = HudAnchor::Centre;
    panel.align = HudAlign::Centre;
    panel.offset_px[1] = 80.0f;
    panel.size_px[0] = 300.0f;
    panel.size_px[1] = 64.0f;
    for (f32& border : panel.slice_px)
      border = 16.0f;
    panel.visible = can_in_reach ? 1u : 0u;
    panel.path = "hud/panel.png";
    if (hud_image(context, panel) != Result::Ok) g_fields.no_panel = true;
  }
  if (g_fields.has_font) {
    HudElement title;
    title.id = k_title_id;
    title.anchor = HudAnchor::Top;
    title.align = HudAlign::Centre;
    title.size_px = 56.0f;
    title.font = g_fields.font;
    title.letter_spacing_px = 4.0f;
    title.shadow_offset_px[0] = 3.0f;
    title.shadow_offset_px[1] = 3.0f;
    title.text = "FIND THE WATER";
    hud_element(context, title);
  }
  if (s.can_placed) {
    HudElement prompt;
    prompt.id = k_prompt_id;
    prompt.anchor = HudAnchor::Centre;
    prompt.align = HudAlign::Centre;
    prompt.offset_px[1] = 80.0f;  // below the crosshair's place, in 1080p pixels
    prompt.size_px = 28.0f;
    prompt.outline_px = 2.0f;
    prompt.visible = can_in_reach ? 1u : 0u;
    prompt.text = "[E] Pick up";
    hud_element(context, prompt);
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
#if WALK_POC_TEST == 4
  out->name = "walk_poc_teleport";
#elif WALK_POC_TEST == 5
  out->name = "walk_poc_actor";
#else
  out->name = "walk_poc";
#endif
  out->exports = exports;
  out->export_count = static_cast<u32>(sizeof(exports) / sizeof(exports[0]));
  out->on_load = &on_load;
  describe_state<Survivor>(out);
  return 0;
}

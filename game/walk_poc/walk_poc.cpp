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
// walks the scene's actor `bar` round a circle on the ground (the tests'); 6: the loading build
// (ABI 7); 7: named walk_poc_aligned, places `bar` on slopes with `actor_place_aligned` and
// samples the ground's normal (ABI 8); 8: named walk_poc_abi7, the actor build declared as a
// module built against ABI 7 is, reading `actor_state` into ABI 5's 136 bytes; 9: named
// walk_poc_level, build 7 with its first placement made through `actor_place`.
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

#if WALK_POC_TEST == 5 || WALK_POC_TEST == 7 || WALK_POC_TEST == 8 || WALK_POC_TEST == 9
#define WALK_POC_ACTORS 1
#endif

#if WALK_POC_TEST == 5 || WALK_POC_TEST == 8
// **The actor build** (apps.md "Actors: the characters a game moves"): the scene's actor `bar`
// walks a 3 m circle round where it stood, a turn every two seconds, standing on the ground
// (`ground_height`) and facing along its way; it plays `bend` from its first tick, at twice the
// rate from tick 120, and fades to its rest pose over half a second from tick 240. What it saw
// last (`actor_state`'s pose hash) is in the state.
constexpr f64 k_circle_radius = 3.0;
constexpr u64 k_circle_ticks = 480;
constexpr f64 k_two_pi = 6.283185307179586;

struct Seen {
  f64 position[3] = {0.0, 0.0, 0.0};
  u64 pose_hash = 0;
};

#if WALK_POC_TEST == 8
// **The ABI 7 build** (`walk_poc_abi7`; apps.md "Actors"): the same circle, declared as a module
// built against ABI 7 is — the actor's state read into the 136 bytes ABI 5 to 7 owned
// (`k_actor_state_v5_size`), the calls placed with `actor_place`, nothing of ABI 8 touched. A
// guard of 24 bytes follows the struct, and any that the host overwrote is counted in the state:
// a host of ABI 8 copies an `ActorState` out only as far as the module's `size`.
struct ActorStateV5 {
  u32 size = k_actor_state_v5_size;
  u32 instance = 0;
  f64 position[3] = {0.0, 0.0, 0.0};
  f32 yaw = 0.0f, playhead_s = 0.0f, speed = 1.0f, weight = 1.0f;
  u32 looping = 0, tier = 0, playing = 0, fading = 0;
  u64 pose_hash = 0;
  char clip[64] = {};
};
static_assert(sizeof(ActorStateV5) == k_actor_state_v5_size, "ABI 5's ActorState is 136 bytes");
struct GuardedState {
  ActorStateV5 state;
  unsigned char guard[24];
};

bool read_actor(ModuleContext* context, Survivor& s, u32 bar, Seen& out) {
  GuardedState g;
  std::memset(g.guard, 0xAB, sizeof(g.guard));
  const Result r =
      context->api->actor_state(context->api->host, bar, reinterpret_cast<ActorState*>(&g.state));
  for (const unsigned char byte : g.guard) {
    if (byte != 0xAB) ++s.abi7_overrun;
  }
  for (u32 k = 0; k < 3; ++k)
    out.position[k] = g.state.position[k];
  out.pose_hash = g.state.pose_hash;
  return r == Result::Ok;
}
#else
bool read_actor(ModuleContext* context, Survivor&, u32 bar, Seen& out) {
  ActorState seen;
  if (actor_state(context, bar, seen) != Result::Ok) return false;
  for (u32 k = 0; k < 3; ++k)
    out.position[k] = seen.position[k];
  out.pose_hash = seen.pose_hash;
  return true;
}
#endif

void walk_actor(ModuleContext* context, Survivor& s, u64 tick) {
  const u32 bar = g_fields.bar;
  Seen seen;
  if (!s.actor_started) {
    if (!read_actor(context, s, bar, seen)) return;
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
  if (read_actor(context, s, bar, seen)) s.actor_pose = seen.pose_hash;
  ++s.actor_ticks;
}
#endif

#if WALK_POC_TEST == 7 || WALK_POC_TEST == 9
// **The aligned build** (ABI 8; apps.md "Actors"): the scene's actor `bar` stood at one place,
// eight phases of a second, each placing it a different way on the first tick of the phase and
// holding it there, so the report's path (a sample at the end of each phase) shows the pose:
//   0  up = (0,1,0)              level, `actor_place`'s own
//   1  a 20 degree slope toward +x, the default limit (30)
//   2  a 60 degree wall, the default limit: held to 30
//   3  the same 60 with a limit of 70 degrees: 60
//   4  four refused calls (an up of length 2, a NaN up, a negative limit, one past a right angle):
//      the actor stays as phase 3 left it
//   5  `ground_sample` at the place and an aligned placement with the normal it answered
//   6  `actor_place`: level again; a `ground_height` every tick
//   7  two refused footprints (negative, 100 m); a `ground_sample` every tick
// The answer of every call is in the state, and `actor_state`'s tilt after the first four.
constexpr u64 k_phase_ticks = 60;
constexpr f32 k_deg = 0.017453292519943295f;

void record(Survivor& s, u32 call, Result r) {
  s.align_codes |= static_cast<u64>(static_cast<u32>(r) & 0xFu) << (4u * call);
}

// The level build (`walk_poc_level`, WALK_POC_TEST 9) makes every placement through `actor_place`
// and ignores the slope: the baseline a capture of the aligned build is held against — the same
// pictures at `up` = (0,1,0), different ones where the slope tipped the actor.
Result stand(ModuleContext* context, u32 actor, const f64 (&position)[3], f32 yaw,
             const f32 (&up)[3], f32 max_tilt_rad = 0.0f) {
#if WALK_POC_TEST == 9
  (void)up;
  (void)max_tilt_rad;
  return actor_place(context, actor, position, yaw);
#else
  return actor_place_aligned(context, actor, position, yaw, up, max_tilt_rad);
#endif
}

void align_actor(ModuleContext* context, Survivor& s, u64 tick) {
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
  ++s.actor_ticks;
  const u64 phase = (tick - 1) / k_phase_ticks;
  const bool first = (tick - 1) % k_phase_ticks == 0;
  f64 at[3] = {s.actor_centre.x, s.actor_centre.y, s.actor_centre.z};
  f64 ground = 0.0;
  // The first tick of a phase asks the height it stands the actor at; phase 6 asks it every tick
  // and phase 7 asks `ground_sample` every tick (and the height once, on its first), which is what
  // the end-to-end test counts the walker's ground queries by: one a `ground_height`, five a
  // sample.
  if (first || phase == 6) {
    if (ground_height(context, at[0], at[2], ground) == Result::Ok) at[1] = ground;
  }
  if (phase == 7) {
    GroundSample every_tick;
    (void)ground_sample(context, at[0], at[2], every_tick);
  }
  if (!first) return;
  const f32 yaw = 0.7f;
  const f32 level[3] = {0.0f, 1.0f, 0.0f};
  const f32 slope20[3] = {-std::sin(20.0f * k_deg), std::cos(20.0f * k_deg), 0.0f};
  const f32 wall60[3] = {-std::sin(60.0f * k_deg), std::cos(60.0f * k_deg), 0.0f};
  const auto tilt_after = [&](u32 slot) {
    if (actor_state(context, bar, seen) != Result::Ok) return;
    s.align_tilt[slot] = static_cast<f64>(seen.tilt_rad);
    if (slot == 2) s.align_clamped = seen.tilt_clamped;
  };
  switch (phase) {
    case 0:
      record(s, 0, stand(context, bar, at, yaw, level));
      tilt_after(0);
      break;
    case 1:
      record(s, 1, stand(context, bar, at, yaw, slope20));
      tilt_after(1);
      break;
    case 2:
      record(s, 2, stand(context, bar, at, yaw, wall60));
      tilt_after(2);
      break;
    case 3:
      record(s, 3, stand(context, bar, at, yaw, wall60, 70.0f * k_deg));
      tilt_after(3);
      break;
    case 4: {
      const f32 too_long[3] = {2.0f, 0.0f, 0.0f};
      const f32 not_a_number[3] = {std::nanf(""), 1.0f, 0.0f};
      record(s, 4, stand(context, bar, at, yaw, too_long));
      record(s, 5, stand(context, bar, at, yaw, not_a_number));
      record(s, 6, stand(context, bar, at, yaw, level, -1.0f));
      record(s, 7, stand(context, bar, at, yaw, level, 2.0f));
      break;
    }
    case 5: {
      GroundSample sample;
      const Result r = ground_sample(context, at[0], at[2], sample);
      record(s, 8, r);
      if (r == Result::Ok) {
        s.align_sample_height_delta = sample.height - ground;
        s.align_sample_normal.x = static_cast<f64>(sample.normal[0]);
        s.align_sample_normal.y = static_cast<f64>(sample.normal[1]);
        s.align_sample_normal.z = static_cast<f64>(sample.normal[2]);
        const f64 raised[3] = {at[0], sample.height, at[2]};
        record(s, 9, stand(context, bar, raised, yaw, sample.normal));
      }
      break;
    }
    case 6: record(s, 10, actor_place(context, bar, at, yaw)); break;
    case 7: {
      GroundSample sample;
      record(s, 11, ground_sample(context, at[0], at[2], sample, -1.0f));
      record(s, 12, ground_sample(context, at[0], at[2], sample, 100.0f));
      break;
    }
    default: break;
  }
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
#ifdef WALK_POC_ACTORS
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
#if WALK_POC_TEST == 5 || WALK_POC_TEST == 8
  if (f.has_bar) walk_actor(context, s, tick_index);
#elif WALK_POC_TEST == 7 || WALK_POC_TEST == 9
  if (f.has_bar) align_actor(context, s, tick_index);
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

#if WALK_POC_TEST == 6
// **The loading build** (ABI 7; apps.md, "The loading presentation"): its own loading line, said
// through the HUD every loading frame, which makes the host's default step aside; and the hand-over
// held until the ground under the feet has collision (the forest would ask for nearby_stable).
void game_loading(ModuleContext* context, const GameLoadProgress* progress) {
  Survivor& s = state<Survivor>(context);
  const f64 fraction = static_cast<f64>(progress->fraction);
  if (s.loading_calls > 0 && fraction < s.loading_fraction) ++s.loading_falls;
  s.loading_fraction = fraction;
  ++s.loading_calls;
  char line[160];
  std::snprintf(line, sizeof(line), "walk_poc is loading: %s %.0f%%", progress->stage,
                fraction * 100.0);
  hud_text(context, 0, line);
}

u32 game_ready_milestone(ModuleContext*) {
  return static_cast<u32>(GameMilestone::GroundCollisionReady);
}
#endif

}  // namespace

ENGINE_HOTLOAD_DESCRIBE(api, out) {
  (void)api;
  static const Export exports[] = {
      export_function<TickFn>("game_tick", &game_tick),
#if WALK_POC_TEST == 6
      export_function<LoadingFn>("game_loading", &game_loading),
      export_function<MilestoneFn>("game_ready_milestone", &game_ready_milestone),
#endif
  };
#if WALK_POC_TEST == 8
  out->abi_version = 7;  // what a module built before ABI 8 declares (see `read_actor`)
#else
  out->abi_version = k_abi_version;
#endif
  out->desc_size = static_cast<u32>(sizeof(ModuleDesc));
#if WALK_POC_TEST == 4
  out->name = "walk_poc_teleport";
#elif WALK_POC_TEST == 5
  out->name = "walk_poc_actor";
#elif WALK_POC_TEST == 7
  out->name = "walk_poc_aligned";
#elif WALK_POC_TEST == 9
  out->name = "walk_poc_level";
#elif WALK_POC_TEST == 8
  out->name = "walk_poc_abi7";
#elif WALK_POC_TEST == 6
  out->name = "walk_poc_loading";
#else
  out->name = "walk_poc";
#endif
  out->exports = exports;
  out->export_count = static_cast<u32>(sizeof(exports) / sizeof(exports[0]));
  out->on_load = &on_load;
  describe_state<Survivor>(out);
  return 0;
}

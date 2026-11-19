// A run recorded at one cadence of frames to ticks replays at another (domain/sim/door.h; sim.md,
// "The door"; roadmap S18): a player steers a walker with a stick the host reads once a frame — a
// frame that runs several ticks, frames that run none, a stall the fixed step drops time over —
// and the stick's changes go through the door, which stamps each with the next tick to run. The
// record replays headless at a fixed step and again under frames that fall another way, to the
// same hash log. A walker that reads the stick as the host sampled it at frame time instead of at
// its tick is the planted cadence dependence: the bisector finds it at its tick, its phase, its
// table and its field.
#include <domain/ecs/identity.h>
#include <domain/ecs/scheduled_tick.h>
#include <domain/ecs/sim_world.h>
#include <domain/ecs/tick_hash.h>
#include <domain/sim/door.h>
#include <domain/sim/hash_log.h>
#include <domain/sim/scheduler.h>
#include <foundation/input/input.h>

#include <doctest/doctest.h>

#include <memory>
#include <schemas/ecs_demo.h>
#include <schemas/ecs_demo_ecs.h>
#include <string>

using namespace engine;
using namespace engine::ecs;

namespace {

constexpr u64 k_ticks = 600;
constexpr i64 k_step_ns = 16'666'667;  // 60 Hz, the scheduler's default

// The stick, a function of the host's real time: centred for the first second, then a staircase of
// quarter steps every 37 ms, which changes inside a tick, between frames, and several times over a
// stall.
f32 stick(i64 real_ns) {
  if (real_ns < 1'000'000'000) return 0.0f;
  const i64 k = (real_ns / 37'000'000) % 7;
  return 0.25f * static_cast<f32>(k - 3);
}

struct World {
  SimWorld sim;
  sim::SimScheduler scheduler;
  sim::Door door;
  input::ActionMap map;
  input::ActionId steer = input::k_invalid_action;
  input::InputState state;
  flecs::entity_t walker = 0;
  // The bug, when set: the walker reads what the host sampled at the start of its frame.
  bool frame_sampled = false;
  f32 polled = 0.0f;
  f32 last_submitted = 0.0f;
  u64 first_moved = 0;  // the first tick the walker moved
  std::unique_ptr<ScheduledTick> tick;
  std::unique_ptr<TickHash> hash;

  World(bool sampled, sim::HashLog& log) : frame_sampled(sampled) {
    demo::register_ecs_demo_components(sim.world());
    flecs::entity e = create_entity(sim.world(), Id128{11, 1});
    e.set<demo::Standing>({Vec3{0.0f, 0.0f, 0.0f}, 0, 1.0f, 0u, 0u});
    walker = e.id();
    steer = map.add_action("steer", input::ActionKind::Axis);
    map.bind(steer, input::Binding{input::Source::GamepadAxis, 0u});
    state.set_map(map);
    // The door first, so its Input hook feeds the state before anything of the phase reads it.
    door.install(scheduler);
    door.set_input_state(&state);
    sim::SystemDesc walk;
    walk.name = "test.walk";
    walk.phase = sim::TickPhase::Input;
    walk.context = this;
    walk.begin_tick = &World::walk;
    scheduler.add_system(walk);
    tick = std::make_unique<ScheduledTick>(sim, scheduler);
    hash = std::make_unique<TickHash>(sim, scheduler, log);
    hash->add_source(TickHashSource{"door", this, [](void* c, sim::TickPhase) {
                                      return static_cast<World*>(c)->door.delivered_hash();
                                    }});
  }

  static void walk(sim::SystemContext& context) {
    auto* self = static_cast<World*>(context.context);
    const f32 axis = self->frame_sampled ? self->polled : self->state.axis(self->steer);
    if (axis != 0.0f && self->first_moved == 0) self->first_moved = context.tick.value;
    flecs::entity e(self->sim.world().c_ptr(), self->walker);
    e.try_get_mut<demo::Standing>()->position.x += axis;
  }

  // A frame at real time `now`: the host reads the stick, hands a change to the door, keeps what
  // it read (which only the buggy walker looks at).
  void frame_input(i64 now) {
    const f32 v = stick(now);
    polled = v;
    if (v != last_submitted) {
      input::RawEvent e;
      e.source = input::Source::GamepadAxis;
      e.code = 0u;
      e.value = v;
      if (door.submit_input(e) || door.replaying()) last_submitted = v;
    }
  }
};

u64 lcg(u64& s) {
  s = s * 6364136223846793005ull + 1442695040888963407ull;
  return s >> 33;
}

// The recorded run: frames of 5 ms (three or four to a tick), 16.7 ms, 41 ms (two or three ticks)
// and, once in a while, a 400 ms stall of which the fixed step runs eight ticks and drops the rest.
u32 record(World& w, u64 seed) {
  i64 now = 0;
  u64 s = seed;
  u32 stalls = 0;
  while (w.scheduler.tick().value < k_ticks) {
    const u64 r = lcg(s) % 100;
    const i64 dt = r < 30 ? 5'000'000 : r < 80 ? k_step_ns : r < 98 ? 41'000'000 : 400'000'000;
    stalls += dt == 400'000'000 ? 1u : 0u;
    now += dt;
    w.frame_input(now);
    // The last frames step one tick at a time, so the run ends at exactly k_ticks.
    if (k_ticks - w.scheduler.tick().value < 8) {
      w.tick->step();
    } else {
      w.tick->advance(dt);
    }
  }
  return stalls;
}

// A replay under other frames: 0 to 3 ticks each, the host still reading its stick (the door
// refuses what it hands over; the buggy walker reads it).
void replay_frames(World& w, u64 seed) {
  i64 now = 0;
  u64 s = seed;
  while (w.scheduler.tick().value < k_ticks) {
    const u64 n = lcg(s) % 4;
    w.frame_input(now);
    for (u64 i = 0; i < n && w.scheduler.tick().value < k_ticks; ++i) {
      w.tick->step();
      now += k_step_ns;
    }
    now += 3'000'000;
  }
}

void replay_headless(World& w) {
  while (w.scheduler.tick().value < k_ticks)
    w.tick->step();
}

}  // namespace

TEST_CASE("door cadence: a run recorded under irregular frames replays at any cadence") {
  sim::HashLog recorded;
  std::string door_bytes;
  {
    World w(false, recorded);
    const u32 stalls = record(w, 1234);
    w.door.serialize(door_bytes);
    MESSAGE("stalls " << stalls << ", door entries " << w.door.entries().size()
                      << ", first moved at tick " << w.first_moved);
    CHECK(stalls > 0);
    CHECK(w.first_moved > 1);
    CHECK(w.door.entries().size() > 20);
    CHECK(w.door.stats().delivered == w.door.entries().size());
    // Several frames' changes landed on one tick: the door kept them all, in order.
    u64 shared = 0;
    for (usize i = 1; i < w.door.entries().size(); ++i)
      shared += w.door.entries()[i].tick == w.door.entries()[i - 1].tick ? 1u : 0u;
    CHECK(shared > 0);
  }
  REQUIRE(recorded.rows().size() == k_ticks * sim::k_phase_count);

  for (int how = 0; how < 2; ++how) {
    sim::HashLog replayed;
    World w(false, replayed);
    std::string error;
    REQUIRE_MESSAGE(w.door.deserialize(door_bytes, &error), error);
    w.door.begin_replay();
    if (how == 0) {
      replay_headless(w);
    } else {
      replay_frames(w, 99);
      CHECK(w.door.stats().refused_live > 0);
    }
    const sim::Divergence d = sim::first_divergence(recorded, replayed);
    CHECK_FALSE(d.found);
    CHECK_FALSE(d.rows_differ);
    CHECK(d.rows_compared == k_ticks * sim::k_phase_count);
  }
}

TEST_CASE("door cadence: an input sampled at frame time is found at its tick, table and field") {
  // The bug is in both runs; only the cadence differs, which is the point: a run that depends on
  // how its frames fell is not the run its record replays.
  sim::HashLog recorded;
  std::string door_bytes;
  u64 first_moved = 0;
  {
    World w(true, recorded);
    record(w, 1234);
    w.door.serialize(door_bytes);
    first_moved = w.first_moved;
  }
  REQUIRE(first_moved != 0);
  sim::HashLog replayed;
  {
    World w(true, replayed);
    REQUIRE(w.door.deserialize(door_bytes));
    w.door.begin_replay();
    replay_headless(w);  // no frames, so nothing sampled: the walker stands
  }
  const sim::Divergence d = sim::first_divergence(recorded, replayed);
  REQUIRE(d.found);
  CHECK(d.tick == first_moved);
  CHECK(d.phase == sim::TickPhase::Input);
  REQUIRE(d.tables.size() == 1);
  CHECK(d.tables[0].table == "engine.ecs.demo.Standing");  // the door's own table agrees

  // The second step: both sides again to that (tick, phase), the table captured, the field named.
  sim::Capture a;
  sim::Capture b;
  {
    sim::HashLog log;
    World w(true, log);
    REQUIRE(w.hash->capture_at(d.tick, d.phase, d.tables[0].table, &a));
    record(w, 1234);
  }
  {
    sim::HashLog log;
    World w(true, log);
    REQUIRE(w.door.deserialize(door_bytes));
    w.door.begin_replay();
    REQUIRE(w.hash->capture_at(d.tick, d.phase, d.tables[0].table, &b));
    replay_headless(w);
  }
  u64 differing = 0;
  const sim::CaptureDiff diff = sim::diff_captures(a, b, 64, &differing);
  CHECK(differing == 1);
  REQUIRE(diff.entities.size() == 1);
  REQUIRE_FALSE(diff.entities[0].fields.empty());
  CHECK(diff.entities[0].fields[0].path == "position[0]");

  // The same walker reading its tick's input replays under the same frames to no divergence (the
  // case above), and the buggy one replayed under other frames parts at or after the same tick.
  sim::HashLog other;
  {
    World w(true, other);
    REQUIRE(w.door.deserialize(door_bytes));
    w.door.begin_replay();
    replay_frames(w, 99);
  }
  const sim::Divergence o = sim::first_divergence(recorded, other);
  CHECK(o.found);
}

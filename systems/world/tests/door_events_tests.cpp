// The door's record in the store's event log (systems/world/door_events.h; world.md, "The door in
// the store"; roadmap S18): a run's player-origin and client-origin entries written into the log as
// they happen, read back into a door that replays the run from the log alone, and hashed as part of
// the persistent state — the same rows however the writes fell, other rows for another input.
#include <domain/doc/document.h>
#include <domain/sim/door.h>
#include <domain/sim/scheduler.h>
#include <foundation/input/input.h>
#include <foundation/store/event_log.h>
#include <systems/world/door_events.h>
#include <systems/world/state_hash.h>
#include <systems/world/tile_store.h>

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace engine;
using namespace engine::world;

namespace {

struct Run {
  sim::SimScheduler scheduler;
  sim::Door door;
  input::ActionMap map;
  input::ActionId push = input::k_invalid_action;
  input::InputState state;
  std::vector<u64> pushed;

  Run() {
    push = map.add_action("push", input::ActionKind::Button);
    map.bind(push, input::Binding{input::Source::Key, 32u});
    state.set_map(map);
    door.install(scheduler);
    door.set_input_state(&state);
    sim::SystemDesc read;
    read.name = "test.read";
    read.phase = sim::TickPhase::Input;
    read.context = this;
    read.begin_tick = [](sim::SystemContext& c) {
      auto* self = static_cast<Run*>(c.context);
      if (self->state.pressed(self->push)) self->pushed.push_back(c.tick.value);
    };
    scheduler.add_system(read);
  }

  void press(f32 value = 1.0f, u32 device = 0) {
    input::RawEvent e;
    e.source = input::Source::Key;
    e.code = 32u;
    e.value = value;
    e.device = device;
    door.submit_input(e);
  }
};

// Thirty ticks: presses at every seventh, a client's flush after every eleventh, an agent's
// transaction (its commands' bytes) after the twentieth; the log written at `writes`.
void record(Run& run, WorldStore& store, std::initializer_list<u64> writes, u64 changed_at = 0) {
  u32 cursor = 0;
  const std::string commands = R"({"commands":[{"op":"set"}]})";
  for (u64 t = 1; t <= 30; ++t) {
    if (t % 7 == 0) {
      run.press(1.0f, t == changed_at ? 1u : 0u);
      run.press(0.0f);
    }
    run.scheduler.step();
    if (t % 11 == 0) run.door.record_between(sim::DoorOrigin::Agent, sim::door_flush_kind());
    if (t == 20)
      run.door.record_between(
          sim::DoorOrigin::Agent, 77u,
          std::span<const u8>(reinterpret_cast<const u8*>(commands.data()), commands.size()));
    for (u64 w : writes) {
      if (w == t) REQUIRE(append_door_events(store.log(), run.door, cursor) == store::Status::Ok);
    }
  }
  REQUIRE(append_door_events(store.log(), run.door, cursor) == store::Status::Ok);
  CHECK(cursor == run.door.entries().size());
}

StateHash hash_of(const doc::Document& d, WorldStore& store) {
  StateHash h;
  REQUIRE(state_hash(30, 500'000, d, &store.log(), h) == store::Status::Ok);
  return h;
}

}  // namespace

TEST_CASE("world door: a run's inputs are rows of the store's log, and replay from it alone") {
  WorldStore store;
  REQUIRE(store.open_memory() == store::Status::Ok);
  Run run;
  record(run, store, {5, 12, 21});
  REQUIRE(run.pushed.size() == 4);  // ticks 7, 14, 21, 28: pressed between 6 and 7, and so on
  CHECK(run.pushed[0] == 7);
  u64 rows = 0;
  REQUIRE(store.log().count_events(rows) == store::Status::Ok);
  // Eight input events, two flushes, one transaction.
  REQUIRE(run.door.entries().size() == 11);
  CHECK(rows == 11);

  // The log's rows carry the origin and the tick; nothing the simulation computed is among them.
  struct Seen {
    u32 player = 0;
    u32 agent = 0;
    u64 first_tick = 0;
  } seen;
  REQUIRE(store.log().replay(
              k_door_tile, 0,
              [](const store::EventRecord& row, void* user) {
                auto* s = static_cast<Seen*>(user);
                if (s->player + s->agent == 0) s->first_tick = row.sim_tick;
                if (row.origin == store::EventOrigin::Player) ++s->player;
                if (row.origin == store::EventOrigin::Agent) ++s->agent;
              },
              &seen) == store::Status::Ok);
  CHECK(seen.player == 8);
  CHECK(seen.agent == 3);
  CHECK(seen.first_tick == 7);

  // A door read back from the log is the door that recorded, byte for byte, and replays the run.
  Run replay;
  u32 read = 0;
  REQUIRE(read_door_events(store.log(), replay.door, &read) == store::Status::Ok);
  CHECK(read == 11);
  std::string a;
  std::string b;
  run.door.serialize(a);
  replay.door.serialize(b);
  CHECK(a == b);
  replay.door.begin_replay();
  for (u32 t = 0; t < 30; ++t) {
    replay.press();  // refused: a replay takes its input from the record
    replay.scheduler.step();
  }
  CHECK(replay.door.replay_pending() == 0);  // nothing after tick 30 in this record
  CHECK(replay.pushed == run.pushed);
  CHECK(replay.door.stats().refused_live == 30);
  CHECK(replay.door.stats().unhandled == 3);  // the flushes and the transaction: no handler here
  // A door that holds entries is not read into.
  CHECK(read_door_events(store.log(), replay.door) == store::Status::InvalidArgument);
}

TEST_CASE("world door: the state hash covers the inputs, however the log's writes fell") {
  doc::Document document;
  document.add_layer("base", doc::LayerRole::Base);
  WorldStore one;
  WorldStore two;
  WorldStore other;
  REQUIRE(one.open_memory() == store::Status::Ok);
  REQUIRE(two.open_memory() == store::Status::Ok);
  REQUIRE(other.open_memory() == store::Status::Ok);
  Run a;
  Run b;
  Run c;
  record(a, one, {3, 9, 10, 25});
  record(b, two, {});
  record(c, other, {}, 14);  // the press between 14 and 15 from a second keyboard
  const StateHash ha = hash_of(document, one);
  const StateHash hb = hash_of(document, two);
  const StateHash hc = hash_of(document, other);
  CHECK(ha.event_count == 11);
  CHECK(ha.events == hb.events);
  CHECK(ha.value == hb.value);
  // The same pushes came out (the state merges every device), but the input was not the same, and
  // the persistent state says so.
  CHECK(c.pushed == a.pushed);
  CHECK(hc.events != ha.events);
  CHECK(hc.value != ha.value);
}

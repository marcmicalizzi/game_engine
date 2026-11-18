// The one recorded door (domain/sim/door.h; docs/subsystems/sim.md, "The door"; roadmap S18): what
// a host submits between ticks reaches the world at the next tick whatever frame it came in, a
// replay hands it back at that tick from the record alone, and a run whose calls ended off the
// write-back cadence replays — split another way, or not at all — to the same commits.
#include <domain/sim/door.h>
#include <domain/sim/hash_log.h>
#include <domain/sim/scheduler.h>
#include <foundation/input/input.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>
#include <utility>
#include <vector>

using namespace engine;
using namespace engine::sim;

namespace {

// A world of one counter a system advances every tick, a player's push read in the Input phase,
// and a write-back stand-in: at `Persist` every `cadence` ticks — and whenever the host ends a
// call — it commits what changed since its last commit. That is the materialization driver's
// write-back (sim.md, "Write-back") reduced to one field: a commit carries the change since the
// last one, so an extra commit off the cadence changes what every later one carries.
struct World {
  SimScheduler scheduler;
  Door door;
  input::ActionMap map;
  input::ActionId push = input::k_invalid_action;
  input::InputState state;
  i64 value = 0;
  i64 committed = 0;
  u32 cadence = 5;
  std::vector<std::pair<u64, i64>> commits;  // (tick, change) per commit
  std::vector<std::pair<u64, u32>> pushes;   // (tick, presses seen that tick)

  World() {
    push = map.add_action("push", input::ActionKind::Button);
    map.bind(push, input::Binding{input::Source::Key, 32u});
    state.set_map(map);
    door.install(scheduler);
    door.set_input_state(&state);
    door.on(door_flush_kind(), DoorHandler{this, &World::replay_flush});

    SystemDesc read;
    read.name = "test.read_input";
    read.phase = TickPhase::Input;
    read.context = this;
    read.begin_tick = [](SystemContext& c) {
      auto* self = static_cast<World*>(c.context);
      if (self->state.pressed(self->push)) {
        self->value += 100;
        self->pushes.push_back({c.tick.value, 1u});
      }
    };
    scheduler.add_system(read);

    SystemDesc count;
    count.name = "test.count";
    count.phase = TickPhase::Systems;
    count.context = this;
    count.begin_tick = [](SystemContext& c) { ++static_cast<World*>(c.context)->value; };
    scheduler.add_system(count);

    SystemDesc persist;
    persist.name = "test.write_back";
    persist.phase = TickPhase::Persist;
    persist.context = this;
    persist.begin_tick = [](SystemContext& c) {
      auto* self = static_cast<World*>(c.context);
      if (c.tick.value % self->cadence == 0) self->commit(c.tick.value);
    };
    scheduler.add_system(persist);
  }

  void commit(u64 tick) {
    if (value == committed) return;
    commits.push_back({tick, value - committed});
    committed = value;
  }

  // A host's call ends: it flushes, and records that it did. In a replay it does neither: the
  // record says where the recorded run flushed and the door does it there.
  void end_call() {
    if (door.replaying()) return;
    commit(scheduler.tick().value);
    door.record_between(DoorOrigin::Agent, door_flush_kind());
  }

  static void replay_flush(void* context, const DoorEntry& entry, std::span<const u8>) {
    // The flush stands for the call that ended after `entry.tick - 1`.
    static_cast<World*>(context)->commit(entry.tick - 1);
  }

  void press_between_ticks() {
    input::RawEvent down;
    down.source = input::Source::Key;
    down.code = 32u;
    down.value = 1.0f;
    door.submit_input(down);
    input::RawEvent up = down;
    up.value = 0.0f;
    door.submit_input(up);
  }
};

}  // namespace

TEST_CASE("door: what a host submits between ticks reaches the world at the next tick") {
  World w;
  w.scheduler.step();  // tick 1
  // Two frames that run no tick: a press, then its release. Both reach tick 2, in that order, so
  // the press is seen once at tick 2 and the button is not held after it.
  w.press_between_ticks();
  CHECK(w.door.entries().size() == 2);
  CHECK(w.door.entries()[0].tick == 2);
  CHECK(w.door.entries()[0].delivery == DoorDelivery::Tick);
  CHECK(w.door.entries()[0].origin == DoorOrigin::Player);
  CHECK(w.pushes.empty());
  w.scheduler.step();  // tick 2
  REQUIRE(w.pushes.size() == 1);
  CHECK(w.pushes[0].first == 2);
  CHECK_FALSE(w.state.held(w.push));
  w.scheduler.step();
  CHECK(w.pushes.size() == 1);  // an event is delivered once
  CHECK(w.door.stats().delivered == 2);
  CHECK(w.door.stats().input_ticks == 3);

  // The encoding of an input event round-trips every field.
  input::RawEvent e;
  e.source = input::Source::GamepadAxis;
  e.code = 0x01020304u;
  e.value = -0.375f;
  e.device = 7u;
  u8 bytes[13];
  Door::encode_input(e, bytes);
  input::RawEvent back;
  REQUIRE(Door::decode_input(std::span<const u8>(bytes, 13), back));
  CHECK(back.source == e.source);
  CHECK(back.code == e.code);
  CHECK(back.value == e.value);
  CHECK(back.device == e.device);
  bytes[0] = 200;
  CHECK_FALSE(Door::decode_input(std::span<const u8>(bytes, 13), back));
}

TEST_CASE("door: a replay hands every input back at its tick from the record, and takes no other") {
  // The recorded run: calls of 3, 7, 1, 9 and 4 ticks, each ended with a flush, the pushes between
  // ticks inside them — the calls end off the write-back cadence of 5 four times out of five.
  World rec;
  const u64 calls[] = {3, 7, 1, 9, 4};
  u64 t = 0;
  for (u64 call : calls) {
    for (u64 i = 0; i < call; ++i) {
      if ((t * 7 + 3) % 5 == 0) rec.press_between_ticks();
      rec.scheduler.step();
      ++t;
    }
    rec.end_call();
  }
  REQUIRE(t == 24);
  REQUIRE(rec.pushes.size() >= 3);
  // The flush at the end of the 3-tick call is a commit the cadence alone would not make.
  REQUIRE(rec.commits.size() >= 2);
  CHECK(rec.commits[0].first == 3);

  std::string bytes;
  rec.door.serialize(bytes);

  // The run with no door replaying it — the same pushes, one call, no flush off the cadence — is
  // not the recorded run: its commits carry other changes. That is the split cadence.
  World plain;
  t = 0;
  for (u64 i = 0; i < 24; ++i, ++t) {
    if ((t * 7 + 3) % 5 == 0) plain.press_between_ticks();
    plain.scheduler.step();
  }
  plain.end_call();
  CHECK(plain.value == rec.value);
  CHECK(plain.commits != rec.commits);

  // Replayed from the record in one call, and again in calls of 5 and 19 that end with no flush
  // of their own; both live sides submit inputs the replay must refuse.
  for (const std::vector<u64>& split : {std::vector<u64>{24}, std::vector<u64>{5, 19}}) {
    World rep;
    std::string error;
    REQUIRE_MESSAGE(rep.door.deserialize(bytes, &error), error);
    rep.door.begin_replay();
    for (u64 call : split) {
      for (u64 i = 0; i < call; ++i) {
        rep.press_between_ticks();  // a frame's input, which a replay does not take
        rep.scheduler.step();
      }
      rep.end_call();
    }
    // The recorded run's last call ended after tick 24; that flush is applied here.
    CHECK(rep.door.replay_pending() == 1);
    CHECK(rep.door.stats().refused_live == 48);
    CHECK(rep.pushes == rec.pushes);
    CHECK(rep.commits == rec.commits);
    CHECK(rep.value == rec.value);
    CHECK(rep.door.delivered_hash() == rec.door.delivered_hash());
  }

  // A record cut short, out of order or naming an origin this build lacks is refused whole.
  Door bad;
  std::string error;
  CHECK_FALSE(bad.deserialize(std::string_view(bytes).substr(0, bytes.size() - 1), &error));
  CHECK(bad.entries().empty());
  std::string reordered = bytes;
  reordered[16] = static_cast<char>(0x7F);  // the first entry's tick, now after the second's
  CHECK_FALSE(bad.deserialize(reordered, &error));
  std::string origin = bytes;
  origin[16 + 28] = 9;
  CHECK_FALSE(bad.deserialize(origin, &error));
}

TEST_CASE("door: the record is a file a run record names beside its hash log") {
  World rec;
  rec.press_between_ticks();
  rec.scheduler.step();
  rec.end_call();
  test::TempDir dir;
  RunRecord record;
  record.ticks = 1;
  record.door_log = "inputs.door";
  record.hash_log = "run.hashlog";
  REQUIRE(rec.door.save(run_file(dir.path(), record.door_log)));
  REQUIRE(write_run_record(dir.path(), record));
  RunRecord read;
  std::string error;
  REQUIRE_MESSAGE(read_run_record(dir.path(), read, &error), error);
  CHECK(read.door_log == "inputs.door");
  Door back;
  REQUIRE_MESSAGE(back.load(run_file(dir.path(), read.door_log), &error), error);
  REQUIRE(back.entries().size() == rec.door.entries().size());
  std::string a;
  std::string b;
  rec.door.serialize(a);
  back.serialize(b);
  CHECK(a == b);
  CHECK(back.memory_bytes() >= 3 * sizeof(DoorEntry));
  // A door log that is not a plain name beside the record is refused at write.
  record.door_log = "../inputs.door";
  CHECK_FALSE(write_run_record(dir.path(), record, &error));
}

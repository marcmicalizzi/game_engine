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

// Hostile (T194, docs/experiments/fuzzing-design.md's fourth list): a door record whose entries all
// name the same payload bytes. Built here, a field at a time, by the layout `Door::serialize`
// writes: a 16-byte header (magic, entry count, payload bytes), 32 bytes an entry, the payload.
namespace hostile_door {

std::string record(u32 count, u32 payload_bytes, u32 (*offset_of)(u32), u32 size) {
  std::string out("ENGDOOR1", 8);
  const auto put32 = [&](u32 v) { out.append(reinterpret_cast<const char*>(&v), 4); };
  const auto put64 = [&](u64 v) { out.append(reinterpret_cast<const char*>(&v), 8); };
  put32(count);
  put32(payload_bytes);
  for (u32 i = 0; i < count; ++i) {
    put64(1);  // every entry at tick 1, so the ticks are in order
    put64(0);
    put32(7);  // a kind
    put32(offset_of(i));
    put32(size);
    out.push_back(static_cast<char>(DoorOrigin::Agent));
    out.push_back(static_cast<char>(DoorDelivery::Between));
    out.append(2, '\0');
  }
  out.append(payload_bytes, 'p');
  return out;
}

}  // namespace hostile_door

TEST_CASE("door hostile: entries that alias one payload are refused, so a load copies no more") {
  // 256 entries of 4 KiB each, laid end to end over 1 MiB: what a writer makes, and it loads.
  const std::string apart =
      hostile_door::record(256, 256 * 4096, [](u32 i) { return i * 4096u; }, 4096);
  Door door;
  std::string error;
  REQUIRE_MESSAGE(door.deserialize(apart, &error), error);
  CHECK(door.entries().size() == 256);
  CHECK(door.memory_bytes() >= 256 * 4096);

  // The same 256 entries all naming the one 4 KiB payload, a file of 12 KiB. Before the mend each
  // entry's bytes were appended: 256 x 4 KiB = 1 MiB from 12 KiB, and the review's shape — 4,096
  // entries over 1 MiB, a 1.1 MiB file — appended 4 GiB before `k_max_payload` refused it. The
  // before is that arithmetic, never the allocation.
  const std::string aliased = hostile_door::record(256, 4096, [](u32) { return 0u; }, 4096);
  CHECK(aliased.size() == 16 + 256 * 32 + 4096);
  CHECK(u64{256} * 4096 == u64{1} << 20);
  CHECK(u64{4096} * (u64{1} << 20) > 0xFFFF'FFFFull);
  CHECK_FALSE(door.deserialize(aliased, &error));
  CHECK(door.entries().empty());
  CHECK(error.find("door record entry 1 names payload bytes 0 to 4096") != std::string::npos);

  // Overlapping by one byte is refused the same way; touching is not overlapping.
  const std::string overlap = hostile_door::record(2, 8192, [](u32 i) { return i * 4095u; }, 4096);
  CHECK_FALSE(door.deserialize(overlap, &error));
  CHECK(error.find("entry 1") != std::string::npos);
  const std::string touching = hostile_door::record(2, 8192, [](u32 i) { return i * 4096u; }, 4096);
  CHECK_MESSAGE(door.deserialize(touching, &error), error);
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

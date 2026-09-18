#include <core/containers/vector.h>
#include <foundation/store/event_log.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>

using namespace engine;
using namespace engine::store;

// One unguessable scratch directory per object, so a second copy of this binary cannot delete
// this one's databases (tests/support/test_temp_dir.h).
using TempDir = test::TempDir;

namespace {

struct Seen {
  Vector<u64> sequences;
  Vector<u32> types;
  Vector<std::string> payloads;
  Vector<TileId> tiles;
  Vector<Id128> entities;
  Vector<u64> versions;
};

void collect_event(const EventRecord& record, void* user) {
  auto* seen = static_cast<Seen*>(user);
  seen->sequences.push_back(record.sequence);
  seen->types.push_back(record.type);
  seen->payloads.push_back(
      std::string(reinterpret_cast<const char*>(record.payload.data()), record.payload.size()));
}

void collect_projection(const ProjectionRecord& record, void* user) {
  auto* seen = static_cast<Seen*>(user);
  seen->entities.push_back(record.entity);
  seen->tiles.push_back(record.tile);
  seen->versions.push_back(record.version);
  seen->payloads.push_back(
      std::string(reinterpret_cast<const char*>(record.blob.data()), record.blob.size()));
}

std::span<const u8> bytes_of(std::string_view text) {
  return {reinterpret_cast<const u8*>(text.data()), text.size()};
}

EventRecord make_event(TileId tile, u32 type, std::string_view payload) {
  EventRecord record;
  record.tile = tile;
  record.type = type;
  record.sim_tick = 100 + type;
  record.game_time_us = 1'000'000 * static_cast<i64>(type);
  record.subject = Id128::from_seed(7, type);
  record.payload = bytes_of(payload);
  return record;
}

}  // namespace

TEST_CASE("store: the event log creates its schema and reports its version") {
  Database db;
  REQUIRE(db.open_memory() == Status::Ok);
  EventLog log(db);
  REQUIRE(log.open() == Status::Ok);
  i32 version = 0;
  REQUIRE(db.user_version(version) == Status::Ok);
  CHECK(version == EventLog::schema_version());
  // Idempotent: a second open finds the tables and changes nothing.
  REQUIRE(log.open() == Status::Ok);
  REQUIRE(db.user_version(version) == Status::Ok);
  CHECK(version == EventLog::schema_version());
}

TEST_CASE("store: appends get ascending per-tile sequences and replay in order") {
  Database db;
  REQUIRE(db.open_memory() == Status::Ok);
  EventLog log(db);
  REQUIRE(log.open() == Status::Ok);

  EventRecord a = make_event(10, 1, "first");
  EventRecord b = make_event(10, 2, "second");
  EventRecord other = make_event(11, 3, "other tile");
  REQUIRE(log.append(a) == Status::Ok);
  REQUIRE(log.append(b) == Status::Ok);
  REQUIRE(log.append(other) == Status::Ok);
  CHECK(a.sequence == 1);
  CHECK(b.sequence == 2);
  CHECK(other.sequence == 1);  // sequences are per tile, so tile 11 starts at 1 again

  EventRecord batch[] = {make_event(10, 4, "third"), make_event(10, 5, "fourth")};
  REQUIRE(log.append(std::span<EventRecord>(batch, 2)) == Status::Ok);
  CHECK(batch[0].sequence == 3);
  CHECK(batch[1].sequence == 4);

  u64 next = 0;
  REQUIRE(log.next_sequence(10, next) == Status::Ok);
  CHECK(next == 5);

  Seen seen;
  REQUIRE(log.replay(10, 1, &collect_event, &seen) == Status::Ok);
  REQUIRE(seen.sequences.size() == 4);
  CHECK(seen.sequences[0] == 1);
  CHECK(seen.sequences[3] == 4);
  CHECK(seen.types[0] == 1);
  CHECK(seen.types[3] == 5);
  CHECK(seen.payloads[0] == "first");
  CHECK(seen.payloads[3] == "fourth");

  // Replay from the middle: the tail only, which is what reconciliation after a snapshot reads.
  Seen tail;
  REQUIRE(log.replay(10, 3, &collect_event, &tail) == Status::Ok);
  REQUIRE(tail.sequences.size() == 2);
  CHECK(tail.sequences[0] == 3);

  // A tile with nothing in it replays nothing rather than failing.
  Seen empty;
  REQUIRE(log.replay(99, 1, &collect_event, &empty) == Status::Ok);
  CHECK(empty.sequences.empty());
}

TEST_CASE("store: a failed batch append leaves the log untouched") {
  Database db;
  REQUIRE(db.open_memory() == Status::Ok);
  EventLog log(db);
  REQUIRE(log.open() == Status::Ok);

  EventRecord first = make_event(3, 1, "one");
  REQUIRE(log.append(first) == Status::Ok);

  // The second record of the batch reuses a sequence that is already taken.
  EventRecord batch[] = {make_event(3, 2, "two"), make_event(3, 3, "collides")};
  batch[1].sequence = 1;
  CHECK(log.append(std::span<EventRecord>(batch, 2)) == Status::Constraint);

  Seen seen;
  REQUIRE(log.replay(3, 1, &collect_event, &seen) == Status::Ok);
  REQUIRE(seen.sequences.size() == 1);
  CHECK(seen.payloads[0] == "one");
}

TEST_CASE("store: the log survives a reopen and keeps counting where it left off") {
  TempDir dir("engine_store_event");
  const std::string path = dir.file("events.db");
  {
    Database db;
    REQUIRE(db.open(path) == Status::Ok);
    EventLog log(db);
    REQUIRE(log.open() == Status::Ok);
    EventRecord a = make_event(5, 1, "before");
    REQUIRE(log.append(a) == Status::Ok);
    CHECK(a.sequence == 1);
  }
  {
    Database db;
    REQUIRE(db.open(path) == Status::Ok);
    EventLog log(db);
    REQUIRE(log.open() == Status::Ok);
    EventRecord b = make_event(5, 2, "after");
    REQUIRE(log.append(b) == Status::Ok);
    CHECK(b.sequence == 2);  // read back from the file, not from a counter that reset
    Seen seen;
    REQUIRE(log.replay(5, 1, &collect_event, &seen) == Status::Ok);
    CHECK(seen.sequences.size() == 2);
  }
}

TEST_CASE("store: projections are read back by tile in key order") {
  Database db;
  REQUIRE(db.open_memory() == Status::Ok);
  EventLog log(db);
  REQUIRE(log.open() == Status::Ok);

  for (u32 i = 0; i < 6; ++i) {
    ProjectionRecord record;
    record.entity = Id128::from_seed(42, i);
    record.tile = i % 2 == 0 ? 100 : 200;
    record.kind = 1;
    record.version = i;
    const std::string payload = "p" + std::to_string(i);
    record.blob = bytes_of(payload);
    REQUIRE(log.upsert_projection(record) == Status::Ok);
  }

  u64 count = 0;
  REQUIRE(log.count_projections(100, count) == Status::Ok);
  CHECK(count == 3);
  REQUIRE(log.count_projections(200, count) == Status::Ok);
  CHECK(count == 3);

  Seen seen;
  REQUIRE(log.projections_by_tile(100, &collect_projection, &seen) == Status::Ok);
  REQUIRE(seen.entities.size() == 3);
  for (const TileId tile : seen.tiles)
    CHECK(tile == 100);
  // Key order, which is what makes a tile load one contiguous scan. SQLite orders the two id
  // halves as signed integers, so the comparison mirrors that rather than Id128's own.
  const auto key_less = [](const Id128& a, const Id128& b) {
    const i64 a_hi = static_cast<i64>(a.hi);
    const i64 b_hi = static_cast<i64>(b.hi);
    if (a_hi != b_hi) return a_hi < b_hi;
    return static_cast<i64>(a.lo) < static_cast<i64>(b.lo);
  };
  CHECK(key_less(seen.entities[0], seen.entities[1]));
  CHECK(key_less(seen.entities[1], seen.entities[2]));

  // A tile nobody wrote to is empty, not an error.
  Seen none;
  REQUIRE(log.projections_by_tile(7, &collect_projection, &none) == Status::Ok);
  CHECK(none.entities.empty());
}

TEST_CASE("store: a projection is replaced in place and follows an entity between tiles") {
  Database db;
  REQUIRE(db.open_memory() == Status::Ok);
  EventLog log(db);
  REQUIRE(log.open() == Status::Ok);

  const Id128 entity = Id128::from_seed(1, 1);
  ProjectionRecord record;
  record.entity = entity;
  record.tile = 10;
  record.kind = 3;
  record.version = 1;
  record.blob = bytes_of("alive");
  REQUIRE(log.upsert_projection(record) == Status::Ok);

  record.version = 2;
  record.blob = bytes_of("hurt");
  REQUIRE(log.upsert_projection(record) == Status::Ok);

  ProjectionRecord loaded;
  REQUIRE(log.load_projection(entity, 3, loaded) == Status::Ok);
  CHECK(loaded.version == 2);
  CHECK(loaded.tile == 10);
  CHECK(std::string(reinterpret_cast<const char*>(loaded.blob.data()), loaded.blob.size()) ==
        "hurt");

  u64 count = 0;
  REQUIRE(log.count_projections(10, count) == Status::Ok);
  CHECK(count == 1);

  // Moving tiles is a delete and an insert, because the tile is part of the key; one row, in
  // the new tile.
  record.tile = 11;
  record.version = 3;
  record.blob = bytes_of("moved");
  REQUIRE(log.upsert_projection(record) == Status::Ok);
  REQUIRE(log.count_projections(10, count) == Status::Ok);
  CHECK(count == 0);
  REQUIRE(log.count_projections(11, count) == Status::Ok);
  CHECK(count == 1);
  REQUIRE(log.load_projection(entity, 3, loaded) == Status::Ok);
  CHECK(loaded.tile == 11);
  CHECK(loaded.version == 3);

  // A different kind on the same entity is a separate row.
  record.kind = 4;
  REQUIRE(log.upsert_projection(record) == Status::Ok);
  REQUIRE(log.count_projections(11, count) == Status::Ok);
  CHECK(count == 2);

  REQUIRE(log.erase_projection(entity, 3) == Status::Ok);
  CHECK(log.load_projection(entity, 3, loaded) == Status::NotFound);
  REQUIRE(log.count_projections(11, count) == Status::Ok);
  CHECK(count == 1);

  CHECK(log.load_projection(Id128::from_seed(9, 9), 1, loaded) == Status::NotFound);
}

TEST_CASE("store: a snapshot round-trips a tile's projections and bounds replay") {
  Database db;
  REQUIRE(db.open_memory() == Status::Ok);
  EventLog log(db);
  REQUIRE(log.open() == Status::Ok);

  Vector<std::string> payloads;
  payloads.reserve(16);
  for (u32 i = 0; i < 16; ++i) {
    payloads.push_back(std::string(64 + i, static_cast<char>('a' + (i % 26))));
    ProjectionRecord record;
    record.entity = Id128::from_seed(11, i);
    record.tile = 77;
    record.kind = i % 3;
    record.version = i + 1;
    record.blob = bytes_of(payloads[i]);
    REQUIRE(log.insert_projection(record) == Status::Ok);
  }
  for (u32 i = 0; i < 5; ++i) {
    EventRecord event = make_event(77, i, "e");
    REQUIRE(log.append(event) == Status::Ok);
  }

  SnapshotInfo info;
  REQUIRE(log.snapshot(77, 4242, 99'000'000, info) == Status::Ok);
  CHECK(info.record_count == 16);
  CHECK(info.sequence == 5);  // the last event the snapshot covers
  CHECK(info.sim_tick == 4242);
  CHECK(info.blob_bytes > 16 * 64);

  Seen seen;
  SnapshotInfo read;
  REQUIRE(log.load_snapshot(77, read, &collect_projection, &seen) == Status::Ok);
  CHECK(read.record_count == 16);
  CHECK(read.sequence == info.sequence);
  CHECK(read.game_time_us == 99'000'000);
  REQUIRE(seen.entities.size() == 16);
  CHECK(seen.versions[0] >= 1);
  CHECK(seen.payloads[0].size() >= 64);

  // Load: wipe the tile, restore the snapshot, replay only what came after it.
  REQUIRE(db.exec("DELETE FROM projections WHERE tile=77") == Status::Ok);
  u64 count = 0;
  REQUIRE(log.count_projections(77, count) == Status::Ok);
  CHECK(count == 0);
  SnapshotInfo restored;
  REQUIRE(log.restore_snapshot(77, restored) == Status::Ok);
  REQUIRE(log.count_projections(77, count) == Status::Ok);
  CHECK(count == 16);
  CHECK(restored.sequence == info.sequence);

  Seen tail;
  REQUIRE(log.replay(77, restored.sequence + 1, &collect_event, &tail) == Status::Ok);
  CHECK(tail.sequences.empty());  // the snapshot covers everything written so far

  EventRecord later = make_event(77, 9, "after the snapshot");
  REQUIRE(log.append(later) == Status::Ok);
  Seen after;
  REQUIRE(log.replay(77, restored.sequence + 1, &collect_event, &after) == Status::Ok);
  REQUIRE(after.sequences.size() == 1);
  CHECK(after.payloads[0] == "after the snapshot");

  // A tile with no snapshot says so.
  SnapshotInfo missing;
  CHECK(log.load_snapshot(1234, missing, &collect_projection, &seen) == Status::NotFound);

  // Snapshotting again replaces the old one rather than accumulating.
  SnapshotInfo second;
  REQUIRE(log.snapshot(77, 4243, 100'000'000, second) == Status::Ok);
  CHECK(second.sequence == 6);
  Statement stmt;
  REQUIRE(db.prepare("SELECT count(*) FROM snapshots WHERE tile=77", stmt) == Status::Ok);
  bool row = false;
  REQUIRE(stmt.step(row) == Status::Ok);
  CHECK(stmt.column_i64(0) == 1);
}

TEST_CASE("store: origins have names") {
  CHECK(std::string_view(origin_name(EventOrigin::Deterministic)) == "deterministic");
  CHECK(std::string_view(origin_name(EventOrigin::Llm)) == "llm");
}

#pragma once

// The persistent world state of docs/plan/03-data-model.md §3.5 and ADR-0003, as three tables:
// an append-only event log, the projections the log folds down to, and one snapshot per tile
// that bounds how far a load has to replay.
//
// Why these keys, since the keys are the whole design:
//
//   events(tile, sequence)        WITHOUT ROWID. Every read of the log is "this tile, from this
//                                 sequence onward" (plan 05 §5.5 reconciliation), so storing the
//                                 rows in exactly that order makes a replay one contiguous
//                                 B-tree walk instead of an index probe per event, and makes an
//                                 append an insert at the right edge of the tree. The sequence
//                                 is per tile, not global, so two tiles being written by two
//                                 threads never contend for one counter.
//
//   projections(tile, entity, kind)   WITHOUT ROWID, with a secondary index on (entity, kind).
//                                 The dominant access is "materialize this tile" (plan 05 §5.5),
//                                 which wants every projection of a tile contiguous; point
//                                 lookup by entity is the rarer one and pays a secondary index.
//                                 The consequence to know about: an entity that moves between
//                                 tiles is a delete and an insert, not an update, because its
//                                 tile is part of its key. `upsert_projection` pays one extra
//                                 index seek to do that correctly; `insert_projection` skips it
//                                 for a bulk load where the caller knows the row is new.
//
//   snapshots(tile)               A rowid table whose rowid *is* the tile, so a lookup is one
//                                 B-tree descent. A snapshot blob is megabytes and lives in
//                                 overflow pages whatever the key, so WITHOUT ROWID would buy
//                                 nothing here.
//
// Everything is `Status`-returning and callbacks are plain function pointers, so nothing in the
// read path allocates or dispatches indirectly through a std::function.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <foundation/store/database.h>

#include <span>

namespace engine {
namespace store {

// A tile of the world grid (plan 03 §3.7). The engine packs (x, y[, level]) into this however
// the game's grid is shaped; the store only ever compares and orders it.
using TileId = u64;

// Where a change came from, so that replay reads recorded non-determinism from the log instead
// of re-executing it (plan 03 §3.10).
enum class EventOrigin : u8 {
  Deterministic = 0,
  Player = 1,
  Agent = 2,
  Llm = 3,
  Debug = 4,
};
const char* origin_name(EventOrigin origin) noexcept;

// One `WorldEvent` of plan 03 §3.5. `payload` is schema-typed bytes the store never interprets.
struct EventRecord {
  TileId tile = 0;
  u64 sequence = 0;  // per tile, ascending; 0 on append means "assign the next one"
  u64 sim_tick = 0;
  i64 game_time_us = 0;
  u32 type = 0;   // schema type id
  u16 depth = 0;  // causal chain length, bounded by the event budget of plan 05 §5.7
  EventOrigin origin = EventOrigin::Deterministic;
  Id128 subject;
  u64 cause = 0;  // the causal parent's sequence within the same tile; 0 for none
  std::span<const u8> payload;
};

// The current state of one (entity, kind) pair: a fold of the events that touched it.
struct ProjectionRecord {
  Id128 entity;
  TileId tile = 0;
  u32 kind = 0;     // schema type id of the projected component
  u64 version = 0;  // bumped on every write; a reader can tell a stale copy from a fresh one
  std::span<const u8> blob;
};

// What a snapshot of one tile covers.
struct SnapshotInfo {
  TileId tile = 0;
  u64 sequence = 0;  // the log sequence the snapshot includes; replay resumes after it
  u64 sim_tick = 0;
  i64 game_time_us = 0;
  u32 record_count = 0;
  u32 blob_bytes = 0;
};

// Visitors are function pointers, not std::function: a replay of a million rows must not pay an
// indirect call through a type-erased object per row, and nothing here needs to capture.
using EventVisitor = void (*)(const EventRecord& record, void* user);
using ProjectionVisitor = void (*)(const ProjectionRecord& record, void* user);
// A snapshot's row and its packed blob, as `visit_snapshots` hands them over.
using SnapshotVisitor = void (*)(const SnapshotInfo& info, std::span<const u8> blob, void* user);

// A place in the whole log, across tiles, in the order `EventLog::scan` walks it: simulation tick,
// then tile, then the tile's own sequence. The sequence alone is per tile, so it cannot say "after
// this event" for the log as a whole; the tick leads because a log is appended in tick order, so a
// reader that stops at a position and comes back later finds every event appended since after it.
// The tile and the tick are compared as the table stores them, as signed 64-bit integers, which is
// consistent between the comparison and the order and is all a cursor needs.
struct LogPosition {
  u64 sim_tick = 0;
  TileId tile = 0;
  u64 sequence = 0;
};

class EventLog {
 public:
  explicit EventLog(Database& db) noexcept : db_(&db) {}
  ENGINE_NON_COPYABLE(EventLog);

  // Creates the tables if they are absent and migrates an older file forward. Idempotent.
  Status open();
  // The schema version this build writes.
  static i32 schema_version() noexcept;

  // Appends one event. `record.sequence` is filled in when it was 0, so the caller learns the
  // sequence it got. Wrap a tick's worth of appends in a Database::begin() transaction: one
  // commit per tick is the difference between thousands and tens of appends a second.
  Status append(EventRecord& record);
  // Appends a batch in one transaction of its own. Sequences are assigned per tile in order.
  Status append(std::span<EventRecord> records);

  // The next sequence `append` would assign for a tile. Cached per tile after the first query.
  Status next_sequence(TileId tile, u64& out);

  // Visits the tile's events with sequence >= from_sequence, in sequence order. The record and
  // its payload are valid only for the duration of the call.
  Status replay(TileId tile, u64 from_sequence, EventVisitor fn, void* user);

  // Visits at most `limit` events of the whole log in `LogPosition` order — (tick, tile,
  // sequence) — with a tick of at least `since_tick` and, when `after` is not null, strictly after
  // it. It is the reader for tools and the protocol (`session.events`), not for the simulation: the
  // table's key leads with the tile, so this is a sort over the matching rows rather than a walk of
  // the B-tree, which is fine for an introspection page and wrong for anything that runs per tick.
  // The record and its payload are valid only for the duration of the call.
  Status scan(const LogPosition* after, u64 since_tick, u32 limit, EventVisitor fn, void* user);
  // How many events the log holds, over every tile.
  Status count_events(u64& out);

  // Writes the projection, replacing any previous value for (entity, kind) — including one
  // filed under a different tile, which costs one seek of the secondary index per call.
  Status upsert_projection(const ProjectionRecord& record);
  Status upsert_projections(std::span<const ProjectionRecord> records);
  // For a bulk load, or after erase_projection: writes the row assuming the pair is not already
  // filed under a different tile. Skips the seek that upsert_projection pays for that case; a
  // row already present under the *same* tile is still replaced.
  Status insert_projection(const ProjectionRecord& record);
  // Removes every row for (entity, kind), whatever tile it is filed under.
  Status erase_projection(Id128 entity, u32 kind);
  // NotFound when the pair has never been written. `out.blob` points into a buffer this log
  // owns — the statement it was read through goes back to the cache before this returns — so it
  // is valid until the next `load_projection` on this log, and no further.
  Status load_projection(Id128 entity, u32 kind, ProjectionRecord& out);
  // Visits every projection of a tile in the table's key order, which is SQLite's *signed*
  // ordering of the two halves of the id: contiguous and stable, but not the order Id128's own
  // comparison gives. Nothing here depends on which of the two it is.
  Status projections_by_tile(TileId tile, ProjectionVisitor fn, void* user);
  Status count_projections(TileId tile, u64& out);

  // Packs every projection of the tile into one blob and stores it against the tile's current
  // log sequence, replacing the previous snapshot. This is what bounds replay length: a load
  // reads the snapshot and replays only the events after `info.sequence`. The sequence is read
  // from the file, never from this log's append cache: another connection to the same file may
  // have appended to the tile since this one last looked, and a snapshot that claimed less than
  // the file holds would replay those events twice (store.md, "Two connections, one file").
  Status snapshot(TileId tile, u64 sim_tick, i64 game_time_us, SnapshotInfo& info);
  // Reads the snapshot back and hands each packed record to `fn`. NotFound when the tile has
  // none. The records point into the snapshot blob and are valid only inside the call.
  Status load_snapshot(TileId tile, SnapshotInfo& info, ProjectionVisitor fn, void* user);
  // Writes the tile's snapshot records back into the projections table, in one transaction.
  Status restore_snapshot(TileId tile, SnapshotInfo& info);

  // The whole store, table by table, in each table's key order: events by (tile, sequence),
  // projections by (tile, entity, kind), snapshots by tile — tile ids and the halves of an id
  // compared as the tables store them, signed. That order is a function of the rows alone, never
  // of when or through which connection they were written, which is what a canonical hash of the
  // store reads them in (`world::state_hash`, docs/subsystems/world.md, "The persistent-state
  // hash"). Records and blobs are valid only inside the call. A walk of the whole table, for a
  // hash or a tool, not for anything per tick.
  Status visit_events(EventVisitor fn, void* user);
  Status visit_projections(ProjectionVisitor fn, void* user);
  Status visit_snapshots(SnapshotVisitor fn, void* user);

 private:
  Status highest_sequence(TileId tile, u64& out);
  Status append_locked(EventRecord& record);

  Database* db_ = nullptr;
  HashMap<TileId, u64> next_sequence_;  // per-tile high-water mark, so an append is one insert
  Vector<u8> loaded_blob_;              // what load_projection's returned span points into
};

}  // namespace store
}  // namespace engine

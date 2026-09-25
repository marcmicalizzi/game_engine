#pragma once

// The persistent-state hash (docs/plan/05-simulation.md §5.10: "input log + event log => replay.
// CI runs replays and asserts identical persistent-state hashes"; docs/subsystems/world.md, "The
// persistent-state hash"). One 64-bit number that two runs of the same world agree on exactly when
// they left the same persistent state behind, defined once, here, so a test, engine-host's
// `session.state_hash`, a save's manifest and an agent all compute the same thing.
//
// **What it covers is what outlives the runtime world** (03 §3.4: the runtime world is a
// materialization and never the source of truth):
//
//   clock      the tick and the game time
//   document   every live record of the session's document, composed across its layers, in id
//              order: its id, its type, its parent and every property, by name, as canonical JSON.
//              That is where the world's write-back lands, and it is what an agent's edit changes
//   store      every row of the store's three tables in key order (`store::EventLog::visit_*`):
//              events by (tile, sequence) with their payloads, projections by (tile, entity, kind)
//              with their blobs, snapshots by tile with their blobs
//
// **What it leaves out, and why.** The journal: a patch's attribution carries the wall-clock time
// it was committed at, so two runs of the same world never write the same journal, and the journal
// is the document's history, not the world's state. The layer a property sits in and the files a
// layer is stored as: the composed value is the state, the storage is not. The runtime world —
// entities, components, flecs ids — which is rebuilt from the document and the store on load. The
// tile ring and its observers, which drive a run rather than result from it and which a save keeps
// beside the hash.
//
// **Canonical, so the same on every machine.** Every integer is hashed as its little-endian bytes,
// every row in the order its table's key sorts it, every property in name order, every value as the
// JSON writer's canonical text (sorted keys, shortest round-trip floats), and the hash is
// `hash_bytes` and `hash_combine` from core/hash, which say they are the same on every platform.
// Nothing here reads a clock, iterates a hash table or depends on how many threads ran the world.

#include <core/base/types.h>
#include <domain/doc/document.h>
#include <foundation/store/database.h>
#include <foundation/store/event_log.h>

#include <string>
#include <string_view>

namespace engine::world {

struct StateHash {
  // The whole: the clock, the document and the store, combined in that order.
  u64 value = 0;
  u64 clock = 0;
  u64 document = 0;
  u64 events = 0;
  u64 projections = 0;
  u64 snapshots = 0;
  u64 records = 0;  // live document records hashed
  u64 event_count = 0;
  u64 projection_count = 0;
  u64 snapshot_count = 0;
  // A store was hashed. Without one the three store parts are the hash of an empty table.
  bool store = false;
};

// Hashes the persistent state of a world at (tick, game time) over `document` and, when `log` is
// not null, the store it names. A store that cannot be read is the status, with `out` untouched.
store::Status state_hash(u64 tick, i64 game_time_us, const doc::Document& document,
                         store::EventLog* log, StateHash& out);

// 16 lowercase hex digits, the form a hash is written in everywhere outside a process.
std::string hash_hex(u64 value);
bool parse_hash_hex(std::string_view text, u64& out) noexcept;

}  // namespace engine::world

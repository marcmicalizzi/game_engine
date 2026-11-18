#pragma once

// The state hash a tick, its log, a recorded run and the divergence bisector's two halves
// (docs/plan/09-testing-profiling.md §9.2, docs/plan/05-simulation.md §5.10; docs/subsystems/sim.md,
// "The hash log and the bisector"; roadmap T53).
//
// **What is here and what is not.** This module is free of flecs (sim.md, "The tick"), so what
// computes a table's hash — `ecs::TickHash`, which walks the entity store's tables through schema
// reflection — lives in `domain/ecs`, and what it computes lands here: a `HashLog` of one row per
// (tick, phase) hashed, one 64-bit hash per *table* in the row. A table is a component type the
// entity store holds, or a source a host added (the event bus's delivery hash, the timing wheel);
// the log names them once, in its header, and every row has one hash per name in that order.
//
// **The bisector is two steps, both here, neither needing a world.** `first_divergence` walks two
// logs in step and names the first (tick, phase) whose rows differ and every table that differs in
// it. Which entities and which fields differ is not in a log — a row is a hash, not the state — so
// the second step reads two *captures*: the table's entities at that (tick, phase), each as its
// component's canonical JSON, taken by re-running each side to that point (`ecs::TickHash`'s
// capture). `diff_captures` names the entities on one side only and, for the rest, every field
// whose value differs, by its path in the component (`position[0]`, `wealth`). `engine-cli --bisect`
// is both steps on files (docs/subsystems/apps.md).
//
// **A recorded run** (`RunRecord`) is what a replay needs and nothing else: the world's seed, the
// clock it started at and the ticks it ran, and the names of the files beside it — the input log
// (foundation/input's), the external raises (domain/events'), and the hash log the run wrote. A
// replay builds the world from the seed (the host's to do: what a world is belongs to whoever
// makes one), feeds the two logs at their ticks, hashes every tick into a fresh log and compares
// the two with `first_divergence`. The record names its files by plain name under its own
// directory and refuses anything else, so a record handed over from another machine cannot point
// a replay at a file outside it.
//
// **Canonical, so the same on every machine.** The log file is little-endian with fixed-width
// fields, a hash is `core/hash`'s, and a capture is the JSON writer's canonical text sorted by
// entity id: a log written by MSVC compares with one written by GCC.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <domain/sim/scheduler.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::sim {

// One (tick, phase) the hash ran at. The table hashes follow it in the log's flat array.
struct HashLogRow {
  u64 tick = 0;
  u32 phase = 0;  // `TickPhase` as a number: a row is laid over bytes on disk
  u32 first = 0;  // index of its first table hash in `HashLog::hashes()`
};

class HashLog {
 public:
  HashLog() = default;

  // The header: the tables' names, in the order every row holds them, and the phases hashed (bit p
  // for `TickPhase` p). Set once, before the first row; setting it again clears the rows.
  void set_tables(std::span<const std::string> names, u8 phases);
  std::span<const std::string> tables() const noexcept { return {tables_.data(), tables_.size()}; }
  u8 phases() const noexcept { return phases_; }
  // The table's index, or `~0u`.
  u32 table_index(std::string_view name) const noexcept;

  // Room for `ticks` ticks of rows, so a run of known length appends without allocating.
  void reserve(u64 ticks);
  // Appends a row: `hashes` has one entry per table. False, with nothing appended, when it does
  // not, or when (tick, phase) is not after the last row.
  bool append(u64 tick, TickPhase phase, std::span<const u64> hashes);
  void clear() noexcept;

  std::span<const HashLogRow> rows() const noexcept { return {rows_.data(), rows_.size()}; }
  std::span<const u64> hashes() const noexcept { return {hashes_.data(), hashes_.size()}; }
  std::span<const u64> row_hashes(u32 row) const noexcept;
  // Bytes the log holds: its rows and their hashes, and the names.
  usize memory_bytes() const noexcept;

  // The file form (sim.md, "The hash log's file"). `load` replaces the log; on failure it is left
  // empty and `error` says why, and nothing a file says is trusted before it is checked against the
  // file's own size.
  bool save(std::string_view native_path, std::string* error = nullptr) const;
  bool load(std::string_view native_path, std::string* error = nullptr);
  void serialize(std::string& out) const;
  bool deserialize(std::string_view bytes, std::string* error = nullptr);

 private:
  Vector<std::string> tables_;
  u8 phases_ = 0;
  Vector<HashLogRow> rows_;
  Vector<u64> hashes_;
};

// Where two logs first differ.
struct TableDivergence {
  std::string table;
  u64 a = 0;
  u64 b = 0;
  // The table is in one log's header only (and so absent from every row of the other).
  bool only_in_a = false;
  bool only_in_b = false;
};

struct Divergence {
  // Two logs that differ somewhere both cover. False also when one simply ran longer: `rows_a` and
  // `rows_b` say so.
  bool found = false;
  u64 tick = 0;
  TickPhase phase = TickPhase::Input;
  // Every table that differs at (tick, phase), in the first log's header order and then the second's
  // own tables.
  Vector<TableDivergence> tables;
  // A (tick, phase) one log has and the other does not, before any hash differed: the two hashed
  // different phases, or one skipped a tick.
  bool rows_differ = false;
  u64 rows_compared = 0;
  u64 rows_a = 0;
  u64 rows_b = 0;
};

// The first (tick, phase) where `a` and `b` differ, comparing tables by name. Linear in the rows
// both cover: a log is compared once, and a row is a few words.
Divergence first_divergence(const HashLog& a, const HashLog& b);

// ---- captures and their diff -------------------------------------------------------------------

// One table's entities at one (tick, phase): `entities` is an array of {"id", "value"} objects
// sorted by id, where `id` is the entity's `Id128` in hex (or `e<n>` for one with no identity) and
// `value` the component as the schema's canonical JSON.
struct Capture {
  u64 tick = 0;
  TickPhase phase = TickPhase::Input;
  std::string table;
  JsonValue entities = JsonValue::array();
};

JsonValue capture_to_json(const Capture& capture);
bool capture_from_json(const JsonValue& value, Capture& out, std::string* error = nullptr);

struct FieldDifference {
  std::string path;  // "wealth", "position[0]", "point.end_us"
  std::string a;     // the value as compact JSON, or "" when the field is on one side only
  std::string b;
};

struct EntityDifference {
  std::string id;
  bool only_in_a = false;
  bool only_in_b = false;
  Vector<FieldDifference> fields;
};

struct CaptureDiff {
  Vector<EntityDifference> entities;  // in id order
  u64 compared = 0;                   // entities on both sides
};

// The entities on one side only and, for the rest, every field whose value differs. `max_entities`
// bounds what is listed (the count of differing entities is still exact in `differing`).
CaptureDiff diff_captures(const Capture& a, const Capture& b, u32 max_entities = 64,
                          u64* differing = nullptr);

// ---- a recorded run ----------------------------------------------------------------------------

struct RunRecord {
  u64 seed = 0;
  u32 hz = 60;
  u64 start_tick = 0;
  i64 start_time_us = 0;
  u64 ticks = 0;
  // Plain file names beside the record, or empty for none.
  std::string input_log;
  std::string external_log;
  std::string hash_log;
  // The door's record (door.h, `Door::save`): every input the run took, at its tick. A record that
  // names one needs neither of the two above — the door carries both kinds — and a record written
  // before the door (2026-11-18) names none and reads as empty.
  std::string door_log;
};

// `run.json` in `directory`, canonical JSON. The names must be plain file names (no separator, no
// "..", not absolute): refused otherwise, at write and at read.
bool write_run_record(std::string_view directory, const RunRecord& record,
                      std::string* error = nullptr);
bool read_run_record(std::string_view directory, RunRecord& out, std::string* error = nullptr);
// `directory` joined with a name the record holds, after the same check; empty when refused.
std::string run_file(std::string_view directory, std::string_view name);
bool plain_file_name(std::string_view name) noexcept;

}  // namespace engine::sim

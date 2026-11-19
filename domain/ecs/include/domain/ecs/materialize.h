#pragma once

// The entity store's half of materialization (docs/plan/03-data-model.md §3.4; the driver is
// domain/sim/materialize.h).
//
// `RecordMaterializer` is a row of `sim::SimScheduler`'s materialization hooks and a
// `sim::MaterializeTarget`. Handed a document record by the driver, it
//
//   - finds the entity the record's `Id128` names, or creates it (ADR-0028 seam 3: the record's id
//     *is* the entity's key, so a record and its entity are one identity across saves);
//   - writes every mapped field of every component the record type's mapping names, through
//     `schema::read_mapped` — a direct copy, or a unit conversion the schema declared — one
//     component at a time, each marked modified once so observers and change detection see one
//     write per component;
//   - makes the document's parent the entity's `ChildOf` relationship when the mapping says so and
//     the parent has an entity, and keeps it in step when the parent changes;
//   - remembers the bytes of every `@writeback` field it wrote, so that later it can report which
//     of them a system changed (`target().collect_writeback`).
//
// **Creation is one table move, into a table found by type.** A new entity's whole archetype —
// `Identity`, every component, the parent pair — is looked up as one type (`ecs_table_find`) and
// the entity committed to it (`ecs_commit`), and its fields written in place. Walking there one id
// at a time (`ecs_entity_init` with `add`) would create and keep a table per step under every new
// parent; this way a parent costs its children one table.
//
// **It is seam 4's other client.** Like `WorldCommands`, it names entities by `Id128`, uses schema
// types and nothing hand-registered, and runs only between ticks: the driver is called by whoever
// owns the tick, outside it, and a call while the world is deferred or readonly is refused. Unlike
// `WorldCommands` it writes fields and not whole components, because an update must leave alone the
// fields no mapping names — a velocity a system integrated, a cache a system filled.
//
// **It never cascades.** flecs deletes an entity's `ChildOf` children with it; the document does
// not delete a record's children with it. So `dematerialize` first moves the entity's children to
// the root, and a child the document still holds keeps its entity (the driver re-links it when the
// parent comes back).
//
// **What a relationship costs.** `ChildOf` is a pair, and every distinct parent is a distinct
// archetype for its children (ADR-0028's guard rails): a document of n parents with children makes
// about n more tables. That is flecs' hierarchy and what `cascade()` queries need; the table
// watchdog is what notices a hierarchy nobody sized.
//
// **Write-back visits what changed** (roadmap S11; docs/subsystems/ecs.md, "Write-back by change
// detection"). A flush compares the shadow only in the tables flecs' own change detection says a
// tracked write touched since the last flush — a system's `inout` term, `set`, or `ensure` followed
// by `modified` — and skips the rest whole. What it cannot see is a write through a raw pointer
// with no `modified` after it: a capability that writes a `@writeback` component outside a system's
// query calls `modified`. `WritebackScan::Every` is the scan of every watched entity it replaced,
// kept as the oracle, and `verify` runs it behind the tracked scan and counts what the tracking
// missed.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/schema/materialize.h>
#include <domain/sim/materialize.h>
#include <domain/sim/scheduler.h>

#include <flecs.h>

namespace engine::ecs {

// What the hook has done, cumulatively.
struct RecordStats {
  u64 created = 0;
  u64 updated = 0;
  u64 destroyed = 0;
  u64 component_writes = 0;  // components written (ensured, filled, marked modified)
  u64 invalid_values = 0;  // property values that did not fit their field; the field kept its value
  u64 unlinked_children = 0;  // children moved to the root instead of cascading
  u64 writeback_scans = 0;    // collect_writeback calls
  u64 writeback_changes = 0;  // changed fields they reported
  u64 writeback_tables = 0;   // tables a tracked scan compared (a table once per scan)
  u64 writeback_skipped = 0;  // tables a tracked scan skipped because nothing tracked wrote them
  u64 writeback_rows = 0;     // watched entities compared against their shadow
  u64 writeback_missed = 0;   // with `verify`: changed fields the tracked scan did not see
};

// How a write-back flush finds what changed.
enum class WritebackScan : u8 {
  // The tables flecs' change detection says a tracked write touched since the last flush, and
  // nothing else: one cached query per mapping that writes back, a byte compare per watched entity
  // of a changed table. The default.
  Changed,
  // Every watched entity, every flush (what write-back did before roadmap S11): the oracle a
  // tracked scan is held to, and the answer for a world whose writers cannot all call `modified`.
  Every,
};

struct RecordMaterializerConfig {
  WritebackScan scan = WritebackScan::Changed;
  // With `Changed`: also run the scan of every entity after the tracked one, report what it found
  // as well (so the flush is the oracle's), count it in `writeback_missed` and warn once per flush
  // that missed something, naming the record and the component. For finding a writer that
  // bypasses the tracking; it costs the full scan.
  bool verify = false;
};

class RecordMaterializer {
 public:
  // The world must outlive this object. Construct it outside a tick: it makes sure the identity
  // map and the component table exist, as `WorldCommands` does, for the same reason.
  explicit RecordMaterializer(flecs::world& world, const RecordMaterializerConfig& config = {});
  ~RecordMaterializer();
  ENGINE_NON_COPYABLE(RecordMaterializer);

  const RecordMaterializerConfig& config() const noexcept { return config_; }

  // The hooks row. Register it with `SimScheduler::add_hooks` **before** any capability's: the
  // first hook to give a record a runtime existence names it, and the others attach to what this
  // one created; dematerialization walks the table backwards, so this one lets go last.
  sim::MaterializationHooks hooks() noexcept;
  // The target the driver asks what this world can hold, resolves ids through, and collects
  // write-back from.
  sim::MaterializeTarget target() noexcept;

  const RecordStats& stats() const noexcept { return stats_; }
  // Entities whose writable fields are being watched for write-back.
  u32 watched() const noexcept { return watches_.size(); }
  // What the write-back holds, in bytes (the owner's memory budget): the shadow slabs and the
  // watch map, which grow with the watched entities, and the tracked scan's cached queries, one per
  // mapping that writes back and nothing until the first entity of that mapping is watched. flecs'
  // own per-table cost of the tracking — a change counter per column and a query's monitor per
  // matched table — is not in it; `docs/subsystems/ecs.md` states it.
  usize writeback_bytes() const noexcept;
  // Cached queries the tracked scan holds (one per mapping with `@writeback` rows that has watched
  // an entity).
  u32 writeback_queries() const noexcept;

 private:
  // One mapping, resolved against this world once: flecs ids, field descriptors, the write-back
  // rows and their shadow storage.
  struct Group {
    flecs::entity_t component = 0;
    u32 size = 0;
    u32 first_row = 0;
    u32 rows = 0;
  };
  struct Resolved {
    const schema::MaterializeInfo* info = nullptr;
    bool usable = false;
    Vector<Group> groups;
    Vector<const schema::FieldInfo*> fields;  // one per mapping row
    Vector<u32> row_group;                    // the group each row belongs to
    Vector<u32> writable;                     // the rows that write back
    u32 shadow_bytes = 0;                     // one entity's worth of writable bytes
    Vector<u8> shadow;                        // slots of shadow_bytes
    Vector<u32> free_slots;
    u32 slots = 0;
    // The tracked scan's query: `Identity` and every component a writable row lives in, `in`, with
    // change detection; made when the mapping's first entity is watched. `writable_term[k]` is the
    // query field of `writable[k]`'s component.
    ecs_query_t* query = nullptr;
    Vector<u8> writable_term;
  };
  struct Watch {
    u32 resolved = 0;
    u32 slot = 0;
  };

  static sim::EntityHandle materialize_hook(void* context, const sim::EntityRecord& record,
                                            u8 tier);
  static void dematerialize_hook(void* context, sim::EntityHandle entity);
  static bool has_component(void* context, const schema::TypeInfo& component);
  static sim::EntityHandle resolve(void* context, const Id128& id);
  static void collect_writeback(void* context, Vector<sim::WriteBackChange>& out);

  u32 resolve_mapping(const schema::MaterializeInfo& info);
  sim::EntityHandle materialize(const sim::EntityRecord& record);
  void dematerialize(sim::EntityHandle handle);
  void collect(Vector<sim::WriteBackChange>& out);
  void collect_every(Vector<sim::WriteBackChange>& out);
  void collect_changed(Vector<sim::WriteBackChange>& out);
  void make_query(u32 resolved);
  void snapshot(const Id128& id, u32 resolved, flecs::entity_t entity);
  void release_watch(const Id128& id);

  flecs::world* world_ = nullptr;
  RecordMaterializerConfig config_;
  Vector<Resolved> resolved_;
  HashMap<const schema::MaterializeInfo*, u32> by_mapping_;
  HashMap<Id128, Watch> watches_;
  Vector<ecs_id_t> ids_;              // scratch: the archetype of an entity being created
  Vector<flecs::entity_t> children_;  // scratch: children to unlink
  Vector<const u8*> columns_;         // scratch: a changed table's writable columns
  RecordStats stats_;
};

}  // namespace engine::ecs

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
};

class RecordMaterializer {
 public:
  // The world must outlive this object. Construct it outside a tick: it makes sure the identity
  // map and the component table exist, as `WorldCommands` does, for the same reason.
  explicit RecordMaterializer(flecs::world& world);
  ENGINE_NON_COPYABLE(RecordMaterializer);

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
  void snapshot(const Id128& id, u32 resolved, flecs::entity_t entity);
  void release_watch(const Id128& id);

  flecs::world* world_ = nullptr;
  Vector<Resolved> resolved_;
  HashMap<const schema::MaterializeInfo*, u32> by_mapping_;
  HashMap<Id128, Watch> watches_;
  Vector<ecs_id_t> ids_;              // scratch: the archetype of an entity being created
  Vector<flecs::entity_t> children_;  // scratch: children to unlink
  RecordStats stats_;
};

}  // namespace engine::ecs

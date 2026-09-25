#pragma once

// The materialization driver (docs/plan/03-data-model.md §3.4, docs/plan/05-simulation.md §5.4 and
// §5.5, ADR-0028): a document's records through the scheduler's materialization hooks, and the
// world's changes back.
//
// **What it is for.** The runtime world is a materialization of the document at a simulation LOD
// (03 §3.4). The hooks table in `scheduler.h` is the contract a capability implements; this is the
// caller of it that reads a document. It walks the live records through `doc::Document`'s composed
// index (never the layers), finds each record type's row in the mapping table schemac compiled
// (`schema::MaterializeRegistry`), and hands `SimScheduler::materialize` one `EntityRecord` per
// record whose `source` carries the mapping and the record's property values. The entity store's
// hook (`domain/ecs/materialize.h`) is the one that creates the entity and writes the fields; this
// module stays free of flecs, as ADR-0028 decision 7 needs it to.
//
// **The order rule.** Within one call, parents before children, and otherwise by id: records are
// materialized in (depth, id) order, where depth is the number of ancestors in the composed parent
// chain. Never by hash-table iteration and never by layer order, so two runs over the same document
// create the same entities in the same order, and a hook that creates something per record (a pool
// slot, a relationship) sees the same sequence on every machine. Dematerialization is the mirror:
// deepest first, then by id.
//
// **Incremental after the first call.** The driver keeps, per record it materialized, the document
// revision it reflects. A later call asks the document's change feed what changed since
// (`doc::Document::changed_since`), so an unchanged record costs nothing — it is not visited, not
// compared, and no hook is called for it. A changed record is materialized again (the hooks find
// the entity by its `Id128` and update it); a deleted one, or one that stopped being mapped, is
// dematerialized. When the feed cannot answer (the document rebuilt its index, or the reader fell
// further behind than the feed's bound) the call is a full pass that compares revisions instead.
//
// **Identity.** A record and its entity are one identity across saves: the entity is keyed by the
// record's `Id128` (ADR-0028 seam 3), and this module stores nothing else — no runtime handle
// outlives the call that produced it; `MaterializeTarget::resolve` turns an id back into one when a
// hook needs it.
//
// **Write-back.** The reverse path, for the rows the mapping marks `@writeback`: at
// `TickPhase::Persist`, every `writeback_every` ticks, the target reports which writable fields
// changed since they were last materialized or written, and they become one transaction of
// `SetProperty` commands attributed to `system`, handed to a sink that commits them — the host
// commits them to its session (journal, save) and logs them in the store. Nothing else writes the
// document from the world.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/json/json_value.h>
#include <core/schema/materialize.h>
#include <core/time/time.h>
#include <domain/doc/document.h>
#include <domain/doc/partition.h>
#include <domain/sim/scheduler.h>

#include <span>
#include <string>

namespace engine::sim {

// What a record came from when the authoring document is its source: `EntityRecord::source` for a
// record this driver hands the hooks. A record from the store (`reconcile_tile`) has none.
struct RecordSource {
  const schema::MaterializeInfo* mapping = nullptr;
  // One value per `mapping->fields` row, in the table's order: the record's composed property, or
  // the record type's schema default where no layer sets it. Never null. Valid for the call.
  std::span<const JsonValue* const> values;
  // The composed parent; the null id for a root. What the hook makes of it is the mapping's
  // `parent` (a `ChildOf` relationship, or nothing).
  Id128 parent;
  // The document revision this materialization reflects.
  u64 revision = 0;
};

// Which records one call covers. A layer with a partition files each record under one tile
// (domain/doc/partition.h); `Tile` is the records of one tile of the partitioned layers that define
// them, `Untiled` everything else — records of unpartitioned layers and records with no position —
// and `Whole` both.
struct MaterializeScope {
  enum class Kind : u8 { Whole, Tile, Untiled };
  Kind kind = Kind::Whole;
  doc::TileCoord tile;

  static MaterializeScope whole() noexcept { return {}; }
  static MaterializeScope of_tile(doc::TileCoord t) noexcept { return {Kind::Tile, t}; }
  static MaterializeScope untiled() noexcept { return {Kind::Untiled, {}}; }
};

// Why a live record in scope has no entity.
enum class SkipReason : u8 {
  NoMapping,         // its type has no `materialize` declaration: document-only by design
  UnknownType,       // its type is not in this build's schema registry
  Tier,              // the mapping does not materialize at the tier the call asked for
  MissingComponent,  // a component the mapping names is not registered in this world
  NoEntityStore,     // the world has no target that makes entities (a build without the ECS)
  NoHook,            // every hook ran and none gave it a runtime existence
};
const char* skip_reason_name(SkipReason reason) noexcept;

// One changed writable field: the record, its mapping row, and the property's new value in the
// record's own units. What a target reports and a write-back commits.
struct WriteBackChange {
  Id128 record;
  const schema::MaterializeInfo* mapping = nullptr;
  u32 row = 0;  // index into mapping->fields
  JsonValue value;
};

// What the world the driver materializes into can take and give back, as function pointers so
// this module does not depend on the entity store (ADR-0028 decision 7). `domain/ecs` provides one
// (`ecs::RecordMaterializer::target()`); a test provides a fake.
struct MaterializeTarget {
  void* context = nullptr;
  const char* name = nullptr;  // "ecs"; null means the world has no entity store at all
  // Whether the world has this component registered. A mapping naming one it lacks is skipped
  // whole, with the component's name in the report, rather than half made.
  bool (*has_component)(void* context, const schema::TypeInfo& component) = nullptr;
  // The live runtime handle for a persistent id, valid for the call; null when nothing holds it.
  EntityHandle (*resolve)(void* context, const Id128& id) = nullptr;
  // Appends every writable field whose value changed since it was last materialized or collected,
  // sorted by (record id, row), and takes those values as the new baseline.
  void (*collect_writeback)(void* context, Vector<WriteBackChange>& out) = nullptr;
};

// One write-back flush: the transaction the document receives, and the per-record changes behind
// it for whoever logs them.
struct WriteBackBatch {
  SimTick tick;
  GameTime time;
  doc::Attribution attribution;
  Vector<doc::Command> commands;    // one SetProperty per change, in (record id, row) order
  Vector<WriteBackChange> changes;  // the same order
};

// Where a flush goes. The host commits it to the session — journal, save, and an event in the
// store's log when the document has one — and a test applies it to a `doc::Document`. Returns
// false when the document refused it; the batch is then counted as refused and not retried.
struct WriteBackSink {
  void* context = nullptr;
  bool (*commit)(void* context, const WriteBackBatch& batch) = nullptr;
};

struct MaterializeConfig {
  // The tier records are materialized at: LOD2 by default, as [05
  // §5.5](../../../docs/plan/05-simulation.md#55-reconciliation-when-a-tile-activates) step 4 has
  // it, promoted from there by observer distance. A host with no observer (a headless run) asks for
  // LOD0.
  u8 tier = 2;
  // How often, in ticks, the write-back system commits what changed; 0 commits only on an explicit
  // `flush_writeback()`.
  u32 writeback_every = 1;
  // How many skipped records a report lists by id; the counts are always complete.
  u32 max_listed_skips = 64;
};

// One record type a call met.
struct MaterializedType {
  std::string type;
  bool mapped = false;
  u32 records = 0;       // live records of the type the call looked at
  u32 materialized = 0;  // of those, the ones that have an entity after the call
  u32 skipped = 0;
  SkipReason reason = SkipReason::NoMapping;  // the first reason a record of the type was skipped
  std::string detail;                         // e.g. the component the world does not have
};

struct SkippedRecord {
  Id128 id;
  std::string type;
  SkipReason reason = SkipReason::NoMapping;
};

// What one call did.
struct MaterializeReport {
  bool full = false;  // compared every live record in scope, rather than following the feed
  u64 revision = 0;   // the document revision the world now reflects
  u32 visited = 0;    // live records in scope the call looked at
  u32 created = 0;
  u32 updated = 0;
  u32 unchanged = 0;  // looked at by a full pass and left alone: same revision as held
  u32 dematerialized = 0;
  u32 relinked = 0;  // held records materialized again because their parent now is
  u32 skipped = 0;
  u32 live = 0;                    // entities the driver holds after the call
  u32 orphans = 0;                 // held records whose document parent has no entity
  Vector<MaterializedType> types;  // by type name
  Vector<SkippedRecord> skips;     // the first `max_listed_skips`, in walk order
  f64 ms = 0;
};

// A held record whose document tile is no longer the tile it is filed under: the world moved it
// and a write-back told the document (`Materializer::take_moved`).
struct TileMove {
  Id128 id;
  doc::TileCoord from;  // the tile it is filed under
  doc::TileCoord to;    // the tile the document now puts it in
  bool from_tiled = false;
  bool to_tiled = false;
};

// Cumulative over the driver's life. `hook_calls` is the number that proves an unchanged record
// costs nothing: it moves only when a record is handed to the hooks.
struct MaterializeStats {
  u64 hook_calls = 0;
  u64 created = 0;
  u64 updated = 0;
  u64 dematerialized = 0;
  u64 full_passes = 0;
  u64 incremental_passes = 0;
  u64 writeback_flushes = 0;
  u64 writeback_fields = 0;
  u64 writeback_refused = 0;
};

class Materializer {
 public:
  explicit Materializer(SimScheduler& scheduler, const MaterializeConfig& config = {});
  ~Materializer();
  ENGINE_NON_COPYABLE(Materializer);

  void set_target(const MaterializeTarget& target) noexcept { target_ = target; }
  void set_writeback_sink(const WriteBackSink& sink) noexcept { sink_ = sink; }
  const MaterializeConfig& config() const noexcept { return config_; }
  void set_writeback_every(u32 ticks) noexcept { config_.writeback_every = ticks; }

  // Registers the write-back as a system in the scheduler's table, at `TickPhase::Persist` — the
  // declared phase (03 §3.5's persistence flush) — so it runs after every other phase of the tick
  // has written what it writes. Call once, before the first `step()`.
  void install_writeback();

  // Materializes `scope` of `document` into the world: creates what is new, updates what changed,
  // dematerializes what went. The first call, and any call the change feed cannot answer, is a full
  // pass; later whole-document calls follow the feed. The document must outlive the driver's use
  // of it (write-back reads it); it is the one it was last called with.
  MaterializeReport materialize(const doc::Document& document,
                                const MaterializeScope& scope = MaterializeScope::whole());

  // Dematerializes everything the driver holds, deepest first. Returns how many.
  u32 dematerialize_all();

  // Dematerializes what the driver holds **filed under `scope`** — the records a tile pass of that
  // tile materialized, deepest first — and leaves everything else as it is: the mirror of
  // `materialize(document, MaterializeScope::of_tile(t))`, which a tile that goes inactive needs
  // (docs/subsystems/world.md). A child held under another tile whose parent this drops becomes an
  // orphan, and is relinked when the parent comes back, as a deletion would leave it. The whole
  // scope is `dematerialize_all()`. Returns how many.
  u32 dematerialize(const MaterializeScope& scope);
  // The ids held under `scope`, sorted: what a tile's snapshot writes before the tile goes.
  void held(const MaterializeScope& scope, Vector<Id128>& out) const;

  // **Records the world moved between tiles** (docs/subsystems/sim.md, "Records that move"). A tile
  // pass files a record under the tile it is in when it is materialized; when a system then moves
  // it and a write-back commits the move, the document files it under another tile while this
  // driver still has it under the first. Which of the two is right is the world's to say — whether
  // the new tile is live is the tile ring's knowledge, not the driver's — so the driver only
  // reports it: every held record a write-back wrote since the last call whose document tile is not
  // the one it is filed under, sorted by id, and forgets them. The world then files each under its
  // new tile (`refile`) or lets it go (`dematerialize(ids)`), which is what keeps the runtime world
  // a function of the document and the live tiles rather than of the way it got there (03 §3.4) —
  // and so a world loaded from a save the same world as the run that never stopped.
  void take_moved(Vector<TileMove>& out);
  // Files a held record under the tile the document now puts it in (`tiled` false: in none).
  // False when the driver does not hold it.
  bool refile(const Id128& id, bool tiled, doc::TileCoord tile) noexcept;
  // Dematerializes these held records, deepest first; an id the driver does not hold is ignored.
  // Returns how many went.
  u32 dematerialize(std::span<const Id128> ids);

  // One write-back flush: collects the changed writable fields and commits them through the sink
  // as one transaction. The Persist system calls it at the cadence; a host calls it when a run ends
  // so the document says where the world stopped. Returns the fields written.
  u32 flush_writeback(SimTick tick, GameTime time);

  u32 live() const noexcept { return held_.size(); }
  bool holds(const Id128& id) const noexcept { return held_.contains(id); }
  // The document revision the world reflects.
  u64 revision() const noexcept { return revision_; }
  const MaterializeStats& stats() const noexcept { return stats_; }
  // The ids the last call handed the hooks, in the order it handed them: what the order rule is
  // tested against.
  std::span<const Id128> last_order() const noexcept { return {order_.data(), order_.size()}; }
  // FNV-1a over `last_order()`'s bytes: two runs over one document agree on it, and a test pins it.
  u64 last_order_hash() const noexcept;

 private:
  // What the driver keeps per record it materialized. No runtime handle: that would outlive the
  // call that produced it (ADR-0028 seam 3), so a hook that needs one is handed `resolve(id)`.
  struct Held {
    u64 revision = 0;  // the document revision the entity reflects
    u64 seen_pass = 0;
    const schema::MaterializeInfo* mapping = nullptr;
    Id128 parent;
    u32 depth = 0;
    doc::TileCoord tile;
    bool tiled = false;
    bool parent_linked = false;
  };
  enum class Verdict : u8 { OutOfScope, Skipped, Candidate };
  struct Candidate {
    Id128 id;
    u32 depth = 0;
    const schema::MaterializeInfo* mapping = nullptr;
    doc::TileCoord tile;
    bool tiled = false;
  };
  struct MappingState {
    const schema::MaterializeInfo* info = nullptr;
    Vector<JsonValue> defaults;  // the record type's default per row
    u64 checked_pass = 0;        // the pass `usable`/`missing` were computed in
    bool usable = false;
    std::string missing;
  };

  static void writeback_tick(SystemContext& context, Batch batch);

  MappingState& mapping_state(const schema::MaterializeInfo& info);
  bool check_mapping(MappingState& state, SkipReason& reason);
  // Classifies one live record: out of scope (nothing counted), skipped (counted with its reason),
  // or a candidate for the hooks.
  Verdict classify(const doc::Document& document, const Id128& id, const MaterializeScope& scope,
                   MaterializeReport& report, Candidate& out);
  u32 depth_of(const doc::Document& document, const Id128& id);
  void count_type(MaterializeReport& report, std::string_view type, bool mapped, bool materialized,
                  const SkipReason* reason, std::string_view detail);
  void skip(MaterializeReport& report, const Id128& id, std::string_view type, bool mapped,
            SkipReason reason, std::string_view detail);
  // Hands one record to the hooks. False when no hook gave it a runtime existence. A relink is
  // counted on its own, not as a creation or an update.
  bool materialize_one(const doc::Document& document, const Candidate& candidate,
                       MaterializeReport& report, bool relink);
  void dematerialize_ids(Vector<Id128>& ids, MaterializeReport& report);
  void relink_orphans(const doc::Document& document, MaterializeReport& report);
  bool in_scope(const MaterializeScope& scope, bool tiled, doc::TileCoord tile) const noexcept;

  SimScheduler* scheduler_ = nullptr;
  MaterializeConfig config_;
  MaterializeTarget target_;
  WriteBackSink sink_;
  const doc::Document* document_ = nullptr;
  u64 revision_ = 0;
  bool synced_ =
      false;  // a whole-document pass has run, so the feed can be followed from revision_
  bool any_partitioned_ = false;  // per call: whether a record's tile has to be read at all
  u64 pass_ = 0;

  HashMap<Id128, Held> held_;
  Vector<MappingState> mappings_;
  HashMap<const schema::MaterializeInfo*, u32> mapping_index_;
  HashMap<Id128, u32> depth_cache_;  // per call
  Vector<Id128> order_;
  Vector<Candidate> candidates_;
  Vector<const JsonValue*> values_;
  Vector<Id128> scratch_ids_;
  Vector<Id128> chain_;
  Vector<Id128> gone_;
  Vector<EntityHandle> handles_;
  Vector<Id128> orphans_;
  Vector<WriteBackChange> changes_;
  // Held records a write-back wrote since the last `take_moved`: bounded by what is held.
  HashMap<Id128, u8> written_;
  MaterializeStats stats_;
};

// The driver as the free function the plan's sentence names: `sim::materialize(document, world,
// scope)`, where the world is the `Materializer` that holds what was materialized before.
MaterializeReport materialize(const doc::Document& document, Materializer& world,
                              const MaterializeScope& scope = MaterializeScope::whole());

}  // namespace engine::sim

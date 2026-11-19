#pragma once

// The state hash a tick, by phase and by component table (docs/plan/09-testing-profiling.md §9.2;
// docs/subsystems/ecs.md, "The state hash a tick"; roadmap T53). The log it fills, the recorded run
// and the bisector are `domain/sim/hash_log.h`'s, which stay free of flecs.
//
// **Off unless asked, and nothing when off.** A host that does not construct a `TickHash` has no
// system, no query and no byte of it: the hash is not a mode of the tick, it is an object a replay,
// a test or a developer chasing a divergence makes. Constructed, it adds one system to the
// scheduler's table per phase it hashes, each an `end_tick` hook and nothing else — no component,
// no resource, every tier — so the hook runs after the phase's flecs systems and the table's waves,
// and after every end-tick hook registered before it: **construct it after everything else is
// installed** and it sees each phase's finished state.
//
// **A table is a component type**: every schema-declared component the world registered with a
// size (tags carry nothing), `@transient` ones too unless the config leaves them out — determinism
// is about what the tick computes, not only about what a save keeps — in qualified-name order, so
// two hosts that registered the same components in different orders write the same header. A
// component with no schema type (hand-registered, private) has no fields to read and is counted in
// `stats().unhashed`; a source the host adds (`add_source`: the event bus's delivery hash, the
// timing wheel's due list) is a table too, after the components. The table set is fixed at the
// first hashed phase; a component registered later is counted (`stats().late`) and not hashed.
//
// **What a table's hash is.** Each entity's component is hashed field by field through schema
// reflection — scalars by value, strings and byte arrays by contents, containers element by
// element, a field marked `@transient` skipped, never a padding byte — seeded with the entity's
// `Id128` (or its flecs id when it has none), and the table's hash is the *sum* of those, with the
// count and the table's name folded in. A sum because it does not depend on the order flecs keeps
// the table's rows in, so nothing is sorted and nothing is allocated a phase; a world whose rows
// moved between archetypes but whose values did not hashes the same. It is a multiset hash: two
// worlds that differ in one entity's one field differ in the table's hash but for a 2^-64 chance.
// A table whose fields are all fixed groups of scalars is hashed through a flat program computed
// once (runs of offset, count and load) to the same words, and a config with `jobs` hashes a
// phase's tables in runs on the job system to the same sums (ecs.md, "The tick hash's fast path").
//
// **The capture**: `capture_at(tick, phase, table, out)` asks for one table's entities as canonical
// JSON (`schema::to_json`, which leaves out the same transient fields the hash skips) at a (tick,
// phase), sorted by id: the second half of the bisector, taken by running each side again to the
// (tick, phase) `sim::first_divergence` named, and diffed with `sim::diff_captures`.
//
// **What it holds** (`memory()`): per table, its name and one cached flecs query; per entity,
// nothing; per tick, one `sim::HashLogRow` and one u64 per table for each phase hashed, in the log
// — which `TickHashConfig::reserve_ticks` sizes up front, so a run of known length allocates
// nothing once it ticks.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/ecs/sim_world.h>
#include <domain/sim/hash_log.h>
#include <domain/sim/scheduler.h>

#include <flecs.h>
#include <string>
#include <string_view>

namespace engine::schema {
struct TypeInfo;
}
namespace engine::jobs {
class JobSystem;
}

namespace engine::ecs {

struct TickHashConfig {
  // Bit p: hash at the end of `TickPhase` p. All eight by default.
  u8 phases = 0xFFu;
  // Hash `@transient` components too.
  bool transient = true;
  // Ticks of log rows reserved at the first hashed phase.
  u64 reserve_ticks = 0;
  // **Hash a phase's tables on this job system's performance pool** (ecs.md, "The tick hash's fast
  // path"): every table's entities cut into runs of at most `grain`, a run a job, each run's sum
  // added into its table's afterwards. The values are the serial ones bit for bit: a table's hash
  // is a sum over its entities, which no split or order changes, and each table's goes to its own
  // column of the row. Null: one thread, the caller's.
  jobs::JobSystem* jobs = nullptr;
  u32 grain = 4096;
  // Hash every table field by field through reflection, never through a flat program: the path a
  // table with a string, a container or an optional takes anyway. For checking the fast path.
  bool reflected = false;
};

// A table that is not a component: `hash` is called at the end of every phase hashed.
struct TickHashSource {
  const char* name = nullptr;
  void* context = nullptr;
  u64 (*hash)(void* context, sim::TickPhase phase) = nullptr;
};

struct TickHashStats {
  u64 rows = 0;         // (tick, phase) rows appended to the log
  u64 entities = 0;     // entity components hashed, summed over rows
  u32 unhashed = 0;     // components with no schema type, not hashed
  u32 late = 0;         // components registered after the table set was fixed
  u32 captures = 0;     // captures delivered
  u32 refused = 0;      // rows the log refused (a tick stepped twice, a log reset under it)
  u32 fast_tables = 0;  // component tables hashed through a flat program (ecs.md)
};

// The two ways a component's value is hashed, for the case that holds them equal on every
// registered type: `reflected` field by field through schema reflection, and, where `info` has a
// flat program — every field not marked `@transient` a scalar, an enum, a vector, a quaternion, a
// world position, an id, or a struct of those, at any depth — `fast` through it. False where it
// has none (a string, bytes, JSON, an optional, an array or a map anywhere in it); `fast` is then
// untouched.
bool tick_hash_both_ways(const schema::TypeInfo& info, const void* value, u64 seed, u64& reflected,
                         u64& fast);

namespace detail {
// One run of a table's flat program: `count` scalars of one load laid end to end from `offset`
// in the component (ecs.md, "The tick hash's fast path").
struct TickHashOp {
  u32 offset = 0;
  u16 count = 0;
  u8 load = 0;
};
}  // namespace detail

struct TickHashMemory {
  u32 tables = 0;        // components and sources
  usize per_table = 0;   // bytes kept per table: its name and its query's handle
  usize per_entity = 0;  // bytes kept per entity: none
  usize per_tick = 0;    // log bytes a tick adds: a row and a hash per table per phase hashed
  usize log = 0;         // what the log holds now
};

class TickHash {
 public:
  // Adds its systems to `scheduler`, which must outlive this and must not step after it is gone
  // (the scheduler has no way to remove a system: like every installer, this lives as long as the
  // world ticks). `log` receives the header at the first hashed phase and a row a phase after it.
  TickHash(SimWorld& world, sim::SimScheduler& scheduler, sim::HashLog& log,
           const TickHashConfig& config = {});
  ~TickHash();
  ENGINE_NON_COPYABLE(TickHash);

  // Before the first tick. False once the table set is fixed or for a source with no name or hash.
  bool add_source(const TickHashSource& source);

  // Fills `*out` with `table`'s entities at the end of `phase` of tick `tick`, when the run gets
  // there. False when the table set is fixed and has no such table, or the hash does not run at
  // `phase`. `out` must outlive the request.
  bool capture_at(u64 tick, sim::TickPhase phase, std::string_view table, sim::Capture* out);

  // Every table's hash now, in the log's order, without appending a row: for a host that checks one
  // point. Fixes the table set if it was not.
  void hash_now(sim::TickPhase phase, Vector<u64>& out);

  const TickHashStats& stats() const noexcept { return stats_; }
  TickHashMemory memory() const noexcept;
  // The tables, once fixed: components by qualified name, then sources.
  std::span<const std::string> tables() const noexcept { return {names_.data(), names_.size()}; }

 private:
  using HashOp = detail::TickHashOp;
  struct Table {
    const schema::TypeInfo* info = nullptr;
    flecs::entity_t component = 0;
    u32 size = 0;
    ecs_query_t* query = nullptr;
    u64 name_hash = 0;
    u32 op_first = 0;  // its flat program in `ops_`, when `fast`
    u32 op_count = 0;
    bool fast = false;
  };
  // A run of one table's entities, gathered on the calling thread and hashed on a worker.
  struct Run {
    u32 table = 0;
    u32 count = 0;
    const u8* values = nullptr;  // the first entity's component, or the one shared value
    const void* ids = nullptr;   // `Identity`s, or the one shared, or null
    const flecs::entity_t* entities = nullptr;
    bool values_self = true;
    bool ids_self = true;
    u64 sum = 0;
  };
  struct Request {
    u64 tick = 0;
    sim::TickPhase phase = sim::TickPhase::Input;
    u32 table = 0;
    std::string name;  // until the table set is fixed
    sim::Capture* out = nullptr;
  };

  static void end_phase(sim::SystemContext& context);
  void freeze();
  void hash_into(sim::TickPhase phase, u64* out);
  u64 hash_table(const Table& table);
  // Fills `runs_` with every table's entities, at most `grain` a run.
  void gather_runs(u32 grain);
  u64 hash_run(const Run& run) const noexcept;
  void capture(const Table& table, sim::Capture& out);

  SimWorld* world_ = nullptr;
  sim::SimScheduler* scheduler_ = nullptr;
  sim::HashLog* log_ = nullptr;
  TickHashConfig config_;
  bool frozen_ = false;
  u32 components_seen_ = 0;
  flecs::entity_t identity_ = 0;
  Vector<Table> tables_;
  Vector<HashOp> ops_;
  Vector<Run> runs_;
  Vector<u64> run_counts_;
  Vector<TickHashSource> sources_;
  Vector<std::string> names_;
  Vector<u64> row_;
  Vector<Request> requests_;
  TickHashStats stats_;
};

}  // namespace engine::ecs

#pragma once

// Mutation from outside a system is guarded (ADR-0028 seam 4).
//
// A system body gets the raw `flecs::world&` and writes it directly: that is the whole point of
// not wrapping flecs, and a system runs at a known point in a known phase with a known set of
// declared accesses. Everything *else* that wants to change the world — the protocol answering
// `world.apply`, the persistence layer materializing a tile, a save being loaded, a network
// packet being applied, an agent's edit — has none of those properties. It arrives on another
// thread or between ticks, it names entities by `Id128` because that is what survived the trip,
// and it must not land in the middle of a phase.
//
// `WorldCommands` is the queue those callers use. What it buys:
//
//   - **A phase boundary.** Commands are applied together, at a point the tick chose, so no
//     system ever sees half of an external edit.
//   - **`Id128`, not entity handles** (seam 3). The caller never holds a flecs id.
//   - **Schema types, by name** (seam 1). A component is named as the IDL names it and its value
//     arrives as JSON or as bytes; the reflection the schema already generated is what reads it,
//     so the protocol needs no per-component code and a type the schema does not declare cannot
//     be set at all.
//   - **Failure at the point of the mistake.** A payload is parsed when it is queued, not when it
//     is applied, so a caller learns its JSON was wrong while it still has somewhere to report
//     it. `apply()` cannot fail on a parse.
//
// **What it is not.** It is not a transaction: `apply()` walks the queue in order and counts what
// it could not do, and a failed command does not roll back the ones before it. The event log is
// what makes an edit undoable (ADR-0003), not this. It is also not thread-safe: queue from one
// thread, apply from the thread that owns the world. Both are the honest v1, and both are stated
// rather than discovered.
//
// **Where the protocol attaches.** `world.query`, `world.apply` and the rest of [06
// §6.2](../../../docs/plan/06-agent-tooling.md)'s method table are not wired up here. When they
// are, `apply`'s parameters map onto `create`/`set_json`/`remove`/`destroy` one for one, the
// dispatcher owns one `WorldCommands` per session, and `install()` is what makes the session's
// edits land on a tick boundary. Nothing in this header has to change for that.

#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/json/json_value.h>
#include <core/schema/json_reflect.h>
#include <core/schema/type_info.h>
#include <domain/ecs/sim_world.h>

#include <flecs.h>
#include <span>
#include <string_view>

namespace engine::ecs {

enum class CommandKind : u8 {
  Create,           // create the entity this Id128 names, if it does not exist
  Destroy,          // destroy it, with everything it owns
  SetComponent,     // add or overwrite one component
  RemoveComponent,  // take one component away
};

// What one `apply()` did. `failed` counts commands whose entity was gone or whose component the
// world no longer had; it is a number the protocol reports back, not an error to abort on.
struct CommandStats {
  u32 created = 0;
  u32 destroyed = 0;
  u32 set = 0;
  u32 removed = 0;
  u32 failed = 0;

  u32 total() const noexcept { return created + destroyed + set + removed + failed; }
  bool operator==(const CommandStats&) const noexcept = default;
};

class WorldCommands {
 public:
  // Constructed outside a tick: it makes sure the component table and the identity map exist,
  // and a world flecs has made readonly for the pipeline cannot have either created in it.
  explicit WorldCommands(flecs::world& world);
  ~WorldCommands();
  ENGINE_NON_COPYABLE(WorldCommands);

  // --- queueing ------------------------------------------------------------------------------

  // Creates the entity `id` names. Applying a create for an id that already exists is a no-op,
  // not a failure: a caller replaying an event log must be able to do it twice.
  void create(const Id128& id);
  void destroy(const Id128& id);

  // Sets a component from JSON, through the reflection schemac generated for it. The value is
  // parsed *now*: false means the type is unknown to this world or the JSON did not fit it, and
  // `diagnostics` (when given) says where, with the field path `core/schema` produces.
  bool set_json(const Id128& id, std::string_view type_name, const JsonValue& value,
                Vector<schema::Diagnostic>* diagnostics = nullptr);

  // Sets a component from its raw bytes. Only legal for a `ComponentType::flat` component —
  // one with no string, bytes, array, map, optional or json field anywhere — because for
  // anything else the bytes are pointers into another process's heap. The size must match.
  bool set_bytes(const Id128& id, std::string_view type_name, std::span<const u8> bytes);

  // Removes a component. Removing one the entity does not have is a no-op.
  bool remove(const Id128& id, std::string_view type_name);

  u32 pending() const noexcept { return static_cast<u32>(commands_.size()); }
  void clear() noexcept;

  // --- application ---------------------------------------------------------------------------

  // Applies everything queued, in the order it was queued, and empties the queue. Call it
  // between phases, or let `install()` do it.
  CommandStats apply();

  // Runs `apply()` at the start of `phase`, every tick. The returned flecs system is the caller's
  // to disable or delete; the `WorldCommands` must outlive it.
  //
  // `EventsIn` is the default because that is what an external edit is: something that happened
  // since the last tick and is delivered at the top of this one, before any system reads the
  // world (plan 05 §5.2).
  flecs::system install(SimWorld& sim, TickPhase phase = TickPhase::EventsIn);

 private:
  struct Command {
    Id128 id;
    const schema::TypeInfo* info = nullptr;  // the component's type, for a set or a remove
    void* value = nullptr;                   // owned, constructed instance of *info, for a set only
    flecs::entity_t component = 0;
    CommandKind kind = CommandKind::Create;
  };

  // Finds the component and makes a constructed, owned instance of it. Null on an unknown type.
  void* allocate_value(std::string_view type_name, const schema::TypeInfo*& out_info,
                       flecs::entity_t& out_component, u32& out_size, bool& out_flat);
  void release(Command& command) noexcept;

  flecs::world* world_ = nullptr;
  Vector<Command> commands_;
};

}  // namespace engine::ecs

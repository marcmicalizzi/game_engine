#pragma once

// Persistent identity is `Id128` (ADR-0028 seam 3).
//
// A flecs entity id is a world-local handle with a generation counter. It is the right thing to
// hold inside a tick and the wrong thing to hold anywhere else: it does not survive a save, a
// reload, a tile unload and remateralization, a replay on another machine, or a second process
// looking at the same world. `Id128` is the engine's stable identity (docs/plan/03-data-model.md
// §3.1) and it is what the document, the store and the protocol already speak.
//
// So: **nothing outside `domain/ecs` stores a flecs entity id in anything that outlives a tick's
// working set.** Inside a tick, iterate entities; across a tick, across a save, across the wire,
// name an `Id128`. `IdentityMap` is the one place the two meet, and it is O(1) both ways.
//
// **Why there is one map and not two.** `Id128 -> flecs::entity` needs a hash map. The reverse
// does not: the entity carries an `Identity` component, so `id_of(entity)` is a component read,
// which is the same O(1) with none of the memory and no second thing to keep in agreement.
//
// **Why observers and not bookkeeping at the call site.** The map has to be right even when an
// entity is destroyed by something that never heard of it — a `delete_with`, a tile teardown, a
// system that removes `Identity` — so it is maintained by flecs' own `OnSet` and `OnRemove`
// observers on the component. A raw `world.entity().set<ecs::Identity>({id})` therefore joins the
// map, which is what makes this a seam rather than a convention that reviews have to enforce.

#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/ids/id128.h>
#include <domain/sim/scheduler.h>

#include <flecs.h>

namespace engine::ecs {

// The persistent name of an entity. Every entity a save file, the protocol or another tick can
// refer to carries one; a purely transient entity (a particle, a query result holder) need not.
struct Identity {
  Id128 id;
};

// `Id128` to flecs entity for one world. A flecs singleton, created with its observers on first
// use; hold the reference only for the call that obtained it.
class IdentityMap {
 public:
  // The live entity with this id, or 0. Never returns a stale handle: the `OnRemove` observer
  // takes the row out before flecs recycles the entity.
  flecs::entity_t find(const Id128& id) const noexcept;
  bool contains(const Id128& id) const noexcept { return find(id) != 0; }
  u32 size() const noexcept { return by_id_.size(); }

  // Called by the observers. Public because flecs' callbacks are free functions, not friends.
  void bind(const Id128& id, flecs::entity_t entity) noexcept;
  void unbind(const Id128& id, flecs::entity_t entity) noexcept;
  // How many `OnSet`s named an id another live entity already held. Non-zero means the world has
  // a duplicate identity, which is a content or protocol bug rather than an engine one, so it is
  // counted and logged rather than asserted on.
  u32 collisions() const noexcept { return collisions_; }

 private:
  HashMap<Id128, flecs::entity_t> by_id_;
  u32 collisions_ = 0;
};

// The world's identity map, installing `Identity`'s observers on first use.
IdentityMap& identity_map(flecs::world& world);
const IdentityMap* identity_map_if_present(const flecs::world& world) noexcept;

// The entity this id names, or a null entity.
flecs::entity entity_for(flecs::world& world, const Id128& id) noexcept;
// Creates an entity carrying `id`, or returns the one that already has it. `id` must not be null.
flecs::entity create_entity(flecs::world& world, const Id128& id);
// The entity's persistent name, or the null id when it has none.
Id128 id_of(flecs::entity entity) noexcept;

// ---- the bridge to `domain/sim`'s materialization contract -------------------------------------
//
// `sim::MaterializationHooks` names a live entity with a `sim::EntityHandle` — opaque, and valid
// only inside the call that carries it — and a persistent record with an `Id128`. These three
// functions are where a flecs entity id becomes one and stops being one, and they are the **only**
// place in the tree that is allowed to know that an `EntityHandle` holds a `flecs::entity_t`. That
// is what makes seam 3's rule checkable: everything outside `domain/ecs` handles the opaque type,
// so storing a runtime id past a tick is a thing nobody can write by accident.
//
// A capability implementing the hooks therefore reads `record.entity` as an `Id128` (which is what
// `entity_for` takes) and `promote`/`demote`/`dematerialize`'s handle through `entity_of`, and it
// keeps neither.
sim::EntityHandle handle_of(flecs::entity entity) noexcept;
// The live entity a handle names, or an entity of id 0 bound to this world when it names none —
// the same "not found" `entity_for` returns, and for the same reason.
flecs::entity entity_of(flecs::world& world, sim::EntityHandle handle) noexcept;
// The runtime handle for a persistent id, through the identity map. Null when nothing holds it.
sim::EntityHandle handle_for(flecs::world& world, const Id128& id) noexcept;

}  // namespace engine::ecs

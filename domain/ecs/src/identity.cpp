#include "undeferred.h"

#include <core/base/assert.h>
#include <core/log/log.h>
#include <domain/ecs/identity.h>

namespace engine::ecs {

ENGINE_LOG_CATEGORY_DECLARE(log_ecs);

namespace {

// Set once per world, so `identity_map()` can install the observers exactly once without asking
// flecs whether an observer already exists.
struct IdentityObserversInstalled {};

}  // namespace

flecs::entity_t IdentityMap::find(const Id128& id) const noexcept {
  const flecs::entity_t* entity = by_id_.find_value(id);
  return entity != nullptr ? *entity : 0;
}

void IdentityMap::bind(const Id128& id, flecs::entity_t entity) noexcept {
  if (id.is_null()) return;
  const flecs::entity_t* existing = by_id_.find_value(id);
  if (existing != nullptr && *existing != entity) {
    ++collisions_;
    ENGINE_LOG_WARN(log_ecs, "two entities claim one persistent id",
                    log::field("entity", static_cast<u64>(entity)),
                    log::field("previous", static_cast<u64>(*existing)));
  }
  by_id_.insert_or_assign(id, entity);
}

void IdentityMap::unbind(const Id128& id, flecs::entity_t entity) noexcept {
  if (id.is_null()) return;
  // Only the entity that currently owns the row may take it out. Without this an entity that
  // lost a duplicated id would evict the winner's row on its way out.
  const flecs::entity_t* existing = by_id_.find_value(id);
  if (existing != nullptr && *existing == entity) by_id_.erase(id);
}

IdentityMap& identity_map(flecs::world& world) {
  const detail::Undeferred undeferred(world.c_ptr());
  if (!world.has<IdentityObserversInstalled>()) {
    world.add<IdentityObserversInstalled>();
    // `Identity` is module-private in the sense seam 1 means — it is the engine's own bookkeeping
    // rather than a schema-declared gameplay component — but it is emphatically *not* transient:
    // it is the one component a save file cannot do without. So it is registered by hand, with a
    // name, and left out of the transient set.
    world.component<Identity>("engine::ecs::Identity");

    // The map exists before an observer can fire, so the observers never create anything: they
    // take the *real* world (flecs may run one on a stage while a deferred batch merges) and
    // read the singleton that is already there. An `ensure` in an observer would be a deferred
    // command writing into a temporary buffer, which is the failure this shape avoids.
    world.ensure<IdentityMap>();

    world.observer<const Identity>("engine::ecs::identity_bound")
        .event(flecs::OnSet)
        .each([](flecs::entity entity, const Identity& identity) {
          const flecs::world owner = entity.world().get_world();
          if (IdentityMap* map = owner.try_get_mut<IdentityMap>())
            map->bind(identity.id, entity.id());
        });
    world.observer<const Identity>("engine::ecs::identity_unbound")
        .event(flecs::OnRemove)
        .each([](flecs::entity entity, const Identity& identity) {
          const flecs::world owner = entity.world().get_world();
          if (IdentityMap* map = owner.try_get_mut<IdentityMap>())
            map->unbind(identity.id, entity.id());
        });
  }
  return world.ensure<IdentityMap>();
}

const IdentityMap* identity_map_if_present(const flecs::world& world) noexcept {
  return world.try_get<IdentityMap>();
}

// Not found is an entity of id 0 *bound to this world*, not `flecs::entity::null()`, whose world
// pointer is null: a caller that forgets to check `is_valid()` should get flecs' "entity 0 is not
// valid" and not a null-world assert three calls deeper.
flecs::entity entity_for(flecs::world& world, const Id128& id) noexcept {
  return flecs::entity(world.c_ptr(), identity_map(world).find(id));
}

flecs::entity create_entity(flecs::world& world, const Id128& id) {
  ENGINE_ASSERT(!id.is_null(), "ecs::create_entity: the null Id128 names nothing");
  const flecs::entity_t existing = identity_map(world).find(id);
  if (existing != 0) return flecs::entity(world.c_ptr(), existing);
  const flecs::entity entity = world.entity();
  entity.set<Identity>(Identity{id});
  return entity;
}

Id128 id_of(flecs::entity entity) noexcept {
  const Identity* identity = entity.try_get<Identity>();
  return identity != nullptr ? identity->id : Id128{};
}

}  // namespace engine::ecs

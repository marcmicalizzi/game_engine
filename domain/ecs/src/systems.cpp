#include "undeferred.h"

#include <core/base/assert.h>
#include <core/log/log.h>
#include <domain/ecs/systems.h>

namespace engine::ecs {

ENGINE_LOG_CATEGORY_DECLARE(log_ecs);

// --- SystemRegistry --------------------------------------------------------------------------

u16 SystemRegistry::add(const sim::SystemDesc& desc, flecs::entity_t system) {
  RegisteredSystem row;
  row.desc = desc;
  row.system = system;
  row.index = static_cast<u16>(systems_.size());
  systems_.push_back(row);
  return row.index;
}

const RegisteredSystem* SystemRegistry::find(flecs::entity_t system) const noexcept {
  for (const RegisteredSystem& row : systems_) {
    if (row.system == system) return &row;
  }
  return nullptr;
}

u16 SystemRegistry::count_in(TickPhase phase) const noexcept {
  u16 count = 0;
  for (const RegisteredSystem& row : systems_) {
    if (row.desc.phase == phase) ++count;
  }
  return count;
}

SystemRegistry& systems(flecs::world& world) {
  const detail::Undeferred undeferred(world.c_ptr());
  return world.ensure<SystemRegistry>();
}

const SystemRegistry* systems_if_present(const flecs::world& world) noexcept {
  return world.try_get<SystemRegistry>();
}

// --- the declared sets against the query ------------------------------------------------------

QueryAccess query_access(flecs::world& world, flecs::entity_t system) noexcept {
  QueryAccess access;
  const ecs_system_t* desc = ecs_system_get(world.c_ptr(), system);
  if (desc == nullptr || desc->query == nullptr) return access;
  const ecs_query_t* query = desc->query;

  const ComponentRegistry& registry = components(world);
  for (i32 i = 0; i < static_cast<i32>(query->term_count); ++i) {
    const ecs_term_t& term = query->terms[static_cast<usize>(i)];
    // A term that provides no data is a filter, not an access: a tag, a `not`, a wildcard flecs
    // refused to type. Nothing is read through it and nothing is written.
    if (term.inout == EcsInOutNone || term.inout == EcsInOutFilter) continue;
    if (term.oper == EcsNot) continue;
    if (ecs_id_is_pair(term.id)) {
      ++access.pair_terms;
      continue;
    }
    const u32 index = registry.index_of(term.id);
    if (index == k_invalid_component_index) {
      ++access.unregistered_terms;
      continue;
    }
    switch (term.inout) {
      case EcsIn: access.reads.set(index); break;
      case EcsOut: access.writes.set(index); break;
      // `InOut` is explicit read-write, and `InOutDefault` is what a non-const term of a typed
      // builder gets: `world.system<Position, const Velocity>()` makes Position default and
      // Velocity `In`. Default therefore means the system may write, and the conservative
      // reading is the correct one — a declaration that omits the write is the bug this check
      // exists to find.
      default:
        access.reads.set(index);
        access.writes.set(index);
        break;
    }
  }
  return access;
}

namespace detail {

namespace {

// Names one offending component for the message. The assert fires on the first mismatch, so one
// name is enough to start from and the log line carries the counts.
const char* first_undeclared(const ComponentRegistry& registry, const sim::ComponentMask& used,
                             const sim::ComponentMask& declared) noexcept {
  for (const ComponentType& type : registry.all()) {
    if (type.index == k_invalid_component_index) continue;
    if (!used.test(type.index) || declared.test(type.index)) continue;
    return type.info != nullptr ? type.info->qualified_name : "(private component)";
  }
  return "(unknown)";
}

}  // namespace

u16 finish_system(flecs::world& world, const sim::SystemDesc& desc, flecs::entity_t system) {
#if ENGINE_DEBUG
  const QueryAccess access = query_access(world, system);
  // A write term has to be declared as a write. A read term has to be declared at all — a system
  // that declares a component only as a write and reads it too is fine, because the schedule
  // treats a writer as conflicting with everything.
  sim::ComponentMask declared_any = desc.reads;
  for (u32 word = 0; word < sim::k_component_mask_words; ++word)
    declared_any.words[word] |= desc.writes.words[word];

  bool undeclared_read = false;
  bool undeclared_write = false;
  for (u32 word = 0; word < sim::k_component_mask_words; ++word) {
    undeclared_read |= (access.reads.words[word] & ~declared_any.words[word]) != 0;
    undeclared_write |= (access.writes.words[word] & ~desc.writes.words[word]) != 0;
  }

  if (undeclared_read || undeclared_write) {
    const ComponentRegistry& registry = components(world);
    const char* offender = undeclared_write
                               ? first_undeclared(registry, access.writes, desc.writes)
                               : first_undeclared(registry, access.reads, declared_any);
    ENGINE_LOG_WARN(log_ecs, "system touches a component its SystemDesc does not declare",
                    log::field("system", desc.name != nullptr ? desc.name : "(unnamed)"),
                    log::field("component", offender),
                    log::field("access", undeclared_write ? "write" : "read"));
    ENGINE_ASSERT(!undeclared_write,
                  "ecs::register_system: the system's query writes a component SystemDesc::writes "
                  "does not name");
    ENGINE_ASSERT(!undeclared_read,
                  "ecs::register_system: the system's query reads a component SystemDesc::reads "
                  "does not name");
  }
#endif
  return systems(world).add(desc, system);
}

}  // namespace detail

}  // namespace engine::ecs

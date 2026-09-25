#include <core/base/assert.h>
#include <core/log/log.h>
#include <domain/ecs/components.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/materialize.h>

#include <algorithm>
#include <cstring>

namespace engine::ecs {

ENGINE_LOG_CATEGORY_DECLARE(log_ecs);

RecordMaterializer::RecordMaterializer(flecs::world& world) : world_(&world) {
  // Both singletons exist before the first record arrives, for the reason `WorldCommands` forces
  // them: a world is writable here, and nothing later should have to create them.
  components(world);
  identity_map(world);
}

sim::MaterializationHooks RecordMaterializer::hooks() noexcept {
  sim::MaterializationHooks row;
  row.name = "ecs";
  row.context = this;
  row.tiers = 0x0Fu;  // an entity exists at every tier; what a tier costs is each capability's
  row.materialize = &RecordMaterializer::materialize_hook;
  row.dematerialize = &RecordMaterializer::dematerialize_hook;
  return row;
}

sim::MaterializeTarget RecordMaterializer::target() noexcept {
  sim::MaterializeTarget target;
  target.context = this;
  target.name = "ecs";
  target.has_component = &RecordMaterializer::has_component;
  target.resolve = &RecordMaterializer::resolve;
  target.collect_writeback = &RecordMaterializer::collect_writeback;
  return target;
}

// ---- trampolines --------------------------------------------------------------------------------

sim::EntityHandle RecordMaterializer::materialize_hook(void* context,
                                                       const sim::EntityRecord& record, u8) {
  return static_cast<RecordMaterializer*>(context)->materialize(record);
}

void RecordMaterializer::dematerialize_hook(void* context, sim::EntityHandle entity) {
  static_cast<RecordMaterializer*>(context)->dematerialize(entity);
}

bool RecordMaterializer::has_component(void* context, const schema::TypeInfo& component) {
  auto* self = static_cast<RecordMaterializer*>(context);
  const ComponentRegistry* registry = components_if_present(*self->world_);
  return registry != nullptr && registry->find(component.qualified_name) != nullptr;
}

sim::EntityHandle RecordMaterializer::resolve(void* context, const Id128& id) {
  return handle_for(*static_cast<RecordMaterializer*>(context)->world_, id);
}

void RecordMaterializer::collect_writeback(void* context, Vector<sim::WriteBackChange>& out) {
  static_cast<RecordMaterializer*>(context)->collect(out);
}

// ---- one mapping, resolved against this world ---------------------------------------------------

u32 RecordMaterializer::resolve_mapping(const schema::MaterializeInfo& info) {
  u32 index = 0;
  if (const u32* found = by_mapping_.find_value(&info)) {
    index = *found;
    // A mapping this world could not take when it was first seen is retried: a capability may have
    // registered its components since.
    if (resolved_[index].usable) return index;
  } else {
    index = resolved_.size();
    resolved_.push_back(Resolved{});
    by_mapping_.insert(&info, index);
  }

  Resolved r;
  r.info = &info;
  r.usable = true;
  const ComponentRegistry& registry = components(*world_);
  for (const schema::TypeInfo* component : info.components) {
    const ComponentType* type = registry.find(component->qualified_name);
    if (type == nullptr) {
      r.usable = false;
      break;
    }
    Group group;
    group.component = type->id;
    group.size = type->size;
    r.groups.push_back(group);
  }
  for (u32 i = 0; r.usable && i < info.fields.size(); ++i) {
    const schema::MaterializeField& row = info.fields[i];
    u32 group = 0;
    while (group < info.components.size() && info.components[group] != row.component)
      ++group;
    const schema::FieldInfo* field = row.component->find_field(row.field);
    if (group == info.components.size() || field == nullptr) {
      r.usable = false;
      break;
    }
    // schemac emits a mapping's rows grouped by component, so a group's rows are one run.
    if (r.groups[group].rows == 0) r.groups[group].first_row = i;
    ++r.groups[group].rows;
    r.fields.push_back(field);
    r.row_group.push_back(group);
    if ((row.flags & schema::MaterializeFlag::writeback) != 0) {
      r.writable.push_back(i);
      r.shadow_bytes += static_cast<u32>(field->type.size);
    }
  }
  resolved_[index] = std::move(r);
  return index;
}

// ---- the hooks ----------------------------------------------------------------------------------

sim::EntityHandle RecordMaterializer::materialize(const sim::EntityRecord& record) {
  flecs::world& w = *world_;
  ecs_world_t* raw = w.c_ptr();
  // Between ticks only: inside one the world is deferred or readonly, a created entity would not
  // be in the identity map until the merge, and a later hook of the same record would not find it.
  if (ecs_is_deferred(raw) || ecs_stage_is_readonly(raw)) {
    ENGINE_LOG_WARN(log_ecs, "materialization inside a tick refused; materialize between ticks");
    return {};
  }
  // A record from the store carries no source yet (its projections are opaque bytes): resolve the
  // entity it names if there is one, and create none.
  if (record.source == nullptr) return handle_for(w, record.entity);
  const sim::RecordSource& source = *record.source;
  const u32 index = resolve_mapping(*source.mapping);
  const Resolved& r = resolved_[index];
  if (!r.usable) return {};

  const IdentityMap* map = identity_map_if_present(w);
  ENGINE_ASSERT(map != nullptr, "ecs::RecordMaterializer: the world has no identity map");
  flecs::entity_t entity = map->find(record.entity);
  flecs::entity_t parent = 0;
  if (source.mapping->parent == schema::MaterializeParent::ChildOf && !source.parent.is_null())
    parent = map->find(source.parent);
  const flecs::entity_t identity = w.id<Identity>().raw_id();

  const bool created = entity == 0;
  if (created) {
    // The whole archetype at once — identity, every component, the parent — found as one type and
    // committed in one move. Not `ecs_entity_init` with `add` and `parent`: that walks the table
    // graph one id at a time from the parent's `ChildOf` table, and every step of the walk is a
    // table flecs creates and keeps. Under a new parent that was three empty tables beside the one
    // its children live in (the 10^5-record bench: 23,787 tables against 5,883 parents); found by
    // type it is the one.
    ids_.clear();
    ids_.push_back(identity);
    for (const Group& group : r.groups)
      ids_.push_back(group.component);
    if (parent != 0) ids_.push_back(ecs_pair(EcsChildOf, parent));
    // flecs' own order for a type: ascending ids, no duplicates.
    std::sort(ids_.begin(), ids_.end());
    ids_.erase(std::unique(ids_.begin(), ids_.end()), ids_.end());
    const auto count = static_cast<i32>(ids_.size());
    ecs_table_t* table = ecs_table_find(raw, ids_.data(), count);
    entity = ecs_new(raw);
    // The added ids are the whole type: `OnAdd` observers see what `ecs_entity_init` would have
    // shown them.
    const ecs_type_t added = {ids_.data(), count};
    ecs_commit(raw, entity, nullptr, table, &added, nullptr);
    // Written and marked modified, so the identity map's `OnSet` observer binds it now: a later
    // hook of the same record finds the entity by its id.
    auto* named = static_cast<Identity*>(ecs_ensure_id(raw, entity, identity, sizeof(Identity)));
    named->id = record.entity;
    ecs_modified_id(raw, entity, identity);
    ++stats_.created;
  } else {
    ++stats_.updated;
    if (source.mapping->parent == schema::MaterializeParent::ChildOf) {
      const flecs::entity_t current = ecs_get_target(raw, entity, EcsChildOf, 0);
      if (current != parent) {
        if (parent != 0) {
          ecs_add_pair(raw, entity, EcsChildOf, parent);
        } else {
          ecs_remove_pair(raw, entity, EcsChildOf, EcsWildcard);
        }
      }
    }
  }

  for (const Group& group : r.groups) {
    if (group.size == 0 || group.rows == 0) {
      // A tag, or a component the mapping names without a field: its defaults on creation, and on
      // an update only put back if something took it away.
      if (!created) ecs_add_id(raw, entity, group.component);
      continue;
    }
    void* storage = ecs_ensure_id(raw, entity, group.component, group.size);
    for (u32 i = group.first_row; i < group.first_row + group.rows; ++i) {
      schema::ReadContext ctx;
      if (!schema::read_mapped(source.mapping->fields[i], *r.fields[i], storage, *source.values[i],
                               ctx)) {
        // The field keeps what it had. The document's own validation names the property.
        ++stats_.invalid_values;
      }
    }
    ecs_modified_id(raw, entity, group.component);
    ++stats_.component_writes;
  }
  if (!r.writable.empty()) snapshot(record.entity, index, entity);
  return handle_of(flecs::entity(raw, entity));
}

void RecordMaterializer::dematerialize(sim::EntityHandle handle) {
  flecs::world& w = *world_;
  const flecs::entity e = entity_of(w, handle);
  if (!e.is_valid()) return;
  // Never a cascade: the document keeps a deleted record's children, so the world keeps their
  // entities, at the root until the driver re-links them.
  children_.clear();
  e.children([this](flecs::entity child) { children_.push_back(child.id()); });
  for (const flecs::entity_t child : children_) {
    ecs_remove_pair(w.c_ptr(), child, EcsChildOf, e.id());
    ++stats_.unlinked_children;
  }
  release_watch(id_of(e));
  ecs_delete(w.c_ptr(), e.id());
  ++stats_.destroyed;
}

// ---- write-back ---------------------------------------------------------------------------------

void RecordMaterializer::snapshot(const Id128& id, u32 resolved, flecs::entity_t entity) {
  Watch* watch = watches_.find_value(id);
  if (watch != nullptr && watch->resolved != resolved) {
    release_watch(id);
    watch = nullptr;
  }
  Resolved& r = resolved_[resolved];
  if (watch == nullptr) {
    u32 slot = 0;
    if (!r.free_slots.empty()) {
      slot = r.free_slots.back();
      r.free_slots.pop_back();
    } else {
      slot = r.slots++;
      r.shadow.resize(r.slots * r.shadow_bytes);
    }
    watches_.insert(id, Watch{resolved, slot});
    watch = watches_.find_value(id);
  }
  // What was just written is the baseline: a field differs from it only once a system writes it.
  u8* shadow = r.shadow.data() + static_cast<usize>(watch->slot) * r.shadow_bytes;
  u32 offset = 0;
  for (const u32 row : r.writable) {
    const Group& group = r.groups[r.row_group[row]];
    const schema::FieldInfo& field = *r.fields[row];
    const u32 size = static_cast<u32>(field.type.size);
    const void* component = ecs_get_id(world_->c_ptr(), entity, group.component);
    if (component != nullptr)
      std::memcpy(shadow + offset, static_cast<const u8*>(component) + field.offset, size);
    offset += size;
  }
}

void RecordMaterializer::release_watch(const Id128& id) {
  const Watch* watch = watches_.find_value(id);
  if (watch == nullptr) return;
  resolved_[watch->resolved].free_slots.push_back(watch->slot);
  watches_.erase(id);
}

void RecordMaterializer::collect(Vector<sim::WriteBackChange>& out) {
  ++stats_.writeback_scans;
  const u32 before = out.size();
  const IdentityMap* map = identity_map_if_present(*world_);
  if (map == nullptr) return;
  ecs_world_t* raw = world_->c_ptr();
  // A byte comparison per writable field of every watched entity: no allocation when nothing
  // changed, and a JSON value only for a field that did.
  for (u32 i = 0; i < watches_.size(); ++i) {
    const Id128 id = watches_.key_at(i);
    const Watch& watch = watches_.value_at(i);
    Resolved& r = resolved_[watch.resolved];
    const flecs::entity_t entity = map->find(id);
    if (entity == 0) continue;
    u8* shadow = r.shadow.data() + static_cast<usize>(watch.slot) * r.shadow_bytes;
    u32 offset = 0;
    for (const u32 row : r.writable) {
      const Group& group = r.groups[r.row_group[row]];
      const schema::FieldInfo& field = *r.fields[row];
      const u32 size = static_cast<u32>(field.type.size);
      const void* component = ecs_get_id(raw, entity, group.component);
      if (component != nullptr) {
        const u8* now = static_cast<const u8*>(component) + field.offset;
        if (std::memcmp(now, shadow + offset, size) != 0) {
          sim::WriteBackChange change;
          change.record = id;
          change.mapping = r.info;
          change.row = row;
          if (schema::write_mapped(r.info->fields[row], field, component, change.value))
            out.push_back(std::move(change));
          std::memcpy(shadow + offset, now, size);
        }
      }
      offset += size;
    }
  }
  // By record, then row: the hash map's order is not an order anything may depend on.
  std::sort(out.begin() + before, out.end(),
            [](const sim::WriteBackChange& a, const sim::WriteBackChange& b) {
              if (!(a.record == b.record)) return a.record < b.record;
              return a.row < b.row;
            });
  stats_.writeback_changes += out.size() - before;
}

}  // namespace engine::ecs

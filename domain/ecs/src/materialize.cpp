#include <core/base/assert.h>
#include <core/log/log.h>
#include <domain/ecs/components.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/materialize.h>

#include <algorithm>
#include <cstring>

namespace engine::ecs {

ENGINE_LOG_CATEGORY_DECLARE(log_ecs);

RecordMaterializer::RecordMaterializer(flecs::world& world, const RecordMaterializerConfig& config)
    : world_(&world), config_(config) {
  // Both singletons exist before the first record arrives, for the reason `WorldCommands` forces
  // them: a world is writable here, and nothing later should have to create them.
  components(world);
  identity_map(world);
}

RecordMaterializer::~RecordMaterializer() {
  for (Resolved& r : resolved_) {
    if (r.query != nullptr) ecs_query_fini(r.query);
  }
}

usize RecordMaterializer::writeback_bytes() const noexcept {
  usize bytes = static_cast<usize>(watches_.capacity()) * (sizeof(Id128) + sizeof(Watch)) +
                static_cast<usize>(watches_.bucket_count()) * sizeof(u32);
  for (const Resolved& r : resolved_) {
    bytes += r.shadow.capacity() + r.free_slots.capacity() * sizeof(u32);
    bytes += r.writable_term.capacity();
    if (r.query != nullptr) bytes += sizeof(ecs_query_t);
  }
  return bytes;
}

u32 RecordMaterializer::writeback_queries() const noexcept {
  u32 n = 0;
  for (const Resolved& r : resolved_)
    n += r.query != nullptr ? 1u : 0u;
  return n;
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
    if (config_.scan == WritebackScan::Changed && r.query == nullptr) make_query(resolved);
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

// The tracked scan's query for one mapping: `Identity` first, then each component a writable row
// lives in, all `in` — reading them marks nothing dirty — with change detection, which makes flecs
// keep a change counter per column of every table it matches and a monitor of what this query last
// saw. Disabled and prefab entities match too, because the scan of every entity it stands in for
// reached them through the identity map.
void RecordMaterializer::make_query(u32 resolved) {
  Resolved& r = resolved_[resolved];
  ecs_query_desc_t desc = {};
  desc.terms[0].id = world_->id<Identity>().raw_id();
  desc.terms[0].inout = EcsIn;
  u8 terms = 1;
  r.writable_term.clear();
  for (const u32 row : r.writable) {
    const flecs::entity_t component = r.groups[r.row_group[row]].component;
    u8 term = 1;
    while (term < terms && desc.terms[term].id != component)
      ++term;
    if (term == terms) {
      ENGINE_ASSERT(terms < FLECS_TERM_COUNT_MAX, "ecs::RecordMaterializer: too many components");
      desc.terms[terms].id = component;
      desc.terms[terms].inout = EcsIn;
      ++terms;
    }
    r.writable_term.push_back(term);
  }
  desc.flags = EcsQueryDetectChanges | EcsQueryMatchDisabled | EcsQueryMatchPrefab;
  desc.cache_kind = EcsQueryCacheAuto;
  r.query = ecs_query_init(world_->c_ptr(), &desc);
  ENGINE_ASSERT(r.query != nullptr, "ecs::RecordMaterializer: the write-back query was refused");
}

void RecordMaterializer::collect(Vector<sim::WriteBackChange>& out) {
  ++stats_.writeback_scans;
  const u32 before = out.size();
  if (config_.scan == WritebackScan::Every) {
    collect_every(out);
  } else {
    collect_changed(out);
    if (config_.verify) {
      // What the tracked scan already took is in the shadow now, so the full scan behind it finds
      // exactly what the tracking missed.
      const u32 tracked = out.size();
      collect_every(out);
      const u32 missed = out.size() - tracked;
      if (missed != 0) {
        stats_.writeback_missed += missed;
        const sim::WriteBackChange& first = out[tracked];
        const schema::MaterializeField& row = first.mapping->fields[first.row];
        ENGINE_LOG_WARN(log_ecs, "write-back: a write the change tracking did not see",
                        log::field("fields", missed), log::field("record", first.record),
                        log::field("component", row.component->qualified_name),
                        log::field("field", row.field));
      }
    }
  }
  // By record, then row: the hash map's order and flecs' table order are not orders anything may
  // depend on.
  std::sort(out.begin() + before, out.end(),
            [](const sim::WriteBackChange& a, const sim::WriteBackChange& b) {
              if (!(a.record == b.record)) return a.record < b.record;
              return a.row < b.row;
            });
  stats_.writeback_changes += out.size() - before;
}

void RecordMaterializer::collect_changed(Vector<sim::WriteBackChange>& out) {
  ecs_world_t* raw = world_->c_ptr();
  for (u32 index = 0; index < resolved_.size(); ++index) {
    Resolved& r = resolved_[index];
    if (r.query == nullptr) continue;
    // Nothing tracked wrote any table this mapping's entities live in: the whole mapping is one
    // check of each matched table's counters against the query's monitor.
    if (!ecs_query_changed(r.query)) continue;
    ecs_iter_t it = ecs_query_iter(raw, r.query);
    while (ecs_query_next(&it)) {
      if (!ecs_iter_changed(&it)) {
        ecs_iter_skip(&it);
        ++stats_.writeback_skipped;
        continue;
      }
      ++stats_.writeback_tables;
      const auto* ids =
          static_cast<const Identity*>(ecs_field_w_size(&it, sizeof(Identity), i8{0}));
      // Each writable row's column in this table, once a table rather than once an entity.
      columns_.clear();
      for (u32 k = 0; k < r.writable.size(); ++k) {
        const Group& group = r.groups[r.row_group[r.writable[k]]];
        columns_.push_back(static_cast<const u8*>(
            ecs_field_w_size(&it, group.size, static_cast<i8>(r.writable_term[k]))));
      }
      for (i32 row_index = 0; row_index < it.count; ++row_index) {
        const Id128& id = ids[row_index].id;
        const Watch* watch = watches_.find_value(id);
        // Not watched, or watched under another mapping whose own query reaches it.
        if (watch == nullptr || watch->resolved != index) continue;
        ++stats_.writeback_rows;
        u8* shadow = r.shadow.data() + static_cast<usize>(watch->slot) * r.shadow_bytes;
        u32 offset = 0;
        for (u32 k = 0; k < r.writable.size(); ++k) {
          const u32 row = r.writable[k];
          const Group& group = r.groups[r.row_group[row]];
          const schema::FieldInfo& field = *r.fields[row];
          const u32 size = static_cast<u32>(field.type.size);
          const u8* component = columns_[k] + static_cast<usize>(row_index) * group.size;
          const u8* now = component + field.offset;
          if (std::memcmp(now, shadow + offset, size) != 0) {
            sim::WriteBackChange change;
            change.record = id;
            change.mapping = r.info;
            change.row = row;
            if (schema::write_mapped(r.info->fields[row], field, component, change.value))
              out.push_back(std::move(change));
            std::memcpy(shadow + offset, now, size);
          }
          offset += size;
        }
      }
    }
  }
}

void RecordMaterializer::collect_every(Vector<sim::WriteBackChange>& out) {
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
    ++stats_.writeback_rows;
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
}

}  // namespace engine::ecs

#include <core/base/assert.h>
#include <core/log/log.h>
#include <core/memory/memory.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/sim/materialize.h>

#include <algorithm>
#include <cstddef>
#include <string>

namespace engine::sim {

ENGINE_LOG_CATEGORY_DECLARE(log_sim);

namespace {

// A parent chain longer than this is a cycle, which `doc::Document::validate` reports and a strict
// command refuses to make; a document loaded non-strictly could still hold one, and the depth walk
// must end whatever it is handed.
constexpr u32 k_max_depth = 1u << 16;

bool covers(u8 mask, u8 tier) noexcept {
  if (static_cast<u32>(tier) >= 8u) return false;
  return ((static_cast<u32>(mask) >> tier) & 1u) != 0u;
}

// The per-entity seed plan 05 §5.10 asks for — a function of the id alone — so a hook that seeds a
// random stream per entity gets the same stream in every run and on every machine.
u64 seed_of(const Id128& id) noexcept {
  u64 x = id.hi ^ (id.lo * 0x9E3779B97F4A7C15ull);
  x ^= x >> 33;
  x *= 0xFF51AFD7ED558CCDull;
  x ^= x >> 33;
  return x;
}

// The tile the record's own file is in: the defining record, under the defining layer's partition,
// by `doc::tile_of` — the same function the layer's file layout uses, so "the records of tile
// (x, y)" means the records `layers/<layer>/tiles/<x>_<y>.json` holds. The document's tile index
// keeps it for every record (`doc::Document::object_tile`), so this is a hash lookup; it used to be
// a visit of the record's layers and a read of its position, for every live record of the document
// on every tile pass.
bool record_tile(const doc::Document& document, const Id128& id, doc::TileCoord& out) {
  return document.object_tile(id, out);
}

u64 tile_key(doc::TileCoord tile) noexcept {
  return (static_cast<u64>(static_cast<u32>(tile.x)) << 32) | static_cast<u32>(tile.y);
}

}  // namespace

const char* skip_reason_name(SkipReason reason) noexcept {
  switch (reason) {
    case SkipReason::NoMapping: return "no mapping";
    case SkipReason::UnknownType: return "unknown type";
    case SkipReason::Tier: return "tier";
    case SkipReason::MissingComponent: return "missing component";
    case SkipReason::NoEntityStore: return "no entity store";
    case SkipReason::NoHook: return "no hook";
  }
  return "?";
}

Materializer::Materializer(SimScheduler& scheduler, const MaterializeConfig& config)
    : scheduler_(&scheduler), config_(config) {}

Materializer::~Materializer() = default;

// ---- write-back as a system in the scheduler's table ------------------------------------------

void Materializer::install_writeback() {
  SystemDesc desc;
  desc.name = "sim.write_back";
  desc.phase = TickPhase::Persist;
  // The one writer of the document from the world, declared as such: nothing else in the table may
  // be scheduled beside it against the same document.
  desc.writes_resources = resource_mask("sim.document");
  desc.determinism = Determinism::Hashed;
  desc.context = this;
  desc.tick = &Materializer::writeback_tick;
  scheduler_->add_system(desc);
}

void Materializer::writeback_tick(SystemContext& context, Batch) {
  auto* self = static_cast<Materializer*>(context.context);
  const u32 every = self->config_.writeback_every;
  if (every == 0 || context.tick.value % every != 0) return;
  self->flush_writeback(context.tick, context.time);
}

u32 Materializer::flush_writeback(SimTick tick, GameTime time) {
  if (target_.collect_writeback == nullptr || document_ == nullptr) return 0;
  changes_.clear();
  target_.collect_writeback(target_.context, changes_);
  if (changes_.empty()) return 0;

  WriteBackBatch batch;
  batch.tick = tick;
  batch.time = time;
  // Attributed to the system, as every other edit is attributed to whoever made it (03 §3.3): the
  // journal and `session.events` then say "the simulation wrote this", which is the whole
  // difference between a write-back and an agent's edit of the same property.
  batch.attribution.actor = "system";
  batch.attribution.role = "system";
  batch.attribution.task = "sim.write_back";
  u32 records = 0;
  for (u32 i = 0; i < changes_.size(); ++i) {
    if (i == 0 || !(changes_[i].record == changes_[i - 1].record)) ++records;
  }
  batch.attribution.rationale = "tick " + std::to_string(tick.value) + ": " +
                                std::to_string(changes_.size()) + " writable field(s) of " +
                                std::to_string(records) + " record(s) changed in the runtime world";
  batch.commands.reserve(changes_.size());
  for (const WriteBackChange& change : changes_) {
    batch.commands.push_back(
        doc::cmd_set(change.record, change.mapping->fields[change.row].property, change.value));
  }

  // A record the driver holds in sync before the commit is still in sync after it — the commit is
  // this world's own state going back — so its held revision moves with the document and the next
  // pass leaves it alone. One that was already behind (edited by someone else since it was
  // materialized) stays behind, so the next pass still materializes that edit.
  scratch_ids_.clear();
  for (u32 i = 0; i < changes_.size(); ++i) {
    if (i != 0 && changes_[i].record == changes_[i - 1].record) continue;
    const Held* held = held_.find_value(changes_[i].record);
    if (held != nullptr && held->revision == document_->revision_of(changes_[i].record))
      scratch_ids_.push_back(changes_[i].record);
  }

  batch.changes = std::move(changes_);
  const bool ok = sink_.commit != nullptr && sink_.commit(sink_.context, batch);
  const u32 fields = batch.changes.size();
  changes_ = std::move(batch.changes);
  if (!ok) {
    ++stats_.writeback_refused;
    ENGINE_LOG_WARN(log_sim, "write-back refused", log::field("tick", tick.value),
                    log::field("fields", fields));
    return 0;
  }
  for (const Id128& id : scratch_ids_) {
    if (Held* held = held_.find_value(id)) held->revision = document_->revision_of(id);
  }
  // Every record this wrote may now be in another tile of the document's; `take_moved` says which.
  for (u32 i = 0; i < changes_.size(); ++i) {
    if (i != 0 && changes_[i].record == changes_[i - 1].record) continue;
    if (held_.contains(changes_[i].record)) written_.insert(changes_[i].record, 1);
  }
  ++stats_.writeback_flushes;
  stats_.writeback_fields += fields;
  return fields;
}

// ---- the mapping table --------------------------------------------------------------------------

Materializer::MappingState& Materializer::mapping_state(const schema::MaterializeInfo& info) {
  if (const u32* at = mapping_index_.find_value(&info)) return mappings_[*at];
  MappingState state;
  state.info = &info;
  // The record type's default for every row's property, rendered once. A record that does not set a
  // property gets exactly what a default-constructed record of its type holds, which is what the
  // document's own validation reads it as.
  const schema::TypeInfo& record = *info.record;
  void* scratch = mem::allocate(record.size, record.align);
  record.ops->construct(scratch);
  for (const schema::MaterializeField& row : info.fields) {
    JsonValue value;
    if (const schema::FieldInfo* field = record.find_field(row.property))
      (void)schema::to_json(field->type, static_cast<const std::byte*>(scratch) + field->offset,
                            value);
    state.defaults.push_back(std::move(value));
  }
  record.ops->destroy(scratch);
  mem::deallocate(scratch, record.size, record.align);
  mappings_.push_back(std::move(state));
  mapping_index_.insert(&info, mappings_.size() - 1u);
  return mappings_.back();
}

bool Materializer::check_mapping(MappingState& state, SkipReason& reason) {
  // Once per pass per mapping: a world may gain a capability's components between two calls.
  if (state.checked_pass != pass_) {
    state.checked_pass = pass_;
    state.usable = true;
    state.missing.clear();
    if (target_.name == nullptr) {
      state.usable = false;
      state.missing = "this world has no entity store";
    } else if (target_.has_component != nullptr) {
      for (const schema::TypeInfo* component : state.info->components) {
        if (target_.has_component(target_.context, *component)) continue;
        state.usable = false;
        state.missing = component->qualified_name;
        break;
      }
    }
  }
  if (!state.usable) {
    reason = target_.name == nullptr ? SkipReason::NoEntityStore : SkipReason::MissingComponent;
  }
  return state.usable;
}

// ---- the report ---------------------------------------------------------------------------------

static MaterializedType& type_row(Vector<MaterializedType>& types, std::string_view type,
                                  bool mapped) {
  for (MaterializedType& row : types) {
    if (row.type == type) return row;
  }
  MaterializedType row;
  row.type = std::string(type);
  row.mapped = mapped;
  types.push_back(std::move(row));
  return types.back();
}

void Materializer::count_type(MaterializeReport& report, std::string_view type, bool mapped,
                              bool materialized, const SkipReason* reason,
                              std::string_view detail) {
  MaterializedType& row = type_row(report.types, type, mapped);
  ++row.records;
  if (materialized) ++row.materialized;
  if (reason != nullptr) {
    if (row.skipped == 0) {
      row.reason = *reason;
      row.detail = std::string(detail);
    }
    ++row.skipped;
  }
}

void Materializer::skip(MaterializeReport& report, const Id128& id, std::string_view type,
                        bool mapped, SkipReason reason, std::string_view detail) {
  ++report.skipped;
  count_type(report, type, mapped, false, &reason, detail);
  if (report.skips.size() < config_.max_listed_skips) {
    SkippedRecord listed;
    listed.id = id;
    listed.type = std::string(type);
    listed.reason = reason;
    report.skips.push_back(std::move(listed));
  }
}

// ---- the walk -----------------------------------------------------------------------------------

u32 Materializer::depth_of(const doc::Document& document, const Id128& id) {
  if (const u32* cached = depth_cache_.find_value(id)) return *cached;
  // Up the chain to a root or to an ancestor already measured, then down again filling the cache,
  // so a pass measures each record once however deep the hierarchy is.
  chain_.clear();
  Id128 cursor = id;
  u32 base = 0;
  for (;;) {
    chain_.push_back(cursor);
    const Id128 parent = document.parent_of(cursor);
    if (parent.is_null() || !document.exists(parent) || chain_.size() > k_max_depth) break;
    if (const u32* cached = depth_cache_.find_value(parent)) {
      base = *cached + 1u;
      break;
    }
    cursor = parent;
  }
  const u32 n = chain_.size();
  for (u32 i = n; i > 0; --i)
    depth_cache_.insert_or_assign(chain_[i - 1u], base + (n - i));
  return base + (n - 1u);
}

bool Materializer::in_scope(const MaterializeScope& scope, bool tiled,
                            doc::TileCoord tile) const noexcept {
  switch (scope.kind) {
    case MaterializeScope::Kind::Whole: return true;
    case MaterializeScope::Kind::Tile: return tiled && tile == scope.tile;
    case MaterializeScope::Kind::Untiled: return !tiled;
  }
  return false;
}

Vector<Id128>* Materializer::filing(bool tiled, doc::TileCoord tile, bool create) {
  if (!tiled) return &filed_untiled_;
  if (create) return &filed_[tile_key(tile)];
  return filed_.find_value(tile_key(tile));
}

void Materializer::filed_in(const MaterializeScope& scope, Vector<Id128>& out) {
  const Vector<Id128>* list = scope.kind == MaterializeScope::Kind::Untiled
                                  ? &filed_untiled_
                                  : filed_.find_value(tile_key(scope.tile));
  if (list == nullptr) return;
  for (const Id128& id : *list)
    out.push_back(id);
}

void Materializer::file(const Id128& id, Held& held) {
  Vector<Id128>& list = *filing(held.tiled, held.tile, true);
  held.slot = list.size();
  list.push_back(id);
}

void Materializer::unfile(const Held& held) {
  Vector<Id128>* list = filing(held.tiled, held.tile, false);
  if (list == nullptr || held.slot >= list->size()) return;
  // Swap-remove: the list has no order to keep (a pass sorts what it takes from it), and the record
  // that takes the slot is told where it now is.
  const u32 last = list->size() - 1u;
  if (held.slot != last) {
    const Id128 moved = (*list)[last];
    (*list)[held.slot] = moved;
    if (Held* other = held_.find_value(moved)) other->slot = held.slot;
  }
  list->pop_back();
  if (list->empty() && held.tiled) filed_.erase(tile_key(held.tile));
}

Materializer::Verdict Materializer::classify(const doc::Document& document, const Id128& id,
                                             const MaterializeScope& scope,
                                             MaterializeReport& report, Candidate& out) {
  out = Candidate{};
  out.id = id;
  out.tiled = record_tile(document, id, out.tile);
  if (!in_scope(scope, out.tiled, out.tile)) return Verdict::OutOfScope;
  ++report.visited;

  const std::string_view type = document.type_of(id);
  const schema::MaterializeInfo* mapping = schema::MaterializeRegistry::global().find(type);
  if (mapping == nullptr) {
    // Document-only, by design or because this build never heard of the type; the report says
    // which, since the second is usually a capability switched off.
    const bool known = schema::Registry::global().find(type) != nullptr;
    skip(report, id, type, false, known ? SkipReason::NoMapping : SkipReason::UnknownType,
         known ? "" : "the type is not in this build's schema registry");
    return Verdict::Skipped;
  }
  if (!covers(mapping->tiers, config_.tier)) {
    skip(report, id, type, true, SkipReason::Tier,
         "the mapping does not materialize at tier " + std::to_string(config_.tier));
    return Verdict::Skipped;
  }
  MappingState& state = mapping_state(*mapping);
  SkipReason reason = SkipReason::NoHook;
  if (!check_mapping(state, reason)) {
    skip(report, id, type, true, reason, state.missing);
    return Verdict::Skipped;
  }
  out.mapping = mapping;
  out.depth = depth_of(document, id);
  return Verdict::Candidate;
}

bool Materializer::materialize_one(const doc::Document& document, const Candidate& candidate,
                                   MaterializeReport& report, bool relink) {
  const schema::MaterializeInfo& info = *candidate.mapping;
  MappingState& state = mapping_state(info);

  // The composed value of every row's property: the strongest layer that sets it, found in one
  // visit of the layers that hold the record (weakest first, so a later layer overwrites), and the
  // type's default for a property no layer sets.
  const u32 rows = static_cast<u32>(info.fields.size());
  values_.assign(rows, nullptr);
  document.visit_records(candidate.id, [&](u32, const doc::ObjectRecord& record) {
    for (u32 i = 0; i < rows; ++i) {
      if (const JsonValue* value =
              record.properties.find_value(std::string_view(info.fields[i].property)))
        values_[i] = value;
    }
  });
  for (u32 i = 0; i < rows; ++i) {
    if (values_[i] == nullptr) values_[i] = &state.defaults[i];
  }

  RecordSource source;
  source.mapping = &info;
  source.values = std::span<const JsonValue* const>(values_.data(), values_.size());
  source.parent = info.parent == schema::MaterializeParent::ChildOf
                      ? document.parent_of(candidate.id)
                      : Id128{};
  source.revision = document.revision_of(candidate.id);

  EntityRecord record;
  record.entity = candidate.id;
  record.seed = seed_of(candidate.id);
  record.kind = schema::stable_type_id(info.record->qualified_name);
  record.tier = config_.tier;
  record.source = &source;

  ++stats_.hook_calls;
  order_.push_back(candidate.id);
  const EntityHandle handle = scheduler_->materialize(record, config_.tier);
  if (handle.is_null()) {
    skip(report, candidate.id, info.record->qualified_name, true, SkipReason::NoHook,
         "every hook ran and none gave the record a runtime existence");
    return false;
  }

  // Linked when the parent is one this driver holds; otherwise an orphan, re-materialized when the
  // parent arrives — a parent in another tile, or one whose type materializes at other tiers.
  const bool linked = source.parent.is_null() || held_.contains(source.parent);
  Held held;
  held.revision = source.revision;
  held.mapping = &info;
  held.parent = source.parent;
  held.depth = candidate.depth;
  held.tile = candidate.tile;
  held.tiled = candidate.tiled;
  held.parent_linked = linked;
  held.seen_pass = pass_;
  Held* existing = held_.find_value(candidate.id);
  const bool existed = existing != nullptr;
  if (existing == nullptr) {
    held_.insert(candidate.id, held);
    file(candidate.id, *held_.find_value(candidate.id));
  } else if (existing->tiled == held.tiled && (!held.tiled || existing->tile == held.tile)) {
    held.slot = existing->slot;
    *existing = held;
  } else {
    unfile(*existing);
    *existing = held;
    file(candidate.id, *existing);
  }
  if (!linked) orphans_.push_back(candidate.id);
  // A relink is neither a creation nor an edit of the record, and the pass has already counted
  // the record if it looked at it; the report counts relinks on their own.
  if (relink) return true;
  if (existed) {
    ++report.updated;
    ++stats_.updated;
  } else {
    ++report.created;
    ++stats_.created;
  }
  count_type(report, info.record->qualified_name, true, true, nullptr, {});
  return true;
}

void Materializer::dematerialize_ids(Vector<Id128>& ids, MaterializeReport& report) {
  if (ids.empty()) return;
  // Deepest first, then by id: the mirror of the creation order, so a child lets go before its
  // parent does and the order is the same on every run.
  std::sort(ids.begin(), ids.end(), [this](const Id128& a, const Id128& b) {
    const Held* ha = held_.find_value(a);
    const Held* hb = held_.find_value(b);
    const u32 da = ha != nullptr ? ha->depth : 0;
    const u32 db = hb != nullptr ? hb->depth : 0;
    if (da != db) return da > db;
    return a < b;
  });
  handles_.clear();
  for (const Id128& id : ids) {
    if (target_.resolve == nullptr) break;
    const EntityHandle handle = target_.resolve(target_.context, id);
    if (!handle.is_null()) handles_.push_back(handle);
  }
  scheduler_->dematerialize(std::span<const EntityHandle>(handles_.data(), handles_.size()));
  for (const Id128& id : ids) {
    const Held* gone = held_.find_value(id);
    if (gone == nullptr) continue;
    unfile(*gone);
    held_.erase(id);
    ++report.dematerialized;
    ++stats_.dematerialized;
    // Children that stay lose their parent's entity, and become orphans until it comes back.
    if (document_ == nullptr) continue;
    for (const Id128& child : document_->children(id)) {
      Held* held = held_.find_value(child);
      if (held == nullptr || !(held->parent == id) || !held->parent_linked) continue;
      held->parent_linked = false;
      orphans_.push_back(child);
    }
  }
}

void Materializer::relink_orphans(const doc::Document& document, MaterializeReport& report) {
  if (orphans_.empty()) return;
  std::sort(orphans_.begin(), orphans_.end());
  orphans_.erase(std::unique(orphans_.begin(), orphans_.end()), orphans_.end());
  // Parents first, so a chain of orphans relinks top-down in one pass.
  std::stable_sort(orphans_.begin(), orphans_.end(), [this](const Id128& a, const Id128& b) {
    const Held* ha = held_.find_value(a);
    const Held* hb = held_.find_value(b);
    return (ha != nullptr ? ha->depth : 0) < (hb != nullptr ? hb->depth : 0);
  });
  u32 kept = 0;
  for (u32 i = 0; i < orphans_.size(); ++i) {
    const Id128 id = orphans_[i];
    const Held* held = held_.find_value(id);
    if (held == nullptr || held->parent_linked) continue;  // gone, or relinked by a later edit
    if (!held_.contains(held->parent) || !document.exists(id)) {
      orphans_[kept++] = id;
      continue;
    }
    Candidate candidate;
    candidate.id = id;
    candidate.depth = held->depth;
    candidate.mapping = held->mapping;
    candidate.tile = held->tile;
    candidate.tiled = held->tiled;
    if (materialize_one(document, candidate, report, true)) ++report.relinked;
  }
  orphans_.resize(kept);
}

MaterializeReport Materializer::materialize(const doc::Document& document,
                                            const MaterializeScope& scope) {
  const i64 start_ns = time::monotonic_ns();
  MaterializeReport report;
  ++pass_;
  if (document_ != &document) synced_ = false;  // another document: nothing held can be trusted
  document_ = &document;
  // Only when something is in it: `clear()` zeroes the whole bucket array, which after a first pass
  // over 10^5 records is a megabyte — and was the entire cost of a pass that found nothing
  // changed (`ecs.materialize.nochange`, 32 µs at 10^5 before this line had its guard).
  if (!depth_cache_.empty()) depth_cache_.clear();
  order_.clear();
  candidates_.clear();
  gone_.clear();

  // Incremental only for the whole document, and only when the feed still reaches back to what this
  // world reflects. Anything else compares revisions over the live set, which is correct whatever
  // happened in between and costs what a first pass costs.
  bool incremental = false;
  if (scope.kind == MaterializeScope::Kind::Whole && synced_) {
    scratch_ids_.clear();
    incremental = document.changed_since(revision_, scratch_ids_);
  }
  report.full = !incremental;

  if (incremental) {
    ++stats_.incremental_passes;
    for (const Id128& id : scratch_ids_) {
      Held* held = held_.find_value(id);
      if (!document.exists(id)) {
        if (held != nullptr) gone_.push_back(id);
        continue;
      }
      Candidate candidate;
      const Verdict verdict = classify(document, id, scope, report, candidate);
      held = held_.find_value(id);
      if (verdict != Verdict::Candidate) {
        if (held != nullptr) gone_.push_back(id);
        continue;
      }
      if (held != nullptr && held->revision == document.revision_of(id)) {
        // In the feed because this world wrote it back, and already in sync with it.
        ++report.unchanged;
        count_type(report, document.type_of(id), true, true, nullptr, {});
        continue;
      }
      candidates_.push_back(candidate);
    }
  } else {
    ++stats_.full_passes;
    // The records in scope: every live one for the whole document, and for a tile or the untiled
    // records what the document's tile index files there — never a classification of the document
    // to find them, which cost a tile pass the document rather than the tile (E38).
    const Vector<Id128> live_ids = scope.kind == MaterializeScope::Kind::Whole ? document.objects()
                                   : scope.kind == MaterializeScope::Kind::Tile
                                       ? document.ids_in_tile(scope.tile)
                                       : document.untiled();
    for (const Id128& id : live_ids) {
      Candidate candidate;
      const Verdict verdict = classify(document, id, scope, report, candidate);
      Held* held = held_.find_value(id);
      if (held == nullptr) {
        if (verdict == Verdict::Candidate) candidates_.push_back(candidate);
        continue;
      }
      if (verdict == Verdict::OutOfScope) {
        // Held and still live but no longer in this scope: it moved to another tile. The scope it
        // was filed under owns it, so it goes when that scope is the one being materialized.
        if (!in_scope(scope, held->tiled, held->tile)) held->seen_pass = pass_;
        continue;
      }
      held->seen_pass = pass_;
      if (verdict != Verdict::Candidate) {
        gone_.push_back(id);
        continue;
      }
      if (held->revision == document.revision_of(id) && held->mapping == candidate.mapping) {
        ++report.unchanged;
        count_type(report, document.type_of(id), true, true, nullptr, {});
        continue;
      }
      candidates_.push_back(candidate);
    }
    // Held under this scope and not seen by the pass: deleted, removed with a layer, or — for a
    // tile or the untiled records — filed elsewhere by the document now. For a tile that is what
    // the driver files under it, not everything it holds.
    if (scope.kind == MaterializeScope::Kind::Whole) {
      for (u32 i = 0; i < held_.size(); ++i) {
        if (held_.value_at(i).seen_pass != pass_) gone_.push_back(held_.key_at(i));
      }
    } else {
      scratch_ids_.clear();
      filed_in(scope, scratch_ids_);
      for (const Id128& id : scratch_ids_) {
        const Held* held = held_.find_value(id);
        if (held != nullptr && held->seen_pass != pass_) gone_.push_back(id);
      }
    }
  }

  // Parents before children, then by id: the order rule, and the reason nothing below depends on
  // the feed's order or the hash map's.
  std::sort(candidates_.begin(), candidates_.end(), [](const Candidate& a, const Candidate& b) {
    if (a.depth != b.depth) return a.depth < b.depth;
    return a.id < b.id;
  });
  for (const Candidate& candidate : candidates_)
    materialize_one(document, candidate, report, false);
  dematerialize_ids(gone_, report);
  relink_orphans(document, report);

  if (scope.kind == MaterializeScope::Kind::Whole) {
    revision_ = document.revision();
    synced_ = true;
  }
  report.revision = document.revision();
  report.live = held_.size();
  report.orphans = orphans_.size();
  report.ms = static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6;
  ENGINE_LOG_DEBUG(log_sim, "materialized", log::field("full", report.full),
                   log::field("visited", report.visited), log::field("created", report.created),
                   log::field("updated", report.updated),
                   log::field("dematerialized", report.dematerialized),
                   log::field("skipped", report.skipped), log::field("live", report.live));
  return report;
}

u32 Materializer::dematerialize_all() {
  MaterializeReport report;
  gone_.clear();
  for (const Id128& id : held_.keys())
    gone_.push_back(id);
  dematerialize_ids(gone_, report);
  orphans_.clear();
  filed_.clear();
  filed_untiled_.clear();
  synced_ = false;
  return report.dematerialized;
}

u32 Materializer::dematerialize(const MaterializeScope& scope) {
  if (scope.kind == MaterializeScope::Kind::Whole) return dematerialize_all();
  MaterializeReport report;
  gone_.clear();
  filed_in(scope, gone_);
  dematerialize_ids(gone_, report);
  // Anything left in the orphan list that is gone now is skipped by the next relink.
  return report.dematerialized;
}

void Materializer::held(const MaterializeScope& scope, Vector<Id128>& out) const {
  out.clear();
  if (scope.kind == MaterializeScope::Kind::Whole) {
    for (const Id128& id : held_.keys())
      out.push_back(id);
  } else {
    const Vector<Id128>* list = scope.kind == MaterializeScope::Kind::Untiled
                                    ? &filed_untiled_
                                    : filed_.find_value(tile_key(scope.tile));
    if (list != nullptr) {
      for (const Id128& id : *list)
        out.push_back(id);
    }
  }
  std::sort(out.begin(), out.end());
}

void Materializer::take_moved(Vector<TileMove>& out) {
  out.clear();
  if (written_.size() == 0) return;
  scratch_ids_.clear();
  for (u32 i = 0; i < written_.size(); ++i)
    scratch_ids_.push_back(written_.key_at(i));
  written_.clear();
  if (document_ == nullptr) return;
  std::sort(scratch_ids_.begin(), scratch_ids_.end());
  for (const Id128& id : scratch_ids_) {
    const Held* held = held_.find_value(id);
    if (held == nullptr) continue;
    TileMove move;
    move.id = id;
    move.from = held->tile;
    move.from_tiled = held->tiled;
    move.to_tiled = record_tile(*document_, id, move.to);
    if (move.to_tiled == held->tiled && (!move.to_tiled || move.to == held->tile)) continue;
    out.push_back(move);
  }
}

bool Materializer::refile(const Id128& id, bool tiled, doc::TileCoord tile) {
  Held* held = held_.find_value(id);
  if (held == nullptr) return false;
  unfile(*held);
  held->tiled = tiled;
  held->tile = tiled ? tile : doc::TileCoord{};
  file(id, *held);
  return true;
}

u32 Materializer::dematerialize(std::span<const Id128> ids) {
  MaterializeReport report;
  gone_.clear();
  for (const Id128& id : ids) {
    if (held_.contains(id)) gone_.push_back(id);
  }
  dematerialize_ids(gone_, report);
  return report.dematerialized;
}

u64 Materializer::last_order_hash() const noexcept {
  u64 hash = 1469598103934665603ull;
  for (const Id128& id : order_) {
    const u64 halves[2] = {id.hi, id.lo};
    for (const u64 half : halves) {
      for (u32 byte = 0; byte < 8; ++byte) {
        hash ^= (half >> (byte * 8u)) & 0xFFu;
        hash *= 1099511628211ull;
      }
    }
  }
  return hash;
}

MaterializeReport materialize(const doc::Document& document, Materializer& world,
                              const MaterializeScope& scope) {
  return world.materialize(document, scope);
}

}  // namespace engine::sim

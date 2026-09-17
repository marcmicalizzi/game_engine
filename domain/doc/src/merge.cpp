#include <core/containers/flat_set.h>
#include <core/profiling/profile.h>
#include <core/schema/json_reflect.h>
#include <domain/doc/merge.h>

#include <algorithm>
#include <utility>

namespace engine::doc {

namespace {

// Pseudo-property names for the record's own fields. A schema field name is an identifier, so
// it never contains '$' and these can never collide with a real property.
constexpr std::string_view k_type_property = "$type";
constexpr std::string_view k_parent_property = "$parent";

void set_error(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
}

std::string hex_of(ObjectId id) {
  char hex[Id128::k_hex_length + 1];
  id.to_hex(hex);
  return std::string(hex, Id128::k_hex_length);
}

JsonValue id_json(const std::optional<ObjectId>& id) {
  return id.has_value() ? JsonValue(hex_of(*id)) : JsonValue();
}

// The layer's opinion about an object's parent, as JSON: null when it has no record or no
// opinion, 32 hex characters otherwise.
JsonValue parent_json(const Layer& layer, ObjectId id) {
  const ObjectRecord* r = layer.find(id);
  return r != nullptr ? id_json(r->parent) : JsonValue();
}

JsonValue record_json(const ObjectRecord* record) {
  return record != nullptr ? schema::to_json(*record) : JsonValue();
}

JsonValue property_json(const ObjectRecord* record, const std::string& name) {
  if (record == nullptr) return JsonValue();
  const JsonValue* value = record->properties.find_value(name);
  return value != nullptr ? *value : JsonValue();
}

// The side removed the record, or turned its tombstone on.
bool side_deletes(const RecordDiff& d) noexcept {
  return d.removed || (d.deleted_changed && d.deleted);
}

// What the side changed apart from the parent, which is counted separately because the cycle
// rule may take a reparent back.
u32 field_change_count(const RecordDiff& d) noexcept {
  if (d.added || d.removed) return 1;
  return (d.type_changed ? 1u : 0u) + (d.deleted_changed ? 1u : 0u) + d.properties.size();
}

void apply_property(ObjectRecord& r, const PropertyChange& p) {
  if (p.removed) {
    r.properties.erase(p.name);
  } else {
    r.properties.insert_or_assign(p.name, p.value);
  }
}

bool is_empty_override(const ObjectRecord& r) noexcept {
  return r.type.empty() && !r.parent.has_value() && !r.deleted && r.properties.empty();
}

// Does `id`'s parent chain inside `layer` come back to `id`? A record the layer does not hold,
// or that has no parent opinion, ends the walk: one layer sees only its own part of the tree.
// A chain longer than the layer has records has run into a cycle that `id` is not part of; the
// members of that cycle report it themselves.
bool closes_cycle(const Layer& layer, ObjectId id) {
  ObjectId cursor = id;
  for (u32 hops = 0; hops <= layer.size(); ++hops) {
    const ObjectRecord* r = layer.find(cursor);
    if (r == nullptr || !r->parent.has_value() || r->parent->is_null()) return false;
    cursor = *r->parent;
    if (cursor == id) return true;
  }
  return false;
}

// A parent opinion the merge changed, with what to fall back to when it turns out to close a
// cycle. Parents are judged once every record is in place, because a cycle is a property of the
// finished layer, not of the order the objects were merged in.
struct ParentChange {
  std::optional<ObjectId> base_parent;
  JsonValue base;
  JsonValue ours;
  JsonValue theirs;
  bool credit_ours = false;
  bool credit_theirs = false;
};

class Merger {
 public:
  Merger(const Layer& base, const Layer& ours, const Layer& theirs, const MergeOptions& options,
         MergeResult& out) noexcept
      : base_(base), ours_(ours), theirs_(theirs), options_(options), out_(out) {}

  bool run(std::string* error);

 private:
  void merge_object(ObjectId id, const RecordDiff* ours_diff, const RecordDiff* theirs_diff);
  void merge_created(ObjectId id);
  void merge_fields(ObjectId id, const RecordDiff& ours_diff, const RecordDiff& theirs_diff);
  void apply_one_sided(const RecordDiff& d, const Layer& side, bool from_ours);
  void apply_fields(const RecordDiff& d, ObjectRecord& r);
  void take_record(ObjectId id, const ObjectRecord& record);
  void note_parent(ObjectId id, const std::optional<ObjectId>& value, bool credit_ours,
                   bool credit_theirs);
  void add_conflict(ObjectId object, std::string_view property, MergeConflict::Kind kind,
                    JsonValue base, JsonValue ours, JsonValue theirs);
  void resolve_cycles();
  void prune();

  const Layer& base_;
  const Layer& ours_;
  const Layer& theirs_;
  const MergeOptions& options_;
  MergeResult& out_;
  FlatMap<ObjectId, ParentChange> parents_;
};

void Merger::add_conflict(ObjectId object, std::string_view property, MergeConflict::Kind kind,
                          JsonValue base, JsonValue ours, JsonValue theirs) {
  MergeConflict c;
  c.object = object;
  c.property.assign(property);
  c.kind = kind;
  c.base = std::move(base);
  c.ours = std::move(ours);
  c.theirs = std::move(theirs);
  out_.conflicts.push_back(std::move(c));
}

void Merger::note_parent(ObjectId id, const std::optional<ObjectId>& value, bool credit_ours,
                         bool credit_theirs) {
  ParentChange info;
  if (const ObjectRecord* b = base_.find(id)) info.base_parent = b->parent;
  info.base = parent_json(base_, id);
  info.ours = parent_json(ours_, id);
  info.theirs = parent_json(theirs_, id);
  info.credit_ours = credit_ours;
  info.credit_theirs = credit_theirs;
  if (credit_ours) ++out_.applied_ours;
  if (credit_theirs) ++out_.applied_theirs;
  if (ObjectRecord* r = out_.merged.find(id)) r->parent = value;
  parents_.insert_or_assign(id, std::move(info));
}

void Merger::apply_fields(const RecordDiff& d, ObjectRecord& r) {
  if (d.type_changed) r.type = d.type;
  if (d.deleted_changed) r.deleted = d.deleted;
  for (const PropertyChange& p : d.properties)
    apply_property(r, p);
}

// Takes one side's record whole, and registers its parent so the cycle rule can judge it.
void Merger::take_record(ObjectId id, const ObjectRecord& record) {
  out_.merged.set(record);
  if (record.parent.has_value()) note_parent(id, record.parent, false, false);
}

void Merger::apply_one_sided(const RecordDiff& d, const Layer& side, bool from_ours) {
  u32& applied = from_ours ? out_.applied_ours : out_.applied_theirs;
  if (d.removed) {
    out_.merged.remove(d.id);
    applied += 1;
    return;
  }
  if (d.added) {
    take_record(d.id, *side.find(d.id));
    applied += 1;
    return;
  }
  ObjectRecord& r = out_.merged.ensure(d.id);
  apply_fields(d, r);
  applied += field_change_count(d);
  if (d.parent_changed) note_parent(d.id, d.parent, from_ours, !from_ours);
}

void Merger::merge_created(ObjectId id) {
  const ObjectRecord& ro = *ours_.find(id);
  const ObjectRecord& rt = *theirs_.find(id);
  if (ro == rt) {  // the same object, twice: nothing to disagree about
    take_record(id, ro);
    ++out_.applied_ours;
    ++out_.applied_theirs;
    return;
  }
  add_conflict(id, std::string_view(), MergeConflict::CreatedBothDifferent, JsonValue(),
               schema::to_json(ro), schema::to_json(rt));
  switch (options_.prefer_on_conflict) {
    case MergeOptions::Ours:
      take_record(id, ro);
      ++out_.applied_ours;
      return;
    case MergeOptions::Theirs:
      take_record(id, rt);
      ++out_.applied_theirs;
      return;
    case MergeOptions::Neither: out_.merged.remove(id); return;  // base has no record to keep
  }
}

void Merger::merge_fields(ObjectId id, const RecordDiff& ours_diff, const RecordDiff& theirs_diff) {
  ObjectRecord& r = out_.merged.ensure(id);
  const ObjectRecord* b = base_.find(id);

  if (ours_diff.type_changed || theirs_diff.type_changed) {
    if (!theirs_diff.type_changed) {
      r.type = ours_diff.type;
      ++out_.applied_ours;
    } else if (!ours_diff.type_changed) {
      r.type = theirs_diff.type;
      ++out_.applied_theirs;
    } else if (ours_diff.type == theirs_diff.type) {
      r.type = ours_diff.type;
      ++out_.applied_ours;
      ++out_.applied_theirs;
    } else {
      add_conflict(id, k_type_property, MergeConflict::PropertyBothChanged,
                   b != nullptr ? JsonValue(b->type) : JsonValue(), JsonValue(ours_diff.type),
                   JsonValue(theirs_diff.type));
      switch (options_.prefer_on_conflict) {
        case MergeOptions::Ours:
          r.type = ours_diff.type;
          ++out_.applied_ours;
          break;
        case MergeOptions::Theirs:
          r.type = theirs_diff.type;
          ++out_.applied_theirs;
          break;
        case MergeOptions::Neither:
          if (b != nullptr) r.type = b->type;
          break;
      }
    }
  }

  // The tombstone is a bool: both sides changing it means both moved off base's value, to the
  // same one, so there is nothing here to conflict about.
  if (ours_diff.deleted_changed || theirs_diff.deleted_changed) {
    r.deleted = ours_diff.deleted_changed ? ours_diff.deleted : theirs_diff.deleted;
    if (ours_diff.deleted_changed) ++out_.applied_ours;
    if (theirs_diff.deleted_changed) ++out_.applied_theirs;
  }

  // Properties, walking both change lists in name order.
  u32 x = 0, y = 0;
  while (x < ours_diff.properties.size() || y < theirs_diff.properties.size()) {
    const PropertyChange* a = x < ours_diff.properties.size() ? &ours_diff.properties[x] : nullptr;
    const PropertyChange* c =
        y < theirs_diff.properties.size() ? &theirs_diff.properties[y] : nullptr;
    if (c == nullptr || (a != nullptr && a->name < c->name)) {
      apply_property(r, *a);
      ++out_.applied_ours;
      ++x;
      continue;
    }
    if (a == nullptr || c->name < a->name) {
      apply_property(r, *c);
      ++out_.applied_theirs;
      ++y;
      continue;
    }
    ++x;
    ++y;
    if (a->removed == c->removed && (a->removed || a->value == c->value)) {
      apply_property(r, *a);  // the same edit on both sides
      ++out_.applied_ours;
      ++out_.applied_theirs;
      continue;
    }
    add_conflict(id, a->name, MergeConflict::PropertyBothChanged, property_json(b, a->name),
                 a->removed ? JsonValue() : a->value, c->removed ? JsonValue() : c->value);
    switch (options_.prefer_on_conflict) {
      case MergeOptions::Ours:
        apply_property(r, *a);
        ++out_.applied_ours;
        break;
      case MergeOptions::Theirs:
        apply_property(r, *c);
        ++out_.applied_theirs;
        break;
      case MergeOptions::Neither: {
        const JsonValue* value = b != nullptr ? b->properties.find_value(a->name) : nullptr;
        if (value != nullptr) {
          r.properties.insert_or_assign(a->name, *value);
        } else {
          r.properties.erase(a->name);
        }
        break;
      }
    }
  }

  if (!ours_diff.parent_changed && !theirs_diff.parent_changed) return;
  if (!theirs_diff.parent_changed) {
    note_parent(id, ours_diff.parent, true, false);
  } else if (!ours_diff.parent_changed) {
    note_parent(id, theirs_diff.parent, false, true);
  } else if (ours_diff.parent == theirs_diff.parent) {
    note_parent(id, ours_diff.parent, true, true);
  } else {
    add_conflict(id, k_parent_property, MergeConflict::PropertyBothChanged, parent_json(base_, id),
                 parent_json(ours_, id), parent_json(theirs_, id));
    switch (options_.prefer_on_conflict) {
      case MergeOptions::Ours: note_parent(id, ours_diff.parent, true, false); break;
      case MergeOptions::Theirs: note_parent(id, theirs_diff.parent, false, true); break;
      case MergeOptions::Neither: break;  // base's parent stands
    }
  }
}

void Merger::merge_object(ObjectId id, const RecordDiff* ours_diff, const RecordDiff* theirs_diff) {
  if (theirs_diff == nullptr) {
    apply_one_sided(*ours_diff, ours_, true);
    return;
  }
  if (ours_diff == nullptr) {
    apply_one_sided(*theirs_diff, theirs_, false);
    return;
  }
  if (ours_diff->added && theirs_diff->added) {
    merge_created(id);
    return;
  }

  const bool ours_deletes = side_deletes(*ours_diff);
  const bool theirs_deletes = side_deletes(*theirs_diff);
  if (ours_deletes != theirs_deletes) {
    // A deletion never silently destroys the other side's edits: the object survives, modified.
    add_conflict(id, std::string_view(), MergeConflict::DeletedAndModified,
                 record_json(base_.find(id)), record_json(ours_.find(id)),
                 record_json(theirs_.find(id)));
    if (options_.prefer_on_conflict == MergeOptions::Neither) return;  // base's record stands
    const RecordDiff& modified = ours_deletes ? *theirs_diff : *ours_diff;
    ObjectRecord& r = out_.merged.ensure(id);
    apply_fields(modified, r);
    u32& applied = ours_deletes ? out_.applied_theirs : out_.applied_ours;
    applied += field_change_count(modified);
    if (modified.parent_changed) note_parent(id, modified.parent, !ours_deletes, ours_deletes);
    return;
  }

  // Both deleted it. Removing the record is the stronger of the two forms, and taking it
  // whenever either side used it keeps the merge independent of the order of the arguments.
  if (ours_deletes && (ours_diff->removed || theirs_diff->removed)) {
    out_.merged.remove(id);
    out_.applied_ours += field_change_count(*ours_diff);
    out_.applied_theirs += field_change_count(*theirs_diff);
    return;
  }

  merge_fields(id, *ours_diff, *theirs_diff);
}

void Merger::resolve_cycles() {
  FlatSet<ObjectId> reverted;
  bool again = true;
  while (again) {
    again = false;
    for (auto [id, info] : parents_) {
      if (reverted.contains(id) || !closes_cycle(out_.merged, id)) continue;
      ObjectRecord* r = out_.merged.find(id);
      if (r == nullptr) continue;
      r->parent = info.base_parent;
      reverted.insert(id);
      again = true;
      if (info.credit_ours) --out_.applied_ours;
      if (info.credit_theirs) --out_.applied_theirs;
      add_conflict(id, k_parent_property, MergeConflict::ParentCycle, info.base, info.ours,
                   info.theirs);
    }
  }
}

void Merger::prune() {
  Vector<ObjectId> empty;
  for (auto [id, record] : out_.merged.records()) {
    if (is_empty_override(record)) empty.push_back(id);
  }
  for (const ObjectId id : empty)
    out_.merged.remove(id);
}

bool Merger::run(std::string* error) {
  out_.merged = base_;
  out_.conflicts.clear();
  out_.applied_ours = 0;
  out_.applied_theirs = 0;

  for (const ObjectId id : base_.records().keys()) {
    if (closes_cycle(base_, id)) {
      set_error(error,
                "base layer '" + base_.name() + "' already has a parent cycle at " + hex_of(id));
      return false;
    }
  }

  const Vector<RecordDiff> ours_diff = diff_records(base_, ours_);
  const Vector<RecordDiff> theirs_diff = diff_records(base_, theirs_);
  u32 i = 0, j = 0;
  while (i < ours_diff.size() || j < theirs_diff.size()) {
    if (j >= theirs_diff.size() || (i < ours_diff.size() && ours_diff[i].id < theirs_diff[j].id)) {
      merge_object(ours_diff[i].id, &ours_diff[i], nullptr);
      ++i;
    } else if (i >= ours_diff.size() || theirs_diff[j].id < ours_diff[i].id) {
      merge_object(theirs_diff[j].id, nullptr, &theirs_diff[j]);
      ++j;
    } else {
      merge_object(ours_diff[i].id, &ours_diff[i], &theirs_diff[j]);
      ++i;
      ++j;
    }
  }

  resolve_cycles();
  prune();
  std::stable_sort(out_.conflicts.begin(), out_.conflicts.end(),
                   [](const MergeConflict& a, const MergeConflict& b) {
                     if (a.object != b.object) return a.object < b.object;
                     return a.property < b.property;
                   });
  return true;
}

}  // namespace

const char* conflict_kind_name(MergeConflict::Kind kind) noexcept {
  switch (kind) {
    case MergeConflict::PropertyBothChanged: return "PropertyBothChanged";
    case MergeConflict::DeletedAndModified: return "DeletedAndModified";
    case MergeConflict::CreatedBothDifferent: return "CreatedBothDifferent";
    case MergeConflict::ParentCycle: return "ParentCycle";
  }
  return "";
}

bool merge_layers(const Layer& base, const Layer& ours, const Layer& theirs, MergeResult& out,
                  std::string* error) {
  return merge_layers(base, ours, theirs, MergeOptions{}, out, error);
}

bool merge_layers(const Layer& base, const Layer& ours, const Layer& theirs,
                  const MergeOptions& options, MergeResult& out, std::string* error) {
  ENGINE_PROFILE_ZONE_NAMED("doc.merge");
  Merger merger(base, ours, theirs, options, out);
  return merger.run(error);
}

}  // namespace engine::doc

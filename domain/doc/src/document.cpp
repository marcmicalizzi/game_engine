#include <core/base/assert.h>
#include <core/containers/flat_set.h>
#include <core/json/json.h>
#include <core/schema/type_info.h>
#include <core/time/time.h>
#include <domain/doc/document.h>

#include <memory>

namespace engine::doc {

// --- helpers ------------------------------------------------------------------------------------

std::string describe_path(std::string_view layer, ObjectId id, std::string_view property) {
  char hex[33];
  id.to_hex(hex);
  std::string out(layer);
  out.push_back('/');
  out.append(hex, 32);
  if (!property.empty()) {
    out.push_back('/');
    out.append(property.data(), property.size());
  }
  return out;
}

namespace {

void diag(Vector<Diagnostic>* out, std::string path, std::string message) {
  if (out != nullptr) out->push_back(Diagnostic{std::move(path), std::move(message)});
}

bool is_override_only(const ObjectRecord& r) noexcept {
  return r.type.empty() && !r.parent.has_value() && !r.deleted && r.properties.empty();
}

}  // namespace

Command cmd_create(ObjectId id, std::string type, ObjectId parent, JsonValue initial_properties) {
  Command c;
  c.kind = CommandKind::CreateObject;
  c.id = id;
  c.type = std::move(type);
  c.parent = parent;
  c.value = std::move(initial_properties);
  return c;
}

Command cmd_delete(ObjectId id) {
  Command c;
  c.kind = CommandKind::DeleteObject;
  c.id = id;
  return c;
}

Command cmd_set(ObjectId id, std::string name, JsonValue value) {
  Command c;
  c.kind = CommandKind::SetProperty;
  c.id = id;
  c.name = std::move(name);
  c.value = std::move(value);
  return c;
}

Command cmd_clear(ObjectId id, std::string name) {
  Command c;
  c.kind = CommandKind::ClearProperty;
  c.id = id;
  c.name = std::move(name);
  return c;
}

Command cmd_set_parent(ObjectId id, ObjectId parent) {
  Command c;
  c.kind = CommandKind::SetParent;
  c.id = id;
  c.parent = parent;
  return c;
}

Command cmd_remove_record(ObjectId id) {
  Command c;
  c.kind = CommandKind::RemoveRecord;
  c.id = id;
  return c;
}

Command cmd_restore(ObjectId id, std::optional<ObjectRecord> record) {
  Command c;
  c.kind = CommandKind::RestoreRecord;
  c.id = id;
  c.record = std::move(record);
  return c;
}

// --- Layer --------------------------------------------------------------------------------------

ObjectRecord& Layer::ensure(ObjectId id) {
  auto [it, inserted] = records_.try_emplace(id);
  if (inserted) it->second.id = id;
  return it->second;
}

void Layer::set(ObjectRecord record) {
  const ObjectId id = record.id;
  records_.insert_or_assign(id, std::move(record));
}

LayerFile Layer::to_file() const {
  LayerFile file;
  file.name = name_;
  file.role = role_;
  file.objects.reserve(records_.size());
  for (auto [id, record] : records_)
    file.objects.push_back(record);
  return file;
}

Layer Layer::from_file(LayerFile&& file) {
  Layer layer(std::move(file.name), file.role);
  for (ObjectRecord& r : file.objects) {
    const ObjectId id = r.id;
    layer.records_.insert_or_assign(id, std::move(r));
  }
  return layer;
}

std::string Layer::to_json_text() const { return write_json(schema::to_json(to_file())); }

bool Layer::from_json_text(std::string_view text, Layer& out, schema::ReadContext& ctx) {
  JsonValue json;
  const JsonParseResult r = parse_json(text, json);
  if (!r.ok) {
    ctx.error(std::string("invalid JSON: ") + r.message);
    return false;
  }
  LayerFile file;
  if (!schema::from_json(file, json, ctx)) return false;
  out = from_file(std::move(file));
  return true;
}

// --- Document: layers ---------------------------------------------------------------------------

u32 Document::add_layer(std::string name, LayerRole role) {
  return add_layer(Layer(std::move(name), role));
}

u32 Document::add_layer(Layer&& layer) {
  layers_.push_back(std::move(layer));
  edit_layer_ = layers_.size() - 1;
  return edit_layer_;
}

i32 Document::find_layer(std::string_view name) const noexcept {
  for (u32 i = 0; i < layers_.size(); ++i) {
    if (layers_[i].name() == name) return static_cast<i32>(i);
  }
  return -1;
}

void Document::set_edit_layer(u32 index) noexcept {
  ENGINE_ASSERT(index < layers_.size(), "Document::set_edit_layer: index out of range");
  edit_layer_ = index;
}

// --- Document: composition ----------------------------------------------------------------------

bool Document::is_defined(ObjectId id) const noexcept {
  for (const Layer& layer : layers_) {
    const ObjectRecord* r = layer.find(id);
    if (r != nullptr && !r->type.empty()) return true;
  }
  return false;
}

bool Document::resolve(ObjectId id, ResolvedObject& out) const {
  out = ResolvedObject{};
  out.id = id;
  bool defined = false;
  u32 defining = 0;
  for (u32 i = 0; i < layers_.size(); ++i) {
    const ObjectRecord* r = layers_[i].find(id);
    if (r != nullptr && !r->type.empty()) {
      defined = true;
      defining = i;
      out.type = r->type;
    }
  }
  if (!defined) return false;
  out.defining_layer = defining;
  for (u32 i = 0; i < layers_.size(); ++i) {
    const ObjectRecord* r = layers_[i].find(id);
    if (r == nullptr) continue;
    if (r->parent.has_value()) out.parent = *r->parent;
    if (r->deleted && i >= defining) out.deleted = true;
    for (auto [name, value] : r->properties)
      out.properties.insert_or_assign(std::string_view(name), &value);
  }
  return true;
}

bool Document::exists(ObjectId id) const noexcept {
  ResolvedObject r;
  return resolve(id, r) && !r.deleted;
}

const JsonValue* Document::property(ObjectId id, std::string_view name) const noexcept {
  const JsonValue* found = nullptr;
  bool defined = false;
  for (const Layer& layer : layers_) {
    const ObjectRecord* r = layer.find(id);
    if (r == nullptr) continue;
    if (!r->type.empty()) defined = true;
    if (const JsonValue* v = r->properties.find_value(name)) found = v;
  }
  return defined ? found : nullptr;
}

Vector<ObjectId> Document::objects() const {
  FlatSet<ObjectId> ids;
  for (const Layer& layer : layers_) {
    for (auto [id, record] : layer.records()) {
      if (!record.type.empty()) ids.insert(id);
    }
  }
  Vector<ObjectId> out;
  out.reserve(ids.size());
  for (ObjectId id : ids) {
    if (exists(id)) out.push_back(id);
  }
  return out;
}

Vector<ObjectId> Document::children(ObjectId parent) const {
  Vector<ObjectId> out;
  for (ObjectId id : objects()) {
    ResolvedObject r;
    if (resolve(id, r) && r.parent == parent) out.push_back(id);
  }
  return out;
}

bool Document::would_cycle(ObjectId id, ObjectId new_parent) const {
  ObjectId cursor = new_parent;
  u32 hops = 0;
  const u32 limit = static_cast<u32>(objects().size()) + 1;
  while (!cursor.is_null()) {
    if (cursor == id) return true;
    ResolvedObject r;
    if (!resolve(cursor, r)) return false;
    cursor = r.parent;
    if (++hops > limit) return true;  // an existing cycle counts as a cycle
  }
  return false;
}

void Document::prune(Layer& layer, ObjectId id) noexcept {
  const ObjectRecord* r = layer.find(id);
  if (r != nullptr && is_override_only(*r)) layer.remove(id);
}

// --- Document: mutation -------------------------------------------------------------------------

bool Document::apply(const Command& cmd, Command* inverse, Vector<Diagnostic>* diagnostics,
                     bool strict) {
  ENGINE_VERIFY(edit_layer_ < layers_.size(), "Document::apply: no layers");
  Layer& layer = layers_[edit_layer_];
  const std::string& layer_name = layer.name();

  auto fail = [&](const char* message) {
    diag(diagnostics, describe_path(layer_name, cmd.id, cmd.name), message);
    return false;
  };
  auto snapshot_inverse = [&]() {
    if (inverse == nullptr) return;
    *inverse = Command{};
    inverse->kind = CommandKind::RestoreRecord;
    inverse->id = cmd.id;
    if (const ObjectRecord* r = layer.find(cmd.id)) inverse->record = *r;
  };

  if (cmd.id.is_null()) return fail("command needs an object id");

  switch (cmd.kind) {
    case CommandKind::CreateObject: {
      if (cmd.type.empty()) return fail("CreateObject requires a type");
      if (strict && is_defined(cmd.id)) return fail("object already exists");
      const ObjectId parent = cmd.parent.value_or(ObjectId{});
      if (strict && !parent.is_null() && !exists(parent)) return fail("parent does not exist");
      if (!cmd.value.is_null() && !cmd.value.is_object())
        return fail("initial properties must be an object");
      snapshot_inverse();
      ObjectRecord& r = layer.ensure(cmd.id);
      r.type = cmd.type;
      r.parent = parent;
      r.deleted = false;
      if (cmd.value.is_object()) {
        for (auto [name, value] : cmd.value.as_object())
          r.properties.insert_or_assign(name, value);
      }
      return true;
    }
    case CommandKind::DeleteObject: {
      if (strict && !exists(cmd.id)) return fail("object does not exist");
      snapshot_inverse();
      ObjectRecord* r = layer.find(cmd.id);
      if (r != nullptr && !r->type.empty()) {
        layer.remove(cmd.id);  // the edit layer defines it: drop the definition
      } else {
        layer.ensure(cmd.id).deleted = true;  // defined below: tombstone
      }
      return true;
    }
    case CommandKind::SetProperty: {
      if (cmd.name.empty()) return fail("SetProperty requires a property name");
      if (strict && !exists(cmd.id)) return fail("object does not exist");
      snapshot_inverse();
      layer.ensure(cmd.id).properties.insert_or_assign(cmd.name, cmd.value);
      return true;
    }
    case CommandKind::ClearProperty: {
      if (cmd.name.empty()) return fail("ClearProperty requires a property name");
      snapshot_inverse();
      if (ObjectRecord* r = layer.find(cmd.id)) {
        r->properties.erase(cmd.name);
        prune(layer, cmd.id);
      }
      return true;
    }
    case CommandKind::SetParent: {
      const ObjectId parent = cmd.parent.value_or(ObjectId{});
      if (strict) {
        if (!exists(cmd.id)) return fail("object does not exist");
        if (!parent.is_null() && !exists(parent)) return fail("parent does not exist");
        if (would_cycle(cmd.id, parent)) return fail("parent would create a cycle");
      }
      snapshot_inverse();
      layer.ensure(cmd.id).parent = parent;
      return true;
    }
    case CommandKind::RemoveRecord: {
      snapshot_inverse();
      layer.remove(cmd.id);
      return true;
    }
    case CommandKind::RestoreRecord: {
      snapshot_inverse();
      if (cmd.record.has_value()) {
        ObjectRecord r = *cmd.record;
        r.id = cmd.id;
        layer.set(std::move(r));
      } else {
        layer.remove(cmd.id);
      }
      return true;
    }
  }
  return fail("unknown command kind");
}

Transaction Document::begin(Attribution attribution) {
  return Transaction(*this, std::move(attribution));
}

bool Document::undo(const Patch& patch) {
  const i32 idx = find_layer(patch.layer);
  if (idx < 0) return false;
  const u32 previous = edit_layer_;
  edit_layer_ = static_cast<u32>(idx);
  bool ok = true;
  for (u32 i = patch.inverse.size(); i > 0; --i)
    ok = apply(patch.inverse[i - 1], nullptr, nullptr, false) && ok;
  edit_layer_ = previous;
  return ok;
}

bool Document::redo(const Patch& patch) {
  const i32 idx = find_layer(patch.layer);
  if (idx < 0) return false;
  const u32 previous = edit_layer_;
  edit_layer_ = static_cast<u32>(idx);
  bool ok = true;
  for (const Command& c : patch.forward)
    ok = apply(c, nullptr, nullptr, false) && ok;
  edit_layer_ = previous;
  return ok;
}

// --- Document: validation -----------------------------------------------------------------------

bool Document::validate(Vector<Diagnostic>& out) const {
  const u32 before = out.size();
  const schema::Registry& registry = schema::Registry::global();

  // Definition uniqueness and type existence, per record.
  FlatMap<ObjectId, u32> definitions;
  for (const Layer& layer : layers_) {
    for (auto [id, record] : layer.records()) {
      if (record.type.empty()) continue;
      definitions[id] += 1;
      const schema::TypeInfo* info = registry.find(record.type);
      if (info == nullptr || info->kind != schema::Kind::Struct) {
        out.push_back(
            {describe_path(layer.name(), id), "unknown object type '" + record.type + "'"});
      }
    }
  }
  for (auto [id, count] : definitions) {
    if (count > 1)
      out.push_back({describe_path("*", id), "object is defined in more than one layer"});
  }

  // Overrides for objects nobody defines.
  for (const Layer& layer : layers_) {
    for (auto [id, record] : layer.records()) {
      if (record.type.empty() && !definitions.contains(id)) {
        out.push_back(
            {describe_path(layer.name(), id), "override record for an object no layer defines"});
      }
    }
  }

  // Composed checks per defined object.
  for (auto [id, count] : definitions) {
    ResolvedObject r;
    if (!resolve(id, r)) continue;
    const schema::TypeInfo* info = registry.find(r.type);

    if (!r.parent.is_null()) {
      if (!exists(r.parent)) {
        out.push_back(
            {describe_path(layers_[r.defining_layer].name(), id), "parent does not exist"});
      } else if (would_cycle(id, r.parent)) {
        out.push_back(
            {describe_path(layers_[r.defining_layer].name(), id), "parent chain forms a cycle"});
      }
    }

    if (info == nullptr || info->kind != schema::Kind::Struct) continue;  // already reported
    // Validate each layer's property values against the type, so the diagnostic names the layer.
    alignas(64) std::byte storage[1024];
    std::unique_ptr<std::byte[]> heap;
    std::byte* object = storage;
    if (info->size > sizeof(storage)) {
      heap = std::make_unique<std::byte[]>(info->size + 64);
      object = heap.get();
    }
    info->ops->construct(object);
    for (const Layer& layer : layers_) {
      const ObjectRecord* rec = layer.find(id);
      if (rec == nullptr) continue;
      for (auto [name, value] : rec->properties) {
        const schema::FieldInfo* field = info->find_field(name);
        if (field == nullptr) {
          out.push_back({describe_path(layer.name(), id, name),
                         "unknown property for type '" + std::string(r.type) + "'"});
          continue;
        }
        if ((field->flags & schema::FieldFlag::transient) != 0) {
          out.push_back({describe_path(layer.name(), id, name),
                         "property is transient and cannot be authored"});
          continue;
        }
        schema::ReadContext ctx;
        if (!schema::from_json(field->type, object + field->offset, value, ctx)) {
          for (const Diagnostic& d : ctx.diagnostics) {
            out.push_back(
                {describe_path(layer.name(), id, name) + (d.path.empty() ? "" : "." + d.path),
                 d.message});
          }
        }
      }
    }
    info->ops->destroy(object);
  }
  return out.size() == before;
}

// --- Transaction --------------------------------------------------------------------------------

Transaction::Transaction(Document& doc, Attribution attribution) : doc_(&doc) {
  if (attribution.timestamp_unix_ms == 0) attribution.timestamp_unix_ms = time::wall_unix_ms();
  patch_.attribution = std::move(attribution);
  patch_.layer = doc.layer(doc.edit_layer()).name();
}

Transaction::~Transaction() {
  if (!finished_) rollback();
}

bool Transaction::apply(const Command& command, bool strict) {
  ENGINE_ASSERT(!finished_, "Transaction::apply after commit or rollback");
  Command inverse;
  if (!doc_->apply(command, &inverse, &diagnostics_, strict)) return false;
  patch_.forward.push_back(command);
  patch_.inverse.push_back(std::move(inverse));
  return true;
}

bool Transaction::commit() {
  if (finished_) return false;
  finished_ = true;
  if (patch_.forward.empty()) return true;  // nothing to record
  doc_->commit(Patch(patch_));
  return true;
}

void Transaction::rollback() {
  if (finished_) return;
  finished_ = true;
  const u32 previous = doc_->edit_layer();
  const i32 idx = doc_->find_layer(patch_.layer);
  if (idx >= 0) doc_->set_edit_layer(static_cast<u32>(idx));
  for (u32 i = patch_.inverse.size(); i > 0; --i)
    doc_->apply(patch_.inverse[i - 1], nullptr, nullptr, false);
  doc_->set_edit_layer(previous);
  patch_.forward.clear();
  patch_.inverse.clear();
}

// --- diff ---------------------------------------------------------------------------------------

Vector<RecordDiff> diff_records(const Layer& from, const Layer& to) {
  Vector<RecordDiff> out;
  const auto& a = from.records();
  const auto& b = to.records();
  u32 i = 0, j = 0;
  while (i < a.size() || j < b.size()) {
    if (j >= b.size() || (i < a.size() && a.key_at(i) < b.key_at(j))) {
      RecordDiff d;
      d.id = a.key_at(i);
      d.removed = true;
      out.push_back(std::move(d));
      ++i;
      continue;
    }
    if (i >= a.size() || b.key_at(j) < a.key_at(i)) {
      RecordDiff d;
      d.id = b.key_at(j);
      d.added = true;
      out.push_back(std::move(d));
      ++j;
      continue;
    }
    const ObjectId id = a.key_at(i);
    const ObjectRecord& ra = a.value_at(i);
    const ObjectRecord& rb = b.value_at(j);
    ++i;
    ++j;
    if (ra == rb) continue;
    RecordDiff d;
    d.id = id;
    if (ra.type != rb.type) {
      d.type_changed = true;
      d.type = rb.type;
    }
    if (ra.parent != rb.parent) {
      d.parent_changed = true;
      d.parent = rb.parent;
    }
    if (ra.deleted != rb.deleted) {
      d.deleted_changed = true;
      d.deleted = rb.deleted;
    }
    // Properties in name order, walking the two sorted maps together.
    const auto& pa = ra.properties;
    const auto& pb = rb.properties;
    u32 x = 0, y = 0;
    while (x < pa.size() || y < pb.size()) {
      if (y >= pb.size() || (x < pa.size() && pa.key_at(x) < pb.key_at(y))) {
        PropertyChange p;
        p.name = pa.key_at(x);
        p.removed = true;
        d.properties.push_back(std::move(p));
        ++x;
        continue;
      }
      if (x >= pa.size() || pb.key_at(y) < pa.key_at(x)) {
        PropertyChange p;
        p.name = pb.key_at(y);
        p.value = pb.value_at(y);
        d.properties.push_back(std::move(p));
        ++y;
        continue;
      }
      if (!(pa.value_at(x) == pb.value_at(y))) {
        PropertyChange p;
        p.name = pb.key_at(y);
        p.value = pb.value_at(y);
        d.properties.push_back(std::move(p));
      }
      ++x;
      ++y;
    }
    out.push_back(std::move(d));
  }
  return out;
}

Vector<Command> diff_layers(const Layer& from, const Layer& to) {
  Vector<Command> out;
  for (const RecordDiff& d : diff_records(from, to)) {
    if (d.removed) {
      out.push_back(cmd_remove_record(d.id));
      continue;
    }
    // No command expresses a changed type or deletion flag, or a parent override that `to`
    // drops, so those (and an added record) restore the record whole.
    if (d.added || d.type_changed || d.deleted_changed ||
        (d.parent_changed && !d.parent.has_value())) {
      out.push_back(cmd_restore(d.id, *to.find(d.id)));
      continue;
    }
    if (d.parent_changed) out.push_back(cmd_set_parent(d.id, *d.parent));
    // Property changes in key order: removals first, then sets.
    for (const PropertyChange& p : d.properties) {
      if (p.removed) out.push_back(cmd_clear(d.id, p.name));
    }
    for (const PropertyChange& p : d.properties) {
      if (!p.removed) out.push_back(cmd_set(d.id, p.name, p.value));
    }
  }
  return out;
}

}  // namespace engine::doc

#include "editor_document.h"

#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/schema/json_reflect.h>

#include <algorithm>
#include <cstddef>
#include <new>

namespace engine::editor {

std::string hex(Id128 id) {
  char text[33];
  id.to_hex(text);
  return std::string(text);
}

std::string kind_name(const schema::TypeRef& type) {
  using schema::Kind;
  switch (type.kind) {
    case Kind::Bool: return "bool";
    case Kind::U8: return "u8";
    case Kind::U16: return "u16";
    case Kind::U32: return "u32";
    case Kind::U64: return "u64";
    case Kind::I8: return "i8";
    case Kind::I16: return "i16";
    case Kind::I32: return "i32";
    case Kind::I64: return "i64";
    case Kind::F32: return "f32";
    case Kind::F64: return "f64";
    case Kind::String: return "string";
    case Kind::Bytes: return "bytes";
    case Kind::Id128: return "id128";
    case Kind::Vec2: return "vec2";
    case Kind::Vec3: return "vec3";
    case Kind::Vec4: return "vec4";
    case Kind::Quat: return "quat";
    case Kind::Json: return "json";
    case Kind::WorldPos: return "worldpos";
    case Kind::DVec3: return "dvec3";
    case Kind::Enum:
    case Kind::Struct: return type.type != nullptr ? type.type->name : "?";
    case Kind::Optional: return (type.element ? kind_name(*type.element) : "?") + "?";
    case Kind::Array: return (type.element ? kind_name(*type.element) : "?") + "[]";
    case Kind::FixedArray:
      return (type.element ? kind_name(*type.element) : "?") + "[" +
             std::to_string(type.fixed_count) + "]";
    case Kind::Map:
      return "map<" + (type.key ? kind_name(*type.key) : std::string("?")) + ", " +
             (type.element ? kind_name(*type.element) : std::string("?")) + ">";
  }
  return "?";
}

namespace {

// A default-constructed instance of a schema struct, where a field's default lives (the same idea
// as `schema.describe`'s, domain/protocol/src/methods.cpp): the panel shows an unset field's
// default in the form a caller would write it.
class DefaultObject {
 public:
  explicit DefaultObject(const schema::TypeInfo& type) : type_(type) {
    if (type.ops == nullptr || type.ops->construct == nullptr || type.size == 0) return;
    align_ = std::max<usize>(type.align, alignof(std::max_align_t));
    storage_ = ::operator new(type.size, std::align_val_t{align_});
    type.ops->construct(storage_);
  }
  ~DefaultObject() {
    if (storage_ == nullptr) return;
    if (type_.ops->destroy != nullptr) type_.ops->destroy(storage_);
    ::operator delete(storage_, std::align_val_t{align_});
  }
  ENGINE_NON_COPYABLE(DefaultObject);
  const void* field(const schema::FieldInfo& f) const noexcept {
    return storage_ != nullptr ? static_cast<const char*>(storage_) + f.offset : nullptr;
  }

 private:
  const schema::TypeInfo& type_;
  void* storage_ = nullptr;
  usize align_ = 0;
};

std::string compact(const JsonValue& value) {
  JsonWriteOptions options;
  options.pretty = false;
  return write_json(value, options);
}

std::string name_of(const doc::Document& document, Id128 id) {
  const JsonValue* name = document.property(id, "name");
  return name != nullptr && name->is_string() ? std::string(name->as_string()) : std::string();
}

void walk(const doc::Document& document, Id128 parent, u32 depth, Vector<OutlinerRow>& out) {
  // `children` is sorted by id, which is the outliner's sibling order: stable across sessions and
  // independent of the order the records were made in.
  for (const Id128 id : document.children(parent)) {
    OutlinerRow row;
    row.id = id;
    row.type = std::string(document.type_of(id));
    row.name = name_of(document, id);
    row.depth = depth;
    const u32 at = static_cast<u32>(out.size());
    out.push_back(std::move(row));
    walk(document, id, depth + 1, out);
    out[at].children = static_cast<u32>(document.children(id).size());
  }
}

}  // namespace

bool EditorDocument::open(const std::string& dir, bool create, const std::string& actor,
                          std::string& error) {
  sessions_ = std::make_unique<protocol::SessionManager>(vfs_);
  protocol::SessionOpenParams params;
  params.path = dir;
  params.create = create;
  protocol::RpcError rpc;
  session_ = sessions_->open(params, rpc);
  if (session_ == nullptr) {
    error = dir + ": " + rpc.message;
    return false;
  }
  dir_ = dir;
  caller_ = protocol::Caller{};
  caller_.who.actor = actor.empty() ? std::string("editor") : actor;
  caller_.who.role = "director";
  caller_.who.task = "editor";
  caller_.role =
      nullptr;  // no role policy: the session's own checks, as engine-host without --roles
  return true;
}

const doc::Document& EditorDocument::document() const { return session_->document(); }

void EditorDocument::outliner(Vector<OutlinerRow>& out) const {
  out.clear();
  walk(document(), Id128{}, 0, out);
}

bool EditorDocument::properties(Id128 id, Vector<PropertyRow>& out) const {
  out.clear();
  const doc::Document& d = document();
  if (id.is_null() || !d.exists(id)) return false;
  const schema::TypeInfo* type = schema::Registry::global().find(d.type_of(id));
  Vector<std::string> declared;
  if (type != nullptr && type->kind == schema::Kind::Struct) {
    const DefaultObject defaults(*type);
    for (const schema::FieldInfo& field : type->fields) {
      if ((field.flags & schema::FieldFlag::transient) != 0) continue;
      PropertyRow row;
      row.name = field.name;
      row.kind = kind_name(field.type);
      declared.push_back(row.name);
      if (const JsonValue* value = d.property(id, field.name)) {
        row.value = compact(*value);
        row.set = true;
      } else if (const void* object = defaults.field(field)) {
        JsonValue fallback;
        if (schema::to_json(field.type, object, fallback)) row.value = compact(fallback);
      }
      out.push_back(std::move(row));
    }
  }
  // What the record holds that its type does not declare (or every property, for a type the
  // registry has not): shown, marked, never dropped.
  doc::ResolvedObject object;
  if (d.resolve(id, object)) {
    Vector<PropertyRow> extra;
    for (const auto& entry : object.properties) {
      const std::string name(entry.first);
      if (std::find(declared.begin(), declared.end(), name) != declared.end()) continue;
      PropertyRow row;
      row.name = name;
      row.kind = "undeclared";
      row.value = entry.second != nullptr ? compact(*entry.second) : std::string("null");
      row.set = true;
      extra.push_back(std::move(row));
    }
    std::sort(extra.begin(), extra.end(),
              [](const PropertyRow& a, const PropertyRow& b) { return a.name < b.name; });
    for (PropertyRow& row : extra)
      out.push_back(std::move(row));
  }
  return true;
}

void EditorDocument::journal(Vector<JournalRow>& out, u32 limit) const {
  out.clear();
  const Vector<doc::Patch>& patches = document().journal();
  const u32 position = session_->undo_position();
  for (u32 i = static_cast<u32>(patches.size()); i > 0 && out.size() < limit; --i) {
    const doc::Patch& patch = patches[i - 1];
    JournalRow row;
    row.index = i - 1;
    row.actor = patch.attribution.actor;
    row.role = patch.attribution.role;
    row.task = patch.attribution.task;
    row.rationale = patch.attribution.rationale;
    row.timestamp_unix_ms = patch.attribution.timestamp_unix_ms;
    row.commands = static_cast<u32>(patch.forward.size());
    row.undone = i - 1 >= position;
    out.push_back(std::move(row));
  }
}

bool EditorDocument::apply(std::span<const doc::Command> commands, const std::string& rationale,
                           std::string& error) {
  protocol::Caller caller = caller_;
  caller.who.rationale =
      rationale.empty() ? "edit in engine-editor (" + std::to_string(commands.size()) + " commands)"
                        : rationale;
  protocol::ApplyResult result;
  protocol::RpcError rpc;
  if (!session_->apply(commands, caller, true, std::string_view(), result, rpc)) {
    error = rpc.message;
    return false;
  }
  if (!result.committed) {
    error = "not committed";
    for (const protocol::Diagnostic& d : result.diagnostics)
      error += "; " + d.path + ": " + d.message;
    return false;
  }
  return true;
}

bool EditorDocument::undo(std::string& error) {
  protocol::StepResult result;
  protocol::RpcError rpc;
  if (!session_->undo(1, &caller_, result, rpc)) {
    error = rpc.message;
    return false;
  }
  return true;
}

bool EditorDocument::redo(std::string& error) {
  protocol::StepResult result;
  protocol::RpcError rpc;
  if (!session_->redo(1, &caller_, result, rpc)) {
    error = rpc.message;
    return false;
  }
  return true;
}

}  // namespace engine::editor

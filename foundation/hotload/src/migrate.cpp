// State migration between two builds of a module, through the two builds' schema descriptors.
//
// The old build's state is written as schema JSON by the old descriptor and read back by the new
// one, with unknown fields ignored: the same walker the document model uses for every schema
// object (core/schema/json_reflect.h), so a module's state follows the IDL's evolution rules
// (schemas/README.md) rather than rules of its own. Before any of that, the two descriptors are
// compared field by field, by name, so an incompatible change is refused with the field's path
// instead of whatever the JSON reader would say about the value it happened to hold.

#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <foundation/hotload/module_host.h>

#include <string>

namespace engine::hotload {

namespace {

using schema::FieldFlag;
using schema::FieldInfo;
using schema::Kind;
using schema::TypeInfo;
using schema::TypeRef;

bool allowed_kind(const TypeRef& ref, const std::string& path, std::string& error) {
  switch (ref.kind) {
    case Kind::Bool:
    case Kind::U8:
    case Kind::U16:
    case Kind::U32:
    case Kind::U64:
    case Kind::I8:
    case Kind::I16:
    case Kind::I32:
    case Kind::I64:
    case Kind::F32:
    case Kind::F64:
    case Kind::Id128:
    case Kind::Vec2:
    case Kind::Vec3:
    case Kind::Vec4:
    case Kind::Quat:
    case Kind::WorldPos:
    case Kind::DVec3: return true;
    case Kind::Enum:
      if (ref.type == nullptr) {
        error = "state field '" + path + "' is an enum with no descriptor";
        return false;
      }
      return true;
    case Kind::Struct:
      if (ref.type == nullptr) {
        error = "state field '" + path + "' is a struct with no descriptor";
        return false;
      }
      for (const FieldInfo& f : ref.type->fields) {
        if (!allowed_kind(f.type, path + "." + f.name, error)) return false;
      }
      return true;
    case Kind::FixedArray:
      if (ref.element == nullptr) {
        error = "state field '" + path + "' is a fixed array with no element type";
        return false;
      }
      return allowed_kind(*ref.element, path + "[]", error);
    case Kind::String:
    case Kind::Bytes:
    case Kind::Json:
    case Kind::Optional:
    case Kind::Array:
    case Kind::Map: break;
  }
  error = "state field '" + path + "' is " + schema::kind_name(ref.kind) +
          ", which owns memory; a module's state holds fixed-layout values only "
          "(scalars, vectors, ids, enums, fixed arrays and structs of those)";
  return false;
}

// Whether a value of kind `from` reads into a field of kind `to` without loss of meaning. The
// JSON reader still checks the value (an integer out of the new range is refused there).
enum class Compat { Same, Converts, Incompatible };

Compat compat(const TypeRef& from, const TypeRef& to) {
  if (from.kind == to.kind) {
    if (from.kind == Kind::FixedArray) {
      if (from.fixed_count != to.fixed_count || from.element == nullptr || to.element == nullptr)
        return Compat::Incompatible;
      return compat(*from.element, *to.element);
    }
    return Compat::Same;
  }
  if (schema::is_integer(from.kind) && schema::is_integer(to.kind)) return Compat::Converts;
  if (schema::is_integer(from.kind) && schema::is_float(to.kind)) return Compat::Converts;
  if (schema::is_float(from.kind) && schema::is_float(to.kind)) return Compat::Converts;
  return Compat::Incompatible;
}

std::string describe_kind(const TypeRef& ref) {
  std::string out = schema::kind_name(ref.kind);
  if ((ref.kind == Kind::Struct || ref.kind == Kind::Enum) && ref.type != nullptr)
    out += std::string(" ") + ref.type->qualified_name;
  if (ref.kind == Kind::FixedArray && ref.element != nullptr)
    out = describe_kind(*ref.element) + "[" + std::to_string(ref.fixed_count) + "]";
  return out;
}

bool compare(const TypeInfo& from, const TypeInfo& to, const std::string& prefix,
             ReloadReport& report, std::string& error) {
  for (const FieldInfo& t : to.fields) {
    if ((t.flags & FieldFlag::transient) != 0) continue;
    const FieldInfo* f = from.find_field(t.name);
    const std::string path = prefix + t.name;
    if (f == nullptr || (f->flags & FieldFlag::transient) != 0) {
      report.added.push_back(path);
      continue;
    }
    if (f->type.kind == Kind::Struct && t.type.kind == Kind::Struct && f->type.type != nullptr &&
        t.type.type != nullptr) {
      if (!compare(*f->type.type, *t.type.type, path + ".", report, error)) return false;
      continue;
    }
    switch (compat(f->type, t.type)) {
      case Compat::Same: break;
      case Compat::Converts: report.converted.push_back(path); break;
      case Compat::Incompatible:
        error = "state field '" + path + "' was " + describe_kind(f->type) + " and is now " +
                describe_kind(t.type) + ": the old value cannot be carried; rename the field " +
                "(the old one is then dropped and the new one defaulted) or keep its kind";
        return false;
    }
  }
  for (const FieldInfo& f : from.fields) {
    if ((f.flags & FieldFlag::transient) != 0) continue;
    const FieldInfo* t = to.find_field(f.name);
    if (t == nullptr || (t->flags & FieldFlag::transient) != 0)
      report.removed.push_back(prefix + f.name);
  }
  return true;
}

}  // namespace

bool check_state_type(const TypeInfo& type, std::string& error) {
  if (type.kind != Kind::Struct) {
    error = std::string("state type ") + (type.qualified_name ? type.qualified_name : "?") +
            " is not a struct";
    return false;
  }
  if (type.ops == nullptr || type.ops->construct == nullptr || type.ops->destroy == nullptr) {
    error = std::string("state type ") + type.qualified_name + " has no construct/destroy";
    return false;
  }
  if (type.size == 0 || type.align == 0 || (type.align & (type.align - 1)) != 0) {
    error =
        std::string("state type ") + type.qualified_name + " has an impossible size or alignment";
    return false;
  }
  for (const FieldInfo& f : type.fields) {
    if (!allowed_kind(f.type, f.name, error)) return false;
  }
  return true;
}

bool migrate_state(const TypeInfo& from_type, const void* from, const TypeInfo& to_type, void* to,
                   ReloadReport& report, std::string& error) {
  report.added.clear();
  report.removed.clear();
  report.converted.clear();
  report.state_bytes = 0;
  if (std::string_view(from_type.qualified_name) != std::string_view(to_type.qualified_name)) {
    error = std::string("the state type was ") + from_type.qualified_name + " and is now " +
            to_type.qualified_name + "; a module keeps one state type for its life";
    return false;
  }
  if (!compare(from_type, to_type, "", report, error)) return false;

  // The bytes handed across: schema JSON text, what a save of the state would write.
  JsonValue json;
  if (!schema::to_json(from_type, from, json)) {
    error = "the old build's state did not serialize (a NaN or an infinity in a float field)";
    return false;
  }
  const std::string text = write_json(json, JsonWriteOptions{.pretty = false});
  report.state_bytes = text.size();
  JsonValue parsed;
  const JsonParseResult parse = parse_json(text, parsed);
  if (!parse.ok) {
    error = std::string("the serialized state did not parse: ") + parse.message;
    return false;
  }
  schema::ReadContext context;
  context.options.ignore_unknown_fields = true;
  if (!schema::from_json(to_type, to, parsed, context) || !context.ok()) {
    if (context.diagnostics.empty()) {
      error = "the new build could not read the old build's state";
    } else {
      const schema::Diagnostic& d = context.diagnostics.front();
      error = "state field '" + d.path + "': " + d.message;
    }
    return false;
  }
  return true;
}

}  // namespace engine::hotload

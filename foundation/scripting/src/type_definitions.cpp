// The typed API: a Luau definition file generated from the schema descriptors (type_definitions.h).

#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <core/schema/type_info.h>
#include <foundation/scripting/type_definitions.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

namespace engine::scripting {

namespace {

constexpr const char* k_engine_api =
    R"(-- The engine API every script sees as the global `engine` (foundation/scripting).
export type LogLevel = "trace" | "debug" | "info" | "warn" | "error"
export type LogFields = { [string]: boolean | number | string }
export type TunableValue = number | boolean | string

declare engine: {
    -- A record in the `scripting` log category, with this script's chunk and line as fields.
    log: (level: LogLevel, message: string, fields: LogFields?) -> (),
    -- A tunable's current value; read it once, not inside a loop.
    tunable: (name: string) -> TunableValue,
    -- The fixed-step tick the host is running, and its step in seconds. Never the wall clock.
    tick: () -> number,
    fixed_step: () -> number,
    -- The index-th number in [0, 1) of the stream named by seed: the only randomness there is.
    random: (seed: number, index: number) -> number,
    -- The object the host knows by this id (32 hex characters), for this call, or nil.
    object: (id: string) -> any,
}
)";

constexpr const char* k_keywords[] = {
    "and",   "break", "do",  "else", "elseif", "end",    "false", "for",  "function", "if",    "in",
    "local", "nil",   "not", "or",   "repeat", "return", "then",  "true", "until",    "while",
};

bool is_keyword(std::string_view name) noexcept {
  for (const char* keyword : k_keywords) {
    if (name == keyword) return true;
  }
  return false;
}

// One `--` line per line of a schema doc comment.
void append_doc(std::string& out, const char* doc, std::string_view indent) {
  if (doc == nullptr || doc[0] == '\0') return;
  std::string_view text(doc);
  while (!text.empty()) {
    const usize end = text.find('\n');
    const std::string_view line = text.substr(0, end);
    out.append(indent);
    out.append("-- ");
    out.append(line);
    out.push_back('\n');
    if (end == std::string_view::npos) break;
    text.remove_prefix(end + 1);
  }
}

struct Emitter {
  Vector<const schema::TypeInfo*> enums;
  Vector<const schema::TypeInfo*> structs;      // dependency order: a struct after what it contains
  HashMap<const schema::TypeInfo*, u8> state;   // 1 visiting, 2 done
  HashMap<std::string_view, u32> simple_names;  // how many emitted types share a simple name

  void visit_ref(const schema::TypeRef& ref) {
    switch (ref.kind) {
      case schema::Kind::Enum:
      case schema::Kind::Struct: visit(*ref.type); return;
      case schema::Kind::Optional:
      case schema::Kind::Array:
      case schema::Kind::FixedArray: visit_ref(*ref.element); return;
      case schema::Kind::Map:
        visit_ref(*ref.key);
        visit_ref(*ref.element);
        return;
      default: return;
    }
  }

  void visit(const schema::TypeInfo& type) {
    if (state.contains(&type)) return;
    state.insert(&type, u8{1});
    if (type.kind == schema::Kind::Enum) {
      enums.push_back(&type);
    } else {
      for (const schema::FieldInfo& field : type.fields)
        visit_ref(field.type);
      structs.push_back(&type);
    }
    *state.find_value(&type) = 2;
    u32* count = simple_names.find_value(std::string_view(type.name));
    if (count != nullptr) {
      ++*count;
    } else {
      simple_names.insert(std::string_view(type.name), 1u);
    }
  }

  std::string luau_name(const schema::TypeInfo& type) const {
    const u32* count = simple_names.find_value(std::string_view(type.name));
    if (count == nullptr || *count <= 1) return type.name;
    std::string name = type.qualified_name;
    std::replace(name.begin(), name.end(), '.', '_');
    return name;
  }

  void append_type(std::string& out, const schema::TypeRef& ref) const {
    using schema::Kind;
    switch (ref.kind) {
      case Kind::Bool: out.append("boolean"); return;
      case Kind::U8:
      case Kind::U16:
      case Kind::U32:
      case Kind::U64:
      case Kind::I8:
      case Kind::I16:
      case Kind::I32:
      case Kind::I64:
      case Kind::F32:
      case Kind::F64: out.append("number"); return;
      case Kind::String:
      case Kind::Bytes:
      case Kind::Id128: out.append("string"); return;
      case Kind::Vec2:
      case Kind::Vec3: out.append("vector"); return;
      case Kind::Vec4:
      case Kind::Quat:
        out.append("{ read x: number, read y: number, read z: number, read w: number }");
        return;
      case Kind::Json: out.append("any"); return;
      case Kind::Enum:
      case Kind::Struct: out.append(luau_name(*ref.type)); return;
      case Kind::Optional:
        append_type(out, *ref.element);
        out.push_back('?');
        return;
      case Kind::Array:
      case Kind::FixedArray:
        out.append("{ ");
        append_type(out, *ref.element);
        out.append(" }");
        return;
      case Kind::Map:
        out.append("{ [");
        append_type(out, *ref.key);
        out.append("]: ");
        append_type(out, *ref.element);
        out.append(" }");
        return;
    }
    out.append("any");
  }

  void append_enum(std::string& out, const schema::TypeInfo& type) const {
    out.append("-- ");
    out.append(type.qualified_name);
    out.push_back('\n');
    append_doc(out, type.doc, "");
    out.append("export type ");
    out.append(luau_name(type));
    out.append(" = ");
    if (type.values.empty()) {
      out.append("never\n\n");
      return;
    }
    for (usize i = 0; i < type.values.size(); ++i) {
      if (i != 0) out.append(" | ");
      out.push_back('"');
      out.append(type.values[i].name);
      out.push_back('"');
    }
    out.append("\n\n");
  }

  void append_struct(std::string& out, const schema::TypeInfo& type) const {
    out.append("-- ");
    out.append(type.qualified_name);
    out.append(" (read-only: a script reads the object in place and returns data)\n");
    append_doc(out, type.doc, "");
    out.append("declare extern type ");
    out.append(luau_name(type));
    out.append(" with\n");
    for (const schema::FieldInfo& field : type.fields) {
      append_doc(out, field.doc, "    ");
      if ((field.flags & schema::FieldFlag::deprecated) != 0) out.append("    -- deprecated\n");
      if (is_keyword(field.name)) {
        // `read` does not combine with a bracketed name; the run time still refuses the write.
        out.append("    [\"");
        out.append(field.name);
        out.append("\"]: ");
      } else {
        out.append("    read ");
        out.append(field.name);
        out.append(": ");
      }
      append_type(out, field.type);
      out.push_back('\n');
    }
    out.append("end\n\n");
  }
};

}  // namespace

std::string emit_engine_api_definitions() { return k_engine_api; }

std::string emit_type_definitions(std::span<const schema::TypeInfo* const> types) {
  // Roots in qualified-name order, so the output depends on the set of types and not on the order
  // the caller listed them in.
  Vector<const schema::TypeInfo*> roots;
  for (const schema::TypeInfo* type : types) {
    if (type != nullptr) roots.push_back(type);
  }
  std::sort(roots.begin(), roots.end(), [](const schema::TypeInfo* a, const schema::TypeInfo* b) {
    return std::strcmp(a->qualified_name, b->qualified_name) < 0;
  });
  Emitter emitter;
  for (const schema::TypeInfo* type : roots)
    emitter.visit(*type);
  std::sort(emitter.enums.begin(), emitter.enums.end(),
            [](const schema::TypeInfo* a, const schema::TypeInfo* b) {
              return std::strcmp(a->qualified_name, b->qualified_name) < 0;
            });

  std::string out;
  out.append("-- Generated by foundation/scripting (emit_type_definitions) from the schema IDL.\n");
  out.append(
      "-- Do not edit: regenerate it. Load it into Luau's analyser as a definition file.\n\n");
  out.append(k_engine_api);
  out.push_back('\n');
  for (const schema::TypeInfo* type : emitter.enums)
    emitter.append_enum(out, *type);
  for (const schema::TypeInfo* type : emitter.structs)
    emitter.append_struct(out, *type);
  return out;
}

}  // namespace engine::scripting

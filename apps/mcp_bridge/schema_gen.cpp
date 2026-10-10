// Tool input schemas, generated from the host's `engine.methods` and `schema.describe`
// (docs/subsystems/apps.md, "engine-mcp"). The engine's schema types are the one source of truth
// for every parameter a tool takes: the field names, their types, their documentation and their
// defaults all come from the IDL through the running host. The bridge adds only what the protocol
// cannot know — which fields a tool hides or requires, the fields it adds of its own, and prose.
#include "bridge.h"

#include <core/json/json.h>

#include <charconv>
#include <limits>

namespace engine::mcp {

// A member type_schema_at puts on every struct's schema while a tool's schema is built, naming the
// type; share_definitions reads it and removes it, so no client ever sees it.
constexpr std::string_view k_type_marker = "x-engine-type";

namespace {

// Descriptions come from `///` comments, which are wrapped at 100 columns; a JSON Schema
// description is one paragraph.
std::string reflow(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text)
    out.push_back(c == '\n' ? ' ' : c);
  return out;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && s.front() == ' ')
    s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ')
    s.remove_suffix(1);
  return s;
}

JsonValue typed(const char* type) {
  JsonValue s = JsonValue::object();
  s.set("type", JsonValue(type));
  return s;
}

JsonValue integer(i64 minimum, u64 maximum) {
  JsonValue s = typed("integer");
  s.set("minimum", JsonValue(minimum));
  if (maximum != 0) s.set("maximum", JsonValue(maximum));
  return s;
}

JsonValue number_array(u32 n) {
  JsonValue s = typed("array");
  s.set("items", typed("number"));
  s.set("minItems", JsonValue(n));
  s.set("maxItems", JsonValue(n));
  return s;
}

// A float field's default as its shortest decimal: an f32 widened to double reads 0.02 as
// 0.019999999552965164, which is the same number and a worse sentence.
JsonValue tidy_f32(const JsonValue& value) {
  f64 wide = 0.0;
  if (!value.is_float() || !value.get_f64(wide)) return value;
  char text[64];
  const std::to_chars_result r = std::to_chars(text, text + sizeof(text), static_cast<f32>(wide));
  if (r.ec != std::errc()) return value;
  f64 narrow = 0.0;
  const std::from_chars_result back = std::from_chars(text, r.ptr, narrow);
  return back.ec == std::errc() ? JsonValue(narrow) : value;
}

// Whether a field's default is worth stating. A scalar is; an empty list, an empty object or null
// is what leaving the field out means anyway, and a struct's defaults are on its own properties.
// So is an empty string, which is the value-initialized default of every string the schema gave no
// default (a session id, a path, a type name) and would read as a suggestion to send "".
bool worth_stating(const JsonValue& v, std::string_view type) {
  if (v.is_bool() || v.is_number()) return true;
  std::string_view s;
  if (!v.get_string(s) || s.empty()) return false;
  // Likewise the null id, which an id128 field holds until someone sets it.
  return !(type == "id128" && s == "00000000000000000000000000000000");
}

// The schema of a primitive as schema::kind_name spells it; false for anything else.
bool primitive_schema(std::string_view t, JsonValue& out) {
  if (t == "bool") {
    out = typed("boolean");
  } else if (t == "u8") {
    out = integer(0, 0xFF);
  } else if (t == "u16") {
    out = integer(0, 0xFFFF);
  } else if (t == "u32") {
    out = integer(0, 0xFFFFFFFFull);
  } else if (t == "u64") {
    out = integer(0, 0);
  } else if (t == "i8") {
    out = integer(-128, 127);
  } else if (t == "i16") {
    out = integer(-32768, 32767);
  } else if (t == "i32") {
    out = typed("integer");
    out.set("minimum", JsonValue(i64{std::numeric_limits<i32>::min()}));
    out.set("maximum", JsonValue(i64{std::numeric_limits<i32>::max()}));
  } else if (t == "i64") {
    out = typed("integer");
  } else if (t == "f32" || t == "f64") {
    out = typed("number");
  } else if (t == "string") {
    out = typed("string");
  } else if (t == "bytes") {
    out = typed("string");
    out.set("pattern", JsonValue("^([0-9a-fA-F]{2})*$"));
  } else if (t == "id128") {
    // 32 hex digits; the all-zero id is the null id, which means "the root" where a parent is
    // asked for.
    out = typed("string");
    out.set("pattern", JsonValue("^[0-9a-fA-F]{32}$"));
  } else if (t == "vec2") {
    out = number_array(2);
  } else if (t == "vec3") {
    out = number_array(3);
  } else if (t == "vec4" || t == "quat") {
    out = number_array(4);
  } else if (t == "worldpos") {
    // A world position in metres (ADR-0053): three f64, each where a 64 m cell's i32 index
    // reaches, which the host refuses past naming the field.
    out = number_array(3);
    JsonValue item = typed("number");
    item.set("minimum", JsonValue(i64{-137438953408}));
    item.set("exclusiveMaximum", JsonValue(i64{137438953408}));
    out.set("items", std::move(item));
    out.set("description", JsonValue("A world position [x, y, z] in metres, y up."));
  } else if (t == "dvec3") {
    out = number_array(3);
  } else if (t == "json") {
    // Any JSON value: the empty schema.
    out = JsonValue::object();
  } else {
    return false;
  }
  return true;
}

}  // namespace

bool SchemaGen::load_methods(std::string& error) {
  JsonValue result;
  HostClient::Error e;
  if (host_.call("engine.methods", JsonValue(), result, e) != HostClient::Status::ok) {
    error = "engine.methods failed: " + e.message;
    return false;
  }
  const JsonValue* list = result.find("methods");
  if (list == nullptr || !list->is_array()) {
    error = "engine.methods returned no method list";
    return false;
  }
  methods_.clear();
  for (usize i = 0; i < list->size(); ++i) {
    const JsonValue& m = (*list)[i];
    methods_.push_back(Method{text_of(m, "name"), text_of(m, "doc"), text_of(m, "params_type"),
                              text_of(m, "result_type")});
  }
  return true;
}

const SchemaGen::Method* SchemaGen::method(std::string_view name) const noexcept {
  for (const Method& m : methods_) {
    if (m.name == name) return &m;
  }
  return nullptr;
}

bool SchemaGen::describe(std::string_view qualified, JsonValue& out, std::string& error) {
  if (const JsonValue* cached = described_.find_value(qualified); cached != nullptr) {
    out = *cached;
    return true;
  }
  JsonValue params = JsonValue::object();
  params.set("type", JsonValue(qualified));
  JsonValue result;
  HostClient::Error e;
  if (host_.call("schema.describe", params, result, e) != HostClient::Status::ok) {
    error = "schema.describe " + std::string(qualified) + " failed: " + e.message;
    return false;
  }
  const JsonValue* d = result.find("description");
  if (d == nullptr || !d->is_object()) {
    error = "schema.describe " + std::string(qualified) + " returned no description";
    return false;
  }
  described_.insert_or_assign(std::string(qualified), *d);
  out = *d;
  return true;
}

bool SchemaGen::type_schema(std::string_view type, JsonValue& out, std::string& error) {
  return type_schema_at(type, 0, out, error);
}

bool SchemaGen::params_schema(std::string_view name, JsonValue& out, std::string& error) {
  const Method* m = method(name);
  if (m == nullptr) {
    error = "the host has no method " + std::string(name);
    return false;
  }
  if (m->params_type.empty()) {
    out = typed("object");
    out.set("properties", JsonValue::object());
    out.set("additionalProperties", JsonValue(false));
    return true;
  }
  if (!type_schema(m->params_type, out, error)) return false;
  // The params struct's own documentation says what the *method* takes; the tool says what the
  // tool does, so the top-level description belongs to the tool.
  if (out.is_object()) out.as_object().erase(std::string_view("description"));
  return true;
}

bool SchemaGen::type_schema_at(std::string_view type, u32 depth, JsonValue& out,
                               std::string& error) {
  const std::string_view t = trim(type);
  // The engine's schemas are not recursive today; the bound only keeps a future one from turning
  // into unbounded recursion here.
  if (depth > 16) {
    out = JsonValue::object();
    return true;
  }
  if (t.starts_with("map<") && t.ends_with(">")) {
    const std::string_view inner = t.substr(4, t.size() - 5);
    usize comma = std::string_view::npos;
    i32 nesting = 0;
    for (usize i = 0; i < inner.size(); ++i) {
      if (inner[i] == '<') ++nesting;
      if (inner[i] == '>') --nesting;
      if (inner[i] == ',' && nesting == 0) {
        comma = i;
        break;
      }
    }
    if (comma == std::string_view::npos) {
      error = "cannot read the map type '" + std::string(t) + "'";
      return false;
    }
    const std::string_view key = trim(inner.substr(0, comma));
    JsonValue value;
    if (!type_schema_at(inner.substr(comma + 1), depth + 1, value, error)) return false;
    if (key == "string") {
      out = typed("object");
      out.set("additionalProperties", std::move(value));
    } else {
      // The protocol writes a map with other keys as an array of [key, value] pairs.
      out = typed("array");
      JsonValue pair = typed("array");
      pair.set("minItems", JsonValue(u32{2}));
      pair.set("maxItems", JsonValue(u32{2}));
      out.set("items", std::move(pair));
      out.set("description", JsonValue("[key, value] pairs, keys of type " + std::string(key)));
    }
    return true;
  }
  // Suffixes read from the end, because the outermost one is written last: "T?[]" is an array of
  // optional T.
  if (t.ends_with("?")) {
    // Optional: the caller may leave it out, which is how "none" is said.
    return type_schema_at(t.substr(0, t.size() - 1), depth, out, error);
  }
  if (t.ends_with("[]")) {
    JsonValue items;
    if (!type_schema_at(t.substr(0, t.size() - 2), depth + 1, items, error)) return false;
    out = typed("array");
    out.set("items", std::move(items));
    return true;
  }
  if (t.ends_with("]")) {
    const usize open = t.rfind('[');
    u32 n = 0;
    if (open == std::string_view::npos ||
        std::from_chars(t.data() + open + 1, t.data() + t.size() - 1, n).ec != std::errc()) {
      error = "cannot read the array type '" + std::string(t) + "'";
      return false;
    }
    JsonValue items;
    if (!type_schema_at(t.substr(0, open), depth + 1, items, error)) return false;
    out = typed("array");
    out.set("items", std::move(items));
    out.set("minItems", JsonValue(n));
    out.set("maxItems", JsonValue(n));
    return true;
  }

  // Primitives, as schema::kind_name spells them.
  if (primitive_schema(t, out)) return true;

  // A named struct or enum.
  JsonValue d;
  if (!describe(t, d, error)) return false;
  const std::string kind = text_of(d, "kind");
  const std::string doc = reflow(text_of(d, "doc"));
  if (kind == "enum") {
    out = typed("string");
    JsonValue names = JsonValue::array();
    std::string prose = doc;
    if (const JsonValue* values = d.find("values"); values != nullptr && values->is_array()) {
      for (usize i = 0; i < values->size(); ++i) {
        const std::string name = text_of((*values)[i], "name");
        const std::string value_doc = reflow(text_of((*values)[i], "doc"));
        names.push_back(JsonValue(name));
        // The value documentation is where an enum says what each choice does (CommandKind is
        // the whole command reference), so it goes into the one description a client shows.
        if (!value_doc.empty()) {
          if (!prose.empty()) prose += ' ';
          prose += name + ": " + value_doc;
        }
      }
    }
    out.set("enum", std::move(names));
    if (!prose.empty()) out.set("description", JsonValue(prose));
    return true;
  }
  if (kind != "struct") {
    error = "schema.describe " + std::string(t) + " is neither a struct nor an enum";
    return false;
  }

  out = typed("object");
  if (!doc.empty()) out.set("description", JsonValue(doc));
  // Which type this is, for share_definitions; removed before the schema leaves the bridge.
  out.set(k_type_marker, JsonValue(std::string(t)));
  JsonValue properties = JsonValue::object();
  if (const JsonValue* fields = d.find("fields"); fields != nullptr && fields->is_array()) {
    for (usize i = 0; i < fields->size(); ++i) {
      const JsonValue& f = (*fields)[i];
      // A transient field is never read from the wire.
      if (bool_of(f, "transient")) continue;
      const std::string name = text_of(f, "name");
      const std::string field_type = text_of(f, "type");
      JsonValue property;
      if (!type_schema_at(field_type, depth + 1, property, error)) return false;
      // The field's own documentation says what it is for; the type's says what it is. The first
      // is the better sentence where both exist.
      const std::string field_doc = reflow(text_of(f, "doc"));
      if (!field_doc.empty()) property.set("description", JsonValue(field_doc));
      if (bool_of(f, "deprecated")) property.set("deprecated", JsonValue(true));
      if (const JsonValue* def = f.find("default");
          def != nullptr && worth_stating(*def, field_type)) {
        property.set("default", field_type == "f32" ? tidy_f32(*def) : *def);
      }
      properties.set(name, std::move(property));
    }
  }
  out.set("properties", std::move(properties));
  // The protocol refuses a field it does not know rather than ignoring it, and says so; the
  // schema says the same thing up front.
  out.set("additionalProperties", JsonValue(false));
  return true;
}

// ---- $defs ---------------------------------------------------------------------------------------

namespace {

// The members a reference keeps from the place it stands: what the field says, not what the type
// is. Everything else is the type's body and goes into the definition.
constexpr std::string_view k_site_members[] = {"description", "default", "deprecated"};

JsonValue body_of(const JsonValue& node) {
  JsonValue body = node;
  for (std::string_view key : k_site_members) body.as_object().erase(key);
  return body;
}

struct Shared {
  std::string type;
  std::string text;  // the body, written compactly: two occurrences are one type when it is equal
  JsonValue body;
  u32 count = 0;
};

void collect(const JsonValue& node, Vector<Shared>& found) {
  if (node.is_array()) {
    for (usize i = 0; i < node.size(); ++i) collect(node[i], found);
    return;
  }
  if (!node.is_object()) return;
  if (const JsonValue* type = node.find(k_type_marker); type != nullptr && type->is_string()) {
    JsonValue body = body_of(node);
    std::string text = write_json(body, JsonWriteOptions{.pretty = false});
    bool known = false;
    for (Shared& s : found) {
      if (s.type == type->as_string() && s.text == text) {
        ++s.count;
        known = true;
        break;
      }
    }
    if (!known) {
      found.push_back(Shared{std::string(type->as_string()), std::move(text), std::move(body), 1});
    }
  }
  const JsonValue::Object& members = node.as_object();
  for (u32 i = 0; i < members.size(); ++i) collect(members.value_at(i), found);
}

void replace(JsonValue& node, const Shared& shared, const std::string& ref) {
  if (node.is_array()) {
    for (usize i = 0; i < node.size(); ++i) replace(node[i], shared, ref);
    return;
  }
  if (!node.is_object()) return;
  if (const JsonValue* type = node.find(k_type_marker);
      type != nullptr && type->is_string() && type->as_string() == shared.type &&
      write_json(body_of(node), JsonWriteOptions{.pretty = false}) == shared.text) {
    JsonValue site = JsonValue::object();
    site.set("$ref", JsonValue(ref));
    for (std::string_view key : k_site_members) {
      if (const JsonValue* v = node.find(key); v != nullptr) site.set(key, *v);
    }
    node = std::move(site);
    return;
  }
  JsonValue::Object& members = node.as_object();
  for (u32 i = 0; i < members.size(); ++i) replace(members.value_at(i), shared, ref);
}

void strip_markers(JsonValue& node) {
  if (node.is_array()) {
    for (usize i = 0; i < node.size(); ++i) strip_markers(node[i]);
    return;
  }
  if (!node.is_object()) return;
  node.as_object().erase(k_type_marker);
  JsonValue::Object& members = node.as_object();
  for (u32 i = 0; i < members.size(); ++i) strip_markers(members.value_at(i));
}

}  // namespace

void share_definitions(JsonValue& schema) {
  if (!schema.is_object()) return;
  // The largest repeated body first, so a struct inside a shared one is shared by being inside
  // its definition rather than given a definition of its own; then again, until nothing repeats.
  for (;;) {
    Vector<Shared> found;
    collect(schema, found);
    const Shared* best = nullptr;
    for (const Shared& s : found) {
      if (s.count >= 2 && (best == nullptr || s.text.size() > best->text.size())) best = &s;
    }
    if (best == nullptr) break;
    const Shared shared = *best;
    // The qualified name, which is unique and says what it is; a second body of the same type
    // (a tool that edited one occurrence) takes a suffix. Neither needs escaping in a pointer.
    JsonValue* defs = schema.find("$defs");
    std::string name = shared.type;
    for (u32 n = 2; defs != nullptr && defs->find(name) != nullptr; ++n)
      name = shared.type + "-" + std::to_string(n);
    replace(schema, shared, "#/$defs/" + name);
    JsonValue body = shared.body;
    body.as_object().erase(k_type_marker);
    if (schema.find("$defs") == nullptr) schema.set("$defs", JsonValue::object());
    schema.find("$defs")->set(name, std::move(body));
  }
  strip_markers(schema);
}

}  // namespace engine::mcp

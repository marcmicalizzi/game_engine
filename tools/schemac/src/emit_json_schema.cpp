#include "model.h"

#include <sstream>
#include <string>

namespace schemac {

namespace {

std::string integer_bounds(const std::string& name) {
  if (name == "u8") return "\"minimum\": 0, \"maximum\": 255";
  if (name == "u16") return "\"minimum\": 0, \"maximum\": 65535";
  if (name == "u32") return "\"minimum\": 0, \"maximum\": 4294967295";
  if (name == "u64") return "\"minimum\": 0, \"maximum\": 18446744073709551615";
  if (name == "i8") return "\"minimum\": -128, \"maximum\": 127";
  if (name == "i16") return "\"minimum\": -32768, \"maximum\": 32767";
  if (name == "i32") return "\"minimum\": -2147483648, \"maximum\": 2147483647";
  return "\"minimum\": -9223372036854775808, \"maximum\": 9223372036854775807";
}

std::string type_schema(const TypeExpr& t, const SchemaFile& file) {
  switch (t.kind) {
    case TypeExpr::Kind::Primitive:
      if (t.name == "bool") return "{\"type\": \"boolean\"}";
      if (t.name == "string") return "{\"type\": \"string\"}";
      if (t.name == "bytes") return "{\"type\": \"string\", \"pattern\": \"^([0-9a-fA-F]{2})*$\"}";
      if (t.name == "id128") return "{\"type\": \"string\", \"pattern\": \"^[0-9a-fA-F]{32}$\"}";
      if (t.name == "f32" || t.name == "f64") return "{\"type\": \"number\"}";
      if (t.name == "vec2")
        return "{\"type\": \"array\", \"items\": {\"type\": \"number\"}, \"minItems\": 2, "
               "\"maxItems\": 2}";
      if (t.name == "vec3")
        return "{\"type\": \"array\", \"items\": {\"type\": \"number\"}, \"minItems\": 3, "
               "\"maxItems\": 3}";
      if (t.name == "vec4" || t.name == "quat") {
        return "{\"type\": \"array\", \"items\": {\"type\": \"number\"}, \"minItems\": 4, "
               "\"maxItems\": 4}";
      }
      // A world position is three numbers each inside the range a 64 m cell's i32 index reaches
      // (core/math/world.h `world_cell_valid`); a reader refuses anything else, naming the field.
      if (t.name == "worldpos") {
        return "{\"type\": \"array\", \"items\": {\"type\": \"number\", \"minimum\": "
               "-137438953408, \"exclusiveMaximum\": 137438953408}, \"minItems\": 3, "
               "\"maxItems\": 3, \"x-engine-type\": \"worldpos\"}";
      }
      if (t.name == "dvec3") {
        return "{\"type\": \"array\", \"items\": {\"type\": \"number\"}, \"minItems\": 3, "
               "\"maxItems\": 3, \"x-engine-type\": \"dvec3\"}";
      }
      if (t.name == "json") return "{}";
      return "{\"type\": \"integer\", " + integer_bounds(t.name) + "}";
    case TypeExpr::Kind::Named: {
      const size_t dot = t.resolved_qualified.rfind('.');
      const std::string simple =
          dot == std::string::npos ? t.resolved_qualified : t.resolved_qualified.substr(dot + 1);
      if (t.resolved_stem == file.stem) return "{\"$ref\": \"#/$defs/" + simple + "\"}";
      return "{\"$ref\": \"" + t.resolved_stem + ".schema.json#/$defs/" + simple + "\"}";
    }
    case TypeExpr::Kind::Optional:
      return "{\"oneOf\": [" + type_schema(*t.element, file) + ", {\"type\": \"null\"}]}";
    case TypeExpr::Kind::Array:
      return "{\"type\": \"array\", \"items\": " + type_schema(*t.element, file) + "}";
    case TypeExpr::Kind::FixedArray:
      return "{\"type\": \"array\", \"items\": " + type_schema(*t.element, file) +
             ", \"minItems\": " + std::to_string(t.count) +
             ", \"maxItems\": " + std::to_string(t.count) + "}";
    case TypeExpr::Kind::Map:
      if (t.key->kind == TypeExpr::Kind::Primitive && t.key->name == "string") {
        return "{\"type\": \"object\", \"additionalProperties\": " + type_schema(*t.element, file) +
               "}";
      }
      return "{\"type\": \"array\", \"items\": {\"type\": \"array\", \"prefixItems\": [" +
             type_schema(*t.key, file) + ", " + type_schema(*t.element, file) +
             "], \"minItems\": 2, \"maxItems\": 2}}";
  }
  return "{}";
}

}  // namespace

std::string emit_json_schema(const Model& model, const SchemaFile& file) {
  (void)model;
  std::ostringstream out;
  out << "{\n";
  out << "  \"$schema\": \"https://json-schema.org/draft/2020-12/schema\",\n";
  out << "  \"$id\": \"" << file.stem << ".schema.json\",\n";
  out << "  \"title\": \"" << escape_json_string(file.ns) << "\",\n";
  out << "  \"$defs\": {\n";
  bool first = true;
  for (const EnumDecl& e : file.enums) {
    if (!first) out << ",\n";
    first = false;
    out << "    \"" << e.name << "\": {\n";
    if (!e.doc.empty()) out << "      \"description\": \"" << escape_json_string(e.doc) << "\",\n";
    out << "      \"enum\": [";
    for (size_t i = 0; i < e.values.size(); ++i)
      out << (i ? ", " : "") << "\"" << e.values[i].name << "\"";
    out << "]\n    }";
  }
  for (const StructDecl& s : file.structs) {
    if (!first) out << ",\n";
    first = false;
    out << "    \"" << s.name << "\": {\n";
    if (!s.doc.empty()) out << "      \"description\": \"" << escape_json_string(s.doc) << "\",\n";
    out << "      \"type\": \"object\",\n";
    out << "      \"x-schema-version\": " << s.version << ",\n";
    if (!s.tag.empty()) out << "      \"x-kind\": \"" << escape_json_string(s.tag) << "\",\n";
    if (s.transient) out << "      \"x-transient\": true,\n";
    out << "      \"properties\": {\n";
    bool first_field = true;
    std::vector<std::string> required;
    for (const Field& f : s.fields) {
      if (f.transient) continue;
      if (!first_field) out << ",\n";
      first_field = false;
      out << "        \"" << f.name << "\": ";
      std::string schema = type_schema(f.type, file);
      if (!f.doc.empty() || f.deprecated || f.since > 1) {
        // Wrap to attach annotations.
        schema.pop_back();  // remove closing brace
        if (schema.size() > 1) schema += ", ";
        if (!f.doc.empty()) schema += "\"description\": \"" + escape_json_string(f.doc) + "\", ";
        if (f.deprecated) schema += "\"deprecated\": true, ";
        if (f.since > 1) schema += "\"x-since-version\": " + std::to_string(f.since) + ", ";
        schema.erase(schema.size() - 2);
        schema += "}";
      }
      out << schema;
      const bool has_default = f.default_kind != DefaultKind::None;
      if (!has_default && f.type.kind != TypeExpr::Kind::Optional &&
          f.type.kind != TypeExpr::Kind::Array && f.type.kind != TypeExpr::Kind::Map) {
        required.push_back(f.name);
      }
    }
    out << "\n      },\n";
    out << "      \"required\": [";
    for (size_t i = 0; i < required.size(); ++i)
      out << (i ? ", " : "") << "\"" << required[i] << "\"";
    out << "],\n";
    out << "      \"additionalProperties\": false\n";
    out << "    }";
  }
  out << "\n  }\n}\n";
  return out.str();
}

}  // namespace schemac

#include "model.h"

#include <cstdlib>
#include <map>
#include <set>
#include <string>

namespace schemac {

namespace {

struct DeclRef {
  const SchemaFile* file;
  bool is_enum;
  const EnumDecl* enum_decl;
  const StructDecl* struct_decl;
};

std::string err(const SourceLoc& loc, const std::string& message) {
  return loc.file + ":" + std::to_string(loc.line) + ": " + message;
}

bool resolve_type(TypeExpr& type, const SchemaFile& file,
                  const std::map<std::string, DeclRef>& decls, const SourceLoc& loc,
                  std::vector<std::string>& errors) {
  switch (type.kind) {
    case TypeExpr::Kind::Primitive: return true;
    case TypeExpr::Kind::Named: {
      // Candidates: same namespace, then each import's namespace, then a fully qualified name.
      std::vector<std::string> candidates;
      candidates.push_back(file.ns + "." + type.name);
      for (const auto& [qualified, ref] : decls) {
        for (const std::string& imp : file.imports) {
          if (ref.file->stem == imp && qualified == ref.file->ns + "." + type.name)
            candidates.push_back(qualified);
        }
      }
      if (type.name.find('.') != std::string::npos) candidates.push_back(type.name);
      for (const std::string& c : candidates) {
        auto it = decls.find(c);
        if (it != decls.end()) {
          type.resolved_qualified = c;
          type.resolved_cpp = ns_to_cpp(c);
          type.resolved_stem = it->second.file->stem;
          type.resolved_is_enum = it->second.is_enum;
          if (it->second.file != &file) {
            bool imported = false;
            for (const std::string& imp : file.imports) {
              if (imp == it->second.file->stem) imported = true;
            }
            if (!imported) {
              errors.push_back(err(loc, "type '" + type.name + "' is declared in '" +
                                            it->second.file->stem +
                                            ".schema', which this file does not import"));
              return false;
            }
          }
          return true;
        }
      }
      errors.push_back(err(loc, "unknown type '" + type.name + "'"));
      return false;
    }
    case TypeExpr::Kind::Optional:
    case TypeExpr::Kind::Array:
    case TypeExpr::Kind::FixedArray: return resolve_type(*type.element, file, decls, loc, errors);
    case TypeExpr::Kind::Map: {
      bool ok = resolve_type(*type.key, file, decls, loc, errors);
      ok = resolve_type(*type.element, file, decls, loc, errors) && ok;
      if (!ok) return false;
      const TypeExpr& k = *type.key;
      const bool key_ok =
          (k.kind == TypeExpr::Kind::Primitive &&
           (k.name == "string" || k.name == "id128" || is_integer_primitive(k.name))) ||
          (k.kind == TypeExpr::Kind::Named && k.resolved_is_enum);
      if (!key_ok) {
        errors.push_back(err(loc, "map keys must be strings, integers, id128, or enums"));
        return false;
      }
      return true;
    }
  }
  return false;
}

bool check_default(const Field& f, const std::map<std::string, DeclRef>& decls,
                   std::vector<std::string>& errors) {
  const TypeExpr& t = f.type;
  switch (f.default_kind) {
    case DefaultKind::None: return true;
    case DefaultKind::Null:
      if (t.kind != TypeExpr::Kind::Optional) {
        errors.push_back(err(f.loc, "'null' default is only valid for optional fields"));
        return false;
      }
      return true;
    case DefaultKind::EmptyArray:
      if (t.kind != TypeExpr::Kind::Array && t.kind != TypeExpr::Kind::Map) {
        errors.push_back(err(f.loc, "'[]' default is only valid for arrays and maps"));
        return false;
      }
      return true;
    case DefaultKind::Bool:
      if (!(t.kind == TypeExpr::Kind::Primitive && t.name == "bool")) {
        errors.push_back(err(f.loc, "boolean default on a non-bool field"));
        return false;
      }
      return true;
    case DefaultKind::Int:
      if (t.kind == TypeExpr::Kind::Primitive &&
          (is_integer_primitive(t.name) || t.name == "f32" || t.name == "f64"))
        return true;
      errors.push_back(err(f.loc, "integer default on a field that is not a number"));
      return false;
    case DefaultKind::Float:
      if (t.kind == TypeExpr::Kind::Primitive && (t.name == "f32" || t.name == "f64")) return true;
      errors.push_back(err(f.loc, "float default on a field that is not f32 or f64"));
      return false;
    case DefaultKind::String:
      if (t.kind == TypeExpr::Kind::Primitive && t.name == "string") return true;
      errors.push_back(err(f.loc, "string default on a non-string field"));
      return false;
    case DefaultKind::Ident: {
      if (!(t.kind == TypeExpr::Kind::Named && t.resolved_is_enum)) {
        errors.push_back(err(
            f.loc, "identifier default '" + f.default_text + "' is only valid for enum fields"));
        return false;
      }
      const DeclRef& ref = decls.at(t.resolved_qualified);
      for (const EnumValue& v : ref.enum_decl->values) {
        if (v.name == f.default_text) return true;
      }
      errors.push_back(
          err(f.loc, "'" + f.default_text + "' is not an enumerator of " + t.resolved_qualified));
      return false;
    }
  }
  return true;
}

// ---- units ------------------------------------------------------------------------------------
//
// What `@unit` may name. Deliberately a small closed table rather than a unit algebra: a mapping
// row converts between two units of one dimension by an affine map, and the table is where the
// factors are written down once and reviewed. A unit a schema needs and the table lacks is a line
// here, not a syntax.

constexpr double k_pi = 3.14159265358979323846;

const UnitInfo k_units[] = {
    {"m", "length", 1.0, 0.0},
    {"cm", "length", 0.01, 0.0},
    {"mm", "length", 0.001, 0.0},
    {"km", "length", 1000.0, 0.0},
    {"rad", "angle", 1.0, 0.0},
    {"deg", "angle", k_pi / 180.0, 0.0},
    {"s", "time", 1.0, 0.0},
    {"ms", "time", 0.001, 0.0},
    {"us", "time", 0.000001, 0.0},
    {"min", "time", 60.0, 0.0},
    {"h", "time", 3600.0, 0.0},
    {"kg", "mass", 1.0, 0.0},
    {"g", "mass", 0.001, 0.0},
    {"t", "mass", 1000.0, 0.0},
    {"m/s", "velocity", 1.0, 0.0},
    {"cm/s", "velocity", 0.01, 0.0},
    {"km/h", "velocity", 1.0 / 3.6, 0.0},
    {"rad/s", "angular velocity", 1.0, 0.0},
    {"deg/s", "angular velocity", k_pi / 180.0, 0.0},
    {"rpm", "angular velocity", 2.0 * k_pi / 60.0, 0.0},
    {"K", "temperature", 1.0, 0.0},
    {"degC", "temperature", 1.0, 273.15},
    {"ratio", "ratio", 1.0, 0.0},
    {"percent", "ratio", 0.01, 0.0},
};

// The kinds a `@unit` can sit on: a number, or a vector of them. A quaternion is not a quantity
// with a unit, and a string or a struct has no number to convert.
bool unit_bearing(const TypeExpr& t) {
  if (t.kind == TypeExpr::Kind::FixedArray) return unit_bearing(*t.element);
  if (t.kind != TypeExpr::Kind::Primitive) return false;
  return is_integer_primitive(t.name) || t.name == "f32" || t.name == "f64" || t.name == "vec2" ||
         t.name == "vec3" || t.name == "vec4";
}

// The kinds a converted row can write: floats and float vectors, where scaling is exact to the
// field's own precision. An integer would round, so a unit change into one is refused.
bool float_based(const TypeExpr& t) {
  return t.kind == TypeExpr::Kind::Primitive &&
         (t.name == "f32" || t.name == "f64" || t.name == "vec2" || t.name == "vec3" ||
          t.name == "vec4");
}

// A field whose every byte is its value, which is what write-back's change test compares. Mirrors
// `schema::is_byte_comparable` in core/schema/src/materialize.cpp.
bool byte_comparable(const TypeExpr& t) {
  switch (t.kind) {
    case TypeExpr::Kind::Primitive:
      return t.name != "string" && t.name != "bytes" && t.name != "json";
    case TypeExpr::Kind::Named: return t.resolved_is_enum;
    case TypeExpr::Kind::FixedArray: return byte_comparable(*t.element);
    default: return false;
  }
}

// Two field types a row may join: the same type, or two float scalars of different width.
bool types_join(const TypeExpr& record, const TypeExpr& component) {
  if (idl_type(record) == idl_type(component)) return true;
  const auto scalar_float = [](const TypeExpr& t) {
    return t.kind == TypeExpr::Kind::Primitive && (t.name == "f32" || t.name == "f64");
  };
  return scalar_float(record) && scalar_float(component);
}

const Field* find_field(const StructDecl& s, const std::string& name) {
  for (const Field& f : s.fields) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

// Resolves a struct name as a named type is resolved — same namespace, then imports, then a
// qualified name — and hands back its declaration. Leaves `errors` alone: a mapping's own message
// says more about what went wrong than "unknown type".
const StructDecl* resolve_struct(const std::string& name, const SchemaFile& file,
                                 const std::map<std::string, DeclRef>& decls,
                                 std::string& out_qualified, bool& out_not_imported) {
  TypeExpr type;
  type.kind = TypeExpr::Kind::Named;
  type.name = name;
  std::vector<std::string> scratch;
  out_not_imported = false;
  if (!resolve_type(type, file, decls, SourceLoc{}, scratch)) {
    out_not_imported = !scratch.empty() && scratch[0].find("does not import") != std::string::npos;
    return nullptr;
  }
  const DeclRef& ref = decls.at(type.resolved_qualified);
  if (ref.is_enum) return nullptr;
  out_qualified = type.resolved_qualified;
  return ref.struct_decl;
}

void resolve_materialize(MaterializeDecl& m, const SchemaFile& file,
                         const std::map<std::string, DeclRef>& decls,
                         std::vector<std::string>& errors) {
  const std::string what = "materialize " + m.record;
  bool not_imported = false;
  const StructDecl* record =
      resolve_struct(m.record, file, decls, m.record_qualified, not_imported);
  if (record == nullptr) {
    errors.push_back(
        err(m.loc, what + ": '" + m.record + "' " +
                       (not_imported ? "is declared in a schema this file does not import"
                                     : "is not a declared struct")));
    return;
  }
  if (!is_record(*record)) {
    errors.push_back(err(m.loc, what + ": '" + m.record_qualified +
                                    "' is not a document record type (@kind(record))"));
    return;
  }
  m.record_cpp = ns_to_cpp(m.record_qualified);
  m.record_stem = decls.at(m.record_qualified).file->stem;

  for (const Attribute& a : m.attrs) {
    if (a.name == "doc") continue;
    if (a.name == "tiers") {
      if (a.args.empty()) {
        errors.push_back(err(a.loc, "@tiers names at least one tier"));
        continue;
      }
      m.tiers = 0;
      for (const std::string& arg : a.args) {
        char* end = nullptr;
        const unsigned long tier = std::strtoul(arg.c_str(), &end, 10);
        if (end == arg.c_str() || *end != '\0' || tier > 7) {
          errors.push_back(err(a.loc, "@tiers takes tier numbers 0..7, got '" + arg + "'"));
          continue;
        }
        m.tiers = static_cast<uint8_t>(m.tiers | (1u << tier));
      }
      continue;
    }
    errors.push_back(err(a.loc, "unknown materialize attribute '@" + a.name + "'"));
  }

  std::set<std::string> fields_seen;   // "Component.field"
  std::set<std::string> written_back;  // record properties a row writes back
  bool parent_seen = false;
  std::vector<std::string> component_order;  // qualified, first appearance
  for (MaterializeRow& row : m.rows) {
    if (row.kind == MaterializeRow::Kind::Parent) {
      if (parent_seen) {
        errors.push_back(err(row.loc, what + ": the parent is declared twice"));
        continue;
      }
      parent_seen = true;
      if (row.property != "ChildOf" && row.property != "none") {
        errors.push_back(
            err(row.loc, what + ": parent is ChildOf or none, not '" + row.property + "'"));
        continue;
      }
      m.parent = row.property;
      continue;
    }

    const StructDecl* component =
        resolve_struct(row.component, file, decls, row.component_qualified, not_imported);
    if (component == nullptr) {
      errors.push_back(
          err(row.loc, what + ": '" + row.component + "' " +
                           (not_imported ? "is declared in a schema this file does not import"
                                         : "is not a declared component; a materialize row names "
                                           "a @kind(component) struct")));
      continue;
    }
    if (!is_component(*component)) {
      errors.push_back(err(row.loc, what + ": '" + row.component_qualified +
                                        "' is not a component (@kind(component))"));
      continue;
    }
    row.component_cpp = ns_to_cpp(row.component_qualified);
    bool known = false;
    for (const std::string& c : component_order)
      known = known || c == row.component_qualified;
    if (!known) component_order.push_back(row.component_qualified);
    if (row.kind == MaterializeRow::Kind::Component) continue;

    const std::string key = row.component_qualified + "." + row.field;
    if (!fields_seen.insert(key).second) {
      errors.push_back(err(row.loc, what + ": '" + key + "' is mapped twice"));
      continue;
    }
    const Field* target = find_field(*component, row.field);
    if (target == nullptr) {
      errors.push_back(err(row.loc, what + ": component '" + row.component_qualified +
                                        "' has no field '" + row.field + "'"));
      continue;
    }
    const Field* source = find_field(*record, row.property);
    if (source == nullptr) {
      errors.push_back(err(row.loc, what + ": record type '" + m.record_qualified +
                                        "' has no property '" + row.property + "'"));
      continue;
    }
    if (source->transient) {
      errors.push_back(err(row.loc, what + ": property '" + row.property +
                                        "' is @transient, so no record ever carries it"));
      continue;
    }
    if (!types_join(source->type, target->type)) {
      errors.push_back(err(row.loc, what + ": '" + row.property + "' is " + idl_type(source->type) +
                                        " and '" + key + "' is " + idl_type(target->type) +
                                        "; a row joins the same type, or two float widths"));
      continue;
    }

    // Units: both sides or neither. A unit on one side only is ambiguous — it cannot say whether
    // the other side means the same unit or forgot to say — so it is refused rather than guessed.
    if (source->unit.empty() != target->unit.empty()) {
      errors.push_back(err(row.loc, what + ": '" + row.property + "' and '" + key +
                                        "' must both declare @unit, or neither"));
      continue;
    }
    if (!source->unit.empty() && source->unit != target->unit) {
      const UnitInfo* from = find_unit(source->unit);
      const UnitInfo* to = find_unit(target->unit);
      if (from == nullptr || to == nullptr) continue;  // reported on the field itself
      if (std::string(from->dimension) != to->dimension) {
        errors.push_back(err(row.loc, what + ": '" + row.property + "' is in " + from->symbol +
                                          " (" + from->dimension + ") and '" + key + "' in " +
                                          to->symbol + " (" + to->dimension +
                                          "); a unit converts only within its dimension"));
        continue;
      }
      if (!float_based(target->type) || !float_based(source->type)) {
        errors.push_back(err(row.loc, what + ": converting " + from->symbol + " to " + to->symbol +
                                          " needs float or float-vector fields; '" + key + "' is " +
                                          idl_type(target->type)));
        continue;
      }
      // si = v × from.to_si + from.si_offset; w = (si − to.si_offset) / to.to_si.
      row.convert = true;
      row.scale = from->to_si / to->to_si;
      row.offset = (from->si_offset - to->si_offset) / to->to_si;
    }

    if (row.writeback) {
      if (component->transient || target->transient) {
        errors.push_back(
            err(row.loc, what + ": '" + key + "' is transient, so the world never writes it back"));
        continue;
      }
      if (!byte_comparable(target->type)) {
        errors.push_back(err(row.loc, what + ": '" + key + "' is " + idl_type(target->type) +
                                          "; @writeback needs a field that is its own bytes (a "
                                          "number, enum, id, vector, quat, or a fixed array of "
                                          "them)"));
        continue;
      }
      if (!written_back.insert(row.property).second) {
        errors.push_back(err(row.loc, what + ": two rows write '" + row.property +
                                          "' back; one property has one writer"));
        continue;
      }
    }
  }

  // The emitted order: components as first named, and each component's field rows in declaration
  // order under it, so a writer that walks the table touches each component once.
  for (const std::string& qualified : component_order) {
    m.components_cpp.push_back(ns_to_cpp(qualified));
    for (size_t i = 0; i < m.rows.size(); ++i) {
      const MaterializeRow& row = m.rows[i];
      if (row.kind == MaterializeRow::Kind::Field && row.component_qualified == qualified &&
          !row.component_cpp.empty())
        m.ordered_fields.push_back(i);
    }
  }
}

}  // namespace

const UnitInfo* find_unit(const std::string& symbol) {
  for (const UnitInfo& u : k_units) {
    if (symbol == u.symbol) return &u;
  }
  return nullptr;
}

bool resolve(Model& model, std::vector<std::string>& errors) {
  std::map<std::string, DeclRef> decls;
  std::set<std::string> stems;
  for (const SchemaFile& file : model.files) {
    if (!stems.insert(file.stem).second) {
      errors.push_back(file.path + ":1: duplicate schema file stem '" + file.stem + "'");
    }
    for (const EnumDecl& e : file.enums) {
      const std::string q = file.ns + "." + e.name;
      if (!decls.emplace(q, DeclRef{&file, true, &e, nullptr}).second)
        errors.push_back(err(e.loc, "duplicate type '" + q + "'"));
      if (!is_integer_primitive(e.underlying))
        errors.push_back(err(e.loc, "enum underlying type must be an integer primitive"));
      std::set<std::string> names;
      std::set<int64_t> values;
      for (const EnumValue& v : e.values) {
        if (!names.insert(v.name).second)
          errors.push_back(err(v.loc, "duplicate enumerator '" + v.name + "'"));
        if (!values.insert(v.value).second)
          errors.push_back(err(v.loc, "duplicate enumerator value for '" + v.name + "'"));
      }
      if (e.values.empty()) errors.push_back(err(e.loc, "enum '" + e.name + "' has no values"));
    }
    for (const StructDecl& s : file.structs) {
      const std::string q = file.ns + "." + s.name;
      if (!decls.emplace(q, DeclRef{&file, false, nullptr, &s}).second)
        errors.push_back(err(s.loc, "duplicate type '" + q + "'"));
      if (s.version == 0) errors.push_back(err(s.loc, "@version must be at least 1"));
    }
  }
  for (const SchemaFile& file : model.files) {
    for (const std::string& imp : file.imports) {
      if (stems.count(imp) == 0)
        errors.push_back(file.path + ":1: import '" + imp + ".schema' was not provided to schemac");
    }
  }
  if (!errors.empty()) return false;

  for (SchemaFile& file : model.files) {
    for (StructDecl& s : file.structs) {
      std::set<std::string> names;
      for (Field& f : s.fields) {
        if (!names.insert(f.name).second)
          errors.push_back(err(f.loc, "duplicate field '" + f.name + "'"));
        if (!resolve_type(f.type, file, decls, f.loc, errors)) continue;
        check_default(f, decls, errors);
        if (f.since > s.version)
          errors.push_back(err(f.loc, "@since exceeds the struct's @version"));
        if (f.since == 0) errors.push_back(err(f.loc, "@since must be at least 1"));
        for (const Attribute& a : f.attrs) {
          if (a.name != "since" && a.name != "transient" && a.name != "deprecated" &&
              a.name != "doc" && a.name != "unit") {
            errors.push_back(err(a.loc, "unknown field attribute '@" + a.name + "'"));
          }
          if (a.name == "unit" && a.args.size() != 1) {
            errors.push_back(err(a.loc, "@unit takes one unit symbol"));
          }
        }
        if (!f.unit.empty()) {
          if (find_unit(f.unit) == nullptr) {
            errors.push_back(err(f.loc, "unknown unit '" + f.unit +
                                            "'; the table is in tools/schemac/src/resolve.cpp"));
          } else if (!unit_bearing(f.type)) {
            errors.push_back(err(f.loc, "@unit on '" + f.name + "', which is " + idl_type(f.type) +
                                            " and has no number to measure"));
          }
        }
      }
      for (const Attribute& a : s.attrs) {
        if (a.name != "version" && a.name != "kind" && a.name != "doc" && a.name != "transient")
          errors.push_back(err(a.loc, "unknown struct attribute '@" + a.name + "'"));
      }
      // A component is registered with the entity store by name, so it needs a name nothing
      // else can take, and it must carry data or be a deliberate tag. What it may not be is
      // both transient and something the persistence layer would be asked to write, which is
      // the check the *store* makes; here the only structural rule is that @transient without
      // @kind(component) says nothing, because only a component type is ever persisted as a
      // whole (ADR-0028 seam 1, docs/plan/03-data-model.md §3.4).
      if (s.transient && !is_component(s)) {
        errors.push_back(err(s.loc, "struct '" + s.name +
                                        "' is @transient but not @kind(component); mark the "
                                        "fields @transient instead"));
      }
    }
    for (EnumDecl& e : file.enums) {
      for (const Attribute& a : e.attrs) {
        if (a.name != "kind" && a.name != "doc")
          errors.push_back(err(a.loc, "unknown enum attribute '@" + a.name + "'"));
      }
    }
  }
  // Mappings last: they read field types, units and defaults that the loop above resolved.
  if (!errors.empty()) return false;
  std::map<std::string, SourceLoc> mapped;
  for (SchemaFile& file : model.files) {
    for (MaterializeDecl& m : file.materializations) {
      resolve_materialize(m, file, decls, errors);
      if (m.record_qualified.empty()) continue;
      const auto [it, fresh] = mapped.emplace(m.record_qualified, m.loc);
      if (!fresh) {
        errors.push_back(err(m.loc, "'" + m.record_qualified + "' is already mapped at " +
                                        it->second.file + ":" + std::to_string(it->second.line) +
                                        "; a record type has one materialize declaration"));
      }
    }
  }
  return errors.empty();
}

// --- shared helpers -------------------------------------------------------------------------

std::string ns_to_cpp(const std::string& dotted) {
  std::string out;
  for (char c : dotted) {
    if (c == '.') {
      out += "::";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string cpp_type(const TypeExpr& t) {
  switch (t.kind) {
    case TypeExpr::Kind::Primitive:
      if (t.name == "bool") return "bool";
      if (t.name == "string") return "std::string";
      if (t.name == "bytes") return "engine::Vector<engine::u8>";
      if (t.name == "id128") return "engine::Id128";
      if (t.name == "vec2") return "engine::Vec2";
      if (t.name == "vec3") return "engine::Vec3";
      if (t.name == "vec4") return "engine::Vec4";
      if (t.name == "quat") return "engine::Quat";
      if (t.name == "json") return "engine::JsonValue";
      return "engine::" + t.name;
    case TypeExpr::Kind::Named: return t.resolved_cpp;
    case TypeExpr::Kind::Optional: return "std::optional<" + cpp_type(*t.element) + ">";
    case TypeExpr::Kind::Array: return "engine::Vector<" + cpp_type(*t.element) + ">";
    case TypeExpr::Kind::FixedArray:
      return "std::array<" + cpp_type(*t.element) + ", " + std::to_string(t.count) + ">";
    case TypeExpr::Kind::Map:
      return "engine::FlatMap<" + cpp_type(*t.key) + ", " + cpp_type(*t.element) + ">";
  }
  return "?";
}

std::string idl_type(const TypeExpr& t) {
  switch (t.kind) {
    case TypeExpr::Kind::Primitive: return t.name;
    case TypeExpr::Kind::Named: return t.resolved_qualified.empty() ? t.name : t.resolved_qualified;
    case TypeExpr::Kind::Optional: return idl_type(*t.element) + "?";
    case TypeExpr::Kind::Array: return idl_type(*t.element) + "[]";
    case TypeExpr::Kind::FixedArray:
      return idl_type(*t.element) + "[" + std::to_string(t.count) + "]";
    case TypeExpr::Kind::Map: return "map<" + idl_type(*t.key) + ", " + idl_type(*t.element) + ">";
  }
  return "?";
}

std::string escape_cpp_string(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

std::string escape_json_string(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x",
                        static_cast<unsigned>(static_cast<unsigned char>(c)));
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

}  // namespace schemac

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

}  // namespace

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
              a.name != "doc") {
            errors.push_back(err(a.loc, "unknown field attribute '@" + a.name + "'"));
          }
        }
      }
      for (const Attribute& a : s.attrs) {
        if (a.name != "version" && a.name != "kind" && a.name != "doc")
          errors.push_back(err(a.loc, "unknown struct attribute '@" + a.name + "'"));
      }
    }
    for (EnumDecl& e : file.enums) {
      for (const Attribute& a : e.attrs) {
        if (a.name != "kind" && a.name != "doc")
          errors.push_back(err(a.loc, "unknown enum attribute '@" + a.name + "'"));
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

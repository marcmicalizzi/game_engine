#pragma once

// In-memory model of parsed schema files. schemac is a standalone tool and uses the standard
// library freely (tools/ is exempt from the engine container policy).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace schemac {

struct SourceLoc {
  std::string file;
  int line = 0;
};

struct Attribute {
  std::string name;
  std::vector<std::string> args;
  SourceLoc loc;
};

struct TypeExpr {
  enum class Kind { Primitive, Named, Optional, Array, FixedArray, Map };
  Kind kind = Kind::Primitive;
  std::string name;                    // primitive keyword or (possibly dotted) type name
  std::shared_ptr<TypeExpr> element;   // Optional/Array/FixedArray element, Map value
  std::shared_ptr<TypeExpr> key;       // Map key
  uint64_t count = 0;                  // FixedArray

  // Filled by resolve() for Named types.
  std::string resolved_qualified;      // "a.b.Name"
  std::string resolved_cpp;            // "a::b::Name"
  std::string resolved_stem;           // schema file stem declaring the type
  bool resolved_is_enum = false;
};

enum class DefaultKind { None, Int, Float, Bool, String, Ident, EmptyArray, Null };

struct Field {
  std::string name;
  TypeExpr type;
  DefaultKind default_kind = DefaultKind::None;
  std::string default_text;  // literal text as written (string contents unescaped)
  std::vector<Attribute> attrs;
  std::string doc;
  SourceLoc loc;
  uint16_t since = 1;
  bool transient = false;
  bool deprecated = false;
};

struct EnumValue {
  std::string name;
  int64_t value = 0;
  std::string doc;
  SourceLoc loc;
};

struct EnumDecl {
  std::string name;
  std::string underlying = "u32";
  std::vector<EnumValue> values;
  std::vector<Attribute> attrs;
  std::string doc;
  std::string tag;
  SourceLoc loc;
};

struct StructDecl {
  std::string name;
  std::vector<Field> fields;
  std::vector<Attribute> attrs;
  std::string doc;
  std::string tag;
  uint16_t version = 1;
  SourceLoc loc;
};

struct SchemaFile {
  std::string path;
  std::string stem;
  std::string ns;                     // dotted
  std::vector<std::string> imports;   // stems
  std::vector<EnumDecl> enums;
  std::vector<StructDecl> structs;
};

struct Model {
  std::vector<SchemaFile> files;
};

bool is_primitive_name(const std::string& name);
bool is_integer_primitive(const std::string& name);

// Parses one schema file. On failure returns false with a "file:line: message" in `error`.
bool parse_schema(const std::string& path, const std::string& text, SchemaFile& out, std::string& error);

// Resolves named types across all files and validates declarations. Errors are appended.
bool resolve(Model& model, std::vector<std::string>& errors);

// Emitters. Each writes one file's output; return false and set error on I/O failure.
std::string emit_cpp_header(const Model& model, const SchemaFile& file);
std::string emit_cpp_source(const Model& model, const SchemaFile& file);
std::string emit_json_schema(const Model& model, const SchemaFile& file);
std::string emit_docs(const Model& model, const SchemaFile& file);

// Shared helpers.
std::string ns_to_cpp(const std::string& dotted);      // "a.b" -> "a::b"
std::string cpp_type(const TypeExpr& type);            // C++ spelling of a type expression
std::string idl_type(const TypeExpr& type);            // IDL spelling, for docs
std::string escape_cpp_string(const std::string& s);   // contents for a C++ string literal
std::string escape_json_string(const std::string& s);  // contents for a JSON string literal

}  // namespace schemac

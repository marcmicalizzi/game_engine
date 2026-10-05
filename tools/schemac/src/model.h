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
  std::string name;                   // primitive keyword or (possibly dotted) type name
  std::shared_ptr<TypeExpr> element;  // Optional/Array/FixedArray element, Map value
  std::shared_ptr<TypeExpr> key;      // Map key
  uint64_t count = 0;                 // FixedArray

  // Filled by resolve() for Named types.
  std::string resolved_qualified;  // "a.b.Name"
  std::string resolved_cpp;        // "a::b::Name"
  std::string resolved_stem;       // schema file stem declaring the type
  bool resolved_is_enum = false;
};

// `Vector` is `[x, y, z]`, only on `worldpos` and `dvec3`; its text is the components as written,
// joined by ','.
enum class DefaultKind { None, Int, Float, Bool, String, Ident, EmptyArray, Null, Vector };

// The two f64 vector primitives (ADR-0053): `worldpos` a point, `dvec3` a displacement.
inline bool is_f64_vector_primitive(const std::string& name) {
  return name == "worldpos" || name == "dvec3";
}

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
  // `@unit(<symbol>)`: the physical unit the field's number is in. Only read by a `materialize`
  // row, which converts when the two sides it joins declare different units of one dimension.
  std::string unit;
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
  // Struct-level @transient: the whole type is runtime-only and is never persisted
  // (docs/plan/03-data-model.md §3.4). Field-level @transient, on Field, is the narrower thing:
  // one field of an otherwise persisted type.
  bool transient = false;
  SourceLoc loc;
};

// A struct that carries @kind(component) is a world component: schemac emits its flecs
// registration as well as its C++ type (ADR-0028 seam 1).
inline bool is_component(const StructDecl& s) { return s.tag == "component"; }
// A document record type: what `materialize` declarations map from.
inline bool is_record(const StructDecl& s) { return s.tag == "record"; }

// One line of a `materialize` block (docs/plan/03-data-model.md §3.4):
//
//     Component.field = property [@writeback]   a field row
//     Component                                 the component, with its schema defaults
//     parent = ChildOf | none                   what the document's parent becomes
struct MaterializeRow {
  enum class Kind { Field, Component, Parent };
  Kind kind = Kind::Field;
  std::string component;  // as written: simple, imported, or qualified name
  std::string field;      // Field: the component's field
  std::string property;   // Field: the record's property; Parent: "ChildOf" or "none"
  bool writeback = false;
  SourceLoc loc;

  // Filled by resolve().
  std::string component_qualified;  // "a.b.Name"
  std::string component_cpp;        // "a::b::Name"
  bool convert = false;
  double scale = 1.0;
  double offset = 0.0;
};

// `materialize <RecordType> [@tiers(...)] { rows }`: which components a record type becomes and how
// its properties map onto their fields. Compiled into a `schema::MaterializeInfo`.
struct MaterializeDecl {
  std::string record;  // as written
  std::vector<MaterializeRow> rows;
  std::vector<Attribute> attrs;
  std::string doc;
  SourceLoc loc;
  uint8_t tiers = 0x0F;

  // Filled by resolve().
  std::string record_qualified;
  std::string record_cpp;
  std::string record_stem;
  std::string parent = "none";
  // Every component in first-appearance order, and the field rows (indices into `rows`) grouped
  // by it.
  std::vector<std::string> components_cpp;
  std::vector<size_t> ordered_fields;
};

struct SchemaFile {
  std::string path;
  std::string stem;
  std::string ns;                    // dotted
  std::vector<std::string> imports;  // stems
  std::vector<EnumDecl> enums;
  std::vector<StructDecl> structs;
  std::vector<MaterializeDecl> materializations;
};

struct Model {
  std::vector<SchemaFile> files;
};

bool is_primitive_name(const std::string& name);
bool is_integer_primitive(const std::string& name);

// Parses one schema file. On failure returns false with a "file:line: message" in `error`.
bool parse_schema(const std::string& path, const std::string& text, SchemaFile& out,
                  std::string& error);

// Resolves named types across all files and validates declarations. Errors are appended.
bool resolve(Model& model, std::vector<std::string>& errors);

// The unit table `@unit` reads (resolve.cpp). A unit is a symbol, a dimension, and the affine map
// to the dimension's SI unit: si = value × to_si + si_offset. Two units convert only within one
// dimension.
struct UnitInfo {
  const char* symbol;
  const char* dimension;
  double to_si;
  double si_offset;
};
const UnitInfo* find_unit(const std::string& symbol);

// Emitters. Each writes one file's output; return false and set error on I/O failure.
std::string emit_cpp_header(const Model& model, const SchemaFile& file);
std::string emit_cpp_source(const Model& model, const SchemaFile& file);
std::string emit_json_schema(const Model& model, const SchemaFile& file);
std::string emit_docs(const Model& model, const SchemaFile& file);
// The ECS backend (ADR-0028 seam 1): flecs registration for this file's @kind(component)
// structs. Always emitted; only compiled where <flecs.h> may be included, which is why it is a
// header and not a source file.
std::string emit_ecs_header(const Model& model, const SchemaFile& file);

// Shared helpers.
std::string ns_to_cpp(const std::string& dotted);      // "a.b" -> "a::b"
std::string cpp_type(const TypeExpr& type);            // C++ spelling of a type expression
std::string idl_type(const TypeExpr& type);            // IDL spelling, for docs
std::string escape_cpp_string(const std::string& s);   // contents for a C++ string literal
std::string escape_json_string(const std::string& s);  // contents for a JSON string literal

}  // namespace schemac

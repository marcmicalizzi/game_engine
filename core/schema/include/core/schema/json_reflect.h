#pragma once

// JSON (de)serialization of any schema type through its TypeInfo, plus schema-version
// migrations applied to the JSON before it is mapped onto the C++ object.
//
// Conventions: structs are objects keyed by field name (transient fields omitted); enums are
// their value names (integers accepted on read); optionals are null when empty; Id128 and bytes
// are hex strings; maps with string keys are objects, other maps are arrays of [key, value]
// pairs; fixed arrays must have exactly their declared length. Missing fields keep the default
// the C++ type constructs with. Unknown fields are errors unless ReadOptions says otherwise.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/schema/type_info.h>

#include <string>
#include <string_view>

namespace engine::schema {

struct Diagnostic {
  std::string path;  // "inners[1].name"
  std::string message;
};

struct ReadOptions {
  bool ignore_unknown_fields = false;
};

class ReadContext {
 public:
  ReadOptions options;
  Vector<Diagnostic> diagnostics;

  bool ok() const noexcept { return diagnostics.empty(); }
  void error(std::string_view message);
  std::string path() const;

  // Path bookkeeping used by the walker.
  void push_field(const char* name);
  void push_index(usize index);
  void pop();

 private:
  Vector<std::string> segments_;
};

// Walkers over any TypeRef.
bool to_json(const TypeRef& type, const void* object, JsonValue& out);
bool from_json(const TypeRef& type, void* object, const JsonValue& in, ReadContext& ctx);

// Whole-type entry points (struct or enum TypeInfo).
bool to_json(const TypeInfo& type, const void* object, JsonValue& out);
bool from_json(const TypeInfo& type, void* object, const JsonValue& in, ReadContext& ctx);

template <class T>
JsonValue to_json(const T& value) {
  JsonValue out;
  to_json(type_of<T>(), &value, out);
  return out;
}

template <class T>
bool from_json(T& value, const JsonValue& in, ReadContext& ctx) {
  return from_json(type_of<T>(), &value, in, ctx);
}

// --- migrations ---------------------------------------------------------------------------

// Rewrites a JSON object of `type_qualified_name` from `from_version` to `from_version + 1`.
struct Migration {
  const char* type_qualified_name;
  u16 from_version;
  bool (*apply)(JsonValue& object, ReadContext& ctx);
};

class MigrationRegistry {
 public:
  static MigrationRegistry& global();
  void add(const Migration& migration);
  // Applies every step from `from_version` up to `type.version`; fails if a step is missing.
  bool migrate(const TypeInfo& type, JsonValue& object, u16 from_version, ReadContext& ctx) const;

 private:
  Vector<Migration> migrations_;
};

// Reads an object stored at an older schema version: migrates the JSON, then maps it.
bool from_json_versioned(const TypeInfo& type, void* object, JsonValue in, u16 stored_version,
                         ReadContext& ctx);

template <class T>
bool from_json_versioned(T& value, JsonValue in, u16 stored_version, ReadContext& ctx) {
  return from_json_versioned(type_of<T>(), &value, std::move(in), stored_version, ctx);
}

}  // namespace engine::schema

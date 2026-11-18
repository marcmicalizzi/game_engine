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

// --- former names (`@alias`; schemas/README.md, "Renaming a field or a type") ----------------
//
// `from_json` already reads a member under a field's former name as the field. These rewrite the
// JSON itself, in place, for a holder of raw JSON that writes it back as it holds it (a document's
// records, whose properties are JSON until validated): every member of `members`, an object of
// `type`, named by a former name of one of its fields gets the field's name, and so does every
// member of an object of a struct type reached through its fields — optionals, arrays, fixed arrays
// and maps included. `renamed` counts the members renamed. An object carrying one field under two
// of its names is refused, naming both: false, with `ctx` saying where, and the JSON is not to be
// used (the members renamed before the refusal stay renamed).
bool rename_former_fields(const TypeInfo& type, JsonValue::Object& members, ReadContext& ctx,
                          u32& renamed);
bool rename_former_fields(const TypeRef& type, JsonValue& value, ReadContext& ctx, u32& renamed);

// --- migrations ---------------------------------------------------------------------------

// Rewrites a JSON object of `type_qualified_name` from `from_version` to `from_version + 1`.
struct Migration {
  const char* type_qualified_name;
  u16 from_version;
  bool (*apply)(JsonValue& object, ReadContext& ctx);
};

// Whether the step to `to_version` only added fields: the schema declares it `@additive` and some
// field of `type` is `@since(to_version)`. Such a step is generated — derived from the tables
// schemac emits — and changes nothing in the JSON, since an absent field takes its default
// (schemas/README.md, "Evolution rules").
bool additive_step(const TypeInfo& type, u16 to_version) noexcept;

class MigrationRegistry {
 public:
  static MigrationRegistry& global();
  void add(const Migration& migration);
  // Applies every step from `from_version` up to `type.version`: the one registered for it, or else
  // the generated additive step when the version is declared additive; fails naming the first step
  // that is neither.
  bool migrate(const TypeInfo& type, JsonValue& object, u16 from_version, ReadContext& ctx) const;

 private:
  Vector<Migration> migrations_;
};

// Brings a JSON object stored at `stored_version` of `type` up to the build's version, through
// every step the registry has or generates, without mapping it: what an event's payload read from
// the store goes through (plan 03 §3.8, "events are upcast"). False, with the reason in `ctx`, for
// a version newer than the build's, a version 0, or a missing step.
bool upcast(const TypeInfo& type, JsonValue& object, u16 stored_version, ReadContext& ctx);

// Reads an object stored at an older schema version: migrates the JSON, then maps it.
bool from_json_versioned(const TypeInfo& type, void* object, JsonValue in, u16 stored_version,
                         ReadContext& ctx);

template <class T>
bool from_json_versioned(T& value, JsonValue in, u16 stored_version, ReadContext& ctx) {
  return from_json_versioned(type_of<T>(), &value, std::move(in), stored_version, ctx);
}

}  // namespace engine::schema

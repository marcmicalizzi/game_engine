#pragma once

// A schema type read from and written to JSON text with no tree in between (roadmap D40).
//
// `read_json_text` is `parse_json` followed by `from_json` into a default-constructed object, the
// same in every outcome — the same object, the same diagnostics with the same paths and in the same
// order, the same parse refusal at the same offset, line and column — without building a JsonValue
// of the text first. It walks the text with a `JsonReader` and the type's descriptors together,
// so a scene file of 172 MB is never a tree of every one of its values held while it is copied out.
// A `json` field still becomes a JsonValue (it is one), built straight into the field.
//
// It is exactly the tree path because it hands every case it does not stream to the tree path:
//   - a value of a shape its field does not take (an array where an object goes, a string where a
//     number goes) is built as a JsonValue and given to `from_json`, which words the refusal; so is
//     a fixed array, whose length the tree path checks before reading an element, and each pair of
//     a map keyed by something other than strings;
//   - an object whose keys are not in strictly increasing byte order — out of order, or one key
//     twice — or that names a field by a former name (`@alias`, D28) is a text the tree path reads
//     in its sorted order, last of two keys winning and two names of one field refused by both. The
//     walk stops there, and the whole text is read again by the tree path into a fresh object.
//     Every file this engine writes is canonical and takes the stream; a hand-written or renamed
//     one pays one more pass.
//
// `write_json_text` is `write_json(to_json(object))` without the tree, and `canonical_json_equals`
// asks whether given bytes are exactly that text followed by `suffix`, comparing as it writes and
// stopping at the first byte that differs: what a document store asks of a file it has just read,
// to learn whether the next save must rewrite it, without serializing the file a second time into
// a string (docs/subsystems/doc.md, "Reading a file without a tree").

#include <core/base/types.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::schema {

// A field of the root type that is read as a JsonValue and prepared before it is mapped: what a
// reader that rewrote the tree before `from_json` (a migration that cannot be told from a version,
// a field dropped before reading) keeps doing on that field alone. `prepare` is called on the
// member's value when the root object has the member, with nothing on the context's path; false
// refuses the read, as the tree path's caller would have.
struct TreeField {
  const char* name = "";
  bool (*prepare)(JsonValue& value, ReadContext& ctx, void* user) = nullptr;
  void* user = nullptr;
};

struct TextReadOptions {
  JsonParseOptions parse;
  std::span<const TreeField> tree_fields;
};

// Reads `text` as `type` (a struct) into `object`, which is reset to the type's default first.
// `parse` is the text's syntax verdict; on a refusal there, the diagnostics are as the context had
// them and the object is the default. True when the text parsed and every value mapped. `streamed`,
// when given, says whether the walk took the text whole or handed it to the tree path.
bool read_json_text(const TypeInfo& type, void* object, std::string_view text, ReadContext& ctx,
                    JsonParseResult& parse, const TextReadOptions& options = {},
                    bool* streamed = nullptr);

template <class T>
bool read_json_text(T& value, std::string_view text, ReadContext& ctx, JsonParseResult& parse,
                    const TextReadOptions& options = {}, bool* streamed = nullptr) {
  return read_json_text(type_of<T>(), &value, text, ctx, parse, options, streamed);
}

// Appends what `write_json(to_json(object), options)` would return. False where that is false
// (a float that is not finite, written as null).
bool write_json_text(const TypeInfo& type, const void* object, std::string& out,
                     const JsonWriteOptions& options = {});

// Whether `text` is byte for byte `write_json(to_json(object), options)` followed by `suffix`.
bool canonical_json_equals(const TypeInfo& type, const void* object, std::string_view text,
                           std::string_view suffix = {}, const JsonWriteOptions& options = {});

template <class T>
bool canonical_json_equals(const T& value, std::string_view text, std::string_view suffix = {},
                           const JsonWriteOptions& options = {}) {
  return canonical_json_equals(type_of<T>(), &value, text, suffix, options);
}

}  // namespace engine::schema

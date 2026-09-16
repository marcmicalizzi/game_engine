#pragma once

// Parsing and writing JSON text.
//
// The writer produces canonical output (docs/plan/03-data-model.md §3.2): object keys sorted
// (a property of JsonValue::Object), integers exact, floats in shortest round-trip form,
// minimal escaping, UTF-8 passed through, two-space indentation when pretty. Two runs over
// equal documents produce byte-identical text on every platform, which is what makes the
// authoring document diffable.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>

#include <string>
#include <string_view>

namespace engine {

struct JsonParseOptions {
  // Nesting depth beyond which the parser reports an error instead of recursing. A budget
  // against hostile input, not a format limit; raise it for deeper legitimate documents. The
  // parser and JsonValue's destructor recurse once per level, at roughly 1 KB of stack per
  // level in debug builds, so a raised budget needs a correspondingly sized thread stack.
  u32 max_depth = 256;
};

struct JsonParseResult {
  bool ok = false;
  usize offset = 0;  // byte offset of the error
  u32 line = 0;      // 1-based
  u32 column = 0;    // 1-based
  const char* message = "";
};

// Parses a complete JSON text. Trailing whitespace is allowed; anything else after the value
// is an error. On failure `out` is left null.
JsonParseResult parse_json(std::string_view text, JsonValue& out,
                           const JsonParseOptions& options = {});

struct JsonWriteOptions {
  bool pretty = true;  // newlines and two-space indentation; false gives the compact form
};

// Appends the serialization of `value` to `out`. Returns false (and writes null in place of
// the offending number) if a float is NaN or infinite, which JSON cannot represent.
bool write_json(const JsonValue& value, std::string& out, const JsonWriteOptions& options = {});
std::string write_json(const JsonValue& value, const JsonWriteOptions& options = {});

}  // namespace engine

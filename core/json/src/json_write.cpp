#include <core/json/json.h>
#include <core/json/json_format.h>

namespace engine {

// The spellings live in json_format.h, shared with the writers that walk a schema type's fields
// rather than a JsonValue (roadmap D40), so every walk spells a value one way.

bool write_json(const JsonValue& value, std::string& out, const JsonWriteOptions& options) {
  json_format::StringSink sink{out};
  return json_format::value(sink, value, options.pretty, 0);
}

std::string write_json(const JsonValue& value, const JsonWriteOptions& options) {
  std::string out;
  json_format::StringSink sink{out};
  json_format::value(sink, value, options.pretty, 0);
  return out;
}

}  // namespace engine

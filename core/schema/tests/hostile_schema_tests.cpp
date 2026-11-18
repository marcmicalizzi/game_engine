// A schema-typed `f32` given a number past the float's range (T187, from T13's reading,
// docs/experiments/fuzzing-design.md): every method's params, every record and every schema-typed
// file reach this reader, and the cast it made was undefined behaviour that came out as infinity.
// Each input is text a function returns, so the fuzz harness can lift it as a seed.

#include <core/json/json.h>
#include <core/schema/json_reflect.h>

#include <doctest/doctest.h>

#include <limits>
#include <schemas/features.h>
#include <string>

using namespace engine;
using namespace engine::schema;
using namespace engine::schema_test;

namespace hostile_schema {

// `Inner::x` is an `f32`; 1e39 is past FLT_MAX (about 3.4e38), and finite as a double.
std::string f32_past_range() { return R"({"x": 1e39})"; }

// A `vec3`'s component, read through the same cast.
std::string vec3_component_past_range() { return R"({"position": [0, -1e39, 0]})"; }

}  // namespace hostile_schema

namespace {

template <class T>
bool read_text(const std::string& text, T& out, std::string& joined) {
  JsonValue in;
  REQUIRE(parse_json(text, in).ok);
  ReadContext ctx;
  const bool ok = from_json(out, in, ctx) && ctx.ok();
  for (const Diagnostic& d : ctx.diagnostics)
    joined += d.path + ": " + d.message + "\n";
  return ok;
}

}  // namespace

TEST_CASE("schema hostile: an f32 given a number past the float's range is refused, not infinity") {
  std::string joined;
  Inner inner;
  CHECK_FALSE(read_text(hostile_schema::f32_past_range(), inner, joined));
  CHECK(joined.find("x: ") != std::string::npos);
  CHECK(joined.find("past a 32-bit float's range") != std::string::npos);

  Spatial spatial;
  joined.clear();
  CHECK_FALSE(read_text(hostile_schema::vec3_component_past_range(), spatial, joined));
  CHECK(joined.find("position") != std::string::npos);
  CHECK(joined.find("past a 32-bit float's range") != std::string::npos);

  // The largest float itself, and its negation, read as themselves.
  joined.clear();
  Inner edge;
  REQUIRE_MESSAGE(read_text(R"({"x": -3.4028234663852886e38, "y": 3.4028234663852886e38})", edge,
                            joined),
                  joined);
  CHECK(edge.x == -std::numeric_limits<f32>::max());
  CHECK(edge.y == std::numeric_limits<f32>::max());
}

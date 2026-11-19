// The schema walk over JSON text without a tree (roadmap D40; core/schema/json_text.h), held to
// the tree path it stands in for: the feature types through the property of
// tests/support/schema_text_property.h (the engine's own types run it in domain/doc's tests); the
// texts the stream hands to the tree path (keys out of order or twice, a former name); T187's
// hostile values; the parser and the reader refusing one way.

#include <core/json/json.h>
#include <core/json/json_reader.h>
#include <core/schema/json_reflect.h>
#include <core/schema/json_text.h>

#include <doctest/doctest.h>

#include <schema_text_property.h>
#include <schemas/features.h>
#include <string>

using namespace engine;
using namespace engine::schema;
using namespace engine::test::schema_text;

TEST_CASE("json text: every registered type reads the same both ways, and writes as write_json") {
  u32 types = 0;
  u32 texts = 0;
  run_property(0xD40D40D40ull, 12, types, texts);
  MESSAGE(types << " types, " << texts << " texts");
  CHECK(types >= 8);
}

TEST_CASE("json text: keys out of order, twice, or under a former name take the tree path") {
  using namespace engine::schema_test;
  const TypeInfo& inner = type_of<Inner>();
  // Out of order: the tree reads in its sorted order; the diagnostics come in that order too.
  check_same(inner, R"({"y": 2, "x": 1})");
  check_same(inner, R"({"z": 1, "y": "no", "a": 2})");
  // One key twice: the last wins.
  check_same(inner, R"({"x": 1, "x": 2})");
  check_same(type_of<Everything>(), R"({"inners": [{"x": 1}], "inners": []})");
  // A former name, alone, beside its field's name, and two former names (D28).
  check_same(type_of<Renamed>(), R"({"name": "a"})");
  check_same(type_of<Renamed>(), R"({"label": "a", "name": "b"})");
  check_same(type_of<Renamed>(), R"({"name": "a", "title": "b"})");

  bool streamed = true;
  Renamed r;
  ReadContext ctx;
  JsonParseResult parse;
  CHECK(read_json_text(r, R"({"name": "a"})", ctx, parse, {}, &streamed));
  CHECK_FALSE(streamed);
  CHECK(r.label == "a");
  CHECK(read_json_text(r, R"({"label": "b"})", ctx, parse, {}, &streamed));
  CHECK(streamed);
}

TEST_CASE("json text: the strict mode refuses a key given twice on the walk as the parser does") {
  // C61's `refuse_duplicate_keys` lives in the reader's `next_member`, which the walk, a skipped
  // value and the tree path all go through: the refusal is the parser's, word for word.
  using namespace engine::schema_test;
  TextReadOptions strict;
  strict.parse.refuse_duplicate_keys = true;
  const auto refused = [&](const char* text, const char* key) {
    JsonValue tree;
    const JsonParseResult tree_parse = parse_json(text, tree, strict.parse);
    CHECK_FALSE(tree_parse.ok);
    CHECK(tree_parse.duplicate_key == key);
    Everything object;
    ReadContext ctx;
    JsonParseResult parse;
    bool streamed = true;
    CHECK_FALSE(read_json_text(object, text, ctx, parse, strict, &streamed));
    CHECK_FALSE(parse.ok);
    CHECK(parse.duplicate_key == key);
    CHECK(parse.offset == tree_parse.offset);
    CHECK(std::string(parse.message) == "duplicate key in object");
  };
  refused(R"({"inners": [{"x": 1, "y": 2, "x": 3}]})", "x");
  refused(R"({"inners": [], "inners": [{"x": 1}]})", "inners");
  // In a value the walk passes over, a key it does not know.
  refused(R"({"unknown": {"k": 1, "k": 2}, "inners": []})", "k");
  // The same key in two objects is no duplicate, and the default is unchanged.
  Everything ok;
  ReadContext ctx;
  JsonParseResult parse;
  CHECK(read_json_text(ok, R"({"inners": [{"x": 1}, {"x": 2}]})", ctx, parse, strict));
  CHECK(parse.ok);
  CHECK(read_json_text(ok, R"({"inners": [{"x": 1, "x": 2}]})", ctx, parse));
  CHECK(parse.ok);
}

TEST_CASE("json text: T187's hostile values are refused both ways, word for word") {
  using namespace engine::schema_test;
  check_same(type_of<Inner>(), R"({"x": 1e39})");
  check_same(type_of<Spatial>(), R"({"position": [0, -1e39, 0]})");
  check_same(type_of<Inner>(), R"({"x": -3.4028234663852886e38, "y": 3.4028234663852886e38})");
  // Shapes a field does not take, scalars where containers go and the reverse.
  check_same(type_of<Everything>(), R"({"inners": {"x": 1}})");
  check_same(type_of<Everything>(), R"([1, 2])");
  check_same(type_of<Spatial>(), R"({"position": [1, 2]})");
  check_same(type_of<Spatial>(), R"({"position": [1, "2", [3]]})");
  check_same(type_of<Spatial>(), R"({"position": [1, 2, 3, 4, 5]})");
  // The parser's depth budget, through the reflection walk and through a json field.
  std::string deep(300, '[');
  deep += std::string(300, ']');
  check_same(type_of<Inner>(), "{\"x\": " + deep + "}");
}

TEST_CASE("json text: a field read through a tree is prepared as the tree path's caller did") {
  using namespace engine::schema_test;
  // Everything's `inners` emptied before it is read, whatever it holds: the scene reader's
  // "without water channels".
  const TreeField emptied{"inners",
                          [](JsonValue& v, ReadContext&, void*) {
                            v = JsonValue::array();
                            return true;
                          },
                          nullptr};
  TextReadOptions options;
  options.tree_fields = std::span<const TreeField>(&emptied, 1);
  const std::string text = R"({"inners": [{"x": "not a number"}], "name": "n"})";
  Instance a(type_of<Everything>());
  Instance b(type_of<Everything>());
  const Outcome tree = by_tree(type_of<Everything>(), a.p, text, options);
  bool streamed = false;
  const Outcome stream = by_text(type_of<Everything>(), b.p, text, &streamed, options);
  CHECK(tree.ok == stream.ok);
  CHECK(tree.diagnostics == stream.diagnostics);
  CHECK(type_of<Everything>().ops->equals(a.p, b.p));
  CHECK(streamed);
}

TEST_CASE("json reader: parse_json and the reader refuse one way") {
  for (const char* text : {"", "{", "[1,]", "{\"a\" 1}", "{\"a\":1,}", "tru", "\"\\x\"", "01", "1.",
                           "-", "1e", "\"\\ud800\"", "[1] 2", "{\"a\":[}", "  ", "nul"}) {
    JsonValue v;
    const JsonParseResult tree = parse_json(text, v);
    JsonReader reader(text);
    JsonValue built;
    const bool ok = reader.read_value(built) && reader.finish();
    const JsonParseResult r = reader.result();
    INFO(text);
    CHECK(ok == tree.ok);
    CHECK(std::string(r.message) == tree.message);
    CHECK(r.offset == tree.offset);
  }
  // Strings without an escape are views of the text; with one, decoded.
  JsonReader reader(R"(["plain", "esc\naped"])");
  JsonReader::Event e;
  bool more = false;
  REQUIRE(reader.next_value(e));
  REQUIRE(reader.next_element(more));
  REQUIRE(reader.next_value(e));
  CHECK(reader.string_value() == "plain");
  REQUIRE(reader.next_element(more));
  REQUIRE(reader.next_value(e));
  CHECK(reader.string_value() == "esc\naped");
  REQUIRE(reader.next_element(more));
  CHECK_FALSE(more);
  CHECK(reader.finish());
}

#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/schema/type_info.h>

#include <doctest/doctest.h>

#include <cstring>
#include <schemas/features.h>
#include <string>

using namespace engine;
using namespace engine::schema;
using namespace engine::schema_test;

TEST_CASE("schema: generated types have the declared defaults") {
  Everything e;
  CHECK(e.b == true);
  CHECK(e.u8v == 255);
  CHECK(e.i64v == -7);
  CHECK(e.f64v == 0.25);
  CHECK(e.s == "hello");
  CHECK(e.blob.empty());
  CHECK(e.id.is_null());
  CHECK(e.color == Color::Green);
  CHECK(e.inner.x == 1.5f);
  CHECK(e.inner.y == -2.0f);
  CHECK(e.inner.name == "inner");
  CHECK_FALSE(e.opt_i.has_value());
  CHECK_FALSE(e.opt_inner.has_value());
  CHECK(e.ints.empty());
  CHECK(e.fixed[2] == 0);
  CHECK(e.by_name.empty());
  CHECK(e.added_later == "v3");
  CHECK(Everything::k_schema_version == 3);
  CHECK(Inner::k_schema_version == 1);
  Everything f;
  CHECK(e == f);
  f.s = "changed";
  CHECK_FALSE(e == f);
}

TEST_CASE("schema: type descriptors and the registry") {
  const TypeInfo& info = type_of<Everything>();
  CHECK(std::string(info.qualified_name) == "engine.schema_test.Everything");
  CHECK(std::string(info.name) == "Everything");
  CHECK(std::string(info.ns) == "engine.schema_test");
  CHECK(info.kind == Kind::Struct);
  CHECK(info.version == 3);
  CHECK(std::string(info.tag) == "record");
  CHECK(std::string(info.doc) == "Everything the reflection layer must handle.");
  CHECK(info.size == sizeof(Everything));
  CHECK(info.fields.size() == 19);

  const FieldInfo* blob = info.find_field("blob");
  REQUIRE(blob != nullptr);
  CHECK(blob->type.kind == Kind::Bytes);
  CHECK(std::string(blob->doc) == "Raw bytes, serialized as hex.");
  CHECK(blob->offset == offsetof(Everything, blob));

  const FieldInfo* added = info.find_field("added_later");
  REQUIRE(added != nullptr);
  CHECK(added->since_version == 3);
  CHECK(info.find_field("scratch")->flags == FieldFlag::transient);
  CHECK(info.find_field("old")->flags == FieldFlag::deprecated);

  const FieldInfo* by_id = info.find_field("by_id");
  REQUIRE(by_id != nullptr);
  CHECK(by_id->type.kind == Kind::Map);
  CHECK(by_id->type.key->kind == Kind::U64);
  CHECK(by_id->type.element->kind == Kind::String);
  CHECK(by_id->type.map_ops != nullptr);

  const FieldInfo* fixed = info.find_field("fixed");
  REQUIRE(fixed != nullptr);
  CHECK(fixed->type.kind == Kind::FixedArray);
  CHECK(fixed->type.fixed_count == 3);
  CHECK(fixed->type.element->kind == Kind::U16);

  const FieldInfo* inners = info.find_field("inners");
  REQUIRE(inners != nullptr);
  CHECK(inners->type.kind == Kind::Array);
  CHECK(inners->type.element->kind == Kind::Struct);
  CHECK(inners->type.element->type == &type_of<Inner>());

  const TypeInfo& color = type_of<Color>();
  CHECK(color.kind == Kind::Enum);
  CHECK(color.enum_underlying == Kind::U8);
  CHECK(color.values.size() == 3);
  CHECK(color.find_value("Blue")->value == 2);
  CHECK(std::string(color.find_value(2)->name) == "Blue");
  CHECK(std::string(color.find_value(2)->doc) == "The third colour.");
  CHECK(color.find_value("Purple") == nullptr);
  CHECK(std::string(color.doc) == "A small enum with an explicit underlying type.");

  Registry& reg = Registry::global();
  CHECK(reg.find("engine.schema_test.Everything") == &info);
  CHECK(reg.find("engine.schema_test.Color") == &color);
  CHECK(reg.find("engine.schema_test.Inner") == &type_of<Inner>());
  CHECK(reg.find("nope") == nullptr);
  CHECK(reg.all().size() >= 3);

  // Struct ops work through the type-erased table.
  alignas(Everything) std::byte storage[sizeof(Everything)];
  info.ops->construct(storage);
  Everything defaults;
  CHECK(info.ops->equals(storage, &defaults));
  defaults.s = "other";
  info.ops->copy_assign(storage, &defaults);
  CHECK(info.ops->equals(storage, &defaults));
  info.ops->destroy(storage);
}

TEST_CASE("schema: JSON round trip through reflection, canonical form pinned") {
  Everything e;
  e.b = false;
  e.u8v = 7;
  e.i64v = -1234567890123ll;
  e.f64v = 3.5;
  e.s = "text with \"quotes\"";
  e.blob = {0xDE, 0xAD, 0xBE, 0xEF};
  e.id = Id128::from_parts(0x0011223344556677ull, 0x8899AABBCCDDEEFFull);
  e.color = Color::Blue;
  e.inner.x = 2.0f;
  e.inner.name = "in";
  e.opt_i = 42;
  e.opt_inner = Inner{};
  e.opt_inner->y = 9.0f;
  e.ints = {3, 1, 2};
  e.fixed = {10, 20, 30};
  e.inners.push_back(Inner{});
  e.inners.push_back(Inner{0.5f, 0.25f, "second"});
  e.by_name.insert("b", 2);
  e.by_name.insert("a", 1);
  e.by_id.insert(u64{5}, "five");
  e.by_id.insert(u64{1}, "one");
  e.added_later = "later";
  e.scratch = 999;  // transient: must not appear
  e.old = -3;

  const JsonValue json = to_json(e);
  const std::string text = write_json(json);
  const std::string expected =
      "{\n"
      "  \"added_later\": \"later\",\n"
      "  \"b\": false,\n"
      "  \"blob\": \"deadbeef\",\n"
      "  \"by_id\": [\n"
      "    [\n"
      "      1,\n"
      "      \"one\"\n"
      "    ],\n"
      "    [\n"
      "      5,\n"
      "      \"five\"\n"
      "    ]\n"
      "  ],\n"
      "  \"by_name\": {\n"
      "    \"a\": 1,\n"
      "    \"b\": 2\n"
      "  },\n"
      "  \"color\": \"Blue\",\n"
      "  \"f64v\": 3.5,\n"
      "  \"fixed\": [\n"
      "    10,\n"
      "    20,\n"
      "    30\n"
      "  ],\n"
      "  \"i64v\": -1234567890123,\n"
      "  \"id\": \"00112233445566778899aabbccddeeff\",\n"
      "  \"inner\": {\n"
      "    \"name\": \"in\",\n"
      "    \"x\": 2.0,\n"
      "    \"y\": -2.0\n"
      "  },\n"
      "  \"inners\": [\n"
      "    {\n"
      "      \"name\": \"inner\",\n"
      "      \"x\": 1.5,\n"
      "      \"y\": -2.0\n"
      "    },\n"
      "    {\n"
      "      \"name\": \"second\",\n"
      "      \"x\": 0.5,\n"
      "      \"y\": 0.25\n"
      "    }\n"
      "  ],\n"
      "  \"ints\": [\n"
      "    3,\n"
      "    1,\n"
      "    2\n"
      "  ],\n"
      "  \"old\": -3,\n"
      "  \"opt_i\": 42,\n"
      "  \"opt_inner\": {\n"
      "    \"name\": \"inner\",\n"
      "    \"x\": 1.5,\n"
      "    \"y\": 9.0\n"
      "  },\n"
      "  \"s\": \"text with \\\"quotes\\\"\",\n"
      "  \"u8v\": 7\n"
      "}";
  CHECK(text == expected);

  Everything back;
  ReadContext ctx;
  REQUIRE(from_json(back, json, ctx));
  CHECK(ctx.ok());
  back.scratch = e.scratch;  // transient is not transported; equalize before comparing
  CHECK(back == e);

  // Parsing the text and reading it yields the same object.
  JsonValue parsed;
  REQUIRE(parse_json(text, parsed).ok);
  Everything again;
  ReadContext ctx2;
  REQUIRE(from_json(again, parsed, ctx2));
  again.scratch = e.scratch;
  CHECK(again == e);
}

TEST_CASE("schema: reading tolerates missing fields and reports bad ones with paths") {
  JsonValue in;
  REQUIRE(parse_json(R"({"s": "only", "color": 1, "opt_i": null})", in).ok);
  Everything e;
  ReadContext ctx;
  REQUIRE(from_json(e, in, ctx));
  CHECK(e.s == "only");
  CHECK(e.color == Color::Green);  // integer accepted
  CHECK(e.u8v == 255);             // default kept
  CHECK_FALSE(e.opt_i.has_value());

  // Errors.
  REQUIRE(
      parse_json(R"({"u8v": 300, "color": "Purple", "fixed": [1, 2], "inners": [{}, {"x": "no"}],
                         "by_id": [[1, "a"], [2]], "id": "zz", "unknown": 1})",
                 in)
          .ok);
  Everything f;
  ReadContext bad;
  CHECK_FALSE(from_json(f, in, bad));
  std::string joined;
  for (const Diagnostic& d : bad.diagnostics)
    joined += d.path + ": " + d.message + "\n";
  MESSAGE(joined);
  CHECK(joined.find("u8v: integer out of range") != std::string::npos);
  CHECK(joined.find("color: unknown enumerator") != std::string::npos);
  CHECK(joined.find("fixed: expected an array of the declared fixed length") != std::string::npos);
  CHECK(joined.find("inners[1].x: expected a number") != std::string::npos);
  CHECK(joined.find("by_id[1]: expected a [key, value] pair") != std::string::npos);
  CHECK(joined.find("id: expected a 32-character hex id") != std::string::npos);
  CHECK(joined.find("unknown: unknown field") != std::string::npos);
  CHECK(bad.diagnostics.size() == 7);

  // Unknown fields can be ignored on request.
  ReadContext lenient;
  lenient.options.ignore_unknown_fields = true;
  REQUIRE(parse_json(R"({"unknown": 1, "s": "ok"})", in).ok);
  Everything g;
  CHECK(from_json(g, in, lenient));
  CHECK(g.s == "ok");

  // Transient fields present in input are skipped, not errors.
  REQUIRE(parse_json(R"({"scratch": 5})", in).ok);
  Everything h;
  ReadContext t;
  CHECK(from_json(h, in, t));
  CHECK(h.scratch == 0);
}

TEST_CASE("schema: enums serialize by name and read by name or value") {
  JsonValue v = to_json(Color::Blue);
  CHECK(v.as_string() == "Blue");
  Color c = Color::Red;
  ReadContext ctx;
  REQUIRE(from_json(c, JsonValue("Green"), ctx));
  CHECK(c == Color::Green);
  REQUIRE(from_json(c, JsonValue(i64{2}), ctx));
  CHECK(c == Color::Blue);
  CHECK_FALSE(from_json(c, JsonValue(i64{300}), ctx));
  // An enumerator outside the declared set is written as its integer.
  const Color raw = static_cast<Color>(9);
  CHECK(to_json(raw).as_int() == 9);
}

TEST_CASE("schema: math and json primitives round-trip") {
  Spatial s;
  CHECK(s.rotation == Quat::identity());  // value-initialized Quat is the identity
  s.position = {1.5f, -2.0f, 3.25f};
  s.rotation = quat_from_axis_angle(Vec3::unit_y(), 0.5f);
  s.uv = {0.25f, 0.75f};
  s.color = {1, 0.5f, 0.25f, 1};
  s.extra = JsonValue::object();
  s.extra["anything"] = JsonValue::array();
  s.extra["anything"].push_back(i64{7});
  const std::string text = write_json(to_json(s), JsonWriteOptions{false});
  CHECK(text.find("\"position\":[1.5,-2.0,3.25]") != std::string::npos);
  CHECK(text.find("\"extra\":{\"anything\":[7]}") != std::string::npos);
  Spatial back;
  ReadContext ctx;
  REQUIRE(from_json(back, to_json(s), ctx));
  CHECK(back == s);

  // Component count is enforced.
  JsonValue bad;
  REQUIRE(parse_json(R"({"position": [1, 2]})", bad).ok);
  ReadContext bad_ctx;
  CHECK_FALSE(from_json(back, bad, bad_ctx));
  CHECK(bad_ctx.diagnostics[0].path == "position");
}

// ADR-0053. The far values of the brief: 420 km, 10,000 km, 1e8 m and 1e11 m, each one that f32
// cannot hold (419070.2 rounds to 419070.1875, -10000000.4 to -10000000.0), through JSON and back
// to the bit, and the text pinned, since a saved document is the same bytes from every toolchain:
// the writer is `std::to_chars`' shortest round trip, a function the standard fixes for a given
// double, and the reader `std::from_chars`, correctly rounded.
TEST_CASE("schema: worldpos and dvec3 round-trip far values to the bit, the text pinned") {
  Far f;
  // Defaults: value-initialized is the origin; a vector default is the doubles it names.
  CHECK(f.at == WorldPos::origin());
  CHECK(f.step == DVec3{});
  CHECK_FALSE(f.goal.has_value());
  CHECK(f.home == WorldPos{419072.0, 0.5, -10000000.25});

  f.at = WorldPos{419070.2, -10000000.4, 100000000.25};
  f.step = DVec3{1e11, -0.1, 3.0e-9};
  f.goal = WorldPos{1e11, -1e11, 419072.0};
  f.route.push_back(WorldPos{-10000000.4, 0.0, 100000000.25});
  const std::string text = write_json(to_json(f), JsonWriteOptions{false});
  CHECK(text ==
        R"({"at":[419070.2,-10000000.4,100000000.25],"goal":[1e+11,-1e+11,419072.0],)"
        R"("home":[419072.0,0.5,-10000000.25],"route":[[-10000000.4,0.0,100000000.25]],)"
        R"("step":[1e+11,-0.1,3e-09]})");

  JsonValue parsed;
  REQUIRE(parse_json(text, parsed).ok);
  Far back;
  ReadContext ctx;
  REQUIRE(from_json(back, parsed, ctx));
  CHECK(back == f);
  CHECK(back.at.x == 419070.2);
  CHECK(back.at.y == -10000000.4);
  CHECK(back.at.z == 100000000.25);
  CHECK(back.goal->x == 1e11);
  // Written again, the same text: the round trip is a fixed point.
  CHECK(write_json(to_json(back), JsonWriteOptions{false}) == text);

  // The binary form is the 24 bytes of three doubles: flat, compared by bytes for write-back.
  const FieldInfo* at = type_of<Far>().find_field("at");
  const FieldInfo* step = type_of<Far>().find_field("step");
  REQUIRE(at != nullptr);
  REQUIRE(step != nullptr);
  CHECK(at->type.kind == Kind::WorldPos);
  CHECK(step->type.kind == Kind::DVec3);
  CHECK(at->type.size == 24);
  CHECK(step->type.size == 24);
  CHECK(std::string(kind_name(at->type.kind)) == "worldpos");
  CHECK(std::string(kind_name(step->type.kind)) == "dvec3");
  WorldPos copied;
  std::memcpy(&copied, reinterpret_cast<const std::byte*>(&back) + at->offset, sizeof(copied));
  CHECK(copied == f.at);
}

TEST_CASE("schema: a worldpos from outside is finite and inside the cells, or refused by name") {
  Far f;
  const auto refused = [&](const char* json, const char* path, const char* words) {
    JsonValue v;
    REQUIRE(parse_json(json, v).ok);
    const WorldPos before = f.at;
    ReadContext ctx;
    CHECK_FALSE(from_json(f, v, ctx));
    REQUIRE_FALSE(ctx.diagnostics.empty());
    CHECK(ctx.diagnostics[0].path == path);
    CHECK(ctx.diagnostics[0].message.find(words) != std::string::npos);
    CHECK(f.at == before);  // a refused value leaves the field as it was
  };
  refused(R"({"at": [2e11, 0, 0]})", "at", "out of range");
  refused(R"({"at": [0, 0, -2e11]})", "at", "out of range");
  refused(R"({"goal": [0, 137438953408, 0]})", "goal", "out of range");
  refused(R"({"route": [[0, 0, 0], [0, 0, 2e11]]})", "route[1]", "out of range");
  refused(R"({"at": [1, 2]})", "at", "an array of 3 numbers");
  refused(R"({"at": [1, "2", 3]})", "at", "numeric");
  // The last metre a cell names is in; integers read as the same doubles.
  JsonValue edge;
  REQUIRE(parse_json(R"({"at": [137438953407, -137438953408, 419072]})", edge).ok);
  ReadContext ok;
  REQUIRE(from_json(f, edge, ok));
  CHECK(f.at == WorldPos{137438953407.0, -137438953408.0, 419072.0});
  // A displacement has no range of its own past finite.
  JsonValue far_step;
  REQUIRE(parse_json(R"({"step": [2e11, 0, 0]})", far_step).ok);
  ReadContext step_ok;
  CHECK(from_json(f, far_step, step_ok));
}

namespace {

bool migrate_everything_2_to_3(JsonValue& object, ReadContext&) {
  // Version 2 stored the field as "note"; version 3 calls it "added_later".
  if (JsonValue* note = object.find("note")) {
    object.set("added_later", std::move(*note));
    object.as_object().erase("note");
  }
  return true;
}

}  // namespace

TEST_CASE("schema: versioned reads apply registered migrations in order") {
  MigrationRegistry::global().add(
      Migration{"engine.schema_test.Everything", 2, &migrate_everything_2_to_3});
  JsonValue old;
  REQUIRE(parse_json(R"({"note": "from v2", "s": "kept"})", old).ok);

  Everything e;
  ReadContext ctx;
  REQUIRE(from_json_versioned(e, old, 2, ctx));
  CHECK(e.added_later == "from v2");
  CHECK(e.s == "kept");

  // Same version: no migration, the field is unknown.
  ReadContext strict;
  CHECK_FALSE(from_json_versioned(e, old, 3, strict));

  // A gap without a registered step fails with a clear message.
  ReadContext gap;
  CHECK_FALSE(from_json_versioned(e, old, 1, gap));
  REQUIRE(gap.diagnostics.size() == 1);
  CHECK(gap.diagnostics[0].message.find("no migration registered from version 1 to 2") !=
        std::string::npos);

  // Newer than we understand.
  ReadContext future;
  CHECK_FALSE(from_json_versioned(e, old, 4, future));
}

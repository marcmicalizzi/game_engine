#include <core/json/json.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace engine;

namespace {

JsonValue parse_ok(std::string_view text) {
  JsonValue v;
  const JsonParseResult r = parse_json(text, v);
  if (!r.ok) {
    MESSAGE("parse failed at " << r.line << ":" << r.column << ": " << r.message << " in " << std::string(text));
  }
  REQUIRE(r.ok);
  return v;
}

}  // namespace

TEST_CASE("JsonValue: scalar kinds, conversions, and equality") {
  JsonValue n;
  CHECK(n.is_null());
  CHECK(JsonValue(true).as_bool());
  CHECK(JsonValue(i64{-5}).as_int() == -5);
  CHECK(JsonValue(u64{5}).as_uint() == 5);
  CHECK(JsonValue(1.5).as_float() == 1.5);
  CHECK(JsonValue("hi").as_string() == "hi");

  i64 i;
  u64 u;
  f64 f;
  CHECK(JsonValue(u64{7}).get_i64(i));
  CHECK(i == 7);
  CHECK_FALSE(JsonValue(u64{1} << 63).get_i64(i));
  CHECK(JsonValue(i64{9}).get_u64(u));
  CHECK(u == 9);
  CHECK_FALSE(JsonValue(i64{-1}).get_u64(u));
  CHECK(JsonValue(3.0).get_i64(i));
  CHECK(i == 3);
  CHECK_FALSE(JsonValue(3.5).get_i64(i));
  CHECK(JsonValue(i64{2}).get_f64(f));
  CHECK(f == 2.0);
  CHECK_FALSE(JsonValue("x").get_f64(f));

  CHECK(JsonValue(i64{4}) == JsonValue(u64{4}));
  CHECK(JsonValue(i64{4}) == JsonValue(4.0));
  CHECK(JsonValue(i64{4}) != JsonValue(4.5));
  CHECK(JsonValue("a") != JsonValue("b"));
  CHECK(JsonValue() == JsonValue());
  static_assert(sizeof(JsonValue) == 24);
}

TEST_CASE("JsonValue: arrays and objects") {
  JsonValue arr = JsonValue::array();
  arr.push_back(i64{1});
  arr.push_back("two");
  arr.push_back(JsonValue::object());
  CHECK(arr.size() == 3);
  CHECK(arr[1].as_string() == "two");
  arr[2]["nested"] = true;
  CHECK(arr[2].find("nested")->as_bool());

  JsonValue obj = JsonValue::object();
  obj["zeta"] = i64{1};
  obj["alpha"] = i64{2};
  obj.set("mid", "m");
  CHECK(obj.size() == 3);
  CHECK(obj.contains("alpha"));
  CHECK_FALSE(obj.contains("omega"));
  CHECK(obj.find("omega") == nullptr);
  // Keys iterate sorted.
  std::string keys;
  for (auto [k, v] : obj.as_object()) keys += k + ",";
  CHECK(keys == "alpha,mid,zeta,");

  JsonValue copy = obj;
  CHECK(copy == obj);
  copy["alpha"] = i64{3};
  CHECK(copy != obj);
  JsonValue moved = std::move(copy);
  CHECK(copy.is_null());
  CHECK(moved.find("alpha")->as_int() == 3);
}

TEST_CASE("parse_json: valid documents") {
  JsonValue v = parse_ok(R"({"a": [1, 2.5, -3, true, false, null, "s\n\"q\""], "b": {"c": {}}, "d": []})");
  REQUIRE(v.is_object());
  const JsonValue& a = *v.find("a");
  REQUIRE(a.is_array());
  CHECK(a.size() == 7);
  CHECK(a[0].is_int());
  CHECK(a[1].is_float());
  CHECK(a[2].as_int() == -3);
  CHECK(a[3].as_bool());
  CHECK_FALSE(a[4].as_bool());
  CHECK(a[5].is_null());
  CHECK(a[6].as_string() == "s\n\"q\"");
  CHECK(v.find("b")->find("c")->is_object());
  CHECK(v.find("d")->as_array().empty());

  CHECK(parse_ok("  42  ").as_int() == 42);
  CHECK(parse_ok("18446744073709551615").as_uint() == 18446744073709551615ull);
  CHECK(parse_ok("9223372036854775807").as_int() == INT64_MAX);
  CHECK(parse_ok("-9223372036854775808").as_int() == INT64_MIN);
  CHECK(parse_ok("1e3").as_float() == 1000.0);
  CHECK(parse_ok("1E-2").as_float() == 0.01);
  CHECK(parse_ok("-0.0").is_float());
  CHECK(parse_ok("\"\\u00e9\"").as_string() == "\xC3\xA9");
  CHECK(parse_ok("\"\\ud83d\\ude00\"").as_string() == "\xF0\x9F\x98\x80");
  CHECK(parse_ok("\"\\/\"").as_string() == "/");
  // Duplicate keys: last wins.
  CHECK(parse_ok(R"({"k": 1, "k": 2})").find("k")->as_int() == 2);
}

TEST_CASE("parse_json: errors are reported with positions and leave the value null") {
  struct Case {
    const char* text;
    u32 line;
  };
  const Case cases[] = {
      {"", 1},           {"{", 1},           {"[1,]", 1},         {"{\"a\" 1}", 1},
      {"tru", 1},        {"01", 1},          {"1.", 1},           {"\"abc", 1},
      {"\"\\x\"", 1},    {"[1] x", 1},       {"{\n\"a\": }", 2},  {"\"\\ud83d\"", 1},
      {"\"a\tb\"", 1},   {"-", 1},           {"1e", 1},
  };
  for (const Case& c : cases) {
    JsonValue v(i64{99});
    const JsonParseResult r = parse_json(c.text, v);
    CHECK_FALSE(r.ok);
    CHECK(v.is_null());
    CHECK(r.line == c.line);
    CHECK(std::string(r.message).size() > 0);
  }
  // Depth budget: refused by default, accepted when raised.
  std::string deep(300, '[');
  deep.append(300, ']');
  JsonValue v;
  CHECK_FALSE(parse_json(deep, v).ok);
  JsonParseOptions opts;
  opts.max_depth = 512;
  CHECK(parse_json(deep, v, opts).ok);
  CHECK(v.is_array());
}

TEST_CASE("write_json: canonical form is stable and round-trips") {
  JsonValue v = JsonValue::object();
  v["zeta"] = i64{-1};
  v["alpha"] = JsonValue::array();
  v["alpha"].push_back(u64{18446744073709551615ull});
  v["alpha"].push_back(0.1);
  v["alpha"].push_back(1.0);
  v["alpha"].push_back(-0.0);
  v["alpha"].push_back(1e21);
  v["alpha"].push_back(JsonValue::object());
  v["mid"] = "tab\tquote\"back\\slash\x01";
  v["flag"] = true;
  v["none"] = JsonValue();

  const std::string pretty = write_json(v);
  const std::string expected =
      "{\n"
      "  \"alpha\": [\n"
      "    18446744073709551615,\n"
      "    0.1,\n"
      "    1.0,\n"
      "    -0.0,\n"
      "    1e+21,\n"
      "    {}\n"
      "  ],\n"
      "  \"flag\": true,\n"
      "  \"mid\": \"tab\\tquote\\\"back\\\\slash\\u0001\",\n"
      "  \"none\": null,\n"
      "  \"zeta\": -1\n"
      "}";
  CHECK(pretty == expected);

  JsonValue back;
  REQUIRE(parse_json(pretty, back).ok);
  CHECK(back == v);
  CHECK(write_json(back) == pretty);  // idempotent

  const std::string compact = write_json(v, JsonWriteOptions{false});
  CHECK(compact == R"({"alpha":[18446744073709551615,0.1,1.0,-0.0,1e+21,{}],"flag":true,"mid":"tab\tquote\"back\\slash\u0001","none":null,"zeta":-1})");
  JsonValue back2;
  REQUIRE(parse_json(compact, back2).ok);
  CHECK(back2 == v);
}

TEST_CASE("write_json: floats round-trip exactly through shortest representation") {
  const f64 values[] = {3.14159, 1.0 / 3.0, 123456789.123456789, 5e-324, 1.7976931348623157e308, 100.0, 0.5};
  for (f64 d : values) {
    JsonValue v(d);
    JsonValue back;
    REQUIRE(parse_json(write_json(v), back).ok);
    CHECK(back.as_float() == d);
  }
  std::string out;
  CHECK_FALSE(write_json(JsonValue(std::nan("")), out));
  CHECK(out == "null");
}

TEST_CASE("write_json: UTF-8 passes through unescaped") {
  JsonValue v("héllo 😀");
  CHECK(write_json(v) == "\"héllo 😀\"");
  JsonValue back;
  REQUIRE(parse_json(write_json(v), back).ok);
  CHECK(back == v);
}

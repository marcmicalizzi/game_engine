#include <core/json/json.h>
#include <foundation/tunables/tunables.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <limits>
#include <string>

using namespace engine;

namespace {

tunables::Int t_batch{"test.tunables.batch", 256, 16, 4096, "Batch size"};
tunables::Float t_scale{"test.tunables.scale", 1.5, 0.0, 10.0, "Scale factor"};
tunables::Bool t_flag{"test.tunables.flag", false, "A flag"};

enum class Variant : u32 { Scalar = 0, Avx2 = 1, Avx512 = 2 };
constexpr const char* k_variants[] = {"scalar", "avx2", "avx512"};
tunables::Enum<Variant> t_variant{"test.tunables.variant", Variant::Scalar, k_variants,
                                  "Kernel variant"};

struct ResetAll {
  ResetAll() { tunables::reset_all(); }
  ~ResetAll() { tunables::reset_all(); }
};

const JsonValue& at(const JsonValue& object, std::string_view key) {
  const JsonValue* v = object.find(key);
  REQUIRE(v != nullptr);
  return *v;
}

std::string text_of(const tunables::Tunable& t) {
  std::string s;
  t.append_value(s);
  return s;
}

}  // namespace

TEST_CASE("tunables: static registration, lookup, and kinds") {
  ResetAll guard;
  CHECK(tunables::find("test.tunables.batch") == &t_batch);
  CHECK(tunables::find("test.tunables.variant") == &t_variant);
  CHECK(tunables::find("test.tunables.nope") == nullptr);
  CHECK(tunables::count() >= 4);
  CHECK(t_batch.kind() == tunables::Kind::Int);
  CHECK(t_scale.kind() == tunables::Kind::Float);
  CHECK(t_flag.kind() == tunables::Kind::Bool);
  CHECK(t_variant.kind() == tunables::Kind::Enum);
  CHECK(std::string_view(tunables::kind_name(tunables::Kind::Enum)) == "enum");
  CHECK(std::string_view(t_batch.doc()) == "Batch size");
  {
    tunables::Int temporary{"test.tunables.temporary", 1, 0, 2, ""};
    CHECK(tunables::find("test.tunables.temporary") == &temporary);
  }
  CHECK(tunables::find("test.tunables.temporary") == nullptr);
}

TEST_CASE("tunables: Int enforces its range and tracks versions") {
  ResetAll guard;
  CHECK(t_batch.get() == 256);
  CHECK(t_batch.is_default());
  const u32 v0 = t_batch.version();
  const u64 g0 = tunables::generation();
  CHECK(t_batch.set(512));
  CHECK(t_batch.get() == 512);
  CHECK_FALSE(t_batch.is_default());
  CHECK(t_batch.version() == v0 + 1);
  CHECK(tunables::generation() == g0 + 1);
  CHECK(t_batch.set(512));  // same value: accepted, no version bump
  CHECK(t_batch.version() == v0 + 1);
  CHECK_FALSE(t_batch.set(8));
  CHECK_FALSE(t_batch.set(5000));
  CHECK(t_batch.get() == 512);

  std::string error;
  CHECK(t_batch.set_from_text(" 1024 ", &error));
  CHECK(t_batch.get() == 1024);
  CHECK_FALSE(t_batch.set_from_text("abc", &error));
  CHECK(error.find("test.tunables.batch") != std::string::npos);
  CHECK(error.find("integer") != std::string::npos);
  CHECK_FALSE(t_batch.set_from_text("99999", &error));
  CHECK(error.find("range") != std::string::npos);
  CHECK(t_batch.get() == 1024);
  CHECK(t_batch.set_from_json(JsonValue(64.0), &error));  // integral float accepted
  CHECK(t_batch.get() == 64);
  CHECK_FALSE(t_batch.set_from_json(JsonValue(64.5), &error));
  CHECK_FALSE(t_batch.set_from_json(JsonValue("64"), &error));
  CHECK(text_of(t_batch) == "64");
  t_batch.reset();
  CHECK(t_batch.get() == 256);
  CHECK(t_batch.is_default());
}

TEST_CASE("tunables: Float, Bool, and Enum text and JSON forms") {
  ResetAll guard;
  std::string error;
  CHECK(t_scale.set_from_text("2.25", &error));
  CHECK(t_scale.get() == 2.25);
  CHECK_FALSE(t_scale.set(11.0));
  CHECK_FALSE(t_scale.set(std::numeric_limits<f64>::quiet_NaN()));
  CHECK_FALSE(t_scale.set_from_text("x", &error));
  CHECK(text_of(t_scale) == "2.25");
  CHECK(t_scale.set_from_json(JsonValue(3), &error));
  CHECK(t_scale.get() == 3.0);

  for (const char* text : {"true", "ON", "Yes", "1"}) {
    t_flag.set(false);
    CHECK(t_flag.set_from_text(text, &error));
    CHECK(t_flag.get());
  }
  for (const char* text : {"false", "off", "NO", "0"}) {
    t_flag.set(true);
    CHECK(t_flag.set_from_text(text, &error));
    CHECK_FALSE(t_flag.get());
  }
  CHECK_FALSE(t_flag.set_from_text("maybe", &error));
  CHECK(t_flag.set_from_json(JsonValue(true), &error));
  CHECK(t_flag.get());
  CHECK_FALSE(t_flag.set_from_json(JsonValue(1), &error));

  CHECK(t_variant.get() == Variant::Scalar);
  CHECK(t_variant.set_from_text("AVX2", &error));
  CHECK(t_variant.get() == Variant::Avx2);
  CHECK(text_of(t_variant) == "avx2");
  CHECK_FALSE(t_variant.set_from_text("neon", &error));
  CHECK(error.find("scalar avx2 avx512") != std::string::npos);
  CHECK(t_variant.set_from_json(JsonValue(u64{2}), &error));
  CHECK(t_variant.get() == Variant::Avx512);
  CHECK_FALSE(t_variant.set_from_json(JsonValue(u64{3}), &error));
  CHECK(t_variant.set(Variant::Scalar));
  CHECK(t_variant.value_json() == JsonValue("scalar"));
  CHECK(t_variant.choices().size() == 3);
}

TEST_CASE("tunables: describe carries range or choices") {
  ResetAll guard;
  (void)t_batch.set(300);
  const JsonValue d = t_batch.describe();
  CHECK(at(d, "name") == JsonValue("test.tunables.batch"));
  CHECK(at(d, "kind") == JsonValue("int"));
  CHECK(at(d, "value") == JsonValue(i64{300}));
  CHECK(at(d, "default") == JsonValue(i64{256}));
  CHECK(at(d, "min") == JsonValue(i64{16}));
  CHECK(at(d, "max") == JsonValue(i64{4096}));
  CHECK(at(d, "modified") == JsonValue(true));
  CHECK(at(d, "doc") == JsonValue("Batch size"));
  const JsonValue e = t_variant.describe();
  CHECK(at(e, "choices").size() == 3);
  CHECK(at(e, "choices")[1] == JsonValue("avx2"));
  CHECK_FALSE(e.contains("min"));
  const JsonValue all = tunables::describe_all();
  CHECK(all.is_array());
  CHECK(all.size() == tunables::count());
  // Name order.
  for (usize i = 1; i < all.size(); ++i) {
    CHECK(at(all[i - 1], "name").as_string() < at(all[i], "name").as_string());
  }
}

TEST_CASE("tunables: overrides apply every valid entry and report the first problem") {
  ResetAll guard;
  std::string error;
  CHECK(tunables::apply_overrides("test.tunables.batch=1024, test.tunables.flag=on", &error));
  CHECK(t_batch.get() == 1024);
  CHECK(t_flag.get());
  CHECK_FALSE(tunables::apply_overrides(
      "test.tunables.batch=2048,test.tunables.nope=1,test.tunables.variant=avx512", &error));
  CHECK(t_batch.get() == 2048);               // applied before the problem
  CHECK(t_variant.get() == Variant::Avx512);  // applied after the problem
  CHECK(error.find("unknown tunable") != std::string::npos);
  CHECK(error.find("test.tunables.nope") != std::string::npos);
  CHECK_FALSE(tunables::apply_overrides("garbage", &error));
  CHECK(error.find("malformed") != std::string::npos);
  CHECK_FALSE(tunables::apply_overrides("test.tunables.batch=99999", &error));
  CHECK(error.find("range") != std::string::npos);
  CHECK(tunables::apply_overrides("", &error));
}

TEST_CASE("tunables: JSON round trip saves only modified values by default") {
  ResetAll guard;
  CHECK(tunables::save_json().size() == 0);
  (void)t_batch.set(2048);
  (void)t_variant.set(Variant::Avx2);
  const JsonValue modified = tunables::save_json();
  CHECK(modified.size() == 2);
  CHECK(at(modified, "test.tunables.batch") == JsonValue(i64{2048}));
  CHECK(at(modified, "test.tunables.variant") == JsonValue("avx2"));
  const JsonValue everything = tunables::save_json(false);
  CHECK(everything.size() == tunables::count());

  tunables::reset_all();
  CHECK(t_batch.is_default());
  CHECK(t_variant.is_default());
  Vector<std::string> problems;
  CHECK(tunables::load_json(modified, &problems));
  CHECK(problems.empty());
  CHECK(t_batch.get() == 2048);
  CHECK(t_variant.get() == Variant::Avx2);

  JsonValue bad = JsonValue::object();
  bad.set("test.tunables.batch", JsonValue(i64{1}));
  bad.set("test.tunables.unknown", JsonValue(true));
  bad.set("test.tunables.flag", JsonValue(true));
  CHECK_FALSE(tunables::load_json(bad, &problems));
  CHECK(problems.size() == 2);
  CHECK(t_flag.get());  // valid entries still apply
  CHECK_FALSE(tunables::load_json(JsonValue(3), &problems));
}

TEST_CASE("tunables: file round trip") {
  ResetAll guard;
  const test::TempDir tmp("engine_tunables");
  const std::string path = tmp.file("tunables.json");
  (void)t_scale.set(4.5);
  t_flag.set(true);
  REQUIRE(tunables::save_file(path.c_str()));
  tunables::reset_all();
  Vector<std::string> problems;
  CHECK(tunables::load_file(path.c_str(), &problems));
  CHECK(problems.empty());
  CHECK(t_scale.get() == 4.5);
  CHECK(t_flag.get());
  std::filesystem::remove(std::filesystem::path(path));
  CHECK_FALSE(tunables::load_file(path.c_str(), &problems));
  CHECK(problems.size() == 1);
}

// engine-content vine and cover end to end (docs/subsystems/cover.md, "The build"; apps.md): a
// small boulder's rock file and a vine's and a cover's recipe over it written into the test's
// scratch directory; the vine built into a named container and again as a cache hit; its key
// moving when the host's recipe does; `build` taking the recipe through the registry under the same
// key; the cover's mask, one word a host vertex; and, without the capability, the refusal.
#include <core/json/json.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <fstream>
#include <string>
#include <vector>

using namespace engine;

namespace {

struct Output {
  i32 exit_code = -1;
  std::string text;
};

Output run(std::vector<std::string> args) {
  static const std::string exe = test::app_path(ENGINE_APP_PATH);
  std::vector<std::string_view> argv;
  argv.push_back(exe);
  for (const std::string& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Output out;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error)) {
    FAIL("cannot spawn engine-content: " << error);
    return out;
  }
  p.close_stdin();
  p.read_all(out.text);
  out.exit_code = p.wait();
  return out;
}

void write_text(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
}

}  // namespace

#if ENGINE_CONTENT_COVER && ENGINE_CONTENT_ROCKS

namespace {

JsonValue line_of(const Output& o) {
  JsonValue v;
  REQUIRE_MESSAGE(parse_json(o.text, v).ok, o.text);
  return v;
}

u64 number(const JsonValue& object, const char* key) {
  u64 v = 0;
  const JsonValue* value = object.find(key);
  return value != nullptr && value->get_u64(v) ? v : ~u64{0};
}

bool flag(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  return value != nullptr && value->is_bool() && value->as_bool();
}

constexpr const char* k_rock =
    "{\"shape\":\"Boulder\",\"boulder\":{\"seed\":5,\"type\":\"Granite\",\"triangle_budget\":3000}"
    "}";
constexpr const char* k_vine =
    "{\"host\":\"small.rock.json\",\"vine\":{\"seeds\":2,\"max_length_m\":1.5}}";
constexpr const char* k_cover =
    "{\"host\":\"small.rock.json\",\"site\":{\"wetness\":0.8,\"canopy_shade\":0.6},"
    "\"cover\":{\"age_years\":40}}";

}  // namespace

TEST_CASE("engine-content vine and cover: grown over a host, keyed by it, built once") {
  const test::TempDir tmp("engine_content_cover");
  REQUIRE(tmp.ok());
  write_text(tmp.file("small.rock.json"), k_rock);
  const std::string vine = tmp.file("ivy.vine.json");
  const std::string cover = tmp.file("moss.cover.json");
  write_text(vine, k_vine);
  write_text(cover, k_cover);

  const std::string out = tmp.file("ivy.clusters");
  const Output first = run({"vine", vine, out, "--no-materials"});
  REQUIRE_MESSAGE(first.exit_code == 0, first.text);
  const JsonValue a = line_of(first);
  CHECK_FALSE(flag(a, "cached"));
  const JsonValue* report = a.find("vine");
  REQUIRE(report != nullptr);
  CHECK(number(*report, "held_steps") > 0u);
  CHECK(number(*report, "stems") >= 2u);
  CHECK(number(a, "triangles") > 0u);

  const Output again = run({"vine", vine, out, "--no-materials"});
  REQUIRE_MESSAGE(again.exit_code == 0, again.text);
  const JsonValue b = line_of(again);
  CHECK(flag(b, "cached"));
  CHECK(number(b, "source_hash") == number(a, "source_hash"));

  // `build` takes the recipe through the registry under the same source hash.
  const Output built = run({"build", vine, tmp.file("built.clusters")});
  REQUIRE_MESSAGE(built.exit_code == 0, built.text);
  CHECK(number(line_of(built), "source_hash") == number(a, "source_hash"));

  // A changed host is a new key.
  write_text(tmp.file("small.rock.json"),
             "{\"shape\":\"Boulder\",\"boulder\":{\"seed\":6,\"type\":\"Granite\","
             "\"triangle_budget\":3000}}");
  const Output moved = run({"vine", vine, out, "--no-materials"});
  REQUIRE_MESSAGE(moved.exit_code == 0, moved.text);
  const JsonValue c = line_of(moved);
  CHECK_FALSE(flag(c, "cached"));
  CHECK(number(c, "source_hash") != number(a, "source_hash"));

  const Output grown = run({"cover", cover, tmp.file("moss.clusters"), "--no-materials"});
  REQUIRE_MESSAGE(grown.exit_code == 0, grown.text);
  const JsonValue d = line_of(grown);
  CHECK(number(d, "cover_mask") > 0u);
  CHECK(d.find("cover") != nullptr);
}

TEST_CASE("engine-content vine: a recipe whose host is missing is refused with a sentence") {
  const test::TempDir tmp("engine_content_cover_refused");
  REQUIRE(tmp.ok());
  const std::string vine = tmp.file("ivy.vine.json");
  write_text(vine, "{\"host\":\"missing.rock.json\"}");
  const Output o = run({"vine", vine, tmp.file("x.clusters"), "--no-materials"});
  CHECK(o.exit_code == 1);
}

#else

TEST_CASE("engine-content vine: refused with a sentence where the capability is off") {
  const test::TempDir tmp("engine_content_cover_off");
  REQUIRE(tmp.ok());
  const std::string vine = tmp.file("ivy.vine.json");
  write_text(vine, "{\"host\":\"a.rock.json\"}");
  CHECK(run({"vine", vine, tmp.file("x.clusters")}).exit_code == 1);
}

#endif

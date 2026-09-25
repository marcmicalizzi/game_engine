// engine-content ruins and ruins-kit end to end (docs/subsystems/ruins.md, apps.md): the kit of
// boxes written into the test's scratch directory, one tile's building written as a scene
// fragment and read back, the far tier leaving only the debris out, and the refusals. The golden
// hash is in determinism_tests.cpp, with the content build's.
#include <core/json/json.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <fstream>
#include <iterator>
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

u64 number(const JsonValue& object, const char* key) {
  u64 v = 0;
  const JsonValue* value = object.find(key);
  return value != nullptr && value->get_u64(v) ? v : ~u64{0};
}

}  // namespace

TEST_CASE("engine-content ruins: a tile's building as a scene fragment") {
  const test::TempDir tmp("engine_content_ruins");
  const Output kit = run({"ruins-kit", tmp.file("kit")});
  REQUIRE_MESSAGE(kit.exit_code == 0, kit.text);
  JsonValue kit_line;
  REQUIRE(parse_json(kit.text, kit_line).ok);
  CHECK(number(kit_line, "members") > 10);
  const std::string kit_path = tmp.file("kit") + "/kit.json";

  const std::string fragment = tmp.file("out/ruin.json");
  const Output made =
      run({"ruins", kit_path, "7", "-3,2", "--out", fragment, "--wind", "90", "--ground", "1.5"});
  REQUIRE_MESSAGE(made.exit_code == 0, made.text);
  JsonValue summary;
  REQUIRE(parse_json(made.text, summary).ok);
  CHECK(number(summary, "buildings") == 1);
  const u64 instances = number(summary, "instances");
  CHECK(instances > 8);
  const JsonValue* kinds = summary.find("kinds");
  REQUIRE(kinds != nullptr);
  CHECK(number(*kinds, "corner") >= 4);
  REQUIRE(summary.find("hash") != nullptr);
  CHECK(summary.find("hash")->as_string().size() == 16);

  // The fragment is a scene: the kit's meshes by relative path, every piece with its tag.
  std::ifstream in(fragment, std::ios::binary);
  REQUIRE(in.is_open());
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  JsonValue scene;
  REQUIRE(parse_json(text, scene).ok);
  CHECK(scene.find("format")->as_string() == "engine.scene.v1");
  const JsonValue* meshes = scene.find("meshes");
  REQUIRE(meshes != nullptr);
  CHECK(meshes->size() == number(kit_line, "meshes"));
  const std::string first = std::string((*meshes)[0].find("path")->as_string());
  CHECK(first.rfind("../kit/", 0) == 0);
  const JsonValue* placed = scene.find("instances");
  REQUIRE(placed != nullptr);
  REQUIRE(placed->size() == instances);
  for (usize i = 0; i < placed->size(); ++i) {
    const JsonValue* tag = (*placed)[i].find("ruin");
    REQUIRE(tag != nullptr);
    CHECK(tag->is_object());
    // Every piece is inside tile (-3, 2) of 32 m.
    const JsonValue* t = (*placed)[i].find("translation");
    REQUIRE(t != nullptr);
    f64 x = 0, z = 0;
    REQUIRE((*t)[0].get_f64(x));
    REQUIRE((*t)[2].get_f64(z));
    CHECK(x >= -96.0 - 2.0);  // the translation carries the member's offset, zero for boxes
    CHECK(x <= -64.0 + 2.0);
    CHECK(z >= 64.0 - 2.0);
    CHECK(z <= 96.0 + 2.0);
  }
  REQUIRE(scene.find("ruin_sites") != nullptr);
  CHECK(scene.find("ruin_sites")->size() == 1);
  REQUIRE(scene.find("sand_drifts") != nullptr);
  CHECK(scene.find("sand_drifts")->size() == number(summary, "drifts"));

  // The far tier: the same walls, no debris.
  const Output far = run({"ruins", kit_path, "7", "-3,2", "--no-write", "--wind", "90", "--walls"});
  REQUIRE_MESSAGE(far.exit_code == 0, far.text);
  JsonValue far_summary;
  REQUIRE(parse_json(far.text, far_summary).ok);
  CHECK(number(*far_summary.find("kinds"), "debris") == 0);
  CHECK(number(far_summary, "instances") == instances - number(*kinds, "debris"));
}

TEST_CASE("engine-content ruins: refusals") {
  const test::TempDir tmp("engine_content_ruins_refuse");
  CHECK(run({"ruins"}).exit_code == 2);
  CHECK(run({"ruins", "kit.json", "7", "1;2", "--no-write"}).exit_code == 2);
  CHECK(run({"ruins", "kit.json", "7", "0,0"}).exit_code == 2);  // no --out
  CHECK(run({"ruins", "kit.json", "7", "0,0", "--no-write", "--count", "3"}).exit_code == 2);
  CHECK(run({"ruins", "kit.json", "x", "0,0", "--no-write"}).exit_code == 2);
  CHECK(run({"ruins-kit"}).exit_code == 2);
  // A kit that is not there is a file error, not a usage error.
  const Output missing = run({"ruins", tmp.file("none.json"), "7", "0,0", "--no-write"});
  CHECK(missing.exit_code == 1);
  CHECK(missing.text.empty());  // stdout only: the reason went to stderr
}

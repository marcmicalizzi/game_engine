// engine-content terrain end to end (docs/subsystems/terrain.md, apps.md): the committed
// desert-dunes scene's tile as one JSON line — its fields, the same bits on a second run, the dunes
// moving with --time and the floor not, crest lines with --crests, the scene's own time by default
// — and the refusals. The golden hashes of the field are domain/terrain's.
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

std::string text(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  std::string_view out;
  if (value != nullptr) (void)value->get_string(out);
  return std::string(out);
}

f64 number(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  f64 v = -1e300;
  if (value != nullptr) (void)value->get_f64(v);
  return v;
}

}  // namespace

TEST_CASE("engine-content terrain: a tile of a scene's dune field as one JSON line") {
  const std::string scene =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/desert-dunes/scene.json",
                      "content/test-scenes/desert-dunes/scene.json");
  if (!test::path_exists(scene)) {
    MESSAGE("skipped: " << scene << " is not here");
    return;
  }
  const Output a = run({"terrain", scene, "--tile", "3,-2", "--cells", "64", "--crests"});
  REQUIRE_MESSAGE(a.exit_code == 0, a.text);
  JsonValue line;
  REQUIRE(parse_json(a.text, line).ok);
  CHECK(text(line, "format") == "engine.terrain.tile");
  CHECK(number(line, "tile_x") == 3.0);
  CHECK(number(line, "tile_z") == -2.0);
  CHECK(number(line, "time_s") == 94608000.0);  // the scene's own time
  CHECK(text(line, "detail") == "dunes");
  CHECK(text(line, "tile_hash").size() == 16);
  CHECK(number(line, "max_m") >= number(line, "min_m"));
  CHECK(number(line, "flux_m2_per_day") >= 0.0);
  const JsonValue* displacement = line.find("displacement_m");
  REQUIRE(displacement != nullptr);
  CHECK(displacement->size() == 3);
  const JsonValue* crests = line.find("crests");
  REQUIRE(crests != nullptr);
  MESSAGE("tile (3, -2) three years in: " << number(line, "min_m") << " .. "
                                          << number(line, "max_m") << " m, " << crests->size()
                                          << " crest lines, " << number(line, "eval_ms") << " ms");

  // The same bits on a second run: the field is a function of the scene and the time.
  const Output again = run({"terrain", scene, "--tile", "3,-2", "--cells", "64", "--crests"});
  JsonValue second;
  REQUIRE(parse_json(again.text, second).ok);
  CHECK(text(second, "tile_hash") == text(line, "tile_hash"));
  CHECK(text(second, "field_hash") == text(line, "field_hash"));

  // A year later the dunes have moved; the floor has not.
  const Output later =
      run({"terrain", scene, "--tile", "3,-2", "--cells", "64", "--time", "126144000"});
  JsonValue moved;
  REQUIRE(parse_json(later.text, moved).ok);
  CHECK(text(moved, "tile_hash") != text(line, "tile_hash"));
  const Output floor_now = run(
      {"terrain", scene, "--tile", "3,-2", "--cells", "16", "--detail", "floor", "--time", "0"});
  const Output floor_later = run({"terrain", scene, "--tile", "3,-2", "--cells", "16", "--detail",
                                  "floor", "--time", "126144000"});
  JsonValue f0, f1;
  REQUIRE(parse_json(floor_now.text, f0).ok);
  REQUIRE(parse_json(floor_later.text, f1).ok);
  CHECK(number(f0, "min_m") == number(f1, "min_m"));
  CHECK(number(f0, "max_m") == number(f1, "max_m"));
  CHECK(text(f0, "tile_hash") != text(f1, "tile_hash"));  // the hash names the time it was asked at
}

TEST_CASE("engine-content terrain: refusals") {
  const test::TempDir tmp("engine_content_terrain");
  CHECK(run({"terrain"}).exit_code == 2);
  const std::string no_terrain = tmp.file("empty.json");
  {
    std::ofstream f(no_terrain);
    f << R"({"format":"engine.scene.v1","meshes":[{"path":"a.glb"}]})";
  }
  CHECK(run({"terrain", no_terrain, "--tile", "0,0"}).exit_code == 1);
  CHECK(run({"terrain", no_terrain}).exit_code == 2);                    // no --tile
  CHECK(run({"terrain", no_terrain, "--tile", "zero"}).exit_code == 2);  // not x,z
  CHECK(run({"terrain", no_terrain, "--tile", "0,0", "--time", "-1"}).exit_code == 2);
  CHECK(run({"terrain", no_terrain, "--tile", "0,0", "--detail", "blurry"}).exit_code == 2);
  CHECK(run({"terrain", tmp.file("missing.json"), "--tile", "0,0"}).exit_code == 1);
  const std::string odd = tmp.file("odd.json");
  {
    std::ofstream f(odd);
    f << R"({"terrain":{"size":3,"extent":10}})";
  }
  CHECK(run({"terrain", odd, "--tile", "0,0", "--cells", "7"}).exit_code == 1);  // 32 m in 7 cells
  CHECK(run({"terrain", odd, "--tile", "0,0", "--cells", "8"}).exit_code == 0);
}

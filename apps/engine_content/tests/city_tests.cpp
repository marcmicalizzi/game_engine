// engine-content city end to end (docs/subsystems/city.md, apps.md): the default parameters written
// and read, a plan made into a directory and into the derived-data cache (and found there the
// second time), one lot's building and its description file, a tile's and a district's proxy
// fragments read back as scenes with their twelve meshes, E18's yield, and the refusals. The golden
// hashes are in determinism_tests.cpp, with the content build's and the ruins'.
#include <core/json/json.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <filesystem>
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

JsonValue line(const Output& o) {
  JsonValue v;
  const bool ok = parse_json(o.text, v).ok;
  REQUIRE_MESSAGE(ok, o.text);
  return v;
}

u64 number(const JsonValue& object, const char* key) {
  u64 v = 0;
  const JsonValue* value = object.find(key);
  return value != nullptr && value->get_u64(v) ? v : ~u64{0};
}

std::string text_of(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  return value != nullptr && value->is_string() ? std::string(value->as_string()) : std::string();
}

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_text(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
}

}  // namespace

TEST_CASE("engine-content city: parameters, a plan, a building, fragments and the yield") {
  const test::TempDir tmp("engine_content_city");
  // The defaults, spelled out, read back to the same key a sparse file gives.
  const Output params = run({"city", "params", tmp.file("params.json")});
  REQUIRE_MESSAGE(params.exit_code == 0, params.text);
  write_text(tmp.file("small.json"), "{\"seed\": 2026, \"radius\": 900}\n");

  // A plan into a directory.
  const Output made = run({"city", "plan", tmp.file("small.json"), "--out", tmp.file("plan")});
  REQUIRE_MESSAGE(made.exit_code == 0, made.text);
  const JsonValue plan = line(made);
  CHECK(number(plan, "lots") > 100);
  CHECK(number(plan, "blocks") > 10);
  CHECK(std::filesystem::exists(tmp.file("plan") + "/plan.json"));
  const JsonValue* validators = plan.find("validators");
  REQUIRE(validators != nullptr);
  CHECK(validators->size() == 0);  // the plan's own rules all pass

  // Into the cache, twice: made the first time, found the second, the same plan.
  const Output first =
      run({"city", "plan", tmp.file("small.json"), "--cache", "--ddc", tmp.file("ddc")});
  REQUIRE_MESSAGE(first.exit_code == 0, first.text);
  const Output second =
      run({"city", "plan", tmp.file("small.json"), "--cache", "--ddc", tmp.file("ddc")});
  REQUIRE_MESSAGE(second.exit_code == 0, second.text);
  const JsonValue a = line(first);
  const JsonValue b = line(second);
  CHECK(a.find("cached")->as_bool() == false);
  CHECK(b.find("cached")->as_bool() == true);
  CHECK(text_of(a, "hash") == text_of(b, "hash"));
  CHECK(text_of(a, "hash") == text_of(plan, "hash"));
  CHECK(text_of(a, "output").find("/city/" + text_of(a, "key") + "/plan.json") !=
        std::string::npos);

  // One lot's building: the first lot of the plan file that is not a park.
  JsonValue file;
  REQUIRE(parse_json(read_text(tmp.file("plan") + "/plan.json"), file).ok);
  const JsonValue& lots = *file.find("lots");
  u64 lot = ~u64{0};
  for (usize i = 0; i < lots.size() && lot == ~u64{0}; ++i) {
    if (text_of(lots[i], "use") == "Building") lot = number(lots[i], "id");
  }
  REQUIRE(lot != ~u64{0});
  const Output building = run({"city", "building", "--plan", tmp.file("plan"), "--lot",
                               std::to_string(lot), "--out", tmp.file("building.json")});
  REQUIRE_MESSAGE(building.exit_code == 0, building.text);
  const JsonValue bl = line(building);
  CHECK(bl.find("passed")->as_bool());
  CHECK(number(bl, "floors") >= 1);
  CHECK(number(bl, "spaces") > 0);
  JsonValue desc;
  REQUIRE(parse_json(read_text(tmp.file("building.json")), desc).ok);
  CHECK(text_of(desc, "format") == "engine.city-building.v1");
  CHECK(desc.find("walls")->size() == number(bl, "walls"));

  // A district's fragment: the twelve proxy meshes beside it, one instance per proxy.
  const Output district = run({"city", "fragment", "--plan", tmp.file("plan"), "--district", "0",
                               "--out", tmp.file("frag/district.json")});
  REQUIRE_MESSAGE(district.exit_code == 0, district.text);
  const JsonValue dl = line(district);
  JsonValue scene;
  REQUIRE(parse_json(read_text(tmp.file("frag/district.json")), scene).ok);
  CHECK(text_of(scene, "format") == "engine.scene.v1");
  CHECK(scene.find("meshes")->size() == 12);
  CHECK(scene.find("instances")->size() == number(dl, "instances"));
  for (const char* mesh : {"massing", "slab", "exterior_wall", "window", "road", "tree"})
    CHECK(std::filesystem::exists(tmp.file("frag/proxy/") + mesh + ".glb"));
  // A tile's fragment: the tile holding the lot's building.
  const JsonValue& fp = *bl.find("footprint_cm");
  i64 x = 0, z = 0;
  REQUIRE(fp[0].get_i64(x));
  REQUIRE(fp[1].get_i64(z));
  const i64 tile_cm = 6400;
  const std::string tile = std::to_string(x >= 0 ? x / tile_cm : -((-x + tile_cm - 1) / tile_cm)) +
                           "," +
                           std::to_string(z >= 0 ? z / tile_cm : -((-z + tile_cm - 1) / tile_cm));
  const Output t = run({"city", "fragment", "--plan", tmp.file("plan"), "--tile", tile, "--out",
                        tmp.file("frag/tile.json")});
  REQUIRE_MESSAGE(t.exit_code == 0, t.text);
  const JsonValue tl = line(t);
  CHECK(number(tl, "buildings") >= 1);
  CHECK(text_of(tl, "detail") == "rooms");
  CHECK(number(*tl.find("meshes"), "interior_wall") > 0);

  // E18 on the small island.
  const Output y =
      run({"city", "yield", "--plan", tmp.file("plan"), "--buildings", "40", "--jobs", "2"});
  REQUIRE_MESSAGE(y.exit_code == 0, y.text);
  const JsonValue yl = line(y);
  const JsonValue& all = *yl.find("all");
  CHECK(number(all, "buildings") == 40);
  CHECK(number(all, "passed") == 40);
}

TEST_CASE("engine-content city: refusals") {
  const test::TempDir tmp("engine_content_city_refusals");
  write_text(tmp.file("small.json"), "{\"seed\": 2026, \"radius\": 900}\n");
  CHECK(run({"city"}).exit_code == 2);
  CHECK(run({"city", "survey"}).exit_code == 2);
  CHECK(run({"city", "plan", tmp.file("small.json")}).exit_code == 2);  // no --out
  write_text(tmp.file("bad.json"), "{\"radius\": 5}\n");
  const Output bad = run({"city", "plan", tmp.file("bad.json"), "--no-write"});
  CHECK(bad.exit_code == 1);
  REQUIRE(run({"city", "plan", tmp.file("small.json"), "--out", tmp.file("plan")}).exit_code == 0);
  CHECK(run({"city", "building", "--plan", tmp.file("plan"), "--lot", "7"}).exit_code == 1);
  CHECK(run({"city", "fragment", "--plan", tmp.file("plan"), "--tile", "0,0", "--district", "0",
             "--out", tmp.file("f.json")})
            .exit_code == 2);
  CHECK(run({"city", "fragment", "--plan", tmp.file("plan"), "--district", "9999", "--out",
             tmp.file("f.json")})
            .exit_code == 1);
}

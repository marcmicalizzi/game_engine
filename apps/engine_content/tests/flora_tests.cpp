// engine-content flora end to end (docs/subsystems/flora.md, "The build"; apps.md): a flora file
// written into the test's scratch directory, built into a named container and into a derived-data
// root, built again as a cache hit, read back by `info` with what generated it; the key's
// canonicalization; and, without the capability, the refusal.
#include <core/json/json.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <filesystem>
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

#if ENGINE_CONTENT_FLORA

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

// A small palm, so a debug build makes it in a moment; the build is the same code at any size.
constexpr const char* k_palm =
    "{\"species\":\"date_palm\",\"palm\":{\"seed\":7,\"height_m\":6,\"frond_count\":8,"
    "\"leaflet_count\":16,\"dead_skirt_fronds\":3,\"triangle_budget\":12000}}";

}  // namespace

TEST_CASE(
    "engine-content flora: built once per parameter set, through a named output and the cache") {
  const test::TempDir tmp("engine_content_flora");
  REQUIRE(tmp.ok());
  const std::string params = tmp.file("small.flora.json");
  write_text(params, k_palm);

  // A named output, then the same again: the second finds the identity it would write.
  const std::string out = tmp.file("small.clusters");
  const Output first = run({"flora", params, out});
  REQUIRE_MESSAGE(first.exit_code == 0, first.text);
  const JsonValue a = line_of(first);
  CHECK_FALSE(flag(a, "cached"));
  CHECK(number(a, "triangles") > 1000);
  CHECK(number(a, "triangles") <= 12000);
  CHECK(number(a, "materials") == 3);
  REQUIRE(a.find("budget") != nullptr);
  const Output second = run({"flora", params, out});
  REQUIRE_MESSAGE(second.exit_code == 0, second.text);
  const JsonValue b = line_of(second);
  CHECK(flag(b, "cached"));
  CHECK(number(b, "build_key") == number(a, "build_key"));
  CHECK(number(b, "clusters") == number(a, "clusters"));

  // Into a derived-data root, at the key's path, and a hit the second time.
  const std::string ddc = tmp.file("ddc");
  const Output cache_first = run({"flora", params, "--ddc", ddc});
  REQUIRE_MESSAGE(cache_first.exit_code == 0, cache_first.text);
  const JsonValue c = line_of(cache_first);
  CHECK_FALSE(flag(c, "cached"));
  CHECK(number(c, "build_key") == number(a, "build_key"));
  const JsonValue* path = c.find("path");
  REQUIRE(path != nullptr);
  CHECK(std::string(path->as_string()).find("clusters") != std::string::npos);
  const Output cache_second = run({"flora", params, "--ddc", ddc});
  REQUIRE_MESSAGE(cache_second.exit_code == 0, cache_second.text);
  CHECK(flag(line_of(cache_second), "cached"));

  // `info` says what made it: the generator, the species and the canonical parameters.
  const Output info = run({"info", out});
  REQUIRE_MESSAGE(info.exit_code == 0, info.text);
  const JsonValue i = line_of(info);
  const JsonValue* generator = i.find("generator");
  REQUIRE_MESSAGE(generator != nullptr, info.text);
  CHECK(std::string(generator->find("name")->as_string()) == "flora");
  CHECK(std::string(generator->find("kind")->as_string()) == "date_palm");
  const JsonValue* palm = generator->find("params")->find("palm");
  REQUIRE(palm != nullptr);
  CHECK(number(*palm, "seed") == 7);
  CHECK(number(*palm, "frond_count") == 8);
  // Every parameter is there, the defaults the file left out included.
  CHECK(palm->find("frond_droop") != nullptr);
}

TEST_CASE("engine-content flora: the key is the parameters, not the file's spelling of them") {
  const test::TempDir tmp("engine_content_flora_key");
  REQUIRE(tmp.ok());
  const std::string a = tmp.file("a.flora.json");
  const std::string b = tmp.file("b.flora.json");
  const std::string c = tmp.file("c.flora.json");
  write_text(a, k_palm);
  // The same parameters, spaced and ordered differently and with a default spelled out.
  write_text(b,
             "{\n  \"palm\": {\"triangle_budget\": 12000, \"dead_skirt_fronds\": 3,\n"
             "    \"leaflet_count\": 16, \"frond_count\": 8, \"height_m\": 6.0, \"seed\": 7,\n"
             "    \"curve\": 0.5},\n  \"species\": \"date_palm\"\n}\n");
  // Another seed: another palm.
  write_text(c,
             "{\"species\":\"date_palm\",\"palm\":{\"seed\":8,\"height_m\":6,\"frond_count\":8,"
             "\"leaflet_count\":16,\"dead_skirt_fronds\":3,\"triangle_budget\":12000}}");
  const Output ra = run({"flora", a, tmp.file("a.clusters")});
  const Output rb = run({"flora", b, tmp.file("b.clusters")});
  const Output rc = run({"flora", c, tmp.file("c.clusters")});
  REQUIRE_MESSAGE(ra.exit_code == 0, ra.text);
  REQUIRE_MESSAGE(rb.exit_code == 0, rb.text);
  REQUIRE_MESSAGE(rc.exit_code == 0, rc.text);
  CHECK(number(line_of(ra), "build_key") == number(line_of(rb), "build_key"));
  CHECK(number(line_of(ra), "build_key") != number(line_of(rc), "build_key"));

  // A parameter out of range, an unknown field and an unknown species are refused with a sentence.
  write_text(c, "{\"species\":\"date_palm\",\"palm\":{\"height_m\":90}}");
  const Output range = run({"flora", c, tmp.file("x.clusters")});
  CHECK(range.exit_code == 1);
  write_text(c, "{\"species\":\"date_palm\",\"palm\":{\"heigth_m\":9}}");
  CHECK(run({"flora", c, tmp.file("x.clusters")}).exit_code == 1);
  write_text(c, "{\"species\":\"baobab\"}");
  CHECK(run({"flora", c, tmp.file("x.clusters")}).exit_code == 1);
  CHECK(run({"flora", c}).exit_code == 2);
}

// docs/subsystems/materials.md: `--materials` names the pack a part with no set falls back to,
// which moves the key and names the pack's images; `--no-materials` is the flat palm. The pack's
// images are not there, so the texture step finds them missing — a warning, as a glTF's missing
// image is, and the build still succeeds.
TEST_CASE("engine-content flora: a material pack's roles fill the parts and move the key") {
  const test::TempDir tmp("engine_content_flora_pack");
  REQUIRE(tmp.ok());
  const std::string params = tmp.file("palm.flora.json");
  write_text(params, k_palm);
  const std::string pack_dir = tmp.file("pack");
  REQUIRE(std::filesystem::create_directories(tmp.native_file("pack")));
  write_text(pack_dir + "/pack.json",
             R"({"format":"engine.materials.pack/1","name":"stand-in","version":"7","roles":{)"
             R"("palm/bark":{"tile_m":0.35,"normal_convention":"opengl","maps":{)"
             R"("base_color":{"file":"b/c.png"},"normal":{"file":"b/n.png"},)"
             R"("roughness":{"file":"b/r.png"},"metallic":{"file":"b/m.png"},)"
             R"("height":{"file":"b/h.png"}}}}})");
  const Output plain = run({"flora", params, "--ddc", tmp.file("ddc"), "--no-materials"});
  REQUIRE_MESSAGE(plain.exit_code == 0, plain.text);
  const Output packed = run({"flora", params, "--ddc", tmp.file("ddc"), "--materials", pack_dir});
  REQUIRE_MESSAGE(packed.exit_code == 0, packed.text);
  const JsonValue a = line_of(plain);
  const JsonValue b = line_of(packed);
  CHECK(number(a, "images") == 0);
  CHECK(number(b, "images") == 3);
  CHECK(number(a, "source_hash") == number(b, "source_hash"));
  CHECK(number(a, "build_key") != number(b, "build_key"));
  const JsonValue* pack = b.find("pack");
  REQUIRE(pack != nullptr);
  REQUIRE(pack->is_string());
  CHECK(pack->as_string() == "stand-in 7");
  const JsonValue* textures = b.find("textures");
  REQUIRE(textures != nullptr);
  CHECK(flag(*textures, "enabled"));
}

#else

TEST_CASE("engine-content flora: refused with a sentence where the capability is off") {
  const test::TempDir tmp("engine_content_flora_off");
  REQUIRE(tmp.ok());
  const std::string params = tmp.file("p.flora.json");
  write_text(params, "{\"species\":\"date_palm\"}");
  const Output o = run({"flora", params, tmp.file("p.clusters")});
  CHECK(o.exit_code == 1);
}

#endif

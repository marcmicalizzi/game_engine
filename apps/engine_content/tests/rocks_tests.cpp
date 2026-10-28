// engine-content rocks end to end (docs/subsystems/rocks.md, "The build"; apps.md): a rock file
// written into the test's scratch directory, built into a named container and into a derived-data
// root, built again as a cache hit, read back by `info` with what generated it; the same file as a
// GLB that `build` imports; `build` and `build-all` taking the `.rock.json` itself under the key
// the command and the renderer use; and, without the capability, the refusal.
#include <core/json/json.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <fstream>
#include <sstream>
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

#if ENGINE_CONTENT_ROCKS

namespace {

JsonValue line_of(const std::string& text) {
  JsonValue v;
  REQUIRE_MESSAGE(parse_json(text, v).ok, text);
  return v;
}
JsonValue line_of(const Output& o) { return line_of(o.text); }

u64 number(const JsonValue& object, const char* key) {
  u64 v = 0;
  const JsonValue* value = object.find(key);
  return value != nullptr && value->get_u64(v) ? v : ~u64{0};
}

bool flag(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  return value != nullptr && value->is_bool() && value->as_bool();
}

std::string text_of(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  return value != nullptr && value->is_string() ? std::string(value->as_string()) : std::string();
}

// A small boulder, so a debug build makes it in a moment.
constexpr const char* k_rock =
    "{\"shape\":\"Boulder\",\"boulder\":{\"seed\":5,\"type\":\"Sandstone\",\"triangle_budget\":"
    "3000}}";

}  // namespace

TEST_CASE(
    "engine-content rocks: built once per parameter set, through a named output and the cache") {
  const test::TempDir tmp("engine_content_rocks");
  REQUIRE(tmp.ok());
  const std::string params = tmp.file("small.rock.json");
  write_text(params, k_rock);

  const std::string out = tmp.file("small.clusters");
  const Output first = run({"rocks", params, out});
  REQUIRE_MESSAGE(first.exit_code == 0, first.text);
  const JsonValue a = line_of(first);
  CHECK_FALSE(flag(a, "cached"));
  CHECK(number(a, "triangles") > 1000);
  CHECK(number(a, "triangles") <= 3000);
  CHECK(number(a, "materials") == 1);
  CHECK(text_of(a, "shape") == "Boulder");
  REQUIRE(a.find("budget") != nullptr);
  const Output second = run({"rocks", params, out});
  REQUIRE_MESSAGE(second.exit_code == 0, second.text);
  const JsonValue b = line_of(second);
  CHECK(flag(b, "cached"));
  CHECK(number(b, "build_key") == number(a, "build_key"));

  const std::string ddc = tmp.file("ddc");
  const Output cache_first = run({"rocks", params, "--ddc", ddc});
  REQUIRE_MESSAGE(cache_first.exit_code == 0, cache_first.text);
  const JsonValue c = line_of(cache_first);
  CHECK_FALSE(flag(c, "cached"));
  CHECK(number(c, "build_key") == number(a, "build_key"));
  CHECK(flag(line_of(run({"rocks", params, "--ddc", ddc})), "cached"));

  // `info` says what made it: the generator, the shape and the canonical parameters.
  const Output info = run({"info", out});
  REQUIRE_MESSAGE(info.exit_code == 0, info.text);
  const JsonValue info_line = line_of(info);
  const JsonValue* generator = info_line.find("generator");
  REQUIRE_MESSAGE(generator != nullptr, info.text);
  CHECK(text_of(*generator, "name") == "rock");
  CHECK(text_of(*generator, "kind") == "Boulder");
  const JsonValue* boulder = generator->find("params")->find("boulder");
  REQUIRE(boulder != nullptr);
  CHECK(number(*boulder, "seed") == 5);
  CHECK(boulder->find("buried") != nullptr);  // the defaults the file left out, spelled out

  // `build` takes the rock file itself, through the mesh-generator registry, under the same key.
  const Output built = run({"build", params, tmp.file("built.clusters")});
  REQUIRE_MESSAGE(built.exit_code == 0, built.text);
  const JsonValue d = line_of(built);
  CHECK(number(d, "build_key") == number(a, "build_key"));
  CHECK(number(d, "source_hash") == number(a, "source_hash"));
  CHECK(number(d, "triangles") == number(a, "triangles"));

  // And the GLB the harness judges: the same triangles, which `build` imports as a glTF.
  const std::string glb = tmp.file("small.glb");
  const Output written = run({"rocks", params, "--glb", glb});
  REQUIRE_MESSAGE(written.exit_code == 0, written.text);
  CHECK(number(line_of(written), "triangles") == number(a, "triangles"));
  const Output imported = run({"build", glb, tmp.file("glb.clusters")});
  REQUIRE_MESSAGE(imported.exit_code == 0, imported.text);
  CHECK(number(line_of(imported), "triangles") == number(a, "triangles"));
  CHECK(number(line_of(imported), "warnings") == 0);
}

TEST_CASE("engine-content rocks: procedural PBR maps survive GLB export and reimport") {
  const test::TempDir tmp("engine_content_rock_maps");
  REQUIRE(tmp.ok());
  const std::string params = tmp.file("surface.rock.json");
  write_text(
      params,
      R"({"boulder":{"triangle_budget":2000,"material":{"surface_maps":{"resolution":256,"veins":0.8}}}})");
  const std::string glb = tmp.file("surface.glb");
  const Output exported = run({"rocks", params, "--glb", glb});
  REQUIRE_MESSAGE(exported.exit_code == 0, exported.text);
  const std::string cluster = tmp.file("surface.clusters");
  const Output imported = run({"build", glb, cluster});
  REQUIRE_MESSAGE(imported.exit_code == 0, imported.text);
  CHECK(number(line_of(imported), "warnings") == 0);
  const Output info = run({"info", cluster});
  REQUIRE_MESSAGE(info.exit_code == 0, info.text);
  // Inspect the standalone GLB itself: three PNG buffer views and three bound texture slots.
  std::ifstream stream(glb, std::ios::binary);
  const std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  REQUIRE(bytes.size() > 20);
  const auto byte = [&](usize i) { return static_cast<u32>(static_cast<u8>(bytes[i])); };
  const u32 json_size = byte(12) | (byte(13) << 8) | (byte(14) << 16) | (byte(15) << 24);
  REQUIRE(20u + json_size <= bytes.size());
  JsonValue json;
  REQUIRE(parse_json(std::string_view(bytes).substr(20, json_size), json).ok);
  REQUIRE(json.find("images") != nullptr);
  CHECK(json.find("images")->as_array().size() == 3);
  const auto& m = json.find("materials")->as_array()[0];
  CHECK(m.find("normalTexture") != nullptr);
  CHECK(m.find("pbrMetallicRoughness")->find("baseColorTexture") != nullptr);
  CHECK(m.find("pbrMetallicRoughness")->find("metallicRoughnessTexture") != nullptr);
}

TEST_CASE("engine-content rocks: build-all builds a manifest's rock files and then skips them") {
  const test::TempDir tmp("engine_content_rocks_all");
  REQUIRE(tmp.ok());
  write_text(tmp.file("a.rock.json"), k_rock);
  write_text(tmp.file("b.rock.json"),
             "{\"shape\":\"Scree\",\"scree\":{\"seed\":2,\"count\":5,\"triangle_budget\":2000}}");
  const std::string manifest = tmp.file("manifest.json");
  write_text(manifest,
             "{\"meshes\":[{\"source\":\"a.rock.json\"},{\"source\":\"b.rock.json\","
             "\"output\":\"b.clusters\"}]}");
  const std::string ddc = tmp.file("ddc");
  const auto lines = [](const Output& o) {
    std::vector<JsonValue> out;
    std::istringstream in(o.text);
    std::string line;
    while (std::getline(in, line)) {
      JsonValue v;
      if (!line.empty() && parse_json(line, v).ok && v.find("source") != nullptr)
        out.push_back(std::move(v));
    }
    return out;
  };
  const Output first = run({"build-all", manifest, "--cache", "--ddc", ddc});
  REQUIRE_MESSAGE(first.exit_code == 0, first.text);
  const std::vector<JsonValue> built = lines(first);
  REQUIRE_MESSAGE(built.size() == 2, first.text);
  for (const JsonValue& line : built)
    CHECK_MESSAGE(text_of(line, "status") == "built", first.text);
  // The cache entry is the one `rocks --ddc` addresses.
  const Output direct = run({"rocks", tmp.file("a.rock.json"), "--ddc", ddc});
  REQUIRE_MESSAGE(direct.exit_code == 0, direct.text);
  CHECK(flag(line_of(direct), "cached"));
  const Output second = run({"build-all", manifest, "--cache", "--ddc", ddc});
  REQUIRE_MESSAGE(second.exit_code == 0, second.text);
  for (const JsonValue& line : lines(second))
    CHECK_MESSAGE(text_of(line, "status") == "skipped", second.text);
}

TEST_CASE("engine-content rocks: a file the solver refuses is refused with a sentence") {
  const test::TempDir tmp("engine_content_rocks_bad");
  REQUIRE(tmp.ok());
  const std::string c = tmp.file("c.rock.json");
  write_text(c, "{\"boulder\":{\"size_x\":90}}");
  const Output range = run({"rocks", c, tmp.file("x.clusters")});
  CHECK(range.exit_code == 1);
  write_text(c, "{\"boulder\":{\"sise_x\":1}}");
  CHECK(run({"rocks", c, tmp.file("x.clusters")}).exit_code == 1);
  CHECK(run({"build", c, tmp.file("x.clusters")}).exit_code == 1);
  CHECK(run({"rocks", c}).exit_code == 2);
}

#else

TEST_CASE("engine-content rocks: refused with a sentence where the capability is off") {
  const test::TempDir tmp("engine_content_rocks_off");
  REQUIRE(tmp.ok());
  const std::string params = tmp.file("p.rock.json");
  write_text(params, "{\"shape\":\"Boulder\"}");
  CHECK(run({"rocks", params, tmp.file("p.clusters")}).exit_code == 1);
  // `build` refuses it too, by the registry's sentence, never by reading it as a glTF.
  const Output o = run({"build", params, tmp.file("p.clusters")});
  CHECK(o.exit_code == 1);
}

#endif

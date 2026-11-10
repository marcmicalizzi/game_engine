// End to end (T78; docs/subsystems/apps.md, "engine-content" and "content.build"): **one manifest
// built by `engine-content build-all` and by engine-host's `content.build` gives the same bytes.**
// The two hosts run one build (domain/content_build), but each hands it what a host hands it — and
// until T78 engine-host handed a generated mesh no material pack, so a palm a manifest names was
// built with flat colours and another key by the protocol, and with the repository's pack by the
// command line.
//
// The repository's pack is `<repo>/content/materials`, found by walking up from the executable to
// the directory holding AGENTS.md (`materials::default_pack_dir`), and a test may not write into
// the repository. So the test makes a repository of its own in its scratch directory — an
// AGENTS.md, a pack of three roles with a striped map each, and copies of the two executables
// under `bin/` — and the copies find that pack as the installed ones find the real one. The palm
// is the cooked-load test's (systems/renderer/tests/cooked_load_tests.cpp) at the palm's smallest
// budget, 2,000 triangles. No GPU.
#include <core/json/json.h>
#include <core/platform/process.h>
#include <foundation/image/png.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace engine;

#if defined(ENGINE_CLI_TESTS_FLORA)

namespace {

struct Run {
  i32 exit_code = -1;
  std::string out;
};

Run run(const std::vector<std::string>& args) {
  std::vector<std::string_view> argv(args.begin(), args.end());
  platform::Process p;
  std::string error;
  Run r;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error)) {
    FAIL("cannot spawn " << args[0] << ": " << error);
    return r;
  }
  p.close_stdin();
  p.read_all(r.out);
  r.exit_code = p.wait();
  return r;
}

bool write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

std::string read_bytes(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string json_string(const std::string& s) { return write_json(JsonValue(s)); }

// A 32 x 32 map of stripes of the test's own, so every image has bytes of its own.
bool write_stripes(const std::filesystem::path& path, u8 a, u8 b) {
  std::vector<u8> pixels(32u * 32u * 4u);
  for (u32 y = 0; y < 32; ++y) {
    for (u32 x = 0; x < 32; ++x) {
      const u8 v = (x / 4u) % 2u == 0 ? a : b;
      u8* p = pixels.data() + (y * 32u + x) * 4u;
      p[0] = v;
      p[1] = static_cast<u8>(v / 2u + 64u);
      p[2] = 128u;
      p[3] = 255u;
    }
  }
  Vector<u8> png;
  if (!image::encode_png(32, 32, 4, std::span<const u8>(pixels.data(), pixels.size()), png))
    return false;
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  return f.good();
}

// The palm's three roles, every map a file of its own (the cooked-load test's pack).
void write_pack(const std::filesystem::path& dir) {
  const char* roles[] = {"palm/bark", "palm/frond", "palm/skirt"};
  const char* maps[] = {"base_color", "normal", "roughness", "metallic", "height"};
  std::string pack = R"({"format":"engine.materials.pack/1","name":"t","version":"1","roles":{)";
  for (u32 r = 0; r < 3; ++r) {
    std::filesystem::create_directories(dir / roles[r]);
    pack += std::string(r > 0 ? "," : "") + "\"" + roles[r] +
            R"(":{"tile_m":0.25,"normal_convention":"opengl","maps":{)";
    for (u32 i = 0; i < 5; ++i) {
      REQUIRE(write_stripes(dir / roles[r] / (std::string(maps[i]) + ".png"),
                            static_cast<u8>(10 + 30 * i + r), static_cast<u8>(20 + 30 * i)));
      pack += std::string(i > 0 ? "," : "") + "\"" + maps[i] + "\":{\"file\":\"" +
              std::string(roles[r]) + "/" + maps[i] + ".png\"}";
    }
    pack += "}}";
  }
  pack += "}}";
  REQUIRE(write_text(dir / "pack.json", pack));
}

std::string exe_name(const char* name) {
#if defined(_WIN32)
  return std::string(name) + ".exe";
#else
  return name;
#endif
}

}  // namespace

TEST_CASE("content build: build-all and content.build give one manifest's palm the same bytes") {
  const test::TempDir tmp("cli_content_parity");
  REQUIRE(tmp.ok());
  const std::filesystem::path repo = tmp.native() / "repo";
  std::filesystem::create_directories(repo / "bin");
  REQUIRE(write_text(repo / "AGENTS.md", "a repository of the test's own\n"));
  write_pack(repo / "content" / "materials");
  // The two hosts, copied where they find that pack.
  const std::filesystem::path content = repo / "bin" / exe_name("engine-content");
  const std::filesystem::path host = repo / "bin" / exe_name("engine-host");
  std::filesystem::copy_file(test::app_path(ENGINE_CONTENT_PATH), content);
  std::filesystem::copy_file(test::app_path(ENGINE_HOST_PATH), host);

  // One recipe, and the same manifest twice, each building into its own directory.
  REQUIRE(write_text(repo / "palm.flora.json",
                     R"({"species":"date_palm","palm":{"seed":9,"height_m":5,"frond_count":8,)"
                     R"("leaflet_count":12,"dead_skirt_fronds":2,"triangle_budget":2000}})"));
  const std::string manifest_text =
      R"({"meshes":[{"source":"../palm.flora.json","output":"palm.clusters"}]})";
  for (const char* side : {"cli", "host"}) {
    std::filesystem::create_directories(repo / side);
    REQUIRE(write_text(repo / side / "meshes.json", manifest_text));
  }

  const Run by_cli = run({content.string(), "build-all", (repo / "cli" / "meshes.json").string()});
  REQUIRE_MESSAGE(by_cli.exit_code == 0, by_cli.out);
  const Run by_host =
      run({test::app_path(ENGINE_APP_PATH), "--host", host.string(), "--compact", "content.build",
           "{\"manifest\":" + json_string((repo / "host" / "meshes.json").generic_string()) +
               ",\"stats\":false}"});
  REQUIRE_MESSAGE(by_host.exit_code == 0, by_host.out);

  // The host's account of the build: one output, built, under the key build-all recorded.
  JsonValue result;
  REQUIRE(parse_json(by_host.out, result).ok);
  const JsonValue* outputs = result.find("outputs");
  REQUIRE((outputs != nullptr && outputs->size() == 1));
  const JsonValue* status = (*outputs)[0].find("status");
  REQUIRE(status != nullptr);
  CHECK(status->as_string() == "built");
  u64 host_key = 0;
  REQUIRE((*outputs)[0].find("build_key") != nullptr);
  REQUIRE((*outputs)[0].find("build_key")->get_u64(host_key));
  // build-all's line for the mesh, the first of its JSON lines.
  JsonValue line;
  REQUIRE(parse_json(by_cli.out.substr(0, by_cli.out.find('\n')), line).ok);
  u64 cli_key = 0;
  REQUIRE(line.find("build_key") != nullptr);
  REQUIRE(line.find("build_key")->get_u64(cli_key));
  CHECK_MESSAGE(host_key == cli_key,
                "content.build keyed the palm without the pack build-all used");

  const std::string a = read_bytes(repo / "cli" / "palm.clusters");
  const std::string b = read_bytes(repo / "host" / "palm.clusters");
  REQUIRE(!a.empty());
  REQUIRE(!b.empty());
  const bool same = a == b;  // not the bytes themselves into a failure's message
  CHECK_MESSAGE(
      same, "the two hosts' containers differ: " << a.size() << " and " << b.size() << " bytes");
}

#endif

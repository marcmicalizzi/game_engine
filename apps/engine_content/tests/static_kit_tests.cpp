// engine-content static-kit end to end (static_kit_commands.cpp; content/test-scenes/static-world/,
// roadmap T130): the kit written into the test's scratch directory, every mesh of it built by the
// ordinary `build` path, and the containers read back with `info`. What this holds is the scene's
// premise — **an imported asset carries nothing a generator adds** — so a world built from the kit
// measures what the renderer does for such an asset: no generator section, no motion or structural
// table, no impostor chain or coverage errors, no seat. The branch cards are alpha-tested and
// two-sided as a bought tree's are, and their cut keeps the share of the card the kit says.
#include <core/json/json.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <span>
#include <string>
#include <string_view>
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

// The last line that parses as a JSON object: `build` may log before its summary.
bool last_json(const std::string& text, JsonValue& out) {
  usize end = text.size();
  while (end > 0) {
    const usize start = text.rfind('\n', end - 1);
    const usize from = start == std::string::npos ? 0 : start + 1;
    const std::string line = text.substr(from, end - from);
    if (!line.empty() && line[0] == '{' && parse_json(line, out).ok) return true;
    if (start == std::string::npos) break;
    end = start;
  }
  return false;
}

}  // namespace

TEST_CASE("engine-content static-kit: plain glTF meshes that carry nothing a generator adds") {
  const test::TempDir tmp("engine_content_static_kit");
  const std::string dir = tmp.file("kit");
  const Output kit = run({"static-kit", dir});
  REQUIRE_MESSAGE(kit.exit_code == 0, kit.text);
  JsonValue summary;
  REQUIRE_MESSAGE(last_json(kit.text, summary), kit.text);
  const JsonValue* triangles = summary.find("triangles");
  REQUIRE(triangles != nullptr);
  // The shapes the scene's page describes: a ground of 160 x 160 quads, a tree of 117 cards over its
  // wood, 24 cards of shrub, three of grass, a sphere of 48 x 24 facets.
  u64 v = 0;
  CHECK((triangles->find("ground")->get_u64(v) && v == 2u * 160u * 160u));
  CHECK((triangles->find("bush")->get_u64(v) && v == 48u));
  CHECK((triangles->find("grass")->get_u64(v) && v == 6u));
  CHECK((triangles->find("rock")->get_u64(v) && v == 2u * 48u * 24u));
  REQUIRE(triangles->find("tree")->get_u64(v));
  CHECK(v == 2u * 117u + 2u * 16u * 8u + 6u * 2u * 8u * 3u);
  // A branch card is about a third leaf: what a crown prior keyed on the card's own alpha reads.
  f64 coverage = 0.0;
  REQUIRE(summary.find("alpha_coverage")->find("branch-card.png")->get_f64(coverage));
  CHECK(coverage > 0.25);
  CHECK(coverage < 0.6);

  for (const char* stem : {"ground", "tree", "bush", "grass", "rock"}) {
    CAPTURE(stem);
    const std::string out = tmp.file(std::string(stem) + ".clusters");
    const Output built = run({"build", dir + "/" + stem + ".gltf", out, "--no-textures"});
    REQUIRE_MESSAGE(built.exit_code == 0, built.text);
    const Output info = run({"info", out});
    REQUIRE_MESSAGE(info.exit_code == 0, info.text);
    JsonValue read;
    REQUIRE_MESSAGE(last_json(info.text, read), info.text);
    CHECK(read.find("generator") == nullptr);
    CHECK(read.find("seat") == nullptr);
    CHECK(read.find("impostors") == nullptr);
    const JsonValue* sections = read.find("sections");
    REQUIRE(sections != nullptr);
    for (usize s = 0; s < sections->size(); ++s) {
      const std::string_view name = (*sections)[s].find("name")->as_string();
      CAPTURE(name);
      for (const char* generated :
           {"generator", "motion_parts", "motion_vertices", "structural_parts", "impostors",
            "lod_coverage", "cover_mask", "sward_vertex", "seat"})
        CHECK(name != generated);
    }
  }
  CHECK(run({"static-kit"}).exit_code == 2);
  CHECK(run({"static-kit", dir, "--bogus"}).exit_code == 2);
}

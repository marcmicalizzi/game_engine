// An authored terrain drawn by the stock engine-view (docs/subsystems/tileset.md, "The provider";
// apps.md, "A scene naming a generator this build lacks"): engine-view links the tile set's `tiles`
// ground provider for its registrar, as it links the dunes', so a scene whose terrain names a tile
// set draws instead of being refused. Compiled where the tileset capability is. The test drives the
// real pair: engine-content synthesizes a small ridge and builds its tile set into the test's own
// scratch directory, and engine-view draws a scene naming that set offscreen and captures it. And
// a provider no build has is refused with the registry's sentence, which lists `tiles` among the
// providers this executable does carry — the link this test exists for.
#include "view_process.h"

#include <core/json/json.h>
#include <foundation/image/decode.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace engine;

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
};

Run run_app(const std::string& app, const std::vector<std::string>& args) {
  std::vector<std::string_view> argv;
  argv.push_back(app);
  for (const std::string& a : args)
    argv.push_back(a);
  Run run;
  run.exit_code = view_test::run_program(
      std::span<const std::string_view>(argv.data(), argv.size()), run.output);
  return run;
}

const std::string& view_exe() {
  static const std::string path = test::app_path(ENGINE_APP_PATH);
  return path;
}

// engine-content is built beside engine-view (apps/engine_content/CMakeLists.txt adds the
// dependency), and in a bundle sits beside it too.
std::string content_exe() {
  const std::filesystem::path app(view_exe());
  return (app.parent_path() / ("engine-content" + app.extension().string())).generic_string();
}

// A scene over a 64 m grid whose terrain is the given provider, with the given extra fields.
std::string scene_text(const std::string& provider, const std::string& extra) {
  return std::string(R"({"format": "engine.scene.v1", "name": "tiles-view",
  "terrain": {"size": 65, "extent": 32, "provider": ")") +
         provider + "\"" + extra + "}}\n";
}

}  // namespace

TEST_CASE("engine-view: a scene naming a built tile set draws it, offscreen") {
  const test::TempDir tmp("engine_view_tiles");
  // A ridge 129 points a side at 50 cm, centred on the origin: 64 m, the scene's grid.
  const std::string heightfield = tmp.file("ridge.json");
  const Run synth = run_app(content_exe(),
                            {"terrain-synth", heightfield, "--kind", "ridge", "--size", "129",
                             "--spacing-mm", "500", "--seed", "3", "--origin-mm", "-32000,-32000"});
  REQUIRE_MESSAGE(synth.exit_code == 0, synth.output);
  const std::string set_dir = tmp.file("set");
  const Run tiles =
      run_app(content_exe(), {"terrain-tiles", heightfield, set_dir, "--cells", "32"});
  REQUIRE_MESSAGE(tiles.exit_code == 0, tiles.output);
  REQUIRE(io::exists(io::join_path(set_dir, "manifest.json")));

  // The set's manifest named relative to the scene, as a game's scene names it.
  const std::string scene = tmp.file("scene.json");
  REQUIRE(io::write_file(scene,
                         scene_text("tiles", R"(, "tiles": {"manifest": "set/manifest.json"})")) ==
          io::Status::Ok);
  const std::string capture = tmp.file("tiles.png");
  const Run drawn = run_app(
      view_exe(), {"--offscreen", "--scene", scene, "--frames", "2", "--width", "160", "--height",
                   "96", "--shadows", "off", "--capture", capture, "--ddc", tmp.file("ddc")});
  if (drawn.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << drawn.output);
    return;
  }
  REQUIRE_MESSAGE(drawn.exit_code == 0, drawn.output);
  image::Image picture;
  REQUIRE(image::read_image(capture, picture) == io::Status::Ok);
  REQUIRE(picture.width == 160);
  REQUIRE(picture.height == 96);
  // Not one colour: the ground is in the picture, not only the sky behind it (the default orbit
  // frames the 64 m ridge as about an eighth of the picture).
  u64 other = 0;
  for (u32 p = 1; p < picture.width * picture.height; ++p)
    other += std::memcmp(&picture.pixels[p * 4], &picture.pixels[0], 3) != 0 ? 1u : 0u;
  MESSAGE(other << " of " << picture.width * picture.height << " pixels differ from the first");
  CHECK(other > picture.width * picture.height / 20);
}

TEST_CASE("engine-view: a ground provider no build has is refused, naming tiles among its own") {
  const test::TempDir tmp("engine_view_tiles_refused");
  const std::string scene = tmp.file("scene.json");
  REQUIRE(io::write_file(scene, scene_text("nonesuch", "")) == io::Status::Ok);
  const Run refused = run_app(view_exe(), {"--offscreen", "--scene", scene, "--frames", "1",
                                           "--width", "64", "--height", "64"});
  CHECK(refused.exit_code == 1);
  CHECK_MESSAGE(refused.output.find("names the ground provider \"nonesuch\", which this build "
                                    "does not have") != std::string::npos,
                refused.output);
  // The sentence lists what the executable carries; the tile set's provider is among them.
  const usize has = refused.output.find("(it has ");
  REQUIRE_MESSAGE(has != std::string::npos, refused.output);
  const usize end = refused.output.find(')', has);
  CHECK_MESSAGE(refused.output.substr(has, end - has).find("tiles") != std::string::npos,
                refused.output);
}

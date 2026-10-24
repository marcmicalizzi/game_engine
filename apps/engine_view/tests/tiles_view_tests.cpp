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
#include <foundation/image/png.h>
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
  // The real viewer, at a stopped terrain clock, must select terrain levels for the authored
  // provider. This flag streams terrain tiles and leaves flora/skin motion available.
  const Run streamed = run_app(view_exe(), {"--offscreen",   "--scene",
                                            scene,           "--terrain-tiles",
                                            "--terrain-far", "1",
                                            "--time-rate",   "0",
                                            "--frames",      "2",
                                            "--width",       "160",
                                            "--height",      "96",
                                            "--shadows",     "off",
                                            "--capture",     tmp.file("streamed.png"),
                                            "--ddc",         tmp.file("ddc")});
  REQUIRE_MESSAGE(streamed.exit_code == 0, streamed.output);
  CHECK_MESSAGE(streamed.output.find("\"layout\":\"tiles\"") != std::string::npos, streamed.output);
  CHECK(streamed.output.find("trees drawn rigid") == std::string::npos);
}

TEST_CASE(
    "engine-view: authored material layers draw on native ground through the stock scene path") {
  const test::TempDir tmp("engine_view_native_materials");
  const u8 colour[] = {20, 180, 40, 255};
  REQUIRE(image::write_png(tmp.file("ground.png"), 1, 1, 4, colour) == io::Status::Ok);
  const std::string scene = tmp.file("scene.json");
  REQUIRE(io::write_file(scene, scene_text("waves", R"(,"dune_height":0,
    "materials":{"layers":[{"base_color":"ground.png","projection":"Map"}]})")) == io::Status::Ok);
  const std::string capture = tmp.file("native.png");
  const Run drawn =
      run_app(view_exe(), {"--offscreen", "--scene", scene, "--frames", "2", "--width", "160",
                           "--height", "96", "--view", "albedo", "--shadows", "off", "--capture",
                           capture, "--ddc", tmp.file("ddc")});
  if (drawn.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << drawn.output);
    return;
  }
  REQUIRE_MESSAGE(drawn.exit_code == 0, drawn.output);
  image::Image picture;
  REQUIRE(image::read_image(capture, picture) == io::Status::Ok);
  u32 green = 0;
  for (u32 p = 0; p < picture.width * picture.height; ++p)
    green += picture.pixels[4 * p + 1] > picture.pixels[4 * p] * 2 &&
                     picture.pixels[4 * p + 1] > picture.pixels[4 * p + 2] * 2
                 ? 1u
                 : 0u;
  CHECK(green > 500);
}

TEST_CASE(
    "engine-view: ground masks draw their regions through the stock scene path, and a level "
    "handover changes none") {
  // A flat authored ground, 64 m at 50 cm, whose mask gives the west half a red layer and the
  // east half a blue one (renderer.md, "Ground materials by place"; tileset.md, "The mask level").
  const test::TempDir tmp("engine_view_masks");
  const std::string heightfield = tmp.file("flat.json");
  const Run synth =
      run_app(content_exe(), {"terrain-synth", heightfield, "--kind", "flat", "--size", "129",
                              "--spacing-mm", "500", "--origin-mm", "-32000,-32000"});
  REQUIRE_MESSAGE(synth.exit_code == 0, synth.output);
  const std::string scene = tmp.file("scene.json");
  REQUIRE(io::write_file(scene, R"({"format": "engine.scene.v1", "name": "masks-view",
  "camera_path": "path.json",
  "terrain": {"size": 17, "extent": 32, "provider": "tiles",
    "tiles": {"heightfield": "flat.json", "cells": 32, "ddc": "ddc"},
    "materials": {"layer_budget": 2, "layers": [
      {"name": "red", "tint": [0.8, 0.02, 0.02]}, {"name": "blue", "tint": [0.02, 0.02, 0.8]}]},
    "masks": {"spacing_mm": 1000, "tile_cells": 8, "regions": [
      {"name": "west", "polygon": [[-40, 0, -40], [0.25, 0, -40], [0.25, 0, 40], [-40, 0, 40]],
       "material": "red", "feather_m": 0},
      {"name": "east", "polygon": [[0.25, 0, -40], [40, 0, -40], [40, 0, 40], [0.25, 0, 40]],
       "material": "blue", "feather_m": 0}]}}})") == io::Status::Ok);
  // Looking down at the seam from 24 m, a little tilted, so near and far levels share the view.
  REQUIRE(io::write_file(tmp.file("path.json"), R"({"format": "engine.camera-path.v1",
  "name": "down", "interpolation": "Linear", "fps": 60, "znear": 0.2, "keys": [
  {"time": 0, "position": [0, 24, 18], "target": [0, 0, -4]},
  {"time": 10, "position": [0, 24, 18], "target": [0, 0, -4]}]})") == io::Status::Ok);
  const auto draw = [&](const char* name, std::vector<std::string> extra) {
    std::vector<std::string> args = {"--offscreen", "--scene",         scene,       "--frames",
                                     "3",           "--width",         "160",       "--height",
                                     "96",          "--shadows",       "off",       "--time-rate",
                                     "0",           "--terrain-tiles", "--capture", tmp.file(name)};
    args.push_back("--camera-path");
    args.push_back(tmp.file("path.json"));
    for (std::string& e : extra)
      args.push_back(std::move(e));
    const Run run = run_app(view_exe(), args);
    image::Image picture;
    if (run.exit_code == 0) REQUIRE(image::read_image(tmp.file(name), picture) == io::Status::Ok);
    return std::make_pair(run, std::move(picture));
  };
  // 0: neither, 1: red, 2: blue — the layer a pixel shows.
  const auto classes = [](const image::Image& p) {
    std::vector<u8> out(p.width * p.height);
    for (u32 i = 0; i < out.size(); ++i) {
      const u32 r = p.pixels[4 * i], g = p.pixels[4 * i + 1], b = p.pixels[4 * i + 2];
      out[i] = r > 2 * g && r > 2 * b ? 1 : (b > 2 * r && b > 2 * g ? 2 : 0);
    }
    return out;
  };
  auto [near_run, near] = draw("near.png", {"--view", "albedo", "--terrain-far", "1"});
  if (near_run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << near_run.output);
    return;
  }
  REQUIRE_MESSAGE(near_run.exit_code == 0, near_run.output);
  const std::vector<u8> a = classes(near);
  u32 red = 0, blue = 0, wrong_side = 0;
  for (u32 y = 0; y < near.height; ++y)
    for (u32 x = 0; x < near.width; ++x) {
      const u8 c = a[y * near.width + x];
      red += c == 1 ? 1u : 0u;
      blue += c == 2 ? 1u : 0u;
      // The seam is at x = 0.25 m, the picture's middle column: west is left.
      if ((c == 1 && x > near.width / 2 + 8) || (c == 2 && x + 8 < near.width / 2)) ++wrong_side;
    }
  MESSAGE(red << " red, " << blue << " blue, " << wrong_side << " on the wrong side");
  CHECK(red > 3000);
  CHECK(blue > 3000);
  CHECK(wrong_side == 0);
  // **A level handover changes no material**: the same view drawn with three far levels instead
  // of one; on flat ground the levels' geometry agrees, so every pixel shows the same layer.
  auto [far_run, far] = draw("far.png", {"--view", "albedo", "--terrain-far", "3"});
  REQUIRE_MESSAGE(far_run.exit_code == 0, far_run.output);
  const std::vector<u8> b = classes(far);
  u32 differ = 0;
  for (u32 i = 0; i < a.size(); ++i)
    differ += a[i] != b[i] ? 1u : 0u;
  CHECK(differ == 0);
  // The path tracer reads the same masks too: engine-view's --reference has its own window path,
  // so that agreement is the renderer's test (terrain_materials_tests.cpp).
}
TEST_CASE("engine-view: a ground provider no build has is refused, naming tiles among its own") {
  const test::TempDir tmp("engine_view_tiles_refused");
  const std::string scene = tmp.file("scene.json");
  REQUIRE(io::write_file(scene, scene_text("nonesuch", "")) == io::Status::Ok);
  const Run refused = run_app(view_exe(), {"--offscreen", "--scene", scene, "--frames", "1",
                                           "--width", "64", "--height", "64"});
  // The device is made before the scene is read, so a machine with none says so first.
  if (refused.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << refused.output);
    return;
  }
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

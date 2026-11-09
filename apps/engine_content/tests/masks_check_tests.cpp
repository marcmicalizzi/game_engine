// engine-content masks check end to end (roadmap T72; docs/subsystems/apps.md): a synthetic
// mountain whose slopes cross the soil, scree and bedrock bands is checked against a budget of
// four (no tile over: exit 0, a summary line) and of two (tiles over: exit 1, one JSON line each
// with its layers and its weakest, then the summary), and the usage and scene refusals.
#include <core/json/json.h>
#include <core/platform/process.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

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

std::vector<JsonValue> lines(const std::string& text) {
  std::vector<JsonValue> out;
  usize at = 0;
  while (at < text.size()) {
    usize end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    if (end > at) {
      JsonValue v;
      REQUIRE_MESSAGE(parse_json(text.substr(at, end - at), v).ok, text);
      out.push_back(v);
    }
    at = end + 1;
  }
  return out;
}

i64 whole(const JsonValue& v, const char* key) {
  const JsonValue* f = v.find(key);
  i64 n = -1;
  if (f != nullptr) (void)f->get_i64(n);
  return n;
}

std::string scene_text(u32 budget) {
  return std::string(R"({"format": "engine.scene.v1", "name": "masks-check", "terrain": {
    "size": 17, "extent": 128, "provider": "tiles",
    "tiles": {"heightfield": "ground.json", "cells": 32},
    "materials": {"layer_budget": )") +
         std::to_string(budget) + R"(, "layers": [
      {"name": "soil"}, {"name": "scree"}, {"name": "bedrock"}, {"name": "snow"}]},
    "masks": {"spacing_mm": 1000, "tile_cells": 16,
      "priors": {"soil": "soil", "scree": "scree", "bedrock": "bedrock", "snow": "snow",
                 "snowline_m": 60, "snow_blend_m": 8, "snow_aspect_m": 6}}}})";
}

}  // namespace

TEST_CASE("engine-content masks check: every tile over the budget is a line, the rest a summary") {
  const test::TempDir tmp("engine_content_masks_check");
  const Output synth =
      run({"terrain-synth", tmp.file("ground.json"), "--kind", "mountain", "--size", "257",
           "--spacing-mm", "1000", "--origin-mm", "-128000,-128000"});
  REQUIRE_MESSAGE(synth.exit_code == 0, synth.text);

  // A budget of four holds the mountain: exit 0 and a summary naming no tile.
  REQUIRE(io::write_file(tmp.file("four.json"), scene_text(4)) == io::Status::Ok);
  const Output four = run({"masks", "check", tmp.file("four.json"), "--ddc", tmp.file("ddc")});
  CAPTURE(four.text);
  CHECK(four.exit_code == 0);
  const std::vector<JsonValue> ok = lines(four.text);
  REQUIRE(ok.size() == 1);
  CHECK(whole(ok[0], "over") == 0);
  CHECK(whole(ok[0], "budget") == 4);
  CHECK(whole(ok[0], "tiles") > 0);
  const i64 largest = whole(ok[0], "largest");
  CHECK(largest >= 3);  // the soil, scree and bedrock bands meet on the slopes

  // A budget of two refuses every tile naming three: exit 1, a line each, the summary last.
  REQUIRE(io::write_file(tmp.file("two.json"), scene_text(2)) == io::Status::Ok);
  const Output two = run({"masks", "check", tmp.file("two.json"), "--ddc", tmp.file("ddc")});
  CAPTURE(two.text);
  CHECK(two.exit_code == 1);
  const std::vector<JsonValue> over = lines(two.text);
  REQUIRE(over.size() >= 3);
  const JsonValue& summary = over.back();
  CHECK(whole(summary, "budget") == 2);
  CHECK(whole(summary, "largest") == largest);
  CHECK(whole(summary, "over") == static_cast<i64>(over.size() - 1));
  for (usize i = 0; i + 1 < over.size(); ++i) {
    CAPTURE(i);
    const JsonValue* layers = over[i].find("layers");
    REQUIRE(layers != nullptr);
    CHECK(whole(over[i], "count") > 2);
    CHECK(over[i].find("tile") != nullptr);
    CHECK(over[i].find("weakest") != nullptr);
  }
}

TEST_CASE("engine-content masks check: usage is 2 and a scene without masks is 1") {
  const test::TempDir tmp("engine_content_masks_check_usage");
  CHECK(run({"masks"}).exit_code == 2);
  CHECK(run({"masks", "check"}).exit_code == 2);
  CHECK(run({"masks", "check", "scene.json", "--bogus"}).exit_code == 2);
  CHECK(run({"masks", "check", tmp.file("missing.json")}).exit_code == 1);
  REQUIRE(io::write_file(tmp.file("plain.json"), R"({"format": "engine.scene.v1"})") ==
          io::Status::Ok);
  CHECK(run({"masks", "check", tmp.file("plain.json")}).exit_code == 1);
}

namespace {

// A flat 40 m ground, optionally with a deep cut across it, and three strong regions in the tile
// (0, 1) (x 0 to 16 m, z 16 to 32 m) that the cut crosses: the soil and the regions are four
// layers; the cut's steep banks add bedrock as a fifth (roadmap T74).
std::string cut_scene_text(bool with_cut) {
  return std::string(R"({"format": "engine.scene.v1", "name": "masks-check-cut", "terrain": {
    "size": 17, "extent": 64, "provider": "tiles",
    "tiles": {"heightfield": "flat.json", "cells": 16, "ring_spacing_mm": 1000, "far_ratio": 1,
              "far_levels": 2)") +
         (with_cut ? R"(, "cuts": [{"name": "run", "points": [[5, 0, 20], [35, 0, 20]],
              "width_m": [8, 8], "depth_m": [4, 4], "bank_slope": 3}])"
                   : "") +
         R"(},
    "materials": {"layer_budget": 4, "layers": [
      {"name": "soil"}, {"name": "scree"}, {"name": "bedrock"}, {"name": "snow"},
      {"name": "gravel"}, {"name": "sand"}, {"name": "trail"}]},
    "masks": {"spacing_mm": 1000, "tile_cells": 16,
      "priors": {"soil": "soil", "scree": "scree", "bedrock": "bedrock", "snow": "snow",
                 "snowline_m": 60, "snow_blend_m": 8, "snow_aspect_m": 6},
      "regions": [
        {"name": "a", "center": [4, 0, 28], "radius_m": 1.5, "material": "gravel", "feather_m": 0},
        {"name": "b", "center": [8, 0, 28], "radius_m": 1.5, "material": "sand", "feather_m": 0},
        {"name": "c", "center": [12, 0, 28], "radius_m": 1.5, "material": "trail",
         "feather_m": 0}]}}})";
}

}  // namespace

TEST_CASE("engine-content masks check: it composes the ground a load composes, cuts included") {
  const test::TempDir tmp("engine_content_masks_check_cut");
  const Output synth = run({"terrain-synth", tmp.file("flat.json"), "--kind", "flat", "--size",
                            "161", "--spacing-mm", "250"});
  REQUIRE_MESSAGE(synth.exit_code == 0, synth.text);
  // Without the cut the four layers fit.
  REQUIRE(io::write_file(tmp.file("plain.json"), cut_scene_text(false)) == io::Status::Ok);
  const Output plain = run({"masks", "check", tmp.file("plain.json"), "--ddc", tmp.file("ddc")});
  CAPTURE(plain.text);
  CHECK(plain.exit_code == 0);
  // With it the banks add a fifth: the tile is reported and the exit is 1.
  REQUIRE(io::write_file(tmp.file("cut.json"), cut_scene_text(true)) == io::Status::Ok);
  const Output cut = run({"masks", "check", tmp.file("cut.json"), "--ddc", tmp.file("ddc")});
  CAPTURE(cut.text);
  CHECK(cut.exit_code == 1);
  const std::vector<JsonValue> over = lines(cut.text);
  REQUIRE(over.size() >= 2);
  CHECK(whole(over.back(), "over") >= 1);
  CHECK(whole(over.front(), "count") >= 5);  // bedrock, and scree where the banks meet the flat
}

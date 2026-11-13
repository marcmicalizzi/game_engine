// engine-content masks check end to end (roadmap T72; docs/subsystems/apps.md): a synthetic
// mountain whose slopes cross the soil, scree and bedrock bands is checked against a budget of
// four (no tile over: exit 0, a summary line) and of two (tiles over: exit 1, one JSON line each
// with its layers and its weakest, then the summary), and the usage and scene refusals. Then the
// level's size and the order a load refuses in (T121, T122, T127; tileset.md, "What a load refuses
// before it builds"): the check and the load refuse a level over `resident_mib` in one sentence,
// the load before it builds anything where the header sizes it, a raised figure holds the cooked
// level as it stands, and the check composes the ground the scene's entries plan.
#include <core/json/json.h>
#include <core/platform/process.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/tileset.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain_materials.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace engine;

namespace {

struct Output {
  i32 exit_code = -1;
  std::string text;
};

Output run(std::vector<std::string> args, bool merge_stderr = false) {
  static const std::string exe = test::app_path(ENGINE_APP_PATH);
  std::vector<std::string_view> argv;
  argv.push_back(exe);
  for (const std::string& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Output out;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error, merge_stderr)) {
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
    // W113: each layer says what the format would store and at how many of the tile's points;
    // a counted layer stores at least one 255th at one point at least.
    const i64 tile_points = whole(over[i], "tile_points");
    CHECK(tile_points > 0);
    for (usize k = 0; k < layers->size(); ++k) {
      CAPTURE(k);
      const JsonValue& layer = (*layers)[k];
      CHECK(whole(layer, "stored") >= 1);
      CHECK(whole(layer, "points") >= 1);
      CHECK(whole(layer, "points") <= tile_points);
    }
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
         (with_cut ? R"(, "edits": [{"name": "run", "points": [[5, 0, 20], [35, 0, 20]],
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

// ---- the level's size, and the order a load refuses in (T121, T122, T127) ----------------------

namespace {

// The 256 m mountain of the first case in tiles of 256 cells: four tiles in the level's range,
// each 110,516 words. With the priors' three slope roles every point's weights sum to one, so every
// tile is present and the level is (4 + 4 * 110,516) * 4 = 1,768,272 bytes, 1.7 MiB, known from
// the heightfield's header alone (`tileset::mask_level_full`). `sparse`: no priors, a gravel patch
// in three of the four tiles, so three are present, 1,326,208 bytes, 1.3 MiB, which only the
// composition can say.
std::string resident_scene(u32 resident_mib, bool sparse) {
  const std::string body =
      sparse ? R"("regions": [
        {"name": "a", "center": [-64, 0, -64], "radius_m": 10, "material": "gravel"},
        {"name": "b", "center": [64, 0, -64], "radius_m": 10, "material": "gravel"},
        {"name": "c", "center": [-64, 0, 64], "radius_m": 10, "material": "gravel"}])"
             : R"("priors": {"soil": "soil", "scree": "scree", "bedrock": "bedrock", "snow": "snow",
                 "snowline_m": 60, "snow_blend_m": 8, "snow_aspect_m": 6})";
  return std::string(R"({"format": "engine.scene.v1", "name": "masks-resident", "terrain": {
    "size": 17, "extent": 128, "provider": "tiles",
    "tiles": {"heightfield": "ground.json", "cells": 32},
    "materials": {"layer_budget": 4, "layers": [
      {"name": "soil"}, {"name": "scree"}, {"name": "bedrock"}, {"name": "snow"},
      {"name": "gravel"}]},
    "masks": {"spacing_mm": 1000, "tile_cells": 256, "resident_mib": )") +
         std::to_string(resident_mib) + ", " + body + "}}}";
}

void write_mountain(const test::TempDir& tmp) {
  const Output synth =
      run({"terrain-synth", tmp.file("ground.json"), "--kind", "mountain", "--size", "257",
           "--spacing-mm", "1000", "--origin-mm", "-128000,-128000"});
  REQUIRE_MESSAGE(synth.exit_code == 0, synth.text);
}

// A load's read of the scene (what engine-view and engine-host do before the upload), in process.
bool load(const std::string& path, const std::string& ddc, renderer::SceneDesc& desc,
          std::string& error) {
  desc = renderer::SceneDesc{};
  desc.ddc = ddc;
  return renderer::read_scene_file(path, renderer::SceneFileOptions{}, desc, error);
}

u32 files_under(const std::string& dir) {
  u32 count = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec))
    count += entry.is_regular_file() ? 1u : 0u;
  return count;
}

// The one mask archive under a derived-data root (`<ddc>/tilemasks/<key>/1000.masks`).
std::vector<std::filesystem::path> mask_archives(const std::string& ddc) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(io::join_path(ddc, "tilemasks"), ec))
    if (entry.is_regular_file() && entry.path().extension() == ".masks")
      out.push_back(entry.path());
  return out;
}

}  // namespace

TEST_CASE("masks: a level over resident_mib is refused by the check in the load's words (T121)") {
  const test::TempDir tmp("engine_content_masks_resident");
  write_mountain(tmp);
  for (const bool sparse : {false, true}) {
    CAPTURE(sparse);
    const std::string scene = tmp.file(sparse ? "sparse.json" : "full.json");
    REQUIRE(io::write_file(scene, resident_scene(1, sparse)) == io::Status::Ok);
    const std::string ddc = tmp.file(sparse ? "ddc-sparse" : "ddc-full");
    // The load refuses it, before or after its cook (T122)...
    renderer::SceneDesc desc;
    std::string load_error;
    CHECK_FALSE(load(scene, ddc, desc, load_error));
    const std::string sentence = sparse ? "the mask level is 1.3 MiB resident (3 tiles of the 4 "
                                          "in its range), over the block's resident_mib of 1; "
                                          "raise it to 2 or more"
                                        : "the mask level is 1.7 MiB resident (4 tiles of the 4 "
                                          "in its range), over the block's resident_mib of 1; "
                                          "raise it to 2 or more";
    CHECK_MESSAGE(load_error.find(sentence) != std::string::npos, load_error);
    // ...and the check refuses it, exit 1, with the same sentence. Before T121 the check printed
    // no size and passed it.
    const Output refused = run({"masks", "check", scene, "--ddc", ddc}, true);
    CAPTURE(refused.text);
    CHECK(refused.exit_code == 1);
    CHECK(refused.text.find(sentence) != std::string::npos);
    // Raised, both accept it; the check's summary says what the level holds against the figure.
    REQUIRE(io::write_file(scene, resident_scene(2, sparse)) == io::Status::Ok);
    CHECK_MESSAGE(load(scene, ddc, desc, load_error), load_error);
    const Output fits = run({"masks", "check", scene, "--ddc", ddc});
    CAPTURE(fits.text);
    CHECK(fits.exit_code == 0);
    const std::vector<JsonValue> summary = lines(fits.text);
    REQUIRE_FALSE(summary.empty());
    CHECK(whole(summary.back(), "present") == (sparse ? 3 : 4));
    CHECK(whole(summary.back(), "resident_bytes") == (sparse ? 1326208 : 1768272));
    CHECK(whole(summary.back(), "resident_mib") == 2);
  }
}

TEST_CASE("masks: a load refuses a level it can size from the header before it builds (T122)") {
  const test::TempDir tmp("engine_content_masks_early");
  write_mountain(tmp);
  const std::string scene = tmp.file("full.json");
  REQUIRE(io::write_file(scene, resident_scene(1, false)) == io::Status::Ok);
  // Every tile present, so the size is known from the heightfield's header: refused at the
  // provider's make, before the tile set is found or built, so the derived-data root holds
  // nothing — no tile set, no mask level. Before T122 the read accepted it, and the upload
  // refused it after the expansion and the mesh loads.
  renderer::SceneDesc desc;
  std::string error;
  CHECK_FALSE(load(scene, tmp.file("ddc"), desc, error));
  CHECK_MESSAGE(error.find("over the block's resident_mib of 1") != std::string::npos, error);
  CHECK(files_under(tmp.file("ddc")) == 0);
  // The sparse level cannot be sized before its cook: the read cooks it, then refuses, still
  // before a placement is expanded (scene.cpp, `read_scene_file`).
  const std::string sparse = tmp.file("sparse.json");
  REQUIRE(io::write_file(sparse, resident_scene(1, true)) == io::Status::Ok);
  CHECK_FALSE(load(sparse, tmp.file("ddc-sparse"), desc, error));
  CHECK_MESSAGE(error.find("over the block's resident_mib of 1") != std::string::npos, error);
  CHECK(mask_archives(tmp.file("ddc-sparse")).size() == 1);
}

TEST_CASE("masks: raising resident_mib holds the cooked level as it stands (T127)") {
  const test::TempDir tmp("engine_content_masks_raise");
  write_mountain(tmp);
  const std::string scene = tmp.file("sparse.json");
  const std::string ddc = tmp.file("ddc");
  REQUIRE(io::write_file(scene, resident_scene(1, true)) == io::Status::Ok);
  renderer::SceneDesc desc;
  std::string error;
  CHECK_FALSE(load(scene, ddc, desc, error));  // cooked, then refused for its size
  const std::vector<std::filesystem::path> cooked = mask_archives(ddc);
  REQUIRE(cooked.size() == 1);
  std::error_code ec;
  const auto written = std::filesystem::last_write_time(cooked[0], ec);
  // The figure raised: the same level is found by its key and held to the scene's new figure, by
  // the read and by the upload's preparation (which before T127 took the figure the cook wrote
  // into the manifest, and refused the raised scene until the level was deleted by hand).
  REQUIRE(io::write_file(scene, resident_scene(2, true)) == io::Status::Ok);
  REQUIRE_MESSAGE(load(scene, ddc, desc, error), error);
  renderer::TerrainMaskUpload upload;
  CHECK_MESSAGE(renderer::prepare_terrain_masks(desc.terrain, upload, &error), error);
  CHECK(upload.tiles == 3);
  const std::vector<std::filesystem::path> after = mask_archives(ddc);
  REQUIRE(after.size() == 1);
  CHECK(after[0] == cooked[0]);
  CHECK(std::filesystem::last_write_time(after[0], ec) == written);  // nothing recooked
  // Lowered again, the upload refuses the same level for the scene's figure.
  desc.terrain.masks->resident_mib = 1;
  CHECK_FALSE(renderer::prepare_terrain_masks(desc.terrain, upload, &error));
  CHECK(error.find("over the block's resident_mib of 1") != std::string::npos);
}

namespace {

// A hillside 64 m square at 25 cm a point (`apps/engine_view/tests/ground_plan_tests.cpp`'s): 10 %
// across x, 4 % across z and a twist, every point between 4.8 and 7.3 degrees.
void write_hill(const test::TempDir& tmp) {
  tileset::Heightfield field;
  field.width = 257;
  field.height = 257;
  field.spacing_mm = 250;
  field.origin_x_mm = -32000;
  field.origin_z_mm = -32000;
  for (u32 j = 0; j < field.height; ++j)
    for (u32 i = 0; i < field.width; ++i) {
      const f64 x = 0.25 * static_cast<f64>(i) - 32.0, z = 0.25 * static_cast<f64>(j) - 32.0;
      field.data.push_back(static_cast<f32>(8.0 + 0.10 * x + 0.04 * z + 0.0005 * x * z));
    }
  std::string error;
  REQUIRE_MESSAGE(tileset::write_heightfield(tmp.file("hill.json"), field, error), error);
}

// The hill under priors that read it as bedrock everywhere (past 4 degrees), with a budget of two;
// `routes`: a footpath along the contour, whose treads the routes generator plans and benches into
// the ground at the read (R144). A tread is near level across, so it is soil, with scree where it
// meets the hill: three layers in the tiles it crosses, over the budget, on the planned ground
// only.
std::string hill_scene(bool routes, u32 budget = 2) {
  return std::string(R"({"format":"engine.scene.v1","name":"masks-planned","terrain":{"size":17,
    "extent":32,"provider":"tiles","seed":3,"tiles":{"heightfield":"hill.json","ddc":"ddc",
      "cells":32,"far_levels":0},
    "materials":{"layer_budget":)") +
         std::to_string(budget) +
         R"(,"layers":[{"name":"soil"},{"name":"scree"},{"name":"bedrock"}]},
    "masks":{"spacing_mm":250,"tile_cells":16,"priors":{"soil":"soil","scree":"scree",
      "bedrock":"bedrock","scree_deg":2,"bedrock_deg":3.5,"slope_blend_deg":0.5}}},
    "placements":[)" +
         std::string(routes
                         ? R"({"generator":"routes","params":{"name":"way","region_min":[-28,-28],
      "region_max":[28,28],"routes":[{"name":"contour","waypoints":[[10,0,-24],[0,0,24]]}]}})"
                         : "") +
         "]}";
}

}  // namespace

TEST_CASE("masks: the check composes the ground the entries plan, as the load does (T121)") {
  if (scene_gen::GeneratorRegistry::global().find_placement("routes") == nullptr) {
    MESSAGE("this configuration has no routes capability: nothing plans the ground");
    return;
  }
  const test::TempDir tmp("engine_content_masks_planned");
  write_hill(tmp);
  // The bare hill: bedrock inside; only the tiles on its far edges, where the set's edge points
  // stand level past the last row, may name more.
  REQUIRE(io::write_file(tmp.file("bare.json"), hill_scene(false)) == io::Status::Ok);
  const Output bare = run({"masks", "check", tmp.file("bare.json")});
  CAPTURE(bare.text);
  const std::vector<JsonValue> bare_lines = lines(bare.text);
  REQUIRE_FALSE(bare_lines.empty());
  const i64 bare_over = whole(bare_lines.back(), "over");
  // The footpath's treads, planned at the read: the load refuses the tiles they cross besides, and
  // the check names the same count, where before T121 it read the file's terrain alone and found
  // the bare hill's.
  REQUIRE(io::write_file(tmp.file("path.json"), hill_scene(true)) == io::Status::Ok);
  renderer::SceneDesc desc;
  std::string error;
  CHECK_FALSE(load(tmp.file("path.json"), std::string(), desc, error));
  const Output path = run({"masks", "check", tmp.file("path.json")});
  CAPTURE(path.text);
  CHECK(path.exit_code == 1);
  const std::vector<JsonValue> over = lines(path.text);
  REQUIRE(over.size() >= 2);
  const i64 path_over = whole(over.back(), "over");
  CHECK(path_over > bare_over);
  CHECK_MESSAGE(
      error.find("terrain.masks: " + std::to_string(path_over) +
                 " tiles name more layers than materials.layer_budget is 2") != std::string::npos,
      error);
  // Tiles the footpath crosses in the hill's middle are among them, where the bare hill has none
  // (its tiles over are on the set's far edges, 4 m tiles 7 and 8 out).
  bool crossed = false;
  for (const JsonValue& line : over) {
    const JsonValue* tile = line.find("tile");
    if (tile == nullptr || !tile->is_array() || tile->size() != 2) continue;
    i64 tx = 0, tz = 0;
    (void)(*tile)[0].get_i64(tx);
    (void)(*tile)[1].get_i64(tz);
    crossed = crossed || (tx > -6 && tx < 6 && tz > -6 && tz < 6);
  }
  CHECK(crossed);
  // With room for the three layers, `terrain-masks` cooks the level over the same planned ground,
  // under the key a load computes: the load then finds it and cooks nothing. Before T121 the step
  // built the hill without the treads, and the load cooked a second level beside it.
  REQUIRE(io::write_file(tmp.file("roomy.json"), hill_scene(true, 3)) == io::Status::Ok);
  const Output cooked = run({"terrain-masks", tmp.file("roomy.json")});
  CAPTURE(cooked.text);
  REQUIRE(cooked.exit_code == 0);
  REQUIRE(mask_archives(tmp.file("ddc")).size() == 1);
  CHECK_MESSAGE(load(tmp.file("roomy.json"), std::string(), desc, error), error);
  CHECK(mask_archives(tmp.file("ddc")).size() == 1);
}

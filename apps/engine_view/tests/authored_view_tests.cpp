// End to end: a streamed world's own trees (renderer.md, "Authored instances per tile";
// authored.md). The scene's instances — small oaks with their card levels and spruces with none —
// handed to the `authored` generator a tile at a time draw the whole read's picture where every
// tile is in the inner ring, and a tile handed from one ring's form to the next changes no pixel of
// a still frame (engine-view's handover pass, with the cross-fade off). Compiled with the flora,
// world and authored capabilities; skips (exit 3) on a machine with no Vulkan device.
#include "view_process.h"

#include <core/json/json.h>
#include <foundation/image/decode.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace engine;

#if ENGINE_VIEW_WORLD && ENGINE_VIEW_TESTS_FLORA

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
};

Run view(const std::vector<std::string>& args) {
  static const std::string exe = test::app_path(ENGINE_APP_PATH);
  std::vector<std::string_view> argv;
  argv.push_back(exe);
  for (const std::string& a : args)
    argv.push_back(a);
  Run run;
  run.exit_code = view_test::run_program(
      std::span<const std::string_view>(argv.data(), argv.size()), run.output);
  return run;
}

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

// authored_scene_tests.cpp's trees, small enough for a debug build.
constexpr const char* k_oak =
    R"({"species":"broadleaf","broadleaf":{"seed":3,"height_m":8,"trunk_radius_m":0.25,)"
    R"("triangle_budget":8000,"card_bake_px":16,"impostor_px":32}})";
constexpr const char* k_spruce =
    R"({"species":"conifer","conifer":{"seed":5,"height_m":6,"trunk_radius_m":0.1,)"
    R"("whorls":6,"branches_per_whorl":4,"crown_radius_m":1.5,"triangle_budget":8000}})";

std::string scene_text(const char* world) {
  std::string text =
      R"({"format":"engine.scene.v1","name":"authored-view",)"
      R"("terrain":{"size":65,"extent":400,"seed":7,"dune_height":0.3,"dune_wavelength":60,)"
      R"("generator":"Waves"},)"
      R"("meshes":[{"name":"oak","path":"oak.flora.json"},)"
      R"({"name":"spruce","path":"spruce.flora.json"}],)"
      R"("scatters":[{"mesh":0,"center":[0,0,0],"radius_min":2,"radius_max":120,"count":70,)"
      R"("seed":11,"scale_min":0.8,"scale_max":1.2},)"
      R"({"mesh":1,"center":[0,0,0],"radius_min":2,"radius_max":120,"count":70,"seed":12}])";
  if (world != nullptr) text += std::string(R"(,"world":)") + world;
  return text + "}";
}

std::vector<std::string> base_args(const test::TempDir& tmp, const char* scene, const char* path,
                                   bool sway = false) {
  return {"--scene",
          tmp.file(scene),
          "--camera-path",
          tmp.file(path),
          "--offscreen",
          "--width",
          "320",
          "--height",
          "180",
          "--shadows",
          "off",
          "--tunable",
          sway ? "renderer.flora.motion=true" : "renderer.flora.motion=false",
          "--ddc",
          tmp.file("ddc")};
}

// The pixels whose colour differs between two captures of one size.
u32 differing_pixels(const std::string& a_path, const std::string& b_path) {
  image::Image a;
  image::Image b;
  REQUIRE(image::read_image(a_path, a) == io::Status::Ok);
  REQUIRE(image::read_image(b_path, b) == io::Status::Ok);
  REQUIRE(a.width == b.width);
  REQUIRE(a.height == b.height);
  u32 differing = 0;
  const u32 pixels = a.width * a.height;
  for (u32 p = 0; p < pixels; ++p) {
    bool d = false;
    for (u32 c = 0; c < 3; ++c)
      d = d || a.pixels[p * 4 + c] != b.pixels[p * 4 + c];
    differing += d ? 1u : 0u;
  }
  return differing;
}

JsonValue summary_of(const std::string& output) {
  const usize line_start = output.find_last_of('\n', output.size() - 2);
  const std::string last = output.substr(line_start == std::string::npos ? 0 : line_start + 1);
  JsonValue summary;
  REQUIRE_MESSAGE(parse_json(last, summary).ok, last);
  return summary;
}

u64 field_u64(const JsonValue& object, const char* name) {
  const JsonValue* v = object.find(name);
  u64 out = 0;
  if (v != nullptr) v->get_u64(out);
  return out;
}

}  // namespace

TEST_CASE("engine-view: a world's own trees streamed per tile draw the whole read's picture") {
  const test::TempDir tmp("engine_view_authored");
  REQUIRE(write_text(tmp.file("oak.flora.json"), k_oak));
  REQUIRE(write_text(tmp.file("spruce.flora.json"), k_spruce));
  REQUIRE(write_text(tmp.file("whole.json"), scene_text(nullptr)));
  // One ring of 30 tiles: every tile the trees stand on is in it, in the full mesh.
  // The 32 m tiles also contain the enlarged three-plane foliage bounds (G87).
  REQUIRE(write_text(tmp.file("streamed.json"),
                     scene_text(R"({"tile_size":32,"rings":[{"radius":30}]})")));
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"over","fps":10,"keys":[)"
                     R"({"time":0,"position":[110,30,80],"target":[0,4,0]}]})"));
  std::vector<std::string> whole_args = base_args(tmp, "whole.json", "path.json");
  for (const char* a : {"--frames", "3", "--capture"})
    whole_args.push_back(a);
  whole_args.push_back(tmp.file("whole.png"));
  const Run whole = view(whole_args);
  if (whole.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << whole.output);
    return;
  }
  REQUIRE_MESSAGE(whole.exit_code == 0, whole.output);
  std::vector<std::string> streamed_args = base_args(tmp, "streamed.json", "path.json");
  for (const char* a : {"--frames", "3", "--capture"})
    streamed_args.push_back(a);
  streamed_args.push_back(tmp.file("streamed.png"));
  const Run streamed = view(streamed_args);
  REQUIRE_MESSAGE(streamed.exit_code == 0, streamed.output);
  const JsonValue summary = summary_of(streamed.output);
  const JsonValue* world = summary.find("world");
  REQUIRE(world != nullptr);
  CHECK(field_u64(*world, "authored_streamed") == 140);
  CHECK(field_u64(*world, "tail_pairs") > 0);
  CHECK(field_u64(*world, "refused") == 0);

  image::Image a;
  image::Image b;
  REQUIRE(image::read_image(tmp.file("whole.png"), a) == io::Status::Ok);
  REQUIRE(image::read_image(tmp.file("streamed.png"), b) == io::Status::Ok);
  REQUIRE(a.width == b.width);
  REQUIRE(a.height == b.height);
  u32 differing = 0;
  const u32 pixels = a.width * a.height;
  for (u32 p = 0; p < pixels; ++p) {
    bool d = false;
    for (u32 c = 0; c < 3; ++c)
      d = d || a.pixels[p * 4 + c] != b.pixels[p * 4 + c];
    differing += d ? 1u : 0u;
  }
  MESSAGE("streamed against whole: " << differing << " of " << pixels << " pixels differ");
  CHECK(differing == 0);
}

TEST_CASE("engine-view: a tree handed to its next ring's distance form changes no pixel") {
  const test::TempDir tmp("engine_view_authored_handover");
  REQUIRE(write_text(tmp.file("oak.flora.json"), k_oak));
  REQUIRE(write_text(tmp.file("spruce.flora.json"), k_spruce));
  REQUIRE(write_text(
      tmp.file("streamed.json"),
      scene_text(R"({"tile_size":16,"rings":[{"radius":1.5},{"radius":3},{"radius":6}]})")));
  // Low across the trees from one side to the other, so tiles cross every ring both ways.
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"across","fps":10,"keys":[)"
                     R"({"time":0,"position":[-150,12,10],"target":[0,4,0]},)"
                     R"({"time":4,"position":[150,12,-10],"target":[250,4,0]}]})"));
  std::vector<std::string> args = base_args(tmp, "streamed.json", "path.json");
  for (const char* a : {"--frames", "40", "--lod-fade", "0", "--benchmark"})
    args.push_back(a);
  args.push_back(tmp.file("bench.jsonl"));
  args.push_back("--world-handover");
  args.push_back(tmp.file("handover.jsonl"));
  const Run run = view(args);
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  std::ifstream log(tmp.file("handover.jsonl"), std::ios::binary);
  std::string line;
  u32 handovers = 0;
  u64 changed = 0;
  u64 drawn = 0;
  while (std::getline(log, line)) {
    JsonValue record;
    if (!parse_json(line, record).ok || record.find("coverage_changed") == nullptr) continue;
    ++handovers;
    changed += field_u64(record, "coverage_changed") + field_u64(record, "depth_changed") +
               field_u64(record, "color_changed");
    drawn += field_u64(record, "tile_before");
  }
  MESSAGE(handovers << " handovers over " << drawn << " of their tiles' pixels, " << changed
                    << " changed");
  CHECK(handovers > 0);
  CHECK(drawn > 0);
  CHECK(changed == 0);
}

// Roadmap W74 (renderer.md, "Streamed trees sway"): the trees a tile brings sway as the whole
// read's do, at the same world time — in the inner ring's full mesh, and in the next ring's
// distance form, which is cut for the sway's padded spheres and sways with its source's parts.
TEST_CASE("engine-view: a world's streamed trees sway as the whole read's do") {
  const test::TempDir tmp("engine_view_authored_sway");
  REQUIRE(write_text(tmp.file("oak.flora.json"), k_oak));
  REQUIRE(write_text(tmp.file("spruce.flora.json"), k_spruce));
  REQUIRE(write_text(tmp.file("whole.json"), scene_text(nullptr)));
  REQUIRE(write_text(tmp.file("inner.json"),
                     scene_text(R"({"tile_size":32,"rings":[{"radius":30}]})")));
  // Two rings: the trees within two tiles of the camera are full meshes, the rest their ring-1
  // form, whose floor (33 m) is inside the sway's range.
  REQUIRE(write_text(tmp.file("rings.json"),
                     scene_text(R"({"tile_size":32,"rings":[{"radius":2},{"radius":30}]})")));
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"over","fps":10,"keys":[)"
                     R"({"time":0,"position":[110,30,80],"target":[0,4,0]}]})"));
  // Thirty frames: the sway's clock at half a second, every tree off its rest pose.
  const auto capture = [&](const char* scene, bool sway, const char* png,
                           const std::vector<const char*>& extra = {}) {
    std::vector<std::string> args = base_args(tmp, scene, "path.json", sway);
    for (const char* a : {"--frames", "30", "--lod-fade", "0", "--capture"})
      args.push_back(a);
    args.push_back(tmp.file(png));
    for (const char* a : extra)
      args.push_back(a);
    return view(args);
  };
  const Run whole = capture("whole.json", true, "whole.png");
  if (whole.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << whole.output);
    return;
  }
  REQUIRE_MESSAGE(whole.exit_code == 0, whole.output);
  const Run rigid = capture("whole.json", false, "rigid.png");
  REQUIRE_MESSAGE(rigid.exit_code == 0, rigid.output);
  const Run inner = capture("inner.json", true, "inner.png");
  REQUIRE_MESSAGE(inner.exit_code == 0, inner.output);
  // The second ring sways only when the range reaches its floor (the default, 0, keeps forms
  // rigid).
  const Run rings =
      capture("rings.json", true, "rings.png", {"--tunable", "renderer.flora.sway_m=100"});
  REQUIRE_MESSAGE(rings.exit_code == 0, rings.output);
  // Before W74: the same streamed world with its tail drawn rigid.
  const Run before =
      capture("inner.json", true, "before.png", {"--tunable", "renderer.flora.tail=false"});
  REQUIRE_MESSAGE(before.exit_code == 0, before.output);

  for (const Run* run : {&inner, &rings}) {
    const JsonValue summary = summary_of(run->output);
    const JsonValue* flora = summary.find("flora");
    REQUIRE(flora != nullptr);
    REQUIRE(flora->is_object());
    CHECK(field_u64(*flora, "tail_instances") == 140);
    CHECK(field_u64(*flora, "tail_records") >= 140);
  }
  const u32 swayed = differing_pixels(tmp.file("whole.png"), tmp.file("rigid.png"));
  const u32 inner_diff = differing_pixels(tmp.file("whole.png"), tmp.file("inner.png"));
  const u32 rings_diff = differing_pixels(tmp.file("whole.png"), tmp.file("rings.png"));
  const u32 before_diff = differing_pixels(tmp.file("whole.png"), tmp.file("before.png"));
  MESSAGE("the sway moves " << swayed << " pixels of the whole read; streamed in one ring "
                            << inner_diff << " differ, in two " << rings_diff
                            << ", with the tail rigid " << before_diff);
  CHECK(swayed > 20);  // 76 at 320 x 180 from 136 m, half a second in (2026-10-24)
  CHECK(inner_diff == 0);
  CHECK(rings_diff == 0);
  CHECK(before_diff > 20);
}

// Roadmap R95 (renderer.md, "The field kind"): a sward is posed by the pool pass from the frame's
// field block alone, so a streamed read draws the whole read's swaying grass to the pixel, in one
// ring and in two, takes no tail record and no part of the pose table, and a second run of the
// same frames draws the same bytes. The caster policy keeps it out of every cascade but the
// nearest and out of the pool there.
constexpr const char* k_sward =
    R"({"species":"sward","sward":{"species":"perennial_ryegrass","blades_m2":192,"density":0.5,)"
    R"("tillers_per_clump":4,"leaves_per_tiller":3,"clumpiness":0.6,"seed":8801,)"
    R"("triangle_budget":2400,"card_px":32}})";

std::string sward_text(const char* world) {
  std::string text =
      R"({"format":"engine.scene.v1","name":"sward-view",)"
      R"("terrain":{"size":65,"extent":200,"seed":7,"dune_height":0.05,"dune_wavelength":60,)"
      R"("generator":"Waves"},)"
      R"("meshes":[{"name":"sward","path":"sward.flora.json"}],)"
      R"("scatters":[{"mesh":0,"center":[0,0,0],"radius_min":0.5,"radius_max":28,"count":160,)"
      R"("seed":21}])";
  if (world != nullptr) text += std::string(R"(,"world":)") + world;
  return text + "}";
}

TEST_CASE("engine-view: a streamed sward sways as a field, as the whole read's does") {
  const test::TempDir tmp("engine_view_sward_field");
  REQUIRE(write_text(tmp.file("sward.flora.json"), k_sward));
  REQUIRE(write_text(tmp.file("whole.json"), sward_text(nullptr)));
  REQUIRE(write_text(tmp.file("inner.json"),
                     sward_text(R"({"tile_size":16,"rings":[{"radius":30}]})")));
  REQUIRE(write_text(tmp.file("rings.json"),
                     sward_text(R"({"tile_size":16,"rings":[{"radius":1.5},{"radius":30}]})")));
  // Knee height, looking along the grass.
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"low","fps":10,"keys":[)"
                     R"({"time":0,"position":[-3,0.9,-3],"target":[6,0.1,6]}]})"));
  const auto capture = [&](const char* scene, bool sway, const char* png,
                           const std::vector<const char*>& extra = {}) {
    std::vector<std::string> args = base_args(tmp, scene, "path.json", sway);
    for (const char* a : {"--frames", "30", "--lod-fade", "0", "--capture"})
      args.push_back(a);
    args.push_back(tmp.file(png));
    for (const char* a : extra)
      args.push_back(a);
    return view(args);
  };
  const Run whole = capture("whole.json", true, "whole.png");
  if (whole.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << whole.output);
    return;
  }
  REQUIRE_MESSAGE(whole.exit_code == 0, whole.output);
  const Run again = capture("whole.json", true, "again.png");
  REQUIRE_MESSAGE(again.exit_code == 0, again.output);
  const Run rigid = capture("whole.json", false, "rigid.png");
  REQUIRE_MESSAGE(rigid.exit_code == 0, rigid.output);
  const Run inner = capture("inner.json", true, "inner.png");
  REQUIRE_MESSAGE(inner.exit_code == 0, inner.output);
  const Run rings =
      capture("rings.json", true, "rings.png", {"--tunable", "renderer.flora.sway_m=100"});
  REQUIRE_MESSAGE(rings.exit_code == 0, rings.output);

  const JsonValue whole_summary = summary_of(whole.output);
  const JsonValue* whole_flora = whole_summary.find("flora");
  REQUIRE(whole_flora != nullptr);
  REQUIRE(whole_flora->is_object());
  CHECK(field_u64(*whole_flora, "field_instances") == 160);
  CHECK(field_u64(*whole_flora, "instances") == 0);  // nothing the CPU poses
  CHECK(field_u64(*whole_flora, "parts") == 0);
  CHECK(field_u64(*whole_flora, "untraced") == 160);
  CHECK(field_u64(*whole_flora, "casters_nearest") == 160);
  CHECK(field_u64(*whole_flora, "casters_rest") == 160);
  for (const Run* run : {&inner, &rings}) {
    const JsonValue summary = summary_of(run->output);
    const JsonValue* flora = summary.find("flora");
    REQUIRE(flora != nullptr);
    REQUIRE(flora->is_object());
    MESSAGE("streamed: " << field_u64(*flora, "field_instances") << " field instances, "
                         << field_u64(*flora, "tail_records") << " tail records, "
                         << field_u64(*flora, "tail_bytes") << " tail bytes, "
                         << field_u64(*flora, "pose_table_bytes") << " pose-table bytes");
    CHECK(field_u64(*flora, "field_instances") == 160);
    CHECK(field_u64(*flora, "tail_records") == 0);
    CHECK(field_u64(*flora, "tail_bytes") == 0);
    // The table is the field block a slot and nothing else: three slots of 160 bytes.
    CHECK(field_u64(*flora, "pose_table_bytes") == 480);
  }
  const u32 swayed = differing_pixels(tmp.file("whole.png"), tmp.file("rigid.png"));
  const u32 replay = differing_pixels(tmp.file("whole.png"), tmp.file("again.png"));
  const u32 inner_diff = differing_pixels(tmp.file("whole.png"), tmp.file("inner.png"));
  const u32 rings_diff = differing_pixels(tmp.file("whole.png"), tmp.file("rings.png"));
  MESSAGE("the field moves " << swayed << " pixels of the whole read; again " << replay
                             << " differ; streamed in one ring " << inner_diff << ", in two "
                             << rings_diff);
  CHECK(swayed > 100);
  CHECK(replay == 0);
  CHECK(inner_diff == 0);
  CHECK(rings_diff == 0);
}

// A refused commit keeps the device (renderer.md; R95): with the tail's ceiling at 1 MiB the
// streamed trees' room cannot grow, so their tiles' commits are refused with a sentence before
// anything changes, and the frames go on — the run ends well, its capture written, and nothing
// reaches the device as an out-of-memory loss.
TEST_CASE("engine-view: a tile commit past the tail's budget is refused and the device lives") {
  const test::TempDir tmp("engine_view_tail_refused");
  REQUIRE(write_text(tmp.file("oak.flora.json"), k_oak));
  REQUIRE(write_text(tmp.file("spruce.flora.json"), k_spruce));
  REQUIRE(write_text(tmp.file("inner.json"),
                     scene_text(R"({"tile_size":32,"rings":[{"radius":30}]})")));
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"over","fps":10,"keys":[)"
                     R"({"time":0,"position":[110,30,80],"target":[0,4,0]}]})"));
  std::vector<std::string> args = base_args(tmp, "inner.json", "path.json", true);
  for (const char* a :
       {"--frames", "8", "--tunable", "renderer.flora.tail_budget_mib=1", "--capture"})
    args.push_back(a);
  args.push_back(tmp.file("refused.png"));
  const Run run = view(args);
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  const JsonValue summary = summary_of(run.output);
  const JsonValue* flora = summary.find("flora");
  REQUIRE(flora != nullptr);
  REQUIRE(flora->is_object());
  const u64 refused = field_u64(*flora, "tail_refused");
  const usize sentence = run.output.find("renderer.flora.tail_budget_mib");
  MESSAGE("refused " << refused << " commits; "
                     << (sentence != std::string::npos ? run.output.substr(sentence, 200) : ""));
  CHECK(refused > 0);
  CHECK(sentence != std::string::npos);
  CHECK(field_u64(*flora, "tail_instances") == 0);  // the frame went on without the tiles
  image::Image picture;
  CHECK(image::read_image(tmp.file("refused.png"), picture) == io::Status::Ok);
}

// The caster policy (renderer.md, "The caster policy"): the sward's species casts into the nearest
// cascade alone, from its rest pose, and is never in the ray tracing chain's structures. Against a
// run with the policy off (every cascade, the swaying pose, traced): fewer cascade pairs, no pool
// block for a cascade's grass, and no cluster of it in the frame's structures.
TEST_CASE("engine-view: a sward casts into the nearest cascade at rest and is never traced") {
  const test::TempDir tmp("engine_view_sward_policy");
  REQUIRE(write_text(tmp.file("sward.flora.json"), k_sward));
  REQUIRE(write_text(tmp.file("whole.json"), sward_text(nullptr)));
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"low","fps":10,"keys":[)"
                     R"({"time":0,"position":[-3,0.9,-3],"target":[6,0.1,6]}]})"));
  const auto run_with = [&](const char* shadows, bool policy, const char* png) {
    std::vector<std::string> args = base_args(tmp, "whole.json", "path.json", true);
    for (std::string& a : args) {
      if (a == "off") a = shadows;  // base_args' `--shadows off`
    }
    for (const char* a : {"--frames", "12", "--lod-fade", "0", "--tunable"})
      args.push_back(a);
    args.push_back(policy ? "renderer.flora.caster_policy=true"
                          : "renderer.flora.caster_policy=false");
    args.push_back("--capture");
    args.push_back(tmp.file(png));
    args.push_back("--benchmark");
    args.push_back(tmp.file(std::string(png) + ".jsonl"));
    return view(args);
  };
  // The flight's summary, the benchmark file's last line: the median of a per-frame count.
  const auto median_of = [&](const char* png, const char* name,
                             const char* inner = nullptr) -> f64 {
    std::ifstream file(tmp.file(std::string(png) + ".jsonl"), std::ios::binary);
    std::string line;
    std::string last;
    while (std::getline(file, line))
      if (!line.empty()) last = line;
    JsonValue record;
    if (!parse_json(last, record).ok) return -1.0;
    const JsonValue* value = record.find(name);
    if (inner != nullptr && value != nullptr && value->is_object()) value = value->find(inner);
    if (value == nullptr || !value->is_object()) return -1.0;
    const JsonValue* median = value->find("median");
    f64 out = -1.0;
    if (median != nullptr) median->get_f64(out);
    return out;
  };
  const Run csm = run_with("csm", true, "csm.png");
  if (csm.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << csm.output);
    return;
  }
  REQUIRE_MESSAGE(csm.exit_code == 0, csm.output);
  const Run csm_all = run_with("csm", false, "csm_all.png");
  REQUIRE_MESSAGE(csm_all.exit_code == 0, csm_all.output);
  const f64 pairs = median_of("csm.png", "shadow_pairs");
  const f64 pairs_all = median_of("csm_all.png", "shadow_pairs");
  MESSAGE("cascades: " << pairs << " shadow pairs with the policy, " << pairs_all
                       << " without; the pictures differ by "
                       << differing_pixels(tmp.file("csm.png"), tmp.file("csm_all.png"))
                       << " pixels");
  CHECK(pairs >= 0.0);
  CHECK(pairs < pairs_all);

  const Run rt = run_with("rt", true, "rt.png");
  if (rt.exit_code == 0) {
    const Run rt_all = run_with("rt", false, "rt_all.png");
    REQUIRE_MESSAGE(rt_all.exit_code == 0, rt_all.output);
    // The chain's structures built a frame (the benchmark's `rt.built`): the ground's alone with
    // the policy, the ground's and the grass's without it.
    const f64 built = median_of("rt.png", "rt", "built");
    const f64 built_all = median_of("rt_all.png", "rt", "built");
    MESSAGE("the chain: " << built << " structures built a frame with the policy, " << built_all
                          << " without it");
    if (built >= 0.0 && built_all >= 0.0) CHECK(built < built_all);
  } else {
    MESSAGE("no traced shadows on this device: " << rt.output.substr(0, 200));
  }
}

// A tile that leaves and comes back brings its trees back in the phase they had: a tree's seed is
// its place in the scene file, never the order the tiles came in, so a flight out of every ring
// and back draws, at the same world time, what a camera that never moved draws.
TEST_CASE("engine-view: a streamed tree that leaves and comes back sways in its own phase") {
  const test::TempDir tmp("engine_view_authored_return");
  REQUIRE(write_text(tmp.file("oak.flora.json"), k_oak));
  REQUIRE(write_text(tmp.file("spruce.flora.json"), k_spruce));
  REQUIRE(write_text(tmp.file("streamed.json"),
                     scene_text(R"({"tile_size":32,"rings":[{"radius":2},{"radius":30}]})")));
  REQUIRE(write_text(tmp.file("stay.json"),
                     R"({"format":"engine.camera-path.v1","name":"stay","fps":10,"keys":[)"
                     R"({"time":0,"position":[110,30,80],"target":[0,4,0]},)"
                     R"({"time":4,"position":[110,30,80],"target":[0,4,0]}]})"));
  // Out past the outer ring's 960 m, where every tile has gone, and back for the last two seconds.
  REQUIRE(write_text(tmp.file("away.json"),
                     R"({"format":"engine.camera-path.v1","name":"away","fps":10,"keys":[)"
                     R"({"time":0,"position":[110,30,80],"target":[0,4,0]},)"
                     R"({"time":1,"position":[2600,30,80],"target":[2500,4,0]},)"
                     R"({"time":2,"position":[110,30,80],"target":[0,4,0]},)"
                     R"({"time":4,"position":[110,30,80],"target":[0,4,0]}]})"));
  const auto fly = [&](const char* path, const char* png) {
    std::vector<std::string> args = base_args(tmp, "streamed.json", path, true);
    // Both rings swaying, so every one of the 140 trees leaves and comes back with a phase.
    for (const char* a : {"--tunable", "renderer.flora.sway_m=100", "--frames", "40", "--lod-fade",
                          "0", "--capture"})
      args.push_back(a);
    args.push_back(tmp.file(png));
    return view(args);
  };
  const Run stay = fly("stay.json", "stay.png");
  if (stay.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << stay.output);
    return;
  }
  REQUIRE_MESSAGE(stay.exit_code == 0, stay.output);
  const Run away = fly("away.json", "away.png");
  REQUIRE_MESSAGE(away.exit_code == 0, away.output);
  const JsonValue summary = summary_of(away.output);
  const JsonValue* flora = summary.find("flora");
  REQUIRE(flora != nullptr);
  REQUIRE(flora->is_object());
  CHECK(field_u64(*flora, "tail_instances") == 140);
  const u32 differing = differing_pixels(tmp.file("stay.png"), tmp.file("away.png"));
  MESSAGE("away and back against staying: " << differing << " pixels differ");
  CHECK(differing == 0);
}

// Roadmap R94 (renderer.md, "Static structures for rigid forms"): under the ray tracing chain a
// rigid distance form is drawn at its fixed cut — its frontier — and traced against structures
// built once, so the traced shadows of the raster picture and the ray-traced picture itself are
// the bytes the per-frame build of the same cut gives (`renderer.rt.static_build=false`), in two
// rings past the inner one; and the frame builds only what is not a form.
TEST_CASE("engine-view: a rigid form's structures built once trace the per-frame build's bytes") {
  const test::TempDir tmp("engine_view_rt_static_forms");
  REQUIRE(write_text(tmp.file("oak.flora.json"), k_oak));
  REQUIRE(write_text(tmp.file("spruce.flora.json"), k_spruce));
  REQUIRE(write_text(
      tmp.file("streamed.json"),
      scene_text(R"({"tile_size":16,"rings":[{"radius":1.5},{"radius":3},{"radius":6}]})")));
  // Low over the trees' edge, looking across them: the inner ring's full meshes, both outer
  // rings' forms, and the forms' traced shadows on the sand.
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"across","fps":10,"keys":[)"
                     R"({"time":0,"position":[-60,14,10],"target":[30,2,0]}]})"));
  const auto capture = [&](const char* png, const std::vector<const char*>& extra) {
    std::vector<std::string> args = base_args(tmp, "streamed.json", "path.json");
    args.erase(args.begin() + 9, args.begin() + 11);  // the base's --shadows off
    for (const char* a : {"--frames", "3", "--capture"})
      args.push_back(a);
    args.push_back(tmp.file(png));
    for (const char* a : extra)
      args.push_back(a);
    return view(args);
  };
  struct Case {
    const char* name;
    std::vector<const char*> mode;
  };
  const Case cases[] = {{"traced shadows", {"--shadows", "rt"}},
                        {"ray path", {"--raster", "rt", "--shadows", "rt"}}};
  for (const Case& c : cases) {
    std::vector<const char*> per_frame = c.mode;
    for (const char* a : {"--tunable", "renderer.rt.static_build=false"})
      per_frame.push_back(a);
    const Run built_once = capture("once.png", c.mode);
    if (built_once.exit_code == 3) {
      MESSAGE("engine-view unavailable here: " << built_once.output);
      return;
    }
    REQUIRE_MESSAGE(built_once.exit_code == 0, built_once.output);
    const Run every_frame = capture("every.png", per_frame);
    REQUIRE_MESSAGE(every_frame.exit_code == 0, every_frame.output);
    const JsonValue once_summary = summary_of(built_once.output);
    const JsonValue every_summary = summary_of(every_frame.output);
    const JsonValue* once_rt = once_summary.find("rt");
    const JsonValue* every_rt = every_summary.find("rt");
    REQUIRE(once_rt != nullptr);
    REQUIRE(every_rt != nullptr);
    const u32 differing = differing_pixels(tmp.file("once.png"), tmp.file("every.png"));
    MESSAGE(std::string(c.name) << ": " << field_u64(*once_rt, "static_forms") << " forms of "
                                << field_u64(*once_rt, "static_clusters")
                                << " clusters built once, "
                                << field_u64(*once_rt, "static_instances")
                                << " instances referencing them; " << differing
                                << " pixels differ from the per-frame build");
    CHECK(field_u64(*once_rt, "static_forms") > 0);
    CHECK(field_u64(*once_rt, "static_instances") > 0);
    CHECK(field_u64(*once_rt, "static_bytes") > 0);
    CHECK(field_u64(*every_rt, "static_instances") == 0);
    CHECK(differing == 0);
  }
}

// Roadmap R101 (renderer.md, "The resident cluster structure cache"): the inner ring's trees are
// rigid full meshes, so their clusters' structures are built once into the cache and referenced
// by every instance and frame after; with the static forms off, the forms are cut by distance and
// cached as well. Under traced shadows and under the ray path the picture is the bytes the
// per-frame build gives (`renderer.rt.cache=false`), and a streamed world's frames build into the
// cache only what they draw first.
TEST_CASE("engine-view: a streamed world's cached structures trace the per-frame build's bytes") {
  const test::TempDir tmp("engine_view_rt_cache");
  REQUIRE(write_text(tmp.file("oak.flora.json"), k_oak));
  REQUIRE(write_text(tmp.file("spruce.flora.json"), k_spruce));
  REQUIRE(write_text(
      tmp.file("streamed.json"),
      scene_text(R"({"tile_size":16,"rings":[{"radius":1.5},{"radius":3},{"radius":6}]})")));
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"across","fps":10,"keys":[)"
                     R"({"time":0,"position":[-60,14,10],"target":[30,2,0]}]})"));
  const auto capture = [&](const char* png, const std::vector<const char*>& extra) {
    std::vector<std::string> args = base_args(tmp, "streamed.json", "path.json");
    args.erase(args.begin() + 9, args.begin() + 11);  // the base's --shadows off
    for (const char* a : {"--frames", "3", "--capture"})
      args.push_back(a);
    args.push_back(tmp.file(png));
    for (const char* a : extra)
      args.push_back(a);
    return view(args);
  };
  struct Case {
    const char* name;
    std::vector<const char*> mode;
  };
  const Case cases[] = {{"traced shadows", {"--shadows", "rt"}},
                        {"ray path", {"--raster", "rt", "--shadows", "rt"}},
                        {"forms cut by distance",
                         {"--shadows", "rt", "--tunable", "renderer.rt.static_forms=false"}}};
  for (const Case& c : cases) {
    std::vector<const char*> per_frame = c.mode;
    for (const char* a : {"--tunable", "renderer.rt.cache=false"})
      per_frame.push_back(a);
    const Run cached = capture("cached.png", c.mode);
    if (cached.exit_code == 3) {
      MESSAGE("engine-view unavailable here: " << cached.output);
      return;
    }
    REQUIRE_MESSAGE(cached.exit_code == 0, cached.output);
    const Run every_frame = capture("every.png", per_frame);
    REQUIRE_MESSAGE(every_frame.exit_code == 0, every_frame.output);
    const JsonValue cached_summary = summary_of(cached.output);
    const JsonValue every_summary = summary_of(every_frame.output);
    const JsonValue* cached_rt = cached_summary.find("rt");
    const JsonValue* every_rt = every_summary.find("rt");
    REQUIRE(cached_rt != nullptr);
    REQUIRE(every_rt != nullptr);
    const u32 differing = differing_pixels(tmp.file("cached.png"), tmp.file("every.png"));
    MESSAGE(std::string(c.name) << ": the cache keeps " << field_u64(*cached_rt, "cache_clusters")
                                << " clusters of " << field_u64(*cached_rt, "cache_meshes")
                                << " meshes in " << field_u64(*cached_rt, "cache_slots")
                                << " slots, built " << field_u64(*cached_rt, "cache_built_total")
                                << " of them; " << differing
                                << " pixels differ from the per-frame build");
    CHECK(field_u64(*cached_rt, "cache_meshes") > 0);
    CHECK(field_u64(*cached_rt, "cache_built_total") > 0);
    CHECK(field_u64(*cached_rt, "cache_overflow") == 0);
    CHECK(field_u64(*every_rt, "cache_slots") == 0);
    CHECK(differing == 0);
  }
}

namespace {

// A denser grove than `scene_text`'s — 400 of each tree, a wider inner ring — so that a frame
// under traced shadows wants a few thousand structures, past what 16 MiB of them hold.
std::string dense_scene_text() {
  std::string text = scene_text(R"({"tile_size":16,"rings":[{"radius":4},{"radius":8}]})");
  for (usize at = text.find(R"("count":70)"); at != std::string::npos;
       at = text.find(R"("count":70)", at)) {
    text.replace(at, 10, R"("count":400)");
  }
  return text;
}

// The most shadow casters a frame of the run kept (the summary's percentiles; still frames).
u64 casters_max(const JsonValue& summary) {
  const JsonValue* casters = summary.find("shadow_casters");
  return casters != nullptr ? field_u64(*casters, "max") : 0u;
}

// Traced shadows over the dense grove at 640 x 360, `frames` frames into `bench` and `png`.
Run traced_dense(const test::TempDir& tmp, const char* bench, const char* png, u32 frames,
                 const std::vector<const char*>& extra) {
  std::vector<std::string> args = base_args(tmp, "dense.json", "path.json");
  args[6] = "640";
  args[8] = "360";
  args[10] = "rt";  // --shadows rt
  for (const std::string& a :
       {std::string("--frames"), std::to_string(frames), std::string("--benchmark"),
        tmp.file(bench), std::string("--capture"), tmp.file(png)})
    args.push_back(a);
  for (const char* a : extra)
    args.push_back(a);
  return view(args);
}

}  // namespace

// R94: the first capacity is what the scene could want — its pairs less the static forms', in
// every view — clamped to the budget, so a streamed world's first frame is drawn complete and no
// growth lands as its first tiles arrive. The floor is lowered to 256 (`renderer.rt.initial_
// clusters`), under which the old first capacity (the union or the floor) dropped instances over
// the first frames while the policy grew towards what they wanted.
TEST_CASE("engine-view: a streamed world's first frames under traced shadows are complete") {
  const test::TempDir tmp("engine_view_rt_first_capacity");
  REQUIRE(write_text(tmp.file("oak.flora.json"), k_oak));
  REQUIRE(write_text(tmp.file("spruce.flora.json"), k_spruce));
  REQUIRE(write_text(tmp.file("dense.json"), dense_scene_text()));
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"across","fps":10,"keys":[)"
                     R"({"time":0,"position":[-60,14,10],"target":[30,2,0]}]})"));
  // The frame's own builds: the grove is rigid, so with the cluster cache (renderer.md, "The
  // resident cluster structure cache") it would build none and have nothing to size.
  const Run run = traced_dense(
      tmp, "bench.jsonl", "first.png", 60,
      {"--tunable", "renderer.rt.initial_clusters=256", "--tunable", "renderer.rt.cache=false"});
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  const JsonValue summary = summary_of(run.output);
  const JsonValue* rt = summary.find("rt");
  REQUIRE(rt != nullptr);
  std::ifstream log(tmp.file("bench.jsonl"), std::ios::binary);
  std::string line;
  u32 frames = 0;
  u32 incomplete = 0;
  u64 wanted_max = 0;
  while (std::getline(log, line)) {
    JsonValue record;
    if (!parse_json(line, record).ok || record.find("frame") == nullptr) continue;
    ++frames;
    const u64 built = field_u64(record, "rt_built");
    const u64 wanted = field_u64(record, "rt_wanted");
    wanted_max = std::max(wanted_max, wanted);
    incomplete += built < wanted ? 1u : 0u;
  }
  MESSAGE(frames << " frames, " << incomplete << " incomplete, at most " << wanted_max
                 << " structures wanted, " << field_u64(*rt, "grows") << " growths, peak capacity "
                 << field_u64(*rt, "peak_capacity"));
  CHECK(frames >= 60);
  CHECK(wanted_max > 256);  // the floor alone would not have held the first frame
  CHECK(incomplete == 0);
  CHECK(field_u64(*rt, "overflow_frames") == 0);
  CHECK(field_u64(*rt, "grows") == 0);
}

TEST_CASE("engine-view: four rings with static forms and grass keep traced casters") {
  const test::TempDir tmp("engine_view_rt_four_rings");
  REQUIRE(write_text(tmp.file("oak.flora.json"), k_oak));
  REQUIRE(write_text(tmp.file("spruce.flora.json"), k_spruce));
  REQUIRE(write_text(tmp.file("sward.flora.json"), k_sward));
  std::string text = scene_text(
      R"({"tile_size":16,"rings":[{"radius":2},{"radius":4},{"radius":8},{"radius":12}]})");
  const usize meshes = text.find(R"("scatters")");
  REQUIRE(meshes != std::string::npos);
  const usize mesh_end = text.rfind(']', meshes);
  text.insert(mesh_end, R"(,{"name":"sward","path":"sward.flora.json"})");
  const usize scatter_end = text.find(R"(],"world")");
  REQUIRE(scatter_end != std::string::npos);
  text.insert(scatter_end,
              R"(,{"mesh":2,"center":[0,0,0],"radius_max":100,"count":800,"seed":21})");
  REQUIRE(write_text(tmp.file("dense.json"), text));
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"four","fps":10,"keys":[)"
                     R"({"time":0,"position":[-30,14,10],"target":[30,2,0]}]})"));
  const Run complete = traced_dense(tmp, "all.jsonl", "all.png", 12,
                                    {"--raster", "hw", "--tunable", "renderer.flora.motion=true"});
  if (complete.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << complete.output);
    return;
  }
  REQUIRE_MESSAGE(complete.exit_code == 0, complete.output);
  const JsonValue summary = summary_of(complete.output);
  const JsonValue* rt = summary.find("rt");
  const JsonValue* world = summary.find("world");
  REQUIRE(rt != nullptr);
  REQUIRE(world != nullptr);
  CHECK(casters_max(summary) > 0);
  CHECK(field_u64(*rt, "static_instances") > 0);
  CHECK(field_u64(*rt, "overflow_frames") == 0);
  CHECK(field_u64(*rt, "dropped_instances") == 0);
  CHECK(field_u64(*world, "refused") == 0);
  const JsonValue* withheld = world->find("ring_withheld");
  REQUIRE(withheld != nullptr);
  REQUIRE(withheld->as_array().size() == 4);
  for (u32 i = 0; i < withheld->as_array().size(); ++i) {
    u64 count = 1;
    REQUIRE(withheld->as_array()[i].get_u64(count));
    CHECK(count == 0);
  }
  const Run small = traced_dense(
      tmp, "small.jsonl", "small.png", 12,
      {"--raster", "hw", "--rt-budget-mib", "1", "--tunable", "renderer.flora.motion=true"});
  REQUIRE_MESSAGE(small.exit_code == 0, small.output);
  const JsonValue limited = summary_of(small.output);
  const JsonValue* limited_rt = limited.find("rt");
  REQUIRE(limited_rt != nullptr);
  CHECK(field_u64(*limited_rt, "overflow_frames") > 0);
  CHECK(field_u64(*limited_rt, "dropped_instances") > 0);
  CHECK(field_u64(*limited.find("world"), "refused") == 0);
}

// R94: past the budget a frame keeps the nearest ring first and, within one ring, drawn clusters
// before casters (`gfx::ClusterRecordParams::drop_order`), and which structures it drops is a
// function of the scene and the camera — two runs draw the same traced shadows.
TEST_CASE("engine-view: past the budget the chain drops the same structures every run") {
  const test::TempDir tmp("engine_view_rt_drop_order");
  REQUIRE(write_text(tmp.file("oak.flora.json"), k_oak));
  REQUIRE(write_text(tmp.file("spruce.flora.json"), k_spruce));
  REQUIRE(write_text(tmp.file("dense.json"), dense_scene_text()));
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"across","fps":10,"keys":[)"
                     R"({"time":0,"position":[-60,14,10],"target":[30,2,0]}]})"));
  // The frame's own builds past their budget: without the cluster cache, which would build none.
  const Run a = traced_dense(tmp, "a.jsonl", "a.png", 3,
                             {"--rt-budget-mib", "16", "--tunable", "renderer.rt.cache=false"});
  if (a.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << a.output);
    return;
  }
  REQUIRE_MESSAGE(a.exit_code == 0, a.output);
  const Run b = traced_dense(tmp, "b.jsonl", "b.png", 3,
                             {"--rt-budget-mib", "16", "--tunable", "renderer.rt.cache=false"});
  REQUIRE_MESSAGE(b.exit_code == 0, b.output);
  for (const Run* run : {&a, &b}) {
    const JsonValue summary = summary_of(run->output);
    const JsonValue* rt = summary.find("rt");
    REQUIRE(rt != nullptr);
    MESSAGE(field_u64(*rt, "overflow_frames")
            << " frames past " << field_u64(*rt, "limit") << " structures, "
            << field_u64(*rt, "dropped_caster_instances") << " casters' instances dropped");
    CHECK(field_u64(*rt, "overflow_frames") > 0);
  }
  CHECK(differing_pixels(tmp.file("a.png"), tmp.file("b.png")) == 0);
}

// R94: the caster horizon. With `--shadow-distance` and `renderer.rt.caster_horizon`, a shadow
// caster farther from the eye than the distance is not built and is counted; one nearer is built
// as before, so the casters kept and the casters past it are the casters without it.
TEST_CASE("engine-view: a shadow caster past the caster horizon casts nothing and is counted") {
  const test::TempDir tmp("engine_view_rt_caster_horizon");
  REQUIRE(write_text(tmp.file("oak.flora.json"), k_oak));
  REQUIRE(write_text(tmp.file("spruce.flora.json"), k_spruce));
  REQUIRE(write_text(tmp.file("dense.json"), dense_scene_text()));
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"across","fps":10,"keys":[)"
                     R"({"time":0,"position":[-60,14,10],"target":[30,2,0]}]})"));
  const Run all = traced_dense(tmp, "all.jsonl", "all.png", 3, {"--shadow-distance", "30"});
  if (all.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << all.output);
    return;
  }
  REQUIRE_MESSAGE(all.exit_code == 0, all.output);
  const Run near =
      traced_dense(tmp, "near.jsonl", "near.png", 3,
                   {"--shadow-distance", "30", "--tunable", "renderer.rt.caster_horizon=true"});
  REQUIRE_MESSAGE(near.exit_code == 0, near.output);

  const JsonValue all_summary = summary_of(all.output);
  const JsonValue near_summary = summary_of(near.output);

  const JsonValue* all_rt = all_summary.find("rt");
  const JsonValue* near_rt = near_summary.find("rt");

  REQUIRE(all_rt != nullptr);
  REQUIRE(near_rt != nullptr);

  const u64 casters_all = casters_max(all_summary);
  const u64 casters_near = casters_max(near_summary);
  const u32 changed = differing_pixels(tmp.file("all.png"), tmp.file("near.png"));

  MESSAGE("casters " << casters_all << " without the horizon, " << casters_near << " within 30 m ("
                     << field_u64(*near_rt, "horizon_casters") << " past it); " << changed
                     << " pixels of the traced shadows change");
  CHECK(field_u64(*all_rt, "horizon_casters") == 0);
  CHECK(field_u64(*near_rt, "horizon_casters") > 0);
  CHECK(casters_near < casters_all);
  CHECK(casters_near + field_u64(*near_rt, "horizon_casters") == casters_all);
}

#endif

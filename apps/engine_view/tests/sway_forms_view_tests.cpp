// End to end: a swaying mesh's form by ring (G204; renderer.md, "A swaying mesh's form by ring").
// A streamed scatter of ferns — plants the flora stage sways, placed as one resident mesh in every
// ring — drawn as a swaying cut in the ring where their motion is seen and a still cut past it:
// fewer pairs than the same flight with the rule off, and no pixel changed by a ring's hand-over,
// neither between two forms at rest (the cuts are the mesh's own choice of clusters, the still
// form padded as the mesh is) nor between two that sway (a cut sways in its source's phase).
// Compiled with the flora, scatter and world capabilities; skips (exit 3) on a machine with no
// Vulkan device.
#include "view_process.h"

#include <core/json/json.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <fstream>
#include <string>
#include <vector>

using namespace engine;

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

// How many of the summary's meshes carry `text` in their names (a form names what it is).
u32 meshes_named(const JsonValue& summary, std::string_view text) {
  const JsonValue* meshes = summary.find("meshes");
  if (meshes == nullptr) return 0;
  u32 count = 0;
  for (u32 i = 0; i < meshes->size(); ++i) {
    const JsonValue* name = (*meshes)[i].find("name");
    std::string_view s;
    if (name != nullptr && name->get_string(s) && s.find(text) != std::string_view::npos) ++count;
  }
  return count;
}

// What a flight's hand-overs changed: the records, the tiles' pixels they covered and the pixels
// that differed (coverage, depth or colour).
struct Handovers {
  u32 count = 0;
  u64 drawn = 0;
  u64 changed = 0;
};
Handovers read_handovers(const std::string& path) {
  Handovers out;
  std::ifstream log(path, std::ios::binary);
  std::string line;
  while (std::getline(log, line)) {
    JsonValue record;
    if (!parse_json(line, record).ok || record.find("coverage_changed") == nullptr) continue;
    ++out.count;
    out.changed += field_u64(record, "coverage_changed") + field_u64(record, "depth_changed") +
                   field_u64(record, "color_changed");
    out.drawn += field_u64(record, "tile_before");
  }
  return out;
}

// Creeping ferns (the engine's own recipe: 0.28 m fronds) over 16 x 16 tiles of 16 m, rings of
// 1.5, 3 and 6 tiles: the floors are 8.7 m and 33 m. At 320 x 180 a fern's reach in a mean wind is
// some pixels from the first and under one from the second.
constexpr const char* k_fern =
    R"({"fern":{"fronds":10,"droop":0.75,"rhizome_length_m":0.8,"stipe":0.28,)"
    R"("frond_length_m":0.28,"seed":29,"triangle_budget":4000,"crown_radius_m":0.06,)"
    R"("pinnae":16,"pinna_length":0.12,"spread_deg":45,"arching":true},"species":"fern"})";
constexpr const char* k_scene =
    R"({"format":"engine.scene.v1","name":"sway-forms-view",)"
    R"("terrain":{"size":65,"extent":200,"seed":7,"dune_height":0.3,"dune_wavelength":30,)"
    R"("generator":"Waves"},)"
    R"("world":{"tile_size":16,"rings":[{"radius":1.5},{"radius":3},{"radius":6}]},)"
    R"("placements":[{"generator":"scatter","params":{"name":"ferns","seed":22,"tile_size":16,)"
    R"("region":{"kind":"Polygon","polygon":[[-120,-120],[120,-120],[120,120],[-120,120]]},)"
    R"("density":{"kind":"Constant","per_m2":0.05},)"
    R"("kinds":[{"name":"fern","mesh":"fern.flora.json","weight":1.0,"spacing":1.0,)"
    R"("scale_min":0.9,"scale_max":1.1}],"budget":{"per_tile":40}}}]})";

std::vector<std::string> base_args(const test::TempDir& tmp, const char* tag) {
  return {"--scene", tmp.file("scene.json"), "--camera-path", tmp.file("path.json"), "--offscreen",
          "--width", "320", "--height", "180", "--lod-fade", "0", "--ddc", tmp.file("ddc"),
          "--frames", "40", "--benchmark", tmp.file(std::string("bench-") + tag + ".jsonl"),
          "--tunable", "renderer.flora.motion=true",
          // Every tile built in its event, so what a flight holds at its end is the scene's and
          // not the build job's pace.
          "--tunable", "world.placements.max_builds=0"};
}

void add(std::vector<std::string>& args, std::initializer_list<const char*> more) {
  for (const char* a : more)
    args.push_back(a);
}

}  // namespace

TEST_CASE(
    "engine-view: ferns handed to their swaying and still forms change no pixel and hold "
    "fewer pairs") {
  const test::TempDir tmp("engine_view_sway_forms");
  REQUIRE(write_text(tmp.file("fern.flora.json"), k_fern));
  REQUIRE(write_text(tmp.file("scene.json"), k_scene));
  // Low across the field from one side to its middle, so tiles cross every ring and the flight
  // ends with every ring full.
  REQUIRE(write_text(tmp.file("path.json"),
                     R"({"format":"engine.camera-path.v1","name":"across","fps":10,"keys":[)"
                     R"({"time":0,"position":[-120,3,4],"target":[0,0,0]},)"
                     R"({"time":4,"position":[0,3,-4],"target":[120,0,0]}]})"));

  // **At rest**: a calm flight, the rule measuring the ferns at a mean wind. Every hand-over —
  // the mesh to its swaying cut, a swaying cut to the still one and back — draws the rest pose on
  // both sides, so it changes no pixel.
  std::vector<std::string> calm = base_args(tmp, "calm");
  // (Two pixels of reach, so this small picture's outer ring stands still: at 320 x 180 a fern
  // a half larger than the recipe's is a little over one pixel of reach from its floor.)
  add(calm,
      {"--tunable", "renderer.flora.wind_strength=0", "--tunable", "renderer.forms.still_wind=1",
       "--tunable", "renderer.forms.still_px=2", "--world-handover"});
  calm.push_back(tmp.file("handover-calm.jsonl"));
  const Run rest = view(calm);
  if (rest.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << rest.output);
    return;
  }
  REQUIRE_MESSAGE(rest.exit_code == 0, rest.output);
  const JsonValue summary = summary_of(rest.output);
  // Both kinds of form were made: a cut that sways and one that stands still.
  CHECK(meshes_named(summary, "(ring form") > 0);
  CHECK(meshes_named(summary, "(still form") > 0);
  const Handovers at_rest = read_handovers(tmp.file("handover-calm.jsonl"));
  MESSAGE("at rest: " << at_rest.count << " handovers over " << at_rest.drawn
                      << " of their tiles' pixels, " << at_rest.changed << " changed");
  CHECK(at_rest.count > 0);
  CHECK(at_rest.drawn > 0);
  CHECK(at_rest.changed == 0);
  const JsonValue* with = summary.find("world");
  REQUIRE(with != nullptr);
  CHECK(field_u64(*with, "refused") == 0);

  // **In the wind, with nothing stilled** (`still_px` 0): every form past the mesh is a swaying
  // cut, posed in its source's phase, so a hand-over changes no pixel of a frame held in time.
  std::vector<std::string> windy = base_args(tmp, "windy");
  add(windy, {"--tunable", "renderer.flora.wind_strength=1", "--tunable",
              "renderer.forms.still_px=0", "--world-handover"});
  windy.push_back(tmp.file("handover-windy.jsonl"));
  const Run sways = view(windy);
  REQUIRE_MESSAGE(sways.exit_code == 0, sways.output);
  CHECK(meshes_named(summary_of(sways.output), "(still form") == 0);
  const Handovers swaying = read_handovers(tmp.file("handover-windy.jsonl"));
  MESSAGE("swaying: " << swaying.count << " handovers over " << swaying.drawn
                      << " of their tiles' pixels, " << swaying.changed << " changed");
  CHECK(swaying.count > 0);
  CHECK(swaying.changed == 0);

  // **The rule off**: every fern its whole mesh in every ring, as before.
  std::vector<std::string> off = base_args(tmp, "off");
  add(off, {"--tunable", "renderer.forms.sway=false"});
  const Run whole = view(off);
  REQUIRE_MESSAGE(whole.exit_code == 0, whole.output);
  const JsonValue summary_off = summary_of(whole.output);
  const JsonValue* without = summary_off.find("world");
  REQUIRE(without != nullptr);
  MESSAGE("tail pairs: " << field_u64(*with, "tail_pairs") << " with the forms, "
                         << field_u64(*without, "tail_pairs") << " without");
  CHECK(field_u64(*with, "tail_pairs") < field_u64(*without, "tail_pairs"));
}

// The frame loop allocates nothing in steady state, on the repository's three path scenes
// (AGENTS.md, "No allocations in the frame loop in steady state"; plan 11 §11.2; docs/subsystems/
// renderer.md, "The frame loop allocates nothing"). engine-view flies each scene's committed camera
// path offscreen with `--benchmark`, and every frame's record says what the frame loop allocated on
// the frame's thread — engine allocations (`mem::allocations_on_thread`) and device buffers
// (`gfx::buffers_created_on_thread`) over the host's turn, `begin_frame` and `submit_frame`, less
// what the host records for the benchmark (`renderer::FlightBookkeeping`).
//
// **Steady state is the second flight over the path.** The path is flown twice (`--repeat 2`), each
// time after `k_warmup` frames at its first camera, and a streamed world starts over at each. The
// first flight is the warm-up: the lists that grow with what a frame holds — the world's tile
// ring and its tiers, the terrain's staging records — reach the most the path needs in it, a
// growth at a time, and on the endless desert that is a few dozen allocations over its first
// frames (2026-10-07). **The second flight allocates nothing**, which is the rule: an allocation
// made every frame, or by a list that keeps growing, shows in it as it does in the first.
//
// **The scenes are the committed ones made small enough for a debug build**: the overlook's and the
// erg's ground at 257 samples over their own extents, the endless desert's world in three rings
// and two far levels, each path resampled to `k_frames` frames so the whole of it is flown in
// coarse steps (which moves more tiles a frame than the path's own 60 fps would), at 160x96, into a
// derived-data cache in the test's scratch directory. What they keep is what allocates: the
// overlook's meshes, palms and terrain with occlusion culling, the erg's rings round a walker, the
// endless desert's tiles and far squares made and let go, with the shadows each scene draws by
// default. The full-size flights are the frame budgets' (tools/frame-budget.ps1), whose summaries
// carry the same `frame_loop` block.
//
// **They are a CTest test of their own**, `engine_view.frame_loop`
// (apps/engine_view/CMakeLists.txt): the three flights take about 220 s in `msvc-debug` and 60 s in
// `msvc-release`, which inside `engine_view` would double its run and bring it near its timeout. So
// they are marked `doctest::skip()`, which the `engine_view` run honours, and
// `engine_view.frame_loop` runs the same executable with `--no-skip` and these cases alone.
#include "view_process.h"

#include <core/json/json.h>
#include <foundation/image/decode.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace engine;

// Everything below is for the cases at the end, and every one of them is behind
// ENGINE_VIEW_TESTS_DUNES: with the dunes off this file has no cases, and Clang would otherwise
// refuse the helpers as unused functions (-Werror), as linux-clang-minimal did on 2026-10-08.
#if ENGINE_VIEW_TESTS_DUNES
namespace {

constexpr u32 k_frames = 96;
constexpr u32 k_warmup = 12;
constexpr u32 k_repeats = 2;

struct Run {
  i32 exit_code = -1;
  std::string output;
};

Run view(const std::vector<std::string>& args) {
  std::vector<std::string_view> argv;
  const std::string exe = test::app_path(ENGINE_APP_PATH);
  argv.push_back(exe);
  for (const std::string& a : args)
    argv.push_back(a);
  Run run;
  run.exit_code = view_test::run_program(
      std::span<const std::string_view>(argv.data(), argv.size()), run.output);
  return run;
}

std::string committed_dir(const char* scene) {
  const std::string relative = std::string("content/test-scenes/") + scene;
  return test::data_path(std::string(ENGINE_SOURCE_DIR "/") + relative, relative);
}

bool read_json(const std::string& file, JsonValue& out) {
  std::string text;
  if (io::read_file(file, text) != io::Status::Ok) return false;
  return parse_json(text, out).ok;
}

std::string forward_slashes(std::string s) {
  for (char& c : s)
    if (c == '\\') c = '/';
  return s;
}

u64 number(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  u64 v = 0;
  return value != nullptr && value->get_u64(v) ? v : ~u64{0};
}

// What one flight's records said, frame by frame.
struct Flown {
  bool ran = false;
  u64 allocations = 0;
  u64 buffers = 0;
  u32 records = 0;
  // The first flight's, the warm-up: reported, not asserted.
  u64 first_allocations = 0;
  u64 first_buffers = 0;
  std::string first;             // the first frames that allocated, for the message
  std::string allocation_sites;  // debug child output, printed only when zero is violated
  std::string provider;          // the last record's `reconstruction.provider`
  JsonValue summary;
};

// Flies `scene` along `path` and reads the `.jsonl` back. False (and a MESSAGE) when this machine
// cannot draw it, which is a skip and not a failure.
Flown fly(const test::TempDir& tmp, const std::string& name, const std::string& scene,
          const std::string& path, const std::vector<std::string>& extra) {
  Flown out;
  const std::string jsonl = tmp.file(name + ".jsonl");
  std::vector<std::string> args = {"--scene",
                                   scene,
                                   "--camera-path",
                                   path,
                                   "--offscreen",
                                   "--benchmark",
                                   jsonl,
                                   "--frames",
                                   std::to_string(k_frames),
                                   "--repeat",
                                   std::to_string(k_repeats),
                                   "--warmup",
                                   std::to_string(k_warmup),
                                   "--warmup-seconds",
                                   "0",
                                   "--width",
                                   "160",
                                   "--height",
                                   "96",
                                   "--ddc",
                                   tmp.file("ddc")};
  args.insert(args.end(), extra.begin(), extra.end());
  const Run run = view(args);
  if (run.exit_code == 3) {
    MESSAGE(name << ": engine-view cannot draw it here: " << run.output);
    return out;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, name << ": " << run.output);
  std::string text;
  REQUIRE(io::read_file(jsonl, text) == io::Status::Ok);
  std::istringstream lines(text);
  u32 listed = 0;
  for (std::string line; std::getline(lines, line);) {
    if (line.empty()) continue;
    JsonValue value;
    REQUIRE_MESSAGE(parse_json(line, value).ok, line);
    if (value.find("format") != nullptr) {
      out.summary = std::move(value);
      continue;
    }
    ++out.records;
    if (const JsonValue* rec = value.find("reconstruction"); rec != nullptr) {
      if (const JsonValue* provider = rec->find("provider"); provider != nullptr)
        out.provider = provider->as_string();
    }
    const u64 a = number(value, "allocations");
    const u64 b = number(value, "buffers");
    REQUIRE_MESSAGE(a != ~u64{0}, "a record without `allocations`: " << line.substr(0, 200));
    REQUIRE(b != ~u64{0});
    if (number(value, "repeat") == 0) {
      out.first_allocations += a;
      out.first_buffers += b;
      continue;
    }
    out.allocations += a;
    out.buffers += b;
    if ((a > 0 || b > 0) && listed < 8) {
      out.first += (listed++ > 0 ? ", " : "") + std::string("frame ") +
                   std::to_string(number(value, "frame")) + ": " + std::to_string(a) +
                   " allocations, " + std::to_string(b) + " buffers";
    }
  }
  REQUIRE(out.records == k_frames * k_repeats);
  const JsonValue* loop = out.summary.find("frame_loop");
  REQUIRE_MESSAGE(loop != nullptr, "the summary has no frame_loop block");
  // The summary's block is the records' totals.
  CHECK(number(*loop, "allocations") == out.first_allocations + out.allocations);
  CHECK(number(*loop, "buffers") == out.first_buffers + out.buffers);
  const JsonValue* device = out.summary.find("device");
  REQUIRE(device != nullptr);
  CHECK_FALSE(device->as_string().empty());
  MESSAGE(name << " on " << device->as_string() << ", " << k_frames
               << " frames a flight: the first " << out.first_allocations << " allocations and "
               << out.first_buffers << " device buffers on the frame's thread, the second "
               << out.allocations << " and " << out.buffers
               << (out.first.empty() ? std::string() : " (" + out.first + ")"));
  if (out.allocations != 0) out.allocation_sites = run.output;
  out.ran = true;
  return out;
}

// The desert overlook made small, written into `tmp`; empty (and a MESSAGE) where it cannot be.
std::string overlook_scene(const test::TempDir& tmp) {
  const std::string dir = committed_dir("desert-overlook");
  JsonValue scene;
  if (!read_json(dir + "/scene.json", scene)) {
    MESSAGE("the desert overlook is not in this bundle");
    return {};
  }
  // Its meshes are the Khronos samples, named relative to the committed file: made absolute so
  // the copy in the scratch directory finds them, and the flight skipped where they were not
  // fetched (tools/fetch-samples.ps1).
  JsonValue* meshes = scene.find("meshes");
  REQUIRE(meshes != nullptr);
  for (usize m = 0; m < meshes->size(); ++m) {
    JsonValue& mesh = (*meshes)[m];
    const JsonValue* relative = mesh.find("path");
    if (relative == nullptr) continue;
    const std::string absolute = forward_slashes(
        std::filesystem::weakly_canonical(std::filesystem::path(dir) / relative->as_string())
            .string());
    if (!test::path_exists(absolute)) {
      MESSAGE("the desert overlook's samples are not fetched here (" << absolute << "); skipped");
      return {};
    }
    mesh.set("path", JsonValue(absolute));
  }
  JsonValue terrain = *scene.find("terrain");
  terrain.set("size", JsonValue(static_cast<u64>(257)));
  scene.set("terrain", std::move(terrain));
  scene.set("camera_path", JsonValue(forward_slashes(dir + "/camera-path.json")));
  const std::string file = tmp.file("overlook.json");
  REQUIRE(io::write_file(file, write_json(scene)) == io::Status::Ok);
  return file;
}

}  // namespace

TEST_CASE("frame loop: the desert overlook allocates nothing in its second flight" *
          doctest::skip()) {
  const test::TempDir tmp("engine_view_frame_loop_overlook");
  const std::string file = overlook_scene(tmp);
  if (file.empty()) return;
  // With `--report` (T212's review, M2): its long frames are classified at the end of an offscreen
  // run, and nothing of it may reach the frame (the window's per-frame classifier is held to zero
  // by frame_records_tests.cpp's count, since no window's record counts its allocations).
  const std::string report = tmp.file("report.json");
  const Flown flown =
      fly(tmp, "desert-overlook", file, committed_dir("desert-overlook") + "/camera-path.json",
          {"--report", report});
  if (!flown.ran) return;
  CHECK_MESSAGE(flown.allocations == 0, flown.first << '\n' << flown.allocation_sites);
  CHECK(flown.buffers == 0);
  std::string text;
  CHECK(io::read_file(report, text) == io::Status::Ok);
  CHECK(text.find("\"long_frames\"") != std::string::npos);
}

// Roadmap R158 (renderer.md, "Sampling over time: the accumulation"): with the engine's own
// accumulation on, its images are made at load and at a resize and nothing in a frame, and its
// inputs (the motion pass, last frame's buffers, the moved table) keep what they had. The same
// flight as above, accumulating.
TEST_CASE("frame loop: the desert overlook accumulating allocates nothing in its second flight" *
          doctest::skip()) {
  const test::TempDir tmp("engine_view_frame_loop_accumulation");
  const std::string file = overlook_scene(tmp);
  if (file.empty()) return;
  const Flown flown = fly(tmp, "desert-overlook accumulating", file,
                          committed_dir("desert-overlook") + "/camera-path.json",
                          {"--tunable", "renderer.reconstruction.accumulation=on"});
  if (!flown.ran) return;
  REQUIRE(flown.provider == "engine");  // the premise: these frames were accumulated
  CHECK_MESSAGE(flown.allocations == 0, flown.first << '\n' << flown.allocation_sites);
  CHECK(flown.buffers == 0);
}

// Roadmap S122 (world_sound.md, "The refresh off the frame's thread"): the same flight heard, the
// world's sound on the null backend in a hand wind, its refresh gathered on the frame's thread,
// computed on `page_jobs`' Efficiency pool and applied at its tick. The frame's thread allocates
// nothing in the second flight with the sound on.
TEST_CASE("frame loop: the desert overlook heard allocates nothing in its second flight" *
          doctest::skip()) {
  const test::TempDir tmp("engine_view_frame_loop_heard");
  const std::string file = overlook_scene(tmp);
  if (file.empty()) return;
  const Flown flown = fly(tmp, "desert-overlook heard", file,
                          committed_dir("desert-overlook") + "/camera-path.json",
                          {"--audio", "null", "--tunable", "renderer.wind.speed_m_s=6"});
  if (!flown.ran) return;
  CHECK_MESSAGE(flown.allocations == 0, flown.first << '\n' << flown.allocation_sites);
  CHECK(flown.buffers == 0);
}

TEST_CASE("frame loop: the erg's walk on its rings allocates nothing in its second pass" *
          doctest::skip()) {
  const test::TempDir tmp("engine_view_frame_loop_erg");
  const std::string dir = committed_dir("desert-erg");
  JsonValue scene;
  if (!read_json(dir + "/scene.json", scene)) {
    MESSAGE("the erg is not in this bundle");
    return;
  }
  JsonValue terrain = *scene.find("terrain");
  terrain.set("size", JsonValue(static_cast<u64>(257)));
  scene.set("terrain", std::move(terrain));
  scene.set("camera_path", JsonValue(forward_slashes(dir + "/walk-path.json")));
  const std::string file = tmp.file("erg.json");
  REQUIRE(io::write_file(file, write_json(scene)) == io::Status::Ok);
  // The walk is the erg's path scene (its README's benchmark line), on the rings round the walker.
  const Flown flown =
      fly(tmp, "desert-erg walk", file, dir + "/walk-path.json", {"--terrain-rings"});
  if (!flown.ran) return;
  CHECK_MESSAGE(flown.allocations == 0, flown.first << '\n' << flown.allocation_sites);
  CHECK(flown.buffers == 0);
}

#if ENGINE_VIEW_WORLD
TEST_CASE(
    "frame loop: the endless desert's flight over its tiles allocates nothing in its "
    "second flight" *
    doctest::skip()) {
  const test::TempDir tmp("engine_view_frame_loop_endless");
  const std::string dir = committed_dir("desert-endless");
  JsonValue scene;
  if (!read_json(dir + "/scene.json", scene)) {
    MESSAGE("the endless desert is not in this bundle");
    return;
  }
  // Its world's rings brought in to six tiles (as render_tests.cpp's flight does), so a debug
  // build lays a ring out in seconds; the tiles, their cells, the bands, the sky and the time are
  // the scene's own. Two far levels past them (`--terrain-far 2`) instead of six.
  JsonValue world = *scene.find("world");
  JsonValue rings = JsonValue::array();
  const f64 radii[3] = {1.5, 3.0, 6.0};
  const u64 cells[3] = {32, 16, 8};
  for (u32 k = 0; k < 3; ++k) {
    JsonValue ring = JsonValue::object();
    ring.set("radius", JsonValue(radii[k]));
    ring.set("ground_cells", JsonValue(cells[k]));
    rings.push_back(std::move(ring));
  }
  world.set("rings", std::move(rings));
  scene.set("world", std::move(world));
  scene.set("camera_path", JsonValue(forward_slashes(dir + "/camera-path.json")));
  const std::string file = tmp.file("endless.json");
  REQUIRE(io::write_file(file, write_json(scene)) == io::Status::Ok);
  const Flown flown =
      fly(tmp, "desert-endless", file, dir + "/camera-path.json", {"--terrain-far", "2"});
  if (!flown.ran) return;
  CHECK_MESSAGE(flown.allocations == 0, flown.first << '\n' << flown.allocation_sites);
  CHECK(flown.buffers == 0);
}

namespace {

// What a standing camera's frames did (roadmap W149; world.md, "A camera that stands").
struct Stood {
  bool ran = false;
  u32 records = 0;
  // The first recorded frame from which every frame did nothing (~0u: some frame after it did).
  u32 quiet_from = ~0u;
  // Summed over the frames from `quiet_from` on — every one of them zero when it is set — and,
  // apart, over the frames before it: the settling, reported.
  u64 tiles_built = 0, slots_written = 0, chunks_built = 0, chunk_bytes = 0, page_bytes = 0;
  u64 allocations = 0, buffers = 0, events = 0;
  u64 settling_work = 0;
  // The premise: the world filled in the warm-up, and every record says what its ground and its
  // world did (a record with neither would make every count above zero for nothing).
  u64 fill_events = 0;
  u32 with_ground = 0;
  u32 with_world = 0;
  std::string capture;  // the last frame's picture
  std::string busy;     // the frames that did something, for the message
};

// A count a record or a log line may leave out (a frame with no world, no terrain): zero then.
u64 count(const JsonValue& object, const char* key) {
  const u64 v = number(object, key);
  return v == ~u64{0} ? 0 : v;
}
u64 field(const JsonValue& object, const char* parent, const char* key) {
  const JsonValue* p = object.find(parent);
  return p == nullptr || p->is_null() ? 0 : count(*p, key);
}

// Stands `frames` frames at the one camera of `path` (after `k_warmup` at it, where the world
// fills) with the world clock running and the ground's time stopped, as the forest's staging is
// loaded, and reads every frame's record and the world log's lines back.
Stood stand(const test::TempDir& tmp, const std::string& name, const std::string& scene,
            const std::string& path, u32 frames, const std::vector<std::string>& extra) {
  Stood out;
  const std::string jsonl = tmp.file(name + ".jsonl");
  const std::string world_log = tmp.file(name + "-world.jsonl");
  out.capture = tmp.file(name + ".png");
  std::vector<std::string> args = {"--scene",
                                   scene,
                                   "--camera-path",
                                   path,
                                   "--offscreen",
                                   "--benchmark",
                                   jsonl,
                                   "--world-log",
                                   world_log,
                                   "--capture",
                                   out.capture,
                                   "--frames",
                                   std::to_string(frames),
                                   "--repeat",
                                   "1",
                                   "--warmup",
                                   std::to_string(k_warmup),
                                   "--warmup-seconds",
                                   "0",
                                   "--width",
                                   "160",
                                   "--height",
                                   "96",
                                   "--time-rate",
                                   "0",
                                   "--world-rate",
                                   "1",
                                   "--terrain-async",
                                   "--ddc",
                                   tmp.file("ddc")};
  args.insert(args.end(), extra.begin(), extra.end());
  const Run run = view(args);
  if (run.exit_code == 3) {
    MESSAGE(name << ": engine-view cannot draw it here: " << run.output);
    return out;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, name << ": " << run.output);
  // Each recorded frame's work, by frame: the records' and the world log's.
  std::vector<u64> work(frames, 0);
  std::vector<std::string> what(frames);
  std::string text;
  REQUIRE(io::read_file(world_log, text) == io::Status::Ok);
  {
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
      JsonValue value;
      if (line.empty() || !parse_json(line, value).ok) continue;
      const JsonValue* recorded = value.find("recorded");
      const u64 f = count(value, "frame");
      const u64 events =
          count(value, "activated") + count(value, "changed") + count(value, "deactivated");
      if (recorded == nullptr || !recorded->as_bool()) {
        out.fill_events += events;  // the warm-up's: the world filling round the camera
        continue;
      }
      if (f < frames && events > 0) {
        work[f] += events;
        what[f] += " " + std::to_string(events) + " tile events";
      }
    }
  }
  REQUIRE(io::read_file(jsonl, text) == io::Status::Ok);
  std::vector<JsonValue> records;
  {
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
      if (line.empty()) continue;
      JsonValue value;
      REQUIRE_MESSAGE(parse_json(line, value).ok, line);
      if (value.find("format") != nullptr) continue;
      records.push_back(std::move(value));
    }
  }
  out.records = static_cast<u32>(records.size());
  REQUIRE(out.records == frames);
  for (u32 r = 0; r < out.records; ++r) {
    const JsonValue& v = records[r];
    const u64 f = number(v, "frame");
    REQUIRE(f < frames);
    const JsonValue* ground = v.find("terrain");
    const JsonValue* world = v.find("world");
    out.with_ground += ground != nullptr && ground->is_object() ? 1u : 0u;
    out.with_world += world != nullptr && world->is_object() ? 1u : 0u;
    const u64 parts[] = {field(v, "world", "tiles_built"),
                         field(v, "world", "slots_written"),
                         field(v, "terrain", "chunks_built"),
                         field(v, "terrain", "chunk_upload_bytes"),
                         count(v, "upload_bytes"),
                         count(v, "allocations"),
                         count(v, "buffers")};
    static const char* const names[] = {"tiles built", "slots written", "chunks built",
                                        "chunk bytes", "page bytes",    "allocations",
                                        "buffers"};
    for (u32 k = 0; k < 7; ++k) {
      if (parts[k] == 0) continue;
      work[f] += parts[k];
      what[f] += " " + std::to_string(parts[k]) + " " + names[k];
    }
  }
  // Quiet from the first frame after the last that did anything.
  u32 last_busy = ~0u;
  for (u32 f = 0; f < frames; ++f)
    if (work[f] > 0) last_busy = f;
  out.quiet_from = last_busy == ~0u ? 0u : last_busy + 1;
  for (u32 f = 0; f < frames; ++f) {
    if (work[f] == 0) continue;
    out.settling_work += work[f];
    if (out.busy.size() < 600) out.busy += "frame " + std::to_string(f) + ":" + what[f] + "; ";
  }
  for (u32 r = 0; r < out.records; ++r) {
    const JsonValue& v = records[r];
    if (number(v, "frame") < out.quiet_from) continue;
    out.tiles_built += field(v, "world", "tiles_built");
    out.slots_written += field(v, "world", "slots_written");
    out.chunks_built += field(v, "terrain", "chunks_built");
    out.chunk_bytes += field(v, "terrain", "chunk_upload_bytes");
    out.page_bytes += count(v, "upload_bytes");
    out.allocations += count(v, "allocations");
    out.buffers += count(v, "buffers");
  }
  MESSAGE(name << ": " << frames << " frames standing after " << k_warmup
               << " at the camera (the warm-up's fill: " << out.fill_events
               << " tile events); quiet from frame " << out.quiet_from
               << (out.busy.empty() ? std::string(" (every one)") : " after: " + out.busy));
  out.ran = true;
  return out;
}

bool same_picture(const std::string& a, const std::string& b, u64& differing) {
  image::Image x;
  image::Image y;
  if (image::read_image(a, x) != io::Status::Ok || image::read_image(b, y) != io::Status::Ok)
    return false;
  if (x.width != y.width || x.height != y.height || x.pixels.size() != y.pixels.size())
    return false;
  differing = 0;
  for (u32 i = 0; i + 3 < x.pixels.size(); i += 4)
    differing += std::memcmp(&x.pixels[i], &y.pixels[i], 4) != 0 ? 1u : 0u;
  return true;
}

}  // namespace

// Roadmap W149 (world.md, "A camera that stands"): the endless desert's world tiles and two far
// levels, the camera standing where its path starts, the world clock running and the sand's time
// stopped (`--time-rate 0 --world-rate 1 --terrain-async`, as the forest's staging is loaded).
// Within the warm-up the place settles; from then on no frame activates, moves or deactivates a
// tile, builds a placements tile or writes a table slot, builds or uploads a ground chunk, uploads
// a geometry page, or allocates — and the picture at the end of the flight is the picture of its
// first frame, byte for byte. A one-frame flight is that first frame.
TEST_CASE("frame loop: the endless desert standing still builds, uploads and allocates nothing" *
          doctest::skip()) {
  const test::TempDir tmp("engine_view_frame_loop_standing");
  const std::string dir = committed_dir("desert-endless");
  JsonValue scene;
  if (!read_json(dir + "/scene.json", scene)) {
    MESSAGE("the endless desert is not in this bundle");
    return;
  }
  JsonValue world = *scene.find("world");
  JsonValue rings = JsonValue::array();
  const f64 radii[3] = {1.5, 3.0, 6.0};
  const u64 cells[3] = {32, 16, 8};
  for (u32 k = 0; k < 3; ++k) {
    JsonValue ring = JsonValue::object();
    ring.set("radius", JsonValue(radii[k]));
    ring.set("ground_cells", JsonValue(cells[k]));
    rings.push_back(std::move(ring));
  }
  world.set("rings", std::move(rings));
  scene.set("world", std::move(world));
  // Standing where the committed path starts, looking where it looks.
  const std::string path =
      R"({"format":"engine.camera-path.v1","name":"standing","fps":60,"znear":0.1,"keys":[)"
      R"({"time":0,"position":[-2000,90,0],"ground":true,"target":[-1300,0,0],"target_ground":true},)"
      R"({"time":10,"position":[-2000,90,0],"ground":true,"target":[-1300,0,0],"target_ground":true}]})";
  REQUIRE(io::write_file(tmp.file("standing.json"), path) == io::Status::Ok);
  scene.set("camera_path", JsonValue(forward_slashes(tmp.file("standing.json"))));
  const std::string file = tmp.file("endless.json");
  REQUIRE(io::write_file(file, write_json(scene)) == io::Status::Ok);
  const std::vector<std::string> far = {"--terrain-far", "2"};
  const Stood first = stand(tmp, "first", file, tmp.file("standing.json"), 1, far);
  if (!first.ran) return;
  const Stood stood = stand(tmp, "standing", file, tmp.file("standing.json"), 240, far);
  if (!stood.ran) return;
  // The premise: the world filled round the camera, and every frame reported its ground and world.
  CHECK(stood.fill_events > 0);
  CHECK(stood.with_ground == stood.records);
  CHECK(stood.with_world == stood.records);
  // Settled by the end of the warm-up, or within its first frames: a frame that waits for its
  // layout (offscreen) leaves the async ground nothing to finish later.
  CHECK(stood.quiet_from <= 30);
  CHECK(stood.tiles_built == 0);
  CHECK(stood.slots_written == 0);
  CHECK(stood.chunks_built == 0);
  CHECK(stood.chunk_bytes == 0);
  CHECK(stood.page_bytes == 0);
  CHECK(stood.allocations == 0);
  CHECK(stood.buffers == 0);
  u64 differing = 0;
  REQUIRE(same_picture(first.capture, stood.capture, differing));
  CHECK_MESSAGE(differing == 0, differing << " pixels differ between the first frame standing "
                                          << "and the 240th");
}

#if ENGINE_VIEW_TESTS_FLORA
// Roadmap W74 (renderer.md, "Streamed trees sway"): a streamed world's own trees sway with their
// tiles, each taking a record of its mesh's free list when its tile arrives and giving it back when
// it leaves. The first flight makes the records a mesh needs at most; the second (`--repeat`
// starts the world over, so every tile arrives and leaves again) reuses them. Tile changes are
// steady state: both the rigid and the swaying flight must allocate nothing.
TEST_CASE("frame loop: a streamed world's swaying trees allocate nothing in their second flight" *
          doctest::skip()) {
  const test::TempDir tmp("engine_view_frame_loop_trees");
  // authored_view_tests.cpp's trees, small enough for a debug build, in two rings of 32 m tiles.
  const std::string oak =
      R"({"species":"broadleaf","broadleaf":{"seed":3,"height_m":8,"trunk_radius_m":0.25,)"
      R"("triangle_budget":8000,"card_bake_px":16,"impostor_px":32}})";
  const std::string spruce =
      R"({"species":"conifer","conifer":{"seed":5,"height_m":6,"trunk_radius_m":0.1,)"
      R"("whorls":6,"branches_per_whorl":4,"crown_radius_m":1.5,"triangle_budget":8000,)"
      R"("card_bake_px":0,"impostor_px":0}})";  // no chain (G120): the decimated top
  REQUIRE(io::write_file(tmp.file("oak.flora.json"), oak) == io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("spruce.flora.json"), spruce) == io::Status::Ok);
  const std::string scene =
      R"({"format":"engine.scene.v1","name":"frame-loop-trees",)"
      R"("terrain":{"size":65,"extent":400,"seed":7,"dune_height":0.3,"dune_wavelength":60,)"
      R"("generator":"Waves"},)"
      R"("meshes":[{"name":"oak","path":"oak.flora.json"},)"
      R"({"name":"spruce","path":"spruce.flora.json"}],)"
      R"("scatters":[{"mesh":0,"center":[0,0,0],"radius_min":2,"radius_max":120,"count":70,)"
      R"("seed":11,"scale_min":0.8,"scale_max":1.2},)"
      R"({"mesh":1,"center":[0,0,0],"radius_min":2,"radius_max":120,"count":70,"seed":12}],)"
      R"("world":{"tile_size":32,"rings":[{"radius":1.5},{"radius":3}]}})";
  REQUIRE(io::write_file(tmp.file("trees.json"), scene) == io::Status::Ok);
  // Low across the grove, so tiles cross both rings both ways.
  const std::string path = R"({"format":"engine.camera-path.v1","name":"across","fps":60,"keys":[)"
                           R"({"time":0,"position":[-220,14,10],"target":[0,4,0]},)"
                           R"({"time":1.6,"position":[220,14,-10],"target":[400,4,0]}]})";
  REQUIRE(io::write_file(tmp.file("path.json"), path) == io::Status::Ok);
  const Flown rigid = fly(tmp, "streamed-trees-rigid", tmp.file("trees.json"),
                          tmp.file("path.json"), {"--tunable", "renderer.flora.tail=false"});
  if (!rigid.ran) return;
  const Flown flown = fly(tmp, "streamed-trees", tmp.file("trees.json"), tmp.file("path.json"), {});
  if (!flown.ran) return;
  MESSAGE("second flight, swaying: " << flown.allocations << " allocations, " << flown.buffers
                                     << " buffers; rigid: " << rigid.allocations << ", "
                                     << rigid.buffers);
  CHECK_MESSAGE(rigid.allocations == 0, rigid.first << '\n' << rigid.allocation_sites);
  CHECK(rigid.buffers == 0);
  CHECK_MESSAGE(flown.allocations == 0, flown.first << '\n' << flown.allocation_sites);
  CHECK(flown.buffers == 0);
  // W154-2: in a storm (force 12, the hand wind) the first frame steps the trees' padding ladder to
  // its top and every tile that comes after is placed at that rung, its still forms leaned; the
  // second flight's tiles take their records, their grass and their still slots from lists the
  // first flight grew, and allocate nothing.
  const Flown storm = fly(tmp, "streamed-trees-storm", tmp.file("trees.json"),
                          tmp.file("path.json"), {"--tunable", "renderer.wind.speed_m_s=34.8"});
  if (!storm.ran) return;
  MESSAGE("second flight at force 12: " << storm.allocations << " allocations, " << storm.buffers
                                        << " buffers");
  CHECK_MESSAGE(storm.allocations == 0, storm.first << '\n' << storm.allocation_sites);
  CHECK(storm.buffers == 0);
}

// Roadmap W149, with the forest's other inputs: a streamed world's own trees, swaying on the world
// clock, in two rings round a camera that stands among them. The sway moves the picture every frame
// (so no picture is compared); nothing it reads reaches a tile's key, its placements or its ground,
// so after the warm-up no tile is touched, built or uploaded and nothing is allocated.
TEST_CASE("frame loop: a streamed world's swaying trees round a standing camera build nothing" *
          doctest::skip()) {
  const test::TempDir tmp("engine_view_frame_loop_standing_trees");
  const std::string oak =
      R"({"species":"broadleaf","broadleaf":{"seed":3,"height_m":8,"trunk_radius_m":0.25,)"
      R"("triangle_budget":8000,"card_bake_px":16,"impostor_px":32}})";
  REQUIRE(io::write_file(tmp.file("oak.flora.json"), oak) == io::Status::Ok);
  const std::string scene =
      R"({"format":"engine.scene.v1","name":"frame-loop-standing-trees",)"
      R"("terrain":{"size":65,"extent":400,"seed":7,"dune_height":0.3,"dune_wavelength":60,)"
      R"("generator":"Waves"},)"
      R"("meshes":[{"name":"oak","path":"oak.flora.json"}],)"
      R"("scatters":[{"mesh":0,"center":[0,0,0],"radius_min":2,"radius_max":120,"count":90,)"
      R"("seed":11,"scale_min":0.8,"scale_max":1.2}],)"
      R"("world":{"tile_size":32,"rings":[{"radius":1.5},{"radius":3}]}})";
  REQUIRE(io::write_file(tmp.file("trees.json"), scene) == io::Status::Ok);
  // Standing at the grove's edge, a tile boundary in reach of the second ring's band.
  const std::string path =
      R"({"format":"engine.camera-path.v1","name":"standing","fps":60,"keys":[)"
      R"({"time":0,"position":[-40,6,10],"target":[0,4,0]},)"
      R"({"time":10,"position":[-40,6,10],"target":[0,4,0]}]})";
  REQUIRE(io::write_file(tmp.file("path.json"), path) == io::Status::Ok);
  const Stood stood = stand(tmp, "standing-trees", tmp.file("trees.json"), tmp.file("path.json"),
                            240, {"--tunable", "renderer.flora.motion=true"});
  if (!stood.ran) return;
  CHECK(stood.fill_events > 0);
  CHECK(stood.with_world == stood.records);
  CHECK(stood.quiet_from <= 30);
  CHECK(stood.tiles_built == 0);
  CHECK(stood.slots_written == 0);
  CHECK(stood.chunks_built == 0);
  CHECK(stood.chunk_bytes == 0);
  CHECK(stood.page_bytes == 0);
  CHECK(stood.allocations == 0);
  CHECK(stood.buffers == 0);
}
#endif  // ENGINE_VIEW_TESTS_FLORA
#endif  // ENGINE_VIEW_WORLD

#endif  // ENGINE_VIEW_TESTS_DUNES

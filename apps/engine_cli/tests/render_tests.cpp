// End to end: the `render.*` methods over the protocol, which is the Phase 1 exit criterion in
// docs/plan/10-roadmap-risks.md — "agents can capture and benchmark". One `engine-host --stdio`
// process carries the whole round trip, because a scene id lives in the host and `engine-cli`
// spawns a host per invocation; the stateless method, `render.compare`, is driven through
// engine-cli as well, so both paths are covered.
//
// The scene is the procedural heightfield, so the case needs no fixture, no sample asset, and no
// derived-data cache entry. On a machine with no GPU — which is every hosted CI runner — the
// first call comes back with error 1007 and the case records a skip, exactly as the engine-view
// suite skips on exit 3. That is what the distinct code is for.
#include <core/json/json.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;

namespace {

constexpr i32 k_render_unavailable = 1007;
constexpr i32 k_not_found = 1003;
constexpr i32 k_invalid_argument = 1004;

// One engine-host process fed a series of requests, so the scene a `render.load` leaves behind
// is still there for the calls after it.
// The built executables, or the bundle's copies of them (tests/support/test_paths.h).
const std::string& cli_exe() {
  static const std::string path = test::app_path(ENGINE_APP_PATH);
  return path;
}
const std::string& host_exe() {
  static const std::string path = test::app_path(ENGINE_HOST_PATH);
  return path;
}

struct Host {
  platform::Process process;
  u32 next_id = 1;
  bool ok = false;

  Host() {
    const std::string_view argv[2] = {host_exe(), "--stdio"};
    std::string error;
    if (!process.spawn(std::span<const std::string_view>(argv, 2), &error)) {
      FAIL("cannot spawn engine-host: " << error);
      return;
    }
    ok = true;
  }
  ~Host() {
    if (!ok) return;
    process.close_stdin();
    (void)process.wait();
  }

  // Sends one request and returns the parsed response object, which carries `result` or `error`.
  JsonValue call(std::string_view method, std::string_view params) {
    JsonValue response;
    std::string line = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(next_id++) +
                       ",\"method\":\"" + std::string(method) +
                       "\",\"params\":" + std::string(params) + "}\n";
    if (!process.write(line)) {
      FAIL("cannot write to engine-host");
      return response;
    }
    std::string text;
    if (!process.read_line(text)) {
      FAIL("engine-host closed the connection");
      return response;
    }
    REQUIRE_MESSAGE(parse_json(text, response).ok, text);
    return response;
  }
};

// The error code of a response, or 0 when it carries a result.
i32 error_code(const JsonValue& response) {
  const JsonValue* error = response.find("error");
  if (error == nullptr) return 0;
  const JsonValue* code = error->find("code");
  i64 value = -1;
  if (code == nullptr || !code->get_i64(value)) return -1;
  return static_cast<i32>(value);
}

std::string error_message(const JsonValue& response) {
  const JsonValue* error = response.find("error");
  if (error == nullptr) return {};
  const JsonValue* message = error->find("message");
  return message != nullptr ? std::string(message->as_string()) : std::string();
}

const JsonValue& result_of(const JsonValue& response) {
  const JsonValue* result = response.find("result");
  REQUIRE_MESSAGE(result != nullptr, error_message(response));
  return *result;
}

u64 number(const JsonValue& object, std::string_view key) {
  const JsonValue* value = object.find(key);
  u64 out = 0;
  return value != nullptr && value->get_u64(out) ? out : ~u64{0};
}

std::string text(const JsonValue& object, std::string_view key) {
  const JsonValue* value = object.find(key);
  std::string_view out;
  return value != nullptr && value->get_string(out) ? std::string(out) : std::string();
}

f64 real(const JsonValue& object, std::string_view key) {
  const JsonValue* value = object.find(key);
  f64 out = -1.0;
  return value != nullptr && value->get_f64(out) ? out : -1.0;
}

// A `file://` URI back to a native path, so the test can look at what the host wrote.
std::string path_of_uri(std::string_view uri) {
  std::string_view rest = uri;
  if (rest.starts_with("file:///")) {
    rest.remove_prefix(8);
    // A Windows URI is file:///C:/...; a POSIX one is file:///home/... and keeps its slash.
    if (rest.size() > 1 && rest[1] == ':') return std::string(rest);
    return "/" + std::string(rest);
  }
  if (rest.starts_with("file://")) rest.remove_prefix(7);
  return std::string(rest);
}

std::string uri_at(const JsonValue& files, const char* channel) {
  const JsonValue* value = files.find(channel);
  std::string_view out;
  return value != nullptr && value->get_string(out) ? path_of_uri(out) : std::string();
}

// The path `render.load` resolves `"raster":"hw"` to on the adapter the host opened, as that
// adapter's own report predicts it: the mesh path where the VK_EXT_mesh_shader row passes, the
// vertex-shader baseline tier where it does not (ADR-0024). Asked of `gpu.adapters` rather than
// assumed, because the assumption is what the first GPU run on a Pascal card broke: every machine
// this case had run on until then had mesh shaders, and the host was right to answer "vertex".
std::string expected_hw_raster(Host& host, const std::string& adapter) {
  const JsonValue response = host.call("gpu.adapters", "{}");
  const JsonValue* adapters = result_of(response).find("adapters");
  REQUIRE(adapters != nullptr);
  for (usize a = 0; a < adapters->size(); ++a) {
    const JsonValue& info = (*adapters)[a];
    if (text(info, "name") != adapter) continue;
    const JsonValue* rows = info.find("requirements");
    REQUIRE(rows != nullptr);
    for (usize r = 0; r < rows->size(); ++r) {
      const JsonValue& row = (*rows)[r];
      if (text(row, "name") != "VK_EXT_mesh_shader") continue;
      bool pass = false;
      const JsonValue* value = row.find("pass");
      REQUIRE((value != nullptr && value->get_bool(pass)));
      return pass ? "hw" : "vertex";
    }
    FAIL("gpu.adapters has no VK_EXT_mesh_shader row for " << adapter);
  }
  FAIL("the adapter render.load named, " << adapter << ", is not in gpu.adapters");
  return {};
}

}  // namespace

TEST_CASE("render: load, capture, benchmark, and compare over the protocol") {
  const test::TempDir tmp("engine_render_cli");
  const std::string out_dir = tmp.path();

  Host host;
  REQUIRE(host.ok);

  // The heightfield: no mesh file, no sample asset, no cache entry.
  const JsonValue loaded = host.call("render.load", R"({"grid":33,"settings":{"raster":"hw"}})");
  if (error_code(loaded) == k_render_unavailable) {
    MESSAGE("render.* unavailable here: " << error_message(loaded));
    return;
  }
  const JsonValue& scene = result_of(loaded);
  const std::string id = text(scene, "scene");
  CHECK(id == "scene1");
  CHECK(number(scene, "instances") == 1);
  CHECK(number(scene, "meshes") == 1);
  CHECK(number(scene, "triangles") == 32 * 32 * 2);
  CHECK(number(scene, "clusters") > 0);
  CHECK(number(scene, "pairs") == number(scene, "clusters"));
  CHECK(number(scene, "leaf_clusters") > 0);
  CHECK(text(scene, "mesh_cache") == "none");  // the heightfield is built, never cached
  CHECK(!text(scene, "adapter").empty());
  // What ran, not what was asked for: "hw" on a device with mesh shaders, "vertex" without.
  const std::string raster = expected_hw_raster(host, text(scene, "adapter"));
  MESSAGE("render.load resolved raster \"hw\" to \"" << text(scene, "raster") << "\" on "
                                                     << text(scene, "adapter"));
  CHECK(text(scene, "raster") == raster);
  CHECK(scene.find("center") != nullptr);
  CHECK(scene.find("radius") != nullptr);

  // Every channel at once. The capture writes five files: the picture, the id buffer and its
  // header, the depth map, and the normals.
  const JsonValue captured =
      host.call("render.capture", "{\"scene\":\"" + id +
                                      "\",\"orbit\":{\"distance\":22},\"width\":160,\"height\":120,"
                                      "\"channels\":[\"color\",\"ids\",\"depth\",\"normals\"],"
                                      "\"out_dir\":\"" +
                                      out_dir + "\",\"name\":\"shot\"}");
  const JsonValue& shot = result_of(captured);
  CHECK(number(shot, "width") == 160);
  CHECK(number(shot, "height") == 120);
  CHECK(number(shot, "covered") > 0);
  const JsonValue* files = shot.find("files");
  REQUIRE(files != nullptr);
  for (const char* channel : {"color", "ids", "ids_header", "depth", "normals"}) {
    const std::string native = uri_at(*files, channel);
    REQUIRE_MESSAGE(!native.empty(), channel);
    CHECK_MESSAGE(std::filesystem::exists(native), native);
  }
  // Three u32 per pixel, and the header the id buffer is read with says exactly that.
  const std::string ids = uri_at(*files, "ids");
  CHECK(std::filesystem::file_size(ids) == std::uintmax_t{160} * 120 * 3 * 4);
  const std::string header = text(shot, "ids_header");
  CHECK(header.find("\"words_per_pixel\":3") != std::string::npos);
  CHECK(header.find("\"channels\":[\"instance\",\"cluster\",\"triangle\"]") != std::string::npos);
  CHECK(header.find("\"empty\":4294967295") != std::string::npos);
  const JsonValue* capture_stats = shot.find("stats");
  REQUIRE(capture_stats != nullptr);
  CHECK(number(*capture_stats, "visible_pairs") > 0);

  // A benchmark of the same scene at a different size reuses the loaded mesh and reports GPU
  // milliseconds per pass from the timers, not from a CPU clock.
  const JsonValue benchmarked = host.call(
      "render.benchmark", "{\"scene\":\"" + id + "\",\"width\":320,\"height\":240,\"frames\":12}");
  const JsonValue& bench = result_of(benchmarked);
  CHECK(number(bench, "width") == 320);
  CHECK(number(bench, "height") == 240);
  CHECK(text(bench, "raster") == raster);
  const JsonValue* stats = bench.find("stats");
  REQUIRE(stats != nullptr);
  CHECK(number(*stats, "frames") == 12);
  CHECK(number(*stats, "visible_pairs") > 0);
  CHECK(number(*stats, "visible_min") == number(*stats, "visible_pairs"));  // a still camera
  const JsonValue* gpu = stats->find("gpu_ms");
  REQUIRE(gpu != nullptr);
  CHECK(number(*gpu, "frames") > 0);  // timestamps came back for all but the frames in flight
  for (const char* pass :
       {"cull", "hw", "sw", "hiz", "resolve", "rt", "clas", "deform", "trace", "total"}) {
    REQUIRE_MESSAGE(gpu->find(pass) != nullptr, pass);
  }

  // The capture against itself: identical, so the PSNR is null rather than a very large number.
  const std::string color = uri_at(*files, "color");
  const JsonValue compared =
      host.call("render.compare", "{\"a\":\"" + color + "\",\"b\":\"" + color + "\"}");
  const JsonValue& metrics = result_of(compared);
  CHECK(number(metrics, "width") == 160);
  REQUIRE(metrics.find("identical") != nullptr);
  CHECK(metrics.find("identical")->as_bool());
  REQUIRE(metrics.find("psnr") != nullptr);
  CHECK(metrics.find("psnr")->is_null());
  CHECK(real(metrics, "ssim") == doctest::Approx(1.0));
  CHECK(real(metrics, "flip_mean") == doctest::Approx(0.0));

  // The error paths a client has to tell apart: a scene that is not there, a spelling neither
  // host knows, and a channel that does not exist.
  CHECK(error_code(host.call("render.capture", R"({"scene":"nope"})")) == k_not_found);
  CHECK(error_code(host.call("render.load", R"({"settings":{"raster":"quantum"}})")) ==
        k_invalid_argument);
  CHECK(error_code(
            host.call("render.capture", "{\"scene\":\"" + id + "\",\"channels\":[\"albedo\"]}")) ==
        k_invalid_argument);
  CHECK(error_code(host.call("render.load", R"({"mesh":"a.gltf","scene":"b.json"})")) ==
        k_invalid_argument);
}

// Geometry streaming over the protocol (docs/plan/04-renderer.md §4.9, docs/subsystems/apps.md
// "Streaming pages at run time"). Two things are being checked, and the second is the one that
// matters: that `stats.stream` comes back coherent, and that a per-call `settings` block turning
// streaming *off* produces an empty one — which is what says the additive schema field is reaching
// the renderer rather than being decoded into a struct nobody reads.
//
// The heightfield again, so the case needs no fixture; `page_bytes` is what makes it a streaming
// scene at all, since the heightfield is not paged unless someone asks. 16 KiB instead of the
// 128 KiB default so that a few hundred kilobytes of payload become dozens of pages rather than
// three. Small frames and few of them, because this measures nothing — the picture converging is
// the whole assertion.
TEST_CASE("render: geometry streaming over the protocol") {
  Host host;
  REQUIRE(host.ok);

  // A budget under the scene's page bytes on purpose: it is the one number whose unit differs
  // between the wire (MiB) and the renderer (bytes), so a budget that came back unclamped is the
  // proof the conversion happened. The default grid is big enough for 1 MiB to bite.
  constexpr u64 k_budget_bytes = u64{1} * 1024 * 1024;
  const JsonValue loaded =
      host.call("render.load", R"({"grid":257,"page_bytes":16384,)"
                               R"("settings":{"raster":"hw","stream":true,"page_budget_mib":1}})");
  if (error_code(loaded) == k_render_unavailable) {
    MESSAGE("render.* unavailable here: " << error_message(loaded));
    return;
  }
  const std::string id = text(result_of(loaded), "scene");

  // Enough frames for the feedback round trip to run its course: a frame's page requests are read
  // back when its slot comes around, and the upload budget lets a couple of pages through a frame.
  const JsonValue benchmarked = host.call(
      "render.benchmark", "{\"scene\":\"" + id + "\",\"width\":320,\"height\":240,\"frames\":60}");
  const JsonValue& bench = result_of(benchmarked);
  const JsonValue* stats = bench.find("stats");
  REQUIRE(stats != nullptr);
  const JsonValue* stream = stats->find("stream");
  REQUIRE(stream != nullptr);
  CHECK(number(*stream, "pages_total") > 0);
  CHECK(number(*stream, "pages_resident") > 0);
  CHECK(number(*stream, "pages_resident") <= number(*stream, "pages_total"));
  CHECK(number(*stream, "pages_pinned") > 0);  // the root pages, resident from the first frame
  CHECK(number(*stream, "page_slots") > 0);
  CHECK(number(*stream, "uploads") > 0);
  CHECK(number(*stream, "uploads_bytes") > 0);
  CHECK(number(*stream, "requests") > 0);
  CHECK(number(*stream, "page_bytes") > k_budget_bytes);  // the budget is a fraction of the scene
  CHECK(number(*stream, "budget_bytes") == k_budget_bytes);  // MiB on the wire, bytes in here
  CHECK(number(*stream, "resident_bytes") <= number(*stream, "page_bytes"));
  // The heightfield is built, never written to a container, so its pages can only come out of the
  // loaded scene in host memory: no file source, and the three file counters stay at zero.
  CHECK(text(*stream, "source") == "host");
  CHECK(number(*stream, "file_reads") == 0);
  CHECK(number(*stream, "file_bytes") == 0);
  CHECK(number(*stream, "host_bytes_freed") == 0);
  CHECK(number(*stats, "visible_pairs") > 0);  // and the frame still drew

  // The same scene with streaming turned off for this call alone: the GPU scene is rebuilt whole
  // and there is no residency at all. `pages_total == 0` is the documented way to tell the two
  // apart without testing for the block, and it is only reachable if `stream` travelled.
  const JsonValue plain =
      host.call("render.benchmark", "{\"scene\":\"" + id +
                                        "\",\"width\":320,\"height\":240,\"frames\":12,"
                                        "\"settings\":{\"raster\":\"hw\",\"stream\":false}}");
  const JsonValue* plain_stats = result_of(plain).find("stats");
  REQUIRE(plain_stats != nullptr);
  const JsonValue* plain_stream = plain_stats->find("stream");
  REQUIRE(plain_stream != nullptr);
  CHECK(number(*plain_stream, "pages_total") == 0);
  CHECK(number(*plain_stream, "pages_resident") == 0);
  CHECK(number(*plain_stream, "uploads") == 0);
  CHECK(number(*plain_stream, "page_bytes") == 0);
  CHECK(number(*plain_stats, "visible_pairs") > 0);  // still the same picture's worth of pairs

  // `page_bytes` is bounded like the other load parameters: a page too small to hold a cluster is
  // a typo, not a request.
  CHECK(error_code(host.call("render.load", R"({"grid":33,"page_bytes":7})")) ==
        k_invalid_argument);
  CHECK(error_code(host.call("render.load",
                             R"({"grid":33,"settings":{"upload_budget_kib":4294967295}})")) ==
        k_invalid_argument);
}

// `render.evaluate` and the reference integrator of `render.capture` (docs/plan/04-renderer.md
// §4.8, docs/subsystems/renderer.md "Reference renderer"). They need a device that can build and
// trace the frame's cluster acceleration structures — VK_NV_cluster_acceleration_structure and
// VK_KHR_ray_query both — which the hosted runners have not and the baseline tier machines have
// not either (the TITAN Xp has neither), so both come back 1007 there and the case records a skip
// whose message is the host's refusal, which names the missing extensions in the words of the
// adapter's own verdict. Everything before the reference capture still runs there.
TEST_CASE("render: the reference integrator and the evaluate loop over the protocol") {
  const test::TempDir tmp("engine_render_reference");
  const std::string out_dir = tmp.path();

  Host host;
  REQUIRE(host.ok);
  // `shadows` left at auto: on a device with cluster acceleration structures that is what turns
  // the chain on, and the reference traces exactly the structures that chain builds.
  const JsonValue loaded = host.call("render.load", R"({"grid":33,"settings":{"raster":"hw"}})");
  if (error_code(loaded) == k_render_unavailable) {
    MESSAGE("render.* unavailable here: " << error_message(loaded));
    return;
  }
  const std::string id = text(result_of(loaded), "scene");

  // A reference capture writes the same file a real-time one does, so a caller comparing the two
  // needs no second code path.
  const JsonValue captured = host.call(
      "render.capture", "{\"scene\":\"" + id +
                            "\",\"orbit\":{\"distance\":22},\"width\":96,\"height\":96,"
                            "\"integrator\":\"reference\",\"reference\":{\"spp\":32,\"batch\":16},"
                            "\"out_dir\":\"" +
                            out_dir + "\",\"name\":\"ref\"}");
  if (error_code(captured) == k_render_unavailable) {
    const std::string refusal = error_message(captured);
    MESSAGE("reference integrator unavailable here: " << refusal);
    // The refusal names what the device lacks, rather than "one or the other".
    CHECK((refusal.find("no VK_KHR_ray_query") != std::string::npos ||
           refusal.find("no VK_NV_cluster_acceleration_structure") != std::string::npos ||
           refusal.find("no VK_KHR_acceleration_structure") != std::string::npos));
    return;
  }
  const JsonValue& shot = result_of(captured);
  CHECK(text(shot, "integrator") == "reference");
  CHECK(number(shot, "samples") == 32);
  CHECK(number(shot, "width") == 96);
  CHECK(real(shot, "seconds") > 0.0);
  const JsonValue* shot_files = shot.find("files");
  REQUIRE(shot_files != nullptr);
  CHECK(std::filesystem::exists(uri_at(*shot_files, "color")));
  // A path tracer writes no visibility buffer, so there is nothing to read an id out of.
  CHECK(error_code(host.call(
            "render.capture",
            "{\"scene\":\"" + id + "\",\"integrator\":\"reference\",\"channels\":[\"ids\"]}")) ==
        k_invalid_argument);
  CHECK(error_code(
            host.call("render.capture", "{\"scene\":\"" + id + "\",\"integrator\":\"quantum\"}")) ==
        k_invalid_argument);

  // And the loop in one call: both pictures, the heat map, and the numbers.
  const JsonValue evaluated =
      host.call("render.evaluate", "{\"scene\":\"" + id +
                                       "\",\"orbit\":{\"distance\":22},\"width\":96,\"height\":96,"
                                       "\"reference\":{\"spp\":32,\"batch\":16},\"out_dir\":\"" +
                                       out_dir + "\",\"name\":\"eval\"}");
  const JsonValue& evaluation = result_of(evaluated);
  CHECK(number(evaluation, "width") == 96);
  CHECK(number(evaluation, "samples") == 32);
  CHECK(real(evaluation, "reference_seconds") > 0.0);
  CHECK(real(evaluation, "realtime_seconds") > 0.0);
  CHECK(real(evaluation, "metrics_ms") > 0.0);
  const JsonValue* files = evaluation.find("files");
  REQUIRE(files != nullptr);
  for (const char* channel : {"realtime", "reference", "flip", "reference_direct", "flip_direct"}) {
    const std::string path = uri_at(*files, channel);
    INFO("channel ", channel);
    CHECK(!path.empty());
    CHECK(std::filesystem::exists(path));
  }
  // The gate's own numbers, and the split the report rests on: a comparison against the full
  // reference and one against the reference at a single bounce.
  const JsonValue* full = evaluation.find("full");
  REQUIRE(full != nullptr);
  CHECK(real(*full, "flip_mean") >= 0.0);
  CHECK(real(*full, "ssim") > 0.0);
  const JsonValue* direct = evaluation.find("direct");
  REQUIRE(direct != nullptr);
  CHECK(real(*direct, "flip_mean") >= 0.0);
  CHECK(evaluation.find("stats") != nullptr);
  CHECK(evaluation.find("machine_state") != nullptr);

  CHECK(error_code(host.call("render.evaluate", R"({"scene":"nope"})")) == k_not_found);
  CHECK(error_code(
            host.call("render.evaluate", "{\"scene\":\"" + id + "\",\"reference\":{\"spp\":0}}")) ==
        k_invalid_argument);
}

// A flythrough over the protocol (docs/plan/09-testing-profiling.md §9.4): a scene file with a
// terrain and nothing else, a camera path whose keys hold a height above that terrain, and
// `render.benchmark` flying it twice. The result carries the same summary `engine-view
// --benchmark` ends its .jsonl with: the frame count the path asked for, the percentiles per pass,
// the hashes that name what was measured, and whether both repeats drew the same pairs.
TEST_CASE("render: a camera path flown over the protocol") {
  const test::TempDir tmp("engine_render_flythrough");
  const std::string scene_file = tmp.file("scene.json");
  const std::string path_file = tmp.file("path.json");
  {
    std::ofstream out(scene_file);
    out << R"({"format":"engine.scene.v1","name":"dunes","terrain":{"size":33,"extent":40,)"
        << R"("seed":5,"ridges":[{"from":[-30,0],"to":[30,0],"height":8,"width":8}]}})";
  }
  {
    std::ofstream out(path_file);
    out << R"({"format":"engine.camera-path.v1","name":"rise","fps":10,"keys":[)"
        << R"({"time":0,"position":[0,2,30],"ground":true,"target":[0,0,-20]},)"
        << R"({"time":1,"position":[0,20,10],"target":[0,0,-20]}],)"
        << R"("markers":[{"frame":10,"name":"top"}]})";
  }
  Host host;
  REQUIRE(host.ok);
  const JsonValue loaded =
      host.call("render.load", R"({"scene":")" + scene_file + R"(","ddc":")" + tmp.file("ddc") +
                                   R"(","settings":{"shadows":"off"}})");
  if (error_code(loaded) == k_render_unavailable) {
    MESSAGE("render.* unavailable here: " << error_message(loaded));
    return;
  }
  const std::string id = text(result_of(loaded), "scene");
  const JsonValue flown = host.call(
      "render.benchmark", R"({"scene":")" + id + R"(","width":160,"height":90,"camera_path":")" +
                              path_file + R"(","repeats":2,"warmup":2})");
  const JsonValue& bench = result_of(flown);
  const JsonValue* summary = bench.find("flythrough");
  REQUIRE(summary != nullptr);
  CHECK(text(*summary, "format") == "engine.flythrough.v1");
  CHECK(text(*summary, "scene") == "dunes");
  CHECK(text(*summary, "path") == "rise");
  CHECK(text(*summary, "path_hash").size() == 16);
  CHECK(text(*summary, "identity").size() == 16);
  CHECK(number(*summary, "frames") == 11);  // one second at 10 fps, both ends
  CHECK(number(*summary, "repeats") == 2);
  bool deterministic = false;
  REQUIRE((summary->find("deterministic") != nullptr &&
           summary->find("deterministic")->get_bool(deterministic)));
  CHECK(deterministic);
  const JsonValue* gpu = summary->find("gpu_ms");
  REQUIRE(gpu != nullptr);
  const JsonValue* total = gpu->find("total");
  REQUIRE(total != nullptr);
  CHECK(real(*total, "median") > 0.0);
  CHECK(real(*total, "p99") >= real(*total, "median"));
  const JsonValue* markers = summary->find("markers");
  REQUIRE(markers != nullptr);
  REQUIRE(markers->size() == 1);
  CHECK(number((*markers)[0], "frame") == 10);

  // A path that asks for heights above a terrain the scene does not have is refused.
  const JsonValue refused = host.call("render.load", R"({"grid":17,"settings":{"shadows":"off"}})");
  const std::string heightfield = text(result_of(refused), "scene");
  const JsonValue no_ground =
      host.call("render.benchmark",
                R"({"scene":")" + heightfield + R"(","camera_path":")" + path_file + R"("})");
  CHECK(error_code(no_ground) != 0);
  CHECK(error_message(no_ground).find("terrain") != std::string::npos);
}

// `render.compare` is the one render method that never opens a device, so it runs everywhere —
// including the hosted runners — and it is the method the accept-or-reject loop of
// docs/plan/04-renderer.md §4.8 calls after a capture. Through engine-cli, one process per call,
// which is how a script uses it.
TEST_CASE("render: compare through engine-cli, on a machine with or without a GPU") {
  std::vector<std::string_view> argv;
  argv.push_back(cli_exe());
  argv.push_back("--host");
  argv.push_back(host_exe());
  argv.push_back("render.compare");
  argv.push_back(R"({"a":"no-such-file.png","b":"no-such-file.png"})");
  platform::Process p;
  std::string error;
  REQUIRE_MESSAGE(p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error,
                          /*merge_stderr=*/true),
                  error);
  p.close_stdin();
  std::string output;
  p.read_all(output);
  // The method was reached and refused the files it could not read: IoError (1005), which is
  // neither "no such method" nor RenderUnavailable, so a script can tell them apart.
  CHECK(p.wait() == 1);
  CHECK_MESSAGE(output.find("error 1005") != std::string::npos, output);
  CHECK_MESSAGE(output.find("no-such-file.png") != std::string::npos, output);
}

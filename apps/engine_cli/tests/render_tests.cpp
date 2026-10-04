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
#include <foundation/image/decode.h>
#if ENGINE_CLI_TESTS_SKY
#include <domain/sky/ephemeris.h>
#endif

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
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

// `render.unload` and `render.scenes` with no scene at all, which is every machine's case: an id
// the host never handed out is 1003 like any missing scene, and listing opens no device — the
// adapter is empty and the memory figures null, not zero, because nothing was asked of a GPU.
TEST_CASE("render: unload and scenes answer on a machine with or without a GPU") {
  Host host;
  REQUIRE(host.ok);
  const JsonValue listed = host.call("render.scenes", "{}");
  const JsonValue& scenes = result_of(listed);
  REQUIRE(scenes.find("scenes") != nullptr);
  CHECK(scenes.find("scenes")->size() == 0);
  CHECK(text(scenes, "adapter").empty());
  REQUIRE(scenes.find("gpu_used_bytes") != nullptr);
  CHECK(scenes.find("gpu_used_bytes")->is_null());
  CHECK(number(scenes, "host_heap_bytes") > 0);

  const JsonValue unknown = host.call("render.unload", R"({"scene":"scene1"})");
  CHECK(error_code(unknown) == k_not_found);
  // Never handed out, so not "unloaded": the message says only that there is no such scene.
  CHECK(error_message(unknown).find("unloaded") == std::string::npos);
  CHECK(error_code(host.call("render.unload", "{}")) == k_not_found);
  CHECK(error_code(host.call("render.unload", R"({"scene":5})")) == -32602);
}

// Loading, drawing and unloading, where there is a GPU (docs/subsystems/apps.md, "Scenes"). What
// is checked is what unload is for: the id is dead afterwards — a capture of it, a benchmark of it
// and a second unload are all 1003, with a message that says it was unloaded — and the memory goes.
//
// **Which memory, measured how.** The host heap is the engine allocator's own count, so it drops
// back to within a little of what it was before the load (the log ring and the dispatcher's buffers
// are the little). The device memory is the driver's figure for this process
// (VK_EXT_memory_budget), which counts the allocator's *blocks*, not its allocations: a scene whose
// buffers fit in blocks an earlier scene opened moves it by nothing, in either direction. So the
// scene here is drawn at 7680x4320 — one benchmark frame, nothing read back — whose screen-sized
// targets are large enough to be allocations of their own: the figure rises by hundreds of MiB with
// the scene and falls back when it is unloaded. The bound is "at least three quarters of what the
// load added came back", because the allocator may keep an emptied block for its next allocation;
// measured on the RTX 5090 the figure went from 137.7 MB to 849.4 MB and back to 139.5 MB. And
// nothing leaks: a second cycle of the same scene ends at no more than the first one did.
TEST_CASE("render: unload releases a scene and scenes lists what the host holds") {
  const test::TempDir tmp("engine_render_unload");
  const std::string out_dir = tmp.path();
  Host host;
  REQUIRE(host.ok);

  // A small scene first, drawn, so the device is open and every figure below is measured with it.
  const JsonValue first = host.call("render.load", R"({"grid":33,"settings":{"shadows":"off"}})");
  if (error_code(first) == k_render_unavailable) {
    MESSAGE("render.* unavailable here: " << error_message(first));
    return;
  }
  const std::string small = text(result_of(first), "scene");
  const auto capture = [&](const std::string& id) {
    return host.call("render.capture", "{\"scene\":\"" + id +
                                           "\",\"width\":320,\"height\":240,\"out_dir\":\"" +
                                           out_dir + "\",\"name\":\"" + id + "\"}");
  };
  result_of(capture(small));

  // What the host holds, and what memory it takes to hold it.
  struct Held {
    usize scenes = 0;
    u64 heap = 0;
    std::optional<u64> gpu;
  };
  const auto held = [&]() {
    const JsonValue response = host.call("render.scenes", "{}");
    const JsonValue& r = result_of(response);
    Held h;
    h.scenes = r.find("scenes") != nullptr ? r.find("scenes")->size() : 0;
    h.heap = number(r, "host_heap_bytes");
    u64 gpu = 0;
    if (const JsonValue* v = r.find("gpu_used_bytes"); v != nullptr && v->get_u64(gpu)) h.gpu = gpu;
    return h;
  };
  const Held before = held();
  CHECK(before.scenes == 1);

  // The listing says where each scene came from and whether it has been drawn.
  {
    const JsonValue response = host.call("render.scenes", "{}");
    const JsonValue& r = result_of(response);
    const JsonValue& entry = (*r.find("scenes"))[0];
    CHECK(text(entry, "scene") == small);
    CHECK(text(entry, "kind") == "procedural");
    CHECK(text(entry, "source") == "heightfield, grid 33");
    CHECK(number(entry, "triangles") == 32 * 32 * 2);
    CHECK(entry.find("built")->as_bool());
    CHECK(number(entry, "width") == 320);
    CHECK_FALSE(text(r, "adapter").empty());
  }

  // One cycle: load, draw one 8K frame, look, unload.
  const auto cycle = [&](Held& peak, JsonValue& unloaded) {
    const JsonValue loaded =
        host.call("render.load", R"({"grid":257,"settings":{"shadows":"off"}})");
    const std::string id = text(result_of(loaded), "scene");
    const JsonValue drawn =
        host.call("render.benchmark",
                  "{\"scene\":\"" + id + "\",\"width\":7680,\"height\":4320,\"frames\":1}");
    result_of(drawn);
    peak = held();
    const JsonValue response = host.call("render.unload", "{\"scene\":\"" + id + "\"}");
    unloaded = result_of(response);
    return id;
  };
  Held peak1;
  JsonValue unload1;
  const std::string big = cycle(peak1, unload1);
  CHECK(peak1.scenes == 2);
  CHECK(peak1.heap > before.heap + 1024 * 1024);  // 131,072 triangles' clusters and LOD graph
  CHECK(text(unload1, "scene") == big);
  CHECK(number(unload1, "triangles") == 256 * 256 * 2);
  CHECK(unload1.find("gpu_scene")->as_bool());
  CHECK_FALSE(unload1.find("reference")->as_bool());
  CHECK(number(unload1, "scenes_left") == 1);
  // The host arrays went: back to within a little of before the load, far below the peak.
  const u64 heap_after = number(unload1, "host_heap_bytes_after");
  CHECK(number(unload1, "host_heap_bytes_before") > heap_after + 1024 * 1024);
  CHECK(heap_after < before.heap + 256 * 1024);
  const Held after1 = held();
  CHECK(after1.scenes == 1);

  // The id names nothing now, and the error says it once did.
  const JsonValue dead = capture(big);
  CHECK(error_code(dead) == k_not_found);
  CHECK(error_message(dead).find("unloaded") != std::string::npos);
  CHECK(error_code(host.call("render.unload", "{\"scene\":\"" + big + "\"}")) == k_not_found);
  CHECK(error_code(host.call("render.benchmark", "{\"scene\":\"" + big + "\"}")) == k_not_found);

  // The device memory, where the driver reports it.
  if (!before.gpu.has_value() || !peak1.gpu.has_value() || !after1.gpu.has_value()) {
    MESSAGE("no VK_EXT_memory_budget on this device: the GPU memory half is not checked");
    return;
  }
  MESSAGE("device memory of this process: " << *before.gpu << " before the load, " << *peak1.gpu
                                            << " with the scene drawn at 7680x4320, " << *after1.gpu
                                            << " after unloading it");
  REQUIRE(*peak1.gpu > *before.gpu + 256u * 1024u * 1024u);
  const u64 added = *peak1.gpu - *before.gpu;
  CHECK(*after1.gpu <= *before.gpu + added / 4);
  u64 reported = 0;
  REQUIRE(unload1.find("gpu_used_bytes_after")->get_u64(reported));
  CHECK(reported == *after1.gpu);  // unload's own figure is the one the listing reads

  // A second cycle of the same scene ends where the first ended: nothing leaked.
  Held peak2;
  JsonValue unload2;
  cycle(peak2, unload2);
  u64 after2 = 0;
  REQUIRE(unload2.find("gpu_used_bytes_after")->get_u64(after2));
  MESSAGE("second cycle: " << *after1.gpu << " before the load, " << *peak2.gpu << " drawn, "
                           << after2 << " after the unload");
  CHECK(after2 <= *after1.gpu);
  CHECK(held().scenes == 1);
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

// ---- one request, two hosts (docs/subsystems/renderer.md, "One request, two hosts") ------------
//
// The sky's hour, the ground's time and the ground's layout reached engine-view with flags and
// render.* not at all until 2026-10-03. These three cases hold the protocol to them: a capture of
// the erg at two hours whose sky differs and whose reported sun stands where the sky capability's
// ephemeris puts it; a capture of the dunes three years on that differs from the scene's own and is
// engine-view's picture with the same flags, byte for byte; and a benchmark along the endless
// desert's path with its ground on the world's tiles, reporting the tile and sky columns
// engine-view's `--benchmark` reports.

namespace {

// A path as a JSON string: forward slashes, which every platform's file API takes.
std::string json_path(std::string path) {
  for (char& c : path)
    if (c == '\\') c = '/';
  return "\"" + path + "\"";
}

bool read_bytes(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

bool read_json_file(const std::string& path, JsonValue& out) {
  std::string text;
  return read_bytes(path, text) && parse_json(text, out).ok;
}

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
  return static_cast<bool>(out);
}

// A committed scene the build cannot draw — one naming a generator this configuration does not
// link — is refused at load with the registry's sentence; such a build skips, as one with no GPU
// does.
bool skipped(const JsonValue& loaded) {
  if (error_code(loaded) == k_render_unavailable) {
    MESSAGE("render.* unavailable here: " << error_message(loaded));
    return true;
  }
  if (error_message(loaded).find("this build does not have") != std::string::npos) {
    MESSAGE("this build cannot draw the scene: " << error_message(loaded));
    return true;
  }
  return false;
}

// The committed erg (content/test-scenes/desert-erg) with its terrain shrunk to 257 samples over
// 256 m, so a debug build loads it in seconds: its bands, its wind, its sky and its time are the
// scene's own. The calendar is read back for the ephemeris.
struct SmallErg {
  std::string path;
  f64 time_s = 0.0;
  f64 latitude_deg = 30.0;
  f64 day_of_year = 80.0;
  f64 moon_age_days = 0.0;
};

bool small_erg(const test::TempDir& tmp, SmallErg& out) {
  const std::string committed =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/desert-erg/scene.json",
                      "content/test-scenes/desert-erg/scene.json");
  JsonValue erg;
  if (!read_json_file(committed, erg)) return false;
  JsonValue terrain = *erg.find("terrain");
  terrain.set("size", JsonValue(static_cast<u64>(257)));
  terrain.set("extent", JsonValue(256.0));
  REQUIRE(terrain.find("time")->get_f64(out.time_s));
  erg.set("terrain", std::move(terrain));
  const JsonValue* sky = erg.find("sky");
  REQUIRE(sky != nullptr);
  (void)sky->find("latitude_deg")->get_f64(out.latitude_deg);
  (void)sky->find("day_of_year")->get_f64(out.day_of_year);
  (void)sky->find("moon_age_days")->get_f64(out.moon_age_days);
  // The scene names its camera path beside it.
  std::string path_text;
  REQUIRE(read_bytes(
      committed.substr(0, committed.size() - std::string("scene.json").size()) + "camera-path.json",
      path_text));
  REQUIRE(write_text(tmp.file("camera-path.json"), path_text));
  out.path = tmp.file("erg.json");
  return write_text(out.path, write_json(erg));
}

f64 coordinate(const JsonValue& object, std::string_view key, usize i) {
  const JsonValue* value = object.find(key);
  f64 out = 0.0;
  REQUIRE((value != nullptr && value->is_array() && value->size() > i));
  REQUIRE((*value)[i].get_f64(out));
  return out;
}

const JsonValue& member(const JsonValue& object, std::string_view key) {
  const JsonValue* value = object.find(key);
  REQUIRE_MESSAGE(value != nullptr, key);
  return *value;
}

// engine-view with `args` after its own path, offscreen: its exit code and everything it printed
// (the summary line last). 3 is a machine it cannot render on.
i32 run_engine_view(const std::vector<std::string>& args, std::string& output) {
  std::vector<std::string> all = {test::app_path(ENGINE_VIEW_PATH)};
  all.insert(all.end(), args.begin(), args.end());
  std::vector<std::string_view> argv(all.begin(), all.end());
  platform::Process p;
  std::string error;
  REQUIRE_MESSAGE(p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error,
                          /*merge_stderr=*/true),
                  error);
  p.close_stdin();
  p.read_all(output);
  return p.wait();
}

// engine-view's picture against the host's of the same request: **the same bytes**, because the
// picture is a function of the scene, the camera and the clock, and the same device gives the same
// bytes in any process. Until 2026-10-04 this was "within one step of 255 on under a thousandth
// of the bytes": the sky's metered sum (`stats.sky.sky_lux`) came out a few parts in ten million
// apart between the two hosts and moved a channel by one at a pixel or two — on the RTX 5090 three
// years on, on the Titan Xp at the erg's own time too. The meter was not the cause: the protocol's
// orbit stood 32 float steps lower than engine-view's `--orbit` (0.8 mm here), and the sky is
// metered from the eye's altitude. Both now place the orbit with one function from one pitch
// (renderer.md, "One request, two hosts"; request_tests.cpp holds the camera to the bit). When the
// bytes differ, how many and by how much is printed before the check fails.
void same_bytes(const std::string& what, const std::string& view_png, const std::string& host_png,
                const std::string& view_bytes, const std::string& host_bytes) {
  if (view_bytes == host_bytes) {
    MESSAGE(what << ": byte for byte");
    return;
  }
  image::Image a;
  image::Image b;
  std::string message;
  REQUIRE(image::read_image(view_png, a, 4, &message) == io::Status::Ok);
  REQUIRE(image::read_image(host_png, b, 4, &message) == io::Status::Ok);
  REQUIRE(a.pixels.size() == b.pixels.size());
  u32 differ = 0;
  u32 largest = 0;
  for (u32 i = 0; i < a.pixels.size(); ++i) {
    const u32 d = a.pixels[i] > b.pixels[i] ? a.pixels[i] - b.pixels[i] : b.pixels[i] - a.pixels[i];
    if (d != 0) ++differ;
    largest = d > largest ? d : largest;
  }
  MESSAGE(what << ": " << differ << " channel bytes differ, by at most " << largest);
  CHECK(view_bytes == host_bytes);
}

}  // namespace

TEST_CASE("render: the erg at two hours, its sun where the ephemeris puts it") {
  const test::TempDir tmp("engine_render_sky_hours");
  SmallErg erg;
  if (!small_erg(tmp, erg)) {
    MESSAGE("the erg is not in this bundle");
    return;
  }
  Host host;
  REQUIRE(host.ok);
  const JsonValue loaded =
      host.call("render.load", "{\"scene\":" + json_path(erg.path) +
                                   ",\"ddc\":" + json_path(tmp.file("ddc")) + "}");
  if (skipped(loaded)) return;
  const JsonValue& scene = result_of(loaded);
  CHECK(member(scene, "sky").as_bool());
  CHECK(real(scene, "ground_time_s") == erg.time_s);
  CHECK(text(scene, "ground") == "mesh");
  const std::string id = text(scene, "scene");
  // From a radius above the bounding sphere's centre, looking up and east: everything above the
  // horizon line is sky, and nothing of the terrain can be above the camera.
  const f64 cx = coordinate(scene, "center", 0);
  const f64 cy = coordinate(scene, "center", 1);
  const f64 cz = coordinate(scene, "center", 2);
  const f64 r = real(scene, "radius");
  char camera[256];
  std::snprintf(camera, sizeof(camera),
                R"({"position":[%.3f,%.3f,%.3f],"target":[%.3f,%.3f,%.3f],"fov_deg":60})", cx,
                cy + r, cz, cx + 2.0 * r, cy + 1.25 * r, cz);
  struct Shot {
    f64 hour = 0.0;
    image::Image color;
    std::vector<u32> ids;
    JsonValue sky;
  };
  Shot shots[2];
  shots[0].hour = 12.0;
  shots[1].hour = 18.5;
  for (Shot& shot : shots) {
    const std::string name = shot.hour < 13.0 ? "noon" : "evening";
    const JsonValue captured = host.call(
        "render.capture", "{\"scene\":\"" + id + "\",\"camera\":" + camera +
                              ",\"width\":160,\"height\":96,\"channels\":[\"color\",\"ids\"],"
                              "\"out_dir\":" +
                              json_path(tmp.path()) + ",\"name\":\"" + name +
                              "\",\"clock\":{\"time_of_day\":" + std::to_string(shot.hour) + "}}");
    const JsonValue& result = result_of(captured);
    const JsonValue& files = member(result, "files");
    std::string message;
    REQUIRE_MESSAGE(
        image::read_image(uri_at(files, "color"), shot.color, 4, &message) == io::Status::Ok,
        message);
    std::string bytes;
    REQUIRE(read_bytes(uri_at(files, "ids"), bytes));
    REQUIRE(bytes.size() == usize{160} * 96 * 3 * 4);
    shot.ids.resize(bytes.size() / 4);
    std::memcpy(shot.ids.data(), bytes.data(), bytes.size());
    const JsonValue* sky = member(result, "stats").find("sky");
    REQUIRE_MESSAGE((sky != nullptr && sky->is_object()), "a scene with a sky reports it");
    shot.sky = *sky;
    // The hour the frame stood at is the hour asked for: the offset on the one clock, applied by
    // the renderer's `frame_clock`, as engine-view's `--time-of-day` applies it.
    CHECK(std::abs(real(shot.sky, "hour") - shot.hour) < 1.0e-6);
#if ENGINE_CLI_TESTS_SKY
    // And the sun stands where the sky capability's ephemeris puts it at the game time reported.
    sky::Calendar calendar;
    calendar.latitude_deg = erg.latitude_deg;
    calendar.day_of_year = erg.day_of_year;
    calendar.moon_age_days = erg.moon_age_days;
    sky::Ephemeris e;
    sky::ephemeris(calendar, real(shot.sky, "time_s"), e);
    const f64 elevation = std::asin(e.sun.y) * 180.0 / 3.14159265358979323846;
    INFO("hour " << shot.hour << ": reported " << real(shot.sky, "sun_elevation_deg")
                 << ", ephemeris " << elevation);
    CHECK(std::abs(real(shot.sky, "sun_elevation_deg") - elevation) < 0.01);
#endif
  }
  CHECK(real(shots[0].sky, "sun_elevation_deg") > real(shots[1].sky, "sun_elevation_deg"));
  // The sky pixels — the ones no surface covers, the same in both, since the camera is — differ.
  u32 uncovered = 0;
  u32 differ = 0;
  const u32 pixels = 160 * 96;
  for (u32 p = 0; p < pixels; ++p) {
    if (shots[0].ids[p * 3] != 0xFFFFFFFFu) continue;
    CHECK(shots[1].ids[p * 3] == 0xFFFFFFFFu);
    ++uncovered;
    if (std::memcmp(&shots[0].color.pixels[p * 4], &shots[1].color.pixels[p * 4], 3) != 0) {
      ++differ;
    }
  }
  MESSAGE("sky pixels " << uncovered << " of " << pixels << ", " << differ << " differ");
  CHECK(uncovered > pixels / 4);
  CHECK(differ * 10 > uncovered * 9);
}

TEST_CASE("render: the dunes three years on, as engine-view draws them, byte for byte") {
  const test::TempDir tmp("engine_render_ground_time");
  SmallErg erg;
  if (!small_erg(tmp, erg)) {
    MESSAGE("the erg is not in this bundle");
    return;
  }
  const f64 later = erg.time_s + 94'608'000.0;  // three years on
  char later_text[64];
  std::snprintf(later_text, sizeof(later_text), "%.1f", later);
  const std::string ddc = tmp.file("ddc");
  const std::string out_dir = tmp.file("out");
  std::string own;
  std::string own_explicit;
  std::string moved;
  {
    // The host's own scope: it holds the machine's GPU lock for as long as it has a device open,
    // and engine-view below takes it too.
    Host host;
    REQUIRE(host.ok);
    const std::string capture_tail =
        ",\"orbit\":{\"distance\":22},\"width\":160,\"height\":96,"
        "\"out_dir\":" +
        json_path(out_dir);
    const JsonValue loaded = host.call(
        "render.load", "{\"scene\":" + json_path(erg.path) + ",\"ddc\":" + json_path(ddc) + "}");
    if (skipped(loaded)) return;
    const std::string id = text(result_of(loaded), "scene");
    const JsonValue a = host.call(
        "render.capture", "{\"scene\":\"" + id + "\"" + capture_tail + ",\"name\":\"own\"}");
    REQUIRE(read_bytes(uri_at(member(result_of(a), "files"), "color"), own));
    // Every field this change added, spelled at its default, draws the same bytes as naming none:
    // what keeps every caller from before it drawing what it drew.
    const JsonValue b = host.call(
        "render.capture",
        "{\"scene\":\"" + id + "\"" + capture_tail +
            ",\"name\":\"own_explicit\",\"clock\":{\"time_of_day\":null,\"sun_rate\":null},"
            "\"settings\":{\"exposure_ev\":0,\"exposure_ev100\":null,\"terrain_rings\":false,"
            "\"terrain_tiles\":false,\"deform_pool_mib\":0,\"static_shape_kib\":0,\"morph\":[],"
            "\"page_budget_pct\":0}}");
    REQUIRE(read_bytes(uri_at(member(result_of(b), "files"), "color"), own_explicit));
    const JsonValue loaded_later = host.call(
        "render.load", "{\"scene\":" + json_path(erg.path) + ",\"ddc\":" + json_path(ddc) +
                           ",\"ground_time_s\":" + later_text + "}");
    const JsonValue& later_scene = result_of(loaded_later);
    CHECK(real(later_scene, "ground_time_s") == later);
    const JsonValue c =
        host.call("render.capture", "{\"scene\":\"" + text(later_scene, "scene") + "\"" +
                                        capture_tail + ",\"name\":\"later\"}");
    REQUIRE(read_bytes(uri_at(member(result_of(c), "files"), "color"), moved));
    // A scene with no terrain has no ground time to set.
    const JsonValue refused = host.call("render.load", R"({"grid":17,"ground_time_s":1})");
    CHECK(error_code(refused) == k_invalid_argument);
  }
  CHECK(own == own_explicit);
  CHECK(own != moved);

  // engine-view with the same flags: the scene, the orbit, the size, and --ground-time.
  const auto view = [&](bool with_time, const std::string& png, std::string& out) {
    std::vector<std::string> args = {"--scene",  erg.path,  "--ddc",   ddc,         "--offscreen",
                                     "--frames", "1",       "--width", "160",       "--height",
                                     "96",       "--orbit", "22",      "--capture", png};
    if (with_time) {
      args.push_back("--ground-time");
      args.push_back(later_text);
    }
    std::string output;
    const i32 code = run_engine_view(args, output);
    if (code == 3) {
      MESSAGE("engine-view cannot render here: " << output);
      return false;
    }
    REQUIRE_MESSAGE(code == 0, output);
    REQUIRE(read_bytes(png, out));
    return true;
  };
  std::string view_own;
  std::string view_later;
  if (!view(false, tmp.file("view-own.png"), view_own)) return;
  REQUIRE(view(true, tmp.file("view-later.png"), view_later));
  // engine-view's picture is the host's, byte for byte, at both times (`same_bytes`).
  same_bytes("the scene's own time", tmp.file("view-own.png"), out_dir + "/own.png", view_own, own);
  same_bytes("three years on", tmp.file("view-later.png"), out_dir + "/later.png", view_later,
             moved);
}

TEST_CASE("render: the endless desert benchmarked on its tiles, with its sky") {
  const test::TempDir tmp("engine_render_endless");
  const std::string committed =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/desert-endless/scene.json",
                      "content/test-scenes/desert-endless/scene.json");
  JsonValue endless;
  JsonValue path;
  if (!read_json_file(committed, endless) ||
      !read_json_file(committed.substr(0, committed.size() - std::string("scene.json").size()) +
                          "camera-path.json",
                      path)) {
    MESSAGE("the endless desert is not in this bundle");
    return;
  }
  // Its world's rings brought in to six tiles, so a debug build lays the first ring out in
  // seconds; the tiles, their cells, the bands, the sky and the time are the scene's own.
  JsonValue world = *endless.find("world");
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
  endless.set("world", std::move(world));
  const std::string scene_file = tmp.file("endless.json");
  REQUIRE(write_text(scene_file, write_json(endless)));
  REQUIRE(write_text(tmp.file("camera-path.json"), write_json(path)));
  // A prefix of the committed path: its first two keys, two seconds and 200 m of the flight.
  JsonValue keys = JsonValue::array();
  keys.push_back(member(path, "keys")[0]);
  keys.push_back(member(path, "keys")[1]);
  path.set("keys", std::move(keys));
  path.set("markers", JsonValue::array());
  const std::string prefix = tmp.file("prefix.json");
  REQUIRE(write_text(prefix, write_json(path)));

  const std::string ddc = tmp.file("ddc");
  const std::string out_dir = tmp.file("captures");
  std::string host_png;
  u64 host_far = 0;
  {
    // The host's own scope: it holds the machine's GPU lock for as long as it has a device open,
    // and engine-view below takes it too.
    Host host;
    REQUIRE(host.ok);
    // Two far levels past the rings (renderer.md, "Ground to the horizon"; ADR-0051): the 4.9 km a
    // second level reaches, where the default's six would cost a debug build seconds more.
    const JsonValue loaded = host.call(
        "render.load", "{\"scene\":" + json_path(scene_file) + ",\"ddc\":" + json_path(ddc) +
                           ",\"settings\":{\"terrain_tiles\":true,\"terrain_far_levels\":2}}");
    if (skipped(loaded)) return;
    const JsonValue& scene = result_of(loaded);
    CHECK(text(scene, "ground") == "tiles");
    CHECK(member(scene, "sky").as_bool());
    const JsonValue flown =
        host.call("render.benchmark",
                  "{\"scene\":\"" + text(scene, "scene") +
                      "\",\"width\":160,\"height\":96,\"camera_path\":" + json_path(prefix) +
                      ",\"path_frames\":12,\"repeats\":1,\"warmup\":0,"
                      "\"clock\":{\"time_of_day\":16}}");
    const JsonValue& bench = result_of(flown);
    const JsonValue& stats = member(bench, "stats");
    // The tile columns: the ground drawn from the world's tiles — the scene's grid, the far
    // levels and the three rings — and its chunks and rebuilds.
    const JsonValue& ground = member(stats, "ground");
    CHECK(text(ground, "layout") == "tiles");
    CHECK(number(ground, "far_levels") == 2);
    CHECK(number(ground, "levels") == 1 + 2 + 3);
    CHECK(number(ground, "chunks") > 0);
    CHECK(number(ground, "device_bytes") > 0);
    // The sky columns: the hour the clock asked for, and what the sky's passes cost a frame.
    const JsonValue& sky = member(stats, "sky");
    CHECK(std::abs(real(sky, "hour") - 16.0) < 1.0e-6);
    CHECK(real(member(stats, "gpu_ms"), "sky") > 0.0);
    // And the flythrough summary carries the blocks engine-view's `--benchmark` ends with.
    const JsonValue& summary = member(bench, "flythrough");
    CHECK(number(summary, "frames") == 12);
    const JsonValue& lapse = member(summary, "time_lapse");
    REQUIRE(lapse.is_object());
    CHECK(text(member(lapse, "rings"), "layout") == "tiles");
    CHECK(number(member(lapse, "rings"), "far_levels") == 2);
    CHECK(member(lapse, "levels").size() == 1 + 2 + 3);
    const JsonValue& sun = member(summary, "sun");
    REQUIRE(sun.is_object());
    CHECK(std::abs(real(member(sun, "sky"), "hour") - 16.0) < 1.0e-6);
    CHECK(real(member(member(summary, "gpu_ms"), "sky"), "median") > 0.0);

    // **A request that names no far levels draws what engine-view draws**: both take the
    // `renderer.terrain.far_levels` tunable's count. From the orbit's 2 km up the ground runs past
    // the rings to the far levels' reach, so a count that differed would be a different picture.
    const JsonValue plain = host.call("render.load", "{\"scene\":" + json_path(scene_file) +
                                                         ",\"ddc\":" + json_path(ddc) +
                                                         ",\"settings\":{\"terrain_tiles\":true}}");
    const JsonValue shot = host.call(
        "render.capture", "{\"scene\":\"" + text(result_of(plain), "scene") +
                              "\",\"orbit\":{\"distance\":22},\"width\":160,\"height\":96,"
                              "\"out_dir\":" +
                              json_path(out_dir) + ",\"name\":\"far\"}");
    host_far = number(member(member(result_of(shot), "stats"), "ground"), "far_levels");
    host_png = uri_at(member(result_of(shot), "files"), "color");
  }
  std::string host_bytes;
  REQUIRE(read_bytes(host_png, host_bytes));
  const std::string view_png = tmp.file("view-far.png");
  std::string output;
  const i32 code =
      run_engine_view({"--scene", scene_file, "--ddc", ddc, "--offscreen", "--frames", "1",
                       "--width", "160", "--height", "96", "--orbit", "22", "--capture", view_png},
                      output);
  if (code == 3) {
    MESSAGE("engine-view cannot render here: " << output);
    return;
  }
  REQUIRE_MESSAGE(code == 0, output);
  // The summary is the last line that is a JSON object; the log is interleaved before it.
  JsonValue view_summary;
  bool found = false;
  for (usize end = output.size(); !found && end > 0;) {
    const usize start = output.rfind('\n', end - 1);
    const usize from = start == std::string::npos ? 0 : start + 1;
    const std::string_view line = std::string_view(output).substr(from, end - from);
    found = !line.empty() && line.front() == '{' && parse_json(line, view_summary).ok;
    end = start == std::string::npos ? 0 : start;
  }
  REQUIRE_MESSAGE(found, output);
  const u64 view_far = number(member(member(view_summary, "time_lapse"), "rings"), "far_levels");
  MESSAGE("far levels with none named: the host " << host_far << ", engine-view " << view_far);
  CHECK(host_far > 0);
  CHECK(host_far == view_far);
  std::string view_bytes;
  REQUIRE(read_bytes(view_png, view_bytes));
  same_bytes("the endless desert's far levels", view_png, host_png, view_bytes, host_bytes);
}

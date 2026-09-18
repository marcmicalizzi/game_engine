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
#include <test_temp_dir.h>

#include <filesystem>
#include <string>
#include <vector>

using namespace engine;

namespace {

constexpr i32 k_render_unavailable = 1007;
constexpr i32 k_not_found = 1003;
constexpr i32 k_invalid_argument = 1004;

// One engine-host process fed a series of requests, so the scene a `render.load` leaves behind
// is still there for the calls after it.
struct Host {
  platform::Process process;
  u32 next_id = 1;
  bool ok = false;

  Host() {
    const std::string_view argv[2] = {ENGINE_HOST_PATH, "--stdio"};
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
  CHECK(text(scene, "raster") == "hw");
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
  CHECK(text(bench, "raster") == "hw");
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
// §4.8, docs/subsystems/renderer.md "Reference renderer"). They need a device that can build the
// frame's cluster acceleration structures, which the hosted runners have not and the baseline
// tier machines have not either, so both come back 1007 there and the case records a skip — the
// same shape as the rest of this file.
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
    MESSAGE("the reference needs cluster acceleration structures: " << error_message(captured));
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

// `render.compare` is the one render method that never opens a device, so it runs everywhere —
// including the hosted runners — and it is the method the accept-or-reject loop of
// docs/plan/04-renderer.md §4.8 calls after a capture. Through engine-cli, one process per call,
// which is how a script uses it.
TEST_CASE("render: compare through engine-cli, on a machine with or without a GPU") {
  std::vector<std::string_view> argv;
  argv.push_back(ENGINE_APP_PATH);
  argv.push_back("--host");
  argv.push_back(ENGINE_HOST_PATH);
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

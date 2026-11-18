#include <core/json/json.h>
#include <core/profiling/profile.h>
#include <foundation/profiler/frame_report.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

using namespace engine;
using namespace engine::profiler;
using profiling::CaptureKind;
using profiling::CaptureTrack;

namespace {

constexpr i64 k_ms = 1'000'000;  // nanoseconds

CaptureEvent mark(u64 index, i64 at_ms) {
  CaptureEvent e;
  e.kind = CaptureKind::frame;
  e.name = "frame";
  e.frame = index;
  e.begin_ns = e.end_ns = at_ms * k_ms;
  return e;
}
CaptureEvent mark_end(u64 index, i64 at_ms) {
  CaptureEvent e = mark(index, at_ms);
  e.kind = CaptureKind::frame_end;
  return e;
}
CaptureEvent zone(const char* name, i64 begin_ms, i64 end_ms, u16 thread = 0) {
  CaptureEvent e;
  e.kind = CaptureKind::zone;
  e.name = name;
  e.begin_ns = begin_ms * k_ms;
  e.end_ns = end_ms * k_ms;
  e.thread = thread;
  return e;
}
CaptureEvent value(CaptureTrack track, const char* name, u64 frame, f64 ms) {
  CaptureEvent e;
  e.kind = CaptureKind::value;
  e.track = track;
  e.name = name;
  e.frame = frame;
  e.ms = ms;
  return e;
}

const SeriesStats* find(const Vector<SeriesStats>& list, const char* name) {
  for (const SeriesStats& s : list)
    if (std::strcmp(s.name, name) == 0) return &s;
  return nullptr;
}

}  // namespace

TEST_CASE("profiler: percentiles interpolate between the closest ranks") {
  std::vector<f64> v = {5, 1, 4, 2, 3};
  const Percentiles p = percentiles(v);
  CHECK(p.median == doctest::Approx(3.0));
  CHECK(p.p95 == doctest::Approx(4.8));
  CHECK(p.p99 == doctest::Approx(4.96));
  CHECK(p.max == 5.0);
  CHECK(p.mean == doctest::Approx(3.0));
  std::vector<f64> none;
  CHECK(percentiles(none).max == 0.0);
}

TEST_CASE("profiler: a report attributes zones and values to their frames, over a window") {
  // Four frames: 0..10, 10..20, 20..25 (ended), 30..40 (the stop). A request's dispatch zone
  // wraps the first frames and begins before any mark: it is outside every frame.
  std::vector<CaptureEvent> events;
  events.push_back(zone("dispatch", -2, 26));
  for (u64 f = 0; f < 3; ++f)
    events.push_back(mark(f, static_cast<i64>(f) * 10));
  events.push_back(mark_end(2, 25));
  events.push_back(mark(3, 30));
  // "cull" runs every frame: 1, 2, 3, 4 ms; "upload" in frame 1 only, twice, on two threads.
  for (u64 f = 0; f < 4; ++f) {
    const i64 at = f < 3 ? static_cast<i64>(f) * 10 : 30;
    events.push_back(zone("cull", at + 1, at + 2 + static_cast<i64>(f)));
  }
  events.push_back(zone("upload", 12, 15, 1));
  events.push_back(zone("upload", 13, 14, 2));
  // GPU values for frames 0..2 only (frame 3's have not come back); a pass recorded in two parts.
  for (u64 f = 0; f < 3; ++f) {
    events.push_back(value(CaptureTrack::gpu, "visibility", f, 2.0 + static_cast<f64>(f)));
    events.push_back(value(CaptureTrack::gpu, "resolve", f, 1.0));
    events.push_back(value(CaptureTrack::gpu, "resolve", f, 0.5));
  }
  events.push_back(value(CaptureTrack::cpu, "submit", 3, 0.75));
  events.push_back(value(CaptureTrack::gpu, "visibility", 99, 50.0));  // no such frame

  CaptureInfo info;
  info.stopped_ns = 40 * k_ms;
  info.threads = 3;
  const FrameReport r = build_report(events, info);
  CHECK(r.frames == 4);
  CHECK(r.first_frame == 0);
  CHECK(r.last_frame == 3);
  CHECK(r.window_ms == doctest::Approx(40.0));
  CHECK(r.frame.max_ms == doctest::Approx(10.0));
  CHECK(r.frame.total_ms == doctest::Approx(35.0));  // 10 + 10 + 5 + 10: frame 2 ended at 25
  CHECK(r.gpu_frames == 3);
  CHECK(r.cpu_frames == 1);

  const SeriesStats* cull = find(r.zones, "cull");
  REQUIRE(cull != nullptr);
  CHECK(cull->frames == 4);
  CHECK(cull->frames_seen == 4);
  CHECK(cull->median_ms == doctest::Approx(2.5));
  CHECK(cull->max_ms == doctest::Approx(4.0));
  const SeriesStats* upload = find(r.zones, "upload");
  REQUIRE(upload != nullptr);
  CHECK(upload->frames_seen == 1);
  CHECK(upload->count == 2);
  CHECK(upload->max_ms == doctest::Approx(4.0));  // 3 + 1, summed over threads
  CHECK(upload->median_ms == doctest::Approx(0.0));
  const SeriesStats* dispatch = find(r.zones, "dispatch");
  REQUIRE(dispatch != nullptr);
  CHECK(dispatch->frames_seen == 0);
  CHECK(dispatch->outside_count == 1);
  CHECK(dispatch->outside_ms == doctest::Approx(28.0));
  CHECK(std::strcmp(r.zones[0].name, "cull") == 0);  // the largest in-window total first

  const SeriesStats* resolve = find(r.gpu, "resolve");
  REQUIRE(resolve != nullptr);
  CHECK(resolve->frames == 3);  // frame 3 is not a GPU frame
  CHECK(resolve->median_ms == doctest::Approx(1.5));
  CHECK(resolve->count == 6);
  const SeriesStats* visibility = find(r.gpu, "visibility");
  REQUIRE(visibility != nullptr);
  CHECK(visibility->max_ms == doctest::Approx(4.0));  // frame 99 is not in the capture
  CHECK(std::strcmp(r.gpu[0].name, "visibility") == 0);
  REQUIRE(r.cpu.size() == 1);
  CHECK(r.cpu[0].median_ms == doctest::Approx(0.75));

  // The last two frames only, and one series a list.
  ReportOptions options;
  options.window_frames = 2;
  options.max_series = 1;
  const FrameReport w = build_report(events, info, options);
  CHECK(w.frames == 2);
  CHECK(w.first_frame == 2);
  CHECK(w.gpu_frames == 1);
  REQUIRE(w.gpu.size() == 1);
  CHECK(w.gpu[0].median_ms == doctest::Approx(4.0));  // visibility, frame 2
  REQUIRE(w.zones.size() == 1);
  CHECK(std::strcmp(w.zones[0].name, "cull") == 0);
  CHECK(w.series_dropped == 1 + 0 + 2);  // resolve; upload and dispatch
}

TEST_CASE("profiler: a live capture's trace opens as Chrome trace JSON, laid out as it says") {
  profiling::capture_reset();
  REQUIRE(profiling::capture_start());
  ENGINE_PROFILE_THREAD("report main");
  for (u64 f = 0; f < 3; ++f) {
    profiling::capture_frame(f);
    {
      ENGINE_PROFILE_ZONE_NAMED("report tests: frame work");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    profiling::capture_value(CaptureTrack::gpu, "pass \"a\"", f, 0.5);
    profiling::capture_value(CaptureTrack::gpu, "pass b", f, 0.25);
    profiling::capture_frame_end(f);
  }
  profiling::capture_stop();
  CaptureCopy copy;
  copy_capture(copy);
  CHECK(copy.events.size() == 3 * 5);
  REQUIRE(copy.thread_names.size() == 1);
  CHECK(copy.thread_names[0] == "report main");
  const FrameReport report = build_report(copy.events, copy.info);
  CHECK(report.frames == 3);
  REQUIRE(find(report.zones, "report tests: frame work") != nullptr);
  CHECK(find(report.zones, "report tests: frame work")->frames_seen == 3);
  CHECK(find(report.zones, "report tests: frame work")->median_ms >= 0.9);

  test::TempDir dir("profiler-trace");
  const std::string path = dir.path() + "/capture.json";
  std::string error;
  REQUIRE_MESSAGE(write_chrome_trace(copy, path, &error), error);
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  JsonValue json;
  REQUIRE(parse_json(text.str(), json).ok);
  const JsonValue* list = json.find("traceEvents");
  REQUIRE(list != nullptr);
  u32 zones = 0, frames = 0, gpu = 0;
  f64 last_b_end = -1.0;
  for (const JsonValue& e : list->as_array()) {
    const JsonValue* cat = e.find("cat");
    if (cat == nullptr) continue;
    if (cat->as_string() == "zone") ++zones;
    if (cat->as_string() == "frame") ++frames;
    if (cat->as_string() == "gpu") {
      ++gpu;
      if (e.find("name")->as_string() == "pass b") {
        // "pass b" follows "pass \"a\"" from the frame's mark: 0.5 ms in, 0.25 ms long.
        last_b_end = e.find("ts")->as_float() + e.find("dur")->as_float();
      }
    }
  }
  CHECK(zones == 3);
  CHECK(frames == 3);
  CHECK(gpu == 6);
  CHECK(last_b_end > 0.0);
  const JsonValue* other = json.find("otherData");
  REQUIRE(other != nullptr);
  CHECK(other->find("format")->as_string() == "engine.frame_capture.v1");
  f64 dropped = -1.0;
  CHECK(other->find("dropped_events")->get_f64(dropped));
  CHECK(dropped == 0.0);
  profiling::capture_reset();
}

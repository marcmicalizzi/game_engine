#include <core/json/json.h>
#include <core/profiling/profile.h>
#include <foundation/profiler/frame_report.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
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

// Found by batch 68's review of the capture: the end match and the name lookup became searches;
// what they find must be what the walks found. An end goes to the latest mark of its index at or
// before it, a repeated index included, an end with no such mark is ignored, and one name met
// through two pointers is one series.
TEST_CASE("profiler: ends find their marks and names their series, as the walks did") {
  char first[] = "decode";
  char second[] = "decode";  // the same name at another address
  std::vector<CaptureEvent> events;
  events.push_back(mark(5, 0));
  events.push_back(mark(9, 10));
  events.push_back(mark(5, 20));      // index 5 again: a host numbering by something else
  events.push_back(mark_end(5, 26));  // the second 5's end, not the first's
  events.push_back(mark_end(9, 13));
  events.push_back(mark_end(5, 4));   // the first 5's: the latest 5 at or before 4 ms
  events.push_back(mark_end(7, 15));  // no mark 7: ignored
  events.push_back(zone(first, 1, 3));
  events.push_back(zone(second, 11, 12));
  events.push_back(zone(first, 21, 25));
  CaptureInfo info;
  info.stopped_ns = 40 * k_ms;
  const FrameReport r = build_report(events, info, ReportOptions{});
  REQUIRE(r.frames == 3);
  CHECK(r.frame.max_ms == doctest::Approx(6.0));     // 20..26
  CHECK(r.frame.median_ms == doctest::Approx(4.0));  // 0..4, 10..13
  CHECK(r.frame.total_ms == doctest::Approx(13.0));
  REQUIRE(r.zones.size() == 1);
  CHECK(r.zones[0].count == 3);
  CHECK(r.zones[0].frames_seen == 3);
  CHECK(r.zones[0].total_ms == doctest::Approx(7.0));
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

// A capture whose buffer fills part-way through a frame, stopped long after (profiler.md, "A
// capture that filled"): the frame in progress lost its end, and before the mend it was reported
// as lasting until the stop, so the frame series' max, p99, mean and window held the whole
// unrecorded stretch. Twelve events a frame into 64 slots: frames 1 to 5 whole (60 events), frame
// 6's mark, its two values and one zone kept, then the buffer is full.
TEST_CASE("profiler: a capture that filled ends its report at the last whole frame") {
  profiling::capture_reset();
  profiling::CaptureOptions options;
  options.capacity = 64;
  REQUIRE(profiling::capture_start(options));
  for (u64 f = 1; f <= 12; ++f) {
    profiling::capture_frame(f);
    profiling::capture_value(CaptureTrack::cpu, "filled: a", f, 1.0);
    profiling::capture_value(CaptureTrack::cpu, "filled: b", f, 1.0);
    for (int z = 0; z < 8; ++z) {
      ENGINE_PROFILE_ZONE_NAMED("filled: work");
    }
    profiling::capture_frame_end(f);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(600));  // the stretch nobody recorded
  profiling::capture_stop();
  CaptureCopy copy;
  copy_capture(copy);
  CHECK(copy.events.size() == 64);
  CHECK(copy.info.dropped == 12 * 12 - 64);
  CHECK(copy.info.full_ns >= 0);
  CHECK(copy.info.full_ns <= copy.info.stopped_ns);
  const FrameReport report = build_report(copy.events, copy.info);
  CHECK(report.truncated);
  CHECK(report.truncated_in_frame);
  CHECK(report.truncated_frame == 6);
  CHECK(report.truncated_ms == doctest::Approx(static_cast<f64>(copy.info.full_ns) * 1e-6));
  CHECK(report.frames == 5);
  CHECK(report.last_frame == 5);
  // Frames of a few microseconds each: nothing near the 600 ms after the buffer filled.
  CHECK(report.frame.max_ms < 250.0);
  CHECK(report.frame.p99_ms < 250.0);
  CHECK(report.window_ms < 250.0);
  CHECK(report.cpu_frames == 5);
  REQUIRE(find(report.cpu, "filled: a") != nullptr);
  CHECK(find(report.cpu, "filled: a")->frames == 5);
  CHECK(find(report.cpu, "filled: a")->count == 5);
  CHECK(find(report.cpu, "filled: b")->median_ms == doctest::Approx(1.0));
  // Frame 6's one kept zone belongs to no whole frame: not counted inside a frame or outside.
  REQUIRE(find(report.zones, "filled: work") != nullptr);
  CHECK(find(report.zones, "filled: work")->count == 5 * 8);
  CHECK(find(report.zones, "filled: work")->outside_count == 0);
  const std::string trace = chrome_trace_json(copy);
  JsonValue json;
  REQUIRE(parse_json(trace, json).ok);
  CHECK(trace.find("\"truncated\":true") != std::string::npos);
  CHECK(trace.find("\"truncated_frame\":6") != std::string::npos);
  CHECK(trace.find("\"name\":\"frame 6\"") == std::string::npos);
  profiling::capture_reset();
}

// The same rules over hand-made events: a capture that filled between frames keeps both whole
// frames, but its GPU track's last frame lost one of its two passes, so that frame is not a
// measured GPU frame (counting it would give pass b a zero there); and values that are not a
// time — NaN, an infinity, 1e300 ms — are counted and left out of the report and the trace.
TEST_CASE("profiler: a filled capture's last value burst and values that are not a time") {
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  const f64 inf = std::numeric_limits<f64>::infinity();
  CaptureEvent a1 = value(CaptureTrack::gpu, "a", 1, 2.0),
               b1 = value(CaptureTrack::gpu, "b", 1, 3.0);
  CaptureEvent a2 = value(CaptureTrack::gpu, "a", 2, 2.0);
  a1.begin_ns = b1.begin_ns = 11 * k_ms;
  a2.begin_ns = 21 * k_ms;
  CaptureEvent bad = value(CaptureTrack::cpu, "bad", 1, nan);
  CaptureEvent big = value(CaptureTrack::cpu, "big", 1, 1e300);
  CaptureEvent endless = value(CaptureTrack::cpu, "endless", 1, -inf);
  const CaptureEvent events[] = {
      mark(1, 0), mark_end(1, 10), mark(2, 10), mark_end(2, 20), a1, b1, a2, bad, big, endless};
  CaptureInfo info;
  info.stopped_ns = 1000 * k_ms;
  info.dropped = 1;
  info.full_ns = 22 * k_ms;
  const FrameReport report = build_report(events, info);
  CHECK(report.truncated);
  CHECK_FALSE(report.truncated_in_frame);
  CHECK(report.truncated_ms == doctest::Approx(22.0));
  CHECK(report.frames == 2);
  CHECK(report.frame.max_ms == doctest::Approx(10.0));
  CHECK(report.gpu_frames == 1);
  REQUIRE(find(report.gpu, "b") != nullptr);
  CHECK(find(report.gpu, "b")->frames == 1);
  CHECK(find(report.gpu, "b")->median_ms == doctest::Approx(3.0));
  CHECK(report.invalid_values == 3);
  CHECK(report.cpu.size() == 0);
  CaptureCopy copy;
  for (const CaptureEvent& e : events)
    copy.events.push_back(e);
  copy.info = info;
  const std::string trace = chrome_trace_json(copy);
  JsonValue json;
  CHECK(parse_json(trace, json).ok);
  CHECK(trace.find("nan") == std::string::npos);
  CHECK(trace.find("inf\"") == std::string::npos);
  CHECK(trace.find("\"bad\"") == std::string::npos);
  CHECK(trace.find("\"big\"") == std::string::npos);
}

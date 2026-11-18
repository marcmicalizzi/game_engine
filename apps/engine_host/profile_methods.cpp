#include "profile_methods.h"

#include <core/profiling/frame_capture.h>
#include <foundation/io/vfs.h>
#include <foundation/profiler/frame_report.h>

#include <schemas/protocol.h>
#include <string>
#include <string_view>

namespace engine::host {

namespace {

// The bounds a call may ask for: a report is at most three lists of `k_max_series` series (about
// 200 bytes each as JSON, so under 40 KB), and a capture at most `k_max_capacity` events (48 bytes
// each and a word: about 220 MiB, a minute of a heavy frame loop's zones).
constexpr u32 k_max_series = 64;
constexpr u32 k_max_capacity = 1u << 22;

// The number of the capture whose events are in the buffer, the running one or the last one
// stopped; 0 before the first. The buffer holds one capture: a start clears it, so a client whose
// capture another client stopped and replaced is told so by number (protocol.md, "Two clients and
// one capture") rather than handed the other's report.
u64 g_capture = 0;

bool same_capture(const protocol::ProfileReportParams& params, protocol::RpcError& error) {
  if (params.capture == 0 || params.capture == g_capture) return true;
  error = protocol::make_error(
      protocol::codes::k_not_found,
      "capture " + std::to_string(params.capture) +
          (params.capture < g_capture ? " was replaced by capture " + std::to_string(g_capture) +
                                            " (another profile.start): its events are gone"
                                      : " has not been started"));
  return false;
}

void series_of(const profiler::SeriesStats& s, protocol::ProfileSeries& out) {
  out.name = s.name;
  out.frames = s.frames;
  out.frames_seen = s.frames_seen;
  out.count = s.count;
  out.median_ms = s.median_ms;
  out.p95_ms = s.p95_ms;
  out.p99_ms = s.p99_ms;
  out.max_ms = s.max_ms;
  out.mean_ms = s.mean_ms;
  out.total_ms = s.total_ms;
  out.outside_count = s.outside_count;
  out.outside_ms = s.outside_ms;
}

void list_of(const Vector<profiler::SeriesStats>& in, Vector<protocol::ProfileSeries>& out) {
  out.clear();
  for (const profiler::SeriesStats& s : in)
    series_of(s, out.emplace_back());
}

bool name_is_plain(std::string_view name) noexcept {
  if (name.empty() || name == "." || name == "..") return false;
  for (const char c : name)
    if (c == '/' || c == '\\' || c == ':') return false;
  return true;
}

// The parameters' bounds, checked before anything is stopped or read.
bool valid(const protocol::ProfileReportParams& params, protocol::RpcError& error) {
  if (params.max_series == 0 || params.max_series > k_max_series) {
    error = protocol::make_error(protocol::codes::k_invalid_argument,
                                 "max_series must be 1 to " + std::to_string(k_max_series));
    return false;
  }
  if (!params.out_dir.empty() && !name_is_plain(params.name)) {
    error = protocol::make_error(protocol::codes::k_invalid_argument,
                                 "name must be a plain file name, with no directory in it");
    return false;
  }
  return true;
}

// The report over the capture as it stands, and its trace when the call names a directory.
bool report(const protocol::ProfileReportParams& params, protocol::ProfileReport& out,
            protocol::RpcError& error) {
  if (g_capture == 0) {
    error = protocol::make_error(protocol::codes::k_invalid_argument,
                                 "no capture: profile.start one, draw frames, then ask");
    return false;
  }
  if (!valid(params, error) || !same_capture(params, error)) return false;
  profiler::CaptureCopy copy;
  profiler::copy_capture(copy);
  profiler::ReportOptions options;
  options.window_frames = params.window_frames;
  options.max_series = params.max_series;
  const profiler::FrameReport r = profiler::build_report(
      std::span<const profiling::CaptureEvent>(copy.events.data(), copy.events.size()), copy.info,
      options);
  out.format = "engine.profile.v1";
  out.running = r.capture_running;
  out.capture = g_capture;
  out.frames = r.frames;
  out.first_frame = r.first_frame;
  out.last_frame = r.last_frame;
  out.window_ms = r.window_ms;
  series_of(r.frame, out.frame);
  list_of(r.gpu, out.gpu);
  list_of(r.cpu, out.cpu);
  list_of(r.zones, out.zones);
  out.gpu_frames = r.gpu_frames;
  out.cpu_frames = r.cpu_frames;
  out.series_dropped = r.series_dropped;
  out.events = r.events;
  out.dropped_events = r.dropped_events;
  out.threads = r.threads;
  out.capture_ms = r.capture_ms;
  out.trace.clear();
  // The trace is written as render.capture and render.compare write theirs (render_methods.cpp):
  // through foundation/io, refused with k_io_error naming the path and the status.
  if (!params.out_dir.empty()) {
    io::Status status = io::make_directories(params.out_dir);
    if (status != io::Status::Ok) {
      error = protocol::make_error(protocol::codes::k_io_error, "cannot create " + params.out_dir +
                                                                    ": " + io::status_name(status));
      return false;
    }
    const std::string path = io::join_path(params.out_dir, params.name + ".trace.json");
    status = io::write_file(path, profiler::chrome_trace_json(copy));
    if (status != io::Status::Ok) {
      error = protocol::make_error(protocol::codes::k_io_error,
                                   "cannot write " + path + ": " + io::status_name(status));
      return false;
    }
    out.trace = path;
  }
  return true;
}

bool profile_start(protocol::Context&, const protocol::ProfileStartParams& params,
                   protocol::ProfileStartResult& out, protocol::RpcError& error) {
  if (profiling::capture_enabled()) {
    error = protocol::make_error(protocol::codes::k_invalid_argument,
                                 "a capture is already running: profile.stop it first");
    return false;
  }
  if (params.capacity > k_max_capacity) {
    error = protocol::make_error(
        protocol::codes::k_invalid_argument,
        "capacity must be at most " + std::to_string(k_max_capacity) + " events");
    return false;
  }
  profiling::CaptureOptions options;
  if (params.capacity != 0) options.capacity = params.capacity;
  if (!profiling::capture_start(options)) {
    error = protocol::make_error(protocol::codes::k_invalid_argument,
                                 "a capture is already running: profile.stop it first");
    return false;
  }
  ++g_capture;
  out.running = true;
  out.capacity = options.capacity;
  out.capture = g_capture;
  return true;
}

bool profile_report(protocol::Context&, const protocol::ProfileReportParams& params,
                    protocol::ProfileReport& out, protocol::RpcError& error) {
  return report(params, out, error);
}

bool profile_stop(protocol::Context&, const protocol::ProfileReportParams& params,
                  protocol::ProfileReport& out, protocol::RpcError& error) {
  // A capture named and replaced is that answer, before "nothing running" can hide it.
  if (!same_capture(params, error)) return false;
  if (!profiling::capture_enabled()) {
    error = protocol::make_error(protocol::codes::k_invalid_argument,
                                 "no capture is running: profile.start one first");
    return false;
  }
  if (!valid(params, error)) return false;  // a refused call leaves the capture running
  profiling::capture_stop();
  return report(params, out, error);
}

}  // namespace

void add_profile_methods(protocol::Dispatcher& d) {
  // Read-only: a capture changes no document, lease or host setting — it records what the host
  // does while it is on, as `render.capture` records a frame — so every role may profile.
  d.add(protocol::read_only(
      protocol::method<protocol::ProfileStartParams, protocol::ProfileStartResult, &profile_start>(
          "profile.start",
          "Start the frame capture: every frame this host draws from now on (render.capture, "
          "render.benchmark), its GPU passes, its CPU spans and the zones of every thread, until "
          "profile.stop. One capture a process.")));
  d.add(protocol::read_only(
      protocol::method<protocol::ProfileReportParams, protocol::ProfileReport, &profile_report>(
          "profile.report",
          "The frame report of the running capture, or of the last one stopped: per GPU pass, CPU "
          "span and zone, the median, p95, p99 and max milliseconds a frame over a window, at most "
          "max_series of each; with out_dir, the trace beside it for Perfetto.")));
  d.add(protocol::read_only(
      protocol::method<protocol::ProfileReportParams, protocol::ProfileReport, &profile_stop>(
          "profile.stop",
          "Stop the frame capture and return its frame report (profile.report's), with the trace "
          "written into out_dir when one is named.")));
}

}  // namespace engine::host

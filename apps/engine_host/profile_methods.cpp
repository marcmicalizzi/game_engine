#include "profile_methods.h"

#include <core/profiling/frame_capture.h>
#include <domain/protocol/paths.h>
#include <foundation/io/roots.h>
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

// The parameters' bounds, and where the trace goes, checked before anything is stopped or read: a
// trace is written only under a writable root (protocol.md, "The host's file roots"), `out_dir`
// admitted for making a directory and the file itself for writing, so a link at its name is
// refused. `dir` is left empty when the call names no directory (no trace is written).
bool valid(protocol::Context& ctx, const protocol::ProfileReportParams& params, io::Admitted& dir,
           protocol::RpcError& error) {
  if (params.max_series == 0 || params.max_series > k_max_series) {
    error = protocol::make_error(protocol::codes::k_invalid_argument,
                                 "max_series must be 1 to " + std::to_string(k_max_series));
    return false;
  }
  dir = io::Admitted{};
  if (params.out_dir.empty()) return true;
  if (!protocol::is_plain_file_name(params.name)) {
    error = protocol::make_error(
        protocol::codes::k_invalid_argument,
        "name must be one plain file name, with no directory, '..' or device name");
    return false;
  }
  return protocol::admit_client_path(ctx, "out_dir", params.out_dir, protocol::client_base(ctx, {}),
                                     io::Access::make_directory, dir, error);
}

// The report over the capture as it stands, and its trace when the call names a directory.
bool report(protocol::Context& ctx, const protocol::ProfileReportParams& params,
            const io::Admitted& dir, protocol::ProfileReport& out, protocol::RpcError& error) {
  if (g_capture == 0) {
    error = protocol::make_error(protocol::codes::k_invalid_argument,
                                 "no capture: profile.start one, draw frames, then ask");
    return false;
  }
  if (!same_capture(params, error)) return false;
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
  out.truncated = r.truncated;
  out.truncated_ms = r.truncated_ms;
  out.truncated_in_frame = r.truncated_in_frame;
  out.truncated_frame = r.truncated_frame;
  out.partial_frames = r.partial_frames;
  out.partial_first_frame = r.partial_first_frame;
  out.invalid_values = r.invalid_values;
  out.threads = r.threads;
  out.capture_ms = r.capture_ms;
  out.trace.clear();
  // The trace goes to the admitted file, replaced by a rename so a hard link there is never
  // written through; a failure on the disk is k_io_error naming what the client named, and the
  // answer is the portable path, never the host's (protocol.md, "The host's file roots").
  if (!dir.native.empty()) {
    const std::string file = params.name + ".trace.json";
    io::Status status = io::make_directories(dir.native);
    io::Admitted path;
    if (status == io::Status::Ok &&
        !protocol::admit_output_file(ctx, "out_dir", params.out_dir, dir, file, path, error))
      return false;
    if (status == io::Status::Ok)
      status = io::write_file_atomic(path.native, profiler::chrome_trace_json(copy));
    if (status != io::Status::Ok) {
      error = protocol::make_error(
          protocol::codes::k_io_error,
          "cannot write " + params.out_dir + "/" + file + ": " + io::status_name(status));
      return false;
    }
    out.trace = protocol::portable_path(ctx, path.native);
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
  // The buffer's own size, which is what the capture drops at, not the size asked for.
  out.capacity = profiling::capture_capacity();
  out.capture = g_capture;
  return true;
}

bool profile_report(protocol::Context& ctx, const protocol::ProfileReportParams& params,
                    protocol::ProfileReport& out, protocol::RpcError& error) {
  io::Admitted dir;
  return valid(ctx, params, dir, error) && report(ctx, params, dir, out, error);
}

bool profile_stop(protocol::Context& ctx, const protocol::ProfileReportParams& params,
                  protocol::ProfileReport& out, protocol::RpcError& error) {
  // A capture named and replaced is that answer, before "nothing running" can hide it.
  if (!same_capture(params, error)) return false;
  if (!profiling::capture_enabled()) {
    error = protocol::make_error(protocol::codes::k_invalid_argument,
                                 "no capture is running: profile.start one first");
    return false;
  }
  io::Admitted dir;
  if (!valid(ctx, params, dir, error)) return false;  // a refused call leaves the capture running
  profiling::capture_stop();
  return report(ctx, params, dir, out, error);
}

// A report leaves a file when it names a directory for its trace (protocol.md, "What a read may
// do").
bool report_leaves_files(const JsonValue& params) {
  return protocol::param_is_named(params, "out_dir");
}

}  // namespace

void add_profile_methods(protocol::Dispatcher& d) {
  // Starting and stopping are writes: the capture is one switch for the whole process, every frame
  // and thread pays for it while it is on, and a stop ends whichever client's capture is running.
  // A role that profiles names them (content/roles/roles.json: qa, performance). The report reads.
  d.add(
      protocol::method<protocol::ProfileStartParams, protocol::ProfileStartResult, &profile_start>(
          "profile.start",
          "Start the frame capture: every frame this host draws from now on (render.capture, "
          "render.benchmark), its GPU passes, its CPU spans and the zones of every thread, until "
          "profile.stop. One capture a process."));
  d.add(protocol::read_leaving_files(
      protocol::method<protocol::ProfileReportParams, protocol::ProfileReport, &profile_report>(
          "profile.report",
          "The frame report of the running capture, or of the last one stopped: per GPU pass, CPU "
          "span and zone, the median, p95, p99 and max milliseconds a frame over a window, at most "
          "max_series of each; with out_dir, the trace beside it for Perfetto."),
      &report_leaves_files));
  d.add(protocol::method<protocol::ProfileReportParams, protocol::ProfileReport, &profile_stop>(
      "profile.stop",
      "Stop the frame capture and return its frame report (profile.report's), with the trace "
      "written into out_dir when one is named."));
}

}  // namespace engine::host

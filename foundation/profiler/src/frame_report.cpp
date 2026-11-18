#include <foundation/profiler/frame_report.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string_view>

namespace engine::profiler {

namespace {

constexpr f64 k_ms_per_ns = 1e-6;

// One frame of the capture: its mark's index and its interval, nanoseconds from the origin.
struct Frame {
  u64 index = 0;
  i64 begin_ns = 0;
  i64 end_ns = 0;
};

// One name while the report is built: its per-frame sums in `sums` (one per window frame).
struct Accumulator {
  const char* name = "";
  Vector<f64> sums;
  Vector<u8> seen;
  u64 count = 0;
  u64 outside_count = 0;
  f64 outside_ms = 0.0;
};

bool same_name(const char* a, const char* b) noexcept { return a == b || std::strcmp(a, b) == 0; }

Accumulator& find_or_add(Vector<Accumulator>& list, const char* name, u32 frames) {
  for (Accumulator& a : list)
    if (same_name(a.name, name)) return a;
  Accumulator& a = list.emplace_back();
  a.name = name;
  a.sums.resize(frames, 0.0);
  a.seen.resize(frames, 0);
  return a;
}

// The window frame holding `ns`, or `~0u`: frames are sorted by begin and do not overlap.
u32 frame_at(const Vector<Frame>& frames, i64 ns) noexcept {
  u32 lo = 0, hi = frames.size();
  while (lo < hi) {
    const u32 mid = lo + (hi - lo) / 2;
    if (frames[mid].begin_ns <= ns)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo == 0) return ~0u;
  const Frame& f = frames[lo - 1];
  return ns < f.end_ns ? lo - 1 : ~0u;
}

// The frames by their marks' indices, for a value's lookup: a capture holds thousands of frames
// and a value names one, so the lookup is a binary search, not a walk. A repeated index (a host
// that numbers frames by something other than submission) names its latest mark.
class FrameIndex {
 public:
  explicit FrameIndex(const Vector<Frame>& frames) {
    for (u32 i = 0; i < frames.size(); ++i) entries_.push_back({frames[i].index, i});
    std::stable_sort(entries_.begin(), entries_.end(),
                     [](const Entry& a, const Entry& b) { return a.index < b.index; });
  }
  // The frame with mark `index`, or `~0u`.
  u32 find(u64 index) const noexcept {
    const Entry* end = entries_.data() + entries_.size();
    const Entry* at = std::upper_bound(entries_.data(), end, index,
                                       [](u64 v, const Entry& e) { return v < e.index; });
    if (at == entries_.data() || (at - 1)->index != index) return ~0u;
    return (at - 1)->at;
  }

 private:
  struct Entry {
    u64 index = 0;
    u32 at = 0;
  };
  Vector<Entry> entries_;
};

// `list` summarized over the frames `measured` marks (all of them when null), sorted by total,
// largest first, and cut to `max`.
void summarize(Vector<Accumulator>& list, const Vector<u8>* measured, u32 frames, u32 max,
               Vector<SeriesStats>& out, u32& dropped) {
  Vector<f64> values;
  for (Accumulator& a : list) {
    values.clear();
    SeriesStats s;
    s.name = a.name;
    for (u32 f = 0; f < frames; ++f) {
      if (measured != nullptr && (*measured)[f] == 0) continue;
      values.push_back(a.sums[f]);
      s.total_ms += a.sums[f];
      if (a.seen[f] != 0) ++s.frames_seen;
    }
    s.frames = values.size();
    s.count = a.count;
    s.outside_count = a.outside_count;
    s.outside_ms = a.outside_ms;
    const Percentiles p = percentiles(std::span<f64>(values.data(), values.size()));
    s.median_ms = p.median;
    s.p95_ms = p.p95;
    s.p99_ms = p.p99;
    s.max_ms = p.max;
    s.mean_ms = p.mean;
    out.push_back(s);
  }
  std::stable_sort(out.begin(), out.end(), [](const SeriesStats& x, const SeriesStats& y) {
    if (x.total_ms != y.total_ms) return x.total_ms > y.total_ms;
    if (x.outside_ms != y.outside_ms) return x.outside_ms > y.outside_ms;
    return std::strcmp(x.name, y.name) < 0;
  });
  if (out.size() > max) {
    dropped += out.size() - max;
    out.resize(max);
  }
}

// The capture's frames, sorted by their marks, each ended by its end event, the next mark or the
// capture's stop.
Vector<Frame> frames_of(std::span<const CaptureEvent> events, const CaptureInfo& info) {
  Vector<Frame> frames;
  for (const CaptureEvent& e : events)
    if (e.kind == profiling::CaptureKind::frame) frames.push_back({e.frame, e.begin_ns, -1});
  std::stable_sort(frames.begin(), frames.end(),
                   [](const Frame& a, const Frame& b) { return a.begin_ns < b.begin_ns; });
  for (const CaptureEvent& e : events) {
    if (e.kind != profiling::CaptureKind::frame_end) continue;
    // The latest mark of that index that began at or before the end.
    for (u32 i = frames.size(); i-- > 0;) {
      if (frames[i].index == e.frame && frames[i].begin_ns <= e.begin_ns) {
        if (frames[i].end_ns < 0 || e.begin_ns < frames[i].end_ns) frames[i].end_ns = e.begin_ns;
        break;
      }
    }
  }
  for (u32 i = 0; i < frames.size(); ++i) {
    const i64 next = i + 1 < frames.size() ? frames[i + 1].begin_ns : info.stopped_ns;
    if (frames[i].end_ns < 0 || frames[i].end_ns > next) frames[i].end_ns = next;
    frames[i].end_ns = std::max(frames[i].end_ns, frames[i].begin_ns);
  }
  return frames;
}

void append_escaped(std::string& out, std::string_view text) {
  out += '"';
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

void append_u64(std::string& out, u64 v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%" PRIu64, v);
  out += buf;
}

// Microseconds with nanosecond precision, the trace format's unit.
void append_us(std::string& out, i64 ns) {
  char buf[32];
  const i64 whole = ns / 1000;
  i64 frac = ns % 1000;
  if (frac < 0) frac = -frac;
  std::snprintf(buf, sizeof(buf), "%" PRId64 ".%03" PRId64, whole, frac);
  out += buf;
}

void append_f64(std::string& out, f64 v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.6g", v);
  out += buf;
}

constexpr u32 k_tid_frames = 100000;
constexpr u32 k_tid_gpu = 100001;
constexpr u32 k_tid_cpu = 100002;

}  // namespace

Percentiles percentiles(std::span<f64> values) {
  Percentiles p;
  if (values.empty()) return p;
  std::sort(values.begin(), values.end());
  auto at = [&](f64 q) {
    const f64 rank = q * static_cast<f64>(values.size() - 1);
    const usize lo = static_cast<usize>(rank);
    const usize hi = std::min(lo + 1, values.size() - 1);
    const f64 t = rank - static_cast<f64>(lo);
    return values[lo] + (values[hi] - values[lo]) * t;
  };
  p.median = at(0.5);
  p.p95 = at(0.95);
  p.p99 = at(0.99);
  p.max = values.back();
  f64 sum = 0.0;
  for (const f64 v : values)
    sum += v;
  p.mean = sum / static_cast<f64>(values.size());
  return p;
}

void copy_capture(CaptureCopy& out) {
  out.events.clear();
  out.events.resize(profiling::capture_count() + 64);
  const u32 n = profiling::capture_snapshot(
      std::span<CaptureEvent>(out.events.data(), out.events.size()), out.info);
  out.events.resize(n);
  out.thread_names.clear();
  for (u32 t = 0; t < out.info.threads; ++t)
    out.thread_names.push_back(std::string(profiling::capture_thread_name(t)));
}

FrameReport build_report(std::span<const CaptureEvent> events, const CaptureInfo& info,
                         const ReportOptions& options) {
  FrameReport report;
  report.events = events.size();
  report.dropped_events = info.dropped;
  report.threads = info.threads;
  report.capture_ms = static_cast<f64>(info.stopped_ns) * k_ms_per_ns;
  report.capture_running = info.running;
  report.frame.name = "frame";

  Vector<Frame> all = frames_of(events, info);
  Vector<Frame> frames;
  const u32 first = options.window_frames != 0 && all.size() > options.window_frames
                        ? all.size() - options.window_frames
                        : 0;
  for (u32 i = first; i < all.size(); ++i)
    frames.push_back(all[i]);
  const u32 n = frames.size();
  report.frames = n;
  if (n != 0) {
    report.first_frame = frames[0].index;
    report.last_frame = frames[n - 1].index;
    report.window_ms = static_cast<f64>(frames[n - 1].end_ns - frames[0].begin_ns) * k_ms_per_ns;
  }

  Vector<Accumulator> gpu, cpu, zones;
  Vector<u8> gpu_measured, cpu_measured;
  gpu_measured.resize(n, 0);
  cpu_measured.resize(n, 0);
  Vector<f64> frame_ms;
  for (const Frame& f : frames)
    frame_ms.push_back(static_cast<f64>(f.end_ns - f.begin_ns) * k_ms_per_ns);
  const FrameIndex by_index(frames);

  for (const CaptureEvent& e : events) {
    if (e.kind == profiling::CaptureKind::zone) {
      const u32 f = frame_at(frames, e.begin_ns);
      Accumulator& a = find_or_add(zones, e.name, n);
      const f64 ms = static_cast<f64>(e.duration_ns()) * k_ms_per_ns;
      if (f == ~0u) {
        ++a.outside_count;
        a.outside_ms += ms;
      } else {
        ++a.count;
        a.sums[f] += ms;
        a.seen[f] = 1;
      }
    } else if (e.kind == profiling::CaptureKind::value) {
      const u32 f = by_index.find(e.frame);
      if (f == ~0u) continue;  // a frame outside the window
      const bool on_gpu = e.track == profiling::CaptureTrack::gpu;
      Accumulator& a = find_or_add(on_gpu ? gpu : cpu, e.name, n);
      ++a.count;
      a.sums[f] += e.ms;
      a.seen[f] = 1;
      (on_gpu ? gpu_measured : cpu_measured)[f] = 1;
    }
  }
  for (u32 f = 0; f < n; ++f) {
    report.gpu_frames += gpu_measured[f];
    report.cpu_frames += cpu_measured[f];
  }

  {
    SeriesStats& s = report.frame;
    s.frames = n;
    s.frames_seen = n;
    s.count = n;
    for (const f64 v : frame_ms)
      s.total_ms += v;
    const Percentiles p = percentiles(std::span<f64>(frame_ms.data(), frame_ms.size()));
    s.median_ms = p.median;
    s.p95_ms = p.p95;
    s.p99_ms = p.p99;
    s.max_ms = p.max;
    s.mean_ms = p.mean;
  }
  summarize(gpu, &gpu_measured, n, options.max_series, report.gpu, report.series_dropped);
  summarize(cpu, &cpu_measured, n, options.max_series, report.cpu, report.series_dropped);
  summarize(zones, nullptr, n, options.max_series, report.zones, report.series_dropped);
  return report;
}

std::string chrome_trace_json(const CaptureCopy& capture) {
  const std::span<const CaptureEvent> events(capture.events.data(), capture.events.size());
  std::string out;
  out.reserve(256 + events.size() * 120);
  out += "{\"traceEvents\":[\n";
  bool first = true;
  auto sep = [&] {
    if (!first) out += ",\n";
    first = false;
  };
  auto thread_meta = [&](u32 tid, std::string_view name) {
    sep();
    out += "{\"ph\":\"M\",\"name\":\"thread_name\",\"pid\":1,\"tid\":";
    append_u64(out, tid);
    out += ",\"args\":{\"name\":";
    append_escaped(out, name);
    out += "}}";
  };
  sep();
  out += "{\"ph\":\"M\",\"name\":\"process_name\",\"pid\":1,\"args\":{\"name\":\"engine\"}}";
  for (u32 t = 0; t < capture.info.threads; ++t) {
    const bool named = t < capture.thread_names.size() && !capture.thread_names[t].empty();
    thread_meta(t, named ? std::string_view(capture.thread_names[t])
                         : std::string_view(t == 0 ? "first thread" : "thread"));
  }
  thread_meta(k_tid_frames, "frames");
  thread_meta(k_tid_gpu, "GPU passes (durations laid end to end from the frame's mark)");
  thread_meta(k_tid_cpu, "CPU timers (durations laid end to end from the frame's mark)");

  for (const CaptureEvent& e : events) {
    if (e.kind != profiling::CaptureKind::zone) continue;
    sep();
    out += "{\"ph\":\"X\",\"cat\":\"zone\",\"name\":";
    append_escaped(out, e.name);
    out += ",\"pid\":1,\"tid\":";
    append_u64(out, e.thread);
    out += ",\"ts\":";
    append_us(out, e.begin_ns);
    out += ",\"dur\":";
    append_us(out, e.duration_ns());
    out += "}";
  }
  const Vector<Frame> frames = frames_of(events, capture.info);
  for (const Frame& f : frames) {
    sep();
    out += "{\"ph\":\"X\",\"cat\":\"frame\",\"name\":\"frame ";
    append_u64(out, f.index);
    out += "\",\"pid\":1,\"tid\":";
    append_u64(out, k_tid_frames);
    out += ",\"ts\":";
    append_us(out, f.begin_ns);
    out += ",\"dur\":";
    append_us(out, f.end_ns - f.begin_ns);
    out += ",\"args\":{\"frame\":";
    append_u64(out, f.index);
    out += "}}";
  }
  // Values: each frame's laid end to end from its mark, per track, in recorded order. A value
  // whose frame has no mark starts from when it was recorded.
  struct Cursor {
    u64 frame = 0;
    i64 at_ns = 0;
  };
  const FrameIndex by_index(frames);
  Vector<Cursor> marked[2];     // by frame position: where the frame's next value goes
  Vector<Cursor> unmarked[2];   // values of a frame with no mark: few, so a walk
  for (u32 t = 0; t < 2; ++t) {
    marked[t].resize(frames.size());
    for (u32 f = 0; f < frames.size(); ++f) marked[t][f] = {frames[f].index, frames[f].begin_ns};
  }
  for (const CaptureEvent& e : events) {
    if (e.kind != profiling::CaptureKind::value) continue;
    const u32 track = e.track == profiling::CaptureTrack::gpu ? 0u : 1u;
    Cursor* cursor = nullptr;
    const u32 f = by_index.find(e.frame);
    if (f != ~0u) {
      cursor = &marked[track][f];
    } else {
      for (Cursor& c : unmarked[track])
        if (c.frame == e.frame) cursor = &c;
      if (cursor == nullptr) cursor = &unmarked[track].push_back({e.frame, e.begin_ns});
    }
    const i64 dur_ns = static_cast<i64>(e.ms * 1e6 + 0.5);
    sep();
    out += track == 0 ? "{\"ph\":\"X\",\"cat\":\"gpu\",\"name\":"
                      : "{\"ph\":\"X\",\"cat\":\"cpu\",\"name\":";
    append_escaped(out, e.name);
    out += ",\"pid\":1,\"tid\":";
    append_u64(out, track == 0 ? k_tid_gpu : k_tid_cpu);
    out += ",\"ts\":";
    append_us(out, cursor->at_ns);
    out += ",\"dur\":";
    append_us(out, dur_ns);
    out += ",\"args\":{\"frame\":";
    append_u64(out, e.frame);
    out += ",\"ms\":";
    append_f64(out, e.ms);
    out += "}}";
    cursor->at_ns += dur_ns;
  }
  out += "\n],\"displayTimeUnit\":\"ms\",\"otherData\":{\"format\":\"engine.frame_capture.v1\",";
  out += "\"capture_ms\":";
  append_f64(out, static_cast<f64>(capture.info.stopped_ns) * k_ms_per_ns);
  out += ",\"dropped_events\":";
  append_u64(out, capture.info.dropped);
  out += ",\"running\":";
  out += capture.info.running ? "true" : "false";
  out += "}}\n";
  return out;
}

bool write_chrome_trace(const CaptureCopy& capture, const std::string& path, std::string* error) {
  const std::string text = chrome_trace_json(capture);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    if (error != nullptr) *error = "cannot open " + path + " for writing";
    return false;
  }
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  file.close();
  if (!file) {
    if (error != nullptr) *error = "cannot write " + path;
    return false;
  }
  return true;
}

}  // namespace engine::profiler

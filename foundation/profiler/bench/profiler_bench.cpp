// What the frame capture costs while it is on, and what reading one into a report costs
// (docs/subsystems/profiler.md, "What it costs"; roadmap A10, batch 68's review C3 and C5).
//
// - `profiler.capture.zone`: one zone recorded on the timed thread while `arg - 1` other threads
//   record zones as fast as they can: the event path's cost, and how much of it is threads meeting
//   on a shared counter. The capture is restarted before it fills, so every zone is written.
// - `profiler.report.build`: a stopped capture of `arg` frames, each a mark, an end, ten zones of
//   ten names and three values, read into a frame report: the report's cost in the frame count.
#include <core/profiling/frame_capture.h>
#include <foundation/bench/bench.h>
#include <foundation/profiler/frame_report.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace engine;

namespace {

constexpr u32 k_capacity = 1u << 22;
constexpr u32 k_restart_at = 3u << 20;

const char* const k_names[10] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j"};

}  // namespace

ENGINE_BENCH_ARGS(profiler_capture_zone, "profiler.capture.zone", 1, 8, 16) {
  const u32 others = static_cast<u32>(state.arg()) - 1;
  profiling::CaptureOptions options;
  options.capacity = k_capacity;
  profiling::capture_reset();
  profiling::capture_start(options);
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  for (u32 t = 0; t < others; ++t) {
    threads.emplace_back([&stop] {
      while (!stop.load(std::memory_order_relaxed)) {
        for (u32 i = 0; i < 64; ++i) {
          const profiling::CaptureZone zone("other");
        }
      }
    });
  }
  constexpr u32 k_batch = 1024;
  while (state.keep_running()) {
    for (u32 i = 0; i < k_batch; ++i) {
      const profiling::CaptureZone zone("timed");
    }
    if (profiling::capture_count() > k_restart_at) {
      profiling::capture_stop();
      profiling::capture_start(options);
    }
  }
  stop.store(true, std::memory_order_relaxed);
  for (std::thread& t : threads)
    t.join();
  profiling::capture_stop();
  bench::keep(profiling::capture_count());
  profiling::capture_reset();
  state.set_items(k_batch);
}

ENGINE_BENCH_ARGS(profiler_report_build, "profiler.report.build", 2000, 20000) {
  const u32 frames = static_cast<u32>(state.arg());
  std::vector<profiling::CaptureEvent> events;
  events.reserve(static_cast<usize>(frames) * 15);
  const i64 frame_ns = 1'000'000;
  for (u32 f = 0; f < frames; ++f) {
    const i64 at = static_cast<i64>(f) * frame_ns;
    profiling::CaptureEvent mark;
    mark.kind = profiling::CaptureKind::frame;
    mark.name = "frame";
    mark.frame = f + 1;
    mark.begin_ns = mark.end_ns = at;
    events.push_back(mark);
    for (u32 z = 0; z < 10; ++z) {
      profiling::CaptureEvent zone;
      zone.kind = profiling::CaptureKind::zone;
      zone.name = k_names[z];
      zone.begin_ns = at + 1000 + z * 50'000;
      zone.end_ns = zone.begin_ns + 40'000;
      events.push_back(zone);
    }
    for (u32 v = 0; v < 3; ++v) {
      profiling::CaptureEvent value;
      value.kind = profiling::CaptureKind::value;
      value.track = v == 0 ? profiling::CaptureTrack::gpu : profiling::CaptureTrack::cpu;
      value.name = k_names[v];
      // A GPU time arrives frames behind the frame it measured.
      value.frame = v == 0 && f >= 2 ? f - 1 : f + 1;
      value.ms = 0.5;
      value.begin_ns = value.end_ns = at + 600'000 + v;
      events.push_back(value);
    }
    profiling::CaptureEvent end;
    end.kind = profiling::CaptureKind::frame_end;
    end.name = "frame end";
    end.frame = f + 1;
    end.begin_ns = end.end_ns = at + 900'000;
    events.push_back(end);
  }
  profiling::CaptureInfo info;
  info.stopped_ns = static_cast<i64>(frames) * frame_ns;
  profiler::ReportOptions options;
  u64 seen = 0;
  while (state.keep_running()) {
    const profiler::FrameReport report = profiler::build_report(
        std::span<const profiling::CaptureEvent>(events.data(), events.size()), info, options);
    seen += report.frames;
  }
  bench::keep(seen);
  state.set_items(events.size());
}

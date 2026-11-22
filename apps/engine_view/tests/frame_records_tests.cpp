// What both frame loops record of a frame, and how a report classifies a long one (roadmap T212;
// frame_records.h, long_frames.h; apps.md, "The report's long frames"). No device and no window:
// the window loop's calls are driven as it makes them — `frame_started` at each frame's top with
// the pacer's wait, `fill_record` when the frame folds — and the classifier is handed records
// built by hand, one per cause. Counts, never wall clock: the thread's CPU is asserted present and
// bounded by the frame's wall time, not measured.

#include "../frame_records.h"
#include "../long_frames.h"

#include <core/time/time.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <string_view>

using namespace engine;
using view::LongFrames;

namespace {

// Work the thread does itself, so its CPU clock moves (a sum the compiler cannot drop).
u64 spin(u32 rounds) {
  volatile u64 sum = 0;
  for (u32 i = 0; i < rounds; ++i)
    sum = sum + i * 2654435761u;
  return sum;
}

scene::FrameRecord with_sections(u32 frame, f64 wall_ms) {
  scene::FrameRecord r;
  r.frame = frame;
  r.frame_ms = wall_ms;
  scene::FrameSections s;
  s.wall_ms = wall_ms;
  r.sections = s;
  return r;
}

}  // namespace

TEST_CASE("engine-view: a window's frame records carry the thread's CPU and the pacing tail") {
  view::PendingFrame pending[view::k_pending_frames];
  u64 submissions = 0;
  // Three frames as the window loop runs them: the top (which ends the one before, with the
  // pacer's wait of its tail), the work, the submission's entry.
  const f64 pace[] = {0.0, 1.5, 2.5};
  for (u32 f = 0; f < 3; ++f) {
    const i64 start = time::monotonic_ns();
    const i64 start_cpu = view::frame_started(pending, submissions, start, pace[f]);
    (void)spin(2'000'000);
    view::PendingFrame& p = pending[submissions % view::k_pending_frames];
    p.submission = submissions;
    p.frame = f;
    p.spans = renderer::FrameSpans{};
    p.spans.start = start;
    p.spans.end = time::monotonic_ns();
    p.start_cpu = start_cpu;
    p.next_cpu = 0;
    ++submissions;
  }
  (void)view::frame_started(pending, submissions, time::monotonic_ns(), 4.0);  // the drain
  for (u32 f = 0; f < 3; ++f) {
    const view::PendingFrame& p = pending[f];
    REQUIRE(p.next_cpu >= p.start_cpu);
    scene::FrameRecord record;
    view::fill_record(p, record);
    REQUIRE(record.sections.has_value());
    const scene::FrameSections& s = *record.sections;
    CHECK(record.frame_ms == s.wall_ms);
    // The thread's CPU is measured (a window's records had none before version 26) and never more
    // than the frame's wall time by more than the system clock's tick.
    CHECK(s.thread_cpu_ms >= 0.0);
    CHECK(s.thread_cpu_ms <= s.wall_ms + 16.0);
    CHECK(s.off_cpu_ms >= 0.0);
    // The tail's pacing is the next frame's `tail_pace_ms`, clamped to the tail it is part of.
    const f64 asked = f < 2 ? pace[f + 1] : 4.0;
    CHECK(s.pace_ms == doctest::Approx(std::min(asked, s.tail_ms)));
  }
  // A frame whose next has not started has no sections.
  view::PendingFrame open;
  open.spans.start = time::monotonic_ns();
  scene::FrameRecord unfinished;
  view::fill_record(open, unfinished);
  CHECK_FALSE(unfinished.sections.has_value());
}

TEST_CASE("engine-view: a long frame is given the part that held it, one record per cause") {
  struct Case {
    scene::FrameRecord record;
    LongFrames::Cause cause;
    std::string_view part;
  };
  Case cases[10];
  u32 n = 0;
  {  // a slow GPU: the slot wait, the GPU inside the awaited frame
    scene::FrameRecord r = with_sections(100, 60.0);
    r.sections->slot_wait_ms = 50.0;
    scene::FrameGpu g;
    g.wait_split = true;
    g.wait_gpu_ms = 45.0;
    g.wait_late_ms = 5.0;
    r.gpu_frame = g;
    cases[n++] = {r, LongFrames::gpu, "wait_gpu"};
  }
  {  // the card on other work: not yet started on the awaited frame
    scene::FrameRecord r = with_sections(101, 70.0);
    r.sections->slot_wait_ms = 60.0;
    scene::FrameGpu g;
    g.wait_split = true;
    g.wait_gpu_ms = 10.0;
    g.wait_before_ms = 50.0;
    r.gpu_frame = g;
    cases[n++] = {r, LongFrames::gpu_elsewhere, "wait_before"};
  }
  {  // a late wake
    scene::FrameRecord r = with_sections(102, 40.0);
    r.sections->slot_wait_ms = 35.0;
    scene::FrameGpu g;
    g.wait_split = true;
    g.wait_gpu_ms = 5.0;
    g.wait_late_ms = 30.0;
    r.gpu_frame = g;
    cases[n++] = {r, LongFrames::late_wake, "wait_late"};
  }
  {  // the world's update, its placements' commit, the thread on the CPU
    scene::FrameRecord r = with_sections(103, 120.0);
    r.sections->host_ms = 110.0;
    r.sections->thread_cpu_ms = 115.0;
    r.sections->off_cpu_ms = 5.0;
    scene::FrameWorld w;
    w.total_ms = 110.0;
    w.update_ms = 100.0;
    w.placements_ms = 90.0;
    w.commit_ms = 80.0;
    r.world = w;
    cases[n++] = {r, LongFrames::world, "update.placements.commit"};
  }
  {  // the same frame with the thread off the processor: descheduled
    scene::FrameRecord r = cases[n - 1].record;
    r.frame = 104;
    r.sections->thread_cpu_ms = 10.0;
    r.sections->off_cpu_ms = 110.0;
    cases[n++] = {r, LongFrames::descheduled, "update.placements.commit"};
  }
  {  // the game module's tick
    scene::FrameRecord r = with_sections(105, 55.0);
    r.sections->ticks_ms = 45.0;
    scene::FrameWorld w;
    w.total_ms = 46.0;
    w.ticks_ms = 45.0;
    w.game_ms = 44.0;
    r.world = w;
    cases[n++] = {r, LongFrames::module, "game"};
  }
  {  // the present path: the pacer's wait in the tail
    scene::FrameRecord r = with_sections(106, 50.0);
    r.sections->tail_ms = 42.0;
    r.sections->pace_ms = 40.0;
    r.sections->present_ms = 2.0;
    cases[n++] = {r, LongFrames::present, "pace"};
  }
  {  // recording: the slowest pass holds most of the submission
    scene::FrameRecord r = with_sections(107, 45.0);
    r.sections->submit_ms = 40.0;
    r.cpu.slowest_pass = "trace";
    r.cpu.slowest_pass_ms = 30.0;
    cases[n++] = {r, LongFrames::recording, "pass:trace"};
  }
  {  // nothing named
    scene::FrameRecord r = with_sections(108, 36.0);
    r.sections->other_ms = 30.0;
    cases[n++] = {r, LongFrames::other, "other"};
  }
  {  // a slot wait with no split
    scene::FrameRecord r = with_sections(109, 101.0);
    r.sections->slot_wait_ms = 95.0;
    cases[n++] = {r, LongFrames::slot_wait, "slot_wait"};
  }
  REQUIRE(n == 10);
  LongFrames frames;
  frames.add(cases[0].record);  // not enabled: a no-op
  CHECK(frames.frames() == 0);
  CHECK(frames.json().is_null());
  frames.enable();
  for (u32 i = 0; i < n; ++i) {
    const LongFrames::Frame f = LongFrames::classify(cases[i].record);
    CHECK_MESSAGE(f.cause == cases[i].cause,
                  "frame " << cases[i].record.frame << ": " << LongFrames::cause_name(f.cause));
    CHECK_MESSAGE(std::string_view(f.part) == cases[i].part, "frame " << cases[i].record.frame);
    frames.add(cases[i].record);
  }
  // Short frames are counted as frames and classified as nothing.
  for (u32 i = 0; i < 20; ++i)
    frames.add(with_sections(200 + i, 16.0));
  CHECK(frames.frames() == 30);
  CHECK(frames.over(0) == 10);  // over 33
  CHECK(frames.over(1) == 6);   // over 50: 60, 70, 120, 120, 55, 101 (the pacer's 50 is not)
  CHECK(frames.over(2) == 3);   // over 100: 120, 120, 101
  CHECK(frames.over(0, LongFrames::world) == 1);
  CHECK(frames.over(2, LongFrames::descheduled) == 1);
  // The ten longest, longest first, by frame index.
  const std::span<const LongFrames::Frame> longest = frames.longest();
  REQUIRE(longest.size() == 10);
  CHECK(longest[0].wall_ms == 120.0);
  CHECK(longest[2].frame == 109);
  for (u32 i = 1; i < longest.size(); ++i)
    CHECK(longest[i - 1].wall_ms >= longest[i].wall_ms);
  // An eleventh long frame shorter than all ten is counted and not listed.
  frames.add(with_sections(300, 34.0));
  CHECK(frames.over(0) == 11);
  CHECK(frames.longest().size() == 10);
  CHECK(frames.longest()[9].frame != 300);
  const JsonValue json = frames.json();
  REQUIRE(json.find("over_100") != nullptr);
  REQUIRE(json.find("longest") != nullptr);
  CHECK(json.find("longest")->size() == 10);
}

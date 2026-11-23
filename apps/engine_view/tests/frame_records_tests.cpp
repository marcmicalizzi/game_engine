// What both frame loops record of a frame, and how a report classifies a long one (roadmap T212;
// frame_records.h, long_frames.h; apps.md, "The report's long frames"). No device and no window:
// the window loop's calls are driven as it makes them — `frame_started` at each frame's top with
// the pacer's wait, `fill_record` when the frame folds — and the classifier is handed records
// built by hand, one per cause. Counts, never wall clock: the thread's CPU is asserted present and
// bounded by the frame's wall time, not measured.

#include "../frame_records.h"
#include "../long_frames.h"
#include "../session_records.h"

#include <core/json/json.h>
#include <core/memory/memory.h>
#include <core/time/time.h>
#include <foundation/input/input_log.h>
#include <systems/renderer/scene_renderer.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
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

// T212's review, M1: the thread clock's tick. The experiments page's rows (docs/experiments/
// wide-frame-and-long-frames-2026-11-21.md): a placements commit of 25.2 ms with 15.6 ms of thread
// CPU is the world's work ("half or more"), however a 36 ms frame round it is cut; frame 2161's
// 51.8 ms commit with no CPU at all in a 65.2 ms frame is the thread off the processor; and a frame
// shorter than two ticks whose CPU reads zero was charged no tick, so it is not called off it.
TEST_CASE("engine-view: a long frame's thread CPU is read to the thread clock's tick") {
  auto commit_frame = [](u32 frame, f64 wall, f64 commit, f64 thread_cpu, f64 waits) {
    scene::FrameRecord r = with_sections(frame, wall);
    r.sections->slot_wait_ms = waits;
    r.sections->host_ms = commit + 2.8;
    r.sections->thread_cpu_ms = thread_cpu;
    r.sections->off_cpu_ms = wall - thread_cpu;
    scene::FrameWorld w;
    w.total_ms = commit + 2.8;
    w.update_ms = commit + 1.0;
    w.placements_ms = commit + 0.5;
    w.commit_ms = commit;
    r.world = w;
    return r;
  };
  // The page's "half or more" commit, in the review's 36 ms frame with 3 ms of waits.
  const LongFrames::Frame own = LongFrames::classify(commit_frame(1, 36.0, 25.2, 15.6, 3.0));
  CHECK(std::string_view(own.part) == "update.placements.commit");
  if (LongFrames::k_thread_cpu_tick_ms > 0.0) {
    CHECK_MESSAGE(own.cause == LongFrames::world, LongFrames::cause_name(own.cause));
  }
  // Frame 2161: 65.2 ms, the commit 51.8 ms, no thread CPU.
  const LongFrames::Frame off = LongFrames::classify(commit_frame(2161, 65.2, 51.8, 0.0, 0.0));
  CHECK_MESSAGE(off.cause == LongFrames::descheduled, LongFrames::cause_name(off.cause));
  // Shorter than two ticks with no CPU charged: unmeasured, never "off the processor".
  if (LongFrames::k_thread_cpu_tick_ms > 0.0) {
    const LongFrames::Frame short_one = LongFrames::classify(
        commit_frame(3, 2.0 * LongFrames::k_thread_cpu_tick_ms - 1.0, 25.0, 0.0, 0.0));
    CHECK(short_one.cause == LongFrames::world);
    CHECK(short_one.thread_cpu_ms < 0.0);
  }
}

// T212's review, M2: a window with `--report` alone classifies each folded frame from the
// renderer's stats and its own entry (`classify_folded`) — the same table the record path gives,
// and no allocation, where building the record allocates (a pass name past the string's small
// buffer, and the time-lapse's levels).
TEST_CASE(
    "engine-view: a window's long frames without records match the records' and allocate nothing") {
  static const char k_pass[] = "a pass whose name runs past the small-string buffer";
  renderer::FrameStats stats;
  stats.cpu.wait_ms = 40.0;
  stats.cpu.slowest_pass = k_pass;
  stats.cpu.slowest_pass_ms = 9.0;
  stats.gpu.valid = true;
  stats.cpu.wait_split = true;
  stats.cpu.wait_gpu_ms = 6.0;
  stats.cpu.wait_late_ms = 34.0;
  view::PendingFrame p;
  p.submission = 7;
  p.frame = 7;
  p.spans.start = 1'000'000;
  p.spans.end = 1'000'000 + 50'000'000;
  p.spans.next_start = 1'000'000 + 60'000'000;  // 60 ms: a long frame
  p.spans.submit_ms = 12.0;
  p.start_cpu = 1;
  p.next_cpu = 1 + 30'000'000;
  scene::FrameWorld w;
  w.total_ms = 5.0;
  p.world = w;
  // The time-lapse as a kept record carries it: its levels are what the counter sees allocate.
  scene::FrameTerrain terrain;
  terrain.levels.resize(3);
  p.terrain = terrain;
  // The record path, as `take_folded` keeps a record.
  LongFrames kept;
  kept.enable();
  const u64 record_before = mem::allocations_on_thread();
  for (u32 i = 0; i < 100; ++i) {
    scene::FrameRecord record = renderer::frame_record(stats, 0, p.frame, 0.0);
    view::fill_record(p, record);
    kept.add(record);
  }
  const u64 record_allocations = mem::allocations_on_thread() - record_before;
  // The window's path with no record.
  LongFrames folded;
  folded.enable();
  const u64 before = mem::allocations_on_thread();
  for (u32 i = 0; i < 100; ++i)
    view::classify_folded(stats, p, folded);
  const u64 allocations = mem::allocations_on_thread() - before;
  MESSAGE("100 frames: the record path allocated " << record_allocations << ", the window's "
                                                   << allocations);
  CHECK(record_allocations > 0);  // the counter sees what the record path costs
  CHECK(allocations == 0);
  CHECK(folded.frames() == 100);
  CHECK(folded.over(1) == 100);
  CHECK(write_json(folded.json()) == write_json(kept.json()));
  REQUIRE(!folded.longest().empty());
  CHECK(std::string_view(folded.longest()[0].part) == std::string_view(kept.longest()[0].part));
}

// T212's review, H1: the input log a lost device's turn writes is one `--replay-input` takes. A
// recording saved through the lost path with the loop's stamp passes the replay's own two checks
// (the session block, `view::session_from_json`; the map's hash, `InputLog::check_map`); the same
// recording saved with no stamp, as the first leg's handler saved it, fails both.
TEST_CASE("engine-view: a lost session's input log carries what a replay checks") {
  const test::TempDir tmp("engine_view_lost_recording");
  struct Stamp {
    u64 ticks = 24;
    u32 calls = 0;
  } stamp;
  auto events = [](input::InputLog& log) {
    for (u32 t = 0; t < 24; ++t)
      log.record(input::RawEvent{SimTick{t}, input::Source::Key, 26, 1.0f, 0});
  };
  u32 accepted = 0, refused = 0;
  auto replayable = [&](const std::string& path) {
    input::InputLog log;
    std::string error;
    REQUIRE_MESSAGE(log.load(path, &error) == io::Status::Ok, error);
    view::SessionHeader header;
    const bool session = view::session_from_json(log.session(), header, &error);
    input::ActionMap map;
    const bool known = view::default_fly_map_for(log.map_hash(), map);
    bool mapped = false;
    if (known) {
      const input::InputState probe(map);  // holds the map by address: a named one
      mapped = log.check_map(probe, &error);
    }
    if (!(session && mapped)) MESSAGE(path << ": " << error);
    (session && mapped ? accepted : refused) += 1;
    return session && mapped && header.ticks == stamp.ticks && log.events().size() == 24;
  };
  input::InputLog recording;
  events(recording);
  view::LostRecords held;
  held.recording = &recording;
  held.record_input = tmp.file("lost.jsonl");
  // The test's stand-in for main.cpp's `stamp_recording`: the map and the session block.
  held.finish_recording = [](void* context, input::InputLog& log) {
    Stamp& s = *static_cast<Stamp*>(context);
    ++s.calls;
    view::SessionHeader header;
    header.ticks = s.ticks;
    log.set_map(view::default_fly_map());
    log.set_session(view::session_to_json(header));
  };
  held.finish_context = &stamp;
  REQUIRE(view::save_lost_recording(held) == io::Status::Ok);
  CHECK(stamp.calls == 1);
  CHECK(replayable(held.record_input));
  // The first leg's handler: saved as it was held.
  input::InputLog unstamped;
  events(unstamped);
  view::LostRecords bare;
  bare.recording = &unstamped;
  bare.record_input = tmp.file("bare.jsonl");
  REQUIRE(view::save_lost_recording(bare) == io::Status::Ok);
  CHECK_FALSE(replayable(bare.record_input));
  CHECK(accepted == 1);
  CHECK(refused == 1);
  // No recording asked for: nothing written, Ok.
  const view::LostRecords none;
  CHECK(view::save_lost_recording(none) == io::Status::Ok);
}

// ---- T216: what a fault's turn and the journal share (session_records.h)
// -------------------------

TEST_CASE("engine-view: the fault seam's parse") {
  using view::TestFault;
  CHECK(view::parse_test_fault("40").kind == TestFault::access);
  CHECK(view::parse_test_fault("40").frame == 40);
  CHECK(view::parse_test_fault("7:access").kind == TestFault::access);
  CHECK(view::parse_test_fault("7:verify").kind == TestFault::verify);
  CHECK(view::parse_test_fault("7:verify").frame == 7);
  CHECK(view::parse_test_fault("").kind == TestFault::none);
  CHECK(view::parse_test_fault("x").kind == TestFault::none);
  CHECK(view::parse_test_fault("7:melt").kind == TestFault::none);
  CHECK(view::parse_test_fault("7x").kind == TestFault::none);
}

// The window loop's journal, driven as the loop drives it — four ticks a frame, a step after each
// frame's ticks — with no window: an append every 120 ticks and one on the tick of each mark, none
// of them asking the engine allocator for anything, and what is on disk after each loads and runs
// to the tick it names. The open's own stamp allocates once, at the first step.
TEST_CASE("engine-view: the recording's journal is on disk every half second and at each mark") {
  const test::TempDir tmp("engine_view_journal");
  struct Stamp {
    u64 ticks = 0;
  } stamp;
  input::InputLog recording;
  view::LostRecords held;
  held.recording = &recording;
  held.record_input = tmp.file("walk.jsonl");
  held.finish_recording = [](void* context, input::InputLog& log) {
    view::SessionHeader header;
    header.ticks = static_cast<Stamp*>(context)->ticks;
    log.set_map(view::default_fly_map());
    log.set_session(view::session_to_json(header));
  };
  held.finish_context = &stamp;
  view::RecordingJournal journal;
  constexpr u64 k_ticks = 1200;  // five seconds at 240 Hz
  constexpr u64 k_marks[] = {250, 251, 777};
  u64 marks = 0;
  u64 step_allocations = 0;
  u32 wrote = 0;
  for (u64 tick = 4; tick <= k_ticks; tick += 4) {
    // A frame's ticks: a key down and up every 40 ticks, a mouse motion every frame.
    for (u64 t = tick - 3; t <= tick; ++t) {
      if (t % 40 == 0) recording.record(input::RawEvent{SimTick{t}, input::Source::Key, 26, 1, 0});
      if (t % 40 == 20) recording.record(input::RawEvent{SimTick{t}, input::Source::Key, 26, 0, 0});
      for (const u64 m : k_marks) {
        if (m == t) {
          ++marks;
          recording.record(input::RawEvent{SimTick{t}, input::Source::Key, 16, 1, 0});  // M
        }
      }
    }
    recording.record(input::RawEvent{SimTick{tick}, input::Source::MouseAxis, 0, 0.25f, 0});
    stamp.ticks = tick;
    const u64 before = mem::allocations_on_thread();
    const bool appended = view::journal_step(journal, recording, held, tick, marks);
    if (tick > 4) step_allocations += mem::allocations_on_thread() - before;
    if (!appended) continue;
    ++wrote;
    // What a process that died now would leave: everything recorded, and the tick reached.
    std::ifstream in(held.record_input, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const std::string copy = tmp.file("copy.jsonl");
    REQUIRE(io::write_file(copy, bytes) == io::Status::Ok);
    input::InputLog crashed;
    std::string error;
    REQUIRE_MESSAGE(crashed.load(copy, &error) == io::Status::Ok, error);
    CHECK(crashed.size() == recording.size());
    REQUIRE(crashed.has_through());
    CHECK(crashed.through().value == tick);
    view::SessionHeader header;
    CHECK(view::session_from_json(crashed.session(), header, &error));
  }
  // Ten half-seconds (the first frame opens it and starts the clock, no append), and the marks:
  // 250 and 251 fall in the frame ending at 252, one append; 777 in the one ending at 780.
  CHECK(journal.appends == wrote);
  CHECK(wrote == 9 + 2);
  CHECK(journal.marks_seen == 3);
  CHECK(step_allocations == 0);
  CHECK(journal.file.through().value == 1140);
  MESSAGE(view::journal_summary(journal));
  // The normal end closes it, and the whole log replaces it.
  journal.file.close();
  REQUIRE(view::save_lost_recording(held) == io::Status::Ok);
  input::InputLog whole;
  REQUIRE(whole.load(held.record_input) == io::Status::Ok);
  CHECK_FALSE(whole.has_through());
  CHECK(whole.size() == recording.size());
}

// T216's review, H1: the fault's turn takes the journal and saves the whole log over it; a frame
// side that steps the journal once more after that (a fault off the frame's thread before the
// mend, or a lost device met on a load's job thread) reopened the path with truncation and left
// half a second of input. Now it writes nothing, and the file is no shorter and loads whole.
TEST_CASE("engine-view: a journal the fault's turn took is never reopened or truncated") {
  const test::TempDir tmp("engine_view_journal_taken");
  input::InputLog recording;
  view::RecordingJournal journal;
  view::LostRecords held;
  held.recording = &recording;
  held.record_input = tmp.file("walk.jsonl");
  held.finish_recording = [](void*, input::InputLog& log) {
    view::SessionHeader header;
    log.set_map(view::default_fly_map());
    log.set_session(view::session_to_json(header));
  };
  held.journal = &journal.file;
  u32 appends = 0;
  for (u64 tick = 4; tick <= 600; tick += 4) {
    recording.record(input::RawEvent{SimTick{tick}, input::Source::MouseAxis, 0, 0.25f, 0});
    if (view::journal_step(journal, recording, held, tick, 0)) ++appends;
  }
  REQUIRE(appends == 4);
  // The fault's turn, on the frame side: taken, and the whole log saved over it.
  REQUIRE(view::save_lost_recording(held) == io::Status::Ok);
  const auto saved_bytes = std::filesystem::file_size(held.record_input);
  const u32 saved_events = recording.size();
  // The frame side steps on: past the half second, and with a mark.
  for (u64 tick = 604; tick <= 840; tick += 4) {
    recording.record(input::RawEvent{SimTick{tick}, input::Source::MouseAxis, 0, 0.25f, 0});
    CHECK_FALSE(view::journal_step(journal, recording, held, tick, tick == 700 ? 1 : 0));
  }
  CHECK_FALSE(journal.file.is_open());
  CHECK(std::filesystem::file_size(held.record_input) >= saved_bytes);
  input::InputLog left;
  std::string error;
  REQUIRE_MESSAGE(left.load(held.record_input, &error) == io::Status::Ok, error);
  CHECK(left.size() == saved_events);
  CHECK(left.last_tick().value == 600);
}

// T216's review, H1: the hook knows the frame's thread by the id `loop_at` stores, and a fault on
// any other thread neither reads nor writes what that thread grows.
TEST_CASE("engine-view: a fault off the frame's thread leaves the records and the marks alone") {
  view::LoopState& s = view::loop_state();
  const u64 before = s.frame_thread.load();
  platform::CrashInfo info;
  info.thread_id = platform::current_thread_id();
  s.frame_thread.store(0);
  CHECK(view::frame_side_still(info));  // before the first frame nothing grows them
  view::loop_at(5, 20);
  CHECK(s.frame_thread.load() == platform::current_thread_id());
  CHECK(view::frame_side_still(info));
  info.thread_id = platform::current_thread_id() + 1;
  CHECK_FALSE(view::frame_side_still(info));
  const JsonValue block = view::crashed_json(info, {}, nullptr, false);
  CHECK(block.find("frame_thread")->as_bool() == false);
  CHECK(block.find("frames_recorded")->is_null());
  CHECK(block.find("last_recorded_frame")->is_null());
  CHECK(block.find("marks")->is_null());
  u64 frame = 0;
  REQUIRE(block.find("frame")->get_u64(frame));
  CHECK(frame == 5);
  s.frame_thread.store(before);
}

TEST_CASE(
    "engine-view: a fault's block names the loop's place, the fault, its stack and the marks") {
  platform::CrashInfo info;
  info.what = "access violation";
  info.code = 0xC0000005u;
  info.address = 0x10;
  info.access = "writing";
  info.at = "engine-view.exe+0x25b7";
  info.stack[0] = "engine-view.exe+0x21fa";
  info.stack[1] = "KERNEL32.DLL+0x2cd87";
  info.stack_count = 2;
  info.thread_id = 42;
  info.main_thread = true;
  info.crash_file = "report.json.crash.json";
  info.line_text = "engine: fatal: access violation writing 0x10 at engine-view.exe+0x25b7";
  Vector<view::FlyMarker> marks;
  marks.push_back(view::FlyMarker{250, {}, false});
  marks.push_back(view::FlyMarker{777, {}, false});
  Vector<scene::FrameRecord> records(3);
  records[2].frame = 27;
  view::loop_doing("frame");
  view::loop_state().frame.store(30);
  view::loop_state().tick.store(120);
  const JsonValue block = view::crashed_json(
      info, std::span<const scene::FrameRecord>(records.data(), records.size()), &marks);
  u64 value = 0;
  REQUIRE(block.find("frame")->get_u64(value));
  CHECK(value == 30);
  REQUIRE(block.find("tick")->get_u64(value));
  CHECK(value == 120);
  CHECK(block.find("doing")->as_string() == "frame");
  CHECK(block.find("code")->as_string() == "0x00000000c0000005");
  CHECK(block.find("address")->as_string() == "0x0000000000000010");
  CHECK(block.find("stack")->size() == 2);
  CHECK(block.find("assertion")->is_null());
  REQUIRE(block.find("frames_recorded")->get_u64(value));
  CHECK(value == 3);
  REQUIRE(block.find("last_recorded_frame")->get_u64(value));
  CHECK(value == 27);
  REQUIRE(block.find("marks")->size() == 2);
  REQUIRE((*block.find("marks"))[1].get_u64(value));
  CHECK(value == 777);
  // A failed verify names itself.
  info.expression = "g_never != 0";
  info.file = "crash_records.cpp";
  info.line = 9;
  const JsonValue verify = view::crashed_json(info, {}, nullptr);
  REQUIRE(verify.find("assertion")->is_object());
  CHECK(verify.find("assertion")->find("expression")->as_string() == "g_never != 0");
  CHECK(verify.find("marks")->size() == 0);
  CHECK(verify.find("last_recorded_frame")->is_null());
}

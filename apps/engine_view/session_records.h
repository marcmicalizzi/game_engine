#pragma once

// **What a session leaves behind, however it ends** (roadmap T212; docs/subsystems/apps.md, "When
// the device is lost"): the benchmark's `.jsonl` writer both loops share, and the lost device's
// turn.
//
// A frame loop names what it holds here once it has it (`lost_records()`: its records, its input
// recording and where each goes), and every device engine-view makes carries `on_device_lost` as
// its handler (gfx::Device::set_lost_handler, domain/gfx/device_lost.h). On the first lost device
// the handler writes, in this order and each only when the session was asked for it: the report
// (`--report`) with exit reason `device_lost` and a `device_lost` block, first because it is small
// and is the file a tester sends back; the vendor's crash dump beside the report or the records
// (`<file>.fault.bin`), which the block names; the input log (`--record-input`), stamped with the
// map's hash and the session block by the loop's own function, so `--replay-input` takes it; the
// frame records to the last one that folded with a last line holding the loss (`--benchmark`);
// and the startup trace (`--startup-trace`). **Every file is written whole or not at all**
// (`io::write_file_atomic`; the trace's writer the same way), so a write the watchdog cuts short
// leaves a `.tmp` beside the last whole file, never a short file under its name. Then it ends the
// process with `platform::k_exit_device_lost` (76).
//
// **No path can hang.** Before it writes anything — its own line on stderr included — the handler
// starts a watchdog that ends the process with the same code after `k_lost_write_seconds` however
// far the writes got, and writes nothing itself (a full stderr pipe cannot hold it); the handler's
// first line says the bound, and the log is flushed before the files are written, so the loss's
// `gfx.device_lost` lines are out whatever the watchdog cuts. Nothing it writes touches the
// device. No dialog: it is a line on stderr and an exit code (AGENTS.md, "No engine binary opens a
// dialog when it fails").
//
// **Which thread** (T212's review, L2; said, not mended). The handler runs on whichever thread met
// the loss first — the frame thread, or a load's job thread through an immediate submission while
// the main thread pumps the loading screen (main.cpp's `LoadingScreen::run`); gfx lets one through
// and holds every later caller, the pump's presents included (device_lost.h). It reads the records
// and the recording without a lock: on the frame thread they are its own, and no frame is folded
// while a load's job runs. Whether the pump hands window events to the recording in that window
// was not read; if it does, the log written from the job thread may miss or tear the last tick.
//
// A loop names its records with `LostRecordsScope` beside them, which clears `lost_records()`
// before they go, so a loss met while the loop's locals are destroyed (a destructor's immediate
// submission, a last present) finds nothing named and writes the report alone.
//
// **A fault in the process** (roadmap T216; apps.md, "When the process faults") takes the same
// turn through core/platform's crash hook (crash.h): `install_crash_records` registers `on_crash`
// and a crash file beside the report, and the hook writes what `on_device_lost` writes — the report
// with exit reason `crashed` and a `crashed` block (the frame, the tick, what the loop was doing,
// the fault and its stack, the marks), the input log stamped as a normal end stamps it, the frame
// records with a last `{"crashed": ...}` line in place of the summary, and the startup trace — then
// returns, and the platform ends the process with `platform::k_exit_crashed` (77). Bounded by the
// platform: `k_crash_write_ms`, and a second fault inside the hook ends the process at once.
// **Heroics in a broken process are a bonus, not the plan**: a fault on the frame thread may hold
// the heap's lock, and then the hook never gets past its first allocation; so what the owner
// cannot afford to lose — the input, and the marks inside it — reaches the disk as it is made, by
// the recording's journal (`RecordingJournal`, below), and survives a hook that never finishes.

#include "fly_camera.h"
#include "report.h"

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/platform/crash.h>
#include <domain/gfx/device_lost.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>

#include <atomic>
#include <schemas/scene.h>
#include <span>
#include <string>

namespace engine::view {

// The `.jsonl` a benchmark writes: one record per line, then the summary line. Atomic.
io::Status write_benchmark(const std::string& path, std::span<const scene::FrameRecord> records,
                           const std::string& summary_line);

// What the running loop holds, for the lost device's turn. Null and empty until a loop names them.
struct LostRecords {
  const Vector<scene::FrameRecord>* records = nullptr;
  std::string benchmark;  // --benchmark
  input::InputLog* recording = nullptr;
  std::string record_input;  // --record-input
  // What the loop's normal end stamps the recording with before it saves it — the map's hash and
  // the session block with the ticks run (main.cpp's `stamp_recording`, which `save_recording`
  // calls too) — so the lost path's log is one `--replay-input` accepts. Null: saved as it is.
  void (*finish_recording)(void* context, input::InputLog& recording) = nullptr;
  void* finish_context = nullptr;
  // The report's frame times come from the records at the end of an offscreen run; a window adds
  // them as it goes. True when the handler should add the records' frames to the report.
  bool report_frames_from_records = false;
  // The session's marks (M; `FlySession::trajectory().markers`), for a crash's block (T216).
  const Vector<FlyMarker>* marks = nullptr;
  // The recording's journal (T216), closed before the recording is saved whole over it.
  input::InputLogJournal* journal = nullptr;
};
LostRecords& lost_records();

// Clears `lost_records()` when the loop's locals it names go (declare it after them).
struct LostRecordsScope {
  LostRecordsScope() = default;
  LostRecordsScope(const LostRecordsScope&) = delete;
  LostRecordsScope& operator=(const LostRecordsScope&) = delete;
  ~LostRecordsScope() { lost_records() = {}; }
};

// The lost path's input log: stamped by the loop's function, then saved (atomically, as
// `InputLog::save` writes). Ok when no recording was asked for. In the header so the no-device
// case runs these lines (tests/frame_records_tests.cpp).
inline io::Status save_lost_recording(const LostRecords& held) {
  if (held.recording == nullptr || held.record_input.empty()) return io::Status::Ok;
  if (held.finish_recording != nullptr) held.finish_recording(held.finish_context, *held.recording);
  // The journal holds the file open; the whole log replaces it (T216). Taken, not just closed: a
  // frame that steps the journal once more after this never reopens the path, which would
  // truncate it (T216's review, H1). If this save fails, the journal is left as it was: whole to
  // its last append, and replayable to its through tick.
  if (held.journal != nullptr) held.journal->take();
  return held.recording->save(held.record_input);
}

// ---- T216: the recording's journal, and the loop's place --------------------------------------

// **A live session's input on disk as it goes** (apps.md, "When the process faults"): the
// recording's journal at `--record-input`'s path (input_log.h, `InputLogJournal`), appended every
// `every_ticks` of the session — half a second at its 240 Hz — and at once on the tick a mark is
// made, so a mark is on disk the moment the owner presses M. A session's normal end closes it and
// saves the log whole over it, so a finished session's file is the bytes it always was.
struct RecordingJournal {
  input::InputLogJournal file;
  u32 written = 0;  // the recording's events already appended
  u64 last_tick = 0;
  u64 every_ticks = 120;
  u64 marks_seen = 0;
  bool failed = false;
  // What it cost, on the frame thread (`summary_line`).
  u64 appends = 0;
  i64 append_ns = 0;
  i64 worst_append_ns = 0;
};

// Opens the journal at the first call (stamping the recording as the loop's normal end would, for
// its header), then appends the recording's new events and the tick reached when `every_ticks`
// have passed since the last append or `marks` grew. Allocates nothing after the open. True when
// it wrote. Nothing without a recording path; a journal that cannot be written says so once and
// stops, and the session goes on; nothing once a session's end has taken it
// (`InputLogJournal::take`), so it never reopens — and truncates — the path after that.
bool journal_step(RecordingJournal& journal, input::InputLog& recording, const LostRecords& held,
                  u64 tick, u64 marks);
// The journal's line for stderr at a session's end: appends, mean and worst microseconds.
std::string journal_summary(const RecordingJournal& journal);

// **Where the loop is**, for a crash's block: the frame, the tick, and what it was doing (a static
// string), and **the frame's thread** (T216's review, H1): the OS's id of the thread that runs the
// frame loop (`platform::current_thread_id`, the id a `CrashInfo` names a thread by), stored by
// `loop_at` at every frame, zero before the first. Relaxed atomics, a few stores a frame.
struct LoopState {
  std::atomic<u64> frame{~u64{0}};
  std::atomic<u64> tick{0};
  std::atomic<const char*> doing{"starting"};
  std::atomic<u64> frame_thread{0};
};
LoopState& loop_state();
// **Whose are the session's records when a fault comes** (T216's review, H1). The records, the
// recording, its journal and the marks are grown by the frame's thread alone. A fault on that
// thread, or before any frame began, holds them still, and the hook may read and write them. A
// fault on any other thread (a job, the streamer, the sound) leaves the frame thread running, still
// growing them, and the hook touches none of them: the journal stays as the durable input record,
// whole through its last append, and the frame records stay what is already on disk.
bool frame_side_still(const platform::CrashInfo& info) noexcept;
inline void loop_doing(const char* what) noexcept {
  loop_state().doing.store(what, std::memory_order_relaxed);
}
// A frame begins: its index and the session's tick, and the test seam's fault when it names this
// frame (`ENGINE_VIEW_TEST_FAULT`, below).
void loop_at(u64 frame, u64 tick) noexcept;

// **The test seam** (T216): `ENGINE_VIEW_TEST_FAULT=<frame>[:<kind>]` makes the frame loop fault at
// that frame — `access` (the default) a write through a bad pointer, `verify` a failed
// ENGINE_VERIFY — honoured only under a live test's main, by the rule the device profile and the
// lost-device seam keep (domain/gfx/test_profile.h): a player's run cannot fault by a variable
// left in a shell. Read once.
inline constexpr const char* k_test_fault_env = "ENGINE_VIEW_TEST_FAULT";
enum class TestFault : u8 { none, access, verify };
struct TestFaultAt {
  u64 frame = 0;
  TestFault kind = TestFault::none;
};
// The parse alone, for the case: "40" and "40:access" a write, "40:verify" a verify, anything else
// none.
TestFaultAt parse_test_fault(std::string_view text) noexcept;

// The report's and the records' `crashed` block: where the loop was, the fault, its stack, the
// crash file, the marks, and what was recorded. `frame_side`: false for a fault off the frame's
// thread (`frame_side_still`), when the records and the marks are not read and their fields are
// null.
JsonValue crashed_json(const platform::CrashInfo& info, std::span<const scene::FrameRecord> records,
                       const Vector<FlyMarker>* marks, bool frame_side = true);

// Registers `on_crash` (context: the run's `RunReport`) and the crash file — `<report>.crash.json`,
// or `<benchmark>.crash.json` with no report, none with neither. Once, after the options are read.
inline constexpr u32 k_crash_write_ms = 30'000;
void install_crash_records(RunReport* report, const std::string& benchmark);
void on_crash(void* context, const platform::CrashInfo& info);

// The report's and the records' `device_lost` block: where, the frame values, the frames recorded
// and the last one's index, and the fault with its records (addresses in hex).
JsonValue device_lost_json(const gfx::DeviceLost& lost, std::span<const scene::FrameRecord> records,
                           const std::string& vendor_binary_path);

// The handler every engine-view device carries; `context` is the run's `RunReport`. Never returns.
inline constexpr u32 k_lost_write_seconds = 30;
[[noreturn]] void on_device_lost(void* context, const gfx::DeviceLost& lost);

}  // namespace engine::view

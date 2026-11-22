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

#include "report.h"

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <domain/gfx/device_lost.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>

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
  return held.recording->save(held.record_input);
}

// The report's and the records' `device_lost` block: where, the frame values, the frames recorded
// and the last one's index, and the fault with its records (addresses in hex).
JsonValue device_lost_json(const gfx::DeviceLost& lost, std::span<const scene::FrameRecord> records,
                           const std::string& vendor_binary_path);

// The handler every engine-view device carries; `context` is the run's `RunReport`. Never returns.
inline constexpr u32 k_lost_write_seconds = 30;
[[noreturn]] void on_device_lost(void* context, const gfx::DeviceLost& lost);

}  // namespace engine::view

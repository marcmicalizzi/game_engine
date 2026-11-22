#pragma once

// **What a session leaves behind, however it ends** (roadmap T212; docs/subsystems/apps.md, "When
// the device is lost"): the benchmark's `.jsonl` writer both loops share, and the lost device's
// turn.
//
// A frame loop names what it holds here once it has it (`lost_records()`: its records, its input
// recording and where each goes), and every device engine-view makes carries `on_device_lost` as
// its handler (gfx::Device::set_lost_handler, domain/gfx/device_lost.h). On the first lost device
// the handler writes, in this order and each only when the session was asked for it: the input
// log (`--record-input`), the frame records to the last one that folded with a last line holding
// the loss (`--benchmark`), the startup trace (`--startup-trace`), the vendor's crash dump beside
// the report or the records (`<file>.fault.bin`), and the report (`--report`) with exit reason
// `device_lost` and a `device_lost` block. Then it ends the process with
// `platform::k_exit_device_lost` (76).
//
// **No path can hang.** Before it writes anything the handler starts a watchdog that ends the
// process with the same code after `k_lost_write_seconds` however far the writes got, and says
// so on stderr; nothing it writes touches the device. No dialog: it is a line on stderr and an
// exit code (AGENTS.md, "No engine binary opens a dialog when it fails").

#include "report.h"

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <domain/gfx/device_lost.h>
#include <foundation/io/vfs.h>

#include <schemas/scene.h>
#include <span>
#include <string>

namespace engine::input {
class InputLog;
}

namespace engine::view {

// The `.jsonl` a benchmark writes: one record per line, then the summary line.
io::Status write_benchmark(const std::string& path, std::span<const scene::FrameRecord> records,
                           const std::string& summary_line);

// What the running loop holds, for the lost device's turn. Null and empty until a loop names them.
struct LostRecords {
  const Vector<scene::FrameRecord>* records = nullptr;
  std::string benchmark;  // --benchmark
  const input::InputLog* recording = nullptr;
  std::string record_input;  // --record-input
  // The report's frame times come from the records at the end of an offscreen run; a window adds
  // them as it goes. True when the handler should add the records' frames to the report.
  bool report_frames_from_records = false;
};
LostRecords& lost_records();

// The report's and the records' `device_lost` block: where, the frame values, the frames recorded
// and the last one's index, and the fault with its records (addresses in hex).
JsonValue device_lost_json(const gfx::DeviceLost& lost, std::span<const scene::FrameRecord> records,
                           const std::string& vendor_binary_path);

// The handler every engine-view device carries; `context` is the run's `RunReport`. Never returns.
inline constexpr u32 k_lost_write_seconds = 30;
[[noreturn]] void on_device_lost(void* context, const gfx::DeviceLost& lost);

}  // namespace engine::view

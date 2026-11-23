#include "session_records.h"

#include "startup.h"

#include <core/json/json.h>
#include <core/log/log.h>
#include <core/platform/process.h>
#include <core/schema/json_reflect.h>
#include <foundation/input/input_log.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>

namespace engine::view {

namespace {

std::string hex_of(u64 value) {
  char text[24];
  std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(value));
  return text;
}

// The watchdog: whatever the writes are doing after `k_lost_write_seconds`, the process ends. It
// writes nothing — a stderr pipe nobody drains would hold it, and the bound with it; the
// handler's first line already said it.
[[noreturn]] void give_up_after() {
  std::this_thread::sleep_for(std::chrono::seconds(k_lost_write_seconds));
  std::_Exit(platform::k_exit_device_lost);
}

}  // namespace

io::Status write_benchmark(const std::string& path, std::span<const scene::FrameRecord> records,
                           const std::string& summary_line) {
  std::string text;
  for (const scene::FrameRecord& record : records) {
    write_json(schema::to_json(record), text, JsonWriteOptions{.pretty = false});
    text.push_back('\n');
  }
  text += summary_line;
  text.push_back('\n');
  return io::write_file_atomic(path, text);  // whole or not at all (T212)
}

LostRecords& lost_records() {
  static LostRecords records;
  return records;
}

JsonValue device_lost_json(const gfx::DeviceLost& lost, std::span<const scene::FrameRecord> records,
                           const std::string& vendor_binary_path) {
  JsonValue out = JsonValue::object();
  out.set("at", gfx::device_lost_at_name(lost.at));
  out.set("simulated", lost.simulated);
  // The frame timeline's values: the one the loss was met at, the last submitted, the last the
  // device had signalled. A frame record's index is the session's frame; the last recorded one is
  // the newest frame whose numbers came back before the loss.
  out.set("frame_value", lost.value);
  out.set("last_submitted", lost.last_submitted);
  out.set("last_completed", lost.last_completed);
  out.set("frames_recorded", static_cast<u64>(records.size()));
  out.set("last_recorded_frame",
          records.empty() ? JsonValue() : JsonValue(records[records.size() - 1].frame));
  const gfx::DeviceFault& f = lost.fault;
  JsonValue fault = JsonValue::object();
  fault.set("read", f.read);
  fault.set("unavailable", f.unavailable);
  fault.set("description", f.description);
  JsonValue addresses = JsonValue::array();
  for (const gfx::FaultAddress& a : f.addresses) {
    JsonValue row = JsonValue::object();
    row.set("kind", gfx::fault_address_kind_name(a.kind));
    row.set("address", hex_of(a.address));
    row.set("precision", a.precision);
    addresses.push_back(std::move(row));
  }
  fault.set("addresses", std::move(addresses));
  JsonValue vendors = JsonValue::array();
  for (const gfx::FaultVendorRecord& v : f.vendors) {
    JsonValue row = JsonValue::object();
    row.set("description", v.description);
    row.set("code", hex_of(v.code));
    row.set("data", hex_of(v.data));
    vendors.push_back(std::move(row));
  }
  fault.set("vendors", std::move(vendors));
  fault.set("vendor_binary_bytes", static_cast<u64>(f.vendor_binary.size()));
  fault.set("vendor_binary", vendor_binary_path);
  out.set("fault", std::move(fault));
  return out;
}

namespace {

std::span<const scene::FrameRecord> held_records(const LostRecords& held) {
  return held.records != nullptr
             ? std::span<const scene::FrameRecord>(held.records->data(), held.records->size())
             : std::span<const scene::FrameRecord>();
}

// What a session's abnormal end writes, the lost device's and the fault's alike: in this order and
// each only when the session was asked for it, the report with `reason`, `exit_code` and the block
// under `key`; `before_input` (the vendor's dump, which the report names); the input log stamped as
// the loop's normal end stamps it, so a replay takes it (T212's H1); the records with the block as
// their last line; the startup trace.
//
// `frame_side` false (a fault off the frame's thread, `frame_side_still`): the report alone — its
// frame times are fixed bins the frame thread adds to without allocating — and the trace; the
// input log, the records and the records' frames are another thread's and are not touched.
void write_session_end(RunReport* report, const char* reason, i32 exit_code,
                       const std::string& error_line, const char* key, const JsonValue& block,
                       void (*before_input)(const void*), const void* before_context,
                       bool frame_side = true) {
  const LostRecords& held = lost_records();
  const std::span<const scene::FrameRecord> records =
      frame_side ? held_records(held) : std::span<const scene::FrameRecord>();
  // 1. The report: small, and the file a tester sends back (T212's review, M3).
  if (report != nullptr && !report->path.empty()) {
    if (frame_side && held.report_frames_from_records) {
      for (const scene::FrameRecord& r : records) {
        report->frames.add(r.frame_ms);
        report->long_frames.add(r);
      }
    }
    report->exit_code = exit_code;
    report->reason = reason;
    if (report->error.empty()) report->error = error_line;
    if (std::string_view(key) == "crashed") {
      report->crashed = block;
    } else {
      report->device_lost = block;
    }
    std::string error;
    if (!write_report(*report, &error))
      std::fprintf(stderr, "engine-view: report: %s\n", error.c_str());
    else
      std::fprintf(stderr, "engine-view: the report is in %s\n", report->path.c_str());
  }
  // 2. Whatever the report names that is written after it.
  if (before_input != nullptr) before_input(before_context);
  // 3. The input log.
  if (frame_side && save_lost_recording(held) != io::Status::Ok)
    std::fprintf(stderr, "engine-view: cannot write %s\n", held.record_input.c_str());
  // 4. The records, the largest, with the block as their last line.
  if (frame_side && !held.benchmark.empty()) {
    JsonValue last = JsonValue::object();
    last.set(key, block);
    if (write_benchmark(held.benchmark, records,
                        write_json(last, JsonWriteOptions{.pretty = false})) != io::Status::Ok)
      std::fprintf(stderr, "engine-view: cannot write %s\n", held.benchmark.c_str());
  }
  // 5. The startup trace (written whole or not at all, core/profiling's writer).
  (void)startup_trace().finish(nullptr);
  log::flush();
  std::fflush(stdout);
  std::fflush(stderr);
}

struct VendorDump {
  const std::string* path;
  const gfx::DeviceLost* lost;
};

void write_vendor_dump(const void* context) {
  const VendorDump& d = *static_cast<const VendorDump*>(context);
  if (d.path->empty()) return;
  const std::string_view bytes(reinterpret_cast<const char*>(d.lost->fault.vendor_binary.data()),
                               d.lost->fault.vendor_binary.size());
  if (io::write_file_atomic(*d.path, bytes) != io::Status::Ok)
    std::fprintf(stderr, "engine-view: cannot write %s\n", d.path->c_str());
}

}  // namespace

void on_device_lost(void* context, const gfx::DeviceLost& lost) {
  // The watchdog first, before even this line: a stderr nobody drains cannot hold the bound.
  std::thread(give_up_after).detach();
  std::fprintf(stderr,
               "engine-view: the device was lost (%s); writing what the session has, %u s at "
               "most, then exiting %d\n",
               gfx::device_lost_at_name(lost.at), k_lost_write_seconds,
               platform::k_exit_device_lost);
  std::fflush(stderr);
  log::flush();  // the loss's own lines (gfx.device_lost), whatever the watchdog cuts below
  RunReport* report = static_cast<RunReport*>(context);
  const LostRecords& held = lost_records();
  // The vendor's dump beside the report, or beside the records when there is no report. The block
  // names it before it is written, since the report goes first.
  std::string dump;
  if (!lost.fault.vendor_binary.empty()) {
    const std::string& beside =
        report != nullptr && !report->path.empty() ? report->path : held.benchmark;
    if (!beside.empty()) dump = beside + ".fault.bin";
  }
  const JsonValue block = device_lost_json(lost, held_records(held), dump);
  const VendorDump vendor{&dump, &lost};
  write_session_end(report, "device_lost", platform::k_exit_device_lost,
                    std::string("device: lost at ") + gfx::device_lost_at_name(lost.at),
                    "device_lost", block, write_vendor_dump, &vendor);
  std::_Exit(platform::k_exit_device_lost);
}

// ---- T216: the fault's turn (the shared pieces are crash_records.cpp) -------------------------

void on_crash(void* context, const platform::CrashInfo& info) {
  std::fprintf(stderr,
               "engine-view: the process faulted (%s); writing what the session has, %u s at "
               "most, then exiting %d\n",
               info.what, k_crash_write_ms / 1000, platform::k_exit_crashed);
  std::fflush(stderr);
  log::flush();
  RunReport* report = static_cast<RunReport*>(context);
  // A fault off the frame's thread leaves that thread running: its records, recording, journal and
  // marks are not this hook's to read or write (T216's review, H1; session_records.h).
  const bool frame_side = frame_side_still(info);
  if (!frame_side) {
    std::fprintf(stderr,
                 "engine-view: the fault is off the frame's thread; the input journal and the "
                 "frame records stay as they stand on disk\n");
  }
  const LostRecords& held = lost_records();
  const JsonValue block = frame_side ? crashed_json(info, held_records(held), held.marks)
                                     : crashed_json(info, {}, nullptr, false);
  write_session_end(report, "crashed", platform::k_exit_crashed,
                    std::string("crashed: ") + info.what, "crashed", block, nullptr, nullptr,
                    frame_side);
  // Returns: the platform ends the process with platform::k_exit_crashed.
}

void install_crash_records(RunReport* report, const std::string& benchmark) {
  const std::string& beside = report != nullptr && !report->path.empty() ? report->path : benchmark;
  if (!beside.empty() && !platform::set_crash_file(beside + ".crash.json"))
    std::fprintf(stderr, "engine-view: no crash file (a sanitizer build, or the path is long)\n");
  if (!platform::set_crash_hook(on_crash, report, k_crash_write_ms))
    std::fprintf(stderr,
                 "engine-view: a fault will not write the session's records (a sanitizer "
                 "build, or ENGINE_ERROR_DIALOGS=1)\n");
}

}  // namespace engine::view

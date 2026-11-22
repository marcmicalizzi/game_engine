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
  const std::span<const scene::FrameRecord> records =
      held.records != nullptr
          ? std::span<const scene::FrameRecord>(held.records->data(), held.records->size())
          : std::span<const scene::FrameRecord>();
  // The vendor's dump beside the report, or beside the records when there is no report. The block
  // names it before it is written, since the report goes first.
  std::string dump;
  if (!lost.fault.vendor_binary.empty()) {
    const std::string& beside =
        report != nullptr && !report->path.empty() ? report->path : held.benchmark;
    if (!beside.empty()) dump = beside + ".fault.bin";
  }
  const JsonValue block = device_lost_json(lost, records, dump);
  // 1. The report: small, and the file a tester sends back (T212's review, M3).
  if (report != nullptr && !report->path.empty()) {
    if (held.report_frames_from_records) {
      for (const scene::FrameRecord& r : records) {
        report->frames.add(r.frame_ms);
        report->long_frames.add(r);
      }
    }
    report->exit_code = platform::k_exit_device_lost;
    report->reason = "device_lost";
    if (report->error.empty())
      report->error = std::string("device: lost at ") + gfx::device_lost_at_name(lost.at);
    report->device_lost = block;
    std::string error;
    if (!write_report(*report, &error))
      std::fprintf(stderr, "engine-view: report: %s\n", error.c_str());
    else
      std::fprintf(stderr, "engine-view: the report is in %s\n", report->path.c_str());
  }
  // 2. The vendor's dump, which the report names.
  if (!dump.empty()) {
    const std::string_view bytes(reinterpret_cast<const char*>(lost.fault.vendor_binary.data()),
                                 lost.fault.vendor_binary.size());
    if (io::write_file_atomic(dump, bytes) != io::Status::Ok)
      std::fprintf(stderr, "engine-view: cannot write %s\n", dump.c_str());
  }
  // 3. The input log, stamped as the loop's normal end stamps it, so a replay takes it (H1).
  if (save_lost_recording(held) != io::Status::Ok)
    std::fprintf(stderr, "engine-view: cannot write %s\n", held.record_input.c_str());
  // 4. The records, the largest, with the loss as their last line.
  if (!held.benchmark.empty()) {
    JsonValue last = JsonValue::object();
    last.set("device_lost", block);
    if (write_benchmark(held.benchmark, records,
                        write_json(last, JsonWriteOptions{.pretty = false})) != io::Status::Ok)
      std::fprintf(stderr, "engine-view: cannot write %s\n", held.benchmark.c_str());
  }
  // 5. The startup trace (written whole or not at all, core/profiling's writer).
  (void)startup_trace().finish(nullptr);
  log::flush();
  std::fflush(stdout);
  std::fflush(stderr);
  std::_Exit(platform::k_exit_device_lost);
}

}  // namespace engine::view

#include "report.h"

#include <core/json/json.h>
#include <core/platform/cpu_baseline.h>
#include <core/platform/cpu_features.h>
#include <foundation/io/vfs.h>

#include <engine_build_stamp.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

#include <algorithm>
#include <cmath>
#include <string>

namespace engine::view {

void FrameTimes::enable() {
  if (!bins_.empty()) return;
  bins_.resize(k_bins);
  for (u32& b : bins_)
    b = 0;
}

void FrameTimes::add(f64 ms) noexcept {
  if (bins_.empty() || !(ms >= 0.0)) return;
  ++count_;
  if (ms > worst_) worst_ = ms;
  const f64 bin = std::floor(ms / k_bin_ms);
  if (bin >= static_cast<f64>(k_bins)) {
    ++over_;
    return;
  }
  ++bins_[static_cast<u32>(bin)];
}

f64 FrameTimes::percentile(f64 fraction) const noexcept {
  if (count_ == 0) return 0.0;
  const f64 wanted = std::ceil(fraction * static_cast<f64>(count_));
  const u64 rank = wanted < 1.0 ? 1u : static_cast<u64>(wanted);
  u64 seen = 0;
  for (u32 b = 0; b < bins_.size(); ++b) {
    seen += bins_[b];
    if (seen >= rank) return std::min(static_cast<f64>(b + 1) * k_bin_ms, worst_);
  }
  return worst_;  // in the frames past the last bin
}

JsonValue FrameTimes::summary_json() const {
  JsonValue out = JsonValue::object();
  out.set("count", count_);
  out.set("median_ms", percentile(0.5));
  out.set("p95_ms", percentile(0.95));
  out.set("p99_ms", percentile(0.99));
  out.set("worst_ms", worst_);
  out.set("resolution_ms", k_bin_ms);
  return out;
}

std::string os_version() {
#if defined(_WIN32)
  // GetVersionEx answers with the version the executable's manifest claims to know; the kernel's
  // own RtlGetVersion answers with the one that is running.
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll != nullptr) {
    const auto get = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (get != nullptr && get(&info) == 0) {
      return "Windows " + std::to_string(info.dwMajorVersion) + "." +
             std::to_string(info.dwMinorVersion) + "." + std::to_string(info.dwBuildNumber);
    }
  }
  return "Windows";
#else
  utsname name{};
  if (uname(&name) != 0) return "unknown";
  return std::string(name.sysname) + " " + name.release + " " + name.machine;
#endif
}

JsonValue report_json(const RunReport& report) {
  JsonValue out = JsonValue::object();
  out.set("format", "engine.view.report");
  out.set("version", static_cast<u32>(1));

  JsonValue exit = JsonValue::object();
  exit.set("code", static_cast<i64>(report.exit_code));
  exit.set("reason", report.reason.empty() ? std::string(report.exit_code == 0 ? "done" : "error")
                                           : report.reason);
  exit.set("error", report.error);
  out.set("exit", std::move(exit));

  JsonValue machine = JsonValue::object();
  machine.set("adapter", report.adapter);
  machine.set("driver", report.driver);
  machine.set("vulkan", report.vulkan);
  machine.set("tier", report.tier);
  machine.set("raster", report.raster);
  machine.set("shadows", report.shadows);
  machine.set("device", report.have_device);
  machine.set("os", os_version());
  machine.set("cpu", std::string(platform::cpu_features().brand));
  machine.set("cpu_baseline", platform::cpu_baseline_name(platform::k_build_cpu_baseline));
  JsonValue build = JsonValue::object();
  build.set("commit", build_stamp::commit());
  build.set("dirty", build_stamp::dirty());
  machine.set("build", std::move(build));
  out.set("machine", std::move(machine));

  JsonValue frames = report.frames.summary_json();
  frames.set("width", report.width);
  frames.set("height", report.height);
  out.set("frames", std::move(frames));

  JsonValue walk = JsonValue::object();
  walk.set("walked", report.have_walk);
  walk.set("distance_m", report.walk_distance_m);
  walk.set("seconds", report.walk_seconds);
  walk.set("drops", report.walk_drops);
  out.set("walk", std::move(walk));

  out.set("game", report.game);

  // The summary as the run printed it: parsed, so a field the summary gains reaches the report
  // with no change here; null for a run that ended before it had one.
  JsonValue summary;
  if (!report.summary_line.empty() && !parse_json(report.summary_line, summary).ok)
    summary = JsonValue(report.summary_line);
  out.set("summary", std::move(summary));
  return out;
}

bool write_report(const RunReport& report, std::string* error) {
  if (report.path.empty()) return true;
  const std::string text = write_json(report_json(report), JsonWriteOptions{.pretty = true}) + "\n";
  const io::Status status = io::write_file(report.path, text);
  if (status != io::Status::Ok) {
    if (error != nullptr)
      *error = "cannot write " + report.path + ": " + std::string(io::status_name(status));
    return false;
  }
  return true;
}

}  // namespace engine::view

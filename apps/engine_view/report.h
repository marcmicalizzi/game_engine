#pragma once

// **`engine-view --report <file.json>`** (docs/subsystems/apps.md, "--report: what a tester sends
// back"): one file, written at exit whether the run succeeded or not, holding what somebody on
// another machine sends back after a session — the summary line the run already prints, the
// machine it ran on, how its frames went, how far the walker walked, what a `--game` module said
// and why the run ended.
//
// **Why one file and why this shape.** A tester who unzipped a package (tools/package-game.ps1)
// has no checkout, no PowerShell 7 and no idea which of a dozen lines on a console matters; what
// comes back has to be one attachment that answers "what ran, on what, how well, and how did it
// end" without a second round trip. The summary is not re-described here: the report embeds the
// summary line itself, parsed, so a field the summary gains reaches the report with no change
// (the summary's writer is the one writer of what a run measured). What the summary cannot say —
// the driver, the OS, a run that failed before it had a summary, the frame-time spread of a live
// session with no `--benchmark` — is what this file adds.
//
// **Frame times cost nothing a frame.** A live session can run for an hour, so the frames are
// counted into a fixed histogram of 0.1 ms bins (10,000 of them, 40 KB, allocated once when the
// report is asked for) rather than kept; the percentiles are the bins', which is a tenth of a
// millisecond of resolution for a figure a tester reads to the millisecond, and the worst frame
// is kept exactly.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>

#include <string>

namespace engine::view {

class FrameTimes {
 public:
  // Allocates the bins; a report nobody asked for never calls it, and `add` is then a no-op.
  void enable();
  bool enabled() const noexcept { return !bins_.empty(); }
  void add(f64 ms) noexcept;
  u64 count() const noexcept { return count_; }
  // The millisecond under which `fraction` of the frames fell (the upper edge of its bin).
  f64 percentile(f64 fraction) const noexcept;
  f64 worst() const noexcept { return worst_; }
  JsonValue summary_json() const;

  static constexpr u32 k_bins = 10'000;  // 0.1 ms each, to a second
  static constexpr f64 k_bin_ms = 0.1;

 private:
  Vector<u32> bins_;
  u64 over_ = 0;  // frames past the last bin (a second or more)
  u64 count_ = 0;
  f64 worst_ = 0.0;
};

// What engine-view fills as it runs, and `write_report` writes at exit.
struct RunReport {
  std::string path;  // --report; empty: no report
  // The summary line as printed (empty when the run ended before it had one).
  std::string summary_line;
  i32 exit_code = 0;
  // Why the run ended: "frames" (--frames ran out), "session" (a replay's or an injected session's
  // last tick), "path" (a camera path's last frame), "escape", "closed" (the window), "end_run"
  // (the game ended it), "game_error", or "error" (`error` says which stage and why).
  std::string reason;
  std::string error;  // the first failure's line: "<stage>: <why>"
  // The device, once one was made.
  bool have_device = false;
  std::string adapter;
  std::string driver;
  std::string vulkan;
  std::string tier;    // the device's verdict: "raster", "rt" or "rt-cluster"
  std::string raster;  // what the session drew with ("mesh", "vertex", ...)
  std::string shadows;
  u32 width = 0;
  u32 height = 0;
  FrameTimes frames;
  // The walk (walk.h), when the session had a walker.
  bool have_walk = false;
  f64 walk_distance_m = 0.0;
  f64 walk_seconds = 0.0;
  u32 walk_drops = 0;
  // The game's block (game_runner.h), or null.
  JsonValue game;
};

// The OS as one line ("Windows 10.0.26200", "Linux 6.8.0-45-generic"), read from the system, not
// from what the binary was built for.
std::string os_version();
// The report as JSON (format `engine.view.report`, version 1).
JsonValue report_json(const RunReport& report);
// Writes `report_json` to `report.path`, pretty, best effort: false with `error` when it cannot.
bool write_report(const RunReport& report, std::string* error);

}  // namespace engine::view

#pragma once

// What else the machine was doing while a measurement was taken (docs/subsystems/bench.md,
// "Measuring on a shared machine"; docs/plan/11-performance-principles.md §11.8).
//
// The development machine is not an idle benchmark rig: it runs GPU jobs and several agent
// builds at once, and a number taken beside them is an upper bound on the cost, not the cost.
// A harness that does not say so hands a budget decision a figure nobody can reproduce, so the
// harness records the machine's state around every run, says when it was busy, and can refuse
// to measure or wait until it is quiet.
//
// What is recorded, and nothing else: CPU busy percentage of the whole machine and of this
// process (from which "others" follows by subtraction), GPU utilization and memory, and whether
// the Windows session is locked. **No process names, no command lines, no user names** — the
// question is "how loaded is this box", and the answer does not need to say by whom.
//
//     const MachineState s = sample_machine_state(k_sample_window_ms);
//     if (!is_quiet(s, QuietThresholds{})) { ... }
//
// Every field is optional: a machine with no nvidia-smi on PATH reports no GPU, and a platform
// with no session lock to read reports Tristate::Unknown rather than "unlocked". A reader must
// be able to tell "quiet" from "not measured".

#include <core/base/types.h>

#include <cstdio>
#include <string>

namespace engine {
class JsonValue;
}

namespace engine::bench {

// How long the CPU counters are sampled over. Long enough that a scheduler tick or two does not
// dominate, short enough to pay at the ends of a run rather than inside it.
inline constexpr i64 k_sample_window_ms = 250;

// "we could not tell" and "no" are different answers, and a caveat line that says "unlocked" on
// a machine that has no lock to read is worse than one that says nothing.
enum class Tristate : u8 { Unknown = 0, No = 1, Yes = 2 };

struct MachineState {
  // Percentages of the whole machine: 100% is every logical CPU busy for the whole window.
  bool cpu_valid = false;
  f64 cpu_total_pct = 0.0;   // everything running, this process included
  f64 cpu_own_pct = 0.0;     // this process alone
  f64 cpu_others_pct = 0.0;  // total - own, clamped at zero

  bool gpu_valid = false;
  f64 gpu_util_pct = 0.0;
  u64 gpu_memory_used_mib = 0;
  u64 gpu_memory_total_mib = 0;

  // Yes when a lock screen is up (Windows: a LogonUI.exe process exists). Unknown elsewhere.
  Tristate session_locked = Tristate::Unknown;
};

// Above either of these, the numbers of a run are upper bounds and the harness says so. The
// defaults are argued for in docs/subsystems/bench.md; they are deliberately loose enough that
// an idle desktop's own background noise does not trip them.
struct QuietThresholds {
  f64 others_cpu_pct = 10.0;
  f64 gpu_util_pct = 20.0;
};

// False when another process is using more than the thresholds allow. An unknown field cannot
// make a machine noisy: a box with no GPU reading is quiet as far as its GPU is concerned,
// because refusing to measure on missing information would make the flag useless on Linux CI.
bool is_quiet(const MachineState& state, const QuietThresholds& thresholds) noexcept;

// Field-wise maximum, for reporting the worst of the samples a run took.
MachineState worst_of(const MachineState& a, const MachineState& b) noexcept;

// One line for a human: "cpu 12.3% (others 9.1%), gpu 41% util 14336/32607 MiB, session
// unlocked". Unknown fields are named as unknown rather than left out.
std::string describe(const MachineState& state);

// The `machine_state` object of a report: every field, with null where it is unknown.
JsonValue machine_state_json(const MachineState& state);

// One WARNING line to `out` when the machine was busier than the thresholds allow; returns
// whether it wrote one. Every consumer of a machine state warns with this function, so the
// sentence a write-up has to explain is the same everywhere.
bool warn_if_busy(const MachineState& state, const QuietThresholds& thresholds, std::FILE* out);

// Injectable so that the quiet/not-quiet logic can be tested without a machine that cooperates:
// a fake sampler returns whatever a case needs, and the run under test takes the same path it
// would take on a loaded box.
class MachineSampler {
 public:
  MachineSampler() noexcept = default;
  virtual ~MachineSampler();
  MachineSampler(const MachineSampler&) = delete;
  MachineSampler& operator=(const MachineSampler&) = delete;

  // Samples the CPU counters over `window_ms` (0 samples nothing and leaves cpu_valid false)
  // and reads whatever else the platform offers.
  virtual MachineState sample(i64 window_ms) = 0;
};

// The real one: GetSystemTimes/GetProcessTimes on Windows, /proc/stat and /proc/self/stat on
// Linux, nvidia-smi for the GPU when it is on PATH.
MachineSampler& system_sampler();

// system_sampler().sample(window_ms).
MachineState sample_machine_state(i64 window_ms);

}  // namespace engine::bench

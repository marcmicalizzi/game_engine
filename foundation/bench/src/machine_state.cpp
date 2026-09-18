#include <core/json/json_value.h>
#include <core/platform/process.h>
#include <foundation/bench/machine_state.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <string_view>
#include <thread>

#if ENGINE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <tlhelp32.h>
// clang-format on
#else
#include <cstdio>
#endif

namespace engine::bench {

namespace {

// ---- platform CPU counters ---------------------------------------------------------------------

// One reading of the two counters a sample needs, in whatever unit the platform counts in. The
// unit cancels: every percentage below is a ratio of two deltas taken from the same counter.
struct CpuCounters {
  bool valid = false;
  u64 machine_total = 0;  // every logical CPU, busy and idle
  u64 machine_idle = 0;
  u64 own_busy = 0;  // this process, user + kernel
};

#if ENGINE_PLATFORM_WINDOWS

u64 to_u64(const FILETIME& t) noexcept {
  return (static_cast<u64>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

CpuCounters read_cpu_counters() noexcept {
  CpuCounters c;
  FILETIME idle{};
  FILETIME kernel{};
  FILETIME user{};
  if (GetSystemTimes(&idle, &kernel, &user) == 0) return c;
  FILETIME created{};
  FILETIME exited{};
  FILETIME own_kernel{};
  FILETIME own_user{};
  if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &own_kernel, &own_user) == 0) {
    return c;
  }
  // The kernel time Windows reports includes the idle time, so it is already the total.
  c.machine_total = to_u64(kernel) + to_u64(user);
  c.machine_idle = to_u64(idle);
  c.own_busy = to_u64(own_kernel) + to_u64(own_user);
  c.valid = true;
  return c;
}

// A lock screen is a LogonUI.exe process. The process list is walked for that one name and
// nothing is kept from it but a boolean: no name, no pid, no owner leaves this function.
Tristate read_session_locked() noexcept {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return Tristate::Unknown;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  Tristate result = Tristate::No;
  if (Process32FirstW(snapshot, &entry) != 0) {
    do {
      if (_wcsicmp(entry.szExeFile, L"LogonUI.exe") == 0) {
        result = Tristate::Yes;
        break;
      }
    } while (Process32NextW(snapshot, &entry) != 0);
  } else {
    result = Tristate::Unknown;
  }
  CloseHandle(snapshot);
  return result;
}

#elif ENGINE_PLATFORM_LINUX

bool read_file(const char* path, char* buffer, usize capacity) noexcept {
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) return false;
  const usize n = std::fread(buffer, 1, capacity - 1, f);
  std::fclose(f);
  buffer[n] = '\0';
  return n > 0;
}

// Reads whitespace-separated integers out of `text`, at most `count` of them, as magnitudes: a
// leading '-' is skipped, because /proc/self/stat's tpgid is -1 with no controlling terminal and
// none of the fields this reads for their value can be negative. Returns how many it read.
u32 scan_u64s(std::string_view text, u64* out, u32 count) noexcept {
  u32 found = 0;
  usize i = 0;
  while (found < count && i < text.size()) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
      ++i;
    if (i < text.size() && text[i] == '-') ++i;
    const usize start = i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9')
      ++i;
    if (i == start) break;
    u64 value = 0;
    const auto r = std::from_chars(text.data() + start, text.data() + i, value);
    if (r.ec != std::errc{}) break;
    out[found++] = value;
  }
  return found;
}

CpuCounters read_cpu_counters() noexcept {
  CpuCounters c;
  char buffer[4096];

  // /proc/stat's first line: "cpu user nice system idle iowait irq softirq steal guest ...",
  // in USER_HZ jiffies summed over every CPU.
  if (!read_file("/proc/stat", buffer, sizeof(buffer))) return c;
  std::string_view stat(buffer);
  const usize newline = stat.find('\n');
  std::string_view first =
      stat.substr(0, newline == std::string_view::npos ? stat.size() : newline);
  if (first.substr(0, 4) != "cpu ") return c;
  u64 fields[10] = {};
  const u32 field_count = scan_u64s(first.substr(4), fields, 10);
  if (field_count < 5) return c;
  u64 total = 0;
  for (u32 i = 0; i < field_count; ++i)
    total += fields[i];
  c.machine_total = total;
  c.machine_idle = fields[3] + fields[4];  // idle + iowait

  // /proc/self/stat: utime is field 14 and stime field 15, both in the same jiffies. The
  // command name is field 2 and may hold spaces and parentheses, so the scan starts after its
  // closing one; field 3 is the single-letter run state, which is skipped as a token rather
  // than parsed. The numbers then start at field 4 (ppid), so utime and stime are at 10 and 11.
  if (!read_file("/proc/self/stat", buffer, sizeof(buffer))) return c;
  std::string_view self(buffer);
  const usize close = self.rfind(')');
  if (close == std::string_view::npos) return c;
  std::string_view rest = self.substr(close + 1);
  usize at = rest.find_first_not_of(' ');
  if (at == std::string_view::npos) return c;
  at = rest.find(' ', at);  // past the run-state letter
  if (at == std::string_view::npos) return c;
  u64 own[12] = {};
  if (scan_u64s(rest.substr(at), own, 12) < 12) return c;
  c.own_busy = own[10] + own[11];
  c.valid = true;
  return c;
}

// Nothing portable to read: a headless Linux box has no session to lock, and a desktop's
// screen-lock state lives in whichever session manager it runs.
Tristate read_session_locked() noexcept { return Tristate::Unknown; }

#else

CpuCounters read_cpu_counters() noexcept { return CpuCounters{}; }
Tristate read_session_locked() noexcept { return Tristate::Unknown; }

#endif

f64 percent_of(u64 part, u64 whole) noexcept {
  if (whole == 0) return 0.0;
  return 100.0 * static_cast<f64>(part) / static_cast<f64>(whole);
}

// ---- the GPU, through nvidia-smi ----------------------------------------------------------------

// NVML would be the direct answer and it is a dependency this engine does not take (AGENTS.md,
// third-party policy): a bench tool may shell out to a driver utility that is already installed,
// and a machine without it reports nothing rather than failing. The first GPU's line is the one
// read; a multi-GPU box would need a flag to say which one the measurement cares about.
bool read_gpu(MachineState& state) {
  const std::string_view argv[] = {
      "nvidia-smi",
      "--query-gpu=utilization.gpu,memory.used,memory.total",
      "--format=csv,noheader,nounits",
  };
  platform::Process process;
  if (!process.spawn(argv, nullptr, /*merge_stderr=*/false)) return false;
  process.close_stdin();
  std::string line;
  const bool have_line = process.read_line(line);
  std::string ignored;
  (void)process.read_all(ignored);
  const i32 code = process.wait();
  if (!have_line || code != 0) return false;

  // "41, 14336, 32607"
  f64 values[3] = {0, 0, 0};
  u32 found = 0;
  usize i = 0;
  while (found < 3 && i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == ',' || line[i] == '\r'))
      ++i;
    const usize start = i;
    while (i < line.size() && line[i] != ',' && line[i] != ' ' && line[i] != '\r')
      ++i;
    if (i == start) break;
    f64 value = 0;
    const auto r = std::from_chars(line.data() + start, line.data() + i, value);
    if (r.ec != std::errc{}) return false;  // "[N/A]" on a GPU that reports no utilization
    values[found++] = value;
  }
  if (found < 3) return false;
  state.gpu_valid = true;
  state.gpu_util_pct = values[0];
  state.gpu_memory_used_mib = static_cast<u64>(values[1]);
  state.gpu_memory_total_mib = static_cast<u64>(values[2]);
  return true;
}

class SystemSampler final : public MachineSampler {
 public:
  MachineState sample(i64 window_ms) override {
    MachineState state;
    if (window_ms > 0) {
      const CpuCounters before = read_cpu_counters();
      std::this_thread::sleep_for(std::chrono::milliseconds(window_ms));
      const CpuCounters after = read_cpu_counters();
      if (before.valid && after.valid && after.machine_total > before.machine_total) {
        const u64 total = after.machine_total - before.machine_total;
        const u64 idle = after.machine_idle >= before.machine_idle
                             ? after.machine_idle - before.machine_idle
                             : 0;
        const u64 own =
            after.own_busy >= before.own_busy ? after.own_busy - before.own_busy : u64{0};
        state.cpu_valid = true;
        state.cpu_total_pct = percent_of(total > idle ? total - idle : 0, total);
        state.cpu_own_pct = std::min(percent_of(own, total), state.cpu_total_pct);
        state.cpu_others_pct = std::max(0.0, state.cpu_total_pct - state.cpu_own_pct);
      }
    }
    (void)read_gpu(state);
    state.session_locked = read_session_locked();
    return state;
  }
};

}  // namespace

MachineSampler::~MachineSampler() = default;

MachineSampler& system_sampler() {
  static SystemSampler sampler;
  return sampler;
}

MachineState sample_machine_state(i64 window_ms) { return system_sampler().sample(window_ms); }

bool is_quiet(const MachineState& state, const QuietThresholds& thresholds) noexcept {
  if (state.cpu_valid && state.cpu_others_pct > thresholds.others_cpu_pct) return false;
  if (state.gpu_valid && state.gpu_util_pct > thresholds.gpu_util_pct) return false;
  return true;
}

MachineState worst_of(const MachineState& a, const MachineState& b) noexcept {
  MachineState w;
  w.cpu_valid = a.cpu_valid || b.cpu_valid;
  if (w.cpu_valid) {
    const MachineState& x = a.cpu_valid ? a : b;
    const MachineState& y = b.cpu_valid ? b : a;
    w.cpu_total_pct = std::max(x.cpu_total_pct, y.cpu_total_pct);
    w.cpu_own_pct = std::max(x.cpu_own_pct, y.cpu_own_pct);
    w.cpu_others_pct = std::max(x.cpu_others_pct, y.cpu_others_pct);
  }
  w.gpu_valid = a.gpu_valid || b.gpu_valid;
  if (w.gpu_valid) {
    const MachineState& x = a.gpu_valid ? a : b;
    const MachineState& y = b.gpu_valid ? b : a;
    w.gpu_util_pct = std::max(x.gpu_util_pct, y.gpu_util_pct);
    w.gpu_memory_used_mib = std::max(x.gpu_memory_used_mib, y.gpu_memory_used_mib);
    w.gpu_memory_total_mib = std::max(x.gpu_memory_total_mib, y.gpu_memory_total_mib);
  }
  w.session_locked =
      a.session_locked == Tristate::Yes || b.session_locked == Tristate::Yes
          ? Tristate::Yes
          : (a.session_locked == Tristate::Unknown ? b.session_locked : a.session_locked);
  return w;
}

std::string describe(const MachineState& state) {
  char buffer[256];
  std::string out;
  if (state.cpu_valid) {
    std::snprintf(buffer, sizeof(buffer), "cpu %.1f%% (others %.1f%%, own %.1f%%)",
                  state.cpu_total_pct, state.cpu_others_pct, state.cpu_own_pct);
    out.append(buffer);
  } else {
    out.append("cpu unknown");
  }
  if (state.gpu_valid) {
    std::snprintf(buffer, sizeof(buffer), ", gpu %.0f%% util %llu/%llu MiB", state.gpu_util_pct,
                  static_cast<unsigned long long>(state.gpu_memory_used_mib),
                  static_cast<unsigned long long>(state.gpu_memory_total_mib));
    out.append(buffer);
  } else {
    out.append(", gpu unknown");
  }
  switch (state.session_locked) {
    case Tristate::Yes: out.append(", session locked"); break;
    case Tristate::No: out.append(", session unlocked"); break;
    case Tristate::Unknown: out.append(", session unknown"); break;
  }
  return out;
}

JsonValue machine_state_json(const MachineState& state) {
  JsonValue o = JsonValue::object();
  if (state.cpu_valid) {
    o.set("cpu_total_pct", state.cpu_total_pct);
    o.set("cpu_own_pct", state.cpu_own_pct);
    o.set("cpu_others_pct", state.cpu_others_pct);
  } else {
    o.set("cpu_total_pct", JsonValue::null());
    o.set("cpu_own_pct", JsonValue::null());
    o.set("cpu_others_pct", JsonValue::null());
  }
  if (state.gpu_valid) {
    o.set("gpu_util_pct", state.gpu_util_pct);
    o.set("gpu_memory_used_mib", state.gpu_memory_used_mib);
    o.set("gpu_memory_total_mib", state.gpu_memory_total_mib);
  } else {
    o.set("gpu_util_pct", JsonValue::null());
    o.set("gpu_memory_used_mib", JsonValue::null());
    o.set("gpu_memory_total_mib", JsonValue::null());
  }
  switch (state.session_locked) {
    case Tristate::Yes: o.set("session_locked", true); break;
    case Tristate::No: o.set("session_locked", false); break;
    case Tristate::Unknown: o.set("session_locked", JsonValue::null()); break;
  }
  return o;
}

bool warn_if_busy(const MachineState& state, const QuietThresholds& thresholds, std::FILE* out) {
  const bool cpu_busy = state.cpu_valid && state.cpu_others_pct > thresholds.others_cpu_pct;
  const bool gpu_busy = state.gpu_valid && state.gpu_util_pct > thresholds.gpu_util_pct;
  if (!cpu_busy && !gpu_busy) return false;
  std::fprintf(out,
               "WARNING: other processes used %.1f%% of the CPU and the GPU was %.0f%% busy; "
               "these numbers are upper bounds (%s)\n",
               state.cpu_valid ? state.cpu_others_pct : 0.0,
               state.gpu_valid ? state.gpu_util_pct : 0.0, describe(state).c_str());
  return true;
}

}  // namespace engine::bench

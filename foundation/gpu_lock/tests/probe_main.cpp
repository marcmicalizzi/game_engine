// engine_gpu_lock_probe: a process that holds the GPU lock the way a process with a GPU device
// does (foundation/gpu_lock/device_hold.h), for the tests that need more than one process — a
// parent and its child, a holder that crashes, a wait that gives up with its exit code, and a
// command wrapped in `tools/gpu-lock.ps1 run`. It opens no device: the hold is the whole point.
//
//   engine_gpu_lock_probe [--on] [--lock <path>] [--lease <s>] [--refresh <s>] [--poll <s>]
//                         [--report <file>] <command>
//
//   --on       sets ENGINE_GPU_LOCK_ON_DEVICE=1 in this process first, as the test main does, so
//              the hold takes the lock and every child inherits the switch
//   --report   appends "<pid> <mode>" once the hold is established, so a test can see what a
//              process that is not its direct child decided
//
//   hold <ms>                   hold for <ms>, then release and exit 0
//   parent <ms> -- <argv...>    hold, run this probe with <argv> as a child and wait for it,
//                               hold <ms> more, release, exit with the child's code
//   parent-dies -- <argv...>    hold, start the child, wait until it has reported, and exit 0
//                               without releasing: a crash, as far as the lock can tell
//   cycles <n>                  take and release a free lock n times and print the mean cost
//   spawn -- <argv...>          hold nothing; run the child and exit with its code (a child that
//                               gave up ends this process with 75 first, in platform::Process)
#include <core/platform/cpu_baseline.h>
#include <core/platform/process.h>
#include <foundation/gpu_lock/device_hold.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace engine;

namespace {

u64 own_pid() {
#if defined(_WIN32)
  return static_cast<u64>(::GetCurrentProcessId());
#else
  return static_cast<u64>(::getpid());
#endif
}

void set_env(const char* name, const char* value) {
#if defined(_WIN32)
  (void)::SetEnvironmentVariableA(name, value);
  (void)_putenv_s(name, value);
#else
  (void)::setenv(name, value, 1);
#endif
}

std::string executable() {
#if defined(_WIN32)
  char buffer[4096] = {};
  const DWORD n = ::GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(sizeof(buffer)));
  return std::string(buffer, n);
#else
  char buffer[4096] = {};
  const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  return n > 0 ? std::string(buffer, static_cast<size_t>(n)) : std::string();
#endif
}

void report(const std::string& path, gpu_lock::HoldMode mode) {
  if (path.empty()) return;
  std::ofstream out(path, std::ios::app);
  out << own_pid() << ' ' << gpu_lock::to_string(mode) << '\n';
}

bool reported_by_other(const std::string& path) {
  std::ifstream in(path);
  std::string line;
  const std::string me = std::to_string(own_pid()) + ' ';
  while (std::getline(in, line)) {
    if (line.rfind(me, 0) != 0) return true;
  }
  return false;
}

int usage() {
  std::fprintf(stderr,
               "usage: engine_gpu_lock_probe [--on] [--lock <path>] [--lease <s>] "
               "[--refresh <s>] [--poll <s>] [--report <file>] "
               "hold <ms> | parent <ms> -- <argv> | parent-dies -- <argv> | cycles <n> | "
               "spawn -- <argv>\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  platform::require_cpu_baseline();
  std::string lock;
  std::string report_path;
  i64 lease = 0;
  i64 refresh = 0;
  i64 poll = 0;
  int i = 1;
  for (; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--on") {
      set_env(gpu_lock::k_env_on_device, "1");
    } else if (a == "--lock") {
      lock = next();
    } else if (a == "--lease") {
      lease = std::atoll(next());
    } else if (a == "--refresh") {
      refresh = std::atoll(next());
    } else if (a == "--poll") {
      poll = std::atoll(next());
    } else if (a == "--report") {
      report_path = next();
    } else {
      break;
    }
  }
  if (i >= argc) return usage();
  const std::string_view command = argv[i++];
  i64 ms = 0;
  if ((command == "hold" || command == "parent") && i < argc) ms = std::atoll(argv[i++]);
  const std::string self = executable();  // outlives `child`, which only views it
  std::vector<std::string_view> child;
  if (i < argc && std::string_view(argv[i]) == "--") {
    child.push_back(self);
    for (++i; i < argc; ++i)
      child.push_back(argv[i]);
  }

  gpu_lock::HoldConfig config = gpu_lock::hold_config_from_environment();
  if (!lock.empty()) config.path = lock;
  if (lease > 0) config.lease_s = lease;
  if (refresh > 0) config.refresh_s = refresh;
  if (poll > 0) config.poll_s = poll;
  config.purpose = "gpu lock probe: " + std::string(command);

  auto run_child = [&](platform::Process& p) {
    std::string error;
    if (!p.spawn(std::span<const std::string_view>(child.data(), child.size()), &error)) {
      std::fprintf(stderr, "probe: cannot start the child: %s\n", error.c_str());
      std::exit(3);
    }
    p.close_stdin();
  };

  if (command == "cycles") {
    // What a hold costs a device: n takes and releases of a free lock, timed. Not a test; how
    // docs/subsystems/gpu_lock.md's performance note was measured.
    const i64 n = i < argc ? std::atoll(argv[i]) : 100;
    const auto began = std::chrono::steady_clock::now();
    for (i64 k = 0; k < n; ++k) {
      gpu_lock::DeviceHold cycle;
      (void)cycle.acquire(config);
      cycle.release();
    }
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - began)
                        .count();
    std::printf("cycles=%lld mean_us=%.1f\n", static_cast<long long>(n),
                static_cast<double>(ns) / 1000.0 / static_cast<double>(n > 0 ? n : 1));
    return 0;
  }
  if (command == "spawn") {
    if (child.size() < 2) return usage();
    platform::Process p;
    run_child(p);
    return p.wait();
  }

  gpu_lock::DeviceHold hold;
  const gpu_lock::HoldMode mode = hold.acquire(config);
  report(report_path, mode);

  if (command == "hold") {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    hold.release();
    return 0;
  }
  if (command == "parent") {
    if (child.size() < 2) return usage();
    platform::Process p;
    run_child(p);
    const i32 code = p.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    hold.release();
    return code;
  }
  if (command == "parent-dies") {
    if (child.size() < 2 || report_path.empty()) return usage();
    platform::Process p;
    run_child(p);
    for (int tries = 0; tries < 200 && !reported_by_other(report_path); ++tries)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::fflush(nullptr);
    std::_Exit(0);  // no release, no keeper join: the lock is left as a crash leaves it
  }
  return usage();
}

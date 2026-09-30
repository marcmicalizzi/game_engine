#pragma once

// The machine-wide GPU lock held for exactly as long as this process has a GPU device open
// (docs/subsystems/gpu_lock.md, ADR-0049).
//
// `gfx::Device::create` acquires a `DeviceHold` just before `vkCreateDevice`, and
// `gfx::Device::destroy` releases it just after `vkDestroyDevice`. Every process that opens a
// device — a test executable through `gfx_test::open_device` or the renderer's rigs,
// `engine-view`, `engine-host` started by an end-to-end test or by `engine-cli` or `engine-mcp`
// — opens it there, so that is the one place the lock is taken for a device, and a test that
// never opens a device never touches it. `window::Window::create` takes the same hold before a
// Vulkan window appears (and `destroy()` leaves it), so that a process that has to wait does it
// with nothing on screen rather than with a window that answers nothing.
//
// It happens only when the switch is on: `ENGINE_GPU_LOCK_ON_DEVICE=1`. The shared test main
// (tests/support/test_main.cpp) sets it for every test executable unless it is already set, and
// the processes a test starts inherit it; nothing else sets it, so the owner's own `engine-view`
// and `engine-host` sessions never take the lock and never wait for it. `0` in the environment
// turns it off for a run.
//
// **One hold per process, counted by device.** The first device takes the lock (waiting for it if
// somebody else holds it), later ones are counted, and the last one to go releases it. While the
// process holds the lock a keeper thread refreshes a short lease (k_device_lease_s, every
// k_device_refresh_s), so a crash leaves the GPU spoken for for at most one lease; the keeper
// starts with the hold and is joined before the release returns, so no thread outlives the
// device it keeps the lock for.
//
// **Children.** While it holds the lock the process sets `ENGINE_GPU_LOCK_HOLDER` to its own pid
// (and puts the old value back afterwards), so a child that opens a device finds the lock held
// on its behalf and neither waits nor releases: its parent does. The same variable is how
// `tools/gpu-lock.ps1 run` marks a whole command as held, so a test run somebody still wraps in
// it sees the lock as its own everywhere. A child in that position watches the lock at the
// keeper's interval, and if its parent's hold goes away while it still has a device — the parent
// died, or released first — it takes the lock in its own name if it is free or expired.
//
// **Giving up.** A wait ends at an absolute deadline, `ENGINE_GPU_LOCK_WAIT_UNTIL` (Unix seconds),
// which the test main sets to its start plus the queue budget CMake gives it, so one test and
// everything it starts share one budget, inside the test's CTest timeout. A process whose wait
// runs out prints who held the lock and exits with `k_exit_gave_up` (75, the value of
// `platform::k_exit_gpu_lock_gave_up`) before it has used the GPU; `platform::Process::wait()`
// ends a parent that sees a child do that the same way, so the code reaches the test executable
// at the root, and CTest reports that test as skipped (`SKIP_RETURN_CODE 75`), not failed.

#include <core/base/types.h>
#include <core/platform/process.h>
#include <foundation/gpu_lock/gpu_lock.h>

#include <cstdio>
#include <string>

namespace engine::gpu_lock {

// The switch: "1" takes the lock when a device is opened; unset or anything else does not.
inline constexpr const char* k_env_on_device = "ENGINE_GPU_LOCK_ON_DEVICE";
// The absolute deadline of every wait in this process tree, in Unix seconds.
inline constexpr const char* k_env_wait_until = "ENGINE_GPU_LOCK_WAIT_UNTIL";
// Who holds the lock on this process's behalf (a pid), set for children while a hold lasts.
inline constexpr const char* k_env_holder = "ENGINE_GPU_LOCK_HOLDER";
// A file to append one JSON line to per hold and per give-up: how the suite's holds are measured.
inline constexpr const char* k_env_log = "ENGINE_GPU_LOCK_LOG";

// The lease a device hold writes, and how often its keeper refreshes it: a crash blocks the GPU
// for two minutes at most, and three refreshes can be missed before the lease runs out.
inline constexpr i64 k_device_lease_s = 120;
inline constexpr i64 k_device_refresh_s = 30;
// How often a waiter looks again: the low end of GPU-LOCK.md's 15-30 s, because the holds it
// waits behind are short.
inline constexpr i64 k_device_poll_s = 15;
// How long a process tree waits when nobody set a deadline: GPU-LOCK.md rule 9's half hour.
inline constexpr i64 k_default_wait_s = 1800;
inline constexpr i32 k_exit_gave_up = platform::k_exit_gpu_lock_gave_up;

struct HoldConfig {
  bool enabled = false;  // the switch
  std::string path;      // the lock file; empty: default_path()
  Identity self;         // who takes it
  std::string purpose;   // one line; empty: "gpu device: <executable> in <directory>"
  i64 lease_s = k_device_lease_s;
  i64 refresh_s = k_device_refresh_s;
  i64 poll_s = k_device_poll_s;
  i64 wait_until_unix_s = 0;    // 0: k_default_wait_s after the first wait begins
  std::string log_path;         // empty: no hold log
  std::FILE* out = stderr;      // the waiting, taking-over and giving-up lines; null: silent
  bool exit_on_give_up = true;  // false only for tests of the give-up itself
};

// The configuration `gfx::Device` uses: the switch, the path, the deadline and the log from the
// environment, this process's identity, and the default purpose.
HoldConfig hold_config_from_environment();

// What this process's hold is.
enum class HoldMode : u8 {
  Off,          // the switch is off, or no device is open
  Taken,        // this process wrote the lock file and releases it with its last device
  Parents,      // held on this process's behalf by the process ENGINE_GPU_LOCK_HOLDER names
  InProcess,    // held by this process through another path (the bench harness's --gpu-lock)
  NoDirectory,  // the lock's directory does not exist: this machine does not use the protocol
  GaveUp,       // the wait ran out (returned only when exit_on_give_up is false)
  Error,        // the file could not be written: runs without the lock, and said so
};

const char* to_string(HoldMode mode) noexcept;

// One device's share of this process's hold. Not thread-affine: acquire and release may happen
// on different threads, and several devices on several threads share one hold.
class DeviceHold {
 public:
  DeviceHold() noexcept = default;
  ~DeviceHold() { release(); }
  DeviceHold(const DeviceHold&) = delete;
  DeviceHold& operator=(const DeviceHold&) = delete;

  // Joins this process's hold, taking the lock first if this is the only device. May wait; may
  // end the process with k_exit_gave_up (see above). Returns the process's mode afterwards.
  HoldMode acquire();
  HoldMode acquire(const HoldConfig& config);
  // Leaves it; the last device out releases the lock. Idempotent.
  void release() noexcept;

  bool counted() const noexcept { return counted_; }

 private:
  bool counted_ = false;
};

// The process's hold as it stands: how many devices share it, what it is, and whether a keeper
// thread is running for it. For tests and reports.
struct HoldStatus {
  u32 devices = 0;
  HoldMode mode = HoldMode::Off;
  bool keeper_running = false;
  u64 parent_pid = 0;  // Parents: who holds it for this process
};
HoldStatus hold_status();

}  // namespace engine::gpu_lock

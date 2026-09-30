#include "lock_file.h"

#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/platform/thread.h>
#include <core/time/time.h>
#include <foundation/gpu_lock/device_hold.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

namespace engine::gpu_lock {

namespace {

namespace fs = std::filesystem;

// This process's hold: one per process however many devices share it. Allocated once and never
// destroyed, so a device that outlives static destruction (a static `gfx::Device`) still finds
// it; `release_at_exit` is what lets the lock go when the process ends normally.
struct ProcessHold {
  std::mutex mutex;
  std::condition_variable wake;
  u32 devices = 0;
  HoldMode mode = HoldMode::Off;
  HoldConfig config;  // as the first device gave it

  // Taken: the file this process wrote.
  std::string path;
  std::string started;
  i64 expires_s = 0;
  i64 taken_ms = 0;   // wall clock, for the hold log
  i64 waited_ms = 0;  // how long the first device waited for it
  bool set_holder_env = false;
  std::string previous_holder;  // ENGINE_GPU_LOCK_HOLDER before this hold set it

  // Parents: who holds it for this process.
  u64 parent_pid = 0;

  std::thread keeper;
  bool keeper_running = false;
  bool stop = false;
  bool exit_handler = false;
};

ProcessHold& process_hold() {
  static ProcessHold* hold = new ProcessHold();
  return *hold;
}

i64 wall_ms() { return time::wall_unix_ms(); }

std::string file_name(const std::string& path) {
  const usize slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string default_purpose() {
  const std::string exe = detail::executable_path();
  if (exe.empty()) return "gpu device";
  const usize slash = exe.find_last_of('/');
  if (slash == std::string::npos) return "gpu device: " + exe;
  return "gpu device: " + exe.substr(slash + 1) + " in " + exe.substr(0, slash);
}

std::string holder_text(const State& seen) {
  if (!seen.present) return "nobody";
  if (!seen.readable) return "a lock file that is being written";
  return "'" + seen.owner + "' (pid " + std::to_string(seen.pid) + "): " + seen.purpose +
         " (until " + seen.expires + ")";
}

// The protocol's own announcement (GPU-LOCK.md rule 2: say who holds it and why you wait), on
// stderr in the form every implementation prints — the PowerShell tools' "gpu-lock: ...", the
// harness's "bench: ..." — rather than through the log: it has to reach the terminal of a test
// whatever sinks the process installed, and the give-up line has to be out before _Exit.
void say(const HoldConfig& config, const std::string& line) {
  if (config.out == nullptr) return;
  std::fprintf(config.out, "gpu-lock: %s\n", line.c_str());
  std::fflush(config.out);
}

// One JSON line per hold and per give-up in the file ENGINE_GPU_LOCK_LOG names: how a suite's
// holds are counted and timed (docs/experiments/gpu-lock-per-device-2026-09-30.md).
void log_event(const HoldConfig& config, const std::string& lock_path, const char* event,
               i64 held_ms, i64 waited_ms, std::string_view started, const State* holder) {
  if (config.log_path.empty()) return;
  JsonValue line = JsonValue::object();
  line.set("event", event);
  line.set("lock", lock_path);
  line.set("pid", config.self.pid);
  line.set("exe", file_name(detail::executable_path()));
  line.set("purpose", config.purpose);
  line.set("started", started);
  line.set("held_ms", held_ms);
  line.set("waited_ms", waited_ms);
  line.set("end_unix_ms", wall_ms());
  if (holder != nullptr && holder->readable) {
    line.set("holder_owner", holder->owner);
    line.set("holder_purpose", holder->purpose);
  }
  detail::append_line(config.log_path, write_json(line, JsonWriteOptions{.pretty = false}));
}

std::string body_of(const ProcessHold& h) {
  return detail::lock_body(h.config.self.owner, h.config.purpose, h.config.self.pid, h.started,
                           h.expires_s, h.config.self.host);
}

// Tries once to write the lock in this process's name. Caller holds the mutex.
detail::CreateResult try_take(ProcessHold& h, i64 now_s) {
  h.started = format_iso8601_utc(now_s);
  h.expires_s = now_s + std::max<i64>(h.config.lease_s, 1);
  const detail::CreateResult result = detail::create_exclusive(h.path, body_of(h));
  if (result != detail::CreateResult::Created) return result;
  h.mode = HoldMode::Taken;
  h.taken_ms = wall_ms();
  detail::arm_signal_release(h.path, h.expires_s);
  return result;
}

// Whether the file at h.path is still the one this hold wrote.
bool still_ours(const ProcessHold& h) {
  std::string text;
  bool exists = false;
  if (!detail::read_text(h.path, text, exists)) return false;
  const detail::ParsedLock lock = detail::parse_lock(text);
  return lock.readable && lock.owner == h.config.self.owner && lock.pid == h.config.self.pid &&
         lock.started == h.started;
}

// Keeps a Taken lease alive, or watches a parent's hold and takes over when it goes. Runs with
// the mutex held except while it sleeps; `stop` ends it at once.
void keeper_main() {
  ProcessHold& h = process_hold();
  (void)platform::set_current_thread_name("gpu-lock keeper");
  std::unique_lock<std::mutex> guard(h.mutex);
  bool warned = false;
  for (;;) {
    const auto interval = std::chrono::seconds(std::max<i64>(h.config.refresh_s, 1));
    if (h.wake.wait_for(guard, interval, [&] { return h.stop; })) return;
    const i64 now_s = detail::now_unix_s();
    if (h.mode == HoldMode::Taken) {
      if (!still_ours(h)) {
        // Only a holder that stalled for a whole lease gets here: somebody broke it as expired.
        if (!warned) {
          const State seen = read(h.path, h.config.self, now_s);
          say(h.config, "the lock this process held for its GPU device is no longer its own (" +
                            holder_text(seen) + "); carrying on without it");
          warned = true;
        }
        continue;
      }
      h.expires_s = now_s + std::max<i64>(h.config.lease_s, 1);
      if (detail::replace_atomically(h.path, body_of(h))) detail::update_signal_expiry(h.expires_s);
      continue;
    }
    if (h.mode != HoldMode::Parents) return;
    const State seen = read(h.path, h.config.self, now_s);
    if (seen.present && seen.readable && seen.pid == h.parent_pid &&
        seen.owner == h.config.self.owner && !seen.expired) {
      continue;  // the parent still holds it for us
    }
    // The parent's hold is gone while this process still has a device: it died (its lease ran
    // out) or it released first. Take the lock in this process's own name if nobody else has it.
    std::string text;
    bool exists = false;
    const bool got = detail::read_text(h.path, text, exists);
    if (exists && got && seen.expired && seen.owner != "marc")
      (void)detail::break_stale(h.path, text);
    if (try_take(h, now_s) == detail::CreateResult::Created) {
      say(h.config, "took the GPU lock over from pid " + std::to_string(h.parent_pid) +
                        ", which held it for this process and let it go while this process "
                        "still has a GPU device");
      h.parent_pid = 0;
      continue;
    }
    if (!warned) {
      const State now_seen = read(h.path, h.config.self, now_s);
      say(h.config, "the GPU lock pid " + std::to_string(h.parent_pid) +
                        " held for this process is gone, and " + holder_text(now_seen) +
                        " has it now; carrying on without it");
      warned = true;
    }
  }
}

void start_keeper(ProcessHold& h) {
  h.stop = false;
  h.keeper = std::thread(keeper_main);
  h.keeper_running = true;
}

// Stops the keeper and lets the lock go if this process wrote it. Caller holds `guard`, which is
// released while the keeper is joined (the keeper needs the mutex to see `stop`).
void end_hold(ProcessHold& h, std::unique_lock<std::mutex>& guard) {
  if (h.keeper_running) {
    h.stop = true;
    guard.unlock();
    h.wake.notify_all();
    h.keeper.join();
    guard.lock();
    h.keeper_running = false;
    h.stop = false;
  }
  if (h.mode == HoldMode::Taken) {
    detail::disarm_signal_release();
    if (still_ours(h)) (void)detail::remove_file(h.path);
    log_event(h.config, h.path, "hold", wall_ms() - h.taken_ms, h.waited_ms, h.started, nullptr);
  }
  if (h.set_holder_env) {
    detail::set_environment(k_env_holder, h.previous_holder);
    h.set_holder_env = false;
  }
  h.mode = HoldMode::Off;
  h.parent_pid = 0;
  h.waited_ms = 0;
}

void release_at_exit() {
  ProcessHold& h = process_hold();
  // Bounded: a thread that is still waiting for the lock with the mutex held while the process
  // exits must not hang the exit. The lease lets the lock go if this gives up.
  std::unique_lock<std::mutex> guard(h.mutex, std::try_to_lock);
  for (int attempt = 0; !guard.owns_lock() && attempt < 200; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    (void)guard.try_lock();
  }
  if (!guard.owns_lock() || h.devices == 0) return;
  // Devices still open at exit (leaked, or static and not destroyed yet): the process is done
  // with the GPU either way. A later release() finds no devices and does nothing.
  h.devices = 0;
  end_hold(h, guard);
}

// The first device's acquisition: take the lock, find it held for this process, or wait.
// Caller holds the mutex; the wait sleeps with it held, which only delays another thread's
// first device, and that one would have waited for the lock anyway.
HoldMode establish(ProcessHold& h, const HoldConfig& config) {
  h.config = config;
  if (h.config.purpose.empty()) h.config.purpose = default_purpose();
  h.path = h.config.path.empty() ? default_path() : h.config.path;
  if (!h.exit_handler) {
    std::atexit(release_at_exit);
    h.exit_handler = true;
  }
  if (!h.config.enabled) return HoldMode::Off;

  const fs::path parent = fs::path(h.path).parent_path();
  std::error_code ec;
  if (!parent.empty() && !fs::is_directory(parent, ec)) return HoldMode::NoDirectory;

  const i64 began_ms = wall_ms();
  i64 deadline_s = h.config.wait_until_unix_s;
  std::string announced;
  for (;;) {
    const i64 now_s = detail::now_unix_s();
    const detail::CreateResult created = try_take(h, now_s);
    if (created == detail::CreateResult::Failed) return HoldMode::Error;
    if (created == detail::CreateResult::Created) {
      h.waited_ms = wall_ms() - began_ms;
      if (!announced.empty()) {
        say(h.config, "taken after " + std::to_string(h.waited_ms / 1000) + " s");
      }
      // Children started while this lasts find the lock held for them (the header's "Children").
      h.previous_holder = detail::environment(k_env_holder);
      detail::set_environment(k_env_holder, std::to_string(h.config.self.pid));
      h.set_holder_env = true;
      start_keeper(h);
      return HoldMode::Taken;
    }
    std::string text;
    bool exists = false;
    const bool got = detail::read_text(h.path, text, exists);
    if (!exists) continue;  // released between the attempt and the look
    const State seen = read(h.path, h.config.self, now_s);
    if (seen.mine && seen.pid == h.config.self.pid) return HoldMode::InProcess;
    if (seen.mine && !seen.expired) {
      h.parent_pid = seen.pid;
      h.mode = HoldMode::Parents;
      start_keeper(h);
      return HoldMode::Parents;
    }
    if (got && seen.expired && seen.owner != "marc") {
      say(h.config, "breaking an expired GPU lock held by " + holder_text(seen));
      if (detail::break_stale(h.path, text)) continue;
    }

    if (deadline_s == 0) deadline_s = now_s + k_default_wait_s;
    if (now_s >= deadline_s) {
      h.waited_ms = wall_ms() - began_ms;
      log_event(h.config, h.path, "gave_up", 0, h.waited_ms, "", &seen);
      say(h.config, "gave up waiting for the GPU lock after " + std::to_string(h.waited_ms / 1000) +
                        " s; it is held by " + holder_text(seen) + ". Exit " +
                        std::to_string(k_exit_gave_up) +
                        ": nothing ran on the GPU; run this again when the lock is free");
      if (h.config.exit_on_give_up) {
        std::fflush(nullptr);
        std::_Exit(k_exit_gave_up);
      }
      return HoldMode::GaveUp;
    }
    const std::string holder =
        seen.readable ? seen.owner + "\n" + seen.purpose + "\n" + std::to_string(seen.pid)
                      : std::string("\n");
    if (holder != announced) {
      say(h.config, "waiting for the GPU lock, held by " + holder_text(seen) + "; gives up at " +
                        format_iso8601_utc(deadline_s));
      announced = holder;
    }
    const i64 sleep_s = std::min<i64>(std::max<i64>(h.config.poll_s, 1), deadline_s - now_s);
    std::this_thread::sleep_for(std::chrono::seconds(std::max<i64>(sleep_s, 1)));
  }
}

}  // namespace

const char* to_string(HoldMode mode) noexcept {
  switch (mode) {
    case HoldMode::Off: return "off";
    case HoldMode::Taken: return "taken";
    case HoldMode::Parents: return "parents";
    case HoldMode::InProcess: return "in-process";
    case HoldMode::NoDirectory: return "no-directory";
    case HoldMode::GaveUp: return "gave-up";
    case HoldMode::Error: return "error";
  }
  return "unknown";
}

HoldConfig hold_config_from_environment() {
  HoldConfig config;
  config.enabled = detail::environment(k_env_on_device) == "1";
  config.path = default_path();
  config.self = current_identity();
  config.log_path = detail::environment(k_env_log);
  const std::string until = detail::environment(k_env_wait_until);
  i64 value = 0;
  bool digits = !until.empty();
  for (const char c : until) {
    if (c < '0' || c > '9') {
      digits = false;
      break;
    }
    value = value * 10 + (c - '0');
  }
  config.wait_until_unix_s = digits ? value : 0;
  return config;
}

HoldMode DeviceHold::acquire() {
  // The environment is read only for a first device: later ones join the hold as it is.
  {
    ProcessHold& h = process_hold();
    std::unique_lock<std::mutex> guard(h.mutex);
    if (counted_) return h.mode;
    if (h.devices > 0) {
      ++h.devices;
      counted_ = true;
      return h.mode;
    }
  }
  return acquire(hold_config_from_environment());
}

HoldMode DeviceHold::acquire(const HoldConfig& config) {
  ProcessHold& h = process_hold();
  std::unique_lock<std::mutex> guard(h.mutex);
  if (counted_) return h.mode;
  if (h.devices > 0) {
    ++h.devices;
    counted_ = true;
    return h.mode;
  }
  const HoldMode mode = establish(h, config);
  if (mode == HoldMode::GaveUp) {
    h.mode = HoldMode::Off;
    return mode;
  }
  if (mode == HoldMode::Error) say(h.config, "cannot write " + h.path + "; running without it");
  h.mode = mode;
  h.devices = 1;
  counted_ = true;
  return mode;
}

void DeviceHold::release() noexcept {
  if (!counted_) return;
  counted_ = false;
  ProcessHold& h = process_hold();
  std::unique_lock<std::mutex> guard(h.mutex);
  if (h.devices == 0) return;  // released at exit already
  if (--h.devices > 0) return;
  end_hold(h, guard);
}

HoldStatus hold_status() {
  ProcessHold& h = process_hold();
  std::lock_guard<std::mutex> guard(h.mutex);
  HoldStatus status;
  status.devices = h.devices;
  status.mode = h.devices > 0 ? h.mode : HoldMode::Off;
  status.keeper_running = h.keeper_running;
  status.parent_pid = h.parent_pid;
  return status;
}

}  // namespace engine::gpu_lock

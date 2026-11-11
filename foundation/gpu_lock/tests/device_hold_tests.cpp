// The GPU lock held for as long as a process has a GPU device open
// (foundation/gpu_lock/device_hold.h; docs/subsystems/gpu_lock.md; ADR-0049).
//
// The first half runs in this process with an explicit configuration: counting devices, the
// modes a first device can find, the keeper's refresh, a parent's hold going away, the hold log,
// and a give-up that returns instead of exiting. The second half starts
// `engine_gpu_lock_probe`, which holds the lock the way a process with a device does, for what
// only several processes can show: a child does not wait on its parent, a wait that runs out ends
// the process with 75 and so does its parent, a crash leaves the lock for one lease, a parent
// that dies first hands the lock to its child, and a command wrapped in `tools/gpu-lock.ps1 run`
// finds the lock its own.
//
// Every lock file is in the case's own scratch directory, and every case clears the variables
// that would otherwise reach in from the run around it (a suite wrapped in `gpu-lock.ps1 run`
// sets ENGINE_GPU_LOCK_HOLDER; a measured suite sets ENGINE_GPU_LOCK_LOG).

#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/platform/process.h>
#include <core/time/time.h>
#include <foundation/gpu_lock/device_hold.h>
#include <foundation/gpu_lock/gpu_lock.h>

#include <doctest/doctest.h>
#include <test_environment.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace engine;

namespace {

i64 now_s() { return time::wall_unix_ms() / 1000; }

constexpr u64 k_other_pid = 999'999'937;

std::string lock_json(std::string_view owner, u64 pid, i64 expires_unix_s, std::string_view host,
                      std::string_view purpose = "a test holder") {
  JsonValue o = JsonValue::object();
  o.set("owner", owner);
  o.set("purpose", purpose);
  o.set("pid", pid);
  o.set("started", gpu_lock::format_iso8601_utc(expires_unix_s - 600));
  o.set("expires", gpu_lock::format_iso8601_utc(expires_unix_s));
  o.set("host", host);
  return write_json(o, JsonWriteOptions{.pretty = false});
}

void write_file(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

bool exists(const std::string& path) {
  std::error_code ec;
  return std::filesystem::exists(std::filesystem::path(path), ec);
}

bool eventually(const std::function<bool()>& condition, int timeout_ms) {
  for (int waited = 0; waited < timeout_ms; waited += 50) {
    if (condition()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return condition();
}

// The keeper refreshes the file by renaming over it, and a reader that opens it in that moment is
// told it is present and being written (unreadable, not expired; gpu_lock.md). A case that reads
// once at a fixed time lands in that window now and then (gate b42, 2026-10-27: the keeper case
// failed on an `expires` it never got), so read again until the file is readable or gone.
gpu_lock::State lock_at(const std::string& path) {
  gpu_lock::Identity self = gpu_lock::current_identity();
  self.holder_pid = 0;
  for (int attempt = 0;; ++attempt) {
    const gpu_lock::State state = gpu_lock::read(path, self, now_s());
    if (!state.present || state.readable || attempt >= 50) return state;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

// The variables a run around this one may have set, cleared for one case. The switch is off in
// this process — so that a child's 75 does not end the test too — and the probes turn it on for
// themselves with --on.
struct Isolated {
  test::ScopedEnv on{gpu_lock::k_env_on_device, "0"};
  test::ScopedEnv holder{gpu_lock::k_env_holder, ""};
  test::ScopedEnv hold_log{gpu_lock::k_env_log, ""};
  test::ScopedEnv until{gpu_lock::k_env_wait_until, ""};
};

gpu_lock::HoldConfig config_at(const std::string& path) {
  gpu_lock::HoldConfig config;
  config.enabled = true;
  config.path = path;
  config.self = gpu_lock::current_identity();
  config.self.holder_pid = 0;
  config.purpose = "device hold test";
  config.out = nullptr;
  config.exit_on_give_up = false;
  return config;
}

}  // namespace

// ---- in this process ------------------------------------------------------------------------

TEST_CASE("device hold: off, it touches nothing") {
  const Isolated isolated;
  test::TempDir dir("gpu_hold_off");
  gpu_lock::HoldConfig config = config_at(dir.file("gpu.lock"));
  config.enabled = false;
  gpu_lock::DeviceHold hold;
  CHECK(hold.acquire(config) == gpu_lock::HoldMode::Off);
  CHECK_FALSE(exists(dir.file("gpu.lock")));
  CHECK(gpu_lock::hold_status().devices == 1);
  CHECK_FALSE(gpu_lock::hold_status().keeper_running);
  hold.release();
  CHECK(gpu_lock::hold_status().devices == 0);

  // And the switch is exactly "1": the test main sets it, "0" or anything else is off.
  {
    const test::ScopedEnv on(gpu_lock::k_env_on_device, "1");
    CHECK(gpu_lock::hold_config_from_environment().enabled);
  }
  {
    const test::ScopedEnv on(gpu_lock::k_env_on_device, "yes");
    CHECK_FALSE(gpu_lock::hold_config_from_environment().enabled);
  }
  const test::ScopedEnv until(gpu_lock::k_env_wait_until, "1790035200");
  CHECK(gpu_lock::hold_config_from_environment().wait_until_unix_s == 1790035200);
}

TEST_CASE("device hold: the first device takes the lock, later ones share it, the last lets go") {
  const Isolated isolated;
  test::TempDir dir("gpu_hold_count");
  const std::string path = dir.file("gpu.lock");
  const gpu_lock::HoldConfig config = config_at(path);

  gpu_lock::DeviceHold first;
  gpu_lock::DeviceHold second;
  REQUIRE(first.acquire(config) == gpu_lock::HoldMode::Taken);
  const gpu_lock::State held = lock_at(path);
  CHECK(held.pid == config.self.pid);
  CHECK(held.owner == config.self.owner);
  CHECK(held.purpose == "device hold test");
  i64 expires = 0;
  REQUIRE(gpu_lock::parse_iso8601_utc(held.expires, expires));
  CHECK(expires <= now_s() + gpu_lock::k_device_lease_s + 1);  // a short lease, kept alive
  CHECK(gpu_lock::hold_status().keeper_running);
  // Children started now find the lock held for them.
  CHECK(test::detail::environment(gpu_lock::k_env_holder) == std::to_string(config.self.pid));

  CHECK(second.acquire(config) == gpu_lock::HoldMode::Taken);
  CHECK(gpu_lock::hold_status().devices == 2);
  CHECK(lock_at(path).started == held.started);  // one file, still the first device's

  first.release();
  CHECK(exists(path));  // the second device still has it
  first.release();      // idempotent
  CHECK(gpu_lock::hold_status().devices == 1);
  second.release();
  CHECK_FALSE(exists(path));
  CHECK_FALSE(gpu_lock::hold_status().keeper_running);
  CHECK(test::detail::environment(gpu_lock::k_env_holder).empty());  // put back as it was
}

TEST_CASE("device hold: the keeper keeps a short lease alive while the device lives") {
  const Isolated isolated;
  test::TempDir dir("gpu_hold_keeper");
  const std::string path = dir.file("gpu.lock");
  gpu_lock::HoldConfig config = config_at(path);
  config.lease_s = 3;
  config.refresh_s = 1;
  gpu_lock::DeviceHold hold;
  REQUIRE(hold.acquire(config) == gpu_lock::HoldMode::Taken);
  i64 first = 0;
  REQUIRE(gpu_lock::parse_iso8601_utc(lock_at(path).expires, first));
  // Past the first lease: still held, not expired, and its expiry has moved.
  std::this_thread::sleep_for(std::chrono::milliseconds(4200));
  const gpu_lock::State later = lock_at(path);
  CHECK(later.present);
  CHECK_FALSE(later.expired);
  i64 moved = 0;
  REQUIRE(gpu_lock::parse_iso8601_utc(later.expires, moved));
  CHECK(moved > first);
  hold.release();
  CHECK_FALSE(exists(path));
}

TEST_CASE("device hold: a lock held for this process is used as it is") {
  const Isolated isolated;
  test::TempDir dir("gpu_hold_mine");
  const std::string path = dir.file("gpu.lock");
  gpu_lock::HoldConfig config = config_at(path);

  // Another path in this process holds it: the bench harness's --gpu-lock.
  {
    gpu_lock::Lease::Config lease_config;
    lease_config.path = path;
    lease_config.self = config.self;
    lease_config.purpose = "bench";
    lease_config.timeout_s = 0;
    lease_config.log = nullptr;
    gpu_lock::Lease lease;
    REQUIRE(lease.acquire(lease_config) == gpu_lock::Lease::Outcome::Taken);
    const std::string before = read_file(path);
    gpu_lock::DeviceHold hold;
    CHECK(hold.acquire(config) == gpu_lock::HoldMode::InProcess);
    CHECK_FALSE(gpu_lock::hold_status().keeper_running);
    hold.release();
    CHECK(read_file(path) == before);  // the lease's, and the lease's to release
  }
  CHECK_FALSE(exists(path));

  // A parent holds it for this process (ENGINE_GPU_LOCK_HOLDER): neither waits nor releases.
  config.self.holder_pid = 4242;
  const std::string parents = lock_json(config.self.owner, 4242, now_s() + 600, config.self.host);
  write_file(path, parents);
  {
    gpu_lock::DeviceHold hold;
    CHECK(hold.acquire(config) == gpu_lock::HoldMode::Parents);
    CHECK(gpu_lock::hold_status().parent_pid == 4242);
    CHECK(gpu_lock::hold_status().keeper_running);
  }
  CHECK(read_file(path) == parents);

  // The same parent's lock, expired: the parent is dead, and its lock is broken and taken.
  write_file(path, lock_json(config.self.owner, 4242, now_s() - 5, config.self.host));
  {
    gpu_lock::DeviceHold hold;
    CHECK(hold.acquire(config) == gpu_lock::HoldMode::Taken);
    CHECK(lock_at(path).pid == config.self.pid);
  }
  CHECK_FALSE(exists(path));
}

TEST_CASE("device hold: a parent's hold that goes while the device lives is taken over") {
  const Isolated isolated;
  test::TempDir dir("gpu_hold_adopt");
  const std::string path = dir.file("gpu.lock");
  gpu_lock::HoldConfig config = config_at(path);
  config.self.holder_pid = 4242;
  config.refresh_s = 1;
  write_file(path, lock_json(config.self.owner, 4242, now_s() + 600, config.self.host));
  gpu_lock::DeviceHold hold;
  REQUIRE(hold.acquire(config) == gpu_lock::HoldMode::Parents);

  // The parent released (or died and somebody cleaned up) while this device is still open.
  std::filesystem::remove(std::filesystem::path(path));
  CHECK(eventually([&] { return lock_at(path).pid == config.self.pid; }, 5000));
  CHECK(gpu_lock::hold_status().mode == gpu_lock::HoldMode::Taken);
  hold.release();
  CHECK_FALSE(exists(path));
}

TEST_CASE(
    "device hold: somebody else's lock is waited for, an expired one broken, a person's not") {
  const Isolated isolated;
  test::TempDir dir("gpu_hold_contended");
  const std::string path = dir.file("gpu.lock");
  const std::string log_path = dir.file("holds.jsonl");
  gpu_lock::HoldConfig config = config_at(path);
  config.log_path = log_path;
  config.poll_s = 1;

  // Held by another engine agent until well past this case's deadline: the wait gives up at
  // the deadline, says so in the log, and leaves the lock alone.
  const std::string theirs = lock_json(config.self.owner, k_other_pid, now_s() + 600,
                                       config.self.host, "another agent's suite");
  write_file(path, theirs);
  config.wait_until_unix_s = now_s() + 1;
  {
    gpu_lock::DeviceHold hold;
    CHECK(hold.acquire(config) == gpu_lock::HoldMode::GaveUp);
    CHECK_FALSE(hold.counted());
    CHECK(gpu_lock::hold_status().devices == 0);
  }
  CHECK(read_file(path) == theirs);
  JsonValue gave_up;
  REQUIRE(parse_json(read_file(log_path), gave_up).ok);
  CHECK(gave_up["event"] == JsonValue("gave_up"));
  CHECK(gave_up["holder_purpose"] == JsonValue("another agent's suite"));
  CHECK(gave_up["lock"] == JsonValue(path));

  // Released part way through the wait: taken then.
  config.wait_until_unix_s = now_s() + 30;
  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    std::filesystem::remove(std::filesystem::path(path));
  });
  {
    gpu_lock::DeviceHold hold;
    CHECK(hold.acquire(config) == gpu_lock::HoldMode::Taken);
    releaser.join();
  }
  // One line a hold, with how long it waited and held.
  std::ifstream lines(log_path);
  std::string line;
  std::vector<std::string> all;
  while (std::getline(lines, line))
    all.push_back(line);
  REQUIRE(all.size() == 2);
  JsonValue hold_line;
  REQUIRE(parse_json(all[1], hold_line).ok);
  CHECK(hold_line["event"] == JsonValue("hold"));
  i64 waited = 0;
  REQUIRE(hold_line["waited_ms"].get_i64(waited));
  CHECK(waited >= 1000);

  // Expired: broken without a wait. A person's, expired or not: never.
  write_file(path, lock_json("astra-blender", 4321, now_s() - 5, config.self.host));
  config.wait_until_unix_s = now_s();
  {
    gpu_lock::DeviceHold hold;
    CHECK(hold.acquire(config) == gpu_lock::HoldMode::Taken);
  }
  const std::string persons = lock_json("marc", 4321, now_s() - 5, config.self.host);
  write_file(path, persons);
  {
    gpu_lock::DeviceHold hold;
    CHECK(hold.acquire(config) == gpu_lock::HoldMode::GaveUp);
  }
  CHECK(read_file(path) == persons);

  // A machine without the lock's directory does not use the protocol.
  gpu_lock::HoldConfig elsewhere = config_at(dir.file("no-such-directory/gpu.lock"));
  gpu_lock::DeviceHold hold;
  CHECK(hold.acquire(elsewhere) == gpu_lock::HoldMode::NoDirectory);
}

// ---- several processes ----------------------------------------------------------------------

namespace {

const std::string& probe_exe() {
  static const std::string path = test::app_path(ENGINE_GPU_LOCK_PROBE_PATH);
  return path;
}

struct Run {
  i32 exit_code = -1;
  std::string output;
};

// Runs the probe with stdout and stderr captured, and waits for it.
Run probe(std::vector<std::string> args) {
  std::vector<std::string_view> argv;
  argv.push_back(probe_exe());
  for (const std::string& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Run run;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error,
               /*merge_stderr=*/true)) {
    FAIL("cannot start the probe: " << error);
    return run;
  }
  p.close_stdin();
  p.read_all(run.output);
  run.exit_code = p.wait();
  return run;
}

// "<pid> <mode>" lines, in order.
std::vector<std::pair<u64, std::string>> reports(const std::string& path) {
  std::vector<std::pair<u64, std::string>> out;
  std::ifstream in(path);
  u64 pid = 0;
  std::string mode;
  while (in >> pid >> mode)
    out.emplace_back(pid, mode);
  return out;
}

bool have_probe() {
  if (test::path_exists(probe_exe())) return true;
  MESSAGE("not in this bundle: " << probe_exe());
  return false;
}

}  // namespace

TEST_CASE("device hold: a child finds its parent's hold and neither waits nor releases") {
  if (!have_probe()) return;
  const Isolated isolated;
  test::TempDir dir("gpu_hold_child");
  const std::string path = dir.file("gpu.lock");
  const std::string report = dir.file("report.txt");
  const Run run = probe({"--on", "--lock", path, "--report", report, "parent", "0", "--", "--lock",
                         path, "--report", report, "hold", "0"});
  CHECK_MESSAGE(run.exit_code == 0, run.output);
  const auto seen = reports(report);
  REQUIRE(seen.size() == 2);
  CHECK(seen[0].second == "taken");    // the parent
  CHECK(seen[1].second == "parents");  // the child, which inherited the switch
  CHECK(seen[0].first != seen[1].first);
  CHECK_FALSE(exists(path));  // the parent released when it was done

  // Off, a probe takes nothing at all.
  const Run off = probe({"--lock", path, "--report", report, "hold", "0"});
  CHECK(off.exit_code == 0);
  CHECK(reports(report).back().second == "off");
  CHECK_FALSE(exists(path));
}

TEST_CASE("device hold: a wait that runs out ends the process with 75, and its parent with it") {
  if (!have_probe()) return;
  const Isolated isolated;
  test::TempDir dir("gpu_hold_give_up");
  const std::string path = dir.file("gpu.lock");
  const gpu_lock::Identity self = gpu_lock::current_identity();
  const std::string theirs =
      lock_json("astra-blender", 4321, now_s() + 600, self.host, "bake: desert");
  write_file(path, theirs);
  const test::ScopedEnv until(gpu_lock::k_env_wait_until, std::to_string(now_s() + 2));

  const Run alone = probe({"--on", "--lock", path, "--poll", "1", "hold", "0"});
  CHECK(alone.exit_code == gpu_lock::k_exit_gave_up);
  CHECK_MESSAGE(alone.output.find("gave up waiting for the GPU lock") != std::string::npos,
                alone.output);
  CHECK_MESSAGE(alone.output.find("bake: desert") != std::string::npos, alone.output);

  // The same child under a parent that holds nothing (an end-to-end test starting an app): the
  // parent learns the code in platform::Process::wait() and ends with it too, so the test at the
  // root of the tree is reported as skipped rather than as a failure of whatever it asked for.
  const Run tree = probe({"--on", "spawn", "--", "--lock", path, "--poll", "1", "hold", "0"});
  CHECK(tree.exit_code == gpu_lock::k_exit_gave_up);
  CHECK_MESSAGE(tree.output.find("gave up waiting for the GPU lock (exit 75)") != std::string::npos,
                tree.output);
  CHECK(read_file(path) == theirs);
}

TEST_CASE("device hold: a crash leaves the lock for one lease, no longer") {
  if (!have_probe()) return;
  const Isolated isolated;
  test::TempDir dir("gpu_hold_crash");
  const std::string path = dir.file("gpu.lock");
  const std::string report = dir.file("report.txt");

  std::vector<std::string> args = {probe_exe(), "--on", "--lock",   path,   "--lease", "3",
                                   "--refresh", "1",    "--report", report, "hold",    "60000"};
  std::vector<std::string_view> argv(args.begin(), args.end());
  platform::Process p;
  REQUIRE(p.spawn(std::span<const std::string_view>(argv.data(), argv.size())));
  p.close_stdin();
  REQUIRE(eventually([&] { return !reports(report).empty(); }, 10000));
  const u64 holder = reports(report)[0].first;
  CHECK(reports(report)[0].second == "taken");
  CHECK(lock_at(path).pid == holder);

  // Alive past its first lease: the keeper kept it.
  std::this_thread::sleep_for(std::chrono::milliseconds(4000));
  CHECK_FALSE(lock_at(path).expired);

  p.kill();  // no release, no signal handler: TerminateProcess / SIGKILL
  (void)p.wait();
  CHECK(lock_at(path).pid == holder);                              // left behind...
  CHECK(eventually([&] { return lock_at(path).expired; }, 6000));  // ...for one lease

  const Run next = probe({"--on", "--lock", path, "--report", report, "hold", "0"});
  CHECK_MESSAGE(next.exit_code == 0, next.output);
  CHECK(reports(report).back().second == "taken");  // broke the expired lock without waiting
  CHECK_FALSE(exists(path));
}

TEST_CASE("device hold: a parent that dies first hands the lock to its child") {
  if (!have_probe()) return;
  const Isolated isolated;
  test::TempDir dir("gpu_hold_orphan");
  const std::string path = dir.file("gpu.lock");
  const std::string report = dir.file("report.txt");
  // Not `probe()`: the child inherits the parent's end of the output pipe, so reading it to the
  // end would wait for the child as well, and this case has to look while the child still runs.
  const std::vector<std::string> args = {
      probe_exe(), "--on", "--lock",      path,   "--lease", "3",   "--refresh", "1",
      "--report",  report, "parent-dies", "--",   "--lock",  path,  "--lease",   "3",
      "--refresh", "1",    "--report",    report, "hold",    "9000"};
  const std::vector<std::string_view> argv(args.begin(), args.end());
  platform::Process parent;
  REQUIRE(parent.spawn(std::span<const std::string_view>(argv.data(), argv.size())));
  parent.close_stdin();
  CHECK(parent.wait() == 0);
  const auto seen = reports(report);
  REQUIRE(seen.size() == 2);
  CHECK(seen[0].second == "taken");
  CHECK(seen[1].second == "parents");
  const u64 child = seen[1].first;
  // The parent's lease runs out unrefreshed and the child, still holding its "device", takes the
  // lock in its own name rather than leave the GPU unspoken for.
  CHECK(eventually([&] { return lock_at(path).pid == child; }, 8000));
  // And lets it go when it is done.
  CHECK(eventually([&] { return !exists(path); }, 12000));
}

TEST_CASE("device hold: a command wrapped in gpu-lock.ps1 run finds the lock its own") {
  if (!have_probe()) return;
  const std::string script =
      test::data_path(ENGINE_SOURCE_DIR "/tools/gpu-lock.ps1", "tools/gpu-lock.ps1");
  if (!test::path_exists(script)) {
    MESSAGE("not in this bundle: " << script);
    return;
  }
  // The three ways a run is still wrapped: under the default owner; by a gate that sets
  // ENGINE_GPU_LOCK_OWNER in its environment; and by one that passes -Owner to the wrapper alone,
  // whose children's environment names the default owner unless the wrapper hands its own down.
  struct Case {
    const char* what;
    const char* env_owner;
    const char* owner_arg;
  };
  const Case cases[] = {{"the default owner", "", ""},
                        {"an owner in the environment", "fable-merge-env", ""},
                        {"an owner given with -Owner", "", "fable-merge-arg"}};
  for (const Case& c : cases) {
    const Isolated isolated;
    const test::ScopedEnv owner("ENGINE_GPU_LOCK_OWNER", c.env_owner);
    // A regression would have the probe wait for its own parent: let it give up in seconds.
    const test::ScopedEnv until(gpu_lock::k_env_wait_until, std::to_string(now_s() + 20));
    test::TempDir dir("gpu_hold_wrapped");
    const std::string path = dir.file("gpu.lock");
    const std::string report = dir.file("report.txt");
    const std::string exec = "& '" + probe_exe() + "' --on --lock '" + path + "' --report '" +
                             report + "' hold 0; exit $LASTEXITCODE";
    std::vector<std::string> args = {"pwsh",    "-NoProfile", "-File", script,  "run", "-Purpose",
                                     "wrapped", "-LockFile",  path,    "-Exec", exec};
    if (c.owner_arg[0] != '\0') {
      args.push_back("-Owner");
      args.push_back(c.owner_arg);
    }
    const std::vector<std::string_view> argv(args.begin(), args.end());
    platform::Process p;
    std::string error;
    if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error, true)) {
      MESSAGE("skipped: no pwsh to run tools/gpu-lock.ps1: " << error);
      return;
    }
    p.close_stdin();
    std::string output;
    p.read_all(output);
    CHECK_MESSAGE(p.wait() == 0, c.what << ": " << output);
    CHECK_MESSAGE(output.find("waiting for the GPU lock") == std::string::npos, c.what);
    const auto seen = reports(report);
    REQUIRE_MESSAGE(seen.size() == 1, c.what);
    // The wrapper's hold, found through ENGINE_GPU_LOCK_HOLDER and the owner it runs under.
    CHECK_MESSAGE(seen[0].second == "parents", c.what);
    CHECK_MESSAGE(!exists(path), c.what);  // and the wrapper's to release
  }
}

// ---- the line (gpu_lock.h, "the line"; roadmap T96) -------------------------------------------
//
// Several probes at one lock in the case's own scratch directory. Each case first writes a plain
// lock file — a holder that knows nothing of tickets, which is every tool built before the line —
// so that the probes it starts all wait, checks that each one's ticket is in the line before it
// starts the next (which is what makes the arrival order the case's and not the scheduler's), and
// then deletes the file as that holder's release.

namespace {

// Starts a probe and leaves it running.
void start_probe(platform::Process& p, const std::vector<std::string>& args) {
  std::vector<std::string_view> argv;
  argv.push_back(probe_exe());
  for (const std::string& a : args)
    argv.push_back(a);
  std::string error;
  REQUIRE_MESSAGE(p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error),
                  error);
  p.close_stdin();
}

usize line_size(const std::string& lock) { return gpu_lock::read_line(lock, now_s()).size(); }

void replace_file(const std::string& path, const std::string& text) {
  write_file(path + ".next", text);
  std::error_code ec;
  std::filesystem::rename(std::filesystem::path(path + ".next"), std::filesystem::path(path), ec);
  REQUIRE_MESSAGE(!ec, ec.message());
}

i64 elapsed_ms(i64 since_ns) { return (time::monotonic_ns() - since_ns) / 1'000'000; }

}  // namespace

TEST_CASE("the line: waiters are served in arrival order, and one that asks again goes last") {
  if (!have_probe()) return;
  const Isolated isolated;
  test::TempDir dir("gpu_line_order");
  const std::string path = dir.file("gpu.lock");
  const std::string report = dir.file("report.txt");
  write_file(path,
             lock_json("astra-blender", 4321, now_s() + 600, gpu_lock::current_identity().host));

  // The one that will ask twice arrives first; three more arrive behind it, one at a time.
  platform::Process again, first, second, third;
  start_probe(again,
              {"--on", "--lock", path, "--look", "50", "--report", report, "holds", "2", "300"});
  REQUIRE(eventually([&] { return line_size(path) == 1; }, 20000));
  platform::Process* later[] = {&first, &second, &third};
  for (usize i = 0; i < 3; ++i) {
    start_probe(*later[i],
                {"--on", "--lock", path, "--look", "50", "--report", report, "hold", "300"});
    REQUIRE(eventually([&] { return line_size(path) == i + 2; }, 20000));
  }
  const Vector<gpu_lock::Waiter> line = gpu_lock::read_line(path, now_s());
  REQUIRE(line.size() == 4);
  CHECK(reports(report).empty());  // nobody passed the holder

  const i64 released = time::monotonic_ns();
  (void)std::filesystem::remove(std::filesystem::path(path));
  CHECK(again.wait() == 0);
  CHECK(first.wait() == 0);
  CHECK(second.wait() == 0);
  CHECK(third.wait() == 0);
  MESSAGE("five holds of 300 ms served in " << elapsed_ms(released) << " ms");

  // Served as they stood, and the first one's second ask after the three that were waiting when
  // it let go: without the line it would have had the lock back before any of them looked.
  const auto seen = reports(report);
  REQUIRE(seen.size() == 5);
  for (u32 i = 0; i < 4; ++i) {
    CHECK(seen[i].first == line[i].pid);
    CHECK(seen[i].second == "taken");
  }
  CHECK(seen[4].first == line[0].pid);
  CHECK(seen[4].second == "taken");
  CHECK_FALSE(exists(path));
  CHECK(line_size(path) == 0);
}

TEST_CASE("the line: a waiter killed in the line stops counting within its ticket's lease") {
  if (!have_probe()) return;
  const Isolated isolated;
  test::TempDir dir("gpu_line_dead_waiter");
  const std::string path = dir.file("gpu.lock");
  const std::string report = dir.file("report.txt");
  write_file(path,
             lock_json("astra-blender", 4321, now_s() + 600, gpu_lock::current_identity().host));

  platform::Process doomed, behind;
  start_probe(doomed, {"--on", "--lock", path, "--look", "50", "--ticket", "3", "--report", report,
                       "hold", "60000"});
  REQUIRE(eventually([&] { return line_size(path) == 1; }, 20000));
  start_probe(behind, {"--on", "--lock", path, "--look", "50", "--report", report, "hold", "0"});
  REQUIRE(eventually([&] { return line_size(path) == 2; }, 20000));

  doomed.kill();  // TerminateProcess / SIGKILL: its ticket stays, refreshed by nobody
  (void)doomed.wait();
  const i64 killed = time::monotonic_ns();
  (void)std::filesystem::remove(std::filesystem::path(path));  // and the lock is free
  CHECK(behind.wait() == 0);
  const i64 waited_ms = elapsed_ms(killed);
  MESSAGE("the waiter behind a killed one had the free lock after " << waited_ms << " ms");
  // The dead ticket's lease (3 s, counted in whole seconds, so under 4) and one look; the rest
  // is the machine.
  CHECK(waited_ms < 4000 + 50 + 4000);
  const auto seen = reports(report);
  REQUIRE(seen.size() == 1);
  CHECK(seen[0].second == "taken");
  CHECK_FALSE(exists(path));
  CHECK(line_size(path) == 0);  // the one behind removed the dead ticket as it passed
}

TEST_CASE(
    "the line: a holder that is killed is broken after its lease, and the line keeps its order") {
  if (!have_probe()) return;
  const Isolated isolated;
  test::TempDir dir("gpu_line_dead_holder");
  const std::string path = dir.file("gpu.lock");
  const std::string report = dir.file("report.txt");

  platform::Process holder, first, second;
  start_probe(holder, {"--on", "--lock", path, "--lease", "3", "--refresh", "1", "--report", report,
                       "hold", "60000"});
  REQUIRE(eventually([&] { return reports(report).size() == 1; }, 20000));
  start_probe(first, {"--on", "--lock", path, "--look", "50", "--report", report, "hold", "200"});
  REQUIRE(eventually([&] { return line_size(path) == 1; }, 20000));
  start_probe(second, {"--on", "--lock", path, "--look", "50", "--report", report, "hold", "200"});
  REQUIRE(eventually([&] { return line_size(path) == 2; }, 20000));
  const Vector<gpu_lock::Waiter> line = gpu_lock::read_line(path, now_s());
  REQUIRE(line.size() == 2);

  holder.kill();
  (void)holder.wait();
  const i64 killed = time::monotonic_ns();
  CHECK(first.wait() == 0);
  CHECK(second.wait() == 0);
  const i64 waited_ms = elapsed_ms(killed);
  MESSAGE("two waiters behind a killed holder were both done after " << waited_ms << " ms");
  CHECK(waited_ms < 4000 + 2 * 250 + 4000);  // its lease, as before the line; then two short holds
  const auto seen = reports(report);
  REQUIRE(seen.size() == 3);
  CHECK(seen[1].first == line[0].pid);  // the head broke the expired lock and took it
  CHECK(seen[2].first == line[1].pid);
  CHECK_FALSE(exists(path));
}

TEST_CASE("the line: a late short job is not starved by holders that keep asking again") {
  if (!have_probe()) return;
  const Isolated isolated;
  test::TempDir dir("gpu_line_late");
  const std::string path = dir.file("gpu.lock");
  const std::string report = dir.file("report.txt");
  const std::string late_report = dir.file("late.txt");

  // Two holders that each take the lock thirty times, 100 ms a hold, asking again the moment they
  // let go: six seconds of holds between them and a look of the line between each two, which is
  // the least this case can take (the loop has to outlast a third process's start on a busy
  // machine, or the case shows nothing). Before the line, whichever held it took it back before
  // anyone looked.
  platform::Process one, two, late;
  const std::vector<std::string> loop = {"--on",     "--lock", path,    "--look", "50",
                                         "--report", report,   "holds", "30",     "100"};
  start_probe(one, loop);
  start_probe(two, loop);
  REQUIRE(eventually([&] { return reports(report).size() >= 4; }, 20000));
  start_probe(late, {"--on", "--lock", path, "--look", "50", "--report", late_report, "hold", "0"});
  // The moment the late one is seen in the line (its purpose says "hold", theirs "holds"), or has
  // already been served.
  usize holds_before = 0;
  REQUIRE(eventually(
      [&] {
        holds_before = reports(report).size();
        if (!reports(late_report).empty()) return true;
        for (const gpu_lock::Waiter& w : gpu_lock::read_line(path, now_s())) {
          if (w.purpose == "gpu lock probe: hold") return true;
        }
        return false;
      },
      20000));
  CHECK(late.wait() == 0);
  const usize holds_after = reports(report).size();
  CHECK(one.wait() == 0);
  CHECK(two.wait() == 0);
  REQUIRE(reports(late_report).size() == 1);
  CHECK(reports(report).size() == 60);
  // Ahead of it when it arrived: the hold in progress and the other holder, already waiting.
  // Three more allow for the holds that ended between this case's two looks.
  MESSAGE("the late job was served after " << (holds_after - holds_before)
                                           << " more holds, of 60 in all; it arrived after "
                                           << holds_before);
  CHECK(holds_after - holds_before <= 2 + 3);
  CHECK(holds_before < 45);  // or the case showed nothing: it must arrive while they still loop
}

TEST_CASE("the line: a holder without a ticket is waited for, and takes nothing from the order") {
  if (!have_probe()) return;
  const Isolated isolated;
  test::TempDir dir("gpu_line_old");
  const std::string path = dir.file("gpu.lock");
  const std::string report = dir.file("report.txt");
  const std::string host = gpu_lock::current_identity().host;
  // A tool built before the line holds the lock: a lock file, and no ticket anywhere.
  write_file(path, lock_json("astra-blender", 4321, now_s() + 600, host, "bake: desert"));

  platform::Process first, second;
  start_probe(first, {"--on", "--lock", path, "--look", "50", "--report", report, "hold", "200"});
  REQUIRE(eventually([&] { return line_size(path) == 1; }, 20000));
  start_probe(second, {"--on", "--lock", path, "--look", "50", "--report", report, "hold", "200"});
  REQUIRE(eventually([&] { return line_size(path) == 2; }, 20000));
  const Vector<gpu_lock::Waiter> line = gpu_lock::read_line(path, now_s());
  REQUIRE(line.size() == 2);

  // It lets go, and a second such tool — another worktree's older binary, whose own look landed
  // in that instant — has the lock before the head of the line does. That is the worst an old
  // binary does to a line of new ones: it is not in the line, so it can be served out of turn;
  // the line sees one more holder, waits for it, and loses neither its order nor a file.
  const std::string barged =
      lock_json("claude-engine", k_other_pid, now_s() + 600, host, "an older test");
  replace_file(path, barged);
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  CHECK(reports(report).empty());
  CHECK(read_file(path) == barged);
  CHECK(line_size(path) == 2);

  (void)std::filesystem::remove(std::filesystem::path(path));
  CHECK(first.wait() == 0);
  CHECK(second.wait() == 0);
  const auto seen = reports(report);
  REQUIRE(seen.size() == 2);
  CHECK(seen[0].first == line[0].pid);
  CHECK(seen[1].first == line[1].pid);
  CHECK_FALSE(exists(path));
}

TEST_CASE("the line: a wait that runs out says where the process stood") {
  const Isolated isolated;
  test::TempDir dir("gpu_line_give_up");
  const std::string path = dir.file("gpu.lock");
  const gpu_lock::Identity self = gpu_lock::current_identity();
  write_file(path, lock_json("astra-blender", 4321, now_s() + 600, self.host, "bake: desert"));

  // Another process has been in the line for twelve seconds; this one stands behind it, and its
  // give-up line names the lock's holder, its own place and who is first.
  const std::string out_path = dir.file("out.txt");
  std::FILE* out = nullptr;
#if defined(_MSC_VER)
  (void)fopen_s(&out, out_path.c_str(), "wb");
#else
  out = std::fopen(out_path.c_str(), "wb");
#endif
  REQUIRE(out != nullptr);
  {
    // Another process's ticket, as that process would have written it: ahead of this one.
    const std::string directory = gpu_lock::line_directory(path);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(directory), ec);
    const i64 arrived_ms = time::wall_unix_ms() - 12'000;
    char name[96];
    std::snprintf(name, sizeof(name), "t-%016lld-%016lld-%010llu-0000.json",
                  static_cast<long long>(arrived_ms + 120'000), static_cast<long long>(arrived_ms),
                  static_cast<unsigned long long>(k_other_pid));
    write_file(directory + "/" + name,
               lock_json("claude-engine", k_other_pid, now_s() + 600, self.host, "gfx tests"));
  }
  gpu_lock::HoldConfig config = config_at(path);
  config.out = out;
  config.look_ms = 50;
  config.wait_until_unix_s = now_s() + 1;
  {
    gpu_lock::DeviceHold hold;
    CHECK(hold.acquire(config) == gpu_lock::HoldMode::GaveUp);
  }
  std::fclose(out);
  const std::string said = read_file(out_path);
  CHECK_MESSAGE(said.find("waiting for the GPU lock, held by 'astra-blender'") != std::string::npos,
                said);
  CHECK_MESSAGE(said.find("gave up waiting for the GPU lock") != std::string::npos, said);
  CHECK_MESSAGE(said.find("bake: desert") != std::string::npos, said);
  CHECK_MESSAGE(said.find("behind 1 of the 2 in the line, first 'claude-engine' (pid 999999937): "
                          "gfx tests, waiting 1") != std::string::npos,
                said);
  CHECK(line_size(path) == 1);  // this process's ticket went with its give-up
}

TEST_CASE("the line: gpu-lock.ps1 status prints the holder and the line a C++ waiter stands in") {
  if (!have_probe()) return;
  const std::string script =
      test::data_path(ENGINE_SOURCE_DIR "/tools/gpu-lock.ps1", "tools/gpu-lock.ps1");
  if (!test::path_exists(script)) {
    MESSAGE("not in this bundle: " << script);
    return;
  }
  const Isolated isolated;
  test::TempDir dir("gpu_line_status");
  const std::string path = dir.file("gpu.lock");
  const std::string report = dir.file("report.txt");
  write_file(path, lock_json("astra-blender", 4321, now_s() + 600,
                             gpu_lock::current_identity().host, "bake: desert"));
  platform::Process first, second;
  start_probe(first, {"--on", "--lock", path, "--look", "50", "--report", report, "hold", "0"});
  REQUIRE(eventually([&] { return line_size(path) == 1; }, 20000));
  start_probe(second,
              {"--on", "--lock", path, "--look", "50", "--report", report, "holds", "1", "0"});
  REQUIRE(eventually([&] { return line_size(path) == 2; }, 20000));

  const std::vector<std::string> args = {"pwsh",   "-NoProfile", "-File", script,
                                         "status", "-LockFile",  path};
  const std::vector<std::string_view> argv(args.begin(), args.end());
  platform::Process status;
  std::string error;
  std::string output;
  const bool ran =
      status.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error, true);
  if (ran) {
    status.close_stdin();
    status.read_all(output);
    CHECK_MESSAGE(status.wait() == 2, output);  // held
    // The two tickets this module wrote, read by the PowerShell half, in the line's order.
    const usize holder = output.find("bake: desert");
    const usize one = output.find("1. 'claude-engine'");
    const usize hold = output.find("gpu lock probe: hold,");
    const usize two = output.find("2. 'claude-engine'");
    const usize holds = output.find("gpu lock probe: holds,");
    CHECK_MESSAGE(holder != std::string::npos, output);
    CHECK_MESSAGE((one != std::string::npos && hold != std::string::npos && one < hold), output);
    CHECK_MESSAGE(
        (two != std::string::npos && holds != std::string::npos && hold < two && two < holds),
        output);
  } else {
    MESSAGE("skipped: no pwsh to run tools/gpu-lock.ps1: " << error);
  }
  (void)std::filesystem::remove(std::filesystem::path(path));
  CHECK(first.wait() == 0);
  CHECK(second.wait() == 0);
}

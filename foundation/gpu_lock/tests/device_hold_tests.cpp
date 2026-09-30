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

gpu_lock::State lock_at(const std::string& path) {
  gpu_lock::Identity self = gpu_lock::current_identity();
  self.holder_pid = 0;
  return gpu_lock::read(path, self, now_s());
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

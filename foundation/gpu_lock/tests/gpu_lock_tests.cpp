// The machine-wide GPU lock's file and the harness's lease (foundation/gpu_lock/gpu_lock.h;
// docs/subsystems/gpu_lock.md). Every case writes its lock into its own scratch directory, so the
// machine's real lock — and whoever holds it while the suite runs — never reaches a result. What
// the benchmark harness does with a reading (quiet or not, the WARNING, the JSON) is
// foundation/bench's tests'.

#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/time/time.h>
#include <foundation/gpu_lock/gpu_lock.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace engine;

namespace {

i64 now_s() { return time::wall_unix_ms() / 1000; }

// A pid no test process has: the "somebody else" of every case.
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

}  // namespace

TEST_CASE("gpu lock: ISO 8601 times round-trip, in the forms every holder writes") {
  // 2026-09-22T18:40:00Z; midnight of that day is 1790035200.
  const i64 t = 1790035200 + 18 * 3600 + 40 * 60;
  CHECK(gpu_lock::format_iso8601_utc(t) == "2026-09-22T18:40:00Z");
  i64 back = 0;
  REQUIRE(gpu_lock::parse_iso8601_utc("2026-09-22T18:40:00Z", back));
  CHECK(back == t);
  // What Python's isoformat() writes, and a zone that is not UTC.
  REQUIRE(gpu_lock::parse_iso8601_utc("2026-09-22T18:40:00.123456+00:00", back));
  CHECK(back == t);
  REQUIRE(gpu_lock::parse_iso8601_utc("2026-09-22T20:40:00+02:00", back));
  CHECK(back == t);
  CHECK(gpu_lock::format_iso8601_utc(0) == "1970-01-01T00:00:00Z");
  CHECK_FALSE(gpu_lock::parse_iso8601_utc("", back));
  CHECK_FALSE(gpu_lock::parse_iso8601_utc("22/09/2026 18:40", back));
  CHECK_FALSE(gpu_lock::parse_iso8601_utc("2026-09-22T18:40:00Zjunk", back));
  CHECK_FALSE(gpu_lock::parse_iso8601_utc("2026-13-22T18:40:00Z", back));
}

TEST_CASE("gpu lock: absent is free") {
  test::TempDir dir("gpu_lock_absent");
  const gpu_lock::State s =
      gpu_lock::read(dir.file("gpu.lock"), gpu_lock::current_identity(), now_s());
  CHECK_FALSE(s.present);
  CHECK_FALSE(s.held_by_other());
}

TEST_CASE("gpu lock: another tool's lock, or another engine agent's, is somebody else's") {
  test::TempDir dir("gpu_lock_other");
  const std::string path = dir.file("gpu.lock");
  const gpu_lock::Identity self = gpu_lock::current_identity();

  // One reading of the clock for the file and the expectation: two can straddle a second.
  const i64 expires = now_s() + 1800;
  write_file(path, lock_json("astra-blender", 4321, expires, self.host, "bake: desert"));
  gpu_lock::State s = gpu_lock::read(path, self, now_s());
  CHECK(s.present);
  CHECK(s.readable);
  CHECK_FALSE(s.mine);
  CHECK_FALSE(s.expired);
  CHECK(s.held_by_other());
  CHECK(s.owner == "astra-blender");
  CHECK(s.purpose == "bake: desert");
  CHECK(s.pid == 4321);
  CHECK(s.started == gpu_lock::format_iso8601_utc(expires - 600));  // lock_json's start

  // Another *engine* agent: the same owner label, a different process. An owner match alone
  // would call this ours, and one agent's measurement would run straight through another's render.
  write_file(path, lock_json(self.owner, k_other_pid, now_s() + 1800, self.host));
  s = gpu_lock::read(path, self, now_s());
  CHECK_FALSE(s.mine);
  CHECK(s.held_by_other());
}

TEST_CASE("gpu lock: held by this process, or for it, is mine") {
  test::TempDir dir("gpu_lock_self");
  const std::string path = dir.file("gpu.lock");
  gpu_lock::Identity self = gpu_lock::current_identity();
  REQUIRE(self.pid != 0);
  CHECK_FALSE(self.owner.empty());

  write_file(path, lock_json(self.owner, self.pid, now_s() + 1800, self.host));
  gpu_lock::State s = gpu_lock::read(path, self, now_s());
  CHECK(s.mine);
  CHECK_FALSE(s.held_by_other());

  // A wrapper took it for this process (tools/gpu-lock.ps1 run sets ENGINE_GPU_LOCK_HOLDER).
  self.holder_pid = 4242;
  write_file(path, lock_json(self.owner, 4242, now_s() + 1800, self.host));
  CHECK(gpu_lock::read(path, self, now_s()).mine);

  // The right pid under another owner label, or on another host, is not ours.
  write_file(path, lock_json("astra-blender", self.pid, now_s() + 1800, self.host));
  CHECK_FALSE(gpu_lock::read(path, self, now_s()).mine);
  write_file(path, lock_json(self.owner, self.pid, now_s() + 1800, "some-other-box"));
  if (!self.host.empty()) CHECK_FALSE(gpu_lock::read(path, self, now_s()).mine);
}

TEST_CASE("gpu lock: expired is free — except a person's") {
  test::TempDir dir("gpu_lock_expired");
  const std::string path = dir.file("gpu.lock");
  const gpu_lock::Identity self = gpu_lock::current_identity();

  write_file(path, lock_json("astra-blender", 4321, now_s() - 60, self.host));
  gpu_lock::State s = gpu_lock::read(path, self, now_s());
  CHECK(s.present);
  CHECK(s.expired);
  CHECK_FALSE(s.held_by_other());

  // GPU-LOCK.md: a lock owned by "marc" is never broken by a tool, expired or not, so the person
  // may still be using the GPU.
  write_file(path, lock_json("marc", 4321, now_s() - 60, self.host));
  s = gpu_lock::read(path, self, now_s());
  CHECK(s.expired);
  CHECK(s.held_by_other());
}

TEST_CASE("gpu lock: an unreadable file is being written until it is stale") {
  test::TempDir dir("gpu_lock_unreadable");
  const std::string path = dir.file("gpu.lock");
  const gpu_lock::Identity self = gpu_lock::current_identity();
  write_file(path, "{\"owner\":\"astra-");  // a writer caught half way
  gpu_lock::State lock = gpu_lock::read(path, self, now_s());
  CHECK(lock.present);
  CHECK_FALSE(lock.readable);
  CHECK_FALSE(lock.expired);
  CHECK(lock.held_by_other());

  // Left behind by a crash: older than k_unreadable_stale_s, and anyone may break it.
  std::error_code ec;
  std::filesystem::last_write_time(std::filesystem::path(path),
                                   std::filesystem::file_time_type::clock::now() -
                                       std::chrono::seconds(gpu_lock::k_unreadable_stale_s + 60),
                                   ec);
  REQUIRE_FALSE(ec);
  lock = gpu_lock::read(path, self, now_s());
  CHECK(lock.expired);
  CHECK_FALSE(lock.held_by_other());
}

TEST_CASE("gpu lock: a lease writes the protocol's file and deletes only its own") {
  test::TempDir dir("gpu_lock_lease");
  const std::string path = dir.file("gpu.lock");
  gpu_lock::Lease::Config config;
  config.path = path;
  config.self = gpu_lock::current_identity();
  config.self.holder_pid = 0;
  config.purpose = "bench test.*";
  config.lease_s = 600;
  config.timeout_s = 0;
  config.log = nullptr;

  {
    gpu_lock::Lease lease;
    REQUIRE(lease.acquire(config) == gpu_lock::Lease::Outcome::Taken);
    CHECK(lease.held());
    JsonValue body;
    REQUIRE(parse_json(read_file(path), body).ok);
    CHECK(body["owner"] == JsonValue(config.self.owner));
    CHECK(body["purpose"] == JsonValue("bench test.*"));
    u64 pid = 0;
    CHECK(body["pid"].get_u64(pid));
    CHECK(pid == config.self.pid);
    std::string_view expires;
    REQUIRE(body["expires"].get_string(expires));
    i64 expires_s = 0;
    REQUIRE(gpu_lock::parse_iso8601_utc(expires, expires_s));
    CHECK(expires_s >= now_s() + 590);
    CHECK(expires_s <= now_s() + 610);
    CHECK(gpu_lock::read(path, config.self, now_s()).mine);

    // A second lease in the same process is told the lock is already its own and does not
    // wait on itself, and does not delete it when it goes.
    {
      gpu_lock::Lease again;
      CHECK(again.acquire(config) == gpu_lock::Lease::Outcome::AlreadyMine);
      CHECK_FALSE(again.held());
    }
    CHECK(exists(path));
  }
  CHECK_FALSE(exists(path));  // released by the destructor

  // Somebody replaced the file after this lease's own expired and broke it: releasing must not
  // delete theirs.
  {
    gpu_lock::Lease lease;
    REQUIRE(lease.acquire(config) == gpu_lock::Lease::Outcome::Taken);
    write_file(path, lock_json("astra-blender", 4321, now_s() + 1800, config.self.host));
    lease.release();
  }
  CHECK(exists(path));
  CHECK(read_file(path).find("astra-blender") != std::string::npos);
}

TEST_CASE(
    "gpu lock: a lease waits for somebody else's lock, breaks an expired one, and never "
    "a person's") {
  test::TempDir dir("gpu_lock_contended");
  const std::string path = dir.file("gpu.lock");
  gpu_lock::Lease::Config config;
  config.path = path;
  config.self = gpu_lock::current_identity();
  config.self.holder_pid = 0;
  config.purpose = "bench test.*";
  config.timeout_s = 0;
  config.log = nullptr;

  const std::string theirs = lock_json("astra-blender", 4321, now_s() + 1800, config.self.host);
  write_file(path, theirs);
  {
    gpu_lock::Lease lease;
    CHECK(lease.acquire(config) == gpu_lock::Lease::Outcome::TimedOut);
    CHECK_FALSE(lease.held());
  }
  CHECK(read_file(path) == theirs);  // untouched

  write_file(path, lock_json("astra-blender", 4321, now_s() - 5, config.self.host));
  {
    gpu_lock::Lease lease;
    CHECK(lease.acquire(config) == gpu_lock::Lease::Outcome::Taken);
    CHECK(gpu_lock::read(path, config.self, now_s()).mine);
  }
  CHECK_FALSE(exists(path));

  const std::string persons = lock_json("marc", 4321, now_s() - 5, config.self.host);
  write_file(path, persons);
  {
    gpu_lock::Lease lease;
    CHECK(lease.acquire(config) == gpu_lock::Lease::Outcome::TimedOut);
  }
  CHECK(read_file(path) == persons);

  // A directory that does not exist is a machine that does not use the lock.
  gpu_lock::Lease::Config elsewhere = config;
  elsewhere.path = dir.file("no-such-directory/gpu.lock");
  gpu_lock::Lease lease;
  CHECK(lease.acquire(elsewhere) == gpu_lock::Lease::Outcome::NoDirectory);
}

TEST_CASE("gpu lock: a wrapper's lock is this process's while it lives, and breakable once dead") {
  test::TempDir dir("gpu_lock_wrapper");
  const std::string path = dir.file("gpu.lock");
  gpu_lock::Lease::Config config;
  config.path = path;
  config.self = gpu_lock::current_identity();
  config.self.holder_pid = 4242;  // ENGINE_GPU_LOCK_HOLDER, as `tools/gpu-lock.ps1 run` sets it
  config.purpose = "bench test.*";
  config.timeout_s = 0;
  config.log = nullptr;

  // Refreshed by its heartbeat: held for this process, so the lease neither waits nor writes.
  const std::string live = lock_json(config.self.owner, 4242, now_s() + 600, config.self.host);
  write_file(path, live);
  {
    gpu_lock::Lease lease;
    CHECK(lease.acquire(config) == gpu_lock::Lease::Outcome::AlreadyMine);
  }
  CHECK(read_file(path) == live);

  // Its lease ran out: the wrapper died, and its lock is anybody's to break — ours included.
  write_file(path, lock_json(config.self.owner, 4242, now_s() - 5, config.self.host));
  gpu_lock::Lease lease;
  CHECK(lease.acquire(config) == gpu_lock::Lease::Outcome::Taken);
  CHECK(gpu_lock::read(path, config.self, now_s()).pid == config.self.pid);
}

#pragma once

// The machine-wide GPU lock, read and taken from C++ (docs/subsystems/bench.md, "The GPU lock").
//
// One GPU is shared by several agents on the development machine, and not all of them are this
// engine: a Blender agent bakes and renders on the same card and never sees this harness. The
// protocol they agree on is one JSON file whose presence means "the GPU is taken":
//
//     {"owner":"claude-engine","purpose":"bench physics.*","pid":1234,
//      "started":"2026-09-22T18:40:00Z","expires":"2026-09-22T19:10:00Z","host":"DESKTOP"}
//
// created atomically (create-new), deleted to release, and carrying an expiry that is the
// holder's promise: "if I have not released or refreshed by then, treat me as dead". An expired
// lock may be broken; an unexpired one never; a lock owned by "marc" (a person) never by a tool.
// The protocol is written once for every tool on the machine in `D:\workspace\GPU-LOCK.md`;
// `tools/lib/MachineLock.psm1` is the PowerShell implementation and this is the C++ one.
//
// Why the harness knows about it at all. `--require-quiet` looked at utilization, and
// utilization is a sample: another agent's render that is between frames, loading a scene, or
// about to start reads as an idle GPU. A lock held by someone else says what utilization cannot,
// that the GPU is *spoken for*, so the harness treats it as not quiet and records who held it.
// And `--gpu-lock` takes the lock for the length of a run, so a measurement session is one
// command rather than a wrapper, a run, and a release that a failure can skip.
//
// "Mine" is decided by process, not by owner string: every engine agent on the machine is
// `claude-engine`, so an owner match alone would let one agent's measurement run straight through
// another agent's render. A lock is this process's when its owner is this process's owner and its
// pid is this process — the harness took it — or the process named by `ENGINE_GPU_LOCK_HOLDER`,
// which a wrapper that took it on this process's behalf (`tools/gpu-lock.ps1 run`, the shared
// PowerShell module's `Invoke-WithMachineLock`) sets for the command it runs.

#include <core/base/macros.h>
#include <core/base/types.h>

#include <cstdio>
#include <string>
#include <string_view>

namespace engine::bench {

// What the lock file said when it was read. `present == false` is "no lock file": the GPU is
// not spoken for. Only `held_by_other()` makes a machine not quiet.
struct GpuLockState {
  bool present = false;   // a lock file exists at the path
  bool readable = false;  // it parsed as the protocol's JSON object
  bool mine = false;      // held by this process, or by the holder ENGINE_GPU_LOCK_HOLDER names
  bool expired = false;   // `expires` is in the past (an unreadable file counts once it is stale)
  std::string owner;      // as the holder wrote them; empty when unreadable
  std::string purpose;    // at most 200 bytes of it
  std::string expires;    // ISO 8601 UTC, as written

  // Somebody else has the GPU. A person's lock counts even after it expires, because GPU-LOCK.md
  // says a tool never breaks one: whoever wrote it may still be at the keyboard.
  bool held_by_other() const noexcept { return present && !mine && (!expired || owner == "marc"); }
};

// Who "this process" is when the question is whether a lock is its own.
struct GpuLockIdentity {
  std::string owner;   // $ENGINE_GPU_LOCK_OWNER, else "claude-engine"
  u64 pid = 0;         // this process
  u64 holder_pid = 0;  // $ENGINE_GPU_LOCK_HOLDER: who took the lock for this process; 0 = nobody
  std::string host;    // COMPUTERNAME / gethostname; a lock naming another host is never mine
};

// $ENGINE_GPU_LOCK, else the machine default: `D:\workspace\gpu.lock` on Windows (the path
// GPU-LOCK.md names) and `/tmp/gpu.lock` elsewhere.
std::string default_gpu_lock_path();

// This process's identity, from the environment.
GpuLockIdentity current_gpu_lock_identity();

// Reads the lock at `path` as of `now_unix_s`. Never waits, never writes. A file that exists but
// does not parse is present and not readable; it is treated as being written — held, not
// expired — until it is older than k_unreadable_stale_s, after which it is garbage anyone may
// break, which is what keeps one truncated file from blocking the machine forever.
GpuLockState read_gpu_lock(const std::string& path, const GpuLockIdentity& self, i64 now_unix_s);

inline constexpr i64 k_unreadable_stale_s = 60;

// "2026-09-22T18:40:00Z" <-> Unix seconds. The parser also accepts fractional seconds and a
// numeric offset ("...00.123456+00:00"), which is what Python's isoformat() writes.
bool parse_iso8601_utc(std::string_view text, i64& out_unix_s) noexcept;
std::string format_iso8601_utc(i64 unix_s);

// A lock this process holds for a while: taken by acquire(), kept alive by refresh_if_due(),
// dropped by release() or the destructor. While it is held, SIGINT and SIGTERM delete the file
// before the process dies, so Ctrl+C does not leave the GPU spoken for until the lease runs out;
// a crash still does, and the lease is what bounds that.
class GpuLockLease {
 public:
  struct Config {
    std::string path;          // empty: default_gpu_lock_path()
    GpuLockIdentity self;      // who takes it
    std::string purpose;       // one line: what is running
    i64 lease_s = 30 * 60;     // how long a silent holder keeps it
    i64 poll_s = 20;           // how often a waiter looks again (GPU-LOCK.md: 15-30 s)
    i64 timeout_s = 4 * 3600;  // how long to wait before giving up; 0 = one attempt
    std::FILE* log = stderr;   // who we are waiting for, and what was broken
  };

  enum class Outcome : u8 {
    Taken,        // this lease wrote the file and will delete it
    AlreadyMine,  // the lock was already this process's (a wrapper took it); nothing to release
    NoDirectory,  // the lock's directory does not exist: this machine does not use the protocol
    TimedOut,     // somebody else still held it when timeout_s ran out
    Error,        // the file could not be written for another reason
  };

  GpuLockLease() noexcept = default;
  ~GpuLockLease();
  GpuLockLease(const GpuLockLease&) = delete;
  GpuLockLease& operator=(const GpuLockLease&) = delete;

  Outcome acquire(const Config& config);
  // Pushes `expires` out to now + lease when less than half the lease is left. Call it between
  // units of work, never inside a timed region; a failed refresh is retried at the next call.
  void refresh_if_due();
  // Deletes the file if it is still the one this lease wrote. Idempotent.
  void release() noexcept;

  bool held() const noexcept { return held_; }
  const std::string& path() const noexcept { return path_; }
  i64 expires_unix_s() const noexcept { return expires_s_; }

 private:
  std::string body_json() const;

  Config config_;
  std::string path_;
  std::string started_;
  i64 expires_s_ = 0;
  bool held_ = false;
};

const char* to_string(GpuLockLease::Outcome outcome) noexcept;

}  // namespace engine::bench

#pragma once

// The machine-wide GPU lock, read and taken from C++ (docs/subsystems/gpu_lock.md).
//
// One GPU is shared by several agents on the development machine, and not all of them are this
// engine: a Blender agent bakes and renders on the same card and never sees this code. The
// protocol they agree on is one JSON file whose presence means "the GPU is taken":
//
//     {"owner":"claude-engine","purpose":"bench physics.*","pid":1234,
//      "started":"2026-09-22T18:40:00Z","expires":"2026-09-22T19:10:00Z","host":"DESKTOP"}
//
// created atomically (create-new), deleted to release, and carrying an expiry that is the
// holder's promise: "if I have not released or refreshed by then, treat me as dead". An expired
// lock may be broken; an unexpired one never; a lock owned by "marc" (a person) never by a tool.
// The protocol is written once for every tool on the machine in `D:\workspace\GPU-LOCK.md`;
// `tools/lib/MachineLock.psm1` is the PowerShell implementation and this module is the C++ one.
// There are those two and no more: the benchmark harness (`foundation/bench`, `--gpu-lock`) and
// device creation (`device_hold.h`, through `gfx::Device`) both take the lock through here.
//
// "Mine" is decided by process, not by owner string: every engine agent on the machine is
// `claude-engine`, so an owner match alone would let one agent's measurement run straight through
// another agent's render. A lock is this process's when its owner is this process's owner and its
// pid is this process — it took the lock itself — or the process named by
// `ENGINE_GPU_LOCK_HOLDER`, which whoever took the lock on this process's behalf sets for the
// processes it starts: `tools/gpu-lock.ps1 run`, the PowerShell module's `Invoke-WithMachineLock`,
// and a process holding the lock for its own GPU device (`device_hold.h`).

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>

#include <cstdio>
#include <string>
#include <string_view>

namespace engine::gpu_lock {

// What the lock file said when it was read. `present == false` is "no lock file": the GPU is
// not spoken for. Only `held_by_other()` makes a machine not quiet.
struct State {
  bool present = false;   // a lock file exists at the path
  bool readable = false;  // it parsed as the protocol's JSON object
  bool mine = false;      // held by this process, or by the holder ENGINE_GPU_LOCK_HOLDER names
  bool expired = false;   // `expires` is in the past (an unreadable file counts once it is stale)
  u64 pid = 0;            // the holder's process, as it wrote it; 0 when unreadable
  std::string owner;      // as the holder wrote them; empty when unreadable
  std::string purpose;    // at most 200 bytes of it
  std::string started;    // ISO 8601 UTC, as written: with `pid`, which of a process's holds
  std::string expires;    // ISO 8601 UTC, as written

  // Somebody else has the GPU. A person's lock counts even after it expires, because GPU-LOCK.md
  // says a tool never breaks one: whoever wrote it may still be at the keyboard.
  bool held_by_other() const noexcept { return present && !mine && (!expired || owner == "marc"); }
};

// Who "this process" is when the question is whether a lock is its own.
struct Identity {
  std::string owner;   // $ENGINE_GPU_LOCK_OWNER, else "claude-engine"
  u64 pid = 0;         // this process
  u64 holder_pid = 0;  // $ENGINE_GPU_LOCK_HOLDER: who took the lock for this process; 0 = nobody
  std::string host;    // COMPUTERNAME / gethostname; a lock naming another host is never mine
};

// $ENGINE_GPU_LOCK, else the machine default: `D:\workspace\gpu.lock` on Windows (the path
// GPU-LOCK.md names) and `/tmp/gpu.lock` elsewhere.
std::string default_path();

// This process's identity, from the environment.
Identity current_identity();

// Reads the lock at `path` as of `now_unix_s`. Never waits, never writes. A file that exists but
// does not parse is present and not readable; it is treated as being written — held, not
// expired — until it is older than k_unreadable_stale_s, after which it is garbage anyone may
// break, which is what keeps one truncated file from blocking the machine forever.
State read(const std::string& path, const Identity& self, i64 now_unix_s);

inline constexpr i64 k_unreadable_stale_s = 60;

// "2026-09-22T18:40:00Z" <-> Unix seconds. The parser also accepts fractional seconds and a
// numeric offset ("...00.123456+00:00"), which is what Python's isoformat() writes.
bool parse_iso8601_utc(std::string_view text, i64& out_unix_s) noexcept;
std::string format_iso8601_utc(i64 unix_s);

// ---- the line ---------------------------------------------------------------------------------
//
// The lock file says who has the GPU; the line says who is next (docs/subsystems/gpu_lock.md,
// "The line"; roadmap T96). A process that asks for the lock writes a **ticket** — one small JSON
// file in the directory `<lock path>.line` — before it tries the lock file, tries the lock file
// only while no live ticket stands before its own, and deletes its ticket when it has the lock or
// stops asking. The lock file and its rules are unchanged, so the line is advice among the
// processes that take part in it and exclusion is still the create-new of the lock file: a tool
// that knows nothing of tickets behaves exactly as it did.
//
// A ticket's file name is its place, so the line is a sorted directory listing:
//
//     t-<key ms, 16 digits>-<arrived ms, 16 digits>-<pid, 10 digits>-<counter, 4 digits>.json
//
// `arrived` is the wall clock in Unix milliseconds when the process began to ask, and
// `key` = arrived + the lease it asked for (at most k_declared_cap_s): the moment it would be done
// if it were served at once and held for what it declared. Earliest key first; equal keys by
// arrival, then by pid, then by the process's own counter — nothing in the order depends on which
// process the scheduler ran first, only on what the tickets say. Equal declarations are plain
// first come, first served, and a process that releases and asks again takes a new ticket at the
// back. A short ask passes a long one only while the long one has waited less than the difference
// between them, so nobody is passed for longer than the lease it asked for.
//
// The body is the lock's own shape (`owner`, `purpose`, `pid`, `started` = arrival, `expires`,
// `host`, and `declared_s`), refreshed by rename like the lock. A ticket whose `expires` has
// passed is a waiter that died: it is skipped, and removed, by whoever looks. A waiter refreshes
// its ticket from the same loop that tries the lock, so a ticket is never alive while its owner
// has stopped trying.
inline constexpr i64 k_ticket_lease_s = 30;       // a dead waiter stops counting after this
inline constexpr i64 k_line_look_ms = 250;        // how often a waiter looks at the line
inline constexpr i64 k_declared_cap_s = 30 * 60;  // GPU-LOCK.md's default lease

struct Waiter {
  std::string owner;
  std::string purpose;  // at most 200 bytes of it
  u64 pid = 0;
  i64 arrived_unix_ms = 0;
  i64 declared_s = 0;  // the lease it asked for, as the line counted it (capped)
};

// `<lock path>.line`.
std::string line_directory(const std::string& lock_path);

// The live tickets of the lock at `lock_path`, in the order they will be served. Never waits and
// never writes: a dead ticket is left for a waiter to remove.
Vector<Waiter> read_line(const std::string& lock_path, i64 now_unix_s);

// A lock this process holds for a while: taken by acquire(), kept alive by refresh_if_due(),
// dropped by release() or the destructor. While it is held, SIGINT and SIGTERM delete the file
// before the process dies, so Ctrl+C does not leave the GPU spoken for until the lease runs out;
// a crash still does, and the lease is what bounds that. This is the benchmark harness's hold
// (`--gpu-lock`), refreshed between benchmarks and never from another thread; a GPU device's
// hold is `device_hold.h`'s.
class Lease {
 public:
  struct Config {
    std::string path;              // empty: default_path()
    Identity self;                 // who takes it
    std::string purpose;           // one line: what is running
    i64 lease_s = 30 * 60;         // how long a silent holder keeps it
    i64 poll_s = 20;               // an upper bound on a look; the line's look_ms is the shorter
    i64 timeout_s = 4 * 3600;      // how long to wait before giving up; 0 = one attempt
    std::FILE* log = stderr;       // who we are waiting for, and what was broken
    i64 look_ms = k_line_look_ms;  // how often a waiter looks at the line and the lock
    i64 ticket_lease_s = k_ticket_lease_s;  // how long its ticket outlives it if it dies
  };

  enum class Outcome : u8 {
    Taken,        // this lease wrote the file and will delete it
    AlreadyMine,  // the lock was already this process's (a wrapper took it); nothing to release
    NoDirectory,  // the lock's directory does not exist: this machine does not use the protocol
    TimedOut,     // somebody else still held it when timeout_s ran out
    Error,        // the file could not be written for another reason
  };

  Lease() noexcept = default;
  ~Lease();
  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;

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

const char* to_string(Lease::Outcome outcome) noexcept;

}  // namespace engine::gpu_lock

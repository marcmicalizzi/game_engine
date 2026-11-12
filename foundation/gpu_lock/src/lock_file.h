#pragma once

// The file operations the protocol rests on, shared by the harness's lease (gpu_lock.cpp) and
// the device hold (device_hold.cpp). Internal to the module.

#include <core/base/types.h>
#include <foundation/gpu_lock/gpu_lock.h>

#include <string>
#include <string_view>

namespace engine::gpu_lock::detail {

// getenv, spelled the way MSVC compiles without a deprecation warning.
std::string environment(const char* name);
// Sets a variable in this process's environment, where the children it starts from now on
// inherit it (the C runtime's copy and, on Windows, the OS block CreateProcess hands a child).
// An empty value removes it.
void set_environment(const char* name, const std::string& value);

u64 current_pid() noexcept;
std::string current_host();
// This executable's full path, forward slashes; empty when the OS will not say.
std::string executable_path();

// Reads the whole file. False when it does not exist (`exists` false) or cannot be opened now
// (`exists` true: a writer has it exclusively, which is what "being written" looks like).
bool read_text(const std::string& path, std::string& out, bool& exists);

// Create-new, or nothing: the atomic step the protocol rests on.
enum class CreateResult : u8 { Created, Exists, Failed };
CreateResult create_exclusive(const std::string& path, std::string_view body);

// Writes `body` beside `path` and moves it over, so a reader sees the old file or the new one.
bool replace_atomically(const std::string& path, std::string_view body);

// Breaks the lock whose bytes were `judged`, and only that one (moved aside, compared, deleted,
// or put back when somebody replaced it in between).
bool break_stale(const std::string& path, const std::string& judged);

// Deletes a file, retrying for a second: on Windows a reader that has it open without
// FILE_SHARE_DELETE — every other tool's poll, for the moment it reads — makes the delete fail,
// and a release that gave up there would leave the GPU spoken for until the lease ran out.
bool remove_file(const std::string& path);

// Appends one line to a file, in one write, so lines from several processes do not interleave.
void append_line(const std::string& path, std::string_view line);

struct ParsedLock {
  bool readable = false;
  std::string owner;
  std::string purpose;
  std::string started;
  std::string expires;
  std::string host;
  u64 pid = 0;
};
ParsedLock parse_lock(std::string_view text);

// The protocol's JSON object (the fields GPU-LOCK.md names; readers look them up by name).
std::string lock_body(std::string_view owner, std::string_view purpose, u64 pid,
                      std::string_view started, i64 expires_unix_s, std::string_view host);

// While a hold is armed, SIGINT and SIGTERM delete its file before the process dies, as long as
// its lease has not run out (after that the file may be somebody else's). One file at a time: a
// process holds the lock through one path.
void arm_signal_release(const std::string& path, i64 expires_unix_s);
void update_signal_expiry(i64 expires_unix_s);
void disarm_signal_release();

// Wall-clock Unix seconds.
i64 now_unix_s();

// ---- the line (gpu_lock.h, "the line") --------------------------------------------------------

// Where a ticket stands at one look.
struct LinePlace {
  bool lined = false;  // false: this process has no ticket (the line's directory cannot be made)
  u32 ahead = 0;       // live tickets before this one; 0 is "try the lock"
  u32 waiting = 0;     // live tickets, this one included
  Waiter first;        // the head of the line when ahead > 0
};

// One ask's place in the line. Entered once, looked at from the loop that tries the lock (which
// is also what keeps it alive), and left when the lock is taken or the ask ends.
class Ticket {
 public:
  Ticket() noexcept = default;
  ~Ticket() { leave(); }
  Ticket(const Ticket&) = delete;
  Ticket& operator=(const Ticket&) = delete;

  // Writes the ticket. False when the line's directory cannot be made or written: the caller
  // then asks as a tool without tickets does, which is the protocol as it was.
  bool enter(const std::string& lock_path, const Identity& self, std::string_view purpose,
             i64 declared_s, i64 ticket_lease_s);
  // Refreshes this ticket when a third of its lease has gone (or it was removed while this
  // process stalled), removes the dead tickets it sees, and says where this one stands.
  LinePlace look();
  void leave() noexcept;
  bool entered() const noexcept { return entered_; }

 private:
  bool write(i64 now_s);

  std::string directory_;
  std::string name_;
  std::string path_;
  std::string owner_;
  std::string purpose_;
  std::string host_;
  u64 pid_ = 0;
  i64 arrived_ms_ = 0;
  i64 declared_s_ = 0;
  i64 lease_s_ = 0;
  i64 refresh_at_s_ = 0;
  bool entered_ = false;
};

// Whether nobody is waiting: no live ticket in the lock's line (the dead ones it finds are
// removed). An ask that finds the line empty tries the lock without a ticket — nobody is there to
// be passed — and takes one only if that fails, so an uncontended take writes no file but the
// lock's own.
bool line_is_empty(const std::string& lock_path, i64 ticket_lease_s);

// "first of 3 in the line", or "behind 2 of the 4 in the line, first 'owner' (pid 1): purpose,
// waiting 12 s"; empty when the process has no ticket.
std::string describe_place(const LinePlace& place, i64 now_unix_ms);

}  // namespace engine::gpu_lock::detail

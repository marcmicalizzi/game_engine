#pragma once

// The file operations the protocol rests on, shared by the harness's lease (gpu_lock.cpp) and
// the device hold (device_hold.cpp). Internal to the module.

#include <core/base/types.h>

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

}  // namespace engine::gpu_lock::detail

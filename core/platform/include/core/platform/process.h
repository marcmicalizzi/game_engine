#pragma once

// Child processes with piped standard input and output, for tools: engine-cli drives
// engine-host through one, content-build and generator services will run behind them, and
// end-to-end tests spawn both sides. The child's standard error is inherited so its diagnostics
// reach the terminal. Arguments and paths are UTF-8.

#include <core/base/macros.h>
#include <core/base/types.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::platform {

class Process {
 public:
  Process() noexcept = default;
  ~Process();
  ENGINE_NON_COPYABLE(Process);

  // argv[0] is the executable (a path, or a name found through PATH). False with a
  // description in `error` when the process cannot be started. With `merge_stderr` the
  // child's standard error is captured into the same stream as its standard output.
  bool spawn(std::span<const std::string_view> argv, std::string* error = nullptr,
             bool merge_stderr = false);
  bool spawned() const noexcept { return spawned_; }

  // Writes every byte to the child's standard input.
  bool write(std::string_view data) noexcept;
  // Signals end of input to the child.
  void close_stdin() noexcept;
  // Next line of the child's standard output without its terminator ("\n" or "\r\n"). False
  // at end of output with nothing pending.
  bool read_line(std::string& line);
  // Everything remaining on the child's standard output.
  bool read_all(std::string& out);

  // Waits for the child to exit and returns its exit code (-1 when unknown). Idempotent.
  i32 wait() noexcept;
  void kill() noexcept;

 private:
  bool fill_pending();
  void close_handles() noexcept;

#if ENGINE_PLATFORM_WINDOWS
  void* process_ = nullptr;
  void* stdin_write_ = nullptr;
  void* stdout_read_ = nullptr;
#else
  i64 pid_ = -1;
  int stdin_write_ = -1;
  int stdout_read_ = -1;
#endif
  std::string pending_;
  i32 exit_code_ = -1;
  bool spawned_ = false;
  bool exited_ = false;
  bool eof_ = false;
};

// Directory holding the running executable, UTF-8 with forward slashes and no trailing slash.
std::string executable_directory();

}  // namespace engine::platform

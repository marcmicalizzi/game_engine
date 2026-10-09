// Every fatal fault prints one line (docs/subsystems/platform.md, "Every fatal fault prints one
// line"): `engine_platform_fault_probe` fails on request, as a child, and the test reads what its
// death left on stderr and how it died — on Windows the exit code, on Linux the signal the parent's
// wait reports. One line per kind, naming the fault, the faulting instruction as module plus
// offset, the thread and a stack, and never a second.
//
// Before the handlers existed every fault here printed nothing — measured on Windows in
// `msvc-release` (2026-10-04): `read` and `worker` exited 0xC0000005 and `abort` 3, all silent;
// Linux had no handler at all — and every case below failed on its "one line" check.
//
// Under a sanitizer (ENGINE_FAULT_UNDER_SANITIZER) the module installs nothing and the sanitizer's
// runtime reports every fault itself, so there is no line to read: each kind is checked for the
// other half of the promise instead — the probe still dies, and the engine prints no line over the
// runtime's report. Reading for the line there failed every kind of the first `linux-clang-asan`
// run (2026-10-07, roadmap F16); the line itself stays checked in every other build.
#include "../src/fault_report.h"

#include <core/base/types.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <cerrno>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

using namespace engine;

struct Outcome {
  std::string output;  // stdout and stderr together
  i32 code = -1;       // the exit code (Windows), or -1
  int died_of = 0;     // the signal it died of (Linux), or 0
};

const std::string& probe_path() {
  static const std::string path = test::app_path(ENGINE_PLATFORM_FAULT_PROBE_PATH);
  return path;
}

Outcome run_probe(const char* kind) {
  Outcome out;
#if defined(_WIN32)
  const std::string_view argv[] = {probe_path(), kind};
  platform::Process p;
  std::string error;
  REQUIRE_MESSAGE(p.spawn(std::span<const std::string_view>(argv), &error, true), error);
  p.close_stdin();
  p.read_all(out.output);
  out.code = p.wait();
#else
  // platform::Process reports a signalled child as -1; the signal is the point here.
  int pipe_fds[2];
  REQUIRE(::pipe(pipe_fds) == 0);
  posix_spawn_file_actions_t actions;
  ::posix_spawn_file_actions_init(&actions);
  ::posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], 1);
  ::posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], 2);
  ::posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
  ::posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
  std::string path = probe_path();
  std::string arg = kind;
  char* argv[] = {path.data(), arg.data(), nullptr};
  pid_t pid = 0;
  const int rc = ::posix_spawn(&pid, path.c_str(), &actions, nullptr, argv, environ);
  ::posix_spawn_file_actions_destroy(&actions);
  ::close(pipe_fds[1]);
  REQUIRE_MESSAGE(rc == 0, std::strerror(rc));
  char buffer[4096];
  for (;;) {
    const ssize_t n = ::read(pipe_fds[0], buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    out.output.append(buffer, static_cast<usize>(n));
  }
  ::close(pipe_fds[0]);
  int status = 0;
  pid_t waited = 0;
  do {
    waited = ::waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  REQUIRE(waited == pid);
  if (WIFEXITED(status)) out.code = WEXITSTATUS(status);
  if (WIFSIGNALED(status)) out.died_of = WTERMSIG(status);
#endif
  return out;
}

// The lines that begin "engine: fatal: ", each with its newline.
std::vector<std::string> fatal_lines(const std::string& output) {
  std::vector<std::string> lines;
  usize at = 0;
  while ((at = output.find("engine: fatal: ", at)) != std::string::npos) {
    const usize end = output.find('\n', at);
    lines.push_back(output.substr(at, end == std::string::npos ? std::string::npos : end - at + 1));
    at += 1;
  }
  return lines;
}

struct Case {
  const char* kind;
  const char* says;    // what the line names
  bool at_probe;       // the faulting instruction is the probe's own
  const char* thread;  // what the line says of the thread
#if defined(_WIN32)
  u32 exit_code;
#else
  int signal_number;
#endif
};

#if ENGINE_FAULT_UNDER_SANITIZER
// The sanitizer's runtime has the fault (fault_report.h): the probe dies of it — the runtime's
// report and exit status, or the signal's default action for the kinds it does not handle — and
// no engine line joins the report.
void check(const Case& c) {
  CAPTURE(c.kind);
  const Outcome out = run_probe(c.kind);
  INFO("the probe printed: " << out.output);
  CHECK(fatal_lines(out.output).empty());
  CHECK((out.code != 0 || out.died_of != 0));
}
#else
void check(const Case& c) {
  CAPTURE(c.kind);
  const Outcome out = run_probe(c.kind);
  INFO("the probe printed: " << out.output);
  const std::vector<std::string> lines = fatal_lines(out.output);
  CHECK(lines.size() == 1);
  if (lines.size() != 1) return;  // the next kind still runs, and says what it printed
  const std::string& line = lines[0];
  MESSAGE(c.kind << ": " << line);
  // A line of its own: whatever the runtime wrote before it (a debug build's "abort() has been
  // called" has no newline) ended first.
  const usize at = out.output.find("engine: fatal: ");
  CHECK((at == 0 || out.output[at - 1] == '\n'));
  CHECK(line.back() == '\n');
  CHECK(line.find(c.says) != std::string::npos);
  CHECK(line.find(c.thread) != std::string::npos);
  CHECK(line.find(" in thread ") != std::string::npos);
  // Module plus offset: the probe's file name, then "+0x".
  const usize probe = line.find("engine_platform_fault_probe");
  CHECK(probe != std::string::npos);
  if (probe != std::string::npos) CHECK(line.find("+0x", probe) != std::string::npos);
  if (c.at_probe) CHECK(line.find(" at engine_platform_fault_probe") != std::string::npos);
  // A stack, and the probe among it: main at least called the function that failed.
  const usize stack = line.find("; stack: ");
  CHECK(stack != std::string::npos);
  if (stack != std::string::npos) {
    CHECK(line.find("engine_platform_fault_probe", stack) != std::string::npos);
  }
#if defined(_WIN32)
  CHECK(static_cast<u32>(out.code) == c.exit_code);
#else
  CHECK(out.died_of == c.signal_number);
  CHECK(line.find("re-raised") != std::string::npos);
#endif
}
#endif

}  // namespace

TEST_CASE("fault report: a process that does not fault prints nothing of it") {
  const Outcome out = run_probe("none");
  CHECK(out.code == 0);
  CHECK(out.died_of == 0);
  CHECK(fatal_lines(out.output).empty());
}

TEST_CASE("fault report: every fatal fault prints one line and exits as documented") {
  // The exit statuses are docs/subsystems/platform.md's table: on Windows the exception's code
  // (abort() and a pure virtual call the runtime's 3, an invalid parameter the runtime's own
  // fail-fast code), on Linux the signal, re-raised with its default action.
#if defined(_WIN32)
  const Case cases[] = {
      {"read", "access violation reading 0x10", true, "(main)", 0xC0000005u},
      {"write", "access violation writing 0x10", true, "(main)", 0xC0000005u},
      {"execute", "access violation executing 0x", true, "(main)", 0xC0000005u},
      {"stack-overflow", "stack overflow", true, "(main)", 0xC00000FDu},
      {"illegal", "illegal instruction", true, "(main)", 0xC000001Du},
      {"divide", "integer divide by zero", true, "(main)", 0xC0000094u},
      {"abort", "abort()", false, "(main)", 3u},
      {"pure-virtual", "pure virtual function call", false, "(main)", 3u},
      {"invalid-parameter", "invalid parameter passed to the C runtime", false, "(main)",
       0xC0000409u},
      {"worker", "access violation writing 0x10", true, "\"fault-worker\"", 0xC0000005u},
  };
#else
  const Case cases[] = {
      {"read", "SIGSEGV, address not mapped reading 0x10", true, "(main)", SIGSEGV},
      {"write", "SIGSEGV, address not mapped writing 0x10", true, "(main)", SIGSEGV},
      {"execute", "SIGSEGV, access denied executing 0x", true, "(main)", SIGSEGV},
      {"stack-overflow", "SIGSEGV, stack overflow", true, "(main)", SIGSEGV},
      {"illegal", "SIGILL, illegal instruction", true, "(main)", SIGILL},
      {"divide", "SIGFPE, integer divide by zero", true, "(main)", SIGFPE},
      {"abort", "SIGABRT, abort()", false, "(main)", SIGABRT},
      {"bus", "SIGBUS, no such physical address reading 0x", true, "(main)", SIGBUS},
      {"worker", "SIGSEGV, address not mapped writing 0x10", true, "\"fault-worker\"", SIGSEGV},
  };
#endif
#if ENGINE_FAULT_UNDER_SANITIZER
  MESSAGE(
      "the one-line checks are skipped: this is a sanitizer build, whose runtime reports "
      "faults itself and in which nothing of the engine's is installed; checking that the "
      "probe still dies and the engine prints no line of its own instead");
#endif
  for (const Case& c : cases)
    check(c);
}

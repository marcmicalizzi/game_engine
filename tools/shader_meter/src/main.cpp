// shader-meter: runs one command (the build's slangc) and writes what it cost beside its output, so
// the build records every shader's peak memory and seconds for nothing (roadmap T118;
// docs/ci/what-to-run.md, "Heavy shaders share a pool"; cmake/EngineShaders.cmake).
//
//   shader-meter <cost file> <program> [arguments...]
//
// The program runs with this process's handles, environment and directory, so its diagnostics
// reach the build's log as they would have, and its exit code is this tool's. When it exits 0 the
// cost file gets two lines,
//
//   peak_mb <whole MiB>
//   seconds <wall seconds, one decimal>
//
// where the peak is the child's peak private commit on Windows (PeakPagefileUsage, what the commit
// limit is charged with) and its peak resident set on Linux (wait4's ru_maxrss). A child that
// fails writes no cost file, so a failed compile never leaves a measurement behind. It cannot
// start the program: exit 127 and a line on stderr, as a shell does.
//
// Standard library and the operating system only, like schemac: a host tool (ADR-0034) that
// builds before any engine module, links none, and is compiled for the build machine.

#include <chrono>
#include <cstdio>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h, which it needs.
#include <psapi.h>
#else
#include <cerrno>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>

extern char** environ;
#endif

namespace {

constexpr int k_cannot_start = 127;
constexpr int k_usage = 2;

#ifdef _WIN32
using native_char = wchar_t;

// One argument quoted the way CommandLineToArgvW and the C runtime split a command line back up:
// backslashes are literal except before a quote, where each is doubled and the quote escaped.
std::wstring quote_argument(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return arg;
  }
  std::wstring out = L"\"";
  for (std::size_t i = 0;; ++i) {
    std::size_t backslashes = 0;
    while (i < arg.size() && arg[i] == L'\\') {
      ++i;
      ++backslashes;
    }
    if (i == arg.size()) {
      out.append(backslashes * 2, L'\\');
      break;
    }
    if (arg[i] == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
    } else {
      out.append(backslashes, L'\\');
    }
    out.push_back(arg[i]);
  }
  out.push_back(L'"');
  return out;
}

// Runs argv[0..argc): false when it could not be started, else true with its exit code in `code`
// and its peak private commit in `peak_bytes`. The code is the process's own 32 bits, so a
// compiler that exits -1 or crashes is never mistaken for one that did not start.
bool run_child(int argc, native_char** argv, int& code, unsigned long long& peak_bytes) {
  std::wstring command_line;
  for (int i = 0; i < argc; ++i) {
    if (i > 0) {
      command_line.push_back(L' ');
    }
    command_line += quote_argument(argv[i]);
  }
  // The compiler writes to this process's standard handles, named outright and inherited, so its
  // diagnostics reach the pipe the build reads whatever kind of handle that is.
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION process{};
  // The child inherits this process's error mode, which main() sets so that a crash in the
  // compiler is an exit code in the build's log and never a dialog waiting for a click.
  if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                      &startup, &process)) {
    return false;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  code = static_cast<int>(exit_code);
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(process.hProcess, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                           sizeof(counters))) {
    peak_bytes = counters.PeakPagefileUsage;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

std::FILE* open_for_write(const native_char* path) {
  std::FILE* file = nullptr;
  if (_wfopen_s(&file, path, L"wb") != 0) {
    return nullptr;
  }
  return file;
}

void print_path(const native_char* path) { std::fwprintf(stderr, L"%ls", path); }
#else
using native_char = char;

bool run_child(int argc, native_char** argv, int& code, unsigned long long& peak_bytes) {
  (void)argc;
  pid_t pid = 0;
  if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv, environ) != 0) {
    return false;
  }
  int status = 0;
  rusage usage{};
  while (wait4(pid, &status, 0, &usage) < 0) {
    if (errno != EINTR) {
      code = 1;
      return true;
    }
  }
  // ru_maxrss is in kibibytes on Linux.
  peak_bytes = static_cast<unsigned long long>(usage.ru_maxrss) * 1024ull;
  if (WIFEXITED(status)) {
    code = WEXITSTATUS(status);
  } else {
    code = WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
  }
  return true;
}

std::FILE* open_for_write(const native_char* path) { return std::fopen(path, "wb"); }

void print_path(const native_char* path) { std::fprintf(stderr, "%s", path); }
#endif

int meter(int argc, native_char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: shader-meter <cost file> <program> [arguments...]\n");
    return k_usage;
  }
  const auto start = std::chrono::steady_clock::now();
  unsigned long long peak_bytes = 0;
  int code = 1;
  const bool started = run_child(argc - 2, argv + 2, code, peak_bytes);
  const auto stop = std::chrono::steady_clock::now();
  if (!started) {
    std::fprintf(stderr, "shader-meter: could not start ");
    print_path(argv[2]);
    std::fprintf(stderr, "\n");
    return k_cannot_start;
  }
  if (code != 0) {
    return code;
  }
  const double seconds = std::chrono::duration<double>(stop - start).count();
  const unsigned long long peak_mb = (peak_bytes + (1ull << 19)) >> 20;
  std::FILE* file = open_for_write(argv[1]);
  if (file == nullptr) {
    // The compile succeeded and its output is what the build wanted; a missing measurement is
    // the check's to report (build.shader_pool), not a reason to fail this step.
    std::fprintf(stderr, "shader-meter: could not write ");
    print_path(argv[1]);
    std::fprintf(stderr, "\n");
    return 0;
  }
  std::fprintf(file, "peak_mb %llu\nseconds %.1f\n", peak_mb, seconds);
  std::fclose(file);
  return 0;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  return meter(argc, argv);
}
#else
int main(int argc, char** argv) { return meter(argc, argv); }
#endif

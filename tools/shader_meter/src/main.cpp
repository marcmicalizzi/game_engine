// shader-meter: runs one command (the build's slangc) and writes what it cost beside its output, so
// the build records every shader's peak memory and seconds for nothing (roadmap T118;
// docs/ci/what-to-run.md, "Heavy shaders share a pool"; cmake/EngineShaders.cmake). Given a store,
// it is also the shader compile cache (roadmap T76; docs/ci/what-to-run.md, "The shader cache";
// shader_cache.h), since it is already the one thing every compile goes through.
//
//   shader-meter [--cache <store> --compiler <identity> --source <file.slang> --output <file.spv>]
//                <cost file> <program> [arguments...]
//   shader-meter --sha256 <file>           prints the file's SHA-256 (the self-test's)
//   shader-meter --parse-depfile <file>    prints each file a depfile lists, one a line (the same)
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
// With --cache and ENGINE_SHADER_CACHE not `0`: a compile the store holds is served from it (the
// SPIR-V, the stored cost lines and `cache hit`, and what slangc printed, on stderr) without
// running the program; any other runs with `-depfile <temporary>` added, its output captured and
// passed on, and when it succeeds it is stored and the cost file's third line says `cache stored`
// or `cache not stored: <why>`. ENGINE_SHADER_CACHE_MAX_MB is the store's limit (4096).
//
// Standard library and the operating system only, like schemac: a host tool (ADR-0034) that
// builds before any engine module, links none, and is compiled for the build machine.

#include "sha256.h"
#include "shader_cache.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

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
#include <fcntl.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace fs = std::filesystem;

namespace {

constexpr int k_cannot_start = 127;
constexpr int k_usage = 2;
constexpr unsigned long long k_default_max_mb = 4096;

#ifdef _WIN32
using native_string = std::wstring;

std::string to_utf8(const std::wstring& w) {
  if (w.empty()) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                    nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr,
                      nullptr);
  return out;
}

native_string native_of(const char* ascii) {
  native_string out;
  for (const char* c = ascii; *c != '\0'; ++c) {
    out.push_back(static_cast<wchar_t>(*c));
  }
  return out;
}

std::string env(const wchar_t* name) {
  wchar_t buffer[64];
  const DWORD n = GetEnvironmentVariableW(name, buffer, 64);
  if (n == 0 || n >= 64) {
    return {};
  }
  return to_utf8(std::wstring(buffer, n));
}
#define ENGINE_ENV(name) env(L##name)

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

// Runs argv: false when it could not be started, else true with its exit code in `code` and its
// peak private commit in `peak_bytes`. The code is the process's own 32 bits, so a compiler that
// exits -1 or crashes is never mistaken for one that did not start. With `capture`, its standard
// output and error both go to that file instead of this process's.
bool run_child(const std::vector<native_string>& argv, const fs::path* capture, int& code,
               unsigned long long& peak_bytes) {
  std::wstring command_line;
  for (std::size_t i = 0; i < argv.size(); ++i) {
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
  HANDLE captured = INVALID_HANDLE_VALUE;
  if (capture != nullptr) {
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    captured = CreateFileW(capture->c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (captured == INVALID_HANDLE_VALUE) {
      return false;
    }
    startup.hStdOutput = captured;
    startup.hStdError = captured;
  }
  PROCESS_INFORMATION process{};
  // The child inherits this process's error mode, which main() sets so that a crash in the
  // compiler is an exit code in the build's log and never a dialog waiting for a click.
  const BOOL created = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, 0,
                                      nullptr, nullptr, &startup, &process);
  if (captured != INVALID_HANDLE_VALUE) {
    CloseHandle(captured);
  }
  if (!created) {
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

// Bytes to this process's standard error as they are, without the C runtime's text translation.
void write_stderr(const std::string& bytes) {
  HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  std::size_t done = 0;
  while (done < bytes.size()) {
    DWORD wrote = 0;
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1u << 20));
    if (!WriteFile(err, bytes.data() + done, want, &wrote, nullptr) || wrote == 0) {
      return;
    }
    done += wrote;
  }
}
#else
using native_string = std::string;

std::string to_utf8(const std::string& s) { return s; }

native_string native_of(const char* ascii) { return ascii; }

std::string env(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
}
#define ENGINE_ENV(name) env(name)

bool run_child(const std::vector<native_string>& argv, const fs::path* capture, int& code,
               unsigned long long& peak_bytes) {
  std::vector<char*> args;
  for (const native_string& a : argv) {
    args.push_back(const_cast<char*>(a.c_str()));
  }
  args.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (capture != nullptr) {
    posix_spawn_file_actions_addopen(&actions, 1, capture->c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                     0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
  }
  pid_t pid = 0;
  const int spawned = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0) {
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
  // A program posix_spawnp could not exec is a child that exits 127 on some C libraries.
  return true;
}

void write_stderr(const std::string& bytes) {
  std::fwrite(bytes.data(), 1, bytes.size(), stderr);
  std::fflush(stderr);
}
#endif

bool write_cost(const fs::path& path, unsigned long long peak_bytes, double seconds,
                const std::string& extra) {
  const unsigned long long peak_mb = (peak_bytes + (1ull << 19)) >> 20;
  char text[96];
  std::snprintf(text, sizeof(text), "peak_mb %llu\nseconds %.1f\n", peak_mb, seconds);
  std::FILE* file = nullptr;
#ifdef _WIN32
  if (_wfopen_s(&file, path.c_str(), L"wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.c_str(), "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  std::fputs(text, file);
  if (!extra.empty()) {
    std::fputs(extra.c_str(), file);
    std::fputs("\n", file);
  }
  std::fclose(file);
  return true;
}

// Runs the compile and measures it. `capture`: its output goes there (the cache passes it on).
int run_metered(const std::vector<native_string>& child, const fs::path& cost,
                const fs::path* capture, double& seconds, unsigned long long& peak_bytes,
                bool write_measurement) {
  const auto start = std::chrono::steady_clock::now();
  int code = 1;
  const bool started = run_child(child, capture, code, peak_bytes);
  const auto stop = std::chrono::steady_clock::now();
  if (!started) {
    std::fprintf(stderr, "shader-meter: could not start %s\n", to_utf8(child[0]).c_str());
    return k_cannot_start;
  }
  if (code != 0) {
    return code;
  }
  seconds = std::chrono::duration<double>(stop - start).count();
  if (write_measurement && !write_cost(cost, peak_bytes, seconds, {})) {
    // The compile succeeded and its output is what the build wanted; a missing measurement is
    // the check's to report (build.shader_pool), not a reason to fail this step.
    std::fprintf(stderr, "shader-meter: could not write %s\n", to_utf8(cost.native()).c_str());
  }
  return 0;
}

int meter(const std::vector<native_string>& all) {
  std::size_t i = 0;
  shader_meter::CacheRequest request;
  while (i < all.size() && all[i].size() > 2 && all[i][0] == '-' && all[i][1] == '-') {
    const std::string option = to_utf8(all[i]);
    if (i + 1 >= all.size()) {
      std::fprintf(stderr, "shader-meter: %s needs a value\n", option.c_str());
      return k_usage;
    }
    const native_string& value = all[i + 1];
    if (option == "--sha256") {
      std::string data;
      if (!shader_meter::read_whole_file(fs::path(value), data)) {
        return 1;
      }
      std::printf("%s\n", shader_meter::sha256_hex(data).c_str());
      return 0;
    }
    if (option == "--parse-depfile") {
      std::string data;
      if (!shader_meter::read_whole_file(fs::path(value), data)) {
        return 1;
      }
      for (const std::string& f : shader_meter::parse_depfile(data)) {
        std::printf("%s\n", f.c_str());
      }
      return 0;
    }
    if (option == "--cache") {
      request.store = fs::path(value);
    } else if (option == "--compiler") {
      request.compiler = to_utf8(value);
    } else if (option == "--source") {
      request.source = fs::absolute(fs::path(value)).lexically_normal();
    } else if (option == "--output") {
      request.output = fs::absolute(fs::path(value)).lexically_normal();
    } else {
      std::fprintf(stderr, "shader-meter: unknown option %s\n", option.c_str());
      return k_usage;
    }
    i += 2;
  }
  if (all.size() - i < 2) {
    std::fprintf(
        stderr,
        "usage: shader-meter [--cache <store> --compiler <id> --source <file> --output <file>] "
        "<cost file> <program> [arguments...]\n");
    return k_usage;
  }
  const fs::path cost(all[i]);
  const std::vector<native_string> child(all.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                                         all.end());
  double seconds = 0.0;
  unsigned long long peak_bytes = 0;

  const bool wanted = !request.store.empty() && ENGINE_ENV("ENGINE_SHADER_CACHE") != "0" &&
                      !request.compiler.empty() && !request.source.empty() &&
                      !request.output.empty();
  std::string key;
  if (wanted) {
    request.cost = fs::absolute(cost).lexically_normal();
    for (std::size_t a = 1; a < child.size(); ++a) {
      request.args.push_back(to_utf8(child[a]));
    }
    request.max_bytes = k_default_max_mb << 20;
    const std::string max_mb = ENGINE_ENV("ENGINE_SHADER_CACHE_MAX_MB");
    if (!max_mb.empty()) {
      request.max_bytes = std::strtoull(max_mb.c_str(), nullptr, 10) << 20;
    }
    key = shader_meter::cache_manifest_key(request);
  }
  if (key.empty()) {
    // No store, switched off, or a source that cannot be read: the compile as it always ran.
    return run_metered(child, cost, nullptr, seconds, peak_bytes, true);
  }

  std::string log;
  if (shader_meter::cache_lookup(request, key, log)) {
    write_stderr(log);
    return 0;
  }

  const fs::path depfile = shader_meter::cache_temp_path(request.store, ".d");
  const fs::path capture = shader_meter::cache_temp_path(request.store, ".out");
  std::vector<native_string> compile = child;
  compile.push_back(native_of("-depfile"));
  compile.push_back(depfile.native());
  const auto started = fs::file_time_type::clock::now();
  const int code = run_metered(compile, cost, &capture, seconds, peak_bytes, false);
  std::string printed;
  shader_meter::read_whole_file(capture, printed);
  write_stderr(printed);
  std::error_code ec;
  fs::remove(capture, ec);
  if (code != 0) {
    fs::remove(depfile, ec);
    return code;
  }
  if (!write_cost(cost, peak_bytes, seconds, {})) {
    std::fprintf(stderr, "shader-meter: could not write %s\n", to_utf8(cost.native()).c_str());
    fs::remove(depfile, ec);
    return 0;
  }
  const std::string outcome = shader_meter::cache_store(request, key, depfile, printed, started);
  fs::remove(depfile, ec);
  write_cost(cost, peak_bytes, seconds, outcome);
  return 0;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  std::vector<native_string> all;
  for (int i = 1; i < argc; ++i) {
    all.emplace_back(argv[i]);
  }
  return meter(all);
}
#else
int main(int argc, char** argv) {
  std::vector<native_string> all;
  for (int i = 1; i < argc; ++i) {
    all.emplace_back(argv[i]);
  }
  return meter(all);
}
#endif

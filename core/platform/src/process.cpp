#include <core/containers/vector.h>
#include <core/platform/process.h>

#include <cstring>

#if ENGINE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace engine::platform {

namespace {

void set_error(std::string* error, std::string_view what, std::string_view detail) {
  if (error == nullptr) return;
  error->assign(what);
  if (!detail.empty()) {
    error->append(": ");
    error->append(detail);
  }
}

#if ENGINE_PLATFORM_WINDOWS

std::wstring to_wide(std::string_view s) {
  if (s.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<usize>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string from_wide(std::wstring_view w) {
  if (w.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
  std::string s(static_cast<usize>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
  return s;
}

// Quoting that CommandLineToArgvW (and the C runtime) parse back to the same argument.
void append_quoted(std::wstring& cmd, std::wstring_view arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    cmd.append(arg);
    return;
  }
  cmd.push_back(L'"');
  usize backslashes = 0;
  for (const wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') {
      cmd.append(backslashes * 2 + 1, L'\\');
      cmd.push_back(L'"');
      backslashes = 0;
      continue;
    }
    cmd.append(backslashes, L'\\');
    backslashes = 0;
    cmd.push_back(c);
  }
  cmd.append(backslashes * 2, L'\\');
  cmd.push_back(L'"');
}

std::string last_error_text() {
  const DWORD code = ::GetLastError();
  char buf[512] = {};
  const DWORD n = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, buf, sizeof(buf) - 1, nullptr);
  std::string text = "error " + std::to_string(code);
  if (n > 0) {
    text.append(": ");
    std::string_view message(buf, n);
    while (!message.empty() &&
           (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
      message.remove_suffix(1);
    }
    text.append(message);
  }
  return text;
}

#endif

}  // namespace

Process::~Process() {
  if (spawned_ && !exited_) {
    close_stdin();
    (void)wait();
  }
  close_handles();
}

// ---- Windows -----------------------------------------------------------------------------------

#if ENGINE_PLATFORM_WINDOWS

bool Process::spawn(std::span<const std::string_view> argv, std::string* error, bool merge_stderr) {
  if (spawned_) {
    set_error(error, "Process::spawn", "already spawned");
    return false;
  }
  if (argv.empty()) {
    set_error(error, "Process::spawn", "argv is empty");
    return false;
  }
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE in_read = nullptr;
  HANDLE in_write = nullptr;
  HANDLE out_read = nullptr;
  HANDLE out_write = nullptr;
  if (!::CreatePipe(&in_read, &in_write, &sa, 0) || !::CreatePipe(&out_read, &out_write, &sa, 0)) {
    set_error(error, "CreatePipe", last_error_text());
    if (in_read) ::CloseHandle(in_read);
    if (in_write) ::CloseHandle(in_write);
    return false;
  }
  ::SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
  ::SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

  std::wstring command;
  for (usize i = 0; i < argv.size(); ++i) {
    if (i > 0) command.push_back(L' ');
    append_quoted(command, to_wide(argv[i]));
  }

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in_read;
  si.hStdOutput = out_write;
  si.hStdError = merge_stderr ? out_write : ::GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi{};
  const BOOL ok = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                   nullptr, &si, &pi);
  ::CloseHandle(in_read);
  ::CloseHandle(out_write);
  if (!ok) {
    set_error(error, "CreateProcess", last_error_text());
    ::CloseHandle(in_write);
    ::CloseHandle(out_read);
    return false;
  }
  ::CloseHandle(pi.hThread);
  process_ = pi.hProcess;
  stdin_write_ = in_write;
  stdout_read_ = out_read;
  spawned_ = true;
  return true;
}

bool Process::write(std::string_view data) noexcept {
  if (stdin_write_ == nullptr) return false;
  while (!data.empty()) {
    DWORD written = 0;
    const DWORD chunk = static_cast<DWORD>(data.size() > 0x7fffffff ? 0x7fffffff : data.size());
    if (!::WriteFile(static_cast<HANDLE>(stdin_write_), data.data(), chunk, &written, nullptr)) {
      return false;
    }
    data.remove_prefix(written);
  }
  return true;
}

void Process::close_stdin() noexcept {
  if (stdin_write_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(stdin_write_));
    stdin_write_ = nullptr;
  }
}

bool Process::fill_pending() {
  if (eof_ || stdout_read_ == nullptr) return false;
  char buf[4096];
  DWORD n = 0;
  if (!::ReadFile(static_cast<HANDLE>(stdout_read_), buf, sizeof(buf), &n, nullptr) || n == 0) {
    eof_ = true;
    return false;
  }
  pending_.append(buf, n);
  return true;
}

i32 Process::wait() noexcept {
  if (!spawned_) return -1;
  if (!exited_) {
    ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
    DWORD code = 0;
    exit_code_ =
        ::GetExitCodeProcess(static_cast<HANDLE>(process_), &code) ? static_cast<i32>(code) : -1;
    exited_ = true;
  }
  return exit_code_;
}

void Process::kill() noexcept {
  if (spawned_ && !exited_) ::TerminateProcess(static_cast<HANDLE>(process_), 1);
}

void Process::close_handles() noexcept {
  close_stdin();
  if (stdout_read_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(stdout_read_));
    stdout_read_ = nullptr;
  }
  if (process_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
  }
}

std::string executable_directory() {
  wchar_t buf[32768];
  const DWORD n =
      ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
  std::string path = from_wide(std::wstring_view(buf, n));
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  const usize slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// ---- POSIX -------------------------------------------------------------------------------------

#else

bool Process::spawn(std::span<const std::string_view> argv, std::string* error, bool merge_stderr) {
  if (spawned_) {
    set_error(error, "Process::spawn", "already spawned");
    return false;
  }
  if (argv.empty()) {
    set_error(error, "Process::spawn", "argv is empty");
    return false;
  }
  int in[2] = {-1, -1};
  int out[2] = {-1, -1};
  if (::pipe(in) != 0 || ::pipe(out) != 0) {
    set_error(error, "pipe", std::strerror(errno));
    if (in[0] >= 0) ::close(in[0]);
    if (in[1] >= 0) ::close(in[1]);
    return false;
  }

  Vector<std::string> storage;
  storage.reserve(static_cast<u32>(argv.size()));
  for (const std::string_view a : argv)
    storage.push_back(std::string(a));
  Vector<char*> args;
  args.reserve(static_cast<u32>(argv.size() + 1));
  for (std::string& s : storage)
    args.push_back(s.data());
  args.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  ::posix_spawn_file_actions_init(&actions);
  ::posix_spawn_file_actions_adddup2(&actions, in[0], 0);
  ::posix_spawn_file_actions_adddup2(&actions, out[1], 1);
  if (merge_stderr) ::posix_spawn_file_actions_adddup2(&actions, out[1], 2);
  ::posix_spawn_file_actions_addclose(&actions, in[0]);
  ::posix_spawn_file_actions_addclose(&actions, in[1]);
  ::posix_spawn_file_actions_addclose(&actions, out[0]);
  ::posix_spawn_file_actions_addclose(&actions, out[1]);
  pid_t pid = -1;
  const int rc = ::posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
  ::posix_spawn_file_actions_destroy(&actions);
  ::close(in[0]);
  ::close(out[1]);
  if (rc != 0) {
    set_error(error, "posix_spawnp", std::strerror(rc));
    ::close(in[1]);
    ::close(out[0]);
    return false;
  }
  pid_ = pid;
  stdin_write_ = in[1];
  stdout_read_ = out[0];
  spawned_ = true;
  return true;
}

bool Process::write(std::string_view data) noexcept {
  if (stdin_write_ < 0) return false;
  while (!data.empty()) {
    const ssize_t n = ::write(stdin_write_, data.data(), data.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    data.remove_prefix(static_cast<usize>(n));
  }
  return true;
}

void Process::close_stdin() noexcept {
  if (stdin_write_ >= 0) {
    ::close(stdin_write_);
    stdin_write_ = -1;
  }
}

bool Process::fill_pending() {
  if (eof_ || stdout_read_ < 0) return false;
  char buf[4096];
  for (;;) {
    const ssize_t n = ::read(stdout_read_, buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      eof_ = true;
      return false;
    }
    pending_.append(buf, static_cast<usize>(n));
    return true;
  }
}

i32 Process::wait() noexcept {
  if (!spawned_) return -1;
  if (!exited_) {
    int status = 0;
    while (::waitpid(static_cast<pid_t>(pid_), &status, 0) < 0 && errno == EINTR) {
    }
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    exited_ = true;
  }
  return exit_code_;
}

void Process::kill() noexcept {
  if (spawned_ && !exited_) ::kill(static_cast<pid_t>(pid_), SIGKILL);
}

void Process::close_handles() noexcept {
  close_stdin();
  if (stdout_read_ >= 0) {
    ::close(stdout_read_);
    stdout_read_ = -1;
  }
}

std::string executable_directory() {
  char buf[4096];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  std::string path(buf, static_cast<usize>(n));
  const usize slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

#endif

// ---- shared ------------------------------------------------------------------------------------

bool Process::read_line(std::string& line) {
  line.clear();
  for (;;) {
    const usize nl = pending_.find('\n');
    if (nl != std::string::npos) {
      line.assign(pending_, 0, nl);
      pending_.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return true;
    }
    if (!fill_pending()) {
      if (pending_.empty()) return false;
      line.swap(pending_);
      pending_.clear();
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return true;
    }
  }
}

bool Process::read_all(std::string& out) {
  while (fill_pending()) {
  }
  out.swap(pending_);
  pending_.clear();
  return true;
}

}  // namespace engine::platform

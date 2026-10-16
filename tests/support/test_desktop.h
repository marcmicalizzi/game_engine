#pragma once

// A private desktop for the windows an end-to-end test's child processes open (Windows; roadmap
// A40, docs/subsystems/apps.md "engine-editor", window.md "What the child's messages say").
//
// A test that starts engine-editor used to put its window on the owner's desktop, where what the
// window's input did depended on what the desktop was doing: which window was in front, where the
// owner's cursor was, whether the session was locked, whose other test windows were open beside it.
// `private_desktop()` makes one desktop per test process in the process's window station
// (`CreateDesktopW`, named after the process id so that two copies of a test never share one), and
// `run_on_desktop` starts a child there, so its windows are on nobody's screen and nothing the
// owner does reaches them. The desktop is never switched to and is closed when the process exits.
// Its users: engine-editor's end-to-end tests (`run_editor`) and engine-view's
// (apps/engine_view/tests/view_process.h, every program those tests start).
//
// What a desktop that is not the input desktop is, measured on 2026-10-14 (window.md has the
// list): Vulkan presents and WebView2 makes its window there; GetForegroundWindow() is NULL, so no
// window there is ever in front and SDL gives none of them its keyboard focus; GetCursorPos fails
// (error 5), SetCursorPos refuses, and GetAsyncKeyState reads every button up. The cases that
// drive input through the platform's queue are written for exactly that.
//
// Windows only: everything below compiles to nothing elsewhere, and a caller falls back to
// platform::Process there or when `valid()` is false (a window station that refuses a desktop).
// Header-only, like the rest of tests/support.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <windows.h>

namespace engine::test {

class PrivateDesktop {
 public:
  PrivateDesktop() {
    wchar_t station[256] = {};
    DWORD needed = 0;
    if (!::GetUserObjectInformationW(::GetProcessWindowStation(), UOI_NAME, station,
                                     sizeof(station), &needed)) {
      error_ =
          "GetUserObjectInformationW(window station) failed: " + std::to_string(::GetLastError());
      return;
    }
    const std::wstring name = L"engine-test-" + std::to_wstring(::GetCurrentProcessId());
    // Everything but DESKTOP_SWITCHDESKTOP: this desktop is never shown.
    const ACCESS_MASK access = DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU |
                               DESKTOP_HOOKCONTROL | DESKTOP_JOURNALRECORD |
                               DESKTOP_JOURNALPLAYBACK | DESKTOP_ENUMERATE | DESKTOP_WRITEOBJECTS |
                               STANDARD_RIGHTS_REQUIRED;
    desktop_ = ::CreateDesktopW(name.c_str(), nullptr, nullptr, 0, access, nullptr);
    if (desktop_ == nullptr) {
      error_ = "CreateDesktopW failed: " + std::to_string(::GetLastError());
      return;
    }
    path_ = std::wstring(station) + L"\\" + name;
    for (const wchar_t c : path_)  // ASCII: a station's name and ours
      name_.push_back(static_cast<char>(c));
  }
  ~PrivateDesktop() {
    if (desktop_ != nullptr) (void)::CloseDesktop(desktop_);
  }
  PrivateDesktop(const PrivateDesktop&) = delete;
  PrivateDesktop& operator=(const PrivateDesktop&) = delete;

  bool valid() const noexcept { return desktop_ != nullptr; }
  // "WinSta0\engine-test-<pid>", as a child's STARTUPINFOW::lpDesktop names it.
  const std::string& name() const noexcept { return name_; }
  const std::wstring& path() const noexcept { return path_; }
  // Why there is none, when `valid()` is false.
  const std::string& error() const noexcept { return error_; }

 private:
  HDESK desktop_ = nullptr;
  std::wstring path_;
  std::string name_;
  std::string error_;
};

// This test process's desktop, made the first time it is asked for and closed at exit.
inline const PrivateDesktop& private_desktop() {
  static const PrivateDesktop desktop;
  return desktop;
}

namespace detail {

inline std::wstring widen(std::string_view s) {
  if (s.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

// Quoting that CommandLineToArgvW and the C runtime parse back to the same argument (the same rule
// as platform::Process).
inline void append_quoted(std::wstring& cmd, std::wstring_view arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    cmd.append(arg);
    return;
  }
  cmd.push_back(L'"');
  size_t backslashes = 0;
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

}  // namespace detail

// Runs `argv` on `desktop` with stdin at its end and stdout and stderr merged into `output`, waits
// for it and returns its exit code in `code`; false, with `error`, when it could not be started.
// A child that gave up waiting for the GPU lock (75, with the lock's switch on) ends this process
// the same way, as platform::Process::wait does, so CTest reports the test as skipped.
inline bool run_on_desktop(const PrivateDesktop& desktop, std::span<const std::string_view> argv,
                           std::string& output, int& code, std::string& error) {
  output.clear();
  code = -1;
  if (!desktop.valid() || argv.empty()) {
    error = desktop.valid() ? "argv is empty" : desktop.error();
    return false;
  }
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE in_read = nullptr;
  HANDLE in_write = nullptr;
  HANDLE out_read = nullptr;
  HANDLE out_write = nullptr;
  if (!::CreatePipe(&in_read, &in_write, &sa, 0)) {
    error = "CreatePipe failed: " + std::to_string(::GetLastError());
    return false;
  }
  if (!::CreatePipe(&out_read, &out_write, &sa, 0)) {
    error = "CreatePipe failed: " + std::to_string(::GetLastError());
    ::CloseHandle(in_read);
    ::CloseHandle(in_write);
    return false;
  }
  ::SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
  ::SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
  std::wstring command;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (i > 0) command.push_back(L' ');
    detail::append_quoted(command, detail::widen(argv[i]));
  }
  std::wstring where = desktop.path();
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.lpDesktop = where.data();
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in_read;
  si.hStdOutput = out_write;
  si.hStdError = out_write;
  PROCESS_INFORMATION pi{};
  const BOOL ok = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                   nullptr, &si, &pi);
  const DWORD started = ok ? 0 : ::GetLastError();
  ::CloseHandle(in_read);
  ::CloseHandle(out_write);
  ::CloseHandle(in_write);  // the child's stdin is at its end at once
  if (!ok) {
    ::CloseHandle(out_read);
    error = "CreateProcessW on " + desktop.name() + " failed: " + std::to_string(started);
    return false;
  }
  ::CloseHandle(pi.hThread);
  char buf[4096];
  DWORD n = 0;
  while (::ReadFile(out_read, buf, sizeof(buf), &n, nullptr) && n > 0)
    output.append(buf, n);
  ::CloseHandle(out_read);
  ::WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 0;
  (void)::GetExitCodeProcess(pi.hProcess, &exit_code);
  ::CloseHandle(pi.hProcess);
  code = static_cast<int>(exit_code);
  if (code == 75) {
    char* on = nullptr;
    size_t size = 0;
    const bool lock_on = _dupenv_s(&on, &size, "ENGINE_GPU_LOCK_ON_DEVICE") == 0 && on != nullptr &&
                         std::strcmp(on, "1") == 0;
    std::free(on);
    if (lock_on) {
      std::fputs(
          "gpu-lock: a process this one started gave up waiting for the GPU lock (exit 75); this "
          "one ends the same way\n",
          stderr);
      std::fflush(nullptr);
      std::_Exit(75);
    }
  }
  return true;
}

}  // namespace engine::test

#endif  // _WIN32

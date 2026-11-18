#include "exclusive_file.h"

#include <core/base/macros.h>
#include <core/time/time.h>

#include <chrono>
#include <thread>

#if ENGINE_PLATFORM_WINDOWS
// The one thing here the standard library has no word for: a lock between processes that the
// system lets go when its holder dies.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace engine::protocol::detail {

namespace {

constexpr auto k_retry = std::chrono::milliseconds(2);

bool waited_out(i64 start_ns, i64 wait_ms) {
  return (time::monotonic_ns() - start_ns) / 1'000'000 >= wait_ms;
}

#if ENGINE_PLATFORM_WINDOWS

std::wstring widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int n =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<usize>(n > 0 ? n : 0), L'\0');
  if (n > 0)
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

#endif

}  // namespace

ExclusiveRun run_exclusive(const std::string& lock_path, i64 wait_ms, bool (*body)(void*),
                           void* context, bool& body_result, std::string& message) {
  body_result = false;
  const i64 start = time::monotonic_ns();
#if ENGINE_PLATFORM_WINDOWS
  const std::wstring wide = widen(lock_path);
  HANDLE handle = INVALID_HANDLE_VALUE;
  for (;;) {
    handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) break;
    // A scanner or an indexer can hold a new file for a moment; anything else is final.
    const DWORD e = GetLastError();
    if ((e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED) || waited_out(start, wait_ms)) {
      message = lock_path + ": cannot open the lock file (Windows error " + std::to_string(e) + ")";
      return ExclusiveRun::Failed;
    }
    std::this_thread::sleep_for(k_retry);
  }
  OVERLAPPED at = {};
  for (;;) {
    if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at))
      break;
    const DWORD e = GetLastError();
    if (e != ERROR_LOCK_VIOLATION && e != ERROR_IO_PENDING) {
      CloseHandle(handle);
      message = lock_path + ": cannot lock (Windows error " + std::to_string(e) + ")";
      return ExclusiveRun::Failed;
    }
    if (waited_out(start, wait_ms)) {
      CloseHandle(handle);
      return ExclusiveRun::Busy;
    }
    std::this_thread::sleep_for(k_retry);
  }
  body_result = body(context);
  OVERLAPPED from = {};
  (void)UnlockFileEx(handle, 0, 1, 0, &from);
  CloseHandle(handle);
  return ExclusiveRun::Ran;
#else
  int fd = -1;
  do {
    fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    message = lock_path + ": cannot open the lock file (" + std::strerror(errno) + ")";
    return ExclusiveRun::Failed;
  }
  for (;;) {
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) break;
    const int e = errno;
    if (e == EINTR) continue;
    if (e != EWOULDBLOCK) {  // flock's word for "held"; EAGAIN is the same number on Linux
      ::close(fd);
      message = lock_path + ": cannot lock (" + std::strerror(e) + ")";
      return ExclusiveRun::Failed;
    }
    if (waited_out(start, wait_ms)) {
      ::close(fd);
      return ExclusiveRun::Busy;
    }
    std::this_thread::sleep_for(k_retry);
  }
  body_result = body(context);
  (void)::flock(fd, LOCK_UN);
  ::close(fd);
  return ExclusiveRun::Ran;
#endif
}

}  // namespace engine::protocol::detail

// A directory-change notice for one file (dir_watch.h; protocol.md, "The ledger's events").
#include <core/log/log.h>
#include <domain/protocol/dir_watch.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

#if ENGINE_PLATFORM_WINDOWS
// The system's own notice is the point of this file: `ReadDirectoryChangesW` here, inotify below;
// the standard library has neither.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/inotify.h>
#endif
#endif

namespace engine::protocol {

namespace {
ENGINE_LOG_CATEGORY_DEFINE(log_watch, "protocol.watch");

// How often a watch with no notice from the system looks (protocol.md, "The ledger's events").
constexpr u32 k_poll_ms = 1000;

std::atomic<u32> g_delay_registration_ms{0};  // delay_registration_for_test
}  // namespace

void DirWatch::delay_registration_for_test(u32 ms) noexcept { g_delay_registration_ms.store(ms); }

struct DirWatch::Impl {
  std::string dir;
  std::string name;
  Notify notify = nullptr;
  void* context = nullptr;
  std::atomic<bool> polling{false};
  std::thread thread;
  // `start` waits for this: the system's watch is registered, or the watch fell back to looking.
  std::mutex ready_mutex;
  std::condition_variable ready_cv;
  bool ready = false;
  void set_ready() {
    {
      std::lock_guard<std::mutex> lock(ready_mutex);
      if (ready) return;
      ready = true;
    }
    ready_cv.notify_all();
  }
  void delay_for_test() {
    if (const u32 ms = g_delay_registration_ms.exchange(0); ms > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  }
#if ENGINE_PLATFORM_WINDOWS
  HANDLE stop = nullptr;  // a manual-reset event, set to stop
#else
  int stop_pipe[2] = {-1, -1};  // a byte written to [1] stops
#endif

  void tell() const { notify(context); }
  void begin_polling() {
    polling.store(true);
    ENGINE_LOG_WARN(log_watch, "no directory-change notice here: looking once a second instead",
                    log::field("file", std::string_view(name)));
    set_ready();
  }
  void run();
};

#if ENGINE_PLATFORM_WINDOWS

namespace {

std::wstring widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int n =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<usize>(n > 0 ? n : 0), L'\0');
  if (n > 0) {
    (void)MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(),
                              n);
  }
  return out;
}

// Whether a buffer of FILE_NOTIFY_INFORMATION entries names `name` (case folded, as the file
// system compares it).
bool names(const unsigned char* buffer, DWORD bytes, const std::wstring& name) {
  DWORD offset = 0;
  while (offset < bytes) {
    const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
    const int length = static_cast<int>(info->FileNameLength / sizeof(WCHAR));
    if (CompareStringOrdinal(info->FileName, length, name.c_str(), static_cast<int>(name.size()),
                             TRUE) == CSTR_EQUAL)
      return true;
    if (info->NextEntryOffset == 0) break;
    offset += info->NextEntryOffset;
  }
  return false;
}

}  // namespace

void DirWatch::Impl::run() {
  delay_for_test();
  const std::wstring wdir = widen(dir);
  const std::wstring wname = widen(name);
  HANDLE handle = CreateFileW(
      wdir.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  OVERLAPPED overlapped{};
  overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  bool stopped = false;
  if (handle != INVALID_HANDLE_VALUE && overlapped.hEvent != nullptr) {
    alignas(DWORD) unsigned char buffer[16 * 1024];
    for (;;) {
      ResetEvent(overlapped.hEvent);
      if (!ReadDirectoryChangesW(handle, buffer, sizeof(buffer), FALSE,
                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                     FILE_NOTIFY_CHANGE_LAST_WRITE,
                                 nullptr, &overlapped, nullptr))
        break;
      // The first read in flight is the registration: the system keeps what changes from here on
      // for this handle, between reads too.
      set_ready();
      HANDLE waits[2] = {stop, overlapped.hEvent};
      const DWORD which = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
      if (which != WAIT_OBJECT_0 + 1) {
        // Stopped (or the wait itself failed): the read in flight is cancelled and waited for,
        // since the system writes into `buffer` until it completes.
        CancelIoEx(handle, &overlapped);
        DWORD ignored = 0;
        (void)GetOverlappedResult(handle, &overlapped, &ignored, TRUE);
        stopped = which == WAIT_OBJECT_0;
        break;
      }
      DWORD bytes = 0;
      if (!GetOverlappedResult(handle, &overlapped, &bytes, FALSE)) {
        // The system dropped what it had: whoever is told looks at the file whatever it holds.
        if (GetLastError() == ERROR_NOTIFY_ENUM_DIR) {
          tell();
          continue;
        }
        break;
      }
      if (bytes == 0 || names(buffer, bytes, wname)) tell();  // 0: the buffer overflowed
    }
  }
  if (overlapped.hEvent != nullptr) CloseHandle(overlapped.hEvent);
  if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  if (stopped) return;  // only after the registration: `start` waited for it
  begin_polling();
  while (WaitForSingleObject(stop, k_poll_ms) == WAIT_TIMEOUT)
    tell();
}

#else

void DirWatch::Impl::run() {
  delay_for_test();
#if defined(__linux__)
  const int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (fd >= 0 && inotify_add_watch(fd, dir.c_str(),
                                   IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE |
                                       IN_DELETE | IN_MODIFY) >= 0) {
    set_ready();  // registered: the system queues what changes from here on
    alignas(inotify_event) char buffer[16 * 1024];
    for (;;) {
      pollfd fds[2] = {{stop_pipe[0], POLLIN, 0}, {fd, POLLIN, 0}};
      const int n = ::poll(fds, 2, -1);
      if (n < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (fds[0].revents != 0) {
        ::close(fd);
        return;
      }
      if (fds[1].revents == 0) continue;
      bool hit = false;
      for (;;) {
        const ssize_t got = ::read(fd, buffer, sizeof(buffer));
        if (got <= 0) break;  // EAGAIN: drained
        const char* p = buffer;
        const char* end = buffer + got;
        while (p < end) {
          const auto* e = reinterpret_cast<const inotify_event*>(p);
          // IN_Q_OVERFLOW: the system dropped what it had, so look whatever the file holds.
          if ((e->mask & IN_Q_OVERFLOW) != 0 || (e->len > 0 && name == e->name)) hit = true;
          p += sizeof(inotify_event) + e->len;
        }
      }
      if (hit) tell();
    }
  }
  if (fd >= 0) ::close(fd);
#endif
  begin_polling();
  for (;;) {
    pollfd fds[1] = {{stop_pipe[0], POLLIN, 0}};
    const int n = ::poll(fds, 1, static_cast<int>(k_poll_ms));
    if (n > 0) return;
    if (n == 0) {
      tell();
    } else if (errno != EINTR) {
      return;
    }
  }
}

#endif

DirWatch::DirWatch() noexcept = default;

DirWatch::~DirWatch() { stop(); }

void DirWatch::start(std::string native_dir, std::string name, Notify notify, void* context) {
  stop();
  auto impl = std::make_unique<Impl>();
  impl->dir = std::move(native_dir);
  impl->name = std::move(name);
  impl->notify = notify;
  impl->context = context;
#if ENGINE_PLATFORM_WINDOWS
  impl->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (impl->stop == nullptr) return;  // no event to stop a thread by: no watch
#else
  if (::pipe(impl->stop_pipe) != 0) return;
  (void)::fcntl(impl->stop_pipe[0], F_SETFD, FD_CLOEXEC);
  (void)::fcntl(impl->stop_pipe[1], F_SETFD, FD_CLOEXEC);
#endif
  Impl* raw = impl.get();
  impl->thread = std::thread([raw] { raw->run(); });
  // Returns once the system's watch is registered (or the watch fell back to looking), so a
  // caller that reads the file after this sees every later write raise a notice (A55).
  {
    std::unique_lock<std::mutex> lock(raw->ready_mutex);
    raw->ready_cv.wait(lock, [raw] { return raw->ready; });
  }
  impl_ = std::move(impl);
}

void DirWatch::stop() noexcept {
  if (impl_ == nullptr) return;
#if ENGINE_PLATFORM_WINDOWS
  SetEvent(impl_->stop);
#else
  const char byte = 1;
  (void)!::write(impl_->stop_pipe[1], &byte, 1);
#endif
  if (impl_->thread.joinable()) impl_->thread.join();
#if ENGINE_PLATFORM_WINDOWS
  CloseHandle(impl_->stop);
#else
  ::close(impl_->stop_pipe[0]);
  ::close(impl_->stop_pipe[1]);
#endif
  impl_.reset();
}

bool DirWatch::running() const noexcept { return impl_ != nullptr; }

bool DirWatch::polling() const noexcept { return impl_ != nullptr && impl_->polling.load(); }

}  // namespace engine::protocol

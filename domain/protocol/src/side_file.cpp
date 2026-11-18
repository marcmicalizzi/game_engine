// The side files' lock, ids and reads (side_file.h; protocol.md, "Two hosts, one side file").
#include <core/platform/random.h>
#include <core/platform/thread.h>
#include <core/time/time.h>
#include <domain/doc/document_store.h>
#include <domain/protocol/side_file.h>

#include <cstdint>
#include <filesystem>
#include <span>

#if ENGINE_PLATFORM_WINDOWS
// The operating system's own file lock is the point of this file: `LockFileEx` here, `flock`
// below; the standard library has neither.
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

namespace engine::protocol {

namespace {

std::filesystem::path native_of(std::string_view utf8) {
  return std::filesystem::path(
      std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

// A wait that starts short, since nearly every holder lets go within a millisecond or two, and
// grows to a few looks a frame for a holder that is saving a large document.
u32 next_pause(u32 pause_ms) noexcept { return pause_ms >= 16 ? 16 : pause_ms * 2; }

}  // namespace

bool SideFileLock::acquire(std::string_view native_path, i64 timeout_ms, std::string& message) {
  release();
  std::string lock_path(native_path);
  lock_path += ".lock";
  const std::filesystem::path path = native_of(lock_path);
  const i64 deadline = time::monotonic_ns() + (timeout_ms > 0 ? timeout_ms : 0) * 1'000'000;
  u32 pause_ms = 1;
#if ENGINE_PLATFORM_WINDOWS
  for (;;) {
    // Every sharing mode, so that a reader of the lock file, a second holder's open and a scanner
    // all open it; the lock is the byte range, not the handle's exclusivity.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD failure = 0;
    if (h == INVALID_HANDLE_VALUE) {
      failure = GetLastError();
      if (failure != ERROR_SHARING_VIOLATION && failure != ERROR_ACCESS_DENIED) {
        message = lock_path + ": cannot be opened (Windows error " + std::to_string(failure) + ")";
        return false;
      }
    } else {
      OVERLAPPED at = {};
      if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at)) {
        handle_ = h;
        return true;
      }
      failure = GetLastError();
      CloseHandle(h);
      if (failure != ERROR_LOCK_VIOLATION && failure != ERROR_IO_PENDING) {
        message = lock_path + ": cannot be locked (Windows error " + std::to_string(failure) + ")";
        return false;
      }
    }
    if (time::monotonic_ns() >= deadline) break;
    platform::sleep_ms(pause_ms);
    pause_ms = next_pause(pause_ms);
  }
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    message = lock_path + ": cannot be opened (" + std::strerror(errno) + ")";
    return false;
  }
  // `flock` locks belong to the open file description, so two opens in one process exclude each
  // other as two processes do (a POSIX record lock would not: it is the process's).
  for (;;) {
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
      fd_ = fd;
      return true;
    }
    if (errno != EWOULDBLOCK && errno != EINTR) {
      message = lock_path + ": cannot be locked (" + std::strerror(errno) + ")";
      ::close(fd);
      return false;
    }
    if (time::monotonic_ns() >= deadline) break;
    platform::sleep_ms(pause_ms);
    pause_ms = next_pause(pause_ms);
  }
  ::close(fd);
#endif
  message = lock_path + ": another host process has held it for " + std::to_string(timeout_ms) +
            " ms; try again, and if it stays held, find the host that has stopped";
  return false;
}

void SideFileLock::release() noexcept {
#if ENGINE_PLATFORM_WINDOWS
  if (handle_ == nullptr) return;
  OVERLAPPED at = {};
  (void)UnlockFileEx(static_cast<HANDLE>(handle_), 0, 1, 0, &at);
  CloseHandle(static_cast<HANDLE>(handle_));
  handle_ = nullptr;
#else
  if (fd_ < 0) return;
  (void)::flock(fd_, LOCK_UN);
  ::close(fd_);
  fd_ = -1;
#endif
}

bool SideFileLock::held() const noexcept {
#if ENGINE_PLATFORM_WINDOWS
  return handle_ != nullptr;
#else
  return fd_ >= 0;
#endif
}

bool lock_side_file(const io::Vfs& vfs, std::string_view path, SideFileLock& lock,
                    RpcError& error) {
  std::string native;
  const io::Status s = vfs.resolve(path, native, /*for_write=*/true);
  if (s != io::Status::Ok) {
    error = make_error(codes::k_io_error, std::string(path) + ": " + io::status_name(s));
    return false;
  }
  // The lock file is beside the side file, in the document's directory, which a document that
  // exists always has.
  if (const io::Status made = io::make_directories(io::parent_path(native));
      made != io::Status::Ok) {
    error = make_error(codes::k_io_error, std::string(path) + ": " + io::status_name(made));
    return false;
  }
  std::string message;
  if (!lock.acquire(native, k_side_file_lock_timeout_ms, message)) {
    error = make_error(codes::k_io_error, std::move(message));
    return false;
  }
  return true;
}

u64 fresh_side_file_id(bool (*taken)(const void* context, u64 id), const void* context) {
  constexpr u64 k_range = u64{1} << 53;
  for (;;) {
    u8 bytes[8] = {};
    u64 id = 0;
    if (platform::secure_random(std::span<u8>(bytes, sizeof(bytes)))) {
      for (const u8 b : bytes)
        id = (id << 8) | b;
    } else {
      // No secure source: the clock and the process's address space are still different in every
      // process, which is all an id here needs.
      id = (static_cast<u64>(time::monotonic_ns()) * 0x9E3779B97F4A7C15ull) ^
           static_cast<u64>(time::wall_unix_ms()) ^
           static_cast<u64>(reinterpret_cast<std::uintptr_t>(&id));
    }
    id %= k_range;
    if (id == 0 || (taken != nullptr && taken(context, id))) continue;
    return id;
  }
}

namespace detail {

bool read_side_json(const io::Vfs& vfs, std::string_view path, JsonValue& value, bool& present,
                    RpcError& error) {
  value = JsonValue();
  std::string text;
  const io::Status s = vfs.read(path, text);
  present = s == io::Status::Ok;
  if (s == io::Status::NotFound) return true;
  if (s != io::Status::Ok) {
    error = make_error(codes::k_io_error, std::string(path) + ": " + io::status_name(s));
    return false;
  }
  if (const JsonParseResult r = parse_json(text, value); !r.ok) {
    error = make_error(codes::k_io_error, std::string(path) + ": " + r.message);
    return false;
  }
  return true;
}

bool write_side_json(const io::Vfs& vfs, std::string_view path, const JsonValue& value,
                     RpcError& error) {
  std::string text = write_json(value);
  text.push_back('\n');
  std::string message;
  if (!doc::DocumentStore::write_side_file(vfs, path, text, &message)) {
    error = make_error(codes::k_io_error, std::move(message));
    return false;
  }
  return true;
}

bool map_side_json(const schema::TypeInfo& type, void* object, void (*reset)(void* object),
                   const JsonValue& value, std::string_view path, RpcError& error) {
  std::string first;
  for (u16 version = type.version; version >= 1; --version) {
    reset(object);
    schema::ReadContext ctx;
    const bool read = version == type.version
                          ? schema::from_json(type, object, value, ctx)
                          : schema::from_json_versioned(type, object, value, version, ctx);
    if (read) return true;
    if (version == type.version) {
      first = "invalid";
      if (!ctx.diagnostics.empty()) first += ": " + ctx.diagnostics[0].message;
    }
  }
  reset(object);
  error = make_error(codes::k_io_error, std::string(path) + ": " + first);
  return false;
}

}  // namespace detail

}  // namespace engine::protocol

#include <core/base/assert.h>
#include <core/time/time.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <thread>

#if ENGINE_PLATFORM_WINDOWS
#include <process.h>
#else
#include <unistd.h>
#endif

namespace engine::io {

namespace fs = std::filesystem;

namespace {

fs::path to_path(std::string_view utf8) {
  return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string from_path(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

Status status_from(const std::error_code& ec) noexcept {
  if (!ec) return Status::Ok;
  const std::error_condition cond = ec.default_error_condition();
  if (cond == std::errc::no_such_file_or_directory) return Status::NotFound;
  if (cond == std::errc::permission_denied || cond == std::errc::operation_not_permitted) {
    return Status::PermissionDenied;
  }
  if (cond == std::errc::is_a_directory) return Status::IsDirectory;
  if (cond == std::errc::not_a_directory) return Status::NotDirectory;
  if (cond == std::errc::file_exists) return Status::AlreadyExists;
  if (cond == std::errc::invalid_argument) return Status::InvalidPath;
  return Status::IoError;
}

Status status_from_errno(int err) noexcept {
  switch (err) {
    case ENOENT: return Status::NotFound;
    case EACCES: return Status::PermissionDenied;
    case EISDIR: return Status::IsDirectory;
    case ENOTDIR: return Status::NotDirectory;
    case EEXIST: return Status::AlreadyExists;
    case EINVAL: return Status::InvalidPath;
    default: return Status::IoError;
  }
}

std::FILE* open_file(const fs::path& p, const char* mode) {
  std::FILE* f = nullptr;
#if ENGINE_PLATFORM_WINDOWS
  wchar_t wmode[8] = {};
  for (usize i = 0; mode[i] != '\0' && i < 7; ++i)
    wmode[i] = static_cast<wchar_t>(mode[i]);
  (void)_wfopen_s(&f, p.c_str(), wmode);
#else
  f = std::fopen(p.c_str(), mode);
#endif
  return f;
}

Status read_into(const fs::path& p, std::string& out) {
  std::error_code ec;
  if (fs::is_directory(p, ec)) return Status::IsDirectory;
  std::FILE* f = open_file(p, "rb");
  if (f == nullptr) return status_from_errno(errno);
  out.clear();
  const u64 size = fs::file_size(p, ec);
  if (!ec && size < (u64{1} << 40)) out.reserve(static_cast<usize>(size));
  char buf[64 * 1024];
  for (;;) {
    const usize n = std::fread(buf, 1, sizeof(buf), f);
    if (n > 0) out.append(buf, n);
    if (n < sizeof(buf)) break;
  }
  const bool failed = std::ferror(f) != 0;
  std::fclose(f);
  return failed ? Status::IoError : Status::Ok;
}

Status write_to(const fs::path& p, std::string_view data) {
  std::FILE* f = open_file(p, "wb");
  if (f == nullptr) return status_from_errno(errno);
  bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
  ok = std::fflush(f) == 0 && ok;
  ok = std::fclose(f) == 0 && ok;
  return ok ? Status::Ok : Status::IoError;
}

std::atomic<u32> g_temp_counter{0};

u32 process_id() noexcept {
#if ENGINE_PLATFORM_WINDOWS
  return static_cast<u32>(::_getpid());
#else
  return static_cast<u32>(::getpid());
#endif
}

bool valid_scheme(std::string_view scheme) noexcept {
  if (scheme.empty()) return false;
  for (const char c : scheme) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return true;
}

}  // namespace

const char* status_name(Status status) noexcept {
  switch (status) {
    case Status::Ok: return "ok";
    case Status::NotFound: return "not found";
    case Status::PermissionDenied: return "permission denied";
    case Status::InvalidPath: return "invalid path";
    case Status::UnknownMount: return "unknown mount";
    case Status::ReadOnly: return "read-only mount";
    case Status::IsDirectory: return "is a directory";
    case Status::NotDirectory: return "not a directory";
    case Status::AlreadyExists: return "already exists";
    case Status::IoError: return "i/o error";
    case Status::InvalidArgument: return "invalid argument";
  }
  return "?";
}

// ---- path helpers ----------------------------------------------------------------------------

bool is_absolute_path(std::string_view path) noexcept {
  if (path.empty()) return false;
  if (path[0] == '/' || path[0] == '\\') return true;
  const bool drive = path.size() >= 2 && path[1] == ':' &&
                     ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'));
  return drive;
}

std::string normalize_path(std::string_view path) {
  std::string out;
  usize i = 0;
  // Drive prefix.
  if (path.size() >= 2 && path[1] == ':' &&
      ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))) {
    out.push_back(path[0]);
    out.push_back(':');
    i = 2;
  }
  const bool rooted = i < path.size() && (path[i] == '/' || path[i] == '\\');
  if (rooted) out.push_back('/');

  Vector<std::string_view> parts;
  usize start = i;
  for (usize k = i; k <= path.size(); ++k) {
    if (k == path.size() || path[k] == '/' || path[k] == '\\') {
      const std::string_view part = path.substr(start, k - start);
      start = k + 1;
      if (part.empty() || part == ".") continue;
      if (part == "..") {
        if (!parts.empty() && parts.back() != "..") {
          parts.pop_back();
        } else if (!rooted) {
          parts.push_back(part);
        }
        continue;
      }
      parts.push_back(part);
    }
  }
  for (u32 k = 0; k < parts.size(); ++k) {
    if (k > 0) out.push_back('/');
    out.append(parts[k]);
  }
  if (out.empty()) return ".";
  if (out.size() == 2 && out[1] == ':') out.push_back('/');  // "C:" -> "C:/"
  return out;
}

std::string join_path(std::string_view base, std::string_view relative) {
  if (relative.empty()) return normalize_path(base);
  if (is_absolute_path(relative) || base.empty()) return normalize_path(relative);
  std::string joined(base);
  joined.push_back('/');
  joined.append(relative);
  return normalize_path(joined);
}

std::string_view parent_path(std::string_view path) noexcept {
  const usize slash = path.find_last_of("/\\");
  if (slash == std::string_view::npos) return {};
  if (slash == 0) return path.substr(0, 1);
  return path.substr(0, slash);
}

std::string_view file_name(std::string_view path) noexcept {
  const usize slash = path.find_last_of("/\\");
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string_view extension(std::string_view path) noexcept {
  const std::string_view name = file_name(path);
  const usize dot = name.find_last_of('.');
  if (dot == std::string_view::npos || dot == 0) return {};
  return name.substr(dot);
}

// ---- native operations -----------------------------------------------------------------------

Status read_file(std::string_view native_path, std::string& out) {
  return read_into(to_path(native_path), out);
}

Status write_file(std::string_view native_path, std::string_view data) {
  return write_to(to_path(native_path), data);
}

Status write_file_atomic(std::string_view native_path, std::string_view data) {
  const fs::path target = to_path(native_path);
  std::string temp_name(native_path);
  temp_name.push_back('.');
  temp_name.append(std::to_string(process_id()));
  temp_name.push_back('.');
  temp_name.append(std::to_string(g_temp_counter.fetch_add(1, std::memory_order_relaxed)));
  temp_name.append(".tmp");
  const fs::path temp = to_path(temp_name);
  const Status written = write_to(temp, data);
  if (written != Status::Ok) {
    std::error_code ignored;
    fs::remove(temp, ignored);
    return written;
  }
  // Replacing a file that another process wrote moments ago can fail transiently on Windows
  // (an indexer or scanner still holds it); retry briefly before giving up.
  std::error_code ec;
  for (int attempt = 0;; ++attempt) {
    fs::rename(temp, target, ec);
    if (!ec) return Status::Ok;
    const Status s = status_from(ec);
    if ((s != Status::PermissionDenied && s != Status::IoError) || attempt >= 40) {
      std::error_code ignored;
      fs::remove(temp, ignored);
      return s;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

Status append_file(std::string_view native_path, std::string_view data) {
  std::FILE* f = open_file(to_path(native_path), "ab");
  if (f == nullptr) return status_from_errno(errno);
  bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
  ok = std::fflush(f) == 0 && ok;
  ok = std::fclose(f) == 0 && ok;
  return ok ? Status::Ok : Status::IoError;
}

Status stat_file(std::string_view native_path, FileInfo& out) {
  std::error_code ec;
  const fs::path p = to_path(native_path);
  const fs::file_status st = fs::status(p, ec);
  if (ec) return status_from(ec);
  if (!fs::exists(st)) return Status::NotFound;
  out.is_directory = fs::is_directory(st);
  out.size = out.is_directory ? 0 : fs::file_size(p, ec);
  if (ec) return status_from(ec);
  const fs::file_time_type mtime = fs::last_write_time(p, ec);
  if (ec) return status_from(ec);
  // file_time_type's epoch is unspecified; go through system_clock.
  const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(mtime);
  out.modified_unix_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count();
  return Status::Ok;
}

bool exists(std::string_view native_path) noexcept {
  std::error_code ec;
  return fs::exists(to_path(native_path), ec) && !ec;
}

Status list_directory(std::string_view native_path, Vector<DirEntry>& out) {
  out.clear();
  std::error_code ec;
  const fs::path p = to_path(native_path);
  const fs::file_status st = fs::status(p, ec);
  if (ec) return status_from(ec);
  if (!fs::exists(st)) return Status::NotFound;
  if (!fs::is_directory(st)) return Status::NotDirectory;
  for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
    DirEntry e;
    e.name = from_path(it->path().filename());
    e.is_directory = it->is_directory(ec);
    out.push_back(std::move(e));
  }
  if (ec) return status_from(ec);
  std::sort(out.begin(), out.end(),
            [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
  return Status::Ok;
}

Status make_directories(std::string_view native_path) {
  std::error_code ec;
  fs::create_directories(to_path(native_path), ec);
  if (ec) return status_from(ec);
  return Status::Ok;
}

Status remove_file(std::string_view native_path) {
  std::error_code ec;
  const fs::path p = to_path(native_path);
  if (fs::is_directory(p, ec)) return Status::IsDirectory;
  const bool removed = fs::remove(p, ec);
  if (ec) return status_from(ec);
  return removed ? Status::Ok : Status::NotFound;
}

Status remove_directory_recursive(std::string_view native_path) {
  std::error_code ec;
  const fs::path p = to_path(native_path);
  if (!fs::exists(p, ec)) return Status::NotFound;
  if (!fs::is_directory(p, ec)) return Status::NotDirectory;
  fs::remove_all(p, ec);
  return status_from(ec);
}

Status rename_path(std::string_view from, std::string_view to) {
  std::error_code ec;
  fs::rename(to_path(from), to_path(to), ec);
  return status_from(ec);
}

// ---- Vfs -------------------------------------------------------------------------------------

Status Vfs::mount(std::string_view scheme, std::string_view native_root, bool writable) {
  if (!valid_scheme(scheme)) return Status::InvalidPath;
  if (find_mount(scheme) != nullptr) return Status::AlreadyExists;
  Mount m;
  m.scheme = std::string(scheme);
  m.root = normalize_path(native_root);
  m.writable = writable;
  mounts_.push_back(std::move(m));
  return Status::Ok;
}

bool Vfs::unmount(std::string_view scheme) noexcept {
  for (u32 i = 0; i < mounts_.size(); ++i) {
    if (mounts_[i].scheme == scheme) {
      mounts_.erase_at(i);
      return true;
    }
  }
  return false;
}

const Mount* Vfs::find_mount(std::string_view scheme) const noexcept {
  for (const Mount& m : mounts_) {
    if (m.scheme == scheme) return &m;
  }
  return nullptr;
}

Status Vfs::resolve(std::string_view path, std::string& native_out, bool for_write) const {
  const usize sep = path.find("://");
  if (sep == std::string_view::npos) {
    if (path.empty()) return Status::InvalidPath;
    native_out = normalize_path(path);
    return Status::Ok;
  }
  const std::string_view scheme = path.substr(0, sep);
  const std::string_view rel = path.substr(sep + 3);
  const Mount* m = find_mount(scheme);
  if (m == nullptr) return Status::UnknownMount;
  if (for_write && !m->writable) return Status::ReadOnly;
  if (is_absolute_path(rel)) return Status::InvalidPath;
  const std::string normalized = normalize_path(rel);
  if (normalized == ".." || normalized.starts_with("../") ||
      normalized.find(':') != std::string::npos) {
    return Status::InvalidPath;
  }
  native_out = normalized == "." ? m->root : join_path(m->root, normalized);
  return Status::Ok;
}

Status Vfs::read(std::string_view path, std::string& out) const {
  std::string native;
  const Status s = resolve(path, native);
  return s != Status::Ok ? s : read_file(native, out);
}

Status Vfs::write(std::string_view path, std::string_view data) const {
  std::string native;
  const Status s = resolve(path, native, /*for_write=*/true);
  if (s != Status::Ok) return s;
  const std::string_view parent = parent_path(native);
  if (!parent.empty()) {
    const Status made = io::make_directories(parent);
    if (made != Status::Ok) return made;
  }
  return write_file_atomic(native, data);
}

Status Vfs::append(std::string_view path, std::string_view data) const {
  std::string native;
  const Status s = resolve(path, native, /*for_write=*/true);
  if (s != Status::Ok) return s;
  const std::string_view parent = parent_path(native);
  if (!parent.empty()) {
    const Status made = io::make_directories(parent);
    if (made != Status::Ok) return made;
  }
  return append_file(native, data);
}

Status Vfs::stat(std::string_view path, FileInfo& out) const {
  std::string native;
  const Status s = resolve(path, native);
  return s != Status::Ok ? s : stat_file(native, out);
}

bool Vfs::exists(std::string_view path) const noexcept {
  std::string native;
  return resolve(path, native) == Status::Ok && io::exists(native);
}

Status Vfs::list(std::string_view path, Vector<DirEntry>& out) const {
  std::string native;
  const Status s = resolve(path, native);
  return s != Status::Ok ? s : list_directory(native, out);
}

Status Vfs::make_directories(std::string_view path) const {
  std::string native;
  const Status s = resolve(path, native, /*for_write=*/true);
  return s != Status::Ok ? s : io::make_directories(native);
}

Status Vfs::remove(std::string_view path) const {
  std::string native;
  const Status s = resolve(path, native, /*for_write=*/true);
  return s != Status::Ok ? s : remove_file(native);
}

// ---- async -----------------------------------------------------------------------------------

void read_async(jobs::JobSystem& jobs, AsyncRead& request) {
  request.counter.add(1);
  jobs::Job job;
  job.fn = [](void* p) {
    auto* r = static_cast<AsyncRead*>(p);
    r->status = read_file(r->path, r->data);
  };
  job.data = &request;
  job.counter = &request.counter;
  jobs.schedule(jobs::Pool::Efficiency, job);
}

bool wait_all(jobs::JobSystem& jobs, std::span<AsyncRead> requests) {
  bool ok = true;
  for (AsyncRead& r : requests) {
    jobs.wait(r.counter);
    ok = ok && r.status == Status::Ok;
  }
  return ok;
}

}  // namespace engine::io

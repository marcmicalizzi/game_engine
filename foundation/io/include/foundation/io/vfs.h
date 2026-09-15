#pragma once

// File access (docs/plan/02-architecture.md §2.3, foundation/io).
//
// Two layers. Free functions operate on native paths with a Status result instead of
// exceptions and use UTF-8 throughout (converted for the OS on Windows). `Vfs` adds a mount
// table so engine code addresses files as "content://levels/a.json" or "cache://ddc/..."
// without knowing where a project keeps them; every mount is a native directory root with a
// writable flag, and resolution refuses paths that escape their root. `read_async` runs reads
// on the job system's Efficiency pool for streaming and tooling.
//
// Not here (yet): memory mapping, file watching, the content-addressed blob store (domain/ddc),
// and OS asynchronous I/O (io_uring, overlapped) behind the same AsyncRead shape.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/jobs/job_system.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::io {

enum class Status : u8 {
  Ok,
  NotFound,
  PermissionDenied,
  InvalidPath,
  UnknownMount,
  ReadOnly,
  IsDirectory,
  NotDirectory,
  AlreadyExists,
  IoError,
};
const char* status_name(Status status) noexcept;

struct FileInfo {
  u64 size = 0;
  i64 modified_unix_ms = 0;
  bool is_directory = false;
};

struct DirEntry {
  std::string name;  // file name only
  bool is_directory = false;
};

// ---- path helpers (pure string operations; forward slashes on every platform) --------------

// Forward slashes, no trailing slash, "." components dropped, ".." collapsed where a previous
// component exists (leading ".." kept). A drive prefix ("C:") or a leading "/" is preserved.
// The empty path normalizes to ".".
std::string normalize_path(std::string_view path);
std::string join_path(std::string_view base, std::string_view relative);
std::string_view parent_path(std::string_view path) noexcept;
std::string_view file_name(std::string_view path) noexcept;
// ".json" for "a/b.json", "" when there is none.
std::string_view extension(std::string_view path) noexcept;
bool is_absolute_path(std::string_view path) noexcept;

// ---- native-path operations ----------------------------------------------------------------

Status read_file(std::string_view native_path, std::string& out);
// Writes straight to the file (partial contents on failure).
Status write_file(std::string_view native_path, std::string_view data);
// Writes a sibling temporary file, flushes, then renames over the target, so readers see the
// old or the new contents and never a mix. Does not create parent directories.
Status write_file_atomic(std::string_view native_path, std::string_view data);
// Appends to the file, creating it when absent (journals and logs).
Status append_file(std::string_view native_path, std::string_view data);
Status stat_file(std::string_view native_path, FileInfo& out);
bool exists(std::string_view native_path) noexcept;
// Entries sorted by name; "." and ".." excluded.
Status list_directory(std::string_view native_path, Vector<DirEntry>& out);
Status make_directories(std::string_view native_path);
Status remove_file(std::string_view native_path);
// Removes a directory and everything under it.
Status remove_directory_recursive(std::string_view native_path);
Status rename_path(std::string_view from, std::string_view to);

// ---- mounts --------------------------------------------------------------------------------

struct Mount {
  std::string scheme;  // "content"
  std::string root;    // normalized native directory
  bool writable = false;
};

class Vfs {
 public:
  Vfs() = default;
  ENGINE_NON_COPYABLE(Vfs);

  // Scheme: [a-z0-9_]+, unique. The root need not exist yet for writable mounts.
  Status mount(std::string_view scheme, std::string_view native_root, bool writable = false);
  bool unmount(std::string_view scheme) noexcept;
  const Vector<Mount>& mounts() const noexcept { return mounts_; }
  const Mount* find_mount(std::string_view scheme) const noexcept;

  // "scheme://relative/path" -> native path under the mount root. Rejects unknown schemes,
  // absolute or root-escaping relative paths, and writes to read-only mounts. A path without
  // "://" is native and passes through normalized, so callers may accept either form.
  Status resolve(std::string_view path, std::string& native_out, bool for_write = false) const;

  Status read(std::string_view path, std::string& out) const;
  // Atomic; creates missing parent directories.
  Status write(std::string_view path, std::string_view data) const;
  // Appends; creates missing parent directories.
  Status append(std::string_view path, std::string_view data) const;
  Status stat(std::string_view path, FileInfo& out) const;
  bool exists(std::string_view path) const noexcept;
  Status list(std::string_view path, Vector<DirEntry>& out) const;
  Status make_directories(std::string_view path) const;
  Status remove(std::string_view path) const;

 private:
  Vector<Mount> mounts_;
};

// ---- asynchronous reads --------------------------------------------------------------------

// One read request. Fill `path` (native), schedule with read_async, then wait on `counter`
// through JobSystem::wait or poll done(). The request must outlive the job.
struct AsyncRead {
  std::string path;
  std::string data;
  Status status = Status::Ok;
  jobs::Counter counter;

  bool done() const noexcept { return counter.done(); }
};

// Schedules one read on the Efficiency pool. `request.counter` is add()ed here.
void read_async(jobs::JobSystem& jobs, AsyncRead& request);
// Waits for every request; returns true when all succeeded.
bool wait_all(jobs::JobSystem& jobs, std::span<AsyncRead> requests);

}  // namespace engine::io

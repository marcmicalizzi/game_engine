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
// `FileHandle` is the third layer and the one streaming needs: a file held open and read **by
// byte range**, so a consumer that wants a few kilobytes out of the middle of a ten-megabyte
// container pays for those kilobytes. The reads are positional — Windows `ReadFile` with an
// `OVERLAPPED` offset on a handle opened `FILE_FLAG_OVERLAPPED`, POSIX `pread` — so one handle
// serves any number of concurrent reads from any number of threads with no shared file pointer
// between them, which is exactly the shape `read_ranges_async` puts on the Efficiency pool.
//
// `read_queued` is the deep queue (roadmap W54): a group of ranged reads issued from the calling
// thread with up to `depth` in flight at once — overlapped reads on Windows — instead of one read a
// job, so a caller with four hundred small ranges keeps the device's queue full without a pool of
// threads to block in each read. A handle opened `ReadMode::unbuffered` bypasses the OS's cache.
//
// Not here (yet): memory mapping, file watching, the content-addressed blob store (domain/ddc),
// io_uring (POSIX `read_queued` reads in order, one at a time), and DirectStorage-style GPU
// decompression behind the same request shape.

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
  InvalidArgument,  // the caller passed inconsistent arguments (nothing was touched)
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

// A file's length when the caller does not know it (overwrite_file).
inline constexpr u64 k_unknown_file_size = ~u64{0};
// Writes `data` over the file in place: opened for writing alone and without truncating it,
// written from the start, cut to the new length, closed. Makes the file, and its missing parent
// directories, when it is not there, and retries briefly while something else holds it. **Not
// atomic**: a reader can see it half written, so a caller that needs all or nothing makes it so
// itself (domain/doc's save log). What it is for is the cost on Windows (docs/subsystems/io.md,
// "Writing a file"): 0.5 ms at any size where `write_file_atomic` costs 3 ms and truncating or
// opening to read as well costs 5-12 ms from 64 KiB. A caller that knows the file's current length
// passes it as `previous_size`: a file of 8 KiB or more whose length changes is then removed and
// made again, 1.2 ms, where changing its length in place costs 4.8 ms from 16 KiB.
Status overwrite_file(std::string_view native_path, std::string_view data,
                      u64 previous_size = k_unknown_file_size);
Status stat_file(std::string_view native_path, FileInfo& out);
bool exists(std::string_view native_path) noexcept;
// Entries sorted by name; "." and ".." excluded.
Status list_directory(std::string_view native_path, Vector<DirEntry>& out);
// Makes the directory and every missing parent. Ok when it is already a directory, which it asks
// first: one stat, where creating every prefix of an existing path cost 4.3 ms on Windows.
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

// ---- ranged reads ----------------------------------------------------------------------------

// A file held open and read by byte range.
//
// **Every read names its own offset**, so nothing about a read depends on what any other read
// did: `read_at` is `const` and safe to call concurrently on one handle from any number of
// threads. That is the property streaming is built on — a geometry page is three or four
// contiguous ranges of one `.clusters` container, several pages are in flight at once, and none
// of them may disturb another's position. On Windows the handle is opened `FILE_FLAG_OVERLAPPED`
// and each read carries its offset in an `OVERLAPPED` with its own event, because a *synchronous*
// handle serializes concurrent reads on one file object even when each names an offset; on POSIX
// it is `pread`, which has the same contract by definition.
//
// The handle is movable so it can live in a `Vector` beside the rest of a mesh's state, and
// closes itself. Nothing is buffered: a range read is one system call, which is what a caller
// reading 128 KB at a time wants and what a caller reading four bytes at a time must not do.

// How a handle reads. `buffered` goes through the OS's file cache, as every other read here does.
// `unbuffered` bypasses it (Windows `FILE_FLAG_NO_BUFFERING`, Linux `O_DIRECT`): every offset,
// length and destination address must then be a multiple of `k_unbuffered_alignment` or the read
// is refused (`InvalidArgument`), and a read past the end comes back short as a buffered one does.
// It is how a measurement reads a file cold without the rights to drop the cache, and how a large
// sequential read avoids copying through it (docs/subsystems/io.md, "Queued reads").
enum class ReadMode : u8 { buffered, unbuffered };
// The alignment an unbuffered read needs: a 4 KiB sector, which covers 512-byte-emulating and
// 4K-native drives alike.
inline constexpr u64 k_unbuffered_alignment = 4096;

class FileHandle {
 public:
  FileHandle() noexcept = default;
  ~FileHandle() { close(); }
  ENGINE_NON_COPYABLE(FileHandle);
  FileHandle(FileHandle&& other) noexcept;
  FileHandle& operator=(FileHandle&& other) noexcept;

  // Opens for reading. Replaces whatever the handle held. A file system that cannot read
  // unbuffered (a tmpfs, some overlay mounts) refuses `ReadMode::unbuffered` at open.
  Status open(std::string_view native_path, ReadMode mode = ReadMode::buffered);
  void close() noexcept;
  bool unbuffered() const noexcept { return unbuffered_; }
#if ENGINE_PLATFORM_WINDOWS
  bool valid() const noexcept { return handle_ != nullptr; }
  // The `HANDLE`, for `read_queued`'s overlapped reads; null when closed.
  void* native() const noexcept { return handle_; }
#else
  bool valid() const noexcept { return handle_ >= 0; }
#endif
  // The size the file had when it was opened, which is what `read_at` clamps against.
  u64 size() const noexcept { return size_; }
  const std::string& path() const noexcept { return path_; }

  // Reads up to `bytes` at `offset` into `dst`, and reports in `read_out` how many arrived.
  //
  // A read that runs past the end of the file is **short, not an error** — `read_out` says so and
  // a caller that needs exactly `bytes` compares the two — because "how long is this file" and
  // "read this range" are two questions and answering the second with the first's failure would
  // make every caller ask both. A zero-length read is `Ok`, reads nothing, and touches neither
  // `dst` nor the disk; an offset at or past the end is `Ok` with `read_out` zero. Only a real
  // I/O failure is a failure.
  Status read_at(u64 offset, void* dst, u64 bytes, u64& read_out) const noexcept;

 private:
#if ENGINE_PLATFORM_WINDOWS
  // A `HANDLE`, kept as `void*` so that <windows.h> stays out of every file that includes this
  // one. `INVALID_HANDLE_VALUE` is normalized to null on the way in, so "closed" is one value.
  void* handle_ = nullptr;
#else
  int handle_ = -1;
#endif
  u64 size_ = 0;
  bool unbuffered_ = false;
  std::string path_;
};

// One ranged read, the way `read_async` is one whole-file read. `file` and `dst` must outlive
// the job. `read` is what arrived, so `complete()` is the test a caller who wanted the whole
// range makes; a short read at the end of the file leaves `status` `Ok`.
struct AsyncRangeRead {
  const FileHandle* file = nullptr;
  u64 offset = 0;
  u64 bytes = 0;
  void* dst = nullptr;
  u64 read = 0;
  Status status = Status::Ok;

  bool complete() const noexcept { return status == Status::Ok && read == bytes; }
};

// Schedules the reads on the Efficiency pool against `counter`, which is add()ed here — one
// counter for a whole group, because the caller of a group (a page, a header) wants to know when
// *all* of it has landed and polling one word is cheaper than polling N. The requests are
// scheduled one at a time rather than as a span, so this allocates nothing and may be called
// every frame.
void read_ranges_async(jobs::JobSystem& jobs, std::span<AsyncRangeRead> requests,
                       jobs::Counter& counter);
void read_range_async(jobs::JobSystem& jobs, AsyncRangeRead& request, jobs::Counter& counter);

// ---- queued reads (roadmap W54) ---------------------------------------------------------------

// Called on the issuing thread as each request of `read_queued` completes, in the requests' order.
using ReadCompleted = void (*)(void* context, u32 index) noexcept;
inline constexpr u32 k_max_queue_depth = 256;

struct QueuedReadStats {
  u32 requests = 0;
  u32 most_in_flight = 0;  // the deepest the queue was: min(depth, requests) unless one failed
};

// **Reads every request with up to `depth` of them in flight at once**, issued from the calling
// thread, and returns when all have landed: true when every one is `complete()`. Each request
// names its own handle, offset, length and destination, as `read_range_async`'s do; a request with
// no handle, or a misaligned one on an unbuffered handle, fails alone (`InvalidArgument`) and the
// rest are still read.
//
// On Windows each read is an overlapped `ReadFile` with an event of its own (a pool kept per
// thread), so `depth` reads are queued at the device together and no thread blocks in one; on
// POSIX the reads are `pread` in order, one at a time (io_uring is the next step, W54). Requests
// **complete in order**: `on_completed(context, i)` runs after request i has landed and before
// request `i + depth` is issued, so a caller may reuse the destination of request i for request
// i + depth — a ring of `depth` buffers serves any number of requests. `depth` is clamped to 1 ..
// `k_max_queue_depth`. Allocates nothing once the thread's event pool is as deep as asked.
bool read_queued(std::span<AsyncRangeRead> requests, u32 depth,
                 ReadCompleted on_completed = nullptr, void* context = nullptr,
                 QueuedReadStats* stats = nullptr) noexcept;

// The synchronous form, for a caller with one range and no job system: opens, reads, closes.
// A caller with more than one range of the same file opens a `FileHandle` instead.
Status read_file_range(std::string_view native_path, u64 offset, void* dst, u64 bytes,
                       u64& read_out);

}  // namespace engine::io

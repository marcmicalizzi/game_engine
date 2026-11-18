#pragma once

// **A file mapped read-only** (docs/subsystems/io.md, "Mapped files"; roadmap T182): the whole
// file as one span of bytes in the process's address space, backed by the file itself.
//
// What it is for: a reader that serves small records out of a large file to several threads at
// once. A copy of a range (`FileHandle::read_at`) costs a system call per record, and a shared
// read-ahead buffer costs a lock that every reader waits behind; a mapped view costs neither,
// because the OS's file cache *is* the buffer — a record is a pointer into it, read by any number
// of threads with no lock, no copy and no call.
//
// What it costs, by the OS's own accounting:
//   - **Address space** the size of the file, reserved by the view. Not commit: a read-only view
//     of a file is backed by the file, so it charges nothing against the commit limit and the
//     page file never holds it.
//   - **Working set** only for the pages a reader touches, and those pages are clean and shared
//     with the file cache: under memory pressure the OS drops them without writing anything, and
//     a later touch reads them back (a soft fault while the cache still holds them, a hard fault
//     with the disk's latency once it does not). On a machine with less memory the view stays the
//     same size and the resident part shrinks to what is in use.
//
// The file's bytes are those it held when it was mapped as long as nobody writes the file in
// place: every writer in this tree replaces a file by renaming a new one over it
// (`write_file_atomic`), which leaves a mapped view reading the old bytes on POSIX and, on Windows,
// is what the case "a mapped file and a replacement" checks. A file truncated in place under a view
// faults the reader on POSIX (SIGBUS); Windows refuses the truncation while the view exists.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <foundation/io/vfs.h>

#include <span>
#include <string_view>

namespace engine::io {

class MappedFile {
 public:
  MappedFile() noexcept = default;
  ~MappedFile() { close(); }
  ENGINE_NON_COPYABLE(MappedFile);
  MappedFile(MappedFile&& other) noexcept;
  MappedFile& operator=(MappedFile&& other) noexcept;

  // Maps the whole file read-only. Replaces whatever was mapped. An empty file opens with an empty
  // span (there is nothing to map). `NotFound`, `PermissionDenied`, `IsDirectory` or `IoError`
  // otherwise, with nothing mapped.
  Status open(std::string_view native_path);
  void close() noexcept;
  bool is_open() const noexcept { return open_; }
  // The file's bytes as they were mapped; empty when closed. Safe to read from any thread.
  std::span<const u8> bytes() const noexcept { return {data_, size_}; }
  u64 size() const noexcept { return size_; }

 private:
  const u8* data_ = nullptr;
  u64 size_ = 0;
  bool open_ = false;
};

}  // namespace engine::io

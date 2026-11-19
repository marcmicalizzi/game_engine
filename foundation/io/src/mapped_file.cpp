// A file mapped read-only (mapped_file.h; docs/subsystems/io.md, "Mapped files").
#include <foundation/io/mapped_file.h>

#include <filesystem>
#include <string>
#include <utility>

#if ENGINE_PLATFORM_WINDOWS
// The view is `CreateFileMappingW` and `MapViewOfFile`, which the standard library has no
// spelling of, as the ranged reads in vfs.cpp are `ReadFile` with an offset.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace engine::io {

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      open_(std::exchange(other.open_, false)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  if (this != &other) {
    close();
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
    open_ = std::exchange(other.open_, false);
  }
  return *this;
}

void MappedFile::close() noexcept {
  if (data_ != nullptr) {
#if ENGINE_PLATFORM_WINDOWS
    ::UnmapViewOfFile(data_);
#else
    ::munmap(const_cast<u8*>(data_), static_cast<size_t>(size_));
#endif
  }
  data_ = nullptr;
  size_ = 0;
  open_ = false;
}

Status MappedFile::open(std::string_view native_path) {
  close();
#if ENGINE_PLATFORM_WINDOWS
  const std::filesystem::path p(
      std::u8string_view(reinterpret_cast<const char8_t*>(native_path.data()), native_path.size()));
  // Shared for reading, writing and deletion, as `FileHandle` opens a file: the view must not be
  // what stops a build replacing the file it maps.
  HANDLE file =
      ::CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD err = ::GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return Status::NotFound;
    if (err == ERROR_ACCESS_DENIED) {
      // A directory opened without FILE_FLAG_BACKUP_SEMANTICS is refused as access denied.
      std::error_code ec;
      return std::filesystem::is_directory(p, ec) ? Status::IsDirectory : Status::PermissionDenied;
    }
    if (err == ERROR_SHARING_VIOLATION) return Status::PermissionDenied;
    return Status::IoError;
  }
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(file, &size) == 0) {
    ::CloseHandle(file);
    return Status::IoError;
  }
  if (size.QuadPart == 0) {
    // A mapping of zero bytes is refused by the OS; an empty file is an empty span.
    ::CloseHandle(file);
    open_ = true;
    return Status::Ok;
  }
  HANDLE section = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  // The view holds the section, and the section the file: both handles go now.
  ::CloseHandle(file);
  if (section == nullptr) return Status::IoError;
  void* view = ::MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0);
  ::CloseHandle(section);
  if (view == nullptr) return Status::IoError;
  data_ = static_cast<const u8*>(view);
  size_ = static_cast<u64>(size.QuadPart);
#else
  const std::string native(native_path);
  const int fd = ::open(native.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    switch (errno) {
      case ENOENT: return Status::NotFound;
      case EACCES: return Status::PermissionDenied;
      case EISDIR: return Status::IsDirectory;
      case ENOTDIR: return Status::NotDirectory;
      default: return Status::IoError;
    }
  }
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return Status::IoError;
  }
  if (S_ISDIR(st.st_mode)) {
    ::close(fd);
    return Status::IsDirectory;
  }
  if (st.st_size == 0) {
    ::close(fd);
    open_ = true;
    return Status::Ok;
  }
  void* view = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
  // The mapping holds the file; the descriptor goes now.
  ::close(fd);
  if (view == MAP_FAILED) return Status::IoError;
  data_ = static_cast<const u8*>(view);
  size_ = static_cast<u64>(st.st_size);
#endif
  open_ = true;
  return Status::Ok;
}

}  // namespace engine::io

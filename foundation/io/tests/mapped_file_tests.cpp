// A file mapped read-only (mapped_file.h; docs/subsystems/io.md, "Mapped files").
#include <foundation/io/mapped_file.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

using namespace engine;
using namespace engine::io;

namespace {
std::string view_text(const MappedFile& m) {
  return std::string(reinterpret_cast<const char*>(m.bytes().data()), m.bytes().size());
}
}  // namespace

TEST_CASE("io: a mapped file is the file's bytes, and an empty one an empty span") {
  test::TempDir tmp("engine_io_mapped");
  const std::string path = tmp.file("a.bin");
  std::string data(70'000, '\0');
  for (u32 i = 0; i < data.size(); ++i)
    data[i] = static_cast<char>((i * 131u + 7u) & 0xFFu);
  REQUIRE(write_file(path, data) == Status::Ok);
  MappedFile m;
  REQUIRE(m.open(path) == Status::Ok);
  CHECK(m.is_open());
  REQUIRE(m.size() == data.size());
  CHECK(std::memcmp(m.bytes().data(), data.data(), data.size()) == 0);

  // Moved, the view goes with it; closed, it is empty.
  MappedFile moved = std::move(m);
  CHECK_FALSE(m.is_open());
  CHECK(m.bytes().empty());
  REQUIRE(moved.size() == data.size());
  CHECK(moved.bytes()[69'999] == static_cast<u8>(data[69'999]));
  moved.close();
  CHECK_FALSE(moved.is_open());
  CHECK(moved.size() == 0);

  const std::string empty = tmp.file("empty.bin");
  REQUIRE(write_file(empty, std::string()) == Status::Ok);
  MappedFile e;
  REQUIRE(e.open(empty) == Status::Ok);
  CHECK(e.is_open());
  CHECK(e.bytes().empty());
}

TEST_CASE("io: a mapped file refuses what is not there or not a file") {
  test::TempDir tmp("engine_io_mapped");
  MappedFile m;
  CHECK(m.open(tmp.file("missing.bin")) == Status::NotFound);
  CHECK_FALSE(m.is_open());
  REQUIRE(make_directories(tmp.file("dir")) == Status::Ok);
  CHECK(m.open(tmp.file("dir")) == Status::IsDirectory);
  CHECK_FALSE(m.is_open());
}

// **A mapped file and a replacement** (io.md, "Mapped files"): a build replaces a level's archive
// by renaming a new file over it (`write_file_atomic`) while a reader may hold the old one. The
// view must not stop the replacement, and keeps reading the bytes it mapped; a new view reads the
// new.
TEST_CASE("io: a mapped file and a replacement") {
  test::TempDir tmp("engine_io_mapped");
  const std::string path = tmp.file("level.bin");
  const std::string before(10'000, 'a');
  const std::string after(12'000, 'b');
  REQUIRE(write_file(path, before) == Status::Ok);
  MappedFile old;
  REQUIRE(old.open(path) == Status::Ok);
  REQUIRE(write_file_atomic(path, after) == Status::Ok);
  CHECK(view_text(old) == before);
  MappedFile fresh;
  REQUIRE(fresh.open(path) == Status::Ok);
  CHECK(view_text(fresh) == after);
  std::string read;
  REQUIRE(read_file(path, read) == Status::Ok);
  CHECK(read == after);
}

// **The view is the size read, and holds it** (T194, io.md, "Mapped files"): the size is read,
// then exactly that much is mapped, so a file a writer truncated between the two fails the map
// instead of leaving a view shorter than `size()`. That window is a race this case cannot open;
// what it holds is the rest: a view is the size the file had, a file grown under it does not grow
// the view, and on Windows the file cannot be cut below the view while it exists.
TEST_CASE("io hostile: a mapped view is exactly the size read, and a writer cannot cut it") {
  test::TempDir tmp("engine_io_mapped");
  const std::string path = tmp.file("level.bin");
  REQUIRE(write_file(path, std::string(9'000, 'a')) == Status::Ok);
  MappedFile m;
  REQUIRE(m.open(path) == Status::Ok);
  REQUIRE(m.size() == 9'000);
  REQUIRE(append_file(path, std::string(5'000, 'b')) == Status::Ok);
  CHECK(m.size() == 9'000);
  CHECK(m.bytes().size() == 9'000);
  CHECK(m.bytes()[8'999] == static_cast<u8>('a'));
#if ENGINE_PLATFORM_WINDOWS
  std::error_code ec;
  std::filesystem::resize_file(path, 100, ec);
  CHECK(ec);  // ERROR_USER_MAPPED_FILE: the section holds the file's length
  CHECK(m.bytes()[8'999] == static_cast<u8>('a'));
#endif
}

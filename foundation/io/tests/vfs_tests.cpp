#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>
#include <utility>

using namespace engine;
using namespace engine::io;

// Scratch space is `engine::test::TempDir`: one unguessable directory per object, so a second
// copy of this binary cannot delete this one's fixtures (tests/support/test_temp_dir.h).
using TempDir = test::TempDir;

TEST_CASE("io: path helpers normalize and decompose") {
  CHECK(normalize_path("a\\b/./c/../d") == "a/b/d");
  CHECK(normalize_path("/a//b/") == "/a/b");
  CHECK(normalize_path("../x") == "../x");
  CHECK(normalize_path("a/../..") == "..");
  CHECK(normalize_path("/../a") == "/a");
  CHECK(normalize_path("") == ".");
  CHECK(normalize_path("./") == ".");
  CHECK(normalize_path("C:\\work\\..\\repo") == "C:/repo");
  CHECK(normalize_path("C:") == "C:/");
  CHECK(join_path("a/b", "c") == "a/b/c");
  CHECK(join_path("a/b", "../c") == "a/c");
  CHECK(join_path("a/b", "/abs") == "/abs");
  CHECK(join_path("", "x") == "x");
  CHECK(parent_path("a/b/c.json") == "a/b");
  CHECK(parent_path("c.json").empty());
  CHECK(parent_path("/c") == "/");
  CHECK(file_name("a/b/c.json") == "c.json");
  CHECK(file_name("c.json") == "c.json");
  CHECK(extension("a/b.tar.gz") == ".gz");
  CHECK(extension("a/.hidden").empty());
  CHECK(extension("a/noext").empty());
  CHECK(is_absolute_path("/x"));
  CHECK(is_absolute_path("D:\\x"));
  CHECK_FALSE(is_absolute_path("x/y"));
  CHECK(std::string_view(status_name(Status::NotFound)) == "not found");
}

TEST_CASE("io: native read, write, stat, list, remove") {
  TempDir tmp("engine_io");
  const std::string file = join_path(tmp.path(), "sub/dir/hello.txt");
  std::string data;
  CHECK(read_file(file, data) == Status::NotFound);
  CHECK(make_directories(parent_path(file)) == Status::Ok);
  CHECK(write_file(file, "hello") == Status::Ok);
  CHECK(read_file(file, data) == Status::Ok);
  CHECK(data == "hello");
  CHECK(write_file_atomic(file, "world!") == Status::Ok);
  CHECK(read_file(file, data) == Status::Ok);
  CHECK(data == "world!");
  FileInfo info;
  CHECK(stat_file(file, info) == Status::Ok);
  CHECK(info.size == 6);
  CHECK_FALSE(info.is_directory);
  CHECK(info.modified_unix_ms > 1'600'000'000'000ll);
  CHECK(stat_file(parent_path(file), info) == Status::Ok);
  CHECK(info.is_directory);
  CHECK(stat_file(join_path(tmp.path(), "missing"), info) == Status::NotFound);
  CHECK(exists(file));
  CHECK_FALSE(exists(join_path(tmp.path(), "missing")));

  CHECK(write_file(join_path(tmp.path(), "sub/dir/b.txt"), "") == Status::Ok);
  Vector<DirEntry> entries;
  CHECK(list_directory(join_path(tmp.path(), "sub/dir"), entries) == Status::Ok);
  REQUIRE(entries.size() == 2);
  CHECK(entries[0].name == "b.txt");
  CHECK(entries[1].name == "hello.txt");
  CHECK(list_directory(join_path(tmp.path(), "sub"), entries) == Status::Ok);
  REQUIRE(entries.size() == 1);
  CHECK(entries[0].is_directory);
  CHECK(list_directory(file, entries) == Status::NotDirectory);
  CHECK(list_directory(join_path(tmp.path(), "nope"), entries) == Status::NotFound);

  CHECK(read_file(join_path(tmp.path(), "sub"), data) == Status::IsDirectory);
  CHECK(remove_file(join_path(tmp.path(), "sub")) == Status::IsDirectory);
  CHECK(remove_file(file) == Status::Ok);
  CHECK(remove_file(file) == Status::NotFound);
  CHECK(rename_path(join_path(tmp.path(), "sub/dir/b.txt"), join_path(tmp.path(), "sub/c.txt")) ==
        Status::Ok);
  CHECK(exists(join_path(tmp.path(), "sub/c.txt")));
  CHECK(remove_directory_recursive(join_path(tmp.path(), "sub")) == Status::Ok);
  CHECK_FALSE(exists(join_path(tmp.path(), "sub")));
  CHECK(remove_directory_recursive(join_path(tmp.path(), "sub")) == Status::NotFound);
  // No temporary files left behind by the atomic write.
  CHECK(list_directory(tmp.path(), entries) == Status::Ok);
  CHECK(entries.empty());
}

TEST_CASE("io: make_directories on a directory that is there, one that is not, and a file") {
  TempDir tmp("engine_io");
  const std::string deep = join_path(tmp.path(), "a/b/c");
  // Missing: every level made, as create_directories always did.
  CHECK(make_directories(deep) == Status::Ok);
  FileInfo info;
  REQUIRE(stat_file(deep, info) == Status::Ok);
  CHECK(info.is_directory);
  // There: Ok again, and nothing touched (the check that answers it without creating anything).
  CHECK(make_directories(deep) == Status::Ok);
  CHECK(make_directories(tmp.path()) == Status::Ok);
  // A file where a directory is asked for, at the leaf or on the way: refused as before, and the
  // file left alone.
  const std::string file = join_path(tmp.path(), "a/f.txt");
  REQUIRE(write_file(file, "x") == Status::Ok);
  CHECK(make_directories(file) != Status::Ok);
  CHECK(make_directories(join_path(file, "under")) != Status::Ok);
  std::string data;
  CHECK(read_file(file, data) == Status::Ok);
  CHECK(data == "x");
}

TEST_CASE("io: overwrite_file writes in place, cuts, grows, makes, and remakes when told") {
  TempDir tmp("engine_io");
  const std::string file = join_path(tmp.path(), "new/dir/file.json");
  std::string data;
  // Not there, nor its directory: made.
  CHECK(overwrite_file(file, "hello, world") == Status::Ok);
  CHECK(read_file(file, data) == Status::Ok);
  CHECK(data == "hello, world");
  // Shorter: the old tail is cut off, not left behind.
  CHECK(overwrite_file(file, "bye") == Status::Ok);
  CHECK(read_file(file, data) == Status::Ok);
  CHECK(data == "bye");
  // Longer, and empty.
  CHECK(overwrite_file(file, "a longer text than before") == Status::Ok);
  CHECK(read_file(file, data) == Status::Ok);
  CHECK(data == "a longer text than before");
  CHECK(overwrite_file(file, "") == Status::Ok);
  CHECK(read_file(file, data) == Status::Ok);
  CHECK(data.empty());
  // Told the old length of a large file whose length changes, it removes it and makes it again:
  // the same bytes either way, in both directions, and when the length it was told is wrong.
  const std::string big(20000, 'b');
  const std::string bigger(30000, 'c');
  CHECK(overwrite_file(file, big, 0) == Status::Ok);
  CHECK(read_file(file, data) == Status::Ok);
  CHECK(data == big);
  CHECK(overwrite_file(file, bigger, big.size()) == Status::Ok);
  CHECK(read_file(file, data) == Status::Ok);
  CHECK(data == bigger);
  CHECK(overwrite_file(file, big, bigger.size()) == Status::Ok);
  CHECK(read_file(file, data) == Status::Ok);
  CHECK(data == big);
  CHECK(overwrite_file(file, "small", 12345) == Status::Ok);
  CHECK(read_file(file, data) == Status::Ok);
  CHECK(data == "small");
  // A directory in the way is not a file.
  CHECK(overwrite_file(join_path(tmp.path(), "new"), "x") != Status::Ok);
  // Nothing else left in the directory: no temporaries, unlike a rename would need.
  Vector<DirEntry> entries;
  CHECK(list_directory(join_path(tmp.path(), "new/dir"), entries) == Status::Ok);
  REQUIRE(entries.size() == 1);
  CHECK(entries[0].name == "file.json");
}

TEST_CASE("io: mounts resolve virtual paths and refuse escapes") {
  TempDir tmp("engine_io");
  Vfs vfs;
  CHECK(vfs.mount("content", join_path(tmp.path(), "content")) == Status::Ok);
  CHECK(vfs.mount("cache", join_path(tmp.path(), "cache"), /*writable=*/true) == Status::Ok);
  CHECK(vfs.mount("content", tmp.path()) == Status::AlreadyExists);
  CHECK(vfs.mount("Bad-Scheme", tmp.path()) == Status::InvalidPath);
  CHECK(vfs.mounts().size() == 2);
  REQUIRE(vfs.find_mount("cache") != nullptr);
  CHECK(vfs.find_mount("cache")->writable);

  std::string native;
  CHECK(vfs.resolve("content://levels/a.json", native) == Status::Ok);
  CHECK(native == join_path(tmp.path(), "content/levels/a.json"));
  CHECK(vfs.resolve("content://./levels//b.json", native) == Status::Ok);
  CHECK(native == join_path(tmp.path(), "content/levels/b.json"));
  CHECK(vfs.resolve("content://", native) == Status::Ok);
  CHECK(native == join_path(tmp.path(), "content"));
  CHECK(vfs.resolve("content://../secret", native) == Status::InvalidPath);
  CHECK(vfs.resolve("content://a/../../secret", native) == Status::InvalidPath);
  CHECK(vfs.resolve("content:///abs", native) == Status::InvalidPath);
  CHECK(vfs.resolve("content://C:/x", native) == Status::InvalidPath);
  CHECK(vfs.resolve("nope://x", native) == Status::UnknownMount);
  CHECK(vfs.resolve("content://x", native, /*for_write=*/true) == Status::ReadOnly);
  CHECK(vfs.resolve("cache://x", native, /*for_write=*/true) == Status::Ok);
  CHECK(vfs.resolve("relative\\native.txt", native) == Status::Ok);
  CHECK(native == "relative/native.txt");
  CHECK(vfs.resolve("", native) == Status::InvalidPath);

  // Writing through the writable mount creates directories and reads back.
  CHECK(vfs.write("cache://ddc/ab/cd.bin", "bytes") == Status::Ok);
  std::string data;
  CHECK(vfs.read("cache://ddc/ab/cd.bin", data) == Status::Ok);
  CHECK(data == "bytes");
  CHECK(vfs.exists("cache://ddc/ab/cd.bin"));
  CHECK_FALSE(vfs.exists("cache://ddc/ab/missing.bin"));
  CHECK(vfs.append("cache://logs/j.jsonl", "a\n") == Status::Ok);
  CHECK(vfs.append("cache://logs/j.jsonl", "b\n") == Status::Ok);
  CHECK(vfs.read("cache://logs/j.jsonl", data) == Status::Ok);
  CHECK(data == "a\nb\n");
  CHECK(vfs.append("content://j.jsonl", "x") == Status::ReadOnly);
  CHECK(vfs.write("content://x.txt", "no") == Status::ReadOnly);
  Vector<DirEntry> entries;
  CHECK(vfs.list("cache://ddc", entries) == Status::Ok);
  REQUIRE(entries.size() == 1);
  CHECK(entries[0].name == "ab");
  FileInfo info;
  CHECK(vfs.stat("cache://ddc/ab/cd.bin", info) == Status::Ok);
  CHECK(info.size == 5);
  CHECK(vfs.remove("cache://ddc/ab/cd.bin") == Status::Ok);
  CHECK_FALSE(vfs.exists("cache://ddc/ab/cd.bin"));
  CHECK(vfs.unmount("cache"));
  CHECK_FALSE(vfs.unmount("cache"));
  CHECK(vfs.resolve("cache://x", native) == Status::UnknownMount);
}

TEST_CASE("io: asynchronous reads complete on the job system") {
  TempDir tmp("engine_io");
  constexpr int k_files = 6;
  AsyncRead requests[k_files + 1];
  for (int i = 0; i < k_files; ++i) {
    requests[i].path = join_path(tmp.path(), "f" + std::to_string(i) + ".txt");
    REQUIRE(write_file(requests[i].path, std::string(static_cast<usize>(i) * 1000, 'x')) ==
            Status::Ok);
  }
  requests[k_files].path = join_path(tmp.path(), "missing.txt");

  jobs::JobSystemConfig config;
  config.performance_workers = 2;
  config.efficiency_workers = 2;
  config.pin_threads = false;
  jobs::JobSystem js(config);
  for (AsyncRead& r : requests)
    read_async(js, r);
  CHECK_FALSE(wait_all(js, requests));  // the missing file fails
  for (int i = 0; i < k_files; ++i) {
    CHECK(requests[i].done());
    CHECK(requests[i].status == Status::Ok);
    CHECK(requests[i].data.size() == static_cast<usize>(i) * 1000);
  }
  CHECK(requests[k_files].status == Status::NotFound);
  CHECK(wait_all(js, std::span<AsyncRead>(requests, k_files)));
}

// ---- ranged reads ------------------------------------------------------------------------------

namespace {

// A pattern whose every byte is a function of its offset, so a range read that lands at the wrong
// place is caught by its contents and not only by its length.
std::string range_fixture(usize bytes) {
  std::string out(bytes, '\0');
  for (usize i = 0; i < bytes; ++i)
    out[i] = static_cast<char>((i * 31u + 7u) & 0xFFu);
  return out;
}

}  // namespace

TEST_CASE("io: a file handle reads byte ranges, short at the end and empty past it") {
  TempDir tmp("engine_io");
  const std::string path = join_path(tmp.path(), "ranges.bin");
  const std::string data = range_fixture(100000);
  REQUIRE(write_file(path, data) == Status::Ok);

  FileHandle file;
  REQUIRE(file.open(path) == Status::Ok);
  CHECK(file.valid());
  CHECK(file.size() == data.size());
  CHECK(file.path() == path);

  std::string buffer(4096, '\1');
  u64 read = 0;
  // A range out of the middle: the bytes are the file's, and nothing past the range is touched.
  CHECK(file.read_at(1234, buffer.data(), 1000, read) == Status::Ok);
  CHECK(read == 1000);
  CHECK(std::string_view(buffer).substr(0, 1000) == std::string_view(data).substr(1234, 1000));
  CHECK(buffer[1000] == '\1');

  // The first and the last byte, which is where an off-by-one in the offset arithmetic shows.
  CHECK(file.read_at(0, buffer.data(), 1, read) == Status::Ok);
  CHECK(read == 1);
  CHECK(buffer[0] == data[0]);
  CHECK(file.read_at(data.size() - 1, buffer.data(), 1, read) == Status::Ok);
  CHECK(read == 1);
  CHECK(buffer[0] == data.back());

  // **Past the end is short, not a failure.** A caller that wanted the whole range compares
  // `read` against what it asked for; one reading "the rest of the file" needs no stat first.
  CHECK(file.read_at(data.size() - 10, buffer.data(), 4096, read) == Status::Ok);
  CHECK(read == 10);
  CHECK(file.read_at(data.size(), buffer.data(), 4096, read) == Status::Ok);
  CHECK(read == 0);
  CHECK(file.read_at(data.size() + 1000, buffer.data(), 4096, read) == Status::Ok);
  CHECK(read == 0);

  // A zero-length read is Ok and touches nothing, so a caller with an empty section — which is
  // what an unskinned mesh's binding stream is — needs no special case.
  CHECK(file.read_at(0, nullptr, 0, read) == Status::Ok);
  CHECK(read == 0);

  // Moved, the handle keeps reading and the source is closed.
  FileHandle moved = std::move(file);
  CHECK_FALSE(file.valid());
  CHECK(moved.valid());
  CHECK(moved.read_at(50, buffer.data(), 8, read) == Status::Ok);
  CHECK(read == 8);
  CHECK(std::string_view(buffer).substr(0, 8) == std::string_view(data).substr(50, 8));
  moved.close();
  CHECK_FALSE(moved.valid());
  CHECK(moved.read_at(0, buffer.data(), 8, read) == Status::InvalidArgument);

  FileHandle missing;
  CHECK(missing.open(join_path(tmp.path(), "nothing.bin")) == Status::NotFound);

  // The one-shot form, for a caller with one range and no job system.
  CHECK(read_file_range(path, 900, buffer.data(), 64, read) == Status::Ok);
  CHECK(read == 64);
  CHECK(std::string_view(buffer).substr(0, 64) == std::string_view(data).substr(900, 64));
  CHECK(read_file_range(join_path(tmp.path(), "nothing.bin"), 0, buffer.data(), 8, read) ==
        Status::NotFound);
}

TEST_CASE("io: many ranges of one file read concurrently on the efficiency pool") {
  TempDir tmp("engine_io");
  const std::string path = join_path(tmp.path(), "concurrent.bin");
  const std::string data = range_fixture(1 << 20);
  REQUIRE(write_file(path, data) == Status::Ok);

  FileHandle file;
  REQUIRE(file.open(path) == Status::Ok);

  jobs::JobSystemConfig config;
  config.performance_workers = 2;
  config.efficiency_workers = 2;
  config.pin_threads = false;
  jobs::JobSystem js(config);

  // 128 scattered ranges of one handle through one counter. Scattered rather than sequential
  // because that is what makes a shared file position visible: with one, a read that ran between
  // another read's seek and its transfer would come back with the wrong bytes.
  constexpr u32 k_count = 128;
  constexpr u64 k_span = 3000;
  Vector<AsyncRangeRead> requests;
  std::string dst(k_count * k_span, '\0');
  requests.resize(k_count);
  for (u32 i = 0; i < k_count; ++i) {
    requests[i].file = &file;
    requests[i].offset = (u64{i} * 7919u) % (data.size() - k_span);
    requests[i].bytes = k_span;
    requests[i].dst = dst.data() + u64{i} * k_span;
  }
  jobs::Counter counter;
  read_ranges_async(js, std::span<AsyncRangeRead>(requests.data(), requests.size()), counter);
  js.wait(counter);
  CHECK(counter.done());
  for (u32 i = 0; i < k_count; ++i) {
    CHECK(requests[i].complete());
    CHECK_MESSAGE(std::string_view(dst).substr(u64{i} * k_span, k_span) ==
                      std::string_view(data).substr(static_cast<usize>(requests[i].offset), k_span),
                  "range " << i << " at " << requests[i].offset);
  }

  // A read past the end through the same path is short and therefore not complete, and a request
  // naming no file is refused rather than taking a worker down with it.
  AsyncRangeRead tail[2];
  tail[0].file = &file;
  tail[0].offset = data.size() - 100;
  tail[0].bytes = 4096;
  tail[0].dst = dst.data();
  tail[1].bytes = 16;
  tail[1].dst = dst.data();
  jobs::Counter tail_counter;
  read_ranges_async(js, std::span<AsyncRangeRead>(tail, 2), tail_counter);
  js.wait(tail_counter);
  CHECK(tail[0].status == Status::Ok);
  CHECK(tail[0].read == 100);
  CHECK_FALSE(tail[0].complete());
  CHECK(tail[1].status == Status::InvalidArgument);
}

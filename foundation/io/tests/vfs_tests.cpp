#include <foundation/io/vfs.h>

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

using namespace engine;
using namespace engine::io;

namespace {

struct TempDir {
  std::string path;
  TempDir() {
    const auto p = std::filesystem::temp_directory_path() / "engine_io_tests";
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    path = normalize_path(p.string());
  }
  ~TempDir() { std::filesystem::remove_all(std::filesystem::path(path)); }
};

}  // namespace

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
  TempDir tmp;
  const std::string file = join_path(tmp.path, "sub/dir/hello.txt");
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
  CHECK(stat_file(join_path(tmp.path, "missing"), info) == Status::NotFound);
  CHECK(exists(file));
  CHECK_FALSE(exists(join_path(tmp.path, "missing")));

  CHECK(write_file(join_path(tmp.path, "sub/dir/b.txt"), "") == Status::Ok);
  Vector<DirEntry> entries;
  CHECK(list_directory(join_path(tmp.path, "sub/dir"), entries) == Status::Ok);
  REQUIRE(entries.size() == 2);
  CHECK(entries[0].name == "b.txt");
  CHECK(entries[1].name == "hello.txt");
  CHECK(list_directory(join_path(tmp.path, "sub"), entries) == Status::Ok);
  REQUIRE(entries.size() == 1);
  CHECK(entries[0].is_directory);
  CHECK(list_directory(file, entries) == Status::NotDirectory);
  CHECK(list_directory(join_path(tmp.path, "nope"), entries) == Status::NotFound);

  CHECK(read_file(join_path(tmp.path, "sub"), data) == Status::IsDirectory);
  CHECK(remove_file(join_path(tmp.path, "sub")) == Status::IsDirectory);
  CHECK(remove_file(file) == Status::Ok);
  CHECK(remove_file(file) == Status::NotFound);
  CHECK(rename_path(join_path(tmp.path, "sub/dir/b.txt"), join_path(tmp.path, "sub/c.txt")) ==
        Status::Ok);
  CHECK(exists(join_path(tmp.path, "sub/c.txt")));
  CHECK(remove_directory_recursive(join_path(tmp.path, "sub")) == Status::Ok);
  CHECK_FALSE(exists(join_path(tmp.path, "sub")));
  CHECK(remove_directory_recursive(join_path(tmp.path, "sub")) == Status::NotFound);
  // No temporary files left behind by the atomic write.
  CHECK(list_directory(tmp.path, entries) == Status::Ok);
  CHECK(entries.empty());
}

TEST_CASE("io: mounts resolve virtual paths and refuse escapes") {
  TempDir tmp;
  Vfs vfs;
  CHECK(vfs.mount("content", join_path(tmp.path, "content")) == Status::Ok);
  CHECK(vfs.mount("cache", join_path(tmp.path, "cache"), /*writable=*/true) == Status::Ok);
  CHECK(vfs.mount("content", tmp.path) == Status::AlreadyExists);
  CHECK(vfs.mount("Bad-Scheme", tmp.path) == Status::InvalidPath);
  CHECK(vfs.mounts().size() == 2);
  REQUIRE(vfs.find_mount("cache") != nullptr);
  CHECK(vfs.find_mount("cache")->writable);

  std::string native;
  CHECK(vfs.resolve("content://levels/a.json", native) == Status::Ok);
  CHECK(native == join_path(tmp.path, "content/levels/a.json"));
  CHECK(vfs.resolve("content://./levels//b.json", native) == Status::Ok);
  CHECK(native == join_path(tmp.path, "content/levels/b.json"));
  CHECK(vfs.resolve("content://", native) == Status::Ok);
  CHECK(native == join_path(tmp.path, "content"));
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
  TempDir tmp;
  constexpr int k_files = 6;
  AsyncRead requests[k_files + 1];
  for (int i = 0; i < k_files; ++i) {
    requests[i].path = join_path(tmp.path, "f" + std::to_string(i) + ".txt");
    REQUIRE(write_file(requests[i].path, std::string(static_cast<usize>(i) * 1000, 'x')) ==
            Status::Ok);
  }
  requests[k_files].path = join_path(tmp.path, "missing.txt");

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

#include <core/platform/process.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;
using namespace engine::platform;

namespace {

#if ENGINE_PLATFORM_WINDOWS
const std::string_view k_echo[] = {"cmd.exe", "/c", "echo", "hello", "world"};
const std::string_view k_exit3[] = {"cmd.exe", "/c", "exit", "3"};
const std::string_view k_sort[] = {"sort"};
#else
const std::string_view k_echo[] = {"sh", "-c", "echo hello world"};
const std::string_view k_exit3[] = {"sh", "-c", "exit 3"};
const std::string_view k_sort[] = {"sort"};
#endif

}  // namespace

TEST_CASE("process: spawn, read a line, and collect the exit code") {
  Process p;
  std::string error;
  REQUIRE(p.spawn(k_echo, &error));
  CHECK(error.empty());
  CHECK(p.spawned());
  std::string line;
  REQUIRE(p.read_line(line));
  CHECK(line == "hello world");
  CHECK_FALSE(p.read_line(line));
  CHECK(p.wait() == 0);
  CHECK(p.wait() == 0);  // idempotent
}

TEST_CASE("process: piped input round-trips through a child") {
  Process p;
  REQUIRE(p.spawn(k_sort));
  CHECK(p.write("pear\napple\n"));
  CHECK(p.write("mango\n"));
  p.close_stdin();
  std::string line;
  REQUIRE(p.read_line(line));
  CHECK(line == "apple");
  REQUIRE(p.read_line(line));
  CHECK(line == "mango");
  REQUIRE(p.read_line(line));
  CHECK(line == "pear");
  CHECK_FALSE(p.read_line(line));
  CHECK(p.wait() == 0);
}

TEST_CASE("process: exit codes and read_all") {
  Process p;
  REQUIRE(p.spawn(k_exit3));
  std::string out;
  CHECK(p.read_all(out));
  CHECK(out.empty());
  CHECK(p.wait() == 3);
}

TEST_CASE("process: spawn failure is reported, not fatal") {
  Process p;
  std::string error;
  const std::string_view argv[] = {"definitely-not-a-program-7f3a9c"};
  CHECK_FALSE(p.spawn(argv, &error));
  CHECK_FALSE(error.empty());
  CHECK_FALSE(p.spawned());
  CHECK(p.wait() == -1);
  Process q;
  CHECK_FALSE(q.spawn({}, &error));
}

TEST_CASE("process: executable_directory") {
  const std::string dir = executable_directory();
  CHECK_FALSE(dir.empty());
  CHECK(dir.back() != '/');
  CHECK(dir.find('\\') == std::string::npos);
}

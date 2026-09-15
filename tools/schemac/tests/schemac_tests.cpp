// Negative tests for the schema compiler: every malformed input is rejected with a message
// naming the file and line, exit code 1, and no outputs written. Positive path: a valid schema
// produces the header, the JSON Schema, and the Markdown page.
#include <core/platform/process.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

using namespace engine;

namespace {

struct TempDir {
  std::string path;
  TempDir() {
    const auto p = std::filesystem::temp_directory_path() / "engine_schemac_tests";
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    path = io::normalize_path(p.string());
  }
  ~TempDir() { std::filesystem::remove_all(std::filesystem::path(path)); }
};

struct Run {
  i32 exit_code = -1;
  std::string output;  // stdout and stderr merged
};

// Writes `text` as <dir>/<name>.schema and compiles it into <dir>/out.
Run compile(const TempDir& tmp, const char* name, std::string_view text) {
  const std::string file = io::join_path(tmp.path, std::string(name) + ".schema");
  REQUIRE(io::write_file(file, text) == io::Status::Ok);
  const std::string out = io::join_path(tmp.path, "out");
  const std::string_view argv[] = {ENGINE_SCHEMAC_PATH, "--out",  out,
                                   "--schema-root",     tmp.path, file};
  platform::Process p;
  std::string error;
  Run run;
  REQUIRE_MESSAGE(p.spawn(argv, &error, /*merge_stderr=*/true), error);
  p.close_stdin();
  p.read_all(run.output);
  run.exit_code = p.wait();
  return run;
}

void expect_rejected(const TempDir& tmp, const char* name, std::string_view text, int line,
                     std::string_view message) {
  const Run run = compile(tmp, name, text);
  INFO(run.output);
  CHECK(run.exit_code == 1);
  CHECK(run.output.find(std::string(name) + ".schema:" + std::to_string(line) + ": error:") !=
        std::string::npos);
  CHECK(run.output.find(message) != std::string::npos);
  CHECK_FALSE(
      io::exists(io::join_path(tmp.path, std::string("out/include/schemas/") + name + ".h")));
}

}  // namespace

TEST_CASE("schemac: a valid schema produces every output and exits 0") {
  TempDir tmp;
  const Run run = compile(tmp, "good",
                          "namespace t.good\n"
                          "/// Documented.\n"
                          "enum Kind : u8 { A = 0\n  B = 1 }\n"
                          "struct Item @version(2) {\n"
                          "  id: id128\n"
                          "  kind: Kind = B\n"
                          "  weight: f32 = 1.5\n"
                          "  tags: string[]\n"
                          "  extra: json\n"
                          "  added: u32 = 0 @since(2)\n"
                          "}\n");
  INFO(run.output);
  CHECK(run.exit_code == 0);
  CHECK(io::exists(io::join_path(tmp.path, "out/include/schemas/good.h")));
  CHECK(io::exists(io::join_path(tmp.path, "out/src/good.cpp")));
  CHECK(io::exists(io::join_path(tmp.path, "out/json/good.schema.json")));
  CHECK(io::exists(io::join_path(tmp.path, "out/docs/good.md")));
  std::string header;
  REQUIRE(io::read_file(io::join_path(tmp.path, "out/include/schemas/good.h"), header) ==
          io::Status::Ok);
  CHECK(header.find("struct Item") != std::string::npos);
  CHECK(header.find("k_schema_version = 2") != std::string::npos);
  CHECK(header.find("Kind::B") != std::string::npos);
}

TEST_CASE("schemac: syntax errors name the file and line") {
  TempDir tmp;
  expect_rejected(tmp, "no_namespace", "struct A { x: u8 }\n", 1, "file declares no namespace");
  expect_rejected(tmp, "anon_struct", "namespace t.a\nstruct { x: u8 }\n", 2,
                  "expected a struct name");
  expect_rejected(tmp, "bad_field", "namespace t.b\nstruct A { x u8 }\n", 2, "expected");
  expect_rejected(tmp, "zero_array", "namespace t.c\nstruct A { x: u8[0] }\n", 2,
                  "fixed array length must be positive");
  expect_rejected(tmp, "unterminated", "namespace t.d\nstruct A {\n  x: u8\n", 4, "expected");
}

TEST_CASE("schemac: resolution errors") {
  TempDir tmp;
  expect_rejected(tmp, "unknown_type", "namespace t.e\nstruct A { x: Nope }\n", 2,
                  "unknown type 'Nope'");
  expect_rejected(tmp, "string_default", "namespace t.f\nstruct A { x: u32 = \"text\" }\n", 2,
                  "string default on a non-string field");
  expect_rejected(tmp, "bad_enumerator",
                  "namespace t.g\nenum E : u8 { A = 0 }\nstruct S { e: E = Nope }\n", 3,
                  "'Nope' is not an enumerator of t.g.E");
  expect_rejected(tmp, "dup_field", "namespace t.h\nstruct A { x: u8\n  x: u8 }\n", 3,
                  "duplicate field 'x'");
  expect_rejected(tmp, "dup_type", "namespace t.i\nstruct A { x: u8 }\nstruct A { y: u8 }\n", 3,
                  "duplicate type 't.i.A'");
}

TEST_CASE("schemac: a missing import is reported and nothing is written") {
  TempDir tmp;
  const Run run = compile(tmp, "imports_missing",
                          "namespace t.j\nimport \"missing.schema\"\nstruct A { x: u8 }\n");
  INFO(run.output);
  CHECK(run.exit_code == 1);
  CHECK(run.output.find("missing.schema") != std::string::npos);
  CHECK_FALSE(io::exists(io::join_path(tmp.path, "out/include/schemas/imports_missing.h")));
}

TEST_CASE("schemac: usage errors exit 2") {
  platform::Process p;
  const std::string_view argv[] = {ENGINE_SCHEMAC_PATH};
  REQUIRE(p.spawn(argv, nullptr, /*merge_stderr=*/true));
  std::string output;
  p.read_all(output);
  CHECK(p.wait() == 2);
  CHECK(output.find("usage:") != std::string::npos);
}

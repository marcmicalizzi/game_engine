// engine-content tissue end to end (docs/subsystems/apps.md, "engine-content tissue"): the
// synthetic definition written as an interchange, imported into a container, described, validated
// and reported, one JSON line each; and a broken block or a broken contract refused with exit 1.
#include <core/json/json.h>
#include <core/platform/process.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <string>
#include <vector>

using namespace engine;

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
  JsonValue result;
};

Run content(std::vector<std::string> args) {
  static const std::string exe = test::app_path(ENGINE_APP_PATH);
  std::vector<std::string_view> argv;
  argv.push_back(exe);
  for (const std::string& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Run run;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error)) {
    FAIL("cannot spawn engine-content: " << error);
    return run;
  }
  p.close_stdin();
  p.read_all(run.output);
  run.exit_code = p.wait();
  if (!run.output.empty()) (void)parse_json(run.output, run.result);
  return run;
}

u64 count(const JsonValue& v, std::string_view key) {
  u64 out = 0;
  const JsonValue* at = v.find(key);
  if (at != nullptr) at->get_u64(out);
  return out;
}

}  // namespace

TEST_CASE("tissue: example, import, info, validate and report, one JSON line each") {
  engine::test::TempDir tmp("content_tissue");
  const std::string dir = tmp.file("example");
  Run run = content({"tissue", "example", dir});
  INFO(run.output);
  REQUIRE(run.exit_code == 0);
  const std::string interchange = dir + "/synthetic.json";
  CHECK(io::exists(interchange));
  CHECK(io::exists(dir + "/synthetic.tissue"));

  const std::string container = tmp.file("copy.tissue");
  run = content({"tissue", "import", interchange, container});
  REQUIRE(run.exit_code == 0);
  CHECK(count(run.result, "blocks") > 20);

  run = content({"tissue", "info", container});
  REQUIRE(run.exit_code == 0);
  const JsonValue* sections = run.result.find("sections");
  REQUIRE(sections != nullptr);
  CHECK(sections->size() > 20);
  for (usize i = 0; i < sections->size(); ++i) {
    bool known = false;
    (*sections)[i].find("known")->get_bool(known);
    CHECK(known);
  }
  // The imported container is the example's container, byte for byte.
  std::string a;
  std::string b;
  io::read_file(container, a);
  io::read_file(dir + "/synthetic.tissue", b);
  CHECK(a == b);

  run = content({"tissue", "validate", container});
  INFO(run.output);
  CHECK(run.exit_code == 0);
  CHECK(count(run.result, "errors") == 0);
  CHECK(run.result.find("rows")->size() > 20);
  CHECK(run.result.find("numbers") == nullptr);

  // The report reads the interchange as readily as the container.
  run = content({"tissue", "report", interchange, "--no-modes"});
  REQUIRE(run.exit_code == 0);
  const JsonValue* numbers = run.result.find("numbers");
  REQUIRE(numbers != nullptr);
  CHECK(numbers->find("regions")->find("slab")->find("states")->find("pressed") != nullptr);
}

TEST_CASE("tissue: the ten-node example, and the fixture mode's contract") {
  engine::test::TempDir tmp("content_tissue_fixture");
  const std::string dir = tmp.file("example");
  Run run = content({"tissue", "example", dir});
  REQUIRE(run.exit_code == 0);
  const std::string body = dir + "/synthetic-quadratic.tissue";
  REQUIRE(io::exists(body));
  CHECK(io::exists(dir + "/synthetic-quadratic.json"));

  // A reference body of ten-node cells: it validates, and its one failure is the cover warning.
  run = content({"tissue", "validate", body, "--no-modes"});
  INFO(run.output);
  CHECK(run.exit_code == 0);
  CHECK(count(run.result, "errors") == 0);
  CHECK(count(run.result, "warnings") == 1);

  // The declaration a first run matches, written for review; then the fixture mode reads it.
  const std::string declared = tmp.file("expected.json");
  run = content({"tissue", "validate", body, "--no-modes", "--write-expect", declared});
  CHECK(run.exit_code == 0);
  CHECK(count(run.result, "expect_failures") == 1);
  run = content({"tissue", "validate", body, "--no-modes", "--expect", declared});
  CHECK(run.exit_code == 0);
  const JsonValue* expect = run.result.find("expect");
  REQUIRE(expect != nullptr);
  bool matched = false;
  expect->find("matched")->get_bool(matched);
  CHECK(matched);

  // The failure left out of the declaration: a regression, exit 1, the difference in the line.
  std::string text;
  REQUIRE(io::read_file(declared, text) == io::Status::Ok);
  JsonValue json;
  REQUIRE(parse_json(text, json).ok);
  json.set("failures", JsonValue::array());
  REQUIRE(io::write_file(declared, write_json(json)) == io::Status::Ok);
  run = content({"tissue", "validate", body, "--no-modes", "--expect", declared});
  CHECK(run.exit_code == 1);
  REQUIRE(run.result.find("expect") != nullptr);
  CHECK(run.result.find("expect")->find("differences")->size() == 1);

  // A declaration that is not one is refused before anything is validated; the flags are
  // validate's.
  REQUIRE(io::write_file(declared, "{\"format\": \"something else\", \"failures\": []}") ==
          io::Status::Ok);
  CHECK(content({"tissue", "validate", body, "--expect", declared}).exit_code == 1);
  CHECK(content({"tissue", "report", body, "--expect", declared}).exit_code == 2);
  CHECK(content({"tissue", "validate", body, "--expect"}).exit_code == 2);
}

TEST_CASE("tissue: a damaged block and a broken contract are refused") {
  engine::test::TempDir tmp("content_tissue_refusals");
  const std::string dir = tmp.file("example");
  REQUIRE(content({"tissue", "example", dir}).exit_code == 0);
  const std::string interchange = dir + "/synthetic.json";

  // The topology hash declared wrong: the contract row fails, and validate says so with exit 1.
  std::string text;
  REQUIRE(io::read_file(interchange, text) == io::Status::Ok);
  JsonValue json;
  REQUIRE(parse_json(text, json).ok);
  json["observation"].set("topology_sha256", JsonValue(std::string(64, '0')));
  REQUIRE(io::write_file(interchange, write_json(json)) == io::Status::Ok);
  Run run = content({"tissue", "validate", interchange, "--no-modes"});
  CHECK(run.exit_code == 1);
  CHECK(count(run.result, "errors") >= 1);

  // A block's bytes changed: the import refuses it by its SHA-256.
  const std::string block = dir + "/synthetic.slab.nodes.bin";
  std::string bytes;
  REQUIRE(io::read_file(block, bytes) == io::Status::Ok);
  bytes[5] = static_cast<char>(bytes[5] ^ 0x40);
  REQUIRE(io::write_file(block, bytes) == io::Status::Ok);
  run = content({"tissue", "import", interchange, tmp.file("x.tissue")});
  CHECK(run.exit_code == 1);

  CHECK(content({"tissue", "frobnicate", interchange}).exit_code == 2);
  CHECK(content({"tissue", "validate"}).exit_code == 2);
}

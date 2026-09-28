// engine-content tissue end to end (docs/subsystems/apps.md, "engine-content tissue"): the
// synthetic definition written as an interchange, imported into a container, described, validated
// and reported, one JSON line each; a broken block or a broken contract refused with exit 1; the
// capability line; the layered model's fixtures, and a capability failure as exit 3 that the
// fixture mode never matches; and the authoring side's native supine fixtures, where the machine
// has them, reading row for row as they always have.
#include <core/json/json.h>
#include <core/platform/process.h>
#include <domain/tissue/sha256.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <span>
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

TEST_CASE("tissue: capabilities is one line: the schema, the records and blocks, every row") {
  Run run = content({"tissue", "capabilities"});
  INFO(run.output);
  REQUIRE(run.exit_code == 0);
  CHECK(count(run.result, "schema_version") == 2);
  REQUIRE(run.result.find("records") != nullptr);
  CHECK(run.result.find("records")->find("ContactPair") != nullptr);
  CHECK(run.result.find("records")->find("MaterialBoundarySurface") != nullptr);
  std::string_view status;
  run.result.find("rows")->find("attachment.reaction_balance")->get_string(status);
  CHECK(status == "not-implemented");
  run.result.find("rows")->find("reference.certificate_provenance")->get_string(status);
  CHECK(status == "info-only");
  run.result.find("rows")->find("contact.curved_clearance")->get_string(status);
  CHECK(status == "evaluated");
  // The separated tie (Attachment version 3): its field, its enumeration and its row.
  CHECK(count(*run.result.find("records"), "Attachment") == 3);
  const JsonValue* fields = run.result.find("fields");
  REQUIRE(fields != nullptr);
  u32 tie_fields = 0;
  for (usize i = 0; i < fields->size(); ++i) {
    std::string_view field;
    (*fields)[i].get_string(field);
    tie_fields += field == "Attachment.interface" || field == "Attachment.gap_m" ? 1u : 0u;
  }
  CHECK(tie_fields == 2);
  const JsonValue* interfaces = run.result.find("enums")->find("AttachmentInterface");
  REQUIRE(interfaces != nullptr);
  CHECK(interfaces->size() == 2);
  run.result.find("rows")->find("attachment.target_gap")->get_string(status);
  CHECK(status == "evaluated");
  CHECK(run.result.find("build")->find("commit") != nullptr);
  CHECK(content({"tissue", "capabilities", "extra"}).exit_code == 2);
}

TEST_CASE("tissue: the layered model's fixtures, and a capability failure is exit 3") {
  engine::test::TempDir tmp("content_tissue_layered");
  const std::string dir = tmp.file("example");
  Run run = content({"tissue", "example", dir});
  INFO(run.output);
  REQUIRE(run.exit_code == 0);
  for (const char* stem : {"layered-slab", "layered-fusiform", "layered-tied-slab"}) {
    const std::string interchange = dir + "/" + stem + ".json";
    const std::string container = tmp.file(std::string(stem) + ".tissue");
    run = content({"tissue", "import", interchange, container});
    REQUIRE(run.exit_code == 0);
    std::string a;
    std::string b;
    io::read_file(container, a);
    io::read_file(dir + "/" + stem + ".tissue", b);
    CHECK(a == b);
    run = content({"tissue", "validate", container});
    INFO(run.output);
    CHECK(run.exit_code == 0);
    CHECK(count(run.result, "errors") == 0);
    CHECK(count(run.result, "warnings") == 0);
    // The tied slab's two grips are separated ties, each with its gap row.
    u32 gaps = 0;
    const JsonValue* rows = run.result.find("rows");
    REQUIRE(rows != nullptr);
    for (usize i = 0; i < rows->size(); ++i) {
      std::string_view id;
      (*rows)[i].find("id")->get_string(id);
      gaps += id == "attachment.target_gap" ? 1u : 0u;
    }
    CHECK(gaps == (std::string_view(stem) == "layered-tied-slab" ? 2u : 0u));
  }

  // A file that requires a row this build does not implement: refused by import and by validate
  // with exit 3 and the capability named, and never matched by the fixture mode, whatever the
  // declaration says.
  const std::string interchange = dir + "/layered-slab.json";
  std::string text;
  REQUIRE(io::read_file(interchange, text) == io::Status::Ok);
  JsonValue json;
  REQUIRE(parse_json(text, json).ok);
  json["requirements"]["rows"].push_back(JsonValue("attachment.reaction_balance"));
  REQUIRE(io::write_file(interchange, write_json(json)) == io::Status::Ok);
  run = content({"tissue", "import", interchange, tmp.file("refused.tissue")});
  CHECK(run.exit_code == 3);
  CHECK_FALSE(io::exists(tmp.file("refused.tissue")));
  run = content({"tissue", "validate", interchange});
  CHECK(run.exit_code == 3);
  REQUIRE(run.result.find("capability_failure") != nullptr);
  CHECK(run.result.find("rows") == nullptr);
  const std::string declared = tmp.file("expected.json");
  REQUIRE(io::write_file(declared,
                         R"({"format": "astra.tissue.expected-failures.v1", "failures": [
                           {"id": "attachment.reaction_balance", "subject": "the definition",
                            "severity": "error", "verdict": "Skipped"}]})") == io::Status::Ok);
  CHECK(content({"tissue", "validate", interchange, "--expect", declared}).exit_code == 3);
  // The same declaration against the sound fixture is refused too: it declares a row this build
  // does not implement, which is a capability and not a failure it could match.
  run = content({"tissue", "validate", dir + "/layered-fusiform.tissue", "--expect", declared});
  CHECK(run.exit_code == 3);
  CHECK(run.result.find("capability_failure") != nullptr);
}

TEST_CASE("tissue: the native supine fixtures still read, row for row") {
  // The authoring side's native ten-node supine pair (docs/subsystems/tissue.md, "The supine
  // fixtures"), found by ENGINE_TISSUE_SUPINE_NATIVE or where the owner's machine keeps it, and
  // skipped where it is not: nothing of the packet is committed. Each file is checked against the
  // hash the regression pins before it is read, and copied to the test's own scratch directory
  // first — the packet's directory is never written or run from.
  std::string root = engine::test::detail::environment("ENGINE_TISSUE_SUPINE_NATIVE");
  if (root.empty())
    root =
        "D:/workspace/game_engine_assets/Blender/_handoff/astra-supine-native-reference-2026-09-26";
  if (!engine::test::path_exists(root)) {
    MESSAGE("the native supine packet is not on this machine (" << root << "): skipped");
    return;
  }
  struct Case {
    const char* name;
    const char* sha256;
  };
  const Case cases[] = {
      {"B2a-supine", "56c034f7413d57d43bf235b2d68060dd61a07de19c1098b1c1cf1c52f5b3fefd"},
      {"B2b-supine", "afb39fc7502979791d4bc0389bc3836d5b4cdf2b9314c6e05d501a3fda20d914"}};
  engine::test::TempDir tmp("content_tissue_supine");
  for (const Case& c : cases) {
    INFO(c.name);
    std::string bytes;
    REQUIRE(io::read_file(root + "/" + c.name + "/reference-native.tissue", bytes) ==
            io::Status::Ok);
    const std::string hash = tissue::sha256_hex(
        std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size()));
    REQUIRE(hash == c.sha256);
    std::string declaration;
    REQUIRE(io::read_file(root + "/" + c.name + "/EXPECTED-FAILURES.predeclared.json",
                          declaration) == io::Status::Ok);
    const std::string file = tmp.file(std::string(c.name) + ".tissue");
    const std::string expected = tmp.file(std::string(c.name) + ".expected.json");
    REQUIRE(io::write_file(file, bytes) == io::Status::Ok);
    REQUIRE(io::write_file(expected, declaration) == io::Status::Ok);

    Run run = content({"tissue", "validate", file, "--expect", expected});
    INFO(run.output);
    CHECK(run.exit_code == 0);
    const JsonValue* rows = run.result.find("rows");
    REQUIRE(rows != nullptr);
    CHECK(rows->size() == 43);
    u32 pass = 0;
    u32 info = 0;
    u32 fail = 0;
    u32 other = 0;
    for (usize i = 0; i < rows->size(); ++i) {
      std::string_view verdict;
      (*rows)[i].find("verdict")->get_string(verdict);
      if (verdict == "pass")
        ++pass;
      else if (verdict == "info")
        ++info;
      else if (verdict == "fail")
        ++fail;
      else
        ++other;
    }
    CHECK(pass == 27);
    CHECK(info == 12);
    CHECK(fail == 4);
    CHECK(other == 0);
    bool matched = false;
    run.result.find("expect")->find("matched")->get_bool(matched);
    CHECK(matched);
  }
}

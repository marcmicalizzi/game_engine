// engine-content tissue end to end (docs/subsystems/apps.md, "engine-content tissue"): the
// synthetic definition written as an interchange, imported into a container, described, validated
// and reported, one JSON line each; a broken block or a broken contract refused with exit 1; the
// capability line; the layered model's fixtures, and a capability failure as exit 3 that the
// fixture mode never matches; a runtime cage derived from a ten-node body and measured against it;
// and the authoring side's native supine fixtures, where the machine has them, reading row for row
// as they always have and each giving the runtime cage the experiment page records.
#include <core/json/json.h>
#include <core/platform/process.h>
#include <domain/tissue/sha256.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
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

f64 number_at(const JsonValue& v, std::string_view key) {
  f64 out = 0.0;
  const JsonValue* at = v.find(key);
  if (at != nullptr) at->get_f64(out);
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

namespace {

// The authoring side's native ten-node supine pair (docs/subsystems/tissue.md, "The supine
// fixtures"), found by ENGINE_TISSUE_SUPINE_NATIVE or where the owner's machine keeps it, and
// skipped where it is not: nothing of the packet is committed. Each file is checked against the
// hash the regression pins before it is read, and copied to the test's own scratch directory
// first — the packet's directory is never written or run from.
std::string supine_root() {
  std::string root = engine::test::detail::environment("ENGINE_TISSUE_SUPINE_NATIVE");
  if (root.empty())
    root =
        "D:/workspace/game_engine_assets/Blender/_handoff/astra-supine-native-reference-2026-09-26";
  return engine::test::path_exists(root) ? root : std::string();
}

struct SupineCase {
  const char* name;
  const char* sha256;
};
constexpr SupineCase k_supine_cases[] = {
    {"B2a-supine", "56c034f7413d57d43bf235b2d68060dd61a07de19c1098b1c1cf1c52f5b3fefd"},
    {"B2b-supine", "afb39fc7502979791d4bc0389bc3836d5b4cdf2b9314c6e05d501a3fda20d914"}};

// The case's `reference-native.tissue`, its hash checked, copied into `dir`: the copy's path.
std::string copy_supine(const std::string& root, const SupineCase& c, engine::test::TempDir& dir) {
  std::string bytes;
  REQUIRE(io::read_file(root + "/" + c.name + "/reference-native.tissue", bytes) == io::Status::Ok);
  const std::string hash = tissue::sha256_hex(
      std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size()));
  REQUIRE(hash == c.sha256);
  const std::string file = dir.file(std::string(c.name) + ".tissue");
  REQUIRE(io::write_file(file, bytes) == io::Status::Ok);
  return file;
}

const JsonValue* row_of(const JsonValue& result, std::string_view id) {
  const JsonValue* rows = result.find("rows");
  if (rows == nullptr) return nullptr;
  for (usize i = 0; i < rows->size(); ++i) {
    std::string_view got;
    (*rows)[i].find("id")->get_string(got);
    if (got == id) return &(*rows)[i];
  }
  return nullptr;
}

std::string verdict_of(const JsonValue& result, std::string_view id) {
  const JsonValue* r = row_of(result, id);
  if (r == nullptr) return "absent";
  std::string_view v;
  r->find("verdict")->get_string(v);
  return std::string(v);
}

}  // namespace

TEST_CASE("tissue: cage derives a runtime cage from the ten-node example, and validates it") {
  engine::test::TempDir tmp("content_tissue_cage");
  const std::string dir = tmp.file("example");
  REQUIRE(content({"tissue", "example", dir}).exit_code == 0);
  const std::string reference = dir + "/synthetic-quadratic.tissue";
  const std::string cage = tmp.file("cage.tissue");
  Run run = content({"tissue", "cage", reference, cage});
  INFO(run.output);
  REQUIRE(run.exit_code == 0);
  CHECK(count(*run.result.find("cage"), "nodes") == 147);
  CHECK(count(*run.result.find("cage"), "cells") == 432);
  CHECK(count(run.result, "node_budget") == 256);
  REQUIRE(run.result.find("mass_ledger") != nullptr);
  CHECK(run.result.find("not_carried")->size() >= 5);

  // Alone, it validates as a runtime cage; with its source, every cage row is evaluated.
  run = content({"tissue", "validate", cage});
  INFO(run.output);
  CHECK(run.exit_code == 0);
  CHECK(verdict_of(run.result, "region.cage_size") == "pass");
  CHECK(verdict_of(run.result, "cage.source") == "pass");
  CHECK(verdict_of(run.result, "cage.boundary_distance") == "skipped");
  run = content({"tissue", "validate", cage, "--source", reference, "--no-modes"});
  INFO(run.output);
  CHECK(run.exit_code == 0);
  CHECK(count(run.result, "errors") == 0);
  for (const char* id :
       {"cage.source", "cage.volume", "cage.boundary_distance", "cage.state_displacement"})
    CHECK_MESSAGE(verdict_of(run.result, id) == "pass", id);
  CHECK(verdict_of(run.result, "cage.strain_energy") == "info");
  // The wrong source is an error.
  run = content({"tissue", "validate", cage, "--source", dir + "/synthetic.tissue"});
  CHECK(run.exit_code == 1);
  CHECK(verdict_of(run.result, "cage.source") == "fail");

  // A smaller budget collapses; a wider one than a solve group needs --hero; a runtime cage is not
  // a reference body; the flags are cage's.
  run = content({"tissue", "cage", reference, tmp.file("small.tissue"), "--nodes", "100"});
  REQUIRE(run.exit_code == 0);
  CHECK(count(*run.result.find("cage"), "nodes") == 100);
  CHECK(content({"tissue", "cage", reference, tmp.file("x.tissue"), "--nodes", "300"}).exit_code ==
        1);
  CHECK(content({"tissue", "cage", reference, tmp.file("x.tissue"), "--nodes", "300", "--hero"})
            .exit_code == 0);
  CHECK(content({"tissue", "cage", dir + "/synthetic.tissue", tmp.file("y.tissue")}).exit_code ==
        1);
  CHECK(content({"tissue", "validate", cage, "--nodes", "10"}).exit_code == 2);
  CHECK(content({"tissue", "cage", reference}).exit_code == 2);
}

TEST_CASE("tissue: settle builds a runtime region as a soft body and settles it under a load") {
  engine::test::TempDir tmp("content_tissue_settle");
  const std::string dir = tmp.file("example");
  REQUIRE(content({"tissue", "example", dir}).exit_code == 0);
  const std::string slab = dir + "/synthetic.tissue";
  const std::string written = tmp.file("settled.tissue");
  Run run =
      content({"tissue", "settle", slab, "--state", "pressed", "--repeat", "--write", written});
  INFO(run.output);
  REQUIRE(run.exit_code == 0);
  bool settled = false;
  run.result.find("settled")->get_bool(settled);
  CHECK(settled);
  bool same = false;
  run.result.find("repeat")->find("same_bytes")->get_bool(same);
  CHECK(same);
  REQUIRE(run.result.find("distance_mm") != nullptr);
  CHECK(count(*run.result.find("distance_mm"), "count") == 147);
  CHECK(count(run.result, "inverted_cells") == 0);
  // The settled nodes as a state of the file, which validates.
  run = content({"tissue", "validate", written, "--no-modes"});
  INFO(run.output);
  CHECK(run.exit_code == 0);
  // A ten-node reference body is not a cage; a state that does not exist; the flags are settle's.
  CHECK(content({"tissue", "settle", dir + "/synthetic-quadratic.tissue", "--state", "pressed"})
            .exit_code == 1);
  CHECK(content({"tissue", "settle", slab, "--state", "no such state"}).exit_code == 1);
  CHECK(content({"tissue", "settle", slab}).exit_code == 2);
  CHECK(content({"tissue", "validate", slab, "--repeat"}).exit_code == 2);
}

TEST_CASE("tissue: a runtime cage from each native supine fixture, and how far it is from it") {
  const std::string root = supine_root();
  if (root.empty()) {
    MESSAGE("the native supine packet is not on this machine: skipped");
    return;
  }
  engine::test::TempDir tmp("content_tissue_supine_cage");
  for (const SupineCase& c : k_supine_cases) {
    const std::string name = c.name;
    INFO(name);
    const std::string reference = copy_supine(root, c, tmp);
    // The failed standing reference is valid as ten-node cells only through their curvature: one
    // corner tetrahedron inverts there, and the derivation refuses to carry it unless told to
    // leave it out.
    Run run = content({"tissue", "cage", reference, tmp.file(name + ".refused.tissue")});
    CHECK(run.exit_code == 1);
    for (const bool hero : {false, true}) {
      INFO(hero);
      const std::string cage = tmp.file(name + (hero ? ".hero" : "") + ".cage.tissue");
      std::vector<std::string> args = {"tissue", "cage",         reference,
                                       cage,     "--omit-state", "failed-standing-reference"};
      if (hero) args.push_back("--hero");
      run = content(args);
      INFO(run.output);
      REQUIRE(run.exit_code == 0);
      // The numbers docs/experiments/tissue-runtime-cage-2026-09-29.md records.
      const JsonValue& summary = run.result;
      CHECK(count(*summary.find("cage"), "nodes") == (hero ? 275u : 256u));
      CHECK(count(*summary.find("cage"), "cells") == (hero ? 846u : 746u));
      CHECK(count(*summary.find("collapses"), "interior") == (hero ? 0u : 19u));
      CHECK(count(*summary.find("collapses"), "boundary") == 0);
      CHECK(count(*summary.find("cage"), "boundary_nodes") == 242);
      CHECK(number_at(*summary.find("cage"), "sicn_min") ==
            doctest::Approx(0.119319).epsilon(1e-5));
      CHECK(std::fabs(number_at(*summary.find("mass_ledger"), "difference_relative")) < 1e-6);
      run = content({"tissue", "validate", cage, "--source", reference, "--no-modes"});
      INFO(run.output);
      CHECK(run.exit_code == 0);
      CHECK(count(run.result, "errors") == 0);
      CHECK(count(run.result, "warnings") == 0);
      for (const char* id :
           {"cage.source", "cage.volume", "cage.boundary_distance", "cage.state_displacement"})
        CHECK_MESSAGE(verdict_of(run.result, id) == "pass", id);
      const JsonValue* boundary = row_of(run.result, "cage.boundary_distance");
      REQUIRE(boundary != nullptr);
      CHECK(number_at(*boundary->find("value"), "max_mm") == doctest::Approx(1.7837).epsilon(1e-3));
      const JsonValue* field = row_of(run.result, "cage.state_displacement");
      REQUIRE(field != nullptr);
      CHECK(number_at(*field->find("value"), "at_reference_nodes_max_mm") ==
            doctest::Approx(1.8059).epsilon(1e-3));
      CHECK(number_at(*field->find("value"), "at_cage_nodes_max_um") < 1e-6);
    }
  }
}

TEST_CASE("tissue: the supine cage as a soft body under the supine load, measured and pinned") {
  // The measurement docs/experiments/tissue-runtime-cage-2026-09-29.md records, pinned as a
  // regression and not a pass: the contact is the physics module's own (vertex against triangle,
  // one-sided, frictionless on the support), not the face-based model the authoring side is still
  // declaring, and how far the settled cage lies from the certified supine state is the gap later
  // work closes. The positions are the same bytes on every run (Jolt's deterministic mode) and on
  // every configuration, so the numbers are pinned tight.
  const std::string root = supine_root();
  if (root.empty()) {
    MESSAGE("the native supine packet is not on this machine: skipped");
    return;
  }
  engine::test::TempDir tmp("content_tissue_supine_settle");
  const auto distance = [](const JsonValue& line, std::string_view key) {
    return number_at(*line.find("distance_mm"), key);
  };
  for (const SupineCase& c : k_supine_cases) {
    const std::string name = c.name;
    INFO(name);
    const std::string reference = copy_supine(root, c, tmp);
    const std::string hero = tmp.file(name + ".hero.cage.tissue");
    REQUIRE(content({"tissue", "cage", reference, hero, "--omit-state", "failed-standing-reference",
                     "--hero"})
                .exit_code == 0);
    // The 275-node hero cage settles at ADR-0029's defaults (8 iterations, 2 sub-steps).
    const bool first = name == "B2a-supine";
    std::vector<std::string> args = {"tissue", "settle", hero, "--state", name};
    if (first) args.push_back("--repeat");
    Run run = content(args);
    INFO(run.output);
    REQUIRE(run.exit_code == 0);
    CHECK(count(run.result, "steps") == 216);  // 221 until the backend became double (ADR-0053)
    CHECK(count(run.result, "inverted_cells") == 0);
    CHECK(distance(run.result, "p50") == doctest::Approx(1.58027).epsilon(1e-4));
    CHECK(distance(run.result, "p95") == doctest::Approx(3.54670).epsilon(1e-4));
    CHECK(distance(run.result, "max") == doctest::Approx(4.02938).epsilon(1e-4));
    CHECK(count(*run.result.find("largest"), "node") == 20);
    if (first) {
      bool same = false;
      run.result.find("repeat")->find("same_bytes")->get_bool(same);
      CHECK(same);
      // The 256-node cage at the same settings does not settle: it keeps a vibration of 7 mm/s at
      // two collision sub-steps a step (four settle it) while its shape stays where it is, so it
      // is pinned at a fixed step count.
      const std::string cage = tmp.file(name + ".cage.tissue");
      REQUIRE(
          content({"tissue", "cage", reference, cage, "--omit-state", "failed-standing-reference"})
              .exit_code == 0);
      run = content({"tissue", "settle", cage, "--state", name, "--max-steps", "300"});
      INFO(run.output);
      CHECK(run.exit_code == 1);
      bool settled = true;
      run.result.find("settled")->get_bool(settled);
      CHECK_FALSE(settled);
      CHECK(count(run.result, "steps") == 300);
      CHECK(distance(run.result, "p50") == doctest::Approx(1.27003).epsilon(1e-4));
      CHECK(distance(run.result, "p95") == doctest::Approx(2.84663).epsilon(1e-4));
      CHECK(distance(run.result, "max") == doctest::Approx(3.35339).epsilon(1e-4));
      CHECK(count(*run.result.find("largest"), "node") == 19);
    }
  }
}

TEST_CASE("tissue: the native supine fixtures still read, row for row") {
  const std::string root = supine_root();
  if (root.empty()) {
    MESSAGE("the native supine packet is not on this machine: skipped");
    return;
  }
  engine::test::TempDir tmp("content_tissue_supine");
  for (const SupineCase& c : k_supine_cases) {
    INFO(c.name);
    const std::string file = copy_supine(root, c, tmp);
    std::string declaration;
    REQUIRE(io::read_file(root + "/" + c.name + "/EXPECTED-FAILURES.predeclared.json",
                          declaration) == io::Status::Ok);
    const std::string expected = tmp.file(std::string(c.name) + ".expected.json");
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

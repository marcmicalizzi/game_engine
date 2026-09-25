// Negative tests for the schema compiler: every malformed input is rejected with a message
// naming the file and line, exit code 1, and no outputs written. Positive path: a valid schema
// produces the header, the JSON Schema, and the Markdown page.
#include <core/platform/process.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <string>

using namespace engine;

namespace {

// The built compiler, or the bundle's copy of it (tests/support/test_paths.h).
const std::string& schemac_exe() {
  static const std::string path = test::app_path(ENGINE_SCHEMAC_PATH);
  return path;
}

// One directory per object under `<system temp>/engine-tests/`, not a fixed path: two copies of
// this test — another worktree's build, a release build beside a debug one, CI running while a
// developer runs the suite — used to land on the same directory and delete each other's
// fixtures. See tests/support/test_temp_dir.h for the measurement.
using TempDir = test::TempDir;

struct Run {
  i32 exit_code = -1;
  std::string output;  // stdout and stderr merged
};

// Writes `text` as <dir>/<name>.schema and compiles it into <dir>/out.
Run compile(const TempDir& tmp, const char* name, std::string_view text) {
  const std::string file = io::join_path(tmp.path(), std::string(name) + ".schema");
  REQUIRE(io::write_file(file, text) == io::Status::Ok);
  const std::string out = io::join_path(tmp.path(), "out");
  const std::string_view argv[] = {schemac_exe(), "--out", out, "--schema-root", tmp.path(), file};
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
      io::exists(io::join_path(tmp.path(), std::string("out/include/schemas/") + name + ".h")));
}

}  // namespace

TEST_CASE("schemac: a valid schema produces every output and exits 0") {
  TempDir tmp("schemac");
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
  CHECK(io::exists(io::join_path(tmp.path(), "out/include/schemas/good.h")));
  CHECK(io::exists(io::join_path(tmp.path(), "out/src/good.cpp")));
  CHECK(io::exists(io::join_path(tmp.path(), "out/json/good.schema.json")));
  CHECK(io::exists(io::join_path(tmp.path(), "out/docs/good.md")));
  // The ECS backend's header is written for every schema, components or not, because CMake needs
  // a static output list and a build with the capability off simply never compiles it.
  CHECK(io::exists(io::join_path(tmp.path(), "out/include/schemas/good_ecs.h")));
  std::string header;
  REQUIRE(io::read_file(io::join_path(tmp.path(), "out/include/schemas/good.h"), header) ==
          io::Status::Ok);
  CHECK(header.find("struct Item") != std::string::npos);
  CHECK(header.find("k_schema_version = 2") != std::string::npos);
  CHECK(header.find("Kind::B") != std::string::npos);
}

TEST_CASE("schemac: syntax errors name the file and line") {
  TempDir tmp("schemac");
  expect_rejected(tmp, "no_namespace", "struct A { x: u8 }\n", 1, "file declares no namespace");
  expect_rejected(tmp, "anon_struct", "namespace t.a\nstruct { x: u8 }\n", 2,
                  "expected a struct name");
  expect_rejected(tmp, "bad_field", "namespace t.b\nstruct A { x u8 }\n", 2, "expected");
  expect_rejected(tmp, "zero_array", "namespace t.c\nstruct A { x: u8[0] }\n", 2,
                  "fixed array length must be positive");
  expect_rejected(tmp, "unterminated", "namespace t.d\nstruct A {\n  x: u8\n", 4, "expected");
}

TEST_CASE("schemac: resolution errors") {
  TempDir tmp("schemac");
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
  TempDir tmp("schemac");
  const Run run = compile(tmp, "imports_missing",
                          "namespace t.j\nimport \"missing.schema\"\nstruct A { x: u8 }\n");
  INFO(run.output);
  CHECK(run.exit_code == 1);
  CHECK(run.output.find("missing.schema") != std::string::npos);
  CHECK_FALSE(io::exists(io::join_path(tmp.path(), "out/include/schemas/imports_missing.h")));
}

// ADR-0028 seam 1: the ECS backend.
TEST_CASE("schemac: components register and plain records do not") {
  TempDir tmp("schemac");
  const Run run = compile(tmp, "parts",
                          "namespace t.parts\n"
                          "struct Live @kind(component) {\n"
                          "  x: f32 = 0.0\n"
                          "}\n"
                          "struct Cache @kind(component) @transient {\n"
                          "  hits: u32 = 0\n"
                          "}\n"
                          "struct Note @kind(record) {\n"
                          "  text: string\n"
                          "}\n");
  INFO(run.output);
  REQUIRE(run.exit_code == 0);

  std::string ecs;
  REQUIRE(io::read_file(io::join_path(tmp.path(), "out/include/schemas/parts_ecs.h"), ecs) ==
          io::Status::Ok);
  CHECK(ecs.find("namespace t::parts {") != std::string::npos);
  CHECK(ecs.find("void register_parts_components(::flecs::world& world)") != std::string::npos);
  CHECK(ecs.find("register_schema_component<::t::parts::Live>") != std::string::npos);
  CHECK(ecs.find("register_schema_component<::t::parts::Cache>") != std::string::npos);
  // A record is not a component and is not registered with the entity store.
  CHECK(ecs.find("Note") == std::string::npos);

  // The transient marking reaches the descriptor, so the store and the protocol see it without
  // linking an ECS.
  std::string source;
  REQUIRE(io::read_file(io::join_path(tmp.path(), "out/src/parts.cpp"), source) == io::Status::Ok);
  CHECK(source.find("engine::schema::TypeFlag::transient") != std::string::npos);
  // And into the JSON Schema and the Markdown page, which are what a reader outside C++ gets.
  std::string json;
  REQUIRE(io::read_file(io::join_path(tmp.path(), "out/json/parts.schema.json"), json) ==
          io::Status::Ok);
  CHECK(json.find("\"x-transient\": true") != std::string::npos);
  std::string docs;
  REQUIRE(io::read_file(io::join_path(tmp.path(), "out/docs/parts.md"), docs) == io::Status::Ok);
  CHECK(docs.find("Registered with the entity store as `t.parts.Live`") != std::string::npos);
  CHECK(docs.find("transient, so it is never persisted") != std::string::npos);
}

TEST_CASE("schemac: a schema with no components still gets a registration function") {
  TempDir tmp("schemac");
  const Run run =
      compile(tmp, "records", "namespace t.records\nstruct Note @kind(record) { text: string }\n");
  INFO(run.output);
  REQUIRE(run.exit_code == 0);
  std::string ecs;
  REQUIRE(io::read_file(io::join_path(tmp.path(), "out/include/schemas/records_ecs.h"), ecs) ==
          io::Status::Ok);
  // Generated unconditionally so a caller can write the call before the first component exists,
  // and so the build's output list does not depend on a schema's contents.
  CHECK(ecs.find("void register_records_components(::flecs::world& world)") != std::string::npos);
  CHECK(ecs.find("declares no @kind(component) structs") != std::string::npos);
}

TEST_CASE("schemac: a transient struct that is not a component is refused") {
  TempDir tmp("schemac");
  // Only a component is persisted as a whole, so marking anything else transient says nothing
  // and is more likely a misplaced field attribute than an intention.
  expect_rejected(tmp, "bad_transient", "namespace t.k\nstruct A @transient { x: u8 }\n", 2,
                  "is @transient but not @kind(component)");
}

// ---- materialize (docs/plan/03-data-model.md §3.4; docs/subsystems/schema.md) ----------------

namespace {

// A record type and two components every materialize case below maps between.
constexpr std::string_view k_world =
    "struct Place @kind(component) {\n"        // 2
    "  position: vec3 @unit(m)\n"              // 3
    "  heading: f32 @unit(rad)\n"              // 4
    "  label: string\n"                        // 5
    "}\n"                                      // 6
    "struct Tag @kind(component) { n: u8 }\n"  // 7
    "struct Plain { x: u8 }\n"                 // 8
    "struct Crate @kind(record) {\n"           // 9
    "  position: vec3 @unit(cm)\n"             // 10
    "  heading: f32 @unit(deg)\n"              // 11
    "  label: string\n"                        // 12
    "  seconds: f32 @unit(s)\n"                // 13
    "  plain: f32\n"                           // 14
    "}\n";                                     // 15

std::string world(std::string_view ns, std::string_view tail) {
  return "namespace " + std::string(ns) + "\n" + std::string(k_world) + std::string(tail);
}

}  // namespace

TEST_CASE("schemac: a materialize declaration compiles to a mapping table") {
  TempDir tmp("schemac");
  const Run run = compile(tmp, "mapped",
                          world("t.map",
                                "/// Crates are placed and tagged.\n"
                                "materialize Crate @tiers(0, 2) {\n"
                                "  Place.position = position @writeback\n"
                                "  Place.heading = heading\n"
                                "  Place.label = label\n"
                                "  Tag\n"
                                "  parent = ChildOf\n"
                                "}\n"));
  INFO(run.output);
  REQUIRE(run.exit_code == 0);
  std::string source;
  REQUIRE(io::read_file(io::join_path(tmp.path(), "out/src/mapped.cpp"), source) == io::Status::Ok);
  CHECK(source.find("#include <core/schema/materialize.h>") != std::string::npos);
  CHECK(source.find("constexpr engine::schema::MaterializeInfo Crate_materialize{") !=
        std::string::npos);
  // cm -> m is a scale of 0.01; deg -> rad of pi/180; the string is a plain copy.
  CHECK(source.find("\"position\", \"position\", 0.01, 0.0") != std::string::npos);
  CHECK(source.find("\"heading\", \"heading\", 0.017453292519943295, 0.0") != std::string::npos);
  CHECK(source.find("MaterializeFlag::writeback") != std::string::npos);
  CHECK(source.find("\"label\", \"label\", 1.0, 0.0, static_cast<engine::u16>(0)") !=
        std::string::npos);
  CHECK(source.find("MaterializeParent::ChildOf") != std::string::npos);
  CHECK(source.find(".tiers = 5,") != std::string::npos);
  CHECK(source.find("&::t::map::schema_detail::Tag_info") != std::string::npos);
  CHECK(source.find("MaterializeRegistrar k_materialize_registrar") != std::string::npos);
  std::string docs;
  REQUIRE(io::read_file(io::join_path(tmp.path(), "out/docs/mapped.md"), docs) == io::Status::Ok);
  CHECK(docs.find("## materialize `t.map.Crate`") != std::string::npos);
  CHECK(docs.find("unit cm") != std::string::npos);
}

TEST_CASE("schemac: a mapping to an undeclared component is refused") {
  TempDir tmp("schemac");
  expect_rejected(tmp, "no_component", world("t.m1", "materialize Crate {\n  Nope.x = label\n}\n"),
                  17, "'Nope' is not a declared component");
  expect_rejected(tmp, "not_component",
                  world("t.m2", "materialize Crate {\n  Plain.x = plain\n}\n"), 17,
                  "'t.m2.Plain' is not a component (@kind(component))");
  expect_rejected(tmp, "bare_unknown", world("t.m3", "materialize Crate {\n  Ghost\n}\n"), 17,
                  "'Ghost' is not a declared component");
}

TEST_CASE("schemac: materialize rows are checked field by field") {
  TempDir tmp("schemac");
  expect_rejected(tmp, "no_record", world("t.r1", "materialize Nothing {\n}\n"), 16,
                  "'Nothing' is not a declared struct");
  expect_rejected(tmp, "not_record", world("t.r2", "materialize Place {\n}\n"), 16,
                  "is not a document record type (@kind(record))");
  expect_rejected(tmp, "no_field", world("t.r3", "materialize Crate {\n  Place.nope = label\n}\n"),
                  17, "component 't.r3.Place' has no field 'nope'");
  expect_rejected(tmp, "no_property",
                  world("t.r4", "materialize Crate {\n  Place.label = nope\n}\n"), 17,
                  "record type 't.r4.Crate' has no property 'nope'");
  expect_rejected(tmp, "bad_types",
                  world("t.r5", "materialize Crate {\n  Place.label = heading\n}\n"), 17,
                  "a row joins the same type");
  // Units: the dimension must agree, and a unit on one side only is ambiguous.
  expect_rejected(tmp, "bad_dimension",
                  world("t.r6", "materialize Crate {\n  Place.heading = seconds\n}\n"), 17,
                  "a unit converts only within its dimension");
  expect_rejected(tmp, "one_sided",
                  world("t.r7", "materialize Crate {\n  Place.heading = plain\n}\n"), 17,
                  "must both declare @unit, or neither");
  // Write-back compares bytes, so a string cannot carry it.
  expect_rejected(tmp, "bad_writeback",
                  world("t.r8", "materialize Crate {\n  Place.label = label @writeback\n}\n"), 17,
                  "@writeback needs a field that is its own bytes");
  expect_rejected(
      tmp, "twice",
      world("t.r9", "materialize Crate {\n  Place.label = label\n  Place.label = label\n}\n"), 18,
      "'t.r9.Place.label' is mapped twice");
  expect_rejected(tmp, "bad_parent", world("t.ra", "materialize Crate {\n  parent = Owner\n}\n"),
                  17, "parent is ChildOf or none");
  expect_rejected(tmp, "dup_mapping",
                  world("t.rb", "materialize Crate {\n}\nmaterialize Crate {\n}\n"), 18,
                  "a record type has one materialize declaration");
  expect_rejected(tmp, "bad_unit", "namespace t.rc\nstruct A { x: f32 @unit(furlong) }\n", 2,
                  "unknown unit 'furlong'");
  expect_rejected(tmp, "unit_on_text", "namespace t.rd\nstruct A { x: string @unit(m) }\n", 2,
                  "has no number to measure");
}

TEST_CASE("schemac: usage errors exit 2") {
  platform::Process p;
  const std::string_view argv[] = {schemac_exe()};
  REQUIRE(p.spawn(argv, nullptr, /*merge_stderr=*/true));
  std::string output;
  p.read_all(output);
  CHECK(p.wait() == 2);
  CHECK(output.find("usage:") != std::string::npos);
}

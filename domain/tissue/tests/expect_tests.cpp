// The fixture mode (docs/subsystems/tissue.md, "The fixture mode"), against a small synthetic
// ten-node reference body that fails two rows on purpose: its cover (a warning, the slab's own
// shape) and a landmark declared one ulp off the observation (an error). Declared exactly, the
// report matches; a failure left undeclared, a declared one that no longer fails, a severity or a
// value that moved, and a skipped row nobody declared are each a difference.
#include "fixture.h"

#include <core/json/json.h>
#include <domain/tissue/expect.h>
#include <domain/tissue/validate.h>

#include <doctest/doctest.h>

#include <cstring>
#include <string>

using namespace engine;
using namespace engine::tissue;

namespace {

// The ten-node slab, with the apex landmark's declared position one ulp from the observation's.
TissueFile fixture_body() {
  SyntheticOptions options;
  options.quadratic = true;
  TissueFile file = make_synthetic_tissue(options).file;
  Landmark& apex = file.definition.observation.landmarks[0];
  Vec3 p = *apex.position;
  u32 bits = 0;
  std::memcpy(&bits, &p.x, 4);
  ++bits;
  std::memcpy(&p.x, &bits, 4);
  apex.position = p;
  return file;
}

TissueReport validated(const TissueFile& file) {
  TissueReport report;
  validate_tissue(file, ValidateOptions{}, report);
  return report;
}

// The fixture's report, every row and every mode, validated once for the tests that compare it.
const TissueReport& fixture_report() {
  static const TissueReport report = validated(fixture_body());
  return report;
}

// The declaration as the authoring side writes it: format, rule, and the failures.
const char* k_declared = R"({
  "format": "astra.tissue.expected-failures.v1",
  "match": "exact multiset of id, subject, severity; no missing or additional failures",
  "engine_binary_sha256": "",
  "report_sha256": "",
  "failures": [
    {"id": "observation.landmarks", "subject": "observation of synthetic-plane-41x41",
     "severity": "error", "witness": "landmark apex is not the observation's position at id 840"},
    {"id": "cover.range", "subject": "region slab", "severity": "warning",
     "witness": "dense roof vertex 0"}
  ],
  "note": "the synthetic fixture's two failures, on purpose"
})";

TissueExpectation declared(const char* text = k_declared) {
  TissueExpectation out;
  std::string error;
  const bool ok = parse_expectation(text, out, &error);
  INFO(error);
  REQUIRE(ok);
  return out;
}

bool has(const ExpectationResult& r, ExpectationDifference::Kind kind, std::string_view id) {
  for (const ExpectationDifference& d : r.differences)
    if (d.kind == kind && d.id == id) return true;
  return false;
}

}  // namespace

TEST_CASE("expect: the fixture fails two rows on purpose, and declared exactly, it matches") {
  const TissueReport& report = fixture_report();
  // validate alone would exit 1 on it: one error row fails.
  CHECK(report.errors == 1);
  CHECK(report.warnings == 1);
  ExpectationResult result;
  compare_expectation(report, declared(), result);
  INFO(expectation_text(result));
  CHECK(result.matched());
  CHECK(result.declared == 2);
  CHECK(result.outcomes == 2);
}

TEST_CASE("expect: an undeclared failure and a declared row that passes are both differences") {
  const TissueReport& report = fixture_report();
  // The cover dropped from the declaration: a regression, as far as the fixture knows.
  TissueExpectation e = declared();
  e.failures.pop_back();
  ExpectationResult result;
  compare_expectation(report, e, result);
  CHECK_FALSE(result.matched());
  CHECK(has(result, ExpectationDifference::Kind::unexpected, "cover.range"));
  CHECK(result.differences.size() == 1);

  // A row declared that passes (fixed upstream, or a role that reports it): the declaration is out
  // of date, and the difference says the row's outcome.
  e = declared();
  ExpectedFailure size;
  size.id = "region.cage_size";
  size.subject = "region slab";
  size.severity = "error";
  size.witness = "845 nodes, past the 800 limit";
  e.failures.push_back(size);
  compare_expectation(report, e, result);
  CHECK_FALSE(result.matched());
  REQUIRE(result.differences.size() == 1);
  CHECK(result.differences[0].kind == ExpectationDifference::Kind::missing);
  CHECK(result.differences[0].actual == "info as info");
  CHECK(result.differences[0].detail.find("845 nodes") != std::string::npos);

  // A row that does not exist at all.
  e = declared();
  e.failures[1].subject = "region elsewhere";
  compare_expectation(report, e, result);
  CHECK(has(result, ExpectationDifference::Kind::missing, "cover.range"));
  CHECK(has(result, ExpectationDifference::Kind::unexpected, "cover.range"));
}

TEST_CASE("expect: the severity is part of the match") {
  const TissueReport& report = fixture_report();
  TissueExpectation e = declared();
  e.failures[1].severity = "error";  // the cover declared an error; it fails as a warning
  ExpectationResult result;
  compare_expectation(report, e, result);
  CHECK_FALSE(result.matched());
  CHECK(has(result, ExpectationDifference::Kind::missing, "cover.range"));
  CHECK(has(result, ExpectationDifference::Kind::unexpected, "cover.range"));
}

TEST_CASE("expect: a declared value is held to its tolerance, and the witness never is") {
  const TissueReport& report = fixture_report();
  const ValidationRow* cover = nullptr;
  for (const ValidationRow& r : report.rows)
    if (r.id == "cover.range") cover = &r;
  REQUIRE(cover != nullptr);
  f64 fraction = 0.0;
  REQUIRE(cover->value.find("area_fraction_in_range")->get_f64(fraction));

  TissueExpectation e = declared();
  JsonValue pattern = JsonValue::object();
  pattern.set("area_fraction_in_range", JsonValue(fraction + 0.004));
  e.failures[1].value = pattern;
  e.failures[1].tolerance = 0.005;
  e.failures[1].witness = "anything at all";
  ExpectationResult result;
  compare_expectation(report, e, result);
  INFO(expectation_text(result));
  CHECK(result.matched());

  e.failures[1].tolerance = 0.001;
  compare_expectation(report, e, result);
  REQUIRE(result.differences.size() == 1);
  CHECK(result.differences[0].kind == ExpectationDifference::Kind::value);
  CHECK(result.differences[0].detail.find("area_fraction_in_range") != std::string::npos);

  // A key the value does not have.
  pattern = JsonValue::object();
  pattern.set("no_such_number", JsonValue(1.0));
  e.failures[1].value = pattern;
  compare_expectation(report, e, result);
  CHECK(has(result, ExpectationDifference::Kind::value, "cover.range"));
}

TEST_CASE("expect: a skipped row is an outcome the fixture has to declare") {
  // Without its rest the energy has nothing to be measured from: three rows cannot run.
  TissueFile file = fixture_body();
  Vector<RegionState>& states = file.definition.states;
  for (u32 i = 0; i < states.size(); ++i)
    if (states[i].role == StateRole::Rest) states.erase(states.begin() + i);
  const TissueReport report = validated(file);
  ExpectationResult result;
  compare_expectation(report, declared(), result);
  CHECK_FALSE(result.matched());
  u32 skipped = 0;
  for (const ExpectationDifference& d : result.differences)
    if (d.kind == ExpectationDifference::Kind::unexpected && d.actual.rfind("skipped", 0) == 0)
      ++skipped;
  CHECK(skipped >= 2);

  // The declaration a first run writes matches that run, skipped rows and all, and reads back.
  const TissueExpectation written = expectation_from_report(report, "a first run");
  const std::string text = write_expectation(written);
  CHECK(text.find("\"Skipped\"") != std::string::npos);
  TissueExpectation reread;
  std::string error;
  REQUIRE(parse_expectation(text, reread, &error));
  compare_expectation(report, reread, result);
  INFO(expectation_text(result));
  CHECK(result.matched());
}

TEST_CASE("expect: a declaration of failures alone is written exactly as v1 writes it") {
  const TissueReport& report = fixture_report();
  const std::string text = write_expectation(expectation_from_report(report, "the fixture"));
  JsonValue json;
  REQUIRE(parse_json(text, json).ok);
  const JsonValue* failures = json.find("failures");
  REQUIRE(failures != nullptr);
  REQUIRE(failures->size() == 2);
  for (usize i = 0; i < failures->size(); ++i) {
    const JsonValue& f = (*failures)[i];
    CHECK(f.size() == 4);  // id, subject, severity, witness: nothing the engine added
    CHECK(f.find("severity") != nullptr);
    CHECK(f.find("verdict") == nullptr);
  }
  std::string_view format;
  REQUIRE(json.find("format")->get_string(format));
  CHECK(format == k_expect_format);
}

TEST_CASE("expect: a declaration that is not one is refused, never read as empty") {
  TissueExpectation e;
  std::string error;
  CHECK_FALSE(parse_expectation("{", e, &error));
  CHECK_FALSE(
      parse_expectation(R"({"format": "engine.tissue.expect.v0", "failures": []})", e, &error));
  CHECK(error.find("format") != std::string::npos);
  CHECK_FALSE(parse_expectation(
      R"({"format": "astra.tissue.expected-failures.v1", "failures": [], "extra": 1})", e, &error));
  CHECK_FALSE(parse_expectation(R"({"format": "astra.tissue.expected-failures.v1",
      "failures": [{"id": "cover.range", "subject": "region slab", "severity": "fatal"}]})",
                                e, &error));
  CHECK(error.find("severity") != std::string::npos);
  CHECK_FALSE(parse_expectation(R"({"format": "astra.tissue.expected-failures.v1",
      "failures": [{"id": "", "subject": "region slab", "severity": "error"}]})",
                                e, &error));
  CHECK_FALSE(parse_expectation(R"({"format": "astra.tissue.expected-failures.v1",
      "failures": [{"id": "a", "subject": "b", "severity": "error", "tolerance": -1}]})",
                                e, &error));
  CHECK(parse_expectation(R"({"format": "astra.tissue.expected-failures.v1", "failures": []})", e,
                          &error));
}

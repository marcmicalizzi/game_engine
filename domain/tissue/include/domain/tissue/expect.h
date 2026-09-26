#pragma once

// The fixture mode (docs/subsystems/tissue.md, "The fixture mode"): a validation report compared
// with what a fixture declares about it.
//
// **A regression fixture is accepted when its outcome is the declared one, not when it passes.**
// A certified reference body the authoring side publishes as a fixture can fail rows for reasons
// the engine knows and has recorded — an audit row still open upstream, a limit a reference body is
// not held to — and "exit 0" would hide both a fix upstream and a regression here behind the same
// status. So a fixture carries a `TissueExpectation` (schemas/tissue.schema; the authoring side's
// "astra.tissue.expected-failures.v1", adopted as published): the rows it is known not to pass, by
// id, subject and severity, each failing or skipped, optionally with part of its value and a
// tolerance. The comparison is exact on the multiset: every row of the report that fails or is
// skipped, whatever its severity, has to be declared, and every declared row has to fail or be
// skipped as declared, with the declared severity. A declared row that now passes is a difference
// (fixed upstream: update the declaration) and so is an undeclared failure (a regression); both are
// listed, with the witnesses.
//
// A skipped row counts because the plan's contract is that every row is evaluated: a row that could
// not run is a row the fixture says nothing about unless it declares so. The witness is recorded
// and never matched, because it names a node or a number and moves with any change in how the
// engine walks the data.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <domain/tissue/validate.h>

#include <schemas/tissue.h>
#include <string>
#include <string_view>

namespace engine::tissue {

// The expectation's `format` string this build reads and writes: the authoring side's.
inline constexpr const char* k_expect_format = "astra.tissue.expected-failures.v1";
// Its rule, in the words the authoring side published it with; `--write-expect` writes it.
inline constexpr const char* k_expect_rule =
    "exact multiset of id, subject, severity; no missing or additional failures";

// Reads an expectation from JSON text or a file: refuses what is not a TissueExpectation (unknown
// fields included: the file is written for this format), a format other than `k_expect_format`, a
// failure without an id or a subject, a severity other than "error", "warning" or "info", and a
// negative or non-finite tolerance. A failure declared twice stands for two rows of that id,
// subject and severity: the comparison matches each declared failure once.
bool parse_expectation(std::string_view json, TissueExpectation& out, std::string* error = nullptr);
bool read_expectation(std::string_view path, TissueExpectation& out, std::string* error = nullptr);

// One way a report and an expectation disagree.
struct ExpectationDifference {
  enum class Kind : u8 {
    missing,     // declared, and no row of that id, subject, severity and outcome is left for it
    unexpected,  // the row fails or is skipped and nothing declares it
    value,       // declared and failing as declared, and its value does not match the pattern
  };
  Kind kind = Kind::missing;
  std::string id;
  std::string subject;
  std::string severity;  // the declared one for a missing row, the row's otherwise
  std::string declared;  // "fail" or "skipped"; empty for an unexpected row
  std::string actual;    // the row's verdict and severity ("pass", "fail as warning"), or "absent"
  std::string detail;    // the row's witness, the declared one, or where the value departs
};

struct ExpectationResult {
  u32 declared = 0;  // failures the expectation lists
  u32 outcomes = 0;  // rows of the report that fail or are skipped
  Vector<ExpectationDifference> differences;
  bool matched() const noexcept { return differences.empty(); }
};

void compare_expectation(const TissueReport& report, const TissueExpectation& expectation,
                         ExpectationResult& out);

const char* difference_kind_name(ExpectationDifference::Kind kind) noexcept;
// {"matched", "declared", "outcomes", "differences": [{"kind", "id", "subject", ...}]}.
JsonValue expectation_json(const ExpectationResult& result);
// One sentence per difference, for a terminal.
std::string expectation_text(const ExpectationResult& result);

// The expectation this report would match: every row that fails or is skipped, with its severity
// and witness, no value. A starting point to review, never a declaration to commit unread.
TissueExpectation expectation_from_report(const TissueReport& report, std::string note);
// An expectation as the JSON a file holds, pretty, with a newline at the end. The engine's four
// additions are written only where they differ from their defaults, so a declaration of failures
// alone is exactly what the authoring side's v1 writer produces.
std::string write_expectation(const TissueExpectation& expectation);

}  // namespace engine::tissue

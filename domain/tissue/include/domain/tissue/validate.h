#pragma once

// The tissue definition's validators and its report (docs/subsystems/tissue.md, "Validators";
// docs/plan/05-simulation.md §5.16, the 2026-09-23 addendum; docs/plan/07-content-pipeline.md
// §7.11).
//
// **Every validator is a row with a stated threshold and a witness.** A row says what it checked,
// the number it measured, the threshold it held that number to, and what to look at when it fails —
// a node, a vertex id, a triangle pair — so that a failure is a place and not a mood. Rows have a
// severity: an `error` row is a definition that is wrong (a node inside its frame, a reversed
// image, the observation not bitwise the base outside its domain), a `warning` row is a quality
// gate the plan states a threshold for (a cage wider than one solve group, the cover outside its
// declared range on too much of the roof), and an `info` row is a number the review asks to see
// reported and never gated (the four volumes, the transfer decomposition, the projection
// diagnostic, the acceptance record). `engine-content tissue validate` exits 1 when any error row
// fails.
//
// **The report is the numbers the rows stand on**, grouped by region, frame, binding and state, in
// millimetres and millilitres, because that is how the reviews state them: `engine-content tissue
// report` prints it as one JSON line.
//
// Nothing here simulates. The states are node positions the authoring side solved; the engine
// binds, transfers, measures and checks them. The first runtime solver is a later capability.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <domain/tissue/tissue_file.h>

#include <string>

namespace engine::tissue {

enum class Severity : u8 { error, warning, info };
enum class Verdict : u8 { pass, fail, info, skipped };

const char* severity_name(Severity severity) noexcept;
const char* verdict_name(Verdict verdict) noexcept;

struct ValidationRow {
  std::string id;       // "frame.containment": what kind of check
  std::string subject;  // "frame thorax, region left, state supine": what it was run on
  Severity severity = Severity::info;
  Verdict verdict = Verdict::info;
  std::string threshold;  // the stated threshold, in words and numbers
  JsonValue value;        // what was measured
  std::string witness;    // where to look
  std::string note;       // why the row exists, in a sentence
};

struct ValidateOptions {
  // The most witnesses a row lists.
  u32 max_witnesses = 8;
  // Also transfer every binding's records under the other two normal modes and report how far the
  // modes are apart (the round-five closure's conformance comparison).
  bool compare_modes = true;
};

struct TissueReport {
  Vector<ValidationRow> rows;
  JsonValue numbers;  // the report, by region, frame, binding and state
  u32 errors = 0;     // error rows that failed
  u32 warnings = 0;   // warning rows that failed
};

// Runs every validator over a definition read from an interchange or a container.
void validate_tissue(const TissueFile& file, const ValidateOptions& options, TissueReport& out);

// {"errors", "warnings", "rows": [...]} and, with the numbers, {"rows", "numbers", ...}.
JsonValue rows_json(const TissueReport& report);
JsonValue report_json(const TissueReport& report);

}  // namespace engine::tissue

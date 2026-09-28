#pragma once

// What this build of the tissue module reads and evaluates, and the gate a file's requirements go
// through before anything else is read (docs/subsystems/tissue.md, "Capabilities").
//
// **A reader that skips what it does not know can misread a newer file.** The container's rule is
// to skip an unknown section and ignore an unknown field with a warning, so that a newer file
// still loads; that is right for data a region can fall back from, and wrong for a field that
// changes the meaning of one it does know — an older build reading an essential attachment reads
// a stiff spring, the version-1 meaning of `Fixed`. So a file names what its meaning depends on
// (`TissueDefinition.requirements`, the file's `requires` list: records and fields, block kinds,
// validator rows, laws), and a build that lacks any of it refuses the file with a **capability
// failure**: its own exit code and sentence, never a validation result, and never matched against a
// fixture's declared failures — a missing capability is not an expected physical failure.
//
// **The table here is the one list.** `engine-content tissue capabilities` prints it: the schema
// version, every record, field, enumeration and block kind this build parses (from the generated
// schema reflection, so it cannot drift from what the reader reads), every validator row with
// whether it is *evaluated*, *info-only* (it reports and never gates: what the engine cannot
// evaluate it names the authority for) or *not implemented*, and the laws it evaluates. The tests
// check that every row the validator emits is in the table with the status it claims.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>

#include <schemas/tissue.h>
#include <span>
#include <string>
#include <string_view>

namespace engine::tissue {

enum class RowStatus : u8 { evaluated, info_only, not_implemented };
// "evaluated", "info-only", "not-implemented".
const char* row_status_name(RowStatus status) noexcept;

struct RowCapability {
  const char* id;
  RowStatus status;
};

// Every validator row id this build knows, emitted or not, with its status, grouped as a report
// emits them.
std::span<const RowCapability> row_capabilities() noexcept;

// What a build can read. `build_capabilities()` is this one's; a test strips names from a copy to
// stand in for an older build.
struct Capabilities {
  u32 schema_version = 0;     // TissueDefinition's @version
  u32 container_version = 0;  // the `.tissue` header's version
  // Schema names a file may require, sorted: every record reachable from TissueDefinition and the
  // laws' coefficient records, "Record.field" for each of their fields, every enumeration, and
  // "Enum.Value" for each enumerator.
  Vector<std::string> records;
  // Block kinds an interchange carries (10 and up), by name, in kind order.
  Vector<std::string> blocks;
  // Every row, with its status.
  Vector<std::string> row_ids;
  Vector<RowStatus> row_status;
  // Laws by spelling, grouped by what they are the law of: "rest_driver", "thickness",
  // "surface_evaluation", "normal_mode", "refinement_rule", "energy".
  Vector<std::string> law_families;
  Vector<Vector<std::string>> laws;

  bool has_record(std::string_view name) const noexcept;
  bool has_block(std::string_view name) const noexcept;
  bool has_law(std::string_view name) const noexcept;
  // A row's status; `not_implemented` for an id this build does not know.
  RowStatus row(std::string_view id) const noexcept;
  // Removes a name from whichever list holds it (a record, a block, a row, a law); false when none
  // did. For the tests that stand in for an older build.
  bool strip(std::string_view name);
};

const Capabilities& build_capabilities();

// Why a file cannot be read with its meaning: what it requires and this build lacks.
struct CapabilityFailure {
  Vector<std::string> missing;  // "record ContactPair", "row attachment.reaction_balance (...)"
  std::string sentence;
  bool failed() const noexcept { return !missing.empty(); }
};

// Checks a definition's requirements, as JSON — before it is read as a TissueDefinition, so that a
// file this build could not read at all still fails as a capability and not as a parse error.
// Absent or null is no requirement. Refuses requirements that are not an object of string arrays,
// or that name a kind of requirement this build does not know (the gate fails closed). Returns
// false and fills `out` when anything is missing.
bool check_requirements_json(const JsonValue& definition, const Capabilities& capabilities,
                             CapabilityFailure& out);
bool check_requirements(const Requirements& requirements, const Capabilities& capabilities,
                        CapabilityFailure& out);

// The capability line: {"format", "schema_version", "container_version", "records": {name:
// version}, "fields": [...], "enums": {name: [values]}, "blocks": [...], "rows": {id: status},
// "laws": {family: [...]}, "requirements": ["records", "blocks", "rows", "laws"]}.
JsonValue capabilities_json(const Capabilities& capabilities);

}  // namespace engine::tissue

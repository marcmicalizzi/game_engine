// The capability gate (docs/subsystems/tissue.md, "Capabilities"; capabilities.h): what this build
// reads and evaluates, one table; a file's requirements checked against it before anything else is
// read; a file that requires what a build lacks refused as a capability failure, never as damage
// and never as a validation result — in both directions: an old file in this build reads with its
// old meaning, and a new file in an older build (simulated by stripping capabilities from a copy of
// this one's: one from before the layered model, one from before the separated tie) is refused,
// not misread.
#include "fixture.h"

#include <core/json/json.h>
#include <domain/tissue/cage.h>
#include <domain/tissue/capabilities.h>
#include <domain/tissue/synthetic.h>
#include <domain/tissue/tissue_file.h>
#include <domain/tissue/validate.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>

using namespace engine;
using namespace engine::tissue;

namespace {

bool contains(const Vector<std::string>& list, std::string_view s) {
  for (const std::string& x : list)
    if (x == s) return true;
  return false;
}

bool mentions(const CapabilityFailure& f, std::string_view s) {
  for (const std::string& m : f.missing)
    if (m.find(s) != std::string::npos) return true;
  return false;
}

Vector<u8> encoded(const TissueFile& file) {
  Vector<u8> bytes;
  std::string error;
  const bool ok = encode_tissue_file(file, bytes, &error);
  INFO(error);
  REQUIRE(ok);
  return bytes;
}

// A build that predates the layered model: every record, field and enumerator added for it, its
// two block kinds and its rows stripped from a copy of this one's capabilities.
Capabilities older_build() {
  Capabilities c = build_capabilities();
  for (const char* name : {"ParameterDomain",
                           "ParameterSample",
                           "ActiveRestShape",
                           "Region.rest_driver",
                           "FrameState",
                           "Attachment.enforcement",
                           "AttachmentEnforcement",
                           "AttachmentEnforcement.Essential",
                           "Attachment.interface",
                           "Attachment.gap_m",
                           "AttachmentInterface",
                           "AttachmentInterface.Coincident",
                           "AttachmentInterface.Separated",
                           "ThicknessField",
                           "DepotPartition",
                           "MaterialBoundarySurface",
                           "MaterialSkin",
                           "ContactPair",
                           "ContactPolicy",
                           "ReferenceCertificate",
                           "BoundaryFaceRefs",
                           "MaterialSurfaceCoordinates",
                           "fusiform-arch-v1",
                           "uniform-v1",
                           "cosine-modulation-v1",
                           "p2-six-node-v1"})
    c.strip(name);
  for (const RowCapability& r : row_capabilities()) {
    const std::string_view id = r.id;
    for (const char* prefix :
         {"parameters.", "state.parameters", "muscle.", "frame.rigidity", "attachment.", "contact.",
          "surface.", "fat.", "skin.", "reference.", "definition.requirements"})
      if (id.rfind(prefix, 0) == 0) {
        c.strip(id);
        break;
      }
  }
  return c;
}

// A build from after the layered model and before the separated tie (Attachment version 2): this
// one's capabilities with only the tie's field, enumeration and row stripped. Its reader would skip
// `interface` and `gap_m` as unknown fields and read a separated grip as a coincident one, which
// is exactly what the gate is for.
Capabilities build_before_the_tie() {
  Capabilities c = build_capabilities();
  for (const char* name :
       {"Attachment.interface", "Attachment.gap_m", "AttachmentInterface",
        "AttachmentInterface.Coincident", "AttachmentInterface.Separated", "attachment.target_gap"})
    REQUIRE(c.strip(name));
  return c;
}

}  // namespace

TEST_CASE("capabilities: every record, block, row and law this build has, as one table") {
  const Capabilities& c = build_capabilities();
  CHECK(c.schema_version == TissueDefinition::k_schema_version);
  CHECK(c.schema_version == 2);
  CHECK(c.container_version == k_tissue_file_version);
  // Records and their fields come from the schema's reflection: the reader's own view.
  for (const char* name : {"TissueDefinition",
                           "Region",
                           "Region.rest_driver",
                           "Region.role",
                           "Attachment.enforcement",
                           "AttachmentEnforcement.Essential",
                           "CageKind.TetrahedralQuadratic",
                           "ContactPair",
                           "ContactPolicy.locality_m",
                           "MaterialBoundarySurface",
                           "MaterialSkin.coincident",
                           "ThicknessField",
                           "DepotPartition",
                           "ParameterSample",
                           "FrameState.rotation",
                           "ReferenceCertificate",
                           "Requirements.laws",
                           "FusiformArchCoefficients.arch_m",
                           "CosineModulation",
                           "Attachment.interface",
                           "Attachment.gap_m",
                           "AttachmentInterface",
                           "AttachmentInterface.Coincident",
                           "AttachmentInterface.Separated"})
    CHECK_MESSAGE(c.has_record(name), std::string(name));
  CHECK_FALSE(c.has_record("Muscle"));
  CHECK_FALSE(c.has_record("Fat"));
  CHECK_FALSE(c.has_record("TissueExpectation"));  // a fixture's declaration, not a record of it
  CHECK(c.has_block("BoundaryFaceRefs"));
  CHECK(c.has_block("MaterialSurfaceCoordinates"));
  CHECK(c.has_block("QuadraticTetrahedra"));
  CHECK_FALSE(c.has_block("Definition"));  // the container's own, never an interchange block
  CHECK(block_element_size(BlockKind::BoundaryFaceRefs) == 8);
  CHECK(block_element_size(BlockKind::MaterialSurfaceCoordinates) == 16);
  CHECK(c.row("contact.curved_clearance") == RowStatus::evaluated);
  CHECK(c.row("reference.certificate_provenance") == RowStatus::info_only);
  CHECK(c.row("skin.energy_transfer") == RowStatus::info_only);
  CHECK(c.row("attachment.reaction_balance") == RowStatus::not_implemented);
  CHECK(contains(c.row_ids, "attachment.reaction_balance"));
  CHECK(c.row("attachment.target_gap") == RowStatus::evaluated);
  CHECK(c.row("no.such_row") == RowStatus::not_implemented);
  CHECK(c.has_law("fusiform-arch-v1"));
  CHECK(c.has_law("limit-interpolated"));
  CHECK_FALSE(c.has_law("fusiform-arch-v2"));

  const JsonValue line = capabilities_json(c);
  CHECK(line.find("schema_version") != nullptr);
  CHECK(line.find("records")->find("Attachment") != nullptr);
  u64 version = 0;
  line.find("records")->find("Attachment")->get_u64(version);
  CHECK(version == 3);
  line.find("records")->find("Region")->get_u64(version);
  CHECK(version == 4);  // version 4: a derived cage's `derivation`
  CHECK(line.find("records")->find("CageDerivation") != nullptr);
  CHECK(line.find("enums")->find("AttachmentEnforcement")->size() == 2);
  const JsonValue* interfaces = line.find("enums")->find("AttachmentInterface");
  REQUIRE(interfaces != nullptr);
  REQUIRE(interfaces->size() == 2);
  std::string_view spelled;
  (*interfaces)[1].get_string(spelled);
  CHECK(spelled == "Separated");
  std::string_view status;
  line.find("rows")->find("attachment.reaction_balance")->get_string(status);
  CHECK(status == "not-implemented");
  line.find("rows")->find("contact.trajectory")->get_string(status);
  CHECK(status == "info-only");
  CHECK(line.find("laws")->find("thickness")->size() == 2);
  // The derived cage's rows and its method.
  line.find("rows")->find("cage.boundary_distance")->get_string(status);
  CHECK(status == "evaluated");
  line.find("rows")->find("cage.strain_energy")->get_string(status);
  CHECK(status == "info-only");
  CHECK(c.has_law("corner-collapse-v1"));
}

TEST_CASE("capabilities: every row a report emits is in the table, with the status it claims") {
  // A runtime cage derived from the synthetic ten-node body, validated with its source: the cage
  // rows (cage.h).
  SyntheticOptions quadratic;
  quadratic.quadratic = true;
  const TissueFile reference = make_synthetic_tissue(quadratic).file;
  TissueFile cage;
  CageSummary summary;
  REQUIRE(derive_cage(reference, CageOptions{}, cage, summary));
  const TissueFile files[] = {make_synthetic_tissue().file, make_layered_slab(),
                              make_layered_fusiform(), make_layered_tied_slab(), cage};
  const Capabilities& c = build_capabilities();
  Vector<std::string> emitted;
  for (const TissueFile& file : files) {
    TissueReport report;
    ValidateOptions options;
    options.compare_modes = false;
    options.cage_source = &reference;
    options.cage_source_sha256 = summary.source_sha256;
    validate_tissue(file, options, report);
    for (const ValidationRow& r : report.rows) {
      INFO(r.id);
      const RowStatus status = c.row(r.id);
      CHECK(status != RowStatus::not_implemented);
      // An info-only row reports and never gates: its severity says so.
      if (status == RowStatus::info_only) CHECK(r.severity == Severity::info);
      if (!contains(emitted, r.id)) emitted.push_back(r.id);
    }
  }
  // And every evaluated or info-only row of the layered model is reached by its three fixtures.
  for (const RowCapability& r : row_capabilities()) {
    const std::string_view id = r.id;
    const bool layered = id.rfind("parameters.", 0) == 0 || id == "state.parameters" ||
                         id.rfind("muscle.", 0) == 0 || id.rfind("fat.", 0) == 0 ||
                         id.rfind("skin.", 0) == 0 || id.rfind("surface.", 0) == 0 ||
                         id.rfind("contact.", 0) == 0 || id.rfind("attachment.", 0) == 0 ||
                         id == "frame.rigidity" || id == "reference.certificate_provenance" ||
                         id == "definition.requirements" || id.rfind("cage.", 0) == 0;
    if (!layered) continue;
    INFO(r.id);
    CHECK(contains(emitted, id) == (r.status != RowStatus::not_implemented));
  }
}

TEST_CASE("capabilities: requirements pass the gate a build has, and fail as a capability") {
  const TissueFile slab = make_layered_slab();
  const Requirements& q = slab.definition.requirements;
  CHECK_FALSE(q.records.empty());
  CapabilityFailure failure;
  CHECK(check_requirements(q, build_capabilities(), failure));
  CHECK_FALSE(failure.failed());

  Capabilities without = build_capabilities();
  REQUIRE(without.strip("ContactPair"));
  CHECK_FALSE(without.strip("ContactPair"));  // gone
  CHECK_FALSE(check_requirements(q, without, failure));
  CHECK(failure.failed());
  CHECK(failure.missing.size() == 1);
  CHECK(mentions(failure, "record ContactPair"));
  CHECK(failure.sentence.find("capability failure") != std::string::npos);

  // A row this build knows and does not implement is a capability it lacks, whatever it knows.
  Requirements rows;
  rows.rows = {"contact.pairs", "attachment.reaction_balance", "contact.everything"};
  CHECK_FALSE(check_requirements(rows, build_capabilities(), failure));
  CHECK(failure.missing.size() == 2);
  CHECK(mentions(failure, "attachment.reaction_balance (known to this build and not implemented)"));
  CHECK(mentions(failure, "contact.everything (unknown to this build)"));

  Requirements laws;
  laws.laws = {"fusiform-arch-v2"};
  CHECK_FALSE(check_requirements(laws, build_capabilities(), failure));
  CHECK(mentions(failure, "law fusiform-arch-v2"));

  // The gate fails closed: a kind of requirement this build does not know, or one it cannot read.
  JsonValue json;
  REQUIRE(parse_json(R"({"requirements": {"records": ["Region"], "solvers": ["x"]}})", json).ok);
  CHECK_FALSE(check_requirements_json(json, build_capabilities(), failure));
  CHECK(mentions(failure, "'solvers'"));
  REQUIRE(parse_json(R"({"requirements": ["Region"]})", json).ok);
  CHECK_FALSE(check_requirements_json(json, build_capabilities(), failure));
  REQUIRE(parse_json(R"({"requirements": {"records": [3]}})", json).ok);
  CHECK_FALSE(check_requirements_json(json, build_capabilities(), failure));
  // Absent and null are no requirement.
  REQUIRE(parse_json(R"({"name": "x"})", json).ok);
  CHECK(check_requirements_json(json, build_capabilities(), failure));
  REQUIRE(parse_json(R"({"requirements": null})", json).ok);
  CHECK(check_requirements_json(json, build_capabilities(), failure));
}

TEST_CASE("capabilities: an older build refuses a new file, and this build reads an old one") {
  const Capabilities older = older_build();
  // The new file, as a container and as an interchange: refused by the older build as a capability,
  // before anything of it is read, with everything it lacks named.
  const TissueFile fusiform = make_layered_fusiform();
  const Vector<u8> bytes = encoded(fusiform);
  TissueFile back;
  std::string error;
  CapabilityFailure capability;
  CHECK_FALSE(read_tissue_file_memory(bytes, back, &error, &capability, &older));
  CHECK(capability.failed());
  CHECK(error == capability.sentence);
  CHECK(mentions(capability, "record Attachment.enforcement"));
  CHECK(mentions(capability, "record AttachmentEnforcement.Essential"));
  CHECK(mentions(capability, "block BoundaryFaceRefs"));
  CHECK(mentions(capability, "row muscle.objectivity"));
  CHECK(mentions(capability, "law fusiform-arch-v1"));
  // This build reads it.
  capability = CapabilityFailure{};
  CHECK(read_tissue_file_memory(bytes, back, &error, &capability));
  CHECK_FALSE(capability.failed());
  CHECK(back.definition.attachments[0].enforcement == AttachmentEnforcement::Essential);

  engine::test::TempDir tmp("tissue_capabilities");
  REQUIRE(export_interchange(fusiform, tmp.path(), "fusiform", &error));
  const std::string json = tmp.file("fusiform.json");
  CHECK_FALSE(import_interchange(json, back, &error, &capability, &older));
  CHECK(capability.failed());
  CHECK(import_interchange(json, back, &error, &capability));

  // A build from before the separated tie refuses the tied slab, as the container and as the
  // interchange, naming the field, the enumerator and the row it lacks — where, without the gate,
  // it would skip `interface` and `gap_m` and read each grip as a coincident tie whose contact is
  // meant to be excluded. engine-content turns this refusal into exit 3 before any row is written
  // (tissue_commands.cpp, capability_failed). The same build still reads the fusiform, whose
  // coincident origin requires none of it.
  const Capabilities before_tie = build_before_the_tie();
  const TissueFile tied = make_layered_tied_slab();
  const Vector<u8> tied_bytes = encoded(tied);
  capability = CapabilityFailure{};
  CHECK_FALSE(read_tissue_file_memory(tied_bytes, back, &error, &capability, &before_tie));
  CHECK(capability.failed());
  CHECK(error == capability.sentence);
  CHECK(mentions(capability, "record Attachment.interface"));
  CHECK(mentions(capability, "record AttachmentInterface.Separated"));
  CHECK(mentions(capability, "record Attachment.gap_m"));
  CHECK(mentions(capability, "row attachment.target_gap"));
  CHECK(capability.missing.size() == 4);
  REQUIRE(export_interchange(tied, tmp.path(), "tied", &error));
  capability = CapabilityFailure{};
  CHECK_FALSE(import_interchange(tmp.file("tied.json"), back, &error, &capability, &before_tie));
  CHECK(mentions(capability, "record AttachmentInterface.Separated"));
  capability = CapabilityFailure{};
  CHECK(read_tissue_file_memory(bytes, back, &error, &capability, &before_tie));
  CHECK_FALSE(capability.failed());
  // This build reads the tie as the tie.
  CHECK(read_tissue_file_memory(tied_bytes, back, &error, &capability));
  CHECK_FALSE(capability.failed());
  CHECK(back.definition.attachments[0].interface == AttachmentInterface::Separated);
  CHECK(back.definition.attachments[0].gap_m == 0.0004);

  // The old file — the synthetic definition, which requires nothing — reads in the older build
  // and in this one, with the same definition: its meaning is its old one.
  const TissueFile old = make_synthetic_tissue().file;
  const Vector<u8> old_bytes = encoded(old);
  TissueFile in_older;
  TissueFile in_this;
  CHECK(read_tissue_file_memory(old_bytes, in_older, &error, &capability, &older));
  CHECK(read_tissue_file_memory(old_bytes, in_this, &error, &capability));
  CHECK(in_older.definition == in_this.definition);
  CHECK(in_this.definition.attachments[0].enforcement == AttachmentEnforcement::Spring);
  CHECK(in_this.warnings.empty());
}

TEST_CASE("capabilities: a file this build could not read at all still fails as a capability") {
  // An enumerator from the future is an error in core/schema even where unknown fields are
  // ignored; a file that requires it is refused by the gate first, as the capability it is.
  engine::test::TempDir tmp("tissue_capabilities_future");
  const TissueFile fusiform = make_layered_fusiform();
  std::string error;
  REQUIRE(export_interchange(fusiform, tmp.path(), "future", &error));
  const std::string path = tmp.file("future.json");
  std::string text;
  REQUIRE(io::read_file(path, text) == io::Status::Ok);
  JsonValue json;
  REQUIRE(parse_json(text, json).ok);
  json["attachments"][0].set("enforcement", JsonValue("Hyperelastic"));
  json["requirements"]["records"].push_back(JsonValue("AttachmentEnforcement.Hyperelastic"));
  REQUIRE(io::write_file(path, write_json(json)) == io::Status::Ok);
  TissueFile back;
  CapabilityFailure capability;
  CHECK_FALSE(import_interchange(path, back, &error, &capability));
  CHECK(capability.failed());
  CHECK(mentions(capability, "AttachmentEnforcement.Hyperelastic"));
  // Without the requirement it is what it always was: a definition this build cannot read.
  json["requirements"]["records"] = JsonValue::array();
  REQUIRE(io::write_file(path, write_json(json)) == io::Status::Ok);
  capability = CapabilityFailure{};
  CHECK_FALSE(import_interchange(path, back, &error, &capability));
  CHECK_FALSE(capability.failed());
  CHECK(error.find("not a TissueDefinition") != std::string::npos);
}

TEST_CASE("capabilities: a definition that uses nothing new is written as before it existed") {
  // The writer leaves out every field added after its record's version 1 while it holds its
  // default, so an older reader finds nothing it does not know in a file that needs nothing new.
  const TissueFile old = make_synthetic_tissue().file;
  const JsonValue written = definition_json(old.definition);
  const std::string json = write_json(written);
  for (const char* key :
       {"\"requirements\"", "\"parameter_domains\"", "\"contact_pairs\"", "\"enforcement\"",
        "\"rest_driver\"", "\"patch\"", "\"interface\"", "\"gap_m\""})
    CHECK_MESSAGE(json.find(key) == std::string::npos, std::string(key));
  // A runtime region's `role` (Region version 2) is its default and is left out; the ten-node
  // synthetic says it, because a reference body's is not.
  CHECK((*written.find("regions"))[0].find("role") == nullptr);
  SyntheticOptions quadratic;
  quadratic.quadratic = true;
  const JsonValue reference = definition_json(make_synthetic_tissue(quadratic).file.definition);
  CHECK((*reference.find("regions"))[0].find("role") != nullptr);
  // What it leaves out reads back as the default: the round trip is exact.
  const Vector<u8> bytes = encoded(old);
  TissueFile back;
  REQUIRE(read_tissue_file_memory(bytes, back));
  CHECK(back.definition == old.definition);
  CHECK(encoded(back) == bytes);
  // A layered file says what it uses. The fusiform's coincident origin at its zero gap is
  // Attachment version 2's meaning and is written as version 2 wrote it; the tied slab's grips say
  // they are separated, and by how much.
  const std::string layered = write_json(definition_json(make_layered_fusiform().definition));
  for (const char* key : {"\"requirements\"", "\"enforcement\"", "\"rest_driver\"", "\"patch\""})
    CHECK_MESSAGE(layered.find(key) != std::string::npos, std::string(key));
  CHECK(layered.find("\"interface\"") == std::string::npos);
  CHECK(layered.find("\"gap_m\"") == std::string::npos);
  const JsonValue tied = definition_json(make_layered_tied_slab().definition);
  const JsonValue& grip = (*tied.find("attachments"))[0];
  std::string_view interface_name;
  REQUIRE(grip.find("interface") != nullptr);
  grip.find("interface")->get_string(interface_name);
  CHECK(interface_name == "Separated");
  f64 gap = 0.0;
  REQUIRE(grip.find("gap_m") != nullptr);
  grip.find("gap_m")->get_f64(gap);
  CHECK(gap == 0.0004);
}

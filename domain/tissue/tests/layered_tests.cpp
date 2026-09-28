// The layered volume model's records and rows (docs/subsystems/tissue.md, "The layered model"),
// against its two fixtures (synthetic.h, "the layered model's worked examples"): each passes every
// row clean; each has a variant that fails chosen rows on purpose, which the fixture mode matches
// against a declaration of exactly those; the round trip through the interchange and the container
// keeps the report; an essential attachment is exact and never read as a spring; and a file that
// uses none of the records gets none of the rows.
#include "fixture.h"

#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/tissue/expect.h>
#include <domain/tissue/sha256.h>
#include <domain/tissue/synthetic.h>
#include <domain/tissue/tissue_file.h>
#include <domain/tissue/validate.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::tissue;

namespace {

TissueReport validated(const TissueFile& file) {
  TissueReport report;
  ValidateOptions options;
  options.compare_modes = false;
  validate_tissue(file, options, report);
  return report;
}

const ValidationRow* find(const TissueReport& r, std::string_view id, std::string_view subject) {
  for (const ValidationRow& row : r.rows)
    if (row.id == id && row.subject == subject) return &row;
  return nullptr;
}

u32 count(const TissueReport& r, std::string_view id) {
  u32 n = 0;
  for (const ValidationRow& row : r.rows)
    n += row.id == id ? 1u : 0u;
  return n;
}

// Every row passes or reports; nothing fails and nothing is skipped.
void check_clean(const TissueReport& report) {
  CHECK(report.errors == 0);
  CHECK(report.warnings == 0);
  for (const ValidationRow& r : report.rows) {
    INFO(r.id << " [" << r.subject << "]: " << r.witness << " " << r.note);
    CHECK(r.verdict != Verdict::fail);
    CHECK(r.verdict != Verdict::skipped);
  }
}

TissueExpectation declared(const char* text) {
  TissueExpectation out;
  std::string error;
  const bool ok = parse_expectation(text, out, &error);
  INFO(error);
  REQUIRE(ok);
  return out;
}

void reseal_sample(TissueFile& file, std::string_view sample, const std::string& block) {
  for (ParameterSample& s : file.definition.parameter_samples)
    if (s.name == sample) s.snapshot_sha256 = sha256_hex(file.find(block)->bytes);
}

}  // namespace

TEST_CASE("layered: the slab passes every row clean") {
  const TissueFile slab = make_layered_slab();
  const TissueReport report = validated(slab);
  check_clean(report);
  for (const char* id :
       {"surface.material_boundary", "surface.orientation", "surface.embedding",
        "fat.thickness_positive", "fat.field_jacobian", "fat.depot_partition", "fat.mass_ledger",
        "skin.material_binding", "skin.rest_metric", "skin.energy_transfer", "contact.pairs",
        "contact.exclusions", "contact.curved_clearance", "contact.constraint_compatibility",
        "contact.trajectory", "reference.certificate_provenance", "definition.requirements"})
    CHECK_MESSAGE(count(report, id) > 0, std::string(id));
  // No observation and no Loop sheet: none of the rows that read them.
  CHECK(count(report, "observation.topology") == 0);
  CHECK(count(report, "region.sheets") == 0);
  CHECK(count(report, "region.shell_closed") == 0);
  CHECK(count(report, "region.skin_containment") == 0);
  // The certificate's hash is verified, and the row stays info: never a pass of the engine's own.
  const ValidationRow* cert =
      find(report, "reference.certificate_provenance", "certificate reference");
  REQUIRE(cert != nullptr);
  CHECK(cert->verdict == Verdict::info);
  bool verified = false;
  cert->value.find("state_verified")->get_bool(verified);
  CHECK(verified);
  // The ledger: the cells hold the field's declared volume, and the depots partition it.
  const ValidationRow* ledger = find(report, "fat.mass_ledger", "thickness field fat");
  REQUIRE(ledger != nullptr);
  f64 volume = 0.0;
  ledger->value.find("volume_ml")->get_f64(volume);
  CHECK(volume == doctest::Approx(192.0).epsilon(1e-6));
  // The geometric rows are chordal and say so.
  CHECK(find(report, "contact.curved_clearance", "contact pair skin on skin")
            ->threshold.find("chordal") != std::string::npos);
  CHECK(find(report, "surface.embedding", "surface upper")->threshold.find("chordal") !=
        std::string::npos);
}

TEST_CASE("layered: the fusiform passes every row clean") {
  const TissueReport report = validated(make_layered_fusiform());
  check_clean(report);
  for (const char* id :
       {"parameters.domain", "state.parameters", "muscle.rest_identity", "muscle.target_volume",
        "muscle.passive_reference", "muscle.objectivity", "frame.rigidity",
        "contact.frame_trajectory", "attachment.enforcement", "attachment.pose_binding",
        "attachment.contact_compatibility", "surface.material_boundary", "contact.curved_clearance",
        "definition.requirements"})
    CHECK_MESSAGE(count(report, id) > 0, std::string(id));
  // Not implemented, so never emitted.
  CHECK(count(report, "attachment.reaction_balance") == 0);
  // The law reproduces the active target to within float32, and the target keeps its volume.
  const ValidationRow* law = find(report, "muscle.objectivity", "region muscle");
  REQUIRE(law != nullptr);
  f64 departure = 1.0;
  law->value.find("samples")->find("active")->find("max_departure_from_law_m")->get_f64(departure);
  CHECK(departure < 1.0e-8);
  const ValidationRow* volume = find(report, "muscle.target_volume", "region muscle");
  f64 change = 1.0;
  volume->value.find("samples")->find("active")->find("relative_change")->get_f64(change);
  CHECK(std::fabs(change) < 1.0e-6);
  // The insertion handle turns 60 degrees, and frame containment follows it there.
  const ValidationRow* path = find(report, "contact.frame_trajectory", "frame insertion handle");
  f64 angle = 0.0;
  (*path->value.find("load_path"))[1].find("rotation_deg")->get_f64(angle);
  CHECK(angle == doctest::Approx(60.0));
  f64 gap = 0.0;
  find(report, "frame.containment", "frame insertion handle, region muscle")
      ->value.find("states")
      ->find("posed")
      ->find("min_node_distance_mm")
      ->get_f64(gap);
  CHECK(gap == doctest::Approx(0.7).epsilon(1e-4));
}

TEST_CASE("layered: the slab's variant fails the rows chosen, and its declaration matches") {
  TissueFile slab = make_layered_slab();
  // An incident exclusion that names two faces sharing no node: it masks a nonincident pair.
  const Vector<u32> upper = fixture::read<u32>(slab, "surface.upper");
  fixture::replace(slab, "exclusion.b",
                   Vector<u32>{upper[upper.size() - 2], upper[upper.size() - 1]}, 2);
  // A depot share that leaves one cell over: volume added.
  Vector<f32> right = fixture::read<f32>(slab, "depot.right");
  right[0] += 0.5f;
  fixture::replace(slab, "depot.right", right);
  // The base raised to 0.05 mm under the fat: inside the pair's 0.1 mm offset at every state.
  Vector<Vec3> base = fixture::read<Vec3>(slab, "base.vertices");
  for (Vec3& v : base)
    v.z += 0.00065f;
  fixture::replace(slab, "base.vertices", base);
  // A skin that does not declare its coincidence with the boundary.
  slab.definition.material_skins[0].coincident = false;

  const TissueReport report = validated(slab);
  CHECK(report.errors == 5);
  ExpectationResult result;
  compare_expectation(report, declared(R"({
    "format": "astra.tissue.expected-failures.v1",
    "match": "exact multiset of id, subject, severity; no missing or additional failures",
    "failures": [
      {"id": "contact.exclusions", "subject": "contact pair skin on skin", "severity": "error"},
      {"id": "fat.depot_partition", "subject": "depot partition depots", "severity": "error"},
      {"id": "fat.mass_ledger", "subject": "thickness field fat", "severity": "error"},
      {"id": "contact.curved_clearance", "subject": "contact pair fat on base",
       "severity": "error"},
      {"id": "skin.material_binding", "subject": "skin skin", "severity": "error"}
    ]
  })"),
                      result);
  INFO(expectation_text(result));
  CHECK(result.matched());
  // Each failure says where.
  CHECK(find(report, "contact.exclusions", "contact pair skin on skin")
            ->witness.find("share no node") != std::string::npos);
  CHECK(find(report, "fat.depot_partition", "depot partition depots")->witness.find("cell 0") !=
        std::string::npos);
  CHECK(find(report, "contact.curved_clearance", "contact pair fat on base")
            ->witness.find("state construction") != std::string::npos);
}

TEST_CASE("layered: the fusiform's variant fails the rows chosen, and its declaration matches") {
  TissueFile fusiform = make_layered_fusiform();
  // An active target without the transverse compensation: det G is sqrt(lambda), not one, and the
  // snapshot is not the law's. Its recorded hash follows it, so the sample itself stays sound.
  const Vector<Vec3> reference = fixture::read<Vec3>(fusiform, "sample.identity");
  Vector<Vec3> wrong;
  const f64 lambda = 1.0 - 0.2 * 0.5;
  for (const Vec3& p : reference) {
    const f64 x = static_cast<f64>(p.x);
    const f64 z = static_cast<f64>(p.z);
    const f64 s = 2.0 * x / 0.12;
    wrong.push_back(
        Vec3{static_cast<f32>(lambda * x), p.y,
             static_cast<f32>(z / std::sqrt(lambda) +
                              0.004 * std::sin(3.14159265358979 / 3.0) * (1.0 - s * s))});
  }
  fixture::replace(fusiform, "sample.active", wrong);
  reseal_sample(fusiform, "active", "sample.active");
  // An essential attachment that declares a stiffness: a spring spelled as exact.
  fusiform.definition.attachments[0].stiffness_pa = 1.0e5;
  // A frame state that scales its frame.
  for (FrameState& s : fusiform.definition.frame_states)
    if (s.state == "posed")
      for (f64& r : s.rotation)
        r *= 1.001;

  const TissueReport report = validated(fusiform);
  ExpectationResult result;
  compare_expectation(report, declared(R"({
    "format": "astra.tissue.expected-failures.v1",
    "match": "exact multiset of id, subject, severity; no missing or additional failures",
    "failures": [
      {"id": "muscle.target_volume", "subject": "region muscle", "severity": "error"},
      {"id": "muscle.objectivity", "subject": "region muscle", "severity": "error"},
      {"id": "attachment.enforcement", "subject": "attachment origin", "severity": "error"},
      {"id": "frame.rigidity", "subject": "frame insertion handle", "severity": "error"}
    ]
  })"),
                      result);
  INFO(expectation_text(result));
  CHECK(result.matched());
  CHECK(find(report, "attachment.enforcement", "attachment origin")
            ->witness.find("never read as one") != std::string::npos);
  CHECK(find(report, "muscle.target_volume", "region muscle")->witness.find("sample active") !=
        std::string::npos);
}

TEST_CASE("layered: an essential attachment is exact, and a version-1 attachment is a spring") {
  // One held node moved a micrometre in the posed state: the essential origin says so.
  TissueFile fusiform = make_layered_fusiform();
  Vector<Vec3> posed = fixture::read<Vec3>(fusiform, "state.posed");
  const Vector<u32> origin = fixture::read<u32>(fusiform, "set.origin");
  posed[origin[3]].y += 1.0e-6f;
  fixture::replace(fusiform, "state.posed", posed);
  TissueReport report = validated(fusiform);
  const ValidationRow* binding = find(report, "attachment.pose_binding", "attachment origin");
  REQUIRE(binding != nullptr);
  CHECK(binding->verdict == Verdict::fail);
  CHECK(binding->witness.find("state posed") != std::string::npos);

  // An attachment written before version 2 — no enforcement — reads as the spring it always was,
  // and gets none of the new rows.
  JsonValue json;
  REQUIRE(parse_json(R"({"name": "rim", "region": "slab", "nodes": "rim", "kind": "Fixed",
                         "target_kind": "Frame", "target": "box"})",
                     json)
              .ok);
  Attachment a;
  schema::ReadContext ctx;
  REQUIRE(schema::from_json(a, json, ctx));
  CHECK(a.enforcement == AttachmentEnforcement::Spring);
  report = validated(make_synthetic_tissue().file);
  CHECK(count(report, "attachment.enforcement") == 0);
  CHECK(count(report, "attachment.pose_binding") == 0);
}

TEST_CASE("layered: a file that uses none of the records gets none of the rows") {
  const TissueReport report = validated(make_synthetic_tissue().file);
  for (const ValidationRow& r : report.rows) {
    const std::string_view id = r.id;
    INFO(r.id);
    CHECK(id.rfind("muscle.", 0) != 0);
    CHECK(id.rfind("fat.", 0) != 0);
    CHECK(id.rfind("skin.", 0) != 0);
    CHECK(id.rfind("surface.", 0) != 0);
    CHECK(id.rfind("contact.", 0) != 0);
    CHECK(id != "definition.requirements");
    CHECK(id != "reference.certificate_provenance");
  }
}

TEST_CASE("layered: both fixtures round-trip through the interchange and the container") {
  engine::test::TempDir tmp("tissue_layered_round_trip");
  for (const TissueFile& file : {make_layered_slab(), make_layered_fusiform()}) {
    INFO(file.definition.name);
    std::string error;
    REQUIRE(export_interchange(file, tmp.path(), "fixture", &error));
    TissueFile imported;
    REQUIRE(import_interchange(tmp.file("fixture.json"), imported, &error));
    CHECK(imported.definition.material_surfaces == file.definition.material_surfaces);
    CHECK(imported.definition.contact_pairs == file.definition.contact_pairs);
    CHECK(imported.definition.requirements == file.definition.requirements);
    Vector<u8> bytes;
    REQUIRE(encode_tissue_file(imported, bytes, &error));
    TissueFile read;
    REQUIRE(read_tissue_file_memory(bytes, read, &error));
    CHECK(read.warnings.empty());
    Vector<u8> again;
    REQUIRE(encode_tissue_file(read, again, &error));
    CHECK(again == bytes);
    // The container, the interchange and the file as generated give one report.
    const std::string a = write_json(rows_json(validated(file)));
    CHECK(write_json(rows_json(validated(imported))) == a);
    CHECK(write_json(rows_json(validated(read))) == a);
  }
}

TEST_CASE("layered: a required row the data never reaches fails the requirements row") {
  TissueFile slab = make_layered_slab();
  slab.definition.requirements.rows.push_back("muscle.target_volume");   // no rest driver here
  slab.definition.requirements.blocks.push_back("QuadraticTetrahedra");  // it has those
  const TissueReport report = validated(slab);
  const ValidationRow* r = find(report, "definition.requirements", "the definition");
  REQUIRE(r != nullptr);
  CHECK(r->verdict == Verdict::fail);
  CHECK(r->witness.find("muscle.target_volume never ran") != std::string::npos);
}

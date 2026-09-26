// The ten-node cell kind and the reference role (docs/subsystems/tissue.md, "The ten-node cell" and
// "Reference bodies"), on the synthetic slab's quadratic variant: every row runs, the rows whose
// meaning is linear say they walked the subdivision, a reference body's size is a budget, and a
// cell written in another node order, a crack between cells and a folded cell are each named.
#include "fixture.h"

#include <core/json/json.h>
#include <domain/tissue/tissue_file.h>
#include <domain/tissue/validate.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::tissue;

namespace {

fixture::Built quadratic() {
  SyntheticOptions options;
  options.quadratic = true;
  return make_synthetic_tissue(options);
}

// Without the three-mode comparison, which no test here reads and which triples the transfers:
// a ten-node slab's sheets carry four times the four-node slab's control triangles.
TissueReport validated(const TissueFile& file) {
  ValidateOptions options;
  options.compare_modes = false;
  TissueReport report;
  validate_tissue(file, options, report);
  return report;
}

// The unbroken slab's full report, modes included, validated once for the tests that read it.
const TissueReport& clean_report() {
  static const TissueReport report = [] {
    TissueReport out;
    validate_tissue(quadratic().file, ValidateOptions{}, out);
    return out;
  }();
  return report;
}

const ValidationRow* find(const TissueReport& report, std::string_view id) {
  for (const ValidationRow& r : report.rows)
    if (r.id == id) return &r;
  return nullptr;
}

std::string failing(const TissueReport& report) {
  std::string out;
  for (const ValidationRow& r : report.rows)
    if (r.verdict == Verdict::fail || r.verdict == Verdict::skipped)
      out += std::string(verdict_name(r.verdict)) + " " + severity_name(r.severity) + " " + r.id +
             " [" + r.subject + "]: " + r.witness + " " + r.note + "\n";
  return out;
}

bool fails(const TissueReport& report, std::string_view id, std::string_view witness = {}) {
  for (const ValidationRow& r : report.rows)
    if (r.id == id && r.verdict == Verdict::fail &&
        (witness.empty() || r.witness.find(witness) != std::string::npos))
      return true;
  return false;
}

std::string text(const JsonValue* v) {
  std::string_view s;
  return v != nullptr && v->get_string(s) ? std::string(s) : std::string();
}

f64 number_at(const JsonValue& value, std::initializer_list<std::string_view> path) {
  const JsonValue* at = &value;
  for (const std::string_view key : path) {
    at = at->find(key);
    if (at == nullptr) return std::numeric_limits<f64>::quiet_NaN();
  }
  f64 out = 0.0;
  return at->get_f64(out) ? out : std::numeric_limits<f64>::quiet_NaN();
}

}  // namespace

TEST_CASE("quadratic: the ten-node reference slab runs every row and fails only its cover") {
  const fixture::Built built = quadratic();
  const Region& region = built.file.definition.regions[0];
  CHECK(region.cage == CageKind::TetrahedralQuadratic);
  CHECK(region.role == RegionRole::Reference);
  CHECK(built.file.find("slab.tets")->kind == BlockKind::QuadraticTetrahedra);
  CHECK(built.file.find("slab.tets")->count == 432);
  CHECK(built.file.find("slab.nodes")->count == 845);

  const TissueReport& report = clean_report();
  INFO(failing(report));
  CHECK(report.errors == 0);
  // Nothing skipped, and the one failure is the slab's cover, as for the four-node slab.
  for (const ValidationRow& r : report.rows) {
    CAPTURE(r.id);
    CHECK(r.verdict != Verdict::skipped);
    if (r.verdict == Verdict::fail) CHECK(r.id == "cover.range");
  }
  CHECK(fails(report, "cover.range"));

  // The size is a budget: reported with the verdict a runtime cage of 845 nodes would get.
  const ValidationRow* size = find(report, "region.cage_size");
  REQUIRE(size != nullptr);
  CHECK(size->severity == Severity::info);
  CHECK(size->verdict == Verdict::info);
  CHECK(text(size->value.find("verdict_as_runtime")) == "Refused");
  CHECK(text(size->value.find("role")) == "Reference");
  CHECK(number_at(size->value, {"nodes"}) == 845.0);
  CHECK(number_at(size->value, {"over_limit"}) == 45.0);

  // The ten-node rows ran natively; the linear ones name the subdivision they walked.
  const ValidationRow* edges = find(report, "region.cell_edge_nodes");
  REQUIRE(edges != nullptr);
  CHECK(edges->verdict == Verdict::pass);
  CHECK(number_at(edges->value, {"edges"}) == 698.0);
  CHECK(number_at(edges->value, {"edges_whose_cells_disagree"}) == 0.0);
  // The edge nodes sit on the torus, not on the chords: curved a little, far inside a quarter.
  const f64 construction_offset =
      number_at(edges->value, {"states", "construction", "offset_over_edge", "max"});
  MESSAGE("edge nodes off their chords by at most " << construction_offset << " of an edge");
  CHECK(construction_offset > 0.001);
  CHECK(construction_offset < 0.05);
  const ValidationRow* orientation = find(report, "region.cell_orientation");
  REQUIRE(orientation != nullptr);
  CHECK(orientation->threshold.find("Bernstein") != std::string::npos);
  for (const char* id : {"region.affine_patch", "state.equilibrium_gap"}) {
    CAPTURE(id);
    CHECK(text(find(report, id)->value.find("representation")).find("subdivision") !=
          std::string::npos);
  }
  CHECK(number_at(find(report, "state.equilibrium_gap")->value, {"cells"}) == 3456.0);
  CHECK(text(find(report, "region.skin_containment")->value.find("piecewise_linear_boundary"))
            .find("subdivision") != std::string::npos);
  CHECK(text(find(report, "region.volumetric_strain")->value.find("representation"))
            .find("ten-node") != std::string::npos);

  // The report prints the kind and the role.
  const JsonValue& numbers = *report.numbers.find("regions")->find("slab");
  CHECK(text(numbers.find("cage")) == "TetrahedralQuadratic");
  CHECK(text(numbers.find("role")) == "Reference");
  CHECK(number_at(numbers, {"nodes_per_cell"}) == 10.0);
  CHECK(number_at(numbers, {"linear_subdivision_cells"}) == 3456.0);
}

TEST_CASE("quadratic: the curved cells hold the torus section's volume far closer than chords") {
  const TissueReport& p2 = clean_report();
  const TissueReport p1 = validated(fixture::make().file);
  const auto cage = [](const TissueReport& r, const char* key) {
    return number_at(*r.numbers.find("regions")->find("slab")->find("states")->find("construction"),
                     {key});
  };
  const f64 a = fixture::k_inner;
  const f64 b = fixture::k_outer;
  const f64 exact = 2.0 * fixture::k_u *
                    (fixture::k_major * 2.0 * fixture::k_v * (b * b - a * a) / 2.0 +
                     2.0 * std::sin(fixture::k_v) * (b * b * b - a * a * a) / 3.0) *
                    1.0e6;
  const f64 quadratic_cage = cage(p2, "cage_ml");
  const f64 linear_view = cage(p2, "cage_linear_ml");
  const f64 chords = cage(p1, "cage_ml");
  MESSAGE("solid " << exact << " ml; ten-node cells " << quadratic_cage << " ml, their subdivision "
                   << linear_view << " ml, the four-node slab " << chords << " ml");
  CHECK(std::fabs(quadratic_cage - exact) < 0.1 * std::fabs(chords - exact));
  CHECK(std::fabs(quadratic_cage - exact) / exact < 1e-3);
  // The subdivision's volume lies between: its nodes are on the torus, its faces chords of it.
  CHECK(std::fabs(linear_view - exact) < std::fabs(chords - exact));
}

TEST_CASE("quadratic: a file in another node order is named, and which order it fits") {
  // Every cell written in VTK's order: nodes 8 and 9 the other way round.
  fixture::Built built = quadratic();
  Vector<u32> cells = fixture::read<u32>(built.file, "slab.tets");
  for (u32 c = 0; c < cells.size(); c += 10)
    std::swap(cells[c + 8], cells[c + 9]);
  fixture::replace(built.file, "slab.tets", cells, 10);
  TissueReport report = validated(built.file);
  CHECK(fails(report, "region.cell_edge_nodes", "VTK's"));

  // In the authoring side's sidecar order, (0,1) (0,2) (0,3) (1,2) (1,3) (2,3).
  built = quadratic();
  cells = fixture::read<u32>(built.file, "slab.tets");
  for (u32 c = 0; c < cells.size(); c += 10) {
    const u32 g[10] = {cells[c + 0], cells[c + 1], cells[c + 2], cells[c + 3], cells[c + 4],
                       cells[c + 5], cells[c + 6], cells[c + 7], cells[c + 8], cells[c + 9]};
    // Gmsh's 4..9 are 01, 12, 02, 03, 23, 13; the lexicographic order is 01, 02, 03, 12, 13, 23.
    const u32 lexicographic[10] = {g[0], g[1], g[2], g[3], g[4], g[6], g[7], g[5], g[9], g[8]};
    std::memcpy(cells.data() + c, lexicographic, sizeof(lexicographic));
  }
  fixture::replace(built.file, "slab.tets", cells, 10);
  report = validated(built.file);
  CHECK(fails(report, "region.cell_edge_nodes", "lexicographic"));
}

TEST_CASE("quadratic: as a runtime cage it is refused for its size; a folded cell is inverted") {
  // Two breaks in one body, each read from its own row, so one validation covers both.
  fixture::Built built = quadratic();
  // The same body as a runtime cage.
  built.file.definition.regions[0].role = RegionRole::Runtime;
  // In the pressed state, one edge node of an interior cell pulled along its edge past a quarter
  // of it: the quadratic edge runs backwards at one end, and det J turns negative there. The first
  // cell of the hexahedron at (2, 2) in the second layer of cells: its edge (0, 1) joins two nodes
  // of the middle layer, away from every sheet and wall.
  const Vector<u32> cells = fixture::read<u32>(built.file, "slab.tets");
  Vector<Vec3> pressed = fixture::read<Vec3>(built.file, "state.pressed");
  const u32 c = 10 * (6 * (36 + 2 * 6 + 2));
  const Vec3 a = pressed[cells[c + 0]];
  const Vec3 b = pressed[cells[c + 1]];
  pressed[cells[c + 4]] = pressed[cells[c + 4]] + (b - a) * 0.4f;
  fixture::replace(built.file, "state.pressed", pressed);
  const TissueReport report = validated(built.file);
  INFO(failing(report));

  CHECK(fails(report, "region.cage_size", "past the 800 limit"));
  CHECK(find(report, "region.cage_size")->severity == Severity::error);

  CHECK(fails(report, "region.cell_orientation", "state pressed"));
  CHECK(fails(report, "region.cell_orientation", "det J reaches"));
  // The construction is untouched, so the node order is not in question.
  CHECK(find(report, "region.cell_edge_nodes")->verdict == Verdict::pass);
}

TEST_CASE("quadratic: cells that disagree on an edge's node are a crack, and counted") {
  // One cell naming another node for one of its edges. The node chosen is the edge node of a
  // neighbouring edge of the same cell, so its geometry fails too; the counts say the rest.
  fixture::Built built = quadratic();
  Vector<u32> cells = fixture::read<u32>(built.file, "slab.tets");
  cells[4] = cells[5];
  fixture::replace(built.file, "slab.tets", cells, 10);
  const TissueReport report = validated(built.file);
  const ValidationRow* edges = find(report, "region.cell_edge_nodes");
  REQUIRE(edges != nullptr);
  CHECK(edges->verdict == Verdict::fail);
  CHECK(number_at(edges->value, {"edges_whose_cells_disagree"}) >= 1.0);
  CHECK(number_at(edges->value, {"nodes_on_two_edges"}) >= 1.0);
}

TEST_CASE("quadratic: the container and the interchange carry the ten-node kind") {
  const fixture::Built built = quadratic();
  Vector<u8> bytes;
  std::string error;
  REQUIRE(encode_tissue_file(built.file, bytes, &error));
  TissueFile read;
  REQUIRE(read_tissue_file_memory(std::span<const u8>(bytes.data(), bytes.size()), read, &error));
  CHECK(read.warnings.empty());
  CHECK(read.definition.regions[0].cage == CageKind::TetrahedralQuadratic);
  CHECK(read.definition.regions[0].role == RegionRole::Reference);
  CHECK(read.find("slab.tets")->bytes == built.file.find("slab.tets")->bytes);
  CHECK(block_element_size(BlockKind::QuadraticTetrahedra) == 40);
  CHECK(std::string(block_kind_name(38)) == "QuadraticTetrahedra");
  // A ten-node block named by a four-node cage is a resolve error, never reinterpreted.
  fixture::Built wrong = quadratic();
  wrong.file.definition.regions[0].cage = CageKind::Tetrahedral;
  const TissueReport report = validated(wrong.file);
  CHECK(fails(report, "blocks.resolve", "QuadraticTetrahedra"));
}

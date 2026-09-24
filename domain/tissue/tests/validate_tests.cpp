// The validators (docs/subsystems/tissue.md, "Validators"): the synthetic definition passes every
// error row, and each row fails, naming a witness, when the one thing it guards is broken.
#include "../src/mesh_query.h"
#include "fixture.h"

#include <core/json/json.h>
#include <domain/geometry/surface_binding.h>
#include <domain/tissue/tissue_file.h>
#include <domain/tissue/validate.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::tissue;

namespace {

TissueReport validated(const TissueFile& file) {
  TissueReport report;
  validate_tissue(file, ValidateOptions{}, report);
  return report;
}

const ValidationRow* find(const TissueReport& report, std::string_view id,
                          std::string_view subject = {}) {
  for (const ValidationRow& r : report.rows)
    if (r.id == id && (subject.empty() || r.subject.find(subject) != std::string::npos)) return &r;
  return nullptr;
}

std::string failing(const TissueReport& report) {
  std::string out;
  for (const ValidationRow& r : report.rows)
    if (r.verdict == Verdict::fail)
      out += std::string(severity_name(r.severity)) + " " + r.id + " [" + r.subject +
             "]: " + r.witness + "\n";
  return out;
}

bool fails(const TissueReport& report, std::string_view id,
           std::string_view witness_contains = {}) {
  for (const ValidationRow& r : report.rows)
    if (r.id == id && r.verdict == Verdict::fail &&
        (witness_contains.empty() || r.witness.find(witness_contains) != std::string::npos))
      return true;
  return false;
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

TEST_CASE("validate: the synthetic definition passes every error row") {
  const fixture::Built built = fixture::make();
  const TissueReport report = validated(built.file);
  INFO(failing(report));
  CHECK(report.errors == 0);
  // The rows the plan names are all there.
  for (const char* id : {"blocks.resolve",
                         "observation.topology",
                         "observation.outside_bitwise",
                         "observation.landmarks",
                         "observation.acceptance",
                         "region.cage_size",
                         "region.cell_quality",
                         "region.cell_orientation",
                         "region.materials",
                         "region.sheets",
                         "state.inverse",
                         "state.inverse_solver",
                         "region.through_thickness",
                         "region.affine_patch",
                         "region.volumetric_strain",
                         "state.equilibrium_gap",
                         "region.shell_closed",
                         "attachments.resolve",
                         "frame.closed_manifold",
                         "frame.cover",
                         "frame.containment",
                         "frame.intersections",
                         "region.skin_containment",
                         "depth.layer_order",
                         "depth.clearance",
                         "cover.range",
                         "cover.report",
                         "binding.seam_identity",
                         "binding.domain",
                         "binding.written_in_observation_domain",
                         "binding.records",
                         "binding.reference_identity",
                         "binding.image_orientation",
                         "binding.footpoint_normals",
                         "binding.mode_conformance",
                         "volume.report",
                         "volume.visible_vs_top",
                         "volume.omitted_volume"}) {
    CAPTURE(id);
    CHECK(find(report, id) != nullptr);
  }
  // The cover sits mostly above its 3 mm range on a slab this curved: a warning, with a witness.
  CHECK(fails(report, "cover.range", "dense roof vertex"));
  // Acceptance is reported and never gated.
  CHECK(find(report, "observation.acceptance")->verdict == Verdict::info);
}

TEST_CASE("validate: the report's numbers are the four volumes and an exact decomposition") {
  const fixture::Built built = fixture::make();
  const TissueReport report = validated(built.file);
  const JsonValue& pressed =
      *report.numbers.find("regions")->find("slab")->find("states")->find("pressed");
  const f64 cage = number_at(pressed, {"cage_ml"});
  const f64 shell = number_at(pressed, {"shell_ml"});
  const f64 top = number_at(pressed, {"top_swept_ml"});
  const f64 visible = number_at(pressed, {"visible_swept_ml"});
  const f64 op = number_at(pressed, {"decomposition", "operator_only_ml"});
  const f64 off = number_at(pressed, {"decomposition", "offset_only_ml"});
  const f64 full = number_at(pressed, {"decomposition", "full_ml"});
  const f64 interaction = number_at(pressed, {"decomposition", "interaction_ml"});
  MESSAGE("pressed: cage " << cage << " ml, shell " << shell << " ml, top swept " << top
                           << " ml, visible swept " << visible << " ml; operator " << op
                           << ", offset " << off << ", full " << full << ", interaction "
                           << interaction);
  CHECK(cage > 0.0);
  CHECK(shell > 0.0);
  CHECK(std::fabs(full - visible) < 1e-12);
  CHECK(std::fabs(interaction - (full - op - off)) < 1e-9);
  // Pressed 3 mm down at the centre, the top sweeps inward and the skin follows it.
  CHECK(top < 0.0);
  CHECK(visible < 0.0);
  // The reference's own sweep is zero, exactly: the top surface is the same bits.
  const JsonValue& standing =
      *report.numbers.find("regions")->find("slab")->find("states")->find("standing");
  CHECK(number_at(standing, {"top_swept_ml"}) == 0.0);
  // The cage's volume is the tetrahedra's, which for a torus section is close to the solid's:
  // (u range) * (v range) * (R (b^2 - a^2)/2 + sin-weighted third term) at the construction.
  const f64 a = fixture::k_inner;
  const f64 b = fixture::k_outer;
  const f64 exact = 2.0 * fixture::k_u *
                    (fixture::k_major * 2.0 * fixture::k_v * (b * b - a * a) / 2.0 +
                     2.0 * std::sin(fixture::k_v) * (b * b * b - a * a * a) / 3.0) *
                    1.0e6;
  const JsonValue& construction =
      *report.numbers.find("regions")->find("slab")->find("states")->find("construction");
  const f64 cage_construction = number_at(construction, {"cage_ml"});
  MESSAGE("construction cage " << cage_construction << " ml against the solid's " << exact
                               << " ml");
  CHECK(std::fabs(cage_construction - exact) / exact < 0.02);
  // The cover split adds up on one measure.
  const JsonValue& cover = *report.numbers.find("regions")->find("slab")->find("cover");
  CHECK(std::fabs(number_at(cover, {"split_residual_ml"})) < 1e-9);
}

TEST_CASE("validate: a node inside the frame is named") {
  fixture::Built built = fixture::make();
  Vector<Vec3> nodes = fixture::read<Vec3>(built.file, "slab.nodes");
  const f32 box_top = fixture::read<Vec3>(built.file, "box.vertices")[4].z;
  nodes[fixture::node(3, 3, fixture::k_layers - 1)].z = box_top - 0.01f;
  fixture::replace(built.file, "slab.nodes", nodes);
  const TissueReport report = validated(built.file);
  INFO(failing(report));
  CHECK(fails(report, "frame.containment",
              "node " + std::to_string(fixture::node(3, 3, fixture::k_layers - 1))));
  CHECK(fails(report, "frame.intersections"));
}

TEST_CASE("validate: a frame that crosses the region, or is not closed, is named") {
  fixture::Built built = fixture::make();
  Vector<Vec3> box = fixture::read<Vec3>(built.file, "box.vertices");
  box[6].z = 0.0f;  // one top corner lifted through the slab
  fixture::replace(built.file, "box.vertices", box);
  TissueReport report = validated(built.file);
  CHECK(fails(report, "frame.intersections"));

  built = fixture::make();
  Vector<u32> faces = fixture::read<u32>(built.file, "box.triangles");
  std::swap(faces[1], faces[2]);  // one triangle wound the wrong way
  fixture::replace(built.file, "box.triangles", faces, 3);
  report = validated(built.file);
  CHECK(fails(report, "frame.closed_manifold", "edge"));
}

TEST_CASE("validate: the observation must be the base bit for bit outside its domain") {
  fixture::Built built = fixture::make();
  Vector<Vec3> observed = fixture::read<Vec3>(built.file, "skin.observed");
  // Id 0 is a corner of the grid, far outside the domain: one ulp.
  u32 bits = 0;
  std::memcpy(&bits, &observed[0].x, 4);
  ++bits;
  std::memcpy(&observed[0].x, &bits, 4);
  fixture::replace(built.file, "skin.observed", observed);
  TissueReport report = validated(built.file);
  CHECK(fails(report, "observation.outside_bitwise", "id 0"));

  built = fixture::make();
  built.file.definition.observation.topology_sha256[0] ^= 1;
  report = validated(built.file);
  CHECK(fails(report, "observation.topology"));

  built = fixture::make();
  built.file.definition.observation.landmarks[0].id += 1;
  report = validated(built.file);
  CHECK(fails(report, "observation.landmarks", "apex"));
}

TEST_CASE("validate: an inverted cell, a region outside its skin, a bad material") {
  fixture::Built built = fixture::make();
  Vector<u32> tets = fixture::read<u32>(built.file, "slab.tets");
  std::swap(tets[1], tets[2]);
  fixture::replace(built.file, "slab.tets", tets, 4);
  TissueReport report = validated(built.file);
  CHECK(fails(report, "region.cell_quality", "tetrahedron 0"));
  CHECK(fails(report, "region.cell_orientation", "tetrahedron 0"));

  built = fixture::make();
  Vector<Vec3> nodes = fixture::read<Vec3>(built.file, "slab.nodes");
  nodes[fixture::node(3, 3, 0)].z = 0.01f;  // the top's centre node through the skin
  fixture::replace(built.file, "slab.nodes", nodes);
  report = validated(built.file);
  CHECK(fails(report, "region.skin_containment", "construction"));

  built = fixture::make();
  built.file.definition.regions[0].phases[0].material.shear_modulus_pa = 0.0;
  report = validated(built.file);
  CHECK(fails(report, "region.materials", "fat"));
}

TEST_CASE("validate: a reversed image and a vertex written twice are named") {
  fixture::Built built = fixture::make();
  // Swap the footpoints of two neighbours in the footprint: the triangles between them turn over.
  const Vector<u32> domain = fixture::read<u32>(built.file, "skin.binding.domain");
  const Vector<u32> footprint = fixture::read<u32>(built.file, "skin.binding.footprint");
  const u32 centre = footprint[footprint.size() / 2];
  u32 i = 0;
  u32 j = 0;
  for (u32 k = 0; k < domain.size(); ++k) {
    if (domain[k] == centre) i = k;
    if (domain[k] == centre + 1) j = k;
  }
  REQUIRE(i != j);
  Vector<u32> triangles = fixture::read<u32>(built.file, "skin.binding.triangles");
  Vector<Vec3> barycentrics = fixture::read<Vec3>(built.file, "skin.binding.barycentrics");
  std::swap(triangles[i], triangles[j]);
  std::swap(barycentrics[i], barycentrics[j]);
  fixture::replace(built.file, "skin.binding.triangles", triangles);
  fixture::replace(built.file, "skin.binding.barycentrics", barycentrics);
  TissueReport report = validated(built.file);
  CHECK(fails(report, "binding.image_orientation", "render triangle"));

  built = fixture::make();
  SkinBinding copy = built.file.definition.bindings[0];
  copy.name = "a second module over the same skin";
  built.file.definition.bindings.push_back(copy);
  report = validated(built.file);
  CHECK(fails(report, "binding.seam_identity", "id "));

  built = fixture::make();
  Vector<u32> band = fixture::read<u32>(built.file, "skin.binding.band");
  Vector<u32> fp = fixture::read<u32>(built.file, "skin.binding.footprint");
  band[0] = fp[0];
  std::sort(band.begin(), band.end());
  fixture::replace(built.file, "skin.binding.band", band);
  report = validated(built.file);
  CHECK(fails(report, "binding.domain", "both"));
}

TEST_CASE("validate: a state that claims inverse statics declares its interior prior") {
  fixture::Built built = fixture::make();
  TissueReport report = validated(built.file);
  const ValidationRow* row = find(report, "state.inverse");
  REQUIRE(row != nullptr);
  CHECK(row->verdict == Verdict::pass);
  std::string_view method;
  row->value.find("states")->find("standing")->find("method")->get_string(method);
  CHECK(method == "surface-targeted");

  built.file.definition.states[0].inverse.reset();
  report = validated(built.file);
  CHECK(fails(report, "state.inverse", "state standing"));

  built = fixture::make();
  built.file.definition.states[1].inverse->interior_extension.clear();
  report = validated(built.file);
  CHECK(fails(report, "state.inverse", "state rest"));

  built = fixture::make();
  built.file.definition.states[0].inverse->held_node_sets.push_back("nowhere");
  report = validated(built.file);
  CHECK(fails(report, "state.inverse", "nowhere"));
}

TEST_CASE("validate: an unknown normal mode is refused, never substituted") {
  fixture::Built built = fixture::make();
  for (const char* spelling : {"normalized_limit_vertex_interpolation", "limit_interpolated", ""}) {
    built.file.definition.bindings[0].normal_mode = spelling;
    const TissueReport report = validated(built.file);
    CHECK(fails(report, "blocks.resolve", "normal mode"));
    CHECK(find(report, "binding.reference_identity") == nullptr);
  }
  for (const char* spelling :
       {"triangle", "interpolated-vertex-area-weighted", "limit-interpolated"}) {
    built.file.definition.bindings[0].normal_mode = spelling;
    const TissueReport report = validated(built.file);
    INFO(failing(report));
    CHECK(report.errors == 0);
    CHECK(find(report, "binding.reference_identity")->verdict == Verdict::pass);
  }
}

TEST_CASE("validate: the transfer is compared with the authoring side's when it is given") {
  fixture::Built built = fixture::make();
  // The expected visible surface, first as the engine computes it: they agree.
  const TissueReport first = validated(built.file);
  (void)first;
  // Reconstruct the transfer the way the validator does, through the same functions.
  const Vector<u32> domain = fixture::read<u32>(built.file, "skin.binding.domain");
  const Vector<Vec3> observed = fixture::read<Vec3>(built.file, "skin.observed");
  const Vector<Vec3> pressed = fixture::read<Vec3>(built.file, "state.pressed");
  Vector<Vec3> top_ref;
  Vector<Vec3> top_now;
  for (u32 k = 0; k < fixture::k_nu * fixture::k_nv; ++k) {
    top_ref.push_back(built.reference_nodes[k]);
    top_now.push_back(pressed[k]);
  }
  geometry::BindingFrame frame;
  REQUIRE(geometry::evaluate_binding_frame(geometry::NormalMode::limit_interpolated, built.top,
                                           top_ref, top_now, frame));
  geometry::BindingFrame rest;
  REQUIRE(geometry::evaluate_binding_frame(geometry::NormalMode::limit_interpolated, built.top,
                                           top_ref, top_ref, rest));
  const Vector<u32> tris = fixture::read<u32>(built.file, "skin.binding.triangles");
  const Vector<Vec3> bary = fixture::read<Vec3>(built.file, "skin.binding.barycentrics");
  const Vector<f32> offsets = fixture::read<f32>(built.file, "skin.binding.offsets");
  const Vector<u32> footprint = fixture::read<u32>(built.file, "skin.binding.footprint");
  const Vector<u32> band = fixture::read<u32>(built.file, "skin.binding.band");
  const Vector<f32> band_weight = fixture::read<f32>(built.file, "skin.binding.band_weights");
  Vector<geometry::AuthoredFootpoint> footpoints;
  Vector<Vec3> base;
  for (u32 k = 0; k < domain.size(); ++k) {
    f32 w = 0.0f;
    if (std::binary_search(footprint.begin(), footprint.end(), domain[k])) w = 1.0f;
    const auto it = std::lower_bound(band.begin(), band.end(), domain[k]);
    if (it != band.end() && *it == domain[k]) w = band_weight[static_cast<u32>(it - band.begin())];
    footpoints.push_back(geometry::AuthoredFootpoint{tris[k], bary[k], offsets[k], w});
    base.push_back(observed[domain[k]]);
  }
  Vector<geometry::SurfaceBinding> records;
  REQUIRE(geometry::bind_from_records(geometry::NormalMode::limit_interpolated, base, footpoints,
                                      rest.view(built.top.faces), geometry::SurfaceBindOptions{},
                                      records));
  Vector<Vec3> expected(base.size());
  geometry::apply_binding(geometry::NormalMode::limit_interpolated, records, base,
                          frame.view(built.top.faces),
                          std::span<Vec3>(expected.data(), expected.size()));
  fixture::add(built.file, "expected.pressed", BlockKind::ExpectedVisible, expected);
  built.file.definition.states[2].expected_visible = "expected.pressed";
  TissueReport report = validated(built.file);
  const ValidationRow* agreement = find(report, "binding.transfer_agreement");
  REQUIRE(agreement != nullptr);
  CHECK(agreement->verdict == Verdict::pass);
  CHECK(number_at(agreement->value, {"difference_mm", "max"}) == 0.0);

  // One vertex 0.1 mm off: a warning naming it.
  expected[footpoints.size() / 2].z += 0.0001f;
  fixture::replace(built.file, "expected.pressed", expected);
  report = validated(built.file);
  CHECK(fails(report, "binding.transfer_agreement",
              "id " + std::to_string(domain[footpoints.size() / 2])));
}

TEST_CASE("validate: the modes are compared on the same records") {
  const fixture::Built built = fixture::make();
  const TissueReport report = validated(built.file);
  const ValidationRow* modes = find(report, "binding.mode_conformance", "pressed");
  REQUIRE(modes != nullptr);
  const f64 limit_vs_area = number_at(
      modes->value, {"interpolated-vertex-area-weighted vs limit-interpolated position_mm", "max"});
  const f64 triangle_vs_limit =
      number_at(modes->value, {"triangle vs limit-interpolated position_mm", "max"});
  MESSAGE("the synthetic slab pressed 3 mm: area-weighted against limit up to "
          << limit_vs_area << " mm, triangle against limit up to " << triangle_vs_limit << " mm");
  CHECK(limit_vs_area > 0.0);
  CHECK(limit_vs_area < triangle_vs_limit);
  const ValidationRow* normals = find(report, "binding.footpoint_normals");
  REQUIRE(normals != nullptr);
  // The authored normals are the engine's own limit normals here: they agree to rounding.
  INFO(write_json(normals->value));
  CHECK(number_at(normals->value, {"limit-interpolated", "against_authored_deg", "max"}) < 1e-3);
}

TEST_CASE("validate: a rest names the solver it is a rest for") {
  fixture::Built built = fixture::make();
  TissueReport report = validated(built.file);
  const ValidationRow* row = find(report, "state.inverse_solver");
  REQUIRE(row != nullptr);
  INFO(failing(report));
  CHECK(row->verdict == Verdict::pass);
  std::string_view kind;
  row->value.find("states")->find("standing")->find("kind")->get_string(kind);
  CHECK(kind == "xpbd");

  // No solver: a warning, never a refusal.
  built.file.definition.states[0].inverse->solver.reset();
  report = validated(built.file);
  CHECK(fails(report, "state.inverse_solver", "state standing"));
  CHECK(find(report, "state.inverse_solver")->severity == Severity::warning);

  // An xpbd identity without its iterations pins no fixed point: refused.
  built = fixture::make();
  built.file.definition.states[1].inverse->solver->iterations = 0;
  report = validated(built.file);
  CHECK(fails(report, "state.inverse_solver", "state rest's xpbd solver"));
  CHECK(find(report, "state.inverse_solver")->severity == Severity::error);

  // A static minimization names the gradient it stopped at.
  built = fixture::make();
  built.file.definition.states[0].inverse->solver->kind = "static-minimization";
  report = validated(built.file);
  CHECK(fails(report, "state.inverse_solver", "gradient tolerance"));
  built.file.definition.states[0].inverse->solver->gradient_tolerance_n = 1.0e-6;
  report = validated(built.file);
  CHECK(find(report, "state.inverse_solver")->verdict == Verdict::pass);
}

TEST_CASE("validate: the equilibrium gap is reported beside every stored state, never failed") {
  fixture::Built built = fixture::make();
  TissueReport report = validated(built.file);
  const ValidationRow* row = find(report, "state.equilibrium_gap");
  REQUIRE(row != nullptr);
  CHECK(row->verdict == Verdict::info);
  CHECK(row->severity == Severity::info);
  std::string_view law;
  row->value.find("law")->get_string(law);
  CHECK(law == "bulk-edge-v0");
  bool complete = true;
  row->value.find("complete")->get_bool(complete);
  CHECK_FALSE(complete);  // the slab declares a membrane and cables the law does not include yet
  CHECK(row->value.find("not_included")->size() == 2);
  // Read with the solver whose fixed point the reference is.
  std::string_view solver;
  row->value.find("solver")->find("kind")->get_string(solver);
  CHECK(solver == "xpbd");
  for (const char* state : {"standing", "rest", "pressed"}) {
    CAPTURE(state);
    const f64 max = number_at(row->value, {"states", state, "max_n"});
    const f64 rms = number_at(row->value, {"states", state, "rms_n"});
    CHECK(max >= rms);
    CHECK(number_at(row->value, {"states", state, "held_nodes"}) == 72.0);
    CHECK(number_at(row->value, {"states", state, "classes", "sliding", "nodes"}) == 25.0);
    CHECK(number_at(row->value, {"states", state, "classes", "free", "nodes"}) == 50.0);
    // The membrane covers the top layer and the cables reach the inner support nodes, so the
    // middle layer's 25 inner nodes are where the gap is complete.
    CHECK(number_at(row->value, {"states", state, "where_complete", "nodes"}) == 25.0);
  }
  // The rest carries no load and is its own rest: zero, exactly.
  CHECK(number_at(row->value, {"states", "rest", "max_n"}) == 0.0);
  CHECK(row->witness.find("state standing") != std::string::npos);

  // Make the reference its own rest, one density throughout: the elastic terms vanish and the gap
  // at a free node is its weight less the air's, exactly.
  const Vector<Vec3> standing = fixture::read<Vec3>(built.file, "state.standing");
  fixture::replace(built.file, "state.rest", standing);
  built.file.definition.regions[0].phases[1].material.density_kg_m3 =
      built.file.definition.regions[0].phases[0].material.density_kg_m3;
  report = validated(built.file);
  row = find(report, "state.equilibrium_gap");
  REQUIRE(row != nullptr);
  const Vector<Vec3> nodes = fixture::read<Vec3>(built.file, "slab.nodes");
  const Vector<u32> tets = fixture::read<u32>(built.file, "slab.tets");
  const Vector<u32> rim = fixture::read<u32>(built.file, "slab.rim");
  const Vector<u32> posterior = fixture::read<u32>(built.file, "slab.posterior");
  Vector<f64> volume(nodes.size(), 0.0);
  for (u32 t = 0; t + 3 < tets.size(); t += 4) {
    const f64 v = query::tet_volume(query::d3(nodes[tets[t]]), query::d3(nodes[tets[t + 1]]),
                                    query::d3(nodes[tets[t + 2]]), query::d3(nodes[tets[t + 3]]));
    for (u32 k = 0; k < 4; ++k)
      volume[tets[t + k]] += v / 4.0;
  }
  const f64 density = built.file.definition.regions[0].phases[0].material.density_kg_m3 -
                      built.file.definition.states[0].load.medium_density_kg_m3;
  const f64 g = static_cast<f64>(9.81f);
  f64 expected = 0.0;
  for (u32 i = 0; i < nodes.size(); ++i) {
    const bool held = std::find(rim.begin(), rim.end(), i) != rim.end();
    const bool sliding = std::find(posterior.begin(), posterior.end(), i) != posterior.end();
    if (!held && !sliding) expected = std::max(expected, density * volume[i] * g);
  }
  const f64 got = number_at(row->value, {"states", "standing", "classes", "free", "max_n"});
  MESSAGE("free node weight " << expected << " N, gap " << got << " N");
  CHECK(std::fabs(got - expected) < 1.0e-9 * expected);
  CHECK(number_at(row->value, {"states", "standing", "terms", "bulk_max_n"}) == 0.0);
  CHECK(number_at(row->value, {"states", "standing", "terms", "edge_max_n"}) == 0.0);

  // And under no gravity the gap is zero at every free node.
  built.file.definition.states[0].load.gravity = Vec3{0.0f, 0.0f, 0.0f};
  report = validated(built.file);
  CHECK(number_at(find(report, "state.equilibrium_gap")->value, {"states", "standing", "max_n"}) ==
        0.0);
}

TEST_CASE("validate: the cells' rows: thickness, the affine patch test, the reference's strain") {
  fixture::Built built = fixture::make();
  TissueReport report = validated(built.file);
  const ValidationRow* thickness = find(report, "region.through_thickness");
  REQUIRE(thickness != nullptr);
  // Two cell layers between three node layers: no cell touches both sheets, and the middle
  // layer's inner 5 x 5 nodes are the interior.
  CHECK(number_at(thickness->value, {"cells_spanning_both_sheets"}) == 0.0);
  CHECK(number_at(thickness->value, {"interior_nodes"}) == 25.0);

  const ValidationRow* patch = find(report, "region.affine_patch");
  REQUIRE(patch != nullptr);
  INFO(write_json(patch->value));
  // The law calibrates the isotropic part exactly and a dilation sees the bulk modulus exactly;
  // what is left is the slab's fabric.
  CHECK(std::fabs(number_at(patch->value, {"isotropic_ratio"}) - 1.0) < 1.0e-5);
  CHECK(std::fabs(number_at(patch->value, {"bulk_ratio"}) - 1.0) < 1.0e-5);
  CHECK(number_at(patch->value, {"anisotropy_min"}) < 1.0);
  CHECK(number_at(patch->value, {"anisotropy_max"}) > 1.0);

  // The reference's strain is read with its load and its prior, never as a verdict.
  const ValidationRow* strain = find(report, "region.volumetric_strain");
  REQUIRE(strain != nullptr);
  CHECK(strain->verdict == Verdict::info);
  std::string_view text;
  strain->value.find("prior")->find("method")->get_string(text);
  CHECK(text == "surface-targeted");
  strain->value.find("load")->find("pose")->get_string(text);
  CHECK(text == "standing");
  // Per cell, node-averaged, the deviation between them, and the reference's equilibrium gap.
  CHECK(number_at(strain->value, {"node_averaged_percent", "count"}) == 147.0);
  CHECK(number_at(strain->value, {"cell_minus_node_average_percent", "count"}) == 432.0);
  CHECK(number_at(strain->value, {"equilibrium_gap", "max_n"}) ==
        number_at(find(report, "state.equilibrium_gap")->value, {"states", "standing", "max_n"}));
  // A rest shrunk 3% about its centre puts about 9% on every cell of the reference.
  const Vector<Vec3> standing = fixture::read<Vec3>(built.file, "state.standing");
  Vec3 centre{0.0f, 0.0f, 0.0f};
  for (const Vec3& p : standing)
    centre = centre + p * (1.0f / static_cast<f32>(standing.size()));
  Vector<Vec3> shrunk;
  for (const Vec3& p : standing)
    shrunk.push_back(centre + (p - centre) * 0.97f);
  fixture::replace(built.file, "state.rest", shrunk);
  report = validated(built.file);
  CHECK(find(report, "region.volumetric_strain")->verdict == Verdict::info);
  CHECK(find(report, "region.volumetric_strain")->witness.find("p99 9.") == 0);
  const f64 p99 = number_at(find(report, "region.volumetric_strain")->value, {"p99_percent"});
  CHECK(std::fabs(p99 - (1.0 / (0.97 * 0.97 * 0.97) - 1.0) * 100.0) < 0.01);
}

TEST_CASE("validate: a surface node on no sheet is omitted volume, and restricts the smooth rows") {
  fixture::Built built = fixture::make();
  TissueReport report = validated(built.file);
  const ValidationRow* omitted = find(report, "volume.omitted_volume");
  REQUIRE(omitted != nullptr);
  CHECK(omitted->verdict == Verdict::pass);
  CHECK(number_at(omitted->value, {"uncovered_nodes"}) == 0.0);
  bool containment = false;
  omitted->value.find("containment_evaluated")->get_bool(containment);
  CHECK(containment);
  std::string_view representation;
  find(report, "volume.report")
      ->value.find("representations")
      ->find("visible_swept_ml")
      ->get_string(representation);
  CHECK(representation.substr(0, 8) == "complete");

  // Release the middle layer of the side wall and push it out 1 mm in the response: 24 surface
  // nodes the skin binding does not carry.
  const u32 per_layer = synthetic::k_nu * synthetic::k_nv;
  Vector<u32> rim;
  for (const u32 v : fixture::read<u32>(built.file, "slab.rim"))
    if (v / per_layer != 1) rim.push_back(v);
  CHECK(rim.size() == 48);
  fixture::replace(built.file, "slab.rim", rim);
  Vector<Vec3> pressed = fixture::read<Vec3>(built.file, "state.pressed");
  const Vector<Vec3> nodes = fixture::read<Vec3>(built.file, "slab.nodes");
  Vec3 centre{0.0f, 0.0f, 0.0f};
  for (u32 i = 0; i < per_layer; ++i)
    centre = centre + nodes[per_layer + i] * (1.0f / static_cast<f32>(per_layer));
  for (u32 j = 0; j < synthetic::k_nv; ++j)
    for (u32 i = 0; i < synthetic::k_nu; ++i) {
      if (i != 0 && j != 0 && i + 1 != synthetic::k_nu && j + 1 != synthetic::k_nv) continue;
      const u32 v = synthetic::node(i, j, 1);
      Vec3 out = nodes[v] - centre;
      out.z = 0.0f;
      pressed[v] = pressed[v] + out * (0.001f / length(out));
    }
  fixture::replace(built.file, "state.pressed", pressed);
  report = validated(built.file);
  omitted = find(report, "volume.omitted_volume");
  REQUIRE(omitted != nullptr);
  CHECK(fails(report, "volume.omitted_volume", "24 moving surface nodes"));
  const f64 ml = number_at(omitted->value, {"states", "pressed", "omitted_ml"});
  MESSAGE("the side wall pushed out 1 mm omits " << ml << " ml");
  CHECK(ml > 0.1);
  find(report, "volume.report")
      ->value.find("representations")
      ->find("visible_swept_ml")
      ->get_string(representation);
  CHECK(representation.substr(0, 10) == "restricted");
}

TEST_CASE("validate: the skin clearance holds a declared floor on the simulated surface too") {
  fixture::Built built = fixture::make();
  TissueReport report = validated(built.file);
  const ValidationRow* row = find(report, "region.skin_containment");
  REQUIRE(row != nullptr);
  INFO(failing(report));
  CHECK(row->verdict == Verdict::pass);
  // The distribution is on the tetrahedra's boundary and from the skin to it, not the cap alone.
  const f64 boundary =
      number_at(row->value, {"states", "pressed", "boundary_node_clearance_mm", "min"});
  const f64 skin =
      number_at(row->value, {"states", "pressed", "skin_to_simulated_surface_mm", "min"});
  const f64 cap = number_at(row->value, {"states", "pressed", "dense_cap_clearance_mm", "min"});
  MESSAGE("pressed: boundary nodes " << boundary << " mm, skin to the boundary " << skin
                                     << " mm, dense cap " << cap << " mm inside the skin");
  CHECK(boundary > 0.0);
  CHECK(skin > 0.0);
  // A floor above what the slab keeps is refused, with the state and the distance named.
  built.file.definition.budgets[0].skin_clearance_floor_m = 0.02;
  report = validated(built.file);
  CHECK(fails(report, "region.skin_containment", " mm"));
  CHECK(std::fabs(number_at(find(report, "region.skin_containment")->value, {"floor_mm"}) - 20.0) <
        1e-9);
}

TEST_CASE("validate: the support side's sweep is its own line in the volume report") {
  const fixture::Built built = fixture::make();
  const TissueReport report = validated(built.file);
  const JsonValue& states = *find(report, "volume.report")->value.find("states");
  // Zero at the reference, exactly; the pressed state moves the support sheet with the bump.
  CHECK(number_at(states, {"standing", "support_swept_ml"}) == 0.0);
  CHECK(number_at(states, {"standing", "support_swept_pl_ml"}) == 0.0);
  const f64 smooth = number_at(states, {"pressed", "support_swept_ml"});
  const f64 pl = number_at(states, {"pressed", "support_swept_pl_ml"});
  MESSAGE("pressed: support sweep " << smooth << " ml smooth, " << pl << " ml on its control net");
  CHECK(std::isfinite(smooth));
  CHECK(std::isfinite(pl));
  CHECK(smooth != 0.0);
}

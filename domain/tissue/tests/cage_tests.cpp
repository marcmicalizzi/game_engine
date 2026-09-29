// Cage derivation (domain/tissue/cage.h; docs/subsystems/tissue.md, "Cage derivation"): a runtime
// cage derived from a ten-node reference body within ADR-0029's budget, carrying its phases by
// volume, its node sets, attachments, frames and states; the rows that say how far it is from its
// reference; and the derivation's bytes pinned, as the content build's are on every toolchain.
#include "block_fixture.h"

#include <core/json/json.h>
#include <domain/tissue/cage.h>
#include <domain/tissue/sha256.h>
#include <domain/tissue/synthetic.h>
#include <domain/tissue/tissue_file.h>
#include <domain/tissue/validate.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

using namespace engine;
using namespace engine::tissue;

namespace {

const ValidationRow* find_row(const TissueReport& report, std::string_view id) {
  for (const ValidationRow& r : report.rows)
    if (r.id == id) return &r;
  return nullptr;
}

template <class T>
Vector<T> read(const TissueFile& file, const std::string& name) {
  Vector<T> out;
  const TissueBlock* block = file.find(name);
  if (block == nullptr) return out;
  out.resize(static_cast<u32>(block->bytes.size() / sizeof(T)));
  std::memcpy(out.data(), block->bytes.data(), block->bytes.size());
  return out;
}

std::string failures(const TissueReport& report) {
  std::string out;
  for (const ValidationRow& r : report.rows)
    if (r.verdict == Verdict::fail) out += r.id + " (" + r.subject + "): " + r.witness + "\n";
  return out;
}

f64 value_of(const ValidationRow& r, std::string_view key) {
  f64 v = 0.0;
  const JsonValue* at = r.value.find(key);
  if (at != nullptr) at->get_f64(v);
  return v;
}

std::string file_sha256(const TissueFile& file) {
  Vector<u8> bytes;
  encode_tissue_file(file, bytes);
  return sha256_hex(std::span<const u8>(bytes.data(), bytes.size()));
}

}  // namespace

TEST_CASE("cage: the block's corners, then collapses, interior first, within the budget") {
  const block::Block reference = block::make_block();
  REQUIRE(reference.corners == 140);
  CageOptions options;
  options.node_budget = 100;
  TissueFile cage;
  CageSummary summary;
  std::string error;
  REQUIRE_MESSAGE(derive_cage(reference.file, options, cage, summary, &error), error);
  CHECK(summary.source_nodes == reference.nodes);
  CHECK(summary.source_cells == 432);
  CHECK(summary.corner_nodes == 140);
  CHECK(summary.cage_nodes == 100);
  // 30 interior corners (5 x 3 x 2): the interior goes first, the boundary only past it.
  CHECK(summary.collapses_interior + summary.collapses_boundary == 40);
  CHECK(summary.collapses_interior > 0);
  CHECK(summary.collapses_boundary > 0);
  CHECK(summary.sicn_min >= 0.15);
  MESSAGE("block cage: " << summary.collapses_interior << " interior and "
                         << summary.collapses_boundary << " boundary collapses, "
                         << summary.cage_cells << " cells, SICN min " << summary.sicn_min
                         << ", boundary moved " << summary.boundary_moved_max_m * 1e3 << " mm");
  const Region& region = cage.definition.regions.front();
  CHECK(region.role == RegionRole::Runtime);
  CHECK(region.cage == CageKind::Tetrahedral);
  REQUIRE(region.derivation.has_value());
  CHECK(region.derivation->method == k_cage_method);
  CHECK(region.derivation->node_budget == 100);
  CHECK(region.derivation->source_sha256 == file_sha256(reference.file));
  CHECK(cage.definition.frames.size() == 1);
  CHECK(cage.definition.surfaces.size() == 1);
  CHECK(cage.definition.attachments.size() == 2);
  CHECK(cage.definition.states.size() == 3);

  // The mass ledger closes: the split is an identity on its numbers, and the cage keeps the mass.
  const CageLedger& l = summary.ledger;
  const f64 split = (l.source_mass_kg - l.source_linear_mass_kg) +
                    (l.source_linear_mass_kg - l.overlap_mass_kg) -
                    (l.cage_mass_kg - l.overlap_mass_kg);
  CHECK(std::fabs(split - (l.source_mass_kg - l.cage_mass_kg)) < 1e-15);
  CHECK(std::fabs(l.source_mass_kg - l.cage_mass_kg) / l.source_mass_kg < 0.01);
  MESSAGE("block ledger: reference " << l.source_mass_kg << " kg (" << l.source_volume_m3 * 1e6
                                     << " ml), cage " << l.cage_mass_kg << " kg ("
                                     << l.cage_volume_m3 * 1e6 << " ml)");

  // It validates as a runtime cage: the size row evaluated, no error, and the rows that need the
  // reference skipped without it.
  TissueReport report;
  ValidateOptions validate;
  validate_tissue(cage, validate, report);
  INFO(failures(report));
  CHECK(report.errors == 0);
  const ValidationRow* size = find_row(report, "region.cage_size");
  REQUIRE(size != nullptr);
  CHECK(size->severity == Severity::warning);
  CHECK(size->verdict == Verdict::pass);
  REQUIRE(find_row(report, "cage.source") != nullptr);
  CHECK(find_row(report, "cage.source")->verdict == Verdict::pass);
  CHECK(find_row(report, "cage.volume")->verdict == Verdict::pass);
  CHECK(find_row(report, "cage.boundary_distance")->verdict == Verdict::skipped);
  CHECK(find_row(report, "cage.state_displacement")->verdict == Verdict::skipped);

  // Handed its source, every row is evaluated.
  TissueReport with;
  validate.cage_source = &reference.file;
  validate.cage_source_sha256 = file_sha256(reference.file);
  validate_tissue(cage, validate, with);
  INFO(failures(with));
  CHECK(with.errors == 0);
  for (const char* id :
       {"cage.source", "cage.volume", "cage.boundary_distance", "cage.state_displacement"}) {
    const ValidationRow* r = find_row(with, id);
    REQUIRE(r != nullptr);
    CHECK_MESSAGE(r->verdict == Verdict::pass, id << ": " << r->witness);
  }
  const ValidationRow* displacement = find_row(with, "cage.state_displacement");
  CHECK(value_of(*displacement, "at_cage_nodes_max_um") < 1e-3);
  MESSAGE(
      "block rows: boundary " << value_of(*find_row(with, "cage.boundary_distance"), "max_mm")
                              << " mm, field at the reference's nodes "
                              << value_of(*displacement, "at_reference_nodes_max_mm")
                              << " mm, volume "
                              << value_of(*find_row(with, "cage.volume"), "volume_relative_max"));
  CHECK(find_row(with, "cage.strain_energy")->verdict == Verdict::info);
}

TEST_CASE("cage: every node is a reference node, so every carried state is the reference's") {
  const block::Block reference = block::make_block();
  CageOptions options;
  options.node_budget = 120;
  TissueFile cage;
  CageSummary summary;
  REQUIRE(derive_cage(reference.file, options, cage, summary));
  const Vector<Vec3> ref_nodes = read<Vec3>(reference.file, "block.nodes");
  const Vector<Vec3> cage_nodes = read<Vec3>(cage, "cage.nodes");
  // Each cage node is the reference node at the same place, in the reference's order.
  Vector<u32> source_of;
  u32 at = 0;
  for (const Vec3& p : cage_nodes) {
    while (at < ref_nodes.size() && std::memcmp(&ref_nodes[at], &p, sizeof(Vec3)) != 0)
      ++at;
    REQUIRE(at < ref_nodes.size());
    source_of.push_back(at++);
  }
  const char* states[] = {"rest", "reference", "pressed"};
  for (u32 s = 0; s < 3; ++s) {
    const Vector<Vec3> ref = read<Vec3>(reference.file, std::string("state.") + states[s]);
    const Vector<Vec3> carried = read<Vec3>(cage, cage.definition.states[s].nodes);
    CHECK(cage.definition.states[s].name == states[s]);
    REQUIRE(carried.size() == cage_nodes.size());
    for (u32 i = 0; i < carried.size(); ++i)
      CHECK(std::memcmp(&carried[i], &ref[source_of[i]], sizeof(Vec3)) == 0);
  }
  // A node set is its members that survive, and none is empty.
  const Vector<u32> rim = read<u32>(cage, cage.definition.regions.front().node_sets[0].nodes);
  CHECK(!rim.empty());
  for (const u32 v : rim)  // on an end wall: x is 0 or the block's length, to f32
    CHECK(block::bump(ref_nodes[source_of[v]].x, ref_nodes[source_of[v]].y) < 1e-6);
}

TEST_CASE("cage: ADR-0029's budgets, and what a cage is derived from") {
  const block::Block reference = block::make_block();
  TissueFile cage;
  CageSummary summary;
  std::string error;
  CageOptions options;
  options.node_budget = 300;
  CHECK_FALSE(derive_cage(reference.file, options, cage, summary, &error));
  CHECK(error.find("hero") != std::string::npos);
  options.hero = true;
  REQUIRE(derive_cage(reference.file, options, cage, summary, &error));
  CHECK(summary.cage_nodes == 140);  // under the budget: the corners, nothing collapsed
  CHECK(summary.collapses_interior + summary.collapses_boundary == 0);
  CHECK(cage.definition.regions.front().hero);
  options.node_budget = 801;
  CHECK_FALSE(derive_cage(reference.file, options, cage, summary, &error));
  CHECK(error.find("800") != std::string::npos);
  // The default budget is 256 without hero and 800 with.
  options = CageOptions{};
  REQUIRE(derive_cage(reference.file, options, cage, summary, &error));
  CHECK(summary.node_budget == 256);
  options.hero = true;
  REQUIRE(derive_cage(reference.file, options, cage, summary, &error));
  CHECK(summary.node_budget == 800);
  // A runtime cage is not a reference body.
  const block::Block runtime = block::make_block(false);
  CHECK_FALSE(derive_cage(runtime.file, CageOptions{}, cage, summary, &error));
  CHECK(error.find("Reference") != std::string::npos);
  // A budget the collapses cannot reach is refused with a sentence, not met by breaking the cage: a
  // box's corners cannot go without turning its faces, and each node set keeps a node.
  options = CageOptions{};
  options.node_budget = 4;
  CHECK_FALSE(derive_cage(reference.file, options, cage, summary, &error));
  CHECK(error.find("stopped at") != std::string::npos);
}

TEST_CASE("cage: a state a corner tetrahedron inverts in is refused, and left out when asked") {
  block::Block reference = block::make_block();
  // The pressed state with one top corner pushed down through its cells: a state the corner
  // tetrahedra cannot hold.
  Vector<Vec3> pressed = read<Vec3>(reference.file, "state.pressed");
  pressed[block::corner(3, 2, 3)].z -= 0.02f;
  for (TissueBlock& b : reference.file.blocks)
    if (b.name == "state.pressed") {
      std::memcpy(b.bytes.data(), pressed.data(), b.bytes.size());
      seal_block(reference.file, b);
    }
  TissueFile cage;
  CageSummary summary;
  std::string error;
  CageOptions options;
  options.node_budget = 120;
  CHECK_FALSE(derive_cage(reference.file, options, cage, summary, &error));
  CHECK(error.find("state pressed") != std::string::npos);
  CHECK(error.find("--omit-state pressed") != std::string::npos);
  options.omit_states.push_back("pressed");
  REQUIRE_MESSAGE(derive_cage(reference.file, options, cage, summary, &error), error);
  CHECK(cage.definition.states.size() == 2);
  const CageDerivation& derivation = *cage.definition.regions.front().derivation;
  REQUIRE(derivation.omitted_states.size() == 1);
  CHECK(derivation.omitted_states[0] == "pressed");
  CHECK(derivation.build_key != cage_build_key(derivation.source_sha256, "block", 120, false));
  CHECK(derivation.build_key ==
        cage_build_key(derivation.source_sha256, "block", 120, false, options.omit_states));
  // The construction is always carried, and a state that is not the region's is not left out.
  options.omit_states = {"construction"};
  CHECK_FALSE(derive_cage(reference.file, options, cage, summary, &error));
  options.omit_states = {"no such state"};
  CHECK_FALSE(derive_cage(reference.file, options, cage, summary, &error));
}

TEST_CASE("cage: the synthetic torus slab's ten-node body, derived and validated clean") {
  SyntheticOptions quadratic;
  quadratic.quadratic = true;
  const SyntheticTissue synthetic = make_synthetic_tissue(quadratic);
  TissueFile cage;
  CageSummary summary;
  std::string error;
  REQUIRE_MESSAGE(derive_cage(synthetic.file, CageOptions{}, cage, summary, &error), error);
  CHECK(summary.corner_nodes == 147);
  CHECK(summary.cage_nodes == 147);
  CHECK(summary.cage_cells == 432);
  // Not carried: the sheets, membranes, cables, the observation, the binding, the budget.
  CHECK(summary.not_carried.size() >= 5);
  TissueReport report;
  ValidateOptions validate;
  validate.compare_modes = false;
  validate.cage_source = &synthetic.file;
  validate.cage_source_sha256 = file_sha256(synthetic.file);
  validate_tissue(cage, validate, report);
  INFO(failures(report));
  CHECK(report.errors == 0);
  for (const char* id :
       {"cage.source", "cage.volume", "cage.boundary_distance", "cage.state_displacement"}) {
    const ValidationRow* r = find_row(report, id);
    REQUIRE(r != nullptr);
    CHECK_MESSAGE(r->verdict == Verdict::pass, id << ": " << r->witness);
  }
  // Its cells curve by up to 3.6% of an edge: the cage (their chords) holds 98.40 ml of the body's
  // 99.15 (docs/subsystems/tissue.md, "The synthetic definition").
  CHECK(summary.ledger.source_volume_m3 * 1e6 == doctest::Approx(99.1515).epsilon(1e-4));
  CHECK(summary.ledger.cage_volume_m3 * 1e6 == doctest::Approx(98.4045).epsilon(1e-4));
}

TEST_CASE("cage: a file that carries both regions, and a source that is not the one recorded") {
  const block::Block reference = block::make_block();
  CageOptions options;
  options.node_budget = 110;
  TissueFile cage;
  CageSummary summary;
  REQUIRE(derive_cage(reference.file, options, cage, summary));
  // The wrong source: a different file, handed over as if it were the one.
  const block::Block other = block::make_block();
  TissueFile moved = other.file;
  {
    Vector<Vec3> nodes = read<Vec3>(moved, "block.nodes");
    nodes[0].x += 1e-4f;
    for (TissueBlock& b : moved.blocks)
      if (b.name == "block.nodes") {
        std::memcpy(b.bytes.data(), nodes.data(), b.bytes.size());
        seal_block(moved, b);
      }
  }
  ValidateOptions validate;
  validate.cage_source = &moved;
  validate.cage_source_sha256 = file_sha256(moved);
  TissueReport report;
  validate_tissue(cage, validate, report);
  CHECK(find_row(report, "cage.source")->verdict == Verdict::fail);
  CHECK(report.errors >= 1);
  CHECK(find_row(report, "cage.boundary_distance")->verdict == Verdict::skipped);

  // Both regions in one file: the reference under its own name, the cage renamed, its derivation
  // naming the reference region with no hash — the rows find the source in the file itself.
  TissueFile both = reference.file;
  Region region = cage.definition.regions.front();
  region.name = "block cage";
  region.derivation->source_sha256.clear();
  region.derivation->build_key =
      cage_build_key("", region.derivation->source_region, region.derivation->node_budget, false);
  for (const TissueBlock& b : cage.blocks) {
    if (b.name.rfind("cage.", 0) != 0) continue;
    TissueBlock copy = b;
    seal_block(both, copy);
    both.blocks.push_back(copy);
  }
  both.definition.regions.push_back(region);
  for (RegionState s : cage.definition.states) {
    s.region = "block cage";
    both.definition.states.push_back(s);
  }
  TissueReport together;
  validate_tissue(both, ValidateOptions{}, together);
  u32 evaluated = 0;
  for (const ValidationRow& r : together.rows)
    if (r.id.rfind("cage.", 0) == 0 && r.verdict != Verdict::skipped) ++evaluated;
  CHECK(evaluated == 5);
  CHECK(find_row(together, "cage.source")->verdict == Verdict::pass);
}

TEST_CASE("cage: round trip through the container keeps the derivation") {
  const block::Block reference = block::make_block();
  TissueFile cage;
  CageSummary summary;
  CageOptions options;
  options.node_budget = 100;
  REQUIRE(derive_cage(reference.file, options, cage, summary));
  Vector<u8> bytes;
  REQUIRE(encode_tissue_file(cage, bytes));
  TissueFile read_back;
  std::string error;
  REQUIRE_MESSAGE(
      read_tissue_file_memory(std::span<const u8>(bytes.data(), bytes.size()), read_back, &error),
      error);
  CHECK(read_back.warnings.empty());
  REQUIRE(read_back.definition.regions.front().derivation.has_value());
  CHECK(*read_back.definition.regions.front().derivation ==
        *cage.definition.regions.front().derivation);
  Vector<u8> again;
  REQUIRE(encode_tissue_file(read_back, again));
  CHECK(again.size() == bytes.size());
  CHECK(std::memcmp(again.data(), bytes.data(), bytes.size()) == 0);
  // A definition that carries no derivation writes no field for it.
  Vector<u8> plain;
  REQUIRE(encode_tissue_file(reference.file, plain));
  const std::string text(reinterpret_cast<const char*>(plain.data()), plain.size());
  CHECK(text.find("derivation") == std::string::npos);
}

// The derived cage's bytes, section by section: taken on MSVC, and the same on GCC and Clang at
// either CPU baseline because the block and the derivation are sums, products, quotients and square
// roots of exact inputs (ADR-0035). A failure prints this build's table; a change that means to
// move the bytes (the method) is a new method version, and this table moves with it.
struct SectionHash {
  const char* name;
  const char* sha256;
};
// Taken on MSVC 14.51, 2026-09-29.
constexpr SectionHash k_golden[] = {
    {"definition", "23d78bce0e6acfac1c62af1286d312a5feab6ef7c39904e1c1637dce1153cc00"},
    {"cage.nodes", "3e78d305688e7f08c2797fc699929f1d3f7d43f258580006f75138f3a559ef8b"},
    {"cage.tets", "f07ddeee2fc9f56ffd2b4cd47c4e4d6839e4e30862417050c178cd53d482921d"},
    {"cage.phase.1", "2ce73cf10a221f0c10fe7ddbe15fbaa44b0e3d26e4469c4ed9d6a16f229e99eb"},
    {"cage.set.0", "c8ffe213182a06c6fa24c97f8d4ca78b96aab49616a1d44be8320be2cc6a4b5b"},
    {"cage.set.1", "a4184273505b701ef1ebbfbaaa950e1a069057cda711df50ede38da3b6be9907"},
    {"base.vertices", "87abe20c2b4094e94b5d5cd95afc027fb5e5f9d7a6977dddb7894ae0aa2434fd"},
    {"base.triangles", "8d6c2cef0f987a5b43db91b5ea47108059cd84c29d4afd621a258b2b72e59773"},
    {"floor.vertices", "b6c2eb4949c145eaa50dc1c2275a0eda5762a9a4c15a008e4d2e3080b2d7b38f"},
    {"floor.triangles", "0c6a9a02be5f7eb53fb20c6b5b3c610b857b50e630614fec5f4bfcd7fc6f1567"},
    {"cage.state.0", "07ea1c3891a241cf1c8dbb2063fb439aeef6e17822cfaa1d53f47bc5684dc43f"},
    {"cage.state.1", "334a0c1151dc4500ec684f6a2b61221dd02521ee5ddf31bf534b12ed3e09c7a9"},
    {"cage.state.2", "2d3f3597ed06c1be31a8c6d0436cc0ceda1b3efe6a4313bb0e33e28995b9efaf"},
};

TEST_CASE("cage: the derivation writes the committed bytes on every toolchain") {
  const block::Block reference = block::make_block();
  CageOptions options;
  options.node_budget = 100;
  TissueFile first;
  TissueFile second;
  CageSummary summary;
  REQUIRE(derive_cage(reference.file, options, first, summary));
  REQUIRE(derive_cage(reference.file, options, second, summary));
  Vector<u8> a;
  Vector<u8> b;
  REQUIRE(encode_tissue_file(first, a));
  REQUIRE(encode_tissue_file(second, b));
  REQUIRE(a.size() == b.size());
  CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
  // The sections: the definition's canonical JSON, then each block.
  Vector<std::pair<std::string, std::string>> got;
  const std::string json = [&] {
    std::string text;
    write_json(definition_json(first.definition), text, JsonWriteOptions{.pretty = false});
    return text;
  }();
  got.push_back({"definition", sha256_hex(std::span<const u8>(
                                   reinterpret_cast<const u8*>(json.data()), json.size()))});
  for (const TissueBlock& block : first.blocks)
    got.push_back({block.name, sha256_hex(block.bytes)});
  bool same = got.size() == std::size(k_golden);
  for (u32 i = 0; same && i < got.size(); ++i)
    same = got[i].first == k_golden[i].name && got[i].second == k_golden[i].sha256;
  if (!same) {
    std::string table;
    for (const auto& [name, hash] : got)
      table += "    {\"" + name + "\", \"" + hash + "\"},\n";
    FAIL_CHECK("the derived cage does not match the committed hashes; this build's:\n" << table);
  }
}

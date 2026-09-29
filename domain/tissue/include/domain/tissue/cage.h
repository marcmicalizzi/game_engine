#pragma once

// Cage derivation (docs/subsystems/tissue.md, "Cage derivation"; docs/plan/07-content-pipeline.md
// §7.10, "cage generation is a derived step"; ADR-0029): a `Reference` region of ten-node cells in,
// a `Runtime` region of four-node cells within ADR-0029's budget out — 256 nodes, or up to 800 for
// a hero volume — carrying the material phases by volume, the node sets and the attachments mapped
// to the cage's nodes, the frames and their states untouched, and every state of the reference as
// a state of the cage. `engine-content tissue cage` is this on the command line.
//
// **The method, `corner-collapse-v1`.** The ten-node cells' corner tetrahedra first: the body's own
// linear mesh with the edge nodes dropped, which is the cage the authoring side meshed before it
// added a node to every edge, and whose boundary is the reference's boundary faces' corner
// triangles. Then, while the node count is over the budget, half-edge collapses — a node moved onto
// a neighbour and its cells re-coned from there — chosen greedily: every collapse of an interior
// node before any of a boundary node (the boundary is kept while the interior can absorb the
// budget), and within each, the cheapest by edge length over the worst quality it leaves (interior)
// or by how far it moves the boundary (boundary). A collapse is taken only when the link condition
// holds (it changes no topology), every re-coned cell is positive in every state and no worse in
// quality than min(0.15, what it replaces) at the construction, no boundary face turns over, and
// every node set that holds the moved node also holds the one it moves onto. **Every cage node is a
// reference node**, so a state carried onto the cage is the reference's displacement field
// evaluated at the cage's nodes exactly, a node set is its members that survive, and nothing is
// interpolated. Why not a fresh tetrahedralization of a coarsened boundary: the tree vendors no
// tetrahedralizer (Gmsh is GPL, TetGen AGPL, fTetWild not vendored; ADR-0014), and this keeps the
// boundary, the frame's clearances and the reference's own nodes.
//
// **The phases by volume.** Each cage cell's phase fractions are the volume-weighted mean of the
// reference's over the part of the reference it overlaps (the reference's same-node linear
// subdivision, each piece taking its cell's fractions, clipped against the cage cell), so the cage
// holds each phase's volume where the reference held it. **The mass ledger** closes: the
// reference's mass (its exact ten-node volumes), the cage's, and the difference split into the
// reference's curvature (exact less its linear subdivision), the reference's material outside the
// cage, and the cage's volume outside the reference.
//
// **Deterministic**: single-threaded, integer decisions from double arithmetic with every tie
// broken by index, no C-library call but the square root: the same bytes on every toolchain
// (`cage_tests.cpp` pins the sections' hashes).

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <domain/tissue/tissue_file.h>

#include <string>

namespace engine::tissue {

// The derivation's spelling, with its version: what `CageDerivation::method` records and the
// capability line lists. Changing what it derives is a new version.
inline constexpr const char* k_cage_method = "corner-collapse-v1";

struct CageOptions {
  // The reference region; empty takes the file's one `Reference` region.
  std::string region;
  // The node budget; 0 takes ADR-0029's: 256, or 800 with `hero`. Past 800 is refused, and past
  // 256 without `hero`: a cage wider than one solve group is a hero volume, deliberately.
  u32 node_budget = 0;
  bool hero = false;
  // SHA-256 of the source container's bytes, which the derivation records; empty takes the SHA-256
  // of the reference as this build encodes it (the same bytes for a container it wrote).
  std::string source_sha256;
  // States of the reference the cage leaves out, by name. Every other state is carried, and one in
  // which a corner tetrahedron is not positive — a ten-node cell valid only through its curvature —
  // refuses the derivation, naming the state and the cell: a cage is never written in a state it
  // cannot represent. The construction is always carried.
  Vector<std::string> omit_states;
};

// The mass ledger at the construction, in kilograms and cubic metres.
struct CageLedger {
  f64 source_volume_m3 = 0.0;  // the reference's exact ten-node volume
  f64 source_mass_kg = 0.0;
  f64 source_linear_volume_m3 = 0.0;  // its same-node linear subdivision's
  f64 source_linear_mass_kg = 0.0;
  f64 cage_volume_m3 = 0.0;
  f64 cage_mass_kg = 0.0;
  f64 overlap_volume_m3 = 0.0;  // of the subdivision inside the cage
  f64 overlap_mass_kg = 0.0;    // at the subdivision's densities
};

// What a derivation did.
struct CageSummary {
  std::string source_region;
  std::string source_sha256;
  std::string build_key;
  u32 node_budget = 0;
  bool hero = false;
  u32 source_nodes = 0;
  u32 source_cells = 0;
  u32 corner_nodes = 0;
  u32 corner_boundary_nodes = 0;
  u32 collapses_interior = 0;
  u32 collapses_boundary = 0;
  u32 cage_nodes = 0;
  u32 cage_cells = 0;
  u32 cage_boundary_nodes = 0;
  f64 sicn_min = 0.0;  // the cage's worst cell at the construction
  u32 sicn_min_cell = 0;
  f64 boundary_moved_max_m = 0.0;  // the farthest a boundary collapse moved the boundary
  CageLedger ledger;
  Vector<std::string> omitted_states;
  Vector<std::string> carried;
  Vector<std::string> not_carried;
};

// The derived entry's key: SHA-256 over the method, the source's SHA-256 and region, the budget,
// the hero flag and the omitted states (sorted), as canonical text.
std::string cage_build_key(const std::string& source_sha256, const std::string& region,
                           u32 node_budget, bool hero, const Vector<std::string>& omitted = {});

// Derives the runtime cage. False, with a sentence, when the region is not a reference body of
// four- or ten-node cells, the budget is outside ADR-0029's, the collapses cannot reach it, or
// something the cage must carry cannot be: a state a corner tetrahedron inverts in (leave it out,
// `omit_states`), a node set with no corner node, an essential attachment (whose patch lies on a
// ten-node surface the cage does not have).
bool derive_cage(const TissueFile& reference, const CageOptions& options, TissueFile& out,
                 CageSummary& summary, std::string* error = nullptr);

JsonValue cage_summary_json(const CageSummary& summary);

}  // namespace engine::tissue

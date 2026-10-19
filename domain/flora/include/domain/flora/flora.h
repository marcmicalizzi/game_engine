#pragma once

// flora capability (ADR-0027; docs/subsystems/flora.md, docs/plan/05-simulation.md §5.15): the
// deterministic tree solver — a species' parameters and a seed in, a mesh out — that makes the
// engine's vegetation instead of modelled or image-to-3D assets, which E10 found lose their detail
// past a person's size (docs/experiments/e10-generated-props.md). Its first species is the date
// palm (palm.h). It owns the species' parameter types (schemas/flora.schema) and the solver; it
// does not own placement (a scatter layer or a scene's instances place what it makes), wind
// (roadmap S66), or the textures its materials will sample (G60).
//
// The registration points, ADR-0027 decision 2:
//
//   [x] schema types      schemas/flora.schema: engine.flora.Palm, engine.flora.FloraFile
//   [-] scheduler entry   none: the solver runs at content-build time and has no tick
//   [-] render passes     none: a palm is a cluster mesh like any other
//   [x] derived data      the solver's container, keyed by the parameters (flora.md, "The build")
//   [-] protocol methods  none yet
//   [-] tunables          none: everything the solver reads is a parameter of the file
//   [x] LOD policy        the cluster DAG's (flora.md, "LOD")
//   [x] determinism       the same parameters give the same bytes on every toolchain (palm.h)
//   [x] zero cost unused  nothing runs unless a mesh names a `.flora.json`
//   [x] docs, tests, size table
//   [-] bench             no hot path: the solver runs once per parameter set

#include <core/base/types.h>
#include <domain/flora/palm.h>
#include <domain/flora/trees.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>

#include <schemas/flora.h>
#include <string>
#include <string_view>

namespace engine::flora {

// The solver's version: part of every build key over a flora file, so a change to what the solver
// makes from the same parameters — a new rung on the budget's ladder, a different leaflet — is a
// new key and never an old container read as the answer. Bump it with any change that moves a
// byte of the default palm's mesh (the determinism test says when one did).
//
// 2: the palm's crown on one leaf spiral by age, the trunk's top a collar of persistent frond bases
//    and swelling sheaths (flora.md, "The crown"), and the motion contract's sections 35 and 36 in
//    every container — an entry built at 1 has neither.
inline constexpr u32 k_flora_version = 2;

// The species `engine.flora.FloraFile.species` names; the solver reads the parameters of the same
// member (`palm`, `broadleaf`, `conifer`).
inline constexpr const char* k_species_date_palm = "date_palm";
inline constexpr const char* k_species_broadleaf = "broadleaf";
inline constexpr const char* k_species_conifer = "conifer";

// ---- the flora file and its build (flora.md, "The build")
// ------------------------------------------

// Reads a `.flora.json`'s text: an `engine.flora.FloraFile`, a species the solver has, and
// parameters in range. False, with a sentence naming the field, otherwise; `out` is replaced.
bool read_flora_file(std::string_view text, FloraFile& out, std::string& error);

// The file as the build keys it: the schema's JSON of what it says, defaults filled in, compact
// and in the schema's field order, so two files that say the same thing — whatever their spacing,
// key order, or the defaults they spell out — are one parameter set. What a container records in
// its generator section.
std::string canonical_flora_json(const FloraFile& file);

// The source hash a flora container records and its cache key is taken over: the canonical
// parameters' bytes, the species, and `k_flora_version`.
u64 flora_source_hash(const FloraFile& file);

// The build key over that hash: `geometry::cluster_cache_key(source_hash, lod, false,
// page_bytes)` — no weld, since the solver's vertices are shared exactly where they are meant to
// be — which is the key `engine-content flora` and the renderer's scene reader both write under.
u64 flora_build_key(u64 source_hash, const geometry::ClusterLodOptions& lod, u32 page_bytes);

// Solves the file's species — the palm, the broadleaf or the conifer, by `species` — into a mesh
// and its motion contract. The report its species fills, when given; the other is left alone.
bool solve_flora(const FloraFile& file, assets::MeshData& mesh, Motion& motion,
                 PalmReport* palm_report, TreeReport* tree_report, std::string& error);

// Solves the file's species and builds its container: one cluster LOD DAG per primitive, each with
// its material's cone setting (the trunk's on, a two-sided frond's off), merged, laid out in
// pages of `page_bytes` (0: no page table), with the materials, the identity (source hash and
// build key) and the generator section (`flora`, the species, the canonical parameters). The
// source path is left to the caller. Deterministic: the same file gives the same bytes on every
// toolchain and thread count.
bool build_flora_container(const FloraFile& file, const geometry::ClusterLodOptions& lod,
                           u32 page_bytes, geometry::ClusterFileData& out, PalmReport* report,
                           std::string& error);

}  // namespace engine::flora

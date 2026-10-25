#pragma once

// flora capability (ADR-0027; docs/subsystems/flora.md, docs/plan/05-simulation.md §5.15): the
// deterministic tree solver — a species' parameters and a seed in, a mesh out — that makes the
// engine's vegetation instead of modelled or image-to-3D assets, which E10 found lose their detail
// past a person's size (docs/experiments/e10-generated-props.md). Its first species is the date
// palm (palm.h). It owns the species' parameter types (schemas/flora.schema) and the solver; it
// owns the sward placement generator (G90); trees use scatter or authored instances. Runtime
// wind posing and material texture streaming belong to their existing engine systems.
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
#include <domain/flora/understory.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>

#include <schemas/flora.h>
#include <string>
#include <string_view>

namespace engine::materials {
struct Pack;
}
namespace engine::assets {
struct MeshGeneratorInputs;
struct MeshGeneratorHosts;
}  // namespace engine::assets

namespace engine::flora {

// The solver's version: part of every build key over a flora file, so a change to what the solver
// makes from the same parameters — a new rung on the budget's ladder, a different leaflet — is a
// new key and never an old container read as the answer. Bump it with any change that moves a
// byte of the default palm's mesh (the determinism test says when one did).
//
// 2: the palm's crown on one leaf spiral by age, the trunk's top a collar of persistent frond bases
//    and swelling sheaths (flora.md, "The crown"), and the motion contract's sections 35 and 36 in
//    every container — an entry built at 1 has neither.
// 3: the palm's bark (the trunk, its cap and the collar of bases) and its fronds and skirt in UVs
// of
//    metres, so a tiled set's `tile_m` is its physical repeat (flora.md, "Materials"); the trunk's
//    were in circumferences, a frond's in its own length.
// 4: the impostor chain nests (every card switches on its own sub-tree's sphere), carries coverage
//    errors (section 38's two words and section 39), and its star has four planes, not five (R91;
//    flora.md, "The impostor chain"); an entry built at 3 switches its cards a kilometre out.
// 5: the megaflora's habits and limbs a wood could carry (G79; flora.md, "Megaflora"): an ancient
//    tree's limbs fork, meander and are sized by their wood, so an entry built at 4 is the old
//    straight-limbed tree.
// 6: shared species rules, supported irregular wood and renewed ancient crowns (G85).
// 7: fuller conifer/willow foliage and seated ancient segment joins (G86).
// 8: maple terminal wood and independent opposite leaf blades (G86 follow-up).
// 9: species-correct terminal foliage and material-dependent canopy LOD (G87).
// 10: species maturity, four profiles, sapling taper, and bounded trunk-tip crowns (G88).
inline constexpr u32 k_flora_version = 10;

// The species `engine.flora.FloraFile.species` names; the solver reads the parameters of the same
// member (`palm`, `broadleaf`, `conifer`, `birch`, `willow`, `orchard`, `ancient`).
inline constexpr const char* k_species_date_palm = "date_palm";
inline constexpr const char* k_species_broadleaf = "broadleaf";
inline constexpr const char* k_species_conifer = "conifer";
inline constexpr const char* k_species_birch = "birch";
inline constexpr const char* k_species_willow = "willow";
inline constexpr const char* k_species_orchard = "orchard";
inline constexpr const char* k_species_ancient =
    "ancient";  // the megaflora (flora.md, "Megaflora")
// The understory (G75; understory.h, flora.md "The understory").
inline constexpr const char* k_species_shrub = "shrub";
inline constexpr const char* k_species_fern = "fern";
inline constexpr const char* k_species_tuft = "tuft";
inline constexpr const char* k_species_herb = "herb";

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

// Includes the bytes of base-colour images consumed by the LOD bake. Pure without a host;
// a host that supplies images must successfully read every referenced bake input.
bool flora_source_hash_with(const FloraFile& file, const assets::MeshGeneratorInputs& inputs,
                            u64& out, std::string& error);

// The build key over that hash: `assets::generated_mesh_build_key` —
// `cluster_cache_key(source_hash, lod, false, page_bytes)`, no weld, since the solver's vertices
// are shared exactly where they are meant to be, with the material pack's key when one with roles
// is given — which is the key `engine-content flora` and the renderer's scene reader both write
// under.
u64 flora_build_key(u64 source_hash, const geometry::ClusterLodOptions& lod, u32 page_bytes,
                    const materials::Pack* pack = nullptr);

// The role a species' material `material` is looked up by in a material pack (flora.md,
// "Materials"): "palm/bark", "palm/frond", "palm/skirt"; "broadleaf/bark", "broadleaf/leaf";
// "conifer/bark", "conifer/needles"; and for a species with no list of its own, `<species>/bark`
// and `<species>/leaf` for its first two. Empty past the species' parts.
std::string flora_role(const std::string& species, u32 material);

// Writes each part's material set into `out`'s materials (domain/materials): the recipe's set for
// the part when it names an image, else the pack's for the part's role, else none, which leaves the
// flat colour. Bark and the palm's fronds and skirt are in metres and take `1 / tile_m`; a leaf or
// needle card takes its image whole. Then the texture records (section 32) when any image is named.
void apply_flora_materials(const FloraFile& file, const materials::Pack* pack,
                           geometry::ClusterFileData& out);

// Solves the file's species — whichever `species` names — into a mesh
// and its motion contract. The report its species fills, when given; the other is left alone.
// `levels`, when given, receives the impostor chain of a species that builds one (the broadleaf
// and the ancient tree; impostor.h) with its cards appended to `mesh` past everything, and is
// left empty by the rest.
bool solve_flora(const FloraFile& file, assets::MeshData& mesh, Motion& motion,
                 PalmReport* palm_report, TreeReport* tree_report, std::string& error,
                 TreeLevels* levels = nullptr);

// The kind's detail tier (flora.md, "Detail tiers"): the one its species struct declares for the
// understory, `essential` for every tree (a tree collides by its trunk; the trees carry no field,
// so their pinned parameters do not move). A graphics preset may leave out a whole tier; the
// renderer honours none of it yet (roadmap G75).
DetailTier flora_detail_tier(const FloraFile& file);

// Whether instances of the file's mesh carry a collision body: every tree (its trunk), a shrub
// whose stoutest stem is at least `k_shrub_collider_radius_m` (understory.h), never a fern, a tuft
// or a herb. The reader refuses `rich` wherever this is true.
bool flora_collides(const FloraFile& file);

// Solves the file's species and builds its container: one cluster LOD DAG per primitive, each with
// its material's cone setting (the trunk's on, a two-sided frond's off), merged, laid out in
// pages of `page_bytes` (0: no page table), with the materials and their sets
// (`apply_flora_materials`, from the recipe's block and `pack`), the identity (source hash and
// build key) and the generator section (`flora`, the species, the canonical parameters). The
// source path is left to the caller. Deterministic: the same file and pack give the same bytes on
// every toolchain and thread count. `tree_report`, when given, receives a branching species'
// report: its budget, and for the allometric crowns the twigs, the orders and the realised leaf
// area index.
bool build_flora_container(const FloraFile& file, const geometry::ClusterLodOptions& lod,
                           u32 page_bytes, geometry::ClusterFileData& out, PalmReport* report,
                           std::string& error, const materials::Pack* pack = nullptr,
                           TreeReport* tree_report = nullptr,
                           const assets::MeshGeneratorHosts* hosts = nullptr);

}  // namespace engine::flora

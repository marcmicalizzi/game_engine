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
//   [x] LOD policy        the cluster DAG's (flora.md, "Shared growth rules and named species")
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
// 11: researched broadleaf blade dimensions, independent of retained leaf allocation (G91).
// 12: every tree's stem carried on below its collar as a skirt, so no tree on ground of up to 45
//    degrees shows a gap under its trunk (G201; flora.md, "The trunk's skirt"): every tree's mesh
//    grows by four triangles a side of its trunk and its box reaches down, so its grid moves.
// 13: foliage optics on every leaf and needle material (R164; flora.md, "Foliage optics"): a
//    leaf transmits, a needle card's normals are a sphere's; the container's alpha word carries
//    them, so an entry built at 12 draws its foliage as a sheet lit from the eye's side alone.
// 14: the impostor bake's tangent frames turn their bitangent over to -dP/dv, the resolve's (R202;
//    docs/experiments/tangent-space-convention.md): an atlas baked at 13 stores its normals' second
//    component the other way up, so the renderer would read every card's relief upside down.
inline constexpr u32 k_flora_version = 14;
// The convention every baked normal of every family is written in (R202: +Y up the image, a
// derived bitangent of -dP/dv), folded into every family's source hash (`flora_source_hash`),
// since the species families that key on a version of their own bake cards and impostors too.
// 1: R202, the first value folded; an entry from before it baked the other way up.
inline constexpr u32 k_baked_normal_convention = 1;
// G129: broadleaf/birch shape and baked-LOD invalidation, without rebuilding other architectures.
inline constexpr u32 k_branch_taper_version = 2;

// The conifers' impostor chain (G120; flora.md, "The conifer's chain"): mixed into the source hash
// a container of a species the conifer solver builds records (`conifer` and the evergreen profiles:
// pine, spruce, cedar, fir, hemlock, juniper; `flora_source_hash_with` and the mesh generator's
// plain hash), after `k_flora_version`, so their entries rebuild with the chain and every other
// species keeps its key, where a bump of `k_flora_version` would rebuild every tree of every cache.
// Not in `flora_source_hash` itself, the tree's shape, which the deadwood's key takes and which
// the chain does not change. Bump it with a change to the conifers' chain alone.
//
// 1: the conifers gain the chain — one three-plane card group a live branch and the whole-tree
//    star — baked with the needles' coverage kept (`ImpostorSpec::coverage_dither`).
// 2: the conifers' sprays are folded, carry one branchlet a spacing and have the image's base at
//    the shoot on both sides (`build_bough`, conifer.cpp; G203's third leg, 2026-11-15). The
//    needle cards' positions and UVs moved, and with them the chain's bakes; the wood, the parts
//    and every count are what they were. It is the conifers' own key, so it is bumped here though
//    the change is to the sprays the chain is baked from: no other species' entry moves.
inline constexpr u32 k_conifer_chain_version = 2;

// The broadleaf solver's impostor chain (G121; flora.md, "The coverage under the mask"): mixed
// into the source hash a container of a species the broadleaf solver builds records (`broadleaf`
// and the deciduous profiles: oak, maple, elm, walnut, beech, ash, basswood, black_cherry,
// sycamore, aspen, alder), after the conifers' place, the same way and for the same reason as
// `k_conifer_chain_version`: those entries rebuild with the chain and no other species' moves, and
// the deadwood, which keys a tree by `flora_source_hash` and draws no chain, keeps its key.
//
// 1: the levels are baked with the leaves' coverage kept and fitted
//    (`ImpostorSpec::coverage_dither`, `bake_matched`), as the conifers' have been since G120.
inline constexpr u32 k_broadleaf_chain_version = 1;

// The birch's and the ancient tree's impostor chains (sweep-P1, after G121; flora.md, "The
// coverage under the mask"): each solver has its own, mixed into the container hash of `birch`
// and of `ancient` after the broadleaf's place, the same way and for the same reason as
// `k_broadleaf_chain_version`, so only those entries rebuild and the deadwood's keys stay.
//
// 1: the levels are baked with the leaves' coverage kept and fitted, as the broadleaf's since G121
//    (a plain bake drew 1.35 to 1.52 of the 14 m birch's leaf coverage and 1.22 to 1.34 of the
//    100 m ancient tree's; kept, 1.00 to 1.06 and 1.00 to 1.10).
inline constexpr u32 k_birch_chain_version = 1;
inline constexpr u32 k_ancient_chain_version = 1;

// The version of the needle cards a tree of this file is built with: `k_conifer_chain_version`
// for a species the conifer solver builds, 0 for every other (and for a regeneration form, whose
// needles are ribbons). For a builder that keeps a tree's needle cards and keys itself by
// `flora_source_hash`, the tree's parameters, which a change to the cards does not move: the
// deadwood mixes it into its key when it is not 0, so a dead conifer's entry rebuilds with the
// cards it keeps and no dead broadleaf's moves.
u32 flora_needle_card_version(const FloraFile& file);

// **The seat** (G201 stage 2; flora.md, "The seat"): mixed into the container's source hash of
// every species whose stem stands on the ground with a skirt (every tree; not a regeneration
// sapling, a shrub or the ground plants), after the chain's version, and not into
// `flora_source_hash`, the tree's shape, which the seat does not change and the deadwood keys by.
// Bump it with a change to what the seat records alone.
//
// 1: the container records the stem's foot (section 42): its reach from the origin, the collar's
//    height and the skirt's bottom, which the hosts seat an instance on the ground by.
inline constexpr u32 k_flora_seat_version = 1;

// **The structural table of the broadleaf and the conifer** (A45; flora.md, "The structural
// table"): mixed into the container's source hash of every species the broadleaf or the conifer
// solver builds (`broadleaf`, `conifer` and every species profile: oak, beech, ash, maple, walnut,
// spruce, pine, fir, ...), after the seat's version, and not into `flora_source_hash`, the tree's
// shape, which the table does not change and the deadwood keys by. Bump it with a change to what
// those two solvers' tables record alone.
//
// 1: the two solvers write section 37 (until then only the birch, willow, orchard, ancient, shrub
//    and understory solvers did): the trunk load-bearing, every limb, branch and shoot its radii.
// 2: every part a leaf, a leaflet or a needle card rides is flagged `k_foliage` (A45's second leg),
//    so the walker's push reaches it as far as before the table.
// 3: the trunk is flagged `k_full_sway` as well (A45's third leg), so it sways as it did before
//    the table rather than at `renderer::k_flora_load_bearing` of its class's compliance.
inline constexpr u32 k_flora_structure_version = 3;

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
// G167: a meadow's drift of herbs posed as a field (`meadow`, understory.h).
inline constexpr const char* k_species_meadow = "meadow";

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

// The tree's parameters as a hash: the canonical parameters' bytes, the species, and
// `k_flora_version` — what a flora container's source hash starts from (`flora_source_hash_with`
// adds the conifers' `k_conifer_chain_version` and the bake's images) and what the deadwood keys
// the tree it kills by. Every species' starts with the foliage optics folded in after the species
// (`fold_foliage_optics`; flora.md, "Foliage optics": R164-3, review B2).
u64 flora_source_hash(const FloraFile& file);
// `h` with the foliage optics flora declares for its parts (kit.h's three numbers) folded in, so a
// change of any of them moves every flora key.
u64 fold_foliage_optics(u64 h) noexcept;

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

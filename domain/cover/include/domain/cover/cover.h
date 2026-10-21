#pragma once

// cover capability (ADR-0027; docs/subsystems/cover.md, docs/experiments/vine-cover.md): what grows
// **over** another mesh — climbing plants (roadmap G76, vine.h) and organic cover, moss, lichen,
// algae and fungi (G78, growth.h) — on one host-surface foundation (host.h). A recipe names its
// host (a rock, a wall, a tree: any mesh a scene names) and the capability makes the host's
// **companion**: its own mesh in the host's frame, placed with the host's transform, the host never
// edited. It owns the recipes (schemas/cover.schema) and the solvers; it does not own the host, the
// placement (the scene places the companion beside its host), or the blended drawing of the smooth
// cover (roadmap R90; the film stands in).
//
// **Why one module, and why here** (cover.md, "Why this shape"): the host query — a BVH over a
// cluster mesh's triangles for the closest point and occlusion — is generic, and a decal or a
// footprint will want it one day; but today both of its readers are here, and a capability that
// edits `domain/geometry` for its own use is the kind of edge ADR-0027 keeps out. So it lives here,
// and moves to geometry with its second reader.
//
// The registration points, ADR-0027 decision 2:
//
//   [x] schema types      schemas/cover.schema: engine.cover.VineFile, engine.cover.CoverFile
//   [-] scheduler entry   none: the solvers run at content-build time and have no tick
//   [-] render passes     none: a companion is a cluster mesh like any other; R90 will add one
//   [x] derived data      the mesh generators "vine" and "cover" (assets/mesh_generator.h), keyed
//                         by the canonical recipe, the solver's version and the host's identity
//   [-] protocol methods  none yet
//   [-] tunables          none: everything the solvers read is a parameter of the recipe
//   [x] LOD policy        the cluster DAG's, one per primitive (cover.md, "LOD")
//   [x] determinism       the same recipe and host give the same bytes on every toolchain
//   [x] zero cost unused  nothing runs unless a scene names a `.vine.json` or a `.cover.json`
//   [x] docs, tests, size table
//   [-] bench             no hot path: the solvers run once per recipe and host

#include <core/base/types.h>
#include <domain/assets/mesh_generator.h>
#include <domain/cover/growth.h>
#include <domain/cover/host.h>
#include <domain/cover/vine.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>

#include <schemas/cover.h>
#include <string>
#include <string_view>

namespace engine::materials {
struct Pack;
}

namespace engine::cover {

// The solvers' version: part of every build key over a recipe, so a change to what the solvers make
// from the same recipe and host is a new key and never an old container read as the answer. Bump
// it with any change that moves a byte of a pinned companion (the determinism test says when one
// did).
inline constexpr u32 k_cover_version = 1;

// The generators' names, as a recipe's file name gives them: `<name>.vine.json`,
// `<name>.cover.json`.
inline constexpr const char* k_vine_generator = "vine";
inline constexpr const char* k_cover_generator = "cover";
inline constexpr const char* k_species_ivy = "ivy";

// The material roles a pack is looked up by (materials.md): the vine's stems and leaves; the
// cover's moss, lichen, algae and fungi.
inline constexpr const char* k_vine_roles[2] = {"vine/stem", "vine/leaf"};
inline constexpr const char* k_cover_roles[k_cover_kinds] = {"moss/mat", "lichen/crust",
                                                             "algae/film", "fungus/cap"};

// Reads a recipe's text: the schema's type, a species the solver has, parameters in range and a
// host named. False, with a sentence naming the field, otherwise.
bool read_vine_file(std::string_view text, VineFile& out, std::string& error);
bool read_cover_file(std::string_view text, CoverFile& out, std::string& error);

// The recipe as the build keys it: the schema's JSON, defaults filled in, compact.
std::string canonical_vine_json(const VineFile& file);
std::string canonical_cover_json(const CoverFile& file);

// The source hash: the canonical recipe, the generator, `k_cover_version` and the host's identity.
u64 vine_source_hash(const VineFile& file, u64 host_identity);
u64 cover_source_hash(const CoverFile& file, u64 host_identity);

// The host as the solvers read it: welded, measured, its BVH built.
struct Host {
  HostSurface surface;
  HostBvh bvh;
};
bool prepare_host(const assets::GeneratorHostMesh& mesh, Host& out, std::string& error);

// Builds a companion's container from its solved mesh and motion: one cluster LOD DAG per
// primitive (cones off for a two-sided material), merged, paged, the motion records, the materials
// with the pack's sets for `roles` (one per material), the identity and the generator section.
bool build_companion_container(const assets::MeshData& mesh, flora::Motion& motion,
                               std::span<const char* const> roles,
                               const geometry::ClusterLodOptions& lod, u32 page_bytes,
                               const materials::Pack* pack, geometry::ClusterFileData& out,
                               std::string& error);

// The whole build of a recipe over a host already read. `report`, when given, is the solver's.
bool build_vine_container(const VineFile& file, const Host& host, u64 host_identity,
                          const geometry::ClusterLodOptions& lod, u32 page_bytes,
                          const materials::Pack* pack, geometry::ClusterFileData& out,
                          VineReport* report, std::string& error);
bool build_cover_container(const CoverFile& file, const Host& host, u64 host_identity,
                           const geometry::ClusterLodOptions& lod, u32 page_bytes,
                           const materials::Pack* pack, geometry::ClusterFileData& out,
                           CoverReport* report, std::string& error);

}  // namespace engine::cover

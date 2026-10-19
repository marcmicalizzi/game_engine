#pragma once

// rocks capability (ADR-0027; docs/subsystems/rocks.md, roadmap G62): the deterministic rock
// solver — a shape's parameters and a seed in, a closed mesh out — that makes the engine's
// boulders, outcrops and scree instead of modelled or image-to-3D props, whose failures in E10
// were mostly topology a solver never has (docs/experiments/e10-generated-props.md). It owns the
// shapes' parameter types (schemas/rocks.schema) and the solver; it does not own placement (a
// scatter layer or a scene's instances place what it makes), the tiled textures its UVs are laid
// out for, or a rock's interior for destruction.
//
// The registration points, ADR-0027 decision 2:
//
//   [x] schema types      schemas/rocks.schema: engine.rocks.Boulder, Outcrop, Scree, RockFile
//   [-] scheduler entry   none: the solver runs at content-build time and has no tick
//   [-] render passes     none: a rock is a cluster mesh like any other
//   [x] derived data      the solver's container, keyed by the parameters (rocks.md, "The build")
//   [-] protocol methods  none yet
//   [-] tunables          none: everything the solver reads is a parameter of the file
//   [x] LOD policy        the cluster DAG's, under the triangle budget's ladder (rocks.md)
//   [x] determinism       the same parameters give the same bytes on every toolchain (below)
//   [x] zero cost unused  nothing runs unless a mesh names a `.rock.json`
//   [x] docs, tests, size table
//   [-] bench             no hot path: the solver runs once per parameter set
//
// **What comes out.** An `assets::MeshData` with one primitive and one material — a flat PBR
// constant by rock type — made of one closed, two-manifold, outward-wound body per part (a
// boulder is one part, an outcrop `count`, scree `count`): every edge of a part is shared by
// exactly two of its triangles once the UV seams are welded by position. A body is star-shaped
// about its centre — a cube-sphere grid of directions, each pushed to the body's radius in that
// direction — which is why it is closed by construction and cannot fold; it is built with normal
// cones on (one-sided, closed: geometry.md, "Normal cones").
//
// **Deterministic.** The same parameters give the same bytes on every toolchain and at either CPU
// baseline: the solver's arithmetic is IEEE `+ - * /` and `sqrt` in f64 with contraction off
// (ADR-0035), its sines and cosines are `det::sin`/`det::cos` (core/math/deterministic.h), its
// randomness a splitmix64 over the seed with one stream per part, and it runs on one thread.

#include <core/base/types.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>

#include <schemas/rocks.h>
#include <span>
#include <string>
#include <string_view>

namespace engine::rocks {

/// Determinism stance (ADR-0010): "derived". A rock is content, rebuilt from its parameters
/// wherever it is asked for, and never part of the simulation's state.
inline constexpr const char* k_determinism = "derived";

// The solver's version: part of every build key over a rock file, so a change to what the solver
// makes from the same parameters is a new key and never an old container read as the answer. Bump
// it with any change that moves a byte of a committed recipe's mesh (the determinism test says
// when one did).
inline constexpr u32 k_rocks_version = 1;

// ---- the budget ladder (rocks.md, "The budget ladder") -----------------------------------------
//
// A body is a cube-sphere of `n` x `n` cells a face, 12 n² triangles. The rungs are the `n` it may
// take, coarsest first: a part takes the finest rung its share of the budget holds, so a smaller
// budget walks down the ladder and the coarsest (n = 4, 192 triangles) is still the same rock —
// the same cuts, warp and bedding sampled more coarsely, with the noise finer than the grid can
// carry left out.
inline constexpr u32 k_grid_rungs[] = {4,  6,  8,  10, 12, 16, 20, 24,  28,
                                       32, 40, 48, 56, 64, 80, 96, 112, 128};
inline constexpr u32 k_grid_rung_count = sizeof(k_grid_rungs) / sizeof(k_grid_rungs[0]);
constexpr u32 triangles_of_grid(u32 n) noexcept { return 12u * n * n; }

// How the solver spent the budget: the rungs the parts took and what they came to.
struct RockReport {
  u32 triangles = 0;
  u32 vertices = 0;       // as emitted: a part's six UV charts each carry their own border
  u32 parts = 0;          // bodies: 1, or the outcrop's or the scree's count
  u32 finest_grid = 0;    // the largest part's `n`
  u32 coarsest_grid = 0;  // the smallest part's `n`
  u32 steps = 0;          // rungs below the top of the ladder the largest part took; 0 is n = 128
  u32 cuts = 0;           // planar cuts across every part
  u32 layers = 0;         // bedding layers as built (the type's default when the file gave none)
  f32 rounding = 0.0f;    // as built
};

// The type's name in a file and a report: "Granite".
const char* rock_type_name(RockType type) noexcept;
const char* rock_shape_name(RockShape shape) noexcept;

// ---- the rock file and its build (rocks.md, "The build") ---------------------------------------

// The ranges the schema documents, for the shape the file names: false, with a sentence naming
// the first field out of range and its range, otherwise. A block for another shape is refused,
// and so is an image path in a material (the tiled sets are not wired yet: rocks.md, "Not yet").
bool rock_file_in_range(const RockFile& file, std::string& error);

// Reads a `.rock.json`'s text: an `engine.rocks.RockFile` in range. The shape's block is filled
// with its defaults when the file gives none, so `out` always holds it. False, with a sentence
// naming the field, otherwise; `out` is replaced.
bool read_rock_file(std::string_view text, RockFile& out, std::string& error);

// The file as the build keys it: the schema's JSON of what it says, the shape's block present and
// defaults filled in, compact and in the schema's field order, so two files that say the same thing
// are one parameter set. What a container records in its generator section.
std::string canonical_rock_json(const RockFile& file);

// The source hash a rock container records and its key is taken over: the canonical parameters'
// bytes and `k_rocks_version`.
u64 rock_source_hash(const RockFile& file);

// `geometry::cluster_cache_key(source_hash, lod, false, page_bytes)` — no weld: the UV seams'
// duplicate vertices are the charts' and the solver means them — which is the key `engine-content
// rocks` and the renderer's scene reader both write under.
u64 rock_build_key(u64 source_hash, const geometry::ClusterLodOptions& lod, u32 page_bytes);

// Solves the file's rock (the file must hold its shape's block, as `read_rock_file` leaves it).
// False, with a sentence, when the file is refused or the budget cannot hold the shape at the
// coarsest rung (the sentence says how many triangles that needs). `out` is replaced.
bool solve_rock(const RockFile& file, assets::MeshData& out, RockReport* report = nullptr,
                std::string* error = nullptr);

// Solves the rock and builds its container: one cluster LOD DAG with normal cones, laid out in
// pages of `page_bytes` (0: no page table), with the material, the identity (source hash and build
// key) and the generator section (`rocks`, the shape, the canonical parameters). The source path
// is left to the caller. Deterministic: the same file gives the same bytes on every toolchain.
bool build_rock_container(const RockFile& file, const geometry::ClusterLodOptions& lod,
                          u32 page_bytes, geometry::ClusterFileData& out, RockReport* report,
                          std::string& error);

}  // namespace engine::rocks

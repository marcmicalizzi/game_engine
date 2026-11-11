#pragma once

// matgen capability (ADR-0027): tiled PBR sets by role, generated (docs/subsystems/matgen.md; the
// sources and the numbers, docs/experiments/generated-material-sets.md).
//
// A **recipe** (`engine.matgen.Set`: a role, a kind, a seed, the side one repeat covers in
// millimetres, the texels across it, and the kind's parameters, every one a whole number) builds
// into four images — base colour, a tangent-space normal map (OpenGL, +Y), roughness and height —
// the same four a material pack's set holds (materials.md, "A set"). The build is a pure function
// of the recipe: hashes, IEEE `+ - * /`, `sqrt` and `core/math/deterministic.h`, single-threaded,
// so the bytes are the same from every compiler, instruction set and C library, and a set's key
// (`set_key`: its canonical JSON and `k_matgen_version`) addresses them. Nothing is committed as
// an image: `engine-content matgen` writes a pack directory of them under the git-ignored
// `content/materials/generated/`, which `materials::pack_at` reads behind the fetched pack.
//
// Three kinds:
//   granular  a bed of clasts on a periodic domain (a gravel bed, a sand bar, scree): sizes from
//             the surface's grid-count distribution, shape and roundness by transport, placed
//             largest first, imbricated against the flow, a matrix or voids between
//   rock      a rock face: crystals by modal share, joints, a weathering rind against fresh
//             breaks, crustose lichen by a cover parameter, pits, laminae
//   ground    a floor: moss and soil under dead leaf fragments under live leaves
//
// **A set tiles by construction**: every clast, crystal, joint and fragment lives on the torus of
// the tile, never in the image, so the image is the same picture whichever texel it starts at
// (`Window`), and the tests hold a build that starts half a tile over to the first one's bytes,
// rotated.
//
// The ADR-0027 list, for this capability:
//   [x] schema types      schemas/matgen.schema
//   [-] scheduler entry   none: nothing here runs in a tick
//   [-] render passes     none: a set reaches the renderer as a pack's images do
//   [x] derived data      a set is a derived node keyed by `set_key`; `engine-content matgen`
//   [-] protocol methods  none
//   [-] tunables          none: every parameter is the recipe's, and so is in the key
//   [x] LOD policy        the texture build's mip chain; a set's far form is its mean (the tests
//                         hold the chain's mean to level 0's)
//   [x] determinism       k_determinism: derived, and the same bytes from every toolchain
//   [x] zero cost unused  no registrar, no static state: a build that never calls it pays nothing
//   [x] docs, tests, size table, bench

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <schemas/matgen.h>
#include <string>
#include <string_view>

namespace engine::matgen {

/// Bumped whenever the generator changes so that an old set is no longer what a recipe builds.
/// 1: the first generator (G148, 2026-11-10).
inline constexpr u32 k_matgen_version = 1;

/// Determinism stance (ADR-0010): derived data, never read back into the simulation.
inline constexpr const char* k_determinism = "derived";

/// The ranges `validate_set` holds a recipe to.
inline constexpr u32 k_tile_min_mm = 50;
inline constexpr u32 k_tile_max_mm = 20000;
inline constexpr u32 k_texels_min = 64;
inline constexpr u32 k_texels_max = 4096;

/// Where on the torus the image starts, in whole texels. The set is the same picture from any
/// start; this exists so a test can say so.
struct Window {
  u32 x = 0;
  u32 y = 0;
};

/// A built set: `size` texels a side, rows top first.
struct SetImages {
  u32 size = 0;
  Vector<u8> base_color;  // RGB, sRGB
  Vector<u8> normal;      // RGB, tangent space, OpenGL (+Y up the image)
  Vector<u8> roughness;   // one channel
  Vector<u8> height;      // one channel, the set's own range (SetStats::height_*)
};

/// What a build measured of its own result.
struct SetStats {
  f64 mean_color[3] = {0, 0, 0};  // linear albedo
  f64 mean_roughness = 0;
  f64 height_min_m = 0;
  f64 height_max_m = 0;
  // granular: the clasts drawn from the distribution, those in the top layer and the lower one,
  // the share of the surface a top-layer clast shows on, the grid count over every texel that
  // shows a clast (Wolman's count in its limit: each texel a grid point), the share of the
  // surface that is matrix, and the drawn clasts' mean maximum projection sphericity.
  u32 clasts_drawn = 0;
  u32 clasts_top = 0;
  u32 clasts_under = 0;
  f64 clast_cover = 0;
  f64 d16_mm = 0;
  f64 d50_mm = 0;
  f64 d84_mm = 0;
  f64 matrix_share = 0;
  f64 mean_sphericity = 0;
  // rock: the shares of the surface that are fresh break and lichen.
  f64 fresh_share = 0;
  f64 lichen_cover = 0;
  // ground: the shares of the surface under moss (of the underlay), thatch and live leaves.
  f64 moss_share = 0;
  f64 thatch_cover = 0;
  f64 live_cover = 0;
};

/// A recipe's text read (an unknown field or a value out of range refused, with a sentence).
bool read_set(std::string_view text, Set& out, std::string& error);
/// The recipe's numbers in range; false with a sentence naming the field otherwise.
bool validate_set(const Set& set, std::string& error);
/// The recipe as compact JSON in field order: the bytes the key is over.
std::string canonical_json(const Set& set);
/// The derived node's key: a hash over `canonical_json` and `k_matgen_version`.
u64 set_key(const Set& set);

/// Builds the set. `stats` may be null. False, with `error`, for a recipe out of range. Pure.
bool build_set(const Set& set, SetImages& out, SetStats* stats, std::string& error,
               Window window = {});

/// The sets the engine's solvers ask a pack for by role and no pack has (matgen.md, "The
/// defaults"): `gravel/bed`, `sand/bar`, `rock/scree`, `rock/granite`, `rock/granite_wet`,
/// `rock/basalt`, `rock/limestone`, `rock/sandstone`, `ground/soil`, `ground/moss`,
/// `ground/thatch`, `ground/turf`. Sorted by role.
Vector<Set> default_sets();
/// One of them by role; false when the role has no default.
bool default_set(std::string_view role, Set& out);

}  // namespace engine::matgen

#pragma once

// **The far form of a rigid mesh** (docs/subsystems/geometry.md, "The far form"; roadmap G198): what
// a mesh draws as once it is far, made from the mesh alone — baked on first offer, or carried in its
// container (sections 43 to 47) when a tool wrote one — so an imported boulder and a generated talus
// get the same one and no generator is asked what its far form is.
//
// **What it is.** The mesh's finest level, welded by source vertex, then for each of a few levels of
// rising error `e_k = e_0 * 2^k`: every connected piece of it whose extent is at most `e_k` left out
// (a loose stone a pixel across), and the rest simplified to `e_k` with its open border locked, so
// two meshes that met edge to edge still meet. `e_0` is the mesh's radius over
// `k_far_form_radius_px`: **a level stands in once the mesh's radius projects under
// `k_far_form_radius_px` pixels times its step**, where its error is under a pixel. Each level
// records the error it was held to and the two things that make it up — what the simplifier
// reports and the largest piece left out — so a consumer that knows its projection says where the
// level may draw (`far_form_distance`) instead of trusting a constant.
//
// **Why not the DAG's own cut.** A far ring of the forest's talus is about 40 meshes of up to 2,442
// clusters each, every cluster a pair and a share of the mesh room whether or not it draws; a
// distance form keeps the DAG's top, which over a field of loose stones is still most of it, because
// a cluster DAG coarsens what is connected and never removes a piece. A far form is a few hundred
// triangles a mesh, merged per tile by its consumer into one mesh of a few clusters (the renderer's
// far tile, renderer.md, "The far tile").
//
// Deterministic: meshoptimizer's simplifier (the DAG's own, contraction off tree-wide), integer
// union-find, and sorts on integer keys; the same bytes from every toolchain and thread count.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/geometry/cluster_lod.h>

#include <span>
#include <string>

namespace engine::geometry {

// Moves whenever what a bake makes from the same mesh moves.
inline constexpr u32 k_far_form_version = 1;
// A level's error is the mesh's radius over this, times its step: it stands in for the mesh once
// the radius projects under this many pixels (one pixel of error).
inline constexpr f32 k_far_form_radius_px = 128.0f;
inline constexpr u32 k_far_form_levels = 3;  // e_0, 2 e_0, 4 e_0

// One vertex of the far form: a source vertex of the finest level, its position and its two
// GPU attributes as the mesh carries them. 20 bytes.
struct FarFormVertex {
  Vec3 position{};
  u32 normal_oct = 0;
  u32 uv_half2 = 0;
};

// One material's triangles of one level: `index_count` indices from `first_index` of
// `FarForm::indices`, drawn with the container's material `material`. 16 bytes.
struct FarFormPart {
  u32 first_index = 0;
  u32 index_count = 0;
  u32 material = 0;
  u32 level = 0;
};

// One level, rising. `error_m` is what it was held to (mesh units); `simplify_m` the largest error
// the simplifier reported over its parts, `dropped_m` the largest extent of a piece it left out; the
// level's honest error is the larger of the two, at most `error_m`. 32 bytes.
struct FarFormLevel {
  f32 error_m = 0.0f;
  f32 simplify_m = 0.0f;
  f32 dropped_m = 0.0f;
  u32 triangles = 0;
  u32 kept_pieces = 0;
  u32 dropped_pieces = 0;
  u32 first_part = 0;
  u32 part_count = 0;
};

// The record a container carries (section 43). 48 bytes.
struct FarFormHeader {
  u32 version = k_far_form_version;
  u32 levels = 0;
  u32 source_triangles = 0;  // the finest level's
  u32 pieces = 0;            // connected pieces of the finest level
  Vec3 center{};             // of the finest level's box
  f32 radius = 0.0f;         // the farthest finest vertex from `center`
  f32 radius_px = k_far_form_radius_px;
  u32 pad[3] = {0, 0, 0};
};

static_assert(sizeof(FarFormVertex) == 20, "a far form vertex is 20 bytes");
static_assert(sizeof(FarFormPart) == 16, "a far form part is 16 bytes");
static_assert(sizeof(FarFormLevel) == 32, "a far form level is 32 bytes");
static_assert(sizeof(FarFormHeader) == 48, "a far form header is 48 bytes");

struct FarForm {
  FarFormHeader header;
  Vector<FarFormLevel> levels;
  Vector<FarFormPart> parts;  // every level's, level by level, material by material
  Vector<FarFormVertex> vertices;
  Vector<u32> indices;
  bool empty() const noexcept { return levels.empty(); }
  // The level's honest error: what the simplifier reported or the largest piece left out.
  static f32 level_error(const FarFormLevel& level) noexcept {
    return level.simplify_m > level.dropped_m ? level.simplify_m : level.dropped_m;
  }
};

// Bakes the far form of `mesh` from its finest level (`ClusterLodDesc::level` 0). `material` is
// empty or one material index per cluster (a container's `cluster_material`); empty draws every
// triangle with material 0. Fails, with a sentence, on a mesh with no finest triangles. Pure,
// single-threaded.
bool bake_far_form(const ClusterLodMesh& mesh, std::span<const u32> material, FarForm& out,
                   std::string* error = nullptr);

// The nearest a camera may be to a mesh drawn at `scale` (its largest axis scale) for `level` to
// stand in for it at `threshold_px` under the projection `proj_scale` (`LodView::proj_scale`): the
// level's honest error projected to the threshold.
f32 far_form_distance(const FarFormLevel& level, f32 proj_scale, f32 threshold_px,
                      f32 scale = 1.0f) noexcept;

// A hash over every byte of the far form, for a pin.
u64 far_form_hash(const FarForm& form) noexcept;

}  // namespace engine::geometry

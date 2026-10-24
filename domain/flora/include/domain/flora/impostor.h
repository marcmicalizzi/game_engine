#pragma once

// The impostor chain of a tree (docs/subsystems/flora.md, "The impostor chain"; roadmap R89;
// docs/experiments/tree-allometry.md, "What the AAA pipelines do with a million leaves"): above the
// tree's finest leaves, **authored levels** of its cluster DAG — branch-group cards, one card a
// sub-tree standing for its leaves, merged one order further up at each level — and at the root a
// **whole-tree impostor**, a star of planes through the trunk. Every card and every plane carries a
// view of the solver's own finest geometry, **baked on the CPU** into an atlas (colour with the
// alpha the leaves cut, and a tangent-space normal), so a far card is the near twigs seen from afar
// and needs no lab image. The levels are what `geometry::build_cluster_lod_levels` takes; the
// errors are geometric and honest, so the LOD selector needs no change.
//
// Deterministic like the solver: IEEE + - * / and sqrt only, a rasterizer in integer fixed point,
// and the same bytes from every toolchain (flora.md, "Determinism").

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/assets/gltf.h>
#include <foundation/image/decode.h>

namespace engine::flora {

// One orthographic view the bake draws: from the side `normal` points to, looking back along it,
// over the rectangle [u_min, u_max] x [v_min, v_max] of the plane through `origin` that `u_axis`
// and `v_axis` span. Pixel row j of the tile is v = v_min + (j + 0.5) / rows of the extent, so the
// tile reads with the UV a card's quad carries.
struct BakeView {
  Vec3 origin{};
  Vec3 u_axis{1.0f, 0.0f, 0.0f};
  Vec3 v_axis{0.0f, 1.0f, 0.0f};
  Vec3 normal{0.0f, 0.0f, 1.0f};
  f32 u_min = 0.0f, u_max = 1.0f;
  f32 v_min = 0.0f, v_max = 1.0f;
};

// What a view draws: triangles of the solver's mesh, by range of `mesh.indices` or of a level's own
// indices, and the tile of the atlas it goes in.
struct BakeRange {
  u32 first_index = 0;
  u32 index_count = 0;
};
struct BakeJob {
  BakeView view;
  Vector<BakeRange> ranges;  // into the mesh's indices
  u32 tile = 0;              // row-major in the level's atlas
  // The card the job's geometry is drawn as (its rectangle exactly, no margin) and the tile that
  // card samples, which for a branch-group card is the one its root's hash picks and not always the
  // job's own: what the coverage bake compares the geometry against ("The coverage bake").
  BakeView card;
  u32 card_tile = 0;
  u32 group = 0;  // jobs of one volume are contiguous and are measured together
};

// What an authored level stands for.
enum class ImpostorKind : u32 {
  branch_group = 1,  // three orthogonal cards per subtree, merged `merge` orders up its twigs
  tree = 2,          // the whole tree: four vertical directions, each split into depth slices
};

// One authored level above the finest leaves.
struct TreeLevel {
  ImpostorKind kind = ImpostorKind::branch_group;
  u32 merge = 0;              // orders up the twigs a card's sub-tree reaches; 0 for the tree
  f32 error = 0.0f;           // mesh-space, metres: the cut's (geometry.md, "Authored levels")
  Vector<u32> indices;        // its triangles over the mesh's positions (appended past the finest)
  u32 groups = 0;             // the cards (planes for the tree)
  u32 tile_px = 64;           // a tile's side in texels
  u32 columns = 1, rows = 1;  // the atlas's tiles
  Vector<BakeJob> jobs;       // one per tile drawn
  // The largest distance any vertex of the geometry a card stands for lies from its card's plane
  // (the parallax term of `error`) and half a texel of the largest card (the bake's resolution).
  f32 parallax_m = 0.0f;
  f32 texel_m = 0.0f;
  // How the level nests (geometry's `build_cluster_lod_hierarchy`): every triangle's element — a
  // card, or the star as one — and every element's parent element in the next level (empty for the
  // last), so each card switches on its own sub-tree's sphere.
  Vector<u32> element_of_triangle;
  Vector<u32> parent_of_element;
  u32 elements = 0;
  // The coverage error (R91; flora.md, "The coverage bake"): `feature_m`, the size of what the
  // level stands in for (a leaf for the first branch-group level, a card of the level below for the
  // next), times `difference`, what the level changes at the pixel over a ring of views (0..1).
  // `difference` and `coverage_m` are filled by `measure_level`.
  f32 feature_m = 0.0f;
  f32 difference = 0.0f;
  f32 coverage_m = 0.0f;
};

// Linear factors and optional decoded material images. Pure callers without images use the
// solver colours and optional ellipse outline; content-build hosts supply actual material pixels.
struct BakeLook {
  Vec4 bark{0.27f, 0.22f, 0.17f, 1.0f};
  Vec4 leaf{0.09f, 0.22f, 0.06f, 1.0f};
  bool leaf_outline = false;
  // Optional decoded sRGB base-colour image from the build host, with straight alpha.
  const image::Image* leaf_image = nullptr;
  const image::Image* bark_image = nullptr;
  Vec2 bark_scale{1, 1};
  f32 leaf_cutoff = .5f;
};

// Everything the solver hands the container build beyond its mesh: the levels above the finest
// leaves (rising errors, the tree's last), and where in `mesh.indices` the bark and the finest
// leaves are (what a view draws, and what is bark rather than leaf).
struct TreeLevels {
  Vector<TreeLevel> levels;
  u32 leaf_first_index = 0;  // mesh.indices from here to the finest leaves' end are leaves
  u32 leaf_index_end = 0;
  // Every finest leaf triangle's element of the first level (its sub-tree's card), in index order.
  Vector<u32> leaf_element;
};

// How finely the chain is baked: a branch-group card's tile and a whole-tree view's, in texels;
// zero builds none of that kind.
struct ImpostorSpec {
  u32 card_px = 64;
  u32 tree_px = 256;
  u32 representatives = 16;  // tiles a branch-group level's cards share (flora.md has why)
  u32 most_levels = 6;       // branch-group levels at most
  static constexpr u32 k_card_planes = 3;  // orthogonal views retain edge-on volume
  static constexpr u32 k_tree_slices = 4;  // depth layers in each far-tree vertical view
};

// A baked atlas: RGBA8 colour (sRGB, alpha the coverage the leaves cut) and RGBA8 tangent-space
// normal (xyz remapped to 0..1, alpha the same coverage), `width` x `height`, rows top first.
struct BakedAtlas {
  u32 width = 0;
  u32 height = 0;
  Vector<u8> colour;
  Vector<u8> normal;
};

// Draws one level's jobs over `mesh` into its atlas. `levels.leaf_first_index` says which of the
// mesh's triangles are leaves (shaded by `look.leaf`, cut by the outline when it asks) and which
// bark. Pure and single-threaded; the same bytes on every toolchain.
void bake_level(const assets::MeshData& mesh, const TreeLevels& levels, const TreeLevel& level,
                const BakeLook& look, BakedAtlas& out);

// **The coverage bake** (flora.md, "The coverage bake"; R91): every job's geometry and the card or
// cards that stand for it (the level's own atlas, `baked`), drawn on the CPU by the bake's
// rasterizer from a ring of views — eight azimuths at the horizon and four at 30 degrees above and
// below — at a resolution of four texels to `level.feature_m`, and compared texel by texel: the
// coverage difference (the summed absolute difference over the summed larger coverage, a soft one
// minus the overlap) and the colour difference where both draw, combined as 1 - (1 - a)(1 - c).
// Returns that difference, 0..1. Pure; the same bytes on every toolchain.
f32 measure_level(const assets::MeshData& mesh, const TreeLevels& levels, const TreeLevel& level,
                  const BakeLook& look, const BakedAtlas& baked);

}  // namespace engine::flora

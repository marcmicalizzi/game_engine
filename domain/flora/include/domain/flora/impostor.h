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
};

// What an authored level stands for.
enum class ImpostorKind : u32 {
  branch_group = 1,  // one card a sub-tree, merged `merge` orders up its twigs
  tree = 2,          // the whole tree: a star of planes through the trunk
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
};

// The colours the bake shades with: the species' own factors, linear, whatever a material pack
// later puts on the near parts (flora.md, "The impostor chain", has why), and whether a leaf card
// is cut to a leaf's outline (a pack's card image is) or drawn whole.
struct BakeLook {
  Vec4 bark{0.27f, 0.22f, 0.17f, 1.0f};
  Vec4 leaf{0.09f, 0.22f, 0.06f, 1.0f};
  bool leaf_outline = false;
};

// Everything the solver hands the container build beyond its mesh: the levels above the finest
// leaves (rising errors, the tree's last), and where in `mesh.indices` the bark and the finest
// leaves are (what a view draws, and what is bark rather than leaf).
struct TreeLevels {
  Vector<TreeLevel> levels;
  u32 leaf_first_index = 0;  // mesh.indices from here to the finest leaves' end are leaves
  u32 leaf_index_end = 0;
};

// How finely the chain is baked: a branch-group card's tile and a whole-tree view's, in texels;
// zero builds none of that kind.
struct ImpostorSpec {
  u32 card_px = 64;
  u32 tree_px = 256;
  u32 representatives = 16;  // tiles a branch-group level's cards share (flora.md has why)
  u32 most_levels = 6;       // branch-group levels at most
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

}  // namespace engine::flora

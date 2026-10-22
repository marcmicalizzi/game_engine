#pragma once

// The branching species (docs/subsystems/flora.md, "The broadleaf", "The conifer"):
// `engine.flora.Broadleaf` or `engine.flora.Conifer` in, a mesh and its motion contract out.
//
// **What comes out.** An `assets::MeshData` with two primitives, each its own material:
//
//   0  bark     the trunk and the woody shoots, closed tubes, one-sided, built with cones on
//   1  leaves   the broadleaf's leaf cards or the conifer's foliated-shoot cards, two-sided sprays
//               (one connected surface a twig, or a conifer's branch or lateral), built with cones
//               off
//
// and a `Motion` (motion.h): the trunk part 0, then every woody shoot, then — the broadleaf — every
// twig, each hanging from the shoot it grew on; a conifer's needle sprays, its laterals' included,
// belong to their branch.
// The structure (where every shoot is, how long, which way) never depends on the budget, so the
// parts are the same on every rung of the ladder.
//
// **Deterministic** as the palm is: f64 with IEEE `+ - * /` and `sqrt`,
// `det::sin`/`cos`/`exp`/`log`, splitmix64 streams per order, one thread.

#include <core/base/types.h>
#include <domain/assets/gltf.h>
#include <domain/flora/impostor.h>
#include <domain/flora/motion.h>

#include <schemas/flora.h>
#include <string>

namespace engine::flora {

// How a branching species' solver spent its budget (flora.md, "The budget" for each species).
struct TreeReport {
  u32 triangles = 0;
  u32 wood_triangles = 0;  // the bark primitive
  u32 leaf_triangles = 0;  // the leaves or needles
  u32 vertices = 0;
  u32 parts = 0;           // motion parts
  u32 shoots = 0;          // woody axes, the trunk included
  u32 sprays = 0;          // twigs of leaves, or branches' needle sprays
  u32 cards_per_side = 0;  // on the longest spray, as built
  u32 trunk_sides = 0;
  u32 shoot_sides = 0;  // the finest order's below the trunk, as built
  u32 steps = 0;        // rungs of the ladder walked down; 0 is the full detail
  // The second batch of species (birch, willow, orchard, ancient); zero for the first three.
  u32 fruit_triangles = 0;  // the orchard tree's fruit primitive
  u32 load_bearing = 0;     // parts flagged load-bearing in the structural table
  u32 hollows = 0;          // the ancient tree's
  // The allometric crown (G72: the broadleaf and the ancient tree; zero for the others, except that
  // the conifer (G80) fills leaf_cards and leaf_area_m2).
  u32 twigs = 0;               // terminal twigs in the structure, leafy or not
  u32 orders = 0;              // the most orders from the trunk to a twig, both counted
  u32 leaf_cards = 0;          // cards built, both sides of every spray
  u32 merge_level = 0;         // 0: one card a leaf; m: a card stands for a sub-tree m orders up
  u32 wood_class = 0;          // the finest radius class meshed as wood (0: the twigs themselves)
  u32 part_class = 0;          // the finest radius class that is a motion part of its own
  f32 card_length = 0.0f;      // the longest card built, metres (the leaf's own at merge 0)
  f32 footprint_m2 = 0.0f;     // the crown's footprint: the convex hull of its tips from above
  f32 leaf_area_m2 = 0.0f;     // the one-sided area of the cards built
  f32 leaf_area_index = 0.0f;  // the realised LAI: leaf_area_m2 over footprint_m2
  // The understory (G75; understory.h).
  u32 colliders = 0;  // a shrub's stems flagged k_structural_collider
  // The birch (G81; zero for the others).
  u32 stems = 0;          // stems from the root collar, named or the seed's choice
  f32 twig_slope = 0.0f;  // the terminal twigs' mean direction's height, -1 straight down
};

// One shoot of an allometric crown (G72), for a test or a tool to check the structure against the
// model: the named orders first (the trunk, the limbs, a megaflora's runs), then the recursion
// depth-first. Every row's parent is above it.
struct CrownShoot {
  u32 parent = 0xFFFFFFFFu;
  u32 generation = 0;  // below the named orders
  f32 base_radius = 0.0f;
  f32 tip_radius = 0.0f;
  f32 length = 0.0f;
  u32 flags = 0;
};
inline constexpr u32 k_crown_shoot_terminal = 1u;  // a terminal twig
inline constexpr u32 k_crown_shoot_part = 2u;      // a motion part of its own
inline constexpr u32 k_crown_shoot_named = 4u;     // a shoot of the named orders

// The documented ranges; false with a sentence naming the first field out of range.
bool broadleaf_in_range(const Broadleaf& params, std::string& error);
bool conifer_in_range(const Conifer& params, std::string& error);

// Build the tree. False, with a sentence, for parameters out of range or a budget that cannot hold
// even the coarsest rung. `out` is replaced; `motion`, when given, receives the motion contract.
bool solve_broadleaf(const Broadleaf& params, assets::MeshData& out, TreeReport* report = nullptr,
                     std::string* error = nullptr, Motion* motion = nullptr);
bool solve_conifer(const Conifer& params, assets::MeshData& out, TreeReport* report = nullptr,
                   std::string* error = nullptr, Motion* motion = nullptr);

// ---- the second batch (flora.md, "The birch", "The willow", "The orchard tree", "Megaflora") ----
//
// The same contract as the two above, and beside the motion contract each emits the **structural
// table** (structure.h): `Motion::structure`, one record per part. The orchard tree with fruit
// makes a third primitive, `fruit` (one-sided, cones on). The ancient tree's bark primitive holds
// its buttressed, hollowed trunk as well as its limbs.

bool birch_in_range(const Birch& params, std::string& error);
bool willow_in_range(const Willow& params, std::string& error);
bool orchard_in_range(const Orchard& params, std::string& error);
bool ancient_in_range(const Ancient& params, std::string& error);

bool solve_birch(const Birch& params, assets::MeshData& out, TreeReport* report = nullptr,
                 std::string* error = nullptr, Motion* motion = nullptr);
bool solve_willow(const Willow& params, assets::MeshData& out, TreeReport* report = nullptr,
                  std::string* error = nullptr, Motion* motion = nullptr);
bool solve_orchard(const Orchard& params, assets::MeshData& out, TreeReport* report = nullptr,
                   std::string* error = nullptr, Motion* motion = nullptr);
bool solve_ancient(const Ancient& params, assets::MeshData& out, TreeReport* report = nullptr,
                   std::string* error = nullptr, Motion* motion = nullptr);

// The same two with the impostor chain (impostor.h): `levels` receives it, its cards appended to
// `out` past everything else, so the mesh up to them is the bytes the plain call makes.
bool solve_broadleaf_chain(const Broadleaf& params, assets::MeshData& out, TreeReport* report,
                           std::string* error, Motion* motion, TreeLevels* levels);
bool solve_ancient_chain(const Ancient& params, assets::MeshData& out, TreeReport* report,
                         std::string* error, Motion* motion, TreeLevels* levels);
bool solve_birch_chain(const Birch& params, assets::MeshData& out, TreeReport* report,
                       std::string* error, Motion* motion, TreeLevels* levels);

// The crown's structure as the solver grows it, independent of the budget (G72): what the pipe
// rule, the twig size and the order count are checked against. False for parameters out of range.
bool broadleaf_crown(const Broadleaf& params, Vector<CrownShoot>& out);
bool ancient_crown(const Ancient& params, Vector<CrownShoot>& out);
bool birch_crown(const Birch& params, Vector<CrownShoot>& out);

// The birch's stem count (G81): `stems` when the parameters name one, else the seed's draw from a
// stream of its own — a clump with `clump_chance`, of two stems and one more at even odds up to
// `max_stems`. A seeded birch of n stems is byte for byte the birch with `stems = n`.
u32 birch_stems(const Birch& params);

}  // namespace engine::flora

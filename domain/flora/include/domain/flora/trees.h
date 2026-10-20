#pragma once

// The branching species (docs/subsystems/flora.md, "The broadleaf", "The conifer"):
// `engine.flora.Broadleaf` or `engine.flora.Conifer` in, a mesh and its motion contract out.
//
// **What comes out.** An `assets::MeshData` with two primitives, each its own material:
//
//   0  bark     the trunk and the woody shoots, closed tubes, one-sided, built with cones on
//   1  leaves   the broadleaf's leaf cards or the conifer's needle-bundle cards, two-sided sprays
//               (one connected surface a twig), built with cones off
//
// and a `Motion` (motion.h): the trunk part 0, then every woody shoot, then — the broadleaf — every
// twig, each hanging from the shoot it grew on; a conifer's needle sprays belong to their branch.
// The structure (where every shoot is, how long, which way) never depends on the budget, so the
// parts are the same on every rung of the ladder.
//
// **Deterministic** as the palm is: f64 with IEEE `+ - * /` and `sqrt`,
// `det::sin`/`cos`/`exp`/`log`, splitmix64 streams per order, one thread.

#include <core/base/types.h>
#include <domain/assets/gltf.h>
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
};

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

}  // namespace engine::flora

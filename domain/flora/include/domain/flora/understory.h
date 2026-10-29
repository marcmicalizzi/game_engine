#pragma once

// The understory (docs/subsystems/flora.md, "The understory"; docs/experiments/understory.md): the
// shrub, herb and ground layers under a forest's canopy, roadmap G75. Four species, each a struct
// of the flora schema with its ranges and a solver:
//
//   shrub  `engine.flora.Shrub`: basal stems from a stool, each the allometric recursion (G73)
//          to the species' twig; two primitives, bark (one-sided) and leaves (two-sided)
//   fern   `engine.flora.Fern`: a crown of pinnate fronds, each a stipe and a rachis of pinna
//          cards; two-sided live fronds and an optional retained-dead primitive (G103)
//   tuft   `engine.flora.Tuft`: grass blades from one base, each a tapering strip; one two-sided
//          primitive, the blades
//   herb   `engine.flora.Herb`: a stem, leaves and flowers of petal cards; two two-sided
//          primitives, leaf (the stem and the leaves) and flower, plus optional dead leaves.
//          Basal rosettes keep leaves at the root and omit a nonflowering scape (G103).
//
// Every one emits the motion contract and the structural table (structure.h). Part 0 is the root
// (the stool, the rhizome, the tuft's base, the herb's root): a pivot at the ground with no
// geometry, which every other part hangs from, so nothing of a fern, a tuft or a herb is ever the
// trunk the collision keeps. A shrub's stems are parts of their own, and a stem whose radius at
// the ground is at least `k_shrub_collider_radius_m` is flagged `k_structural_collider`: it stops
// a walker, the rest are walked through and pushed aside (scene_collision.md, "What gets no
// body").
//
// **Deterministic** as the trees are: f64 with IEEE `+ - * /` and `sqrt`, the deterministic
// sines, one splitmix64 stream per part, one thread.

#include <core/base/types.h>
#include <domain/assets/gltf.h>
#include <domain/flora/motion.h>
#include <domain/flora/trees.h>

#include <schemas/flora.h>
#include <string>

namespace engine::flora {

// Fern/herb output only; unrelated flora cache identities remain unchanged.
inline constexpr u32 k_ground_plants_version = 2;

// A shrub's stem stops a walker when its radius at the ground is at least this: 1.25 cm, a stem
// 2.5 cm across. A green stem bent as a cantilever 1 m long by a walker's 0.3 m stride takes
// F = 3 E I d / L^3 with I = pi r^4 / 4 and green hardwood's E of about 9 GPa (the Wood Handbook):
// 150 N, the push a walker leans into, at r = 1.24 cm (docs/experiments/understory.md, "The
// collision threshold"). A hazel's stems are 2 to 8 cm across and stop a walker; a dogwood's are
// thumb-thick and bend aside.
inline constexpr f32 k_shrub_collider_radius_m = 0.0125f;

// The documented ranges, and the detail tier's rule; false with a sentence naming the first field
// out of range. A shrub with any stem that collides refuses `rich`.
bool shrub_in_range(const Shrub& params, std::string& error);
bool fern_in_range(const Fern& params, std::string& error);
bool tuft_in_range(const Tuft& params, std::string& error);
bool herb_in_range(const Herb& params, std::string& error);

// Whether any of the shrub's stems collides: the stoutest's radius is `stem_radius_m`, or the
// allometric radius of the height when that is 0, so this is that radius at least
// `k_shrub_collider_radius_m`.
bool shrub_collides(const Shrub& params) noexcept;

// Build the plant. False, with a sentence, for parameters out of range or a budget that cannot
// hold even the coarsest rung. `out` is replaced; `motion`, when given, receives the motion
// contract and the structural table. The report's `sprays` is the fronds, blades or leafy twigs,
// its `cards_per_side` the most cards a side on one of them.
bool solve_shrub(const Shrub& params, assets::MeshData& out, TreeReport* report = nullptr,
                 std::string* error = nullptr, Motion* motion = nullptr);
bool solve_fern(const Fern& params, assets::MeshData& out, TreeReport* report = nullptr,
                std::string* error = nullptr, Motion* motion = nullptr);
bool solve_tuft(const Tuft& params, assets::MeshData& out, TreeReport* report = nullptr,
                std::string* error = nullptr, Motion* motion = nullptr);
bool solve_herb(const Herb& params, assets::MeshData& out, TreeReport* report = nullptr,
                std::string* error = nullptr, Motion* motion = nullptr);

}  // namespace engine::flora

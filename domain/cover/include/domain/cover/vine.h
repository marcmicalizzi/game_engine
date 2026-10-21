#pragma once

// The vine solver (docs/subsystems/cover.md, "The vine"; roadmap G76): a root climber grown over a
// host's surface by a growth walk — Luft's ivy generator (2007) with the climbing biology of
// docs/experiments/vine-cover.md — and meshed with the flora kit's shapes (flora/companion.h).
//
// **The walk.** Seeds on holding surface at the host's base (and at any anchors). Each step a
// stem's direction is the normalized sum of its last direction, the pull onto the surface
// (adhesion), the light (up, leaning towards the sun), gravity (growing with the run since the
// last hold) and a random turn, each by its weight; a stem that holds keeps to the surface — its
// next point is the closest point of the surface to the step, lifted by its radius — and one that
// does not grows free and stops after `max_free_m`. The rootlets hold where the host's roughness is
// at least `hold_roughness`; on a smoother surface a stem leans on it (it stays outside the
// surface, but nothing holds it there). A held step may branch, into a stem turned by the branch
// angle in the surface's plane, with a share of what its parent has left to grow.
//
// **The mesh.** Part 0 is the vine's root, a pivot at the first seed with no geometry, so the
// walker's collision keeps none of the vine (scene_collision.md, "What gets no body"); every stem
// is a part of its own, never load-bearing, a tube by the pipe model — a stem's base radius
// `tip_radius * sqrt(tips it carries)`, capped at `stem_radius_m`, tapering to the tip radius —
// and its leaves are cards on petioles off its outer side. **The motion contract**: a vertex where
// the stem holds weighs 0 (it is fixed to the host); along a free run the weight rises with the run
// to 1 at a metre, so the hanging tips are what the wind moves. Two primitives: the stems
// (one-sided, `vine/stem`) and the leaves (two-sided, `vine/leaf`).
//
// The vine is a **companion**: its own mesh in the host's frame, placed with the host's transform,
// the host never edited. Static hosts (a rock, a wall) and a tree's holding parts in this round; on
// a host that sways, the vine's vertices will have to ride the host's parts through the motion
// contract, which is not built (cover.md, "Not yet").

#include <core/base/types.h>
#include <domain/assets/gltf.h>
#include <domain/cover/host.h>
#include <domain/flora/motion.h>

#include <schemas/cover.h>
#include <string>

namespace engine::cover {

struct VineReport {
  u32 stems = 0;          // seed stems and branches
  u32 seeds = 0;          // stems started at the base or an anchor
  u32 steps = 0;          // walk steps over every stem
  u32 held_steps = 0;     // steps where the rootlets held (the attachment count)
  u32 leaning_steps = 0;  // steps on a surface too smooth to hold
  u32 free_steps = 0;     // steps off the surface
  u32 leaves = 0;
  u32 triangles = 0;
  u32 row_stride = 1;         // the budget's rung: tube rows every this many of `row_m`
  u32 leaf_stride = 1;        // and one leaf in this many
  f64 length_m = 0.0;         // the stems' total length
  f64 seed_height = 0.0;      // the mean height of the seeds
  f64 tip_height = 0.0;       // the mean height of the seed stems' tips
  f64 max_off_surface = 0.0;  // the farthest a held stem's point stands off the surface
};

// The documented ranges; false with a sentence naming the first field out of range.
bool vine_in_range(const Vine& params, std::string& error);

// One stem of the walk, before it is meshed: its points, whether each held, and its run off the
// surface at each.
struct VineStem {
  Vector<P3> points;
  Vector<P3> normals;  // the surface's normal at each point (the last found one when free)
  Vector<u8> held;
  Vector<f64> free_run;
  u32 parent = ~0u;  // the stem it branched from
  u32 at = 0;        // the parent's point it left from
  u32 depth = 0;
  u32 tips = 1;  // tips it carries, itself included
};

// The walk alone (what the tests measure): the stems, in growth order — every seed's stem, then
// branches in the order they were made.
bool grow_vine(const HostSurface& surface, const HostBvh& bvh, const HostSite& site,
               const Vine& params, Vector<VineStem>& stems, VineReport& report, std::string& error);

// Grows and meshes the vine. `out` and `motion` are replaced.
bool solve_vine(const HostSurface& surface, const HostBvh& bvh, const HostSite& site,
                const Vine& params, assets::MeshData& out, flora::Motion& motion,
                VineReport* report, std::string* error);

}  // namespace engine::cover

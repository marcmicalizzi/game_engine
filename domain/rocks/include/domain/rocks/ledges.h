#pragma once

// Ledges: the resistant beds of the world's formation standing out of a soil-mantled slope
// (roadmap G142; docs/subsystems/rocks.md, "Exposed bedrock (G142)"; the grounding,
// docs/experiments/exposed-bedrock.md), and the scene generator "ledges" that stands them, a
// merged mesh a tile and form.
//
// **The geometry.** A resistant unit (a run of beds at or past the rule's resistance) has a top
// bedding plane. Where the ground has cut below that plane the plane is in the air; the ledge is
// the unit put back under it: a **tread** in the plane, from the line where the plane meets the
// ground (its buried back) out to where it stands `h` above the ground, and a **front** from the
// tread's edge down to the ground and a little under it. `h` is the unit's plumb thickness times
// the point's exposure (exposure.h), so the front is tallest where the slope is steep or the
// ground is a nose and falls to nothing where the exposure ends: the ends taper because the rule
// does, not because a mesh was clipped. Nothing is built behind the tread's back or under the
// front's foot but a skirt.
//
// **Why a lattice of the world.** The tread is the part of each lattice triangle between two
// level lines of functions that are linear on it (the plane's height over the ground, and that
// less `h`), and the front runs along the second line. Every corner value is a function of the
// world's millimetres, and a crossing on a lattice edge is computed from that edge's two ends in
// one order, so two triangles, or two tiles, that share an edge compute the same vertex: a ledge
// that crosses a tile's border meets itself there, and a joint block is the same block whichever
// tile draws it (its index is its place along the formation's strike).
//
// **What it costs and keeps.** A patch is a function of its recipe (the heights, the formation,
// the rule), so it is built by the mesh-generator registry like any recipe and cached by its
// hash. Nothing is kept per frame.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/rocks/exposure.h>
#include <domain/rocks/rocks.h>
#include <domain/scene_gen/scene_gen.h>

#include <schemas/rocks.h>
#include <string>

namespace engine::rocks {

// The registry's names: a scene's `{"generator": "ledges", "params": {...}}`, and the mesh
// generator a `<name>.ledge.json` is built by.
inline constexpr const char* k_ledges_generator = "ledges";
// 2: a block is a body (clefts, rims worn all round, undercut feet, crept blocks), G142c.
// 3: a cleft's flank follows the front's rows and its undercut at the face, G174.
// 4: beds at any attitude, G200: a front recesses along its bed edge's own outward normal and
//    leans by the beds' apparent dip along it; a tread steeper than the soil slope is rock.
inline constexpr u32 k_ledge_version = 4;

// What a solve made, for a test and a host's summary.
struct LedgeReport {
  u32 triangles = 0;
  u32 tread_triangles = 0;  // in a bedding plane, facing up: treads and the bed rows' shelves
  u32 front_triangles = 0;
  u32 above_ground = 0;       // triangles whose centroid is above the ground under it
  f64 front_length_m = 0.0;   // metres of front along the tread's edge, its worn corners too
  f64 corner_length_m = 0.0;  // of which corners worn in plan, at the joints
  f64 tallest_m = 0.0;        // the tallest front above the ground
  u32 units = 0;              // bed-top planes that crop out in the patch
  // A block is a body (rocks.md, "A block is a body"): its flanks in the joints' clefts, the
  // worn rim over them, the undersides of its undercut foot, and the pieces of crept blocks.
  f64 joint_length_m = 0.0;  // metres of flank along the joints, in plan
  f64 joint_worn_m2 = 0.0;   // of worn rim between a tread and a flank
  f64 cleft_depth_m = 0.0;   // the deepest flank: a cleft's depth from the tread to the ground
  f64 undercut_m2 = 0.0;     // of undercut underside, facing down
  // A cleft's flank over an undercut foot (G174): the side of the notch, in the joint's plane,
  // that a flank going straight down would stand past the undercut as a fin. `notch_m2` the
  // flanks that follow the undercut took out; `fin_m2` what was left standing, under a notch too
  // shallow or too low to follow (`k_notch_min_m`).
  f64 notch_m2 = 0.0;
  f64 fin_m2 = 0.0;
  u32 slipped = 0;  // pieces (a block's part in one lattice triangle) of crept blocks
  // A crag has mass (rocks.md, "A crag has mass"): the ground between its ledges, its rock.
  u32 riser_triangles = 0;
  f64 riser_m2 = 0.0;
};

// The ranges `engine.rocks.LedgePatch` documents: false, with a sentence naming the first field
// out of them, otherwise.
bool ledge_patch_in_range(const LedgePatch& patch, std::string& error);

// The patch of `layer` whose corner is (x_mm, z_mm), `tile_mm` a side, at the near or the far
// form: the ground's heights over it and the rule's apron, in whole millimetres. A function of
// the layer, the corner and the ground's heights there.
bool sample_ledge_patch(const LedgeLayer& layer, const scene_gen::Ground& ground, i64 x_mm,
                        i64 z_mm, i64 tile_mm, bool far, LedgePatch& out, std::string& error);

// **A block that has fallen out of a ledge** (rocks.md, "Blocks"): one joint block in five is a
// gap in its bed's front. What a deposit needs to lay the block under its gap: which block it is
// (the same number from every tile), where its gap is, and how big the block was.
struct LedgeGap {
  u64 block = 0;  // its identity: its bed top and its place along the strike
  f64 x = 0.0;    // the gap's centre inside this patch, metres from the patch's corner,
  f64 y = 0.0;    // on the ground
  f64 z = 0.0;
  f64 area_m2 = 0.0;   // of the tread the block would have carried, inside this patch
  f64 length_m = 0.0;  // along the strike, between its two joints
  f64 height_m = 0.0;  // the step it would have stood to, at its tallest in this patch
};
// The patch's fallen blocks, in order of identity, each once. A gap that crosses a tile's border
// is an entry in each tile with one identity and one length; their area-weighted mean is its
// place. The far form has no blocks: an empty list.
bool ledge_gaps(const LedgePatch& patch, Vector<LedgeGap>& out, std::string& error);

// The patch's mesh, metres from its corner; an empty mesh (and true) where nothing crops out.
bool solve_ledges(const LedgePatch& patch, assets::MeshData& out, LedgeReport* report,
                  std::string& error);

u64 ledge_source_hash(const LedgePatch& patch);
// Refuses a patch nothing crops out in ("no ledges"): the generator never writes one.
bool build_ledge_container(const LedgePatch& patch, const geometry::ClusterLodOptions& lod,
                           u32 page_bytes, geometry::ClusterFileData& out, std::string& error,
                           const materials::Pack* pack = nullptr);

}  // namespace engine::rocks

#pragma once

// **A network's water from its sources** (roadmap W120 stage A; channels.md, "Discharge"), and
// the planner as one writer of lines for the profile solver (profile.h).
//
// The discharge at a point of a link is the specific runoff over the point's contributing area
// plus the point sources upstream of it (`ChannelFlow`): a sum down the links, conserved at every
// confluence because the areas are, and carried through a lake of the plan into its outlet
// (W122), less what the lake loses. A link is **wet** from the first point at which that
// discharge, over a metre of its bed, is a sheet's (a critical depth of `wet_depth_m`, the falls'
// own threshold); a link that never reaches it is dry, and says why.
//
// `solve_network` then solves every wet link, downstream links first, so a tributary's last
// station meets the level its main stem was solved to, and writes each as a `WaterChannel`.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/channels/channels.h>
#include <domain/channels/profile.h>

#include <schemas/scene.h>
#include <string>

namespace engine::jobs {
class JobSystem;
}

namespace engine::channels {

// Why a link carries no water.
enum class DryReason : u8 {
  Wet = 0,
  // Its discharge never reaches a sheet's over its bed: an ephemeral gully.
  UnderSheet = 1,
  // Fewer than two of its points are kept (a tributary inside its main stem's banks) or wet.
  TooShort = 2,
};

// One link's water: the discharge at each point of its line, today, m^3/s; the first point that
// is wet (`line.size()` when none is); the link its water goes on into by the cell it ends at
// (~0u: a mouth, the region's edge, or a body between; a source's flow and a lake's loss go on
// through a lake of the plan all the same, `network_flow`); and what it came to.
struct LinkFlow {
  Vector<f64> discharge;
  u32 first_wet = 0;
  u32 down = ~0u;
  DryReason reason = DryReason::Wet;
  f64 tail_m3s = 0;      // the discharge at its last kept point
  f64 tail_sheet_m = 0;  // the critical depth of that discharge over the bed's width there
  f64 sources_m3s = 0;   // of `tail_m3s`, what point sources gave
};

struct NetworkFlow {
  Vector<LinkFlow> links;
  u32 wet = 0;
  u32 dry_under_sheet = 0;
  u32 dry_too_short = 0;
  u32 sources = 0;           // point sources that joined a link
  f64 wet_m = 0;             // metres of line that carry water
  f64 dry_m = 0;             // metres of kept line that do not
  f64 largest_m3s = 0;       // the largest discharge at any point
  f64 smallest_wet_m3s = 0;  // the smallest discharge a wet link ends with
  // W122: the plan's lakes that lose water (evaporation, seepage, a leak) and have an outlet, and
  // what was taken off their outlets and everything below them, m3/s, each lake's once.
  u32 lake_outlets = 0;
  f64 lake_loss_m3s = 0;
};

// `ChannelFlow`'s numbers in range. False with a sentence.
bool flow_in_range(const ChannelFlow& flow, std::string& error);

// **The discharge down a network.** False with a sentence for numbers out of range and for a
// source that stands further than `snap_m` from every channel point (named, with the distance).
bool network_flow(const ChannelLayer& layer, const ChannelNetwork& network, NetworkFlow& out,
                  std::string& error);

// One link solved: its profile and the block the description carries (`reach` and the flood
// bed's reaches at each station, and the bank's reach where the recipe moved it, as the planner's
// writer always wrote them). Empty for a dry link.
struct SolvedLink {
  bool wet = false;
  Profile profile;
  scene::WaterChannel block;
};
// What a link's block declares as its writer (`WaterChannel.derived_from`): the cook replaces
// the blocks that carry it and solves the ones that carry none, and reads no name to tell which.
inline std::string flow_writer(const ChannelLayer& layer) { return "channels " + layer.name; }
// **The planner's lines, solved** on `ground` — the composed ground the scene draws, cuts and
// all. Links are solved downstream first; `jobs` (nullable) solves the links of one generation a
// job each, and the result does not depend on it. `out` holds one entry a link, in link order.
bool solve_network(const ChannelLayer& layer, const ChannelNetwork& network,
                   const NetworkFlow& flow, const scene_gen::Ground& ground,
                   const ProfileOptions& options, jobs::JobSystem* jobs, Vector<SolvedLink>& out,
                   std::string& error);

}  // namespace engine::channels

#pragma once

// **The stations of a solved description** (roadmap W120; channels.md, "The water's description",
// "Stations"): which points of a profile a `WaterChannel` carries. The profile is solved on nodes
// one ground lattice apart; the description keeps the fewest of them such that a reader who joins
// neighbours by straight lines stands within the pinned bounds of the profile everywhere, so the
// draw needs no curve and no knowledge of the solver.

#include <core/base/types.h>
#include <core/containers/vector.h>

namespace engine::channels {

// **The pins** (the contract with the draw; W120's stage 0). Between two neighbouring stations
// that are not a jump's or a fall's pair, the straight line between them stands within
// `k_station_level_m` of the profile's level and within `k_station_wet_m` of each wet reach at
// every node between; two neighbours are never farther apart along the line than
// `k_station_max_m`, and never closer than the ground's lattice (the nodes' own spacing, which the
// caller keeps: the selection only ever drops nodes).
inline constexpr f64 k_station_level_m = .01;
inline constexpr f64 k_station_wet_m = .05;
inline constexpr f64 k_station_max_m = 8.0;
// What the selection holds a chord to: the pins less the half millimetre each end's level and the
// half millimetre each end's reach may move when the description rounds them.
inline constexpr f64 k_station_round_m = .001;

// A node of the solved profile: its distance along the line, metres; the water's level; the wet
// reach each side of the line; whether the description must carry it (a control, a link's ends, a
// jump's or a fall's station); and whether it and the next node are a discontinuity's pair (a
// jump's head, a fall's lip), between which no line is drawn and no node is tested.
struct StationNode {
  f64 s = 0;
  f64 level = 0;
  f64 wet_left = 0;
  f64 wet_right = 0;
  bool keep = false;
  bool pair = false;
};

// The indices of the nodes the description carries, ascending: the first and the last, every
// `keep` node, both nodes of every pair, and between them the fewest that hold the pins (greedy
// from upstream: each station is the farthest node the chord from the one before reaches within
// the bounds). Nodes must ascend in `s`.
inline Vector<u32> select_stations(const Vector<StationNode>& nodes) {
  Vector<u32> out;
  const u32 n = nodes.size();
  if (n == 0) return out;
  out.push_back(0);
  u32 i = 0;
  while (i + 1 < n) {
    u32 j = i + 1;
    if (!nodes[i].pair && !nodes[j].keep) {
      // Extend the chord from i while every node under it holds.
      while (j + 1 < n) {
        const u32 e = j + 1;
        const f64 run = nodes[e].s - nodes[i].s;
        if (run > k_station_max_m) break;
        bool ok = true;
        for (u32 k = i + 1; k < e && ok; ++k) {
          const f64 u = run > 0 ? (nodes[k].s - nodes[i].s) / run : 0.0;
          const f64 level = nodes[i].level + u * (nodes[e].level - nodes[i].level);
          const f64 left = nodes[i].wet_left + u * (nodes[e].wet_left - nodes[i].wet_left);
          const f64 right = nodes[i].wet_right + u * (nodes[e].wet_right - nodes[i].wet_right);
          const f64 dl = level - nodes[k].level;
          const f64 da = left - nodes[k].wet_left;
          const f64 db = right - nodes[k].wet_right;
          ok = (dl < 0 ? -dl : dl) <= k_station_level_m - k_station_round_m &&
               (da < 0 ? -da : da) <= k_station_wet_m - k_station_round_m &&
               (db < 0 ? -db : db) <= k_station_wet_m - k_station_round_m;
        }
        if (!ok) break;
        j = e;
        if (nodes[j].keep) break;  // a kept node ends the chord: it is a station
      }
    }
    out.push_back(j);
    i = j;
  }
  return out;
}

}  // namespace engine::channels

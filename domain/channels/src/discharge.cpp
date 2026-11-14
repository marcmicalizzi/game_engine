// A network's water from its sources, and the planner's lines solved (discharge.h; channels.md,
// "Discharge"; roadmap W120 stage A).
#include "detail.h"

#include <core/jobs/job_system.h>
#include <domain/channels/discharge.h>
#include <domain/channels/falls.h>
#include <domain/channels/hydraulics.h>

#include <algorithm>
#include <cmath>

namespace engine::channels {

using detail::bend_section;
using detail::in;
using detail::round_to;
using detail::wide;

bool flow_in_range(const ChannelFlow& flow, std::string& error) {
  if (!in(wide(flow.runoff_l_s_km2), 0, 200)) return detail::fail(error, "flow runoff_l_s_km2");
  if (!in(wide(flow.stage), 0, 100)) return detail::fail(error, "flow stage");
  if (!in(wide(flow.wet_depth_m), .001, 1)) return detail::fail(error, "flow wet_depth_m");
  if (!in(wide(flow.snap_m), 0, 1000)) return detail::fail(error, "flow snap_m");
  for (const ChannelSource& source : flow.sources) {
    if (!(source.flow_m3s > 0.0f) || !in(wide(source.flow_m3s), 0, 10000))
      return detail::fail(error, "flow source \"" + source.name + "\" flow_m3s");
    if (!in(wide(source.season), 0, 1))
      return detail::fail(error, "flow source \"" + source.name + "\" season");
    if (!std::isfinite(source.at.x) || !std::isfinite(source.at.z))
      return detail::fail(error, "flow source \"" + source.name + "\" at");
  }
  return true;
}

bool network_flow(const ChannelLayer& layer, const ChannelNetwork& network, NetworkFlow& out,
                  std::string& error) {
  out = NetworkFlow{};
  const ChannelFlow& flow = layer.flow;
  if (!flow_in_range(flow, error)) return false;
  const u32 count = network.links.size();
  out.links.resize(count);
  // The link each link's water goes on into: the one that begins at the cell this one ends at.
  {
    Vector<std::pair<u32, u32>> heads;  // (first cell, link)
    for (u32 k = 0; k < count; ++k)
      if (!network.links[k].cells.empty() && !network.links[k].outlet)
        heads.push_back({network.links[k].cells.front(), k});
    std::sort(heads.begin(), heads.end());
    for (u32 k = 0; k < count; ++k) {
      const ChannelLink& link = network.links[k];
      if (link.cells.empty() || link.mouth) continue;
      const auto it =
          std::lower_bound(heads.begin(), heads.end(), std::pair<u32, u32>{link.cells.back(), 0u});
      for (auto h = it; h != heads.end() && h->first == link.cells.back(); ++h)
        if (h->second != k) {
          out.links[k].down = h->second;
          break;
        }
    }
  }
  // Rain over the catchment: litres a second a square kilometre, times the area, times the stage.
  const f64 runoff = wide(flow.runoff_l_s_km2) * 1e-3 * 1e-6 * wide(flow.stage);
  for (u32 k = 0; k < count; ++k) {
    const ChannelLink& link = network.links[k];
    LinkFlow& lf = out.links[k];
    lf.discharge.reserve(link.line.size());
    for (const ChannelPoint& p : link.line)
      lf.discharge.push_back(runoff * wide(p.area_m2));
  }
  // The point sources: each joins the nearest kept point, and everything downstream of it.
  Vector<f64> from_sources(count, 0.0);
  for (const ChannelSource& source : flow.sources) {
    u32 best_link = ~0u, best_point = 0;
    f64 best = 1e300;
    for (u32 k = 0; k < count; ++k) {
      const ChannelLink& link = network.links[k];
      const u32 kept = std::min<u32>(link.keep, link.line.size());
      for (u32 i = 0; i < kept; ++i) {
        const f64 dx = link.line[i].at.x - source.at.x, dz = link.line[i].at.z - source.at.z;
        const f64 d2 = dx * dx + dz * dz;
        if (d2 < best) {
          best = d2;
          best_link = k;
          best_point = i;
        }
      }
    }
    const f64 distance = std::sqrt(best);
    if (best_link == ~0u || distance > wide(flow.snap_m)) {
      error = "engine.channels: the source \"" + source.name + "\" stands on no channel: the " +
              "nearest channel point is " +
              (best_link == ~0u ? std::string("none")
                                : std::to_string(round_to(distance, 10)) + " m away") +
              ", further than flow.snap_m";
      return false;
    }
    const f64 gives =
        wide(source.flow_m3s) *
        (source.kind == ChannelSourceKind::Melt ? wide(source.season) : wide(flow.stage));
    ++out.sources;
    u32 k = best_link, from = best_point;
    for (u32 hops = 0; k != ~0u && hops <= count; ++hops) {
      LinkFlow& lf = out.links[k];
      for (u32 i = from; i < lf.discharge.size(); ++i)
        lf.discharge[i] += gives;
      from_sources[k] += gives;
      k = lf.down;
      from = 0;
    }
  }
  // Where each link is wet: from the first kept point whose discharge over a metre of its bed is
  // a sheet's.
  const f64 sheet = wide(flow.wet_depth_m);
  bool any_wet = false;
  for (u32 k = 0; k < count; ++k) {
    const ChannelLink& link = network.links[k];
    LinkFlow& lf = out.links[k];
    const u32 kept = std::min<u32>(link.keep, link.line.size());
    const Vector<f64> s = [&] {
      Vector<f64> along;
      along.push_back(0.0);
      for (u32 i = 1; i < link.line.size(); ++i) {
        const f64 dx = link.line[i].at.x - link.line[i - 1].at.x;
        const f64 dz = link.line[i].at.z - link.line[i - 1].at.z;
        along.push_back(along[i - 1] + std::sqrt(dx * dx + dz * dz));
      }
      return along;
    }();
    const f64 kept_m = kept > 0 ? s[kept - 1] : 0.0;
    lf.first_wet = link.line.size();
    lf.sources_m3s = from_sources[k];
    if (kept < 2) {
      lf.reason = DryReason::TooShort;
      ++out.dry_too_short;
      continue;
    }
    const auto sheet_depth = [&](u32 i) {
      const f64 w = wide(link.line[i].width_m);
      return w > 0.0 ? critical_depth(lf.discharge[i] / w) : 0.0;
    };
    lf.tail_m3s = lf.discharge[kept - 1];
    lf.tail_sheet_m = sheet_depth(kept - 1);
    for (u32 i = 0; i < kept; ++i) {
      out.largest_m3s = std::max(out.largest_m3s, lf.discharge[i]);
      if (lf.first_wet == link.line.size() && sheet_depth(i) >= sheet) lf.first_wet = i;
    }
    if (lf.first_wet == link.line.size()) {
      lf.reason = DryReason::UnderSheet;
      ++out.dry_under_sheet;
      out.dry_m += kept_m;
    } else if (kept - lf.first_wet < 2) {
      lf.reason = DryReason::TooShort;
      lf.first_wet = link.line.size();
      ++out.dry_too_short;
      out.dry_m += kept_m;
    } else {
      lf.reason = DryReason::Wet;
      ++out.wet;
      out.wet_m += kept_m - s[lf.first_wet];
      out.dry_m += s[lf.first_wet];
      out.smallest_wet_m3s = any_wet ? std::min(out.smallest_wet_m3s, lf.tail_m3s) : lf.tail_m3s;
      any_wet = true;
    }
  }
  return true;
}

bool solve_network(const ChannelLayer& layer, const ChannelNetwork& network,
                   const NetworkFlow& flow, const scene_gen::Ground& ground,
                   const ProfileOptions& options, jobs::JobSystem* jobs, Vector<SolvedLink>& out,
                   std::string& error) {
  const u32 count = network.links.size();
  out.clear();
  out.resize(count);
  if (flow.links.size() != count) {
    error = "engine.channels: the flow is not this network's";
    return false;
  }
  // A link's generation: how many links its water passes through after it. A generation is
  // solved after the one below it, so every link meets a level that is already solved.
  Vector<u32> generation(count, 0);
  u32 last = 0;
  for (u32 k = 0; k < count; ++k) {
    u32 hops = 0;
    for (u32 d = flow.links[k].down; d != ~0u && hops < count; d = flow.links[d].down)
      ++hops;
    generation[k] = hops;
    last = std::max(last, hops);
  }
  const ChannelLayer bank_defaults;
  Vector<std::string> errors;
  errors.resize(count);
  for (u32 g = 0; g <= last; ++g) {
    Vector<u32> todo;
    for (u32 k = 0; k < count; ++k)
      if (generation[k] == g && flow.links[k].reason == DryReason::Wet) todo.push_back(k);
    const auto solve = [&](u32 begin, u32 end) {
      for (u32 t = begin; t < end; ++t) {
        const u32 k = todo[t];
        const ChannelLink& link = network.links[k];
        const LinkFlow& lf = flow.links[k];
        const u32 kept = std::min<u32>(link.keep, link.line.size());
        FlowLine line;
        line.name = layer.name + " link " + std::to_string(k);
        Vector<f64> along;
        for (u32 i = lf.first_wet; i < kept; ++i) {
          const ChannelPoint& p = link.line[i];
          if (!line.points.empty()) {
            const WorldPos& before = line.points.back();
            const f64 dx = p.at.x - before.x, dz = p.at.z - before.z;
            along.push_back(along.back() + std::sqrt(dx * dx + dz * dz));
          } else {
            along.push_back(0.0);
          }
          line.points.push_back(WorldPos{p.at.x, 0.0, p.at.z});
          line.discharge.push_back(lf.discharge[i]);
          // The cut's width there (channels.md, "The section"): the hydraulic geometry's with its
          // swell, as `terrain_cuts_json` writes it.
          line.width.push_back(bend_section(layer, wide(p.width_m), wide(p.depth_m),
                                            i < link.asym.size() ? wide(link.asym[i]) : 0.0,
                                            i < link.swell.size() ? wide(link.swell[i]) : 0.0)
                                   .width);
        }
        if (link.mouth) {
          line.tail_level = true;
          line.level = round_to(link.mouth_level, 1e3);
          line.tail = scene_gen::ChannelControl::Mouth;
        } else if (lf.down != ~0u && out[lf.down].wet && !out[lf.down].profile.stations.empty()) {
          line.tail_level = true;
          line.level = out[lf.down].profile.stations[0].level;
          line.tail = scene_gen::ChannelControl::Confluence;
        }
        line.head_spill = link.outlet && lf.first_wet == 0;
        SolvedLink& solved = out[k];
        if (!solve_profile(line, ground, options, solved.profile, errors[k])) continue;
        solved.wet = true;
        solved.block = describe_profile(line.name, solved.profile, link.seed);
        // What the planner's writer always carried beside the water: the reach's code at each
        // station and the flood bed's reach each side (the cut's own numbers, for the placements'
        // exclusion), read at the line's point nearest the station.
        bool flooded = false;
        for (u32 i = lf.first_wet; i < kept && !flooded; ++i)
          flooded = i < link.flood_left.size() && link.flood_left[i] > 0;
        for (const ProfileStation& station : solved.profile.stations) {
          u32 j = static_cast<u32>(std::lower_bound(along.begin(), along.end(), station.s) -
                                   along.begin());
          if (j >= along.size()) j = along.size() - 1;
          if (j > 0 && station.s - along[j - 1] < along[j] - station.s) --j;
          const u32 i = lf.first_wet + j;
          solved.block.reach.push_back(i < link.reach.size() ? link.reach[i] : u8{0});
          if (flooded) {
            solved.block.flood_left_m.push_back(static_cast<f32>(
                round_to(i < link.flood_left.size() ? wide(link.flood_left[i]) : 0.0, 1e3)));
            solved.block.flood_right_m.push_back(static_cast<f32>(
                round_to(i < link.flood_right.size() ? wide(link.flood_right[i]) : 0.0, 1e3)));
          }
        }
        if (layer.bank_width_widths != bank_defaults.bank_width_widths ||
            layer.bank_width_m != bank_defaults.bank_width_m) {
          solved.block.bank_width_widths = wide(layer.bank_width_widths);
          solved.block.bank_width_m = wide(layer.bank_width_m);
        }
      }
    };
    if (jobs != nullptr && todo.size() > 1)
      jobs->parallel_for(jobs::Pool::Performance, todo.size(), 1,
                         [&](u32 begin, u32 end) { solve(begin, end); });
    else
      solve(0, todo.size());
    for (const u32 k : todo)
      if (!errors[k].empty()) {
        error = errors[k];
        return false;
      }
  }
  return true;
}

}  // namespace engine::channels

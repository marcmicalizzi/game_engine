// The bed's surface sediment, point by point down every link (docs/subsystems/channels.md, "Bed
// sediment"; the sources and every number's status in docs/experiments/channels.md, "Bed
// sediment"): part of the plan, so the generator's clasts, the water's reading of the bed (S101)
// and anything else that asks get the same answer and the plan's key moves with it.
#include "detail.h"

#include <algorithm>

namespace engine::channels {
namespace detail {
namespace {

// Lamb, Dietrich & Venditti 2008 (JGR 113, F02008, eq. 3, read in full): the critical Shields
// number rises with the bed's slope, tau*c = 0.15 S^0.25 over flume and field data from S = 0.001
// to 0.1. A bed's surface median is taken as what the bankfull flow just moves there, the threshold
// channel's assumption (Parker et al. 2007's quasi-universal gravel rivers, read in a secondary):
// D50 = h S / (R tau*), R = 1.65 the submerged specific gravity of quartz.
constexpr f64 k_shields_a = .15;
constexpr f64 k_shields_b = .25;
constexpr f64 k_submerged = 1.65;
// The slope a decision reads, low-passed over this many bankfull widths (and at least the metres)
// so a lattice step does not make a grain-size step.
constexpr f64 k_slope_window_widths = 5;
constexpr f64 k_slope_window_min_m = 10;

// **Sorting by reach**, sigma in phi units (Bunte & Abt 2001, RMRS-GTR-74 p. 68, read in full:
// about 0.5 after long fluvial transport, 1.5 to 2 in mountain gravel-bed streams spanning sand to
// boulders; Montgomery & Buffington 1997's Finney Creek counts, read in full, give D84/D50 of 3.4
// on its pool-riffle and 3.1 on its cascade reaches, sigma 1.6 to 1.8). The value a reach takes in
// that range is this row's choice, by the reach's place from lowland to mountain.
f64 sorting_of(ChannelReach r) {
  switch (r) {
    case ChannelReach::DuneRipple: return .6;
    case ChannelReach::PoolRiffle: return 1.2;
    case ChannelReach::PlaneBed: return 1.5;
    default: return 1.75;
  }
}
// **Relative roughness**, d90 over the bankfull depth, at most (Montgomery & Buffington 1997, read
// in full: pool-riffle under about 0.3, plane-bed 0.2 to 0.8, step-pool 0.3 to 0.8, the cascade's
// largest clasts approaching the bankfull depth).
f64 roughness_of(ChannelReach r) {
  switch (r) {
    case ChannelReach::DuneRipple:
    case ChannelReach::PoolRiffle: return .3;
    case ChannelReach::PlaneBed:
    case ChannelReach::StepPool: return .8;
    default: return 1.0;
  }
}
// Bedrock reaches are rock with a thin, patchy alluvial cover (Montgomery & Buffington 1997): the
// share of the bed a bedrock reach keeps of what its sediment would cover. A design choice.
constexpr f64 k_bedrock_cover = .3;
// A bar's run: points whose section is a bend's to at least this much of its asymmetry, all on one
// side. Its surface median fines from head to tail by a third (Rice & Church 2010, the abstract:
// 33 % for the surface on the Fraser's bars): 1.2 of the reach's at the head, 0.8 at the tail.
constexpr f64 k_bar_asym = .25;

}  // namespace

f64 normal_cdf(f64 x) {
  // The logistic approximation, 1 / (1 + e^(-1.702 x)), about 0.01 from the normal at worst (from
  // memory; the clasts' size test holds what it draws against `erfc`), with the engine's
  // exponential: the same bits on every toolchain.
  if (x > 30) return 1;
  if (x < -30) return 0;
  return 1 / (1 + det::exp(-1.702 * x));
}
f64 log2_of(f64 x) { return det::log(x) / k_ln2; }
f64 exp2_of(f64 x) { return det::exp(x * k_ln2); }

ChannelSediment sediment_of(const ChannelLayer& p, ChannelReach reach, f64 depth_m, f64 slope,
                            f64 transport_m) {
  ChannelSediment out;
  const f64 h = std::max(depth_m, 1e-3);
  const f64 s = std::clamp(slope, 1e-4, .5);
  const f64 tau = k_shields_a * powd(s, k_shields_b);
  const f64 sigma = sorting_of(reach);
  f64 d50 = h * s / (k_submerged * tau);
  // d90 = d50 2^(1.2816 sigma) under the reach's relative roughness.
  d50 = std::min(d50, roughness_of(reach) * h / exp2_of(1.2816 * sigma));
  if (reach == ChannelReach::DuneRipple) d50 = std::min(d50, .002);  // sand
  d50 = std::max(d50, .0005);
  const f64 spread = exp2_of(sigma);
  out.d50_m = static_cast<f32>(d50);
  out.d84_m = static_cast<f32>(d50 * spread);
  out.d16_m = static_cast<f32>(d50 / spread);
  out.sorting = static_cast<f32>(sigma);
  const f64 rounding = std::max(1.0, wide(p.rounding_m));
  out.roundness =
      static_cast<f32>(std::clamp(1 - det::exp(-std::max(0.0, transport_m) / rounding), .05, .95));
  // The share of the surface a pebble count would find at or above the drawn size: a pebble count
  // samples by area (Wolman 1954), so its distribution is the share of the bed's surface.
  const f64 psi_min = log2_of(std::max(1.0, wide(p.clast_min_mm)) / 1000.0);
  f64 cover = 1 - normal_cdf((psi_min - log2_of(d50)) / sigma);
  if (reach == ChannelReach::Bedrock) cover *= k_bedrock_cover;
  out.cover = static_cast<f32>(cover);
  out.transport_m = static_cast<f32>(transport_m);
  return out;
}

void plan_sediment(const ChannelLayer& p, ChannelNetwork& n) {
  const u32 links = n.links.size();
  // How far the network has carried the bed at each link's start: the longest path from a head
  // down to it, the links taken upstream first (contributing area only grows downstream).
  Vector<f64> upstream(links, 0.0);
  Vector<u32> order(links);
  for (u32 k = 0; k < links; ++k)
    order[k] = k;
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
    return n.links[a].area_end != n.links[b].area_end ? n.links[a].area_end < n.links[b].area_end
                                                      : a < b;
  });
  for (const u32 k : order) {
    const ChannelLink& l = n.links[k];
    if (l.cells.empty()) continue;
    for (u32 j = 0; j < links; ++j) {
      const ChannelLink& below = n.links[j];
      if (j != k && !below.cells.empty() && below.cells.front() == l.cells.back())
        upstream[j] = std::max(upstream[j], upstream[k] + l.length_m);
    }
  }
  for (u32 k = 0; k < links; ++k) {
    ChannelLink& link = n.links[k];
    const u32 m = link.line.size();
    link.sediment.assign(m, ChannelSediment{});
    if (m == 0) continue;
    Vector<f64> s(m, 0.0);
    for (u32 i = 1; i < m; ++i) {
      const f64 dx = link.line[i].at.x - link.line[i - 1].at.x;
      const f64 dz = link.line[i].at.z - link.line[i - 1].at.z;
      s[i] = s[i - 1] + std::sqrt(dx * dx + dz * dz);
    }
    // The slope, a running mean over the window (two pointers, in point order).
    Vector<f64> prefix(m + 1, 0.0);
    for (u32 i = 0; i < m; ++i)
      prefix[i + 1] = prefix[i] + std::max(0.0, wide(link.line[i].slope));
    u32 lo = 0, hi = 0;
    for (u32 i = 0; i < m; ++i) {
      const f64 half =
          .5 * std::max(k_slope_window_min_m, k_slope_window_widths * wide(link.line[i].width_m));
      while (lo < i && s[lo] < s[i] - half)
        ++lo;
      if (hi < i) hi = i;
      while (hi + 1 < m && s[hi + 1] <= s[i] + half)
        ++hi;
      const f64 slope = (prefix[hi + 1] - prefix[lo]) / static_cast<f64>(hi + 1 - lo);
      const ChannelReach reach = i < link.reach.size() ? static_cast<ChannelReach>(link.reach[i])
                                                       : ChannelReach::PoolRiffle;
      link.sediment[i] =
          sediment_of(p, reach, wide(link.line[i].depth_m), slope, upstream[k] + s[i]);
    }
    // The bars' runs: the inner bank of a bend, head to tail.
    if (link.asym.size() == m) {
      for (u32 i = 0; i < m;) {
        const f64 a = wide(link.asym[i]);
        if (std::fabs(a) < k_bar_asym) {
          ++i;
          continue;
        }
        u32 e = i;
        while (e + 1 < m && std::fabs(wide(link.asym[e + 1])) >= k_bar_asym &&
               (wide(link.asym[e + 1]) > 0) == (a > 0))
          ++e;
        const f64 length = s[e] - s[i];
        for (u32 q = i; q <= e; ++q)
          link.sediment[q].bar = static_cast<f32>(length > 0 ? (s[q] - s[i]) / length : .5);
        i = e + 1;
      }
    }
  }
}

}  // namespace detail
}  // namespace engine::channels

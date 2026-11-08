// The network (docs/subsystems/channels.md, "The network"; grounding in
// docs/experiments/channels.md): the lattice, the fill, the routing, the heads, the links and their
// lines. Everything a decision rests on is an integer or IEEE arithmetic with no C library call
// (the det:: functions stand in for exp, log, sin and cos), so the network is the same on every
// toolchain and at every thread count.
#include "detail.h"

#include <core/jobs/job_system.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::channels {
namespace {
using namespace detail;

constexpr i32 k_dx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr i32 k_dz[8] = {0, 1, 1, 1, 0, -1, -1, -1};
constexpr f64 k_sqrt2 = 1.4142135623730951;

struct Node {
  i64 h;
  u32 i;
};
// A min-heap on (height, index): a total order, so the flood visits the cells in one order.
bool after(const Node& a, const Node& b) { return a.h != b.h ? a.h > b.h : a.i > b.i; }

f64 ground_at(const scene_gen::Ground& ground, f64 x, f64 z) {
  return ground.surface_mm(scene_gen::nearest_mm(x), scene_gen::nearest_mm(z));
}

// The line of one link: the cells' centres low-passed, resampled at the step, bent by the meander
// rule inside the trough, and given its widths, depths and the bed's fall.
void build_line(const ChannelLayer& p, const ChannelNetwork& n, const scene_gen::Ground& ground,
                ChannelLink& link) {
  const f64 spacing = static_cast<f64>(n.spacing_mm) / 1000.0;
  const f64 cell_area = spacing * spacing;
  const u32 count = link.cells.size();
  Vector<f64> x(count), z(count), s(count), area(count), w(count);
  for (u32 j = 0; j < count; ++j) {
    const u32 c = link.cells[j];
    x[j] = static_cast<f64>(n.x_mm + static_cast<i64>(c % n.width) * n.spacing_mm) / 1000.0;
    z[j] = static_cast<f64>(n.z_mm + static_cast<i64>(c / n.width) * n.spacing_mm) / 1000.0;
    area[j] = static_cast<f64>(n.area_cells[c]) * cell_area;
    w[j] = hydraulic(wide(p.width_at_km2_m), wide(p.width_exponent), area[j]);
    s[j] = j == 0 ? 0.0
                  : s[j - 1] + std::sqrt((x[j] - x[j - 1]) * (x[j] - x[j - 1]) +
                                         (z[j] - z[j - 1]) * (z[j] - z[j - 1]));
  }
  const f64 cells_length = s[count - 1];
  link.slope = cells_length > 0
                   ? std::max(0.0, static_cast<f64>(n.ground_um[link.cells[0]] -
                                                    n.ground_um[link.cells[count - 1]]) /
                                       1e6 / cells_length)
                   : 0.0;
  // Low-pass: a Gaussian along the length, its sigma the larger of a floor and a few widths (the
  // lattice's 45-degree steps are not a channel's form), eased to nothing at the two ends so a link
  // meets its junctions where the lattice put them.
  Vector<f64> qx(count), qz(count);
  for (u32 j = 0; j < count; ++j) {
    const f64 sigma = std::max(wide(p.smooth_min_m), wide(p.smooth_widths) * w[j]);
    if (sigma <= 0 || j == 0 || j + 1 == count) {
      qx[j] = x[j];
      qz[j] = z[j];
      continue;
    }
    f64 sx = 0, sz = 0, sw = 0;
    for (u32 k = 0; k < count; ++k) {
      const f64 d = s[k] - s[j];
      if (d < -3 * sigma) continue;
      if (d > 3 * sigma) break;
      const f64 weight = det::exp(-d * d / (2 * sigma * sigma));
      sx += weight * x[k];
      sz += weight * z[k];
      sw += weight;
    }
    const f64 t = std::min(1.0, std::min(s[j], cells_length - s[j]) / sigma);
    qx[j] = x[j] + t * (sx / sw - x[j]);
    qz[j] = z[j] + t * (sz / sw - z[j]);
  }
  Vector<f64> qs(count, 0.0);
  for (u32 j = 1; j < count; ++j)
    qs[j] = qs[j - 1] + std::sqrt((qx[j] - qx[j - 1]) * (qx[j] - qx[j - 1]) +
                                  (qz[j] - qz[j - 1]) * (qz[j] - qz[j - 1]));
  const f64 length = qs[count - 1];
  const u32 steps = std::max<u32>(1, static_cast<u32>(std::ceil(length / wide(p.step_m))));
  const u32 m = steps + 1;
  Vector<f64> px(m), pz(m), pa(m), ps(m);
  u32 seg = 0;
  for (u32 i = 0; i < m; ++i) {
    const f64 at = length * static_cast<f64>(i) / static_cast<f64>(steps);
    while (seg + 2 < count && qs[seg + 1] < at)
      ++seg;
    const f64 span = qs[seg + 1] - qs[seg];
    const f64 t = span > 0 ? std::clamp((at - qs[seg]) / span, 0.0, 1.0) : 0.0;
    px[i] = qx[seg] + t * (qx[seg + 1] - qx[seg]);
    pz[i] = qz[seg] + t * (qz[seg + 1] - qz[seg]);
    pa[i] = area[seg] + t * (area[seg + 1] - area[seg]);
    ps[i] = at;
  }
  px[0] = qx[0];
  pz[0] = qz[0];
  px[m - 1] = qx[count - 1];
  pz[m - 1] = qz[count - 1];
  // Meanders (Leopold, Wolman & Miller 1964: wavelength 10.9 w^1.01, full amplitude 2.7 w^1.1),
  // faded out with slope (Montgomery & Buffington 1997: no meanders on step-pool and cascade beds),
  // eased in over half a wavelength at each end, and held inside the trough: an offset that would
  // climb more than half the depth out of it is halved until it does not.
  const f64 lo = wide(p.meander_slope_min), hi = wide(p.meander_slope_max);
  link.meander = 1.0 - smoothstep(lo, hi, link.slope);
  Vector<f64> offset(m, 0.0);
  if (link.meander > 0 && p.meander_amplitude_coeff > 0) {
    Rng rng(link.seed);
    const f64 jitter = .7 + .6 * rng.unit();
    const f64 phase0 = rng.unit() * 2 * k_pi;
    f64 phase = phase0;
    // **No two bends alike** (W90; the owner's v35 walk: "a regular sawtooth"): each bend's
    // wavelength and amplitude vary along the line by a value noise of the link's own, a knot a
    // bend (half a wavelength of phase) eased between knots, so the 10-14 widths Leopold & Wolman
    // (1960) report is a mean with a spread rather than one number.
    const u64 vary = hash_combine(link.seed, 0x91a7ull);
    auto noise = [&](u64 salt, f64 at) {
      const f64 k = std::floor(at);
      const i64 knot = static_cast<i64>(k);
      const f64 t = at - k, e = t * t * (3 - 2 * t);
      const u64 h = hash_combine(vary, salt);
      const f64 a = place_noise(h, knot, 0), b = place_noise(h, knot + 1, 0);
      return a + (b - a) * e;
    };
    for (u32 i = 0; i < m; ++i) {
      const f64 wi = hydraulic(wide(p.width_at_km2_m), wide(p.width_exponent), pa[i]);
      const f64 di = hydraulic(wide(p.depth_at_km2_m), wide(p.depth_exponent), pa[i]);
      const f64 bend = (phase - phase0) / k_pi;
      const f64 lambda = wide(p.meander_wavelength_widths) * powd(wi, 1.01) * jitter *
                         (1 + wide(p.meander_wavelength_variation) * noise(1, bend));
      const f64 amplitude = 1 + wide(p.meander_amplitude_variation) * noise(2, bend);
      if (i > 0) phase += (ps[i] - ps[i - 1]) * 2 * k_pi / lambda;
      const f64 edge = std::min(ps[i], length - ps[i]) / (.5 * lambda);
      const f64 ease = smoothstep(0.0, 1.0, edge);
      f64 o = link.meander * .5 * wide(p.meander_amplitude_coeff) * powd(wi, 1.1) * amplitude *
              det::sin(phase) * ease;
      if (i == 0 || i + 1 == m || o == 0) continue;
      f64 tx = px[i + 1] - px[i - 1], tz = pz[i + 1] - pz[i - 1];
      const f64 tl = std::sqrt(tx * tx + tz * tz);
      if (tl <= 0) continue;
      tx /= tl;
      tz /= tl;
      const f64 g0 = ground_at(ground, px[i], pz[i]);
      for (u32 tries = 0; tries < 4 && o != 0; ++tries) {
        const f64 g = ground_at(ground, px[i] - tz * o, pz[i] + tx * o);
        if (g - g0 <= .5 * di) break;
        o = tries == 3 ? 0.0 : o * .5;
      }
      offset[i] = o;
    }
    // The halvings leave steps in the offset; three binomial passes take them out.
    for (u32 pass = 0; pass < 3; ++pass) {
      Vector<f64> next = offset;
      for (u32 i = 1; i + 1 < m; ++i)
        next[i] = .25 * offset[i - 1] + .5 * offset[i] + .25 * offset[i + 1];
      offset = std::move(next);
    }
  }
  Vector<f64> fx(m), fz(m);
  for (u32 i = 0; i < m; ++i) {
    fx[i] = px[i];
    fz[i] = pz[i];
    if (i == 0 || i + 1 == m || offset[i] == 0) continue;
    f64 tx = px[i + 1] - px[i - 1], tz = pz[i + 1] - pz[i - 1];
    const f64 tl = std::sqrt(tx * tx + tz * tz);
    if (tl <= 0) continue;
    fx[i] = px[i] - tz / tl * offset[i];
    fz[i] = pz[i] + tx / tl * offset[i];
  }
  link.line.clear();
  link.length_m = 0;
  Vector<f64> bed(m);
  for (u32 i = 0; i < m; ++i) {
    ChannelPoint pt;
    const f64 g = ground_at(ground, fx[i], fz[i]);
    pt.at = WorldPos{fx[i], g, fz[i]};
    pt.width_m = static_cast<f32>(hydraulic(wide(p.width_at_km2_m), wide(p.width_exponent), pa[i]));
    pt.depth_m = static_cast<f32>(hydraulic(wide(p.depth_at_km2_m), wide(p.depth_exponent), pa[i]));
    pt.area_m2 = static_cast<f32>(pa[i]);
    bed[i] = i == 0 ? g : std::min(g, bed[i - 1]);
    if (i > 0) {
      const f64 dx = fx[i] - fx[i - 1], dz = fz[i] - fz[i - 1];
      link.length_m += std::sqrt(dx * dx + dz * dz);
    }
    link.line.push_back(pt);
  }
  // The bed's fall per metre at each point: over a pool-to-pool length centred on it, from the
  // bed's running minimum, so a lattice bump on the line does not read as an adverse slope.
  for (u32 i = 0; i < m; ++i) {
    const f64 reach = .5 * wide(p.pool_riffle_widths) * wide(link.line[i].width_m);
    const u32 k = std::max<u32>(1, static_cast<u32>(std::ceil(reach / wide(p.step_m))));
    const u32 a = i > k ? i - k : 0, b = std::min(m - 1, i + k);
    const f64 run = ps[b] - ps[a];
    const f64 fall = run > 0 ? (bed[a] - bed[b]) / run : 0.0;
    link.line[i].slope = static_cast<f32>(std::max(1e-4, fall));
  }
}

Vector<f64> lengths_of(const Vector<ChannelPoint>& line) {
  Vector<f64> s(line.size(), 0.0);
  for (u32 i = 1; i < line.size(); ++i) {
    const f64 dx = line[i].at.x - line[i - 1].at.x, dz = line[i].at.z - line[i - 1].at.z;
    s[i] = s[i - 1] + std::sqrt(dx * dx + dz * dz);
  }
  return s;
}

// **Mouths** (W90; channels.md, "Mouths"): a link's line split at the water bodies' shorelines.
// A point is under water where `Exclusion::excluded_mm`'s water kind says (the ground below a
// body's level inside its outline, or anywhere for an open sea), however short the wet stretch: a
// dip below the level inside an outline is under the body's surface. A dry run shorter than
// `max(4 m, 2 w)` is dropped. The first dry run stays the link (its cells, its id); each later one
// (what leaves a lake again) is appended as an outlet link of its own. A run that ends at a
// shoreline ends on the shoreline itself, found by bisection along the stretch that crosses it,
// and the ground there is the body's level.
constexpr f64 k_run_min_m = 4.0;
void apply_water(ChannelNetwork& n, const scene_gen::Ground& ground,
                 const scene_gen::Exclusion* water) {
  if (water == nullptr || water->body_count() == 0) return;
  auto wet_at = [&](f64 x, f64 z) {
    return water->excluded_mm(ground, scene_gen::nearest_mm(x), scene_gen::nearest_mm(z),
                              scene_gen::k_exclude_water);
  };
  // The shoreline between a dry point and a wet one: 16 halvings of the stretch (under 0.1 mm
  // for the 0.5 m step), the dry point's numbers, the ground's height there.
  auto shore = [&](const ChannelPoint& dry, const ChannelPoint& wet) {
    f64 lo = 0, hi = 1;
    for (u32 k = 0; k < 16; ++k) {
      const f64 mid = .5 * (lo + hi);
      if (wet_at(dry.at.x + mid * (wet.at.x - dry.at.x), dry.at.z + mid * (wet.at.z - dry.at.z)))
        hi = mid;
      else
        lo = mid;
    }
    ChannelPoint pt = dry;
    pt.at.x = round_to(dry.at.x + lo * (wet.at.x - dry.at.x), 1e3);
    pt.at.z = round_to(dry.at.z + lo * (wet.at.z - dry.at.z), 1e3);
    pt.at.y = ground_at(ground, pt.at.x, pt.at.z);
    return pt;
  };
  Vector<ChannelLink> extra;
  const u32 count = n.links.size();
  for (u32 k = 0; k < count; ++k) {
    ChannelLink& link = n.links[k];
    const u32 m = link.line.size();
    if (m < 2) continue;
    Vector<u8> wet(m, 0);
    bool any = false;
    for (u32 i = 0; i < m; ++i) {
      wet[i] = wet_at(link.line[i].at.x, link.line[i].at.z) ? 1 : 0;
      any = any || wet[i] != 0;
    }
    if (!any) continue;
    const Vector<f64> s = lengths_of(link.line);

    struct Run {
      u32 a, b;  // [a, b)
    };
    Vector<Run> runs;
    for (u32 i = 0; i < m;) {
      u32 j = i;
      while (j < m && wet[j] == wet[i])
        ++j;
      if (!wet[i] && j - i >= 2 &&
          s[j - 1] - s[i] >= std::max(k_run_min_m, 2 * wide(link.line[i].width_m)))
        runs.push_back({i, j});
      i = j;
    }
    u64 kept = 0;
    Vector<ChannelLink> made;
    for (u32 r = 0; r < runs.size(); ++r) {
      const Run run = runs[r];
      ChannelLink out;
      out.head = link.head && run.a == 0;
      out.seed = r == 0 ? link.seed : hash_combine(link.seed, r);
      out.meander = link.meander;
      out.slope = link.slope;
      if (r == 0) out.cells = link.cells;
      if (run.a > 0) {
        out.outlet = true;
        out.line.push_back(shore(link.line[run.a], link.line[run.a - 1]));
      }
      for (u32 i = run.a; i < run.b; ++i)
        out.line.push_back(link.line[i]);
      kept += run.b - run.a;
      if (run.b < m) {
        const ChannelPoint end = shore(link.line[run.b - 1], link.line[run.b]);
        out.mouth = true;
        out.mouth_level = end.at.y;
        out.line.push_back(end);
      }
      out.length_m = lengths_of(out.line).back();
      made.push_back(std::move(out));
    }
    n.water.points_removed += m - kept;
    if (made.empty()) {
      link.line.clear();
      link.length_m = 0;
      ++n.water.dropped;
      continue;
    }
    for (const ChannelLink& l : made) {
      n.water.mouths += l.mouth ? 1u : 0u;
      n.water.outlets += l.outlet ? 1u : 0u;
    }
    link = std::move(made[0]);
    for (u32 r = 1; r < made.size(); ++r)
      extra.push_back(std::move(made[r]));
  }
  for (ChannelLink& l : extra)
    n.links.push_back(std::move(l));
}

// **Reaches and steps** (W90; channels.md, "Reaches"): each point's reach from the ground's fall
// per metre along the line over a window centred on it (Montgomery & Buffington 1997 class a reach
// over many widths), and on step-pool, cascade and bedrock reaches a step every `L` metres —
// `L = w clamp(step_pool_widths sqrt(slope_step_pool / S), 1, 4)`, the 1-4 widths of the step-pool
// literature, closer on steeper beds — of height `H = step_height_ratio S L` (Abrahams et al.
// 1995), at most 4 m. Below a crest the pool lowers the bed by H at once (in one stretch where H
// reaches `fall_min_m`, else at a rise of at most a half over the run) and comes back up to the
// line at the next crest, so the bed is never raised and its mean fall is the ground's.
constexpr f64 k_step_max_m = 4.0;
constexpr f64 k_step_min_m = .05;
bool stepped(ChannelReach r) {
  return r == ChannelReach::StepPool || r == ChannelReach::Cascade || r == ChannelReach::Bedrock;
}
void classify(const ChannelLayer& p, ChannelLink& link) {
  const u32 m = link.line.size();
  link.reach.assign(m, 0);
  link.step_depth.assign(m, 0.0f);
  link.step_at.clear();
  link.step_height.clear();
  link.keep = m;
  if (m < 2) return;
  const Vector<f64> s = lengths_of(link.line);
  Vector<f64> slope(m, 0.0);
  for (u32 i = 0; i < m; ++i) {
    const f64 half = .5 * std::max(wide(p.reach_window_min_m),
                                   wide(p.reach_window_widths) * wide(link.line[i].width_m));
    const u32 a = static_cast<u32>(std::lower_bound(s.begin(), s.end(), s[i] - half) - s.begin());
    u32 b = static_cast<u32>(std::upper_bound(s.begin(), s.end(), s[i] + half) - s.begin());
    b = b > 0 ? b - 1 : 0;
    const f64 run = s[b] - s[a];
    slope[i] = run > 0 ? std::max(0.0, (link.line[a].at.y - link.line[b].at.y) / run) : 0.0;
    link.reach[i] = static_cast<u8>(reach_of(p, slope[i], link.head));
  }
  const f64 step = wide(p.step_m);
  f64 next = -1;
  for (u32 i = 0; i + 1 < m; ++i) {
    if (!stepped(static_cast<ChannelReach>(link.reach[i])) || s[i] < next) continue;
    const f64 S = std::max(slope[i], 1e-4), w = wide(link.line[i].width_m);
    const f64 L =
        w * std::clamp(wide(p.step_pool_widths) * std::sqrt(wide(p.slope_step_pool) / S), 1.0, 4.0);
    next = s[i] + L;
    u32 j = i + 1;
    while (j + 1 < m && s[j] < s[i] + L && stepped(static_cast<ChannelReach>(link.reach[j])))
      ++j;
    const f64 run = s[j] - s[i];
    if (run < .5 * L) continue;
    const f64 H = std::min(k_step_max_m, wide(p.step_height_ratio) * S * run);
    if (H < k_step_min_m) continue;
    const f64 ramp = H >= wide(p.fall_min_m) ? step : std::max(step, std::min(.5 * run, 2 * H));
    for (u32 q = i + 1; q <= j; ++q) {
      const f64 u = (s[q] - s[i]) / run;
      const f64 e = H * std::min(1.0, (s[q] - s[i]) / ramp) * (1 - u);
      link.step_depth[q] = std::max(link.step_depth[q], static_cast<f32>(round_to(e, 1e3)));
    }
    link.step_at.push_back(i);
    link.step_height.push_back(static_cast<f32>(round_to(H, 1e3)));
  }
}

// The cut's bed down a link (channels.md, "The cut", and W90's steps, fan and grade): the top of
// the bank less the thalweg, held down the line where the ground rises a little but **never more
// than two thalwegs under the ground at the line**; then lowered by the steps' pools; then, on a
// link that ends at a shoreline, graded over the fan's length to half a thalweg under the body's
// level, whatever was there.
constexpr f64 k_cut_depth_thalwegs = 2.0;
void cut_bed(const ChannelLayer& p, ChannelLink& link) {
  const u32 m = link.line.size();
  link.bed.assign(m, 0.0);
  f64 bed = std::numeric_limits<f64>::infinity();
  for (u32 i = 0; i < m; ++i) {
    const ChannelPoint& pt = link.line[i];
    const f64 top = round_to(pt.at.y, 1e3);
    const f64 thalweg =
        round_to(section_of(wide(pt.width_m), wide(pt.depth_m), wide(p.bank_slope)).thalweg, 1e3);
    bed = std::min(bed, round_to(top - thalweg, 1e3));
    const f64 b = std::max(bed, round_to(top - k_cut_depth_thalwegs * thalweg, 1e3));
    link.bed[i] = round_to(b - wide(link.step_depth[i]), 1e3);
  }
  if (!link.mouth || m < 2) return;
  const Vector<f64> s = lengths_of(link.line);
  const ChannelPoint& end = link.line[m - 1];
  const f64 thalweg_end =
      section_of(wide(end.width_m), wide(end.depth_m), wide(p.bank_slope)).thalweg;
  const f64 target = round_to(link.mouth_level - .5 * thalweg_end, 1e3);
  const f64 fan = wide(p.fan_widths) * wide(end.width_m);
  u32 f = m - 1;
  while (f > 0 && s[m - 1] - s[f - 1] <= fan)
    --f;
  const f64 b0 = link.bed[f], span = s[m - 1] - s[f];
  for (u32 i = f; i < m; ++i) {
    const f64 t = span > 0 ? (s[i] - s[f]) / span : 1.0;
    const f64 top = round_to(link.line[i].at.y, 1e3);
    link.bed[i] = std::min(round_to(b0 + t * (target - b0), 1e3), round_to(top - .01, 1e3));
  }
}

// The fan's share at point i of a link that ends at a shoreline: 0 above the fan, 1 at the shore.
f64 fan_share(const ChannelLayer& p, const ChannelLink& link, const Vector<f64>& s, u32 i) {
  const u32 m = link.line.size();
  if (!link.mouth || m < 2) return 0.0;
  const f64 fan = wide(p.fan_widths) * wide(link.line[m - 1].width_m);
  const f64 from_end = s[m - 1] - s[i];
  return fan > 0 && from_end < fan ? 1.0 - from_end / fan : 0.0;
}

// **Confluences** (W90; channels.md, "Confluences"): at every junction the incoming link of the
// largest contributing area is the trunk, which runs on as the link that begins there; every other
// incoming link is a tributary of that link. Junctions are taken by the downstream link's area,
// largest first (ties by index), the tributaries of one in index order. A tributary's bed is
// lowered (never raised) over its last `confluence_widths` of its own width to the trunk's bed at
// the junction, and its water and pieces stop where it enters the trunk's banks: at its first
// point within half the trunk's width of the junction.
void confluences(const ChannelLayer& p, ChannelNetwork& n) {
  Vector<u32> order;
  for (u32 k = 0; k < n.links.size(); ++k) {
    const ChannelLink& c = n.links[k];
    if (c.line.size() >= 2 && !c.cells.empty() && !c.outlet) order.push_back(k);
  }
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
    const u32 x = n.area_cells[n.links[a].cells.back()], y = n.area_cells[n.links[b].cells.back()];
    return x != y ? x > y : a < b;
  });
  // The links that end at each cell, by index: a sorted list of (last cell, link).
  Vector<std::pair<u32, u32>> ends;
  for (u32 k = 0; k < n.links.size(); ++k) {
    const ChannelLink& t = n.links[k];
    if (t.line.size() >= 2 && t.cells.size() >= 2 && !t.mouth) ends.push_back({t.cells.back(), k});
  }
  std::sort(ends.begin(), ends.end());
  for (u32 c : order) {
    const ChannelLink& down = n.links[c];
    const u32 junction = down.cells.front();
    auto it = std::lower_bound(ends.begin(), ends.end(), std::pair<u32, u32>{junction, 0u});
    Vector<u32> in;
    for (; it != ends.end() && it->first == junction; ++it)
      in.push_back(it->second);
    if (in.size() < 2) continue;
    u32 trunk = in[0];
    for (u32 t : in) {
      const ChannelLink& l = n.links[t];
      const ChannelLink& best = n.links[trunk];
      const u32 a = n.area_cells[l.cells[l.cells.size() - 2]];
      const u32 b = n.area_cells[best.cells[best.cells.size() - 2]];
      if (a > b) trunk = t;
    }
    const WorldPos at = down.line[0].at;
    const f64 reach = .5 * wide(down.line[0].width_m);
    const f64 target = down.bed.empty() ? at.y : down.bed[0];
    for (u32 t : in) {
      if (t == trunk) continue;
      ChannelLink& trib = n.links[t];
      trib.trunk = c;
      ++n.water.confluences;
      const u32 m = trib.line.size();
      u32 keep = m;
      for (u32 i = 0; i < m; ++i) {
        const f64 dx = trib.line[i].at.x - at.x, dz = trib.line[i].at.z - at.z;
        if (std::sqrt(dx * dx + dz * dz) < reach) {
          keep = i;
          break;
        }
      }
      trib.keep = keep >= 2 ? keep : 0;
      // The grade reaches the trunk's bed at the trunk's banks (the last point kept) and holds
      // it on to the junction, so the water the tributary keeps can stand at the trunk's surface.
      const Vector<f64> s = lengths_of(trib.line);
      const u32 e = std::max<u32>(keep, 1) - 1;
      const f64 grade = wide(p.confluence_widths) * wide(trib.line[m - 1].width_m);
      u32 f = e;
      while (f > 0 && s[e] - s[f - 1] <= grade)
        --f;
      const f64 b0 = trib.bed[f], span = s[e] - s[f];
      for (u32 i = f; i < m; ++i) {
        const f64 u = span > 0 && i <= e ? (s[i] - s[f]) / span : 1.0;
        trib.bed[i] = std::min(trib.bed[i], round_to(b0 + u * (target - b0), 1e3));
      }
    }
  }
}
}  // namespace

ChannelReach reach_of(const ChannelLayer& p, f64 slope, bool colluvial) {
  if (slope < wide(p.slope_pool_riffle)) return ChannelReach::DuneRipple;
  if (slope < wide(p.slope_plane_bed)) return ChannelReach::PoolRiffle;
  if (slope < wide(p.slope_step_pool)) return ChannelReach::PlaneBed;
  if (slope < wide(p.slope_cascade)) return ChannelReach::StepPool;
  if (slope < wide(p.slope_steep)) return ChannelReach::Cascade;
  return colluvial ? ChannelReach::Colluvial : ChannelReach::Bedrock;
}

bool layer_in_range(const ChannelLayer& p, std::string& error) {
  if (!in(wide(p.spacing_m), 1, 32)) return fail(error, "spacing_m");
  const i64 spacing = scene_gen::nearest_mm(wide(p.spacing_m));
  if (static_cast<f32>(static_cast<f64>(spacing) / 1000.0) != p.spacing_m)
    return fail(error, "spacing_m (whole millimetres)");
  for (u32 a = 0; a < 2; ++a) {
    if (!in(p.region_min[a], -1e8, 1e8) || !in(p.region_max[a], -1e8, 1e8) ||
        p.region_max[a] - p.region_min[a] < wide(p.spacing_m))
      return fail(error, "region_min/max");
    if (scene_gen::nearest_mm(p.region_min[a]) % spacing != 0 ||
        scene_gen::nearest_mm(p.region_max[a]) % spacing != 0)
      return fail(error, "region alignment (a whole number of spacings from the origin)");
  }
  if (p.max_samples < 4 || p.max_samples > 16777216) return fail(error, "max_samples");
  if (!in(wide(p.head_threshold_m2), 10, 1e6)) return fail(error, "head_threshold_m2");
  if (!in(wide(p.head_slope_exponent), 0, 3)) return fail(error, "head_slope_exponent");
  if (!in(wide(p.min_head_area_m2), 0, 1e6)) return fail(error, "min_head_area_m2");
  if (!in(wide(p.width_at_km2_m), .1, 100) || !in(wide(p.width_exponent), .1, .8))
    return fail(error, "width_at_km2_m/width_exponent");
  if (!in(wide(p.depth_at_km2_m), .02, 10) || !in(wide(p.depth_exponent), .1, .8))
    return fail(error, "depth_at_km2_m/depth_exponent");
  if (!in(wide(p.smooth_min_m), 0, 100) || !in(wide(p.smooth_widths), 0, 10))
    return fail(error, "smooth_min_m/smooth_widths");
  if (!in(wide(p.meander_wavelength_widths), 4, 30) || !in(wide(p.meander_amplitude_coeff), 0, 10))
    return fail(error, "meander_wavelength_widths/meander_amplitude_coeff");
  if (!in(wide(p.meander_slope_min), 0, .5) || !in(wide(p.meander_slope_max), 0, .5) ||
      p.meander_slope_min >= p.meander_slope_max)
    return fail(error, "meander_slope_min/max");
  if (!in(wide(p.pool_riffle_widths), 2, 20) || !in(wide(p.riffle_rise), 0, 1) ||
      !in(wide(p.step_pool_widths), 1, 10))
    return fail(error, "pool_riffle_widths/riffle_rise/step_pool_widths");
  if (!in(wide(p.shields_bankfull), .01, .2)) return fail(error, "shields_bankfull");
  if (!in(wide(p.bar_min_width_m), 0, 100) || !in(wide(p.bar_rise), 0, 1) ||
      !in(wide(p.bar_width_widths), .05, .5) || !in(wide(p.bar_length_widths), .5, 20))
    return fail(error, "bar_min_width_m/bar_rise/bar_width_widths/bar_length_widths");
  if (!in(wide(p.bank_min_width_m), 0, 100) || !in(wide(p.bank_height), 0, 2) ||
      !in(wide(p.bank_width_widths), 0, 2) || !in(wide(p.bank_width_m), 0, 10))
    return fail(error, "bank_min_width_m/bank_height/bank_width_widths/bank_width_m");
  if (!in(wide(p.bank_slope), .25, 8)) return fail(error, "bank_slope");
  if (!in(wide(p.reach_window_widths), 5, 50) || !in(wide(p.reach_window_min_m), 0, 200))
    return fail(error, "reach_window_widths/reach_window_min_m");
  {
    const f64 bounds[5] = {wide(p.slope_pool_riffle), wide(p.slope_plane_bed),
                           wide(p.slope_step_pool), wide(p.slope_cascade), wide(p.slope_steep)};
    for (u32 b = 0; b < 5; ++b)
      if (!in(bounds[b], 0, 2) || (b > 0 && !(bounds[b] > bounds[b - 1])))
        return fail(error, "slope_pool_riffle..slope_steep (ascending, 0 to 2)");
  }
  if (!in(wide(p.step_height_ratio), 1, 2)) return fail(error, "step_height_ratio");
  if (!in(wide(p.fall_min_m), .2, 10)) return fail(error, "fall_min_m");
  if (!in(wide(p.fan_widths), 1, 20) || !in(wide(p.fan_spread), 0, 4))
    return fail(error, "fan_widths/fan_spread");
  if (!in(wide(p.confluence_widths), 1, 20)) return fail(error, "confluence_widths");
  if (!in(wide(p.meander_wavelength_variation), 0, .5) ||
      !in(wide(p.meander_amplitude_variation), 0, .8))
    return fail(error, "meander_wavelength_variation/meander_amplitude_variation");
  if (!in(wide(p.step_m), .1, 4)) return fail(error, "step_m");
  if (!in(wide(p.piece_m), 4, 256) || p.piece_m < 4 * p.step_m) return fail(error, "piece_m");
  for (const ChannelSurface* s : {&p.bed, &p.bars, &p.banks})
    if (!surface_in_range(*s, error)) return false;
  return true;
}

bool plan_channels(const ChannelLayer& p, const scene_gen::Ground& ground, u64 seed,
                   jobs::JobSystem* jobs, ChannelNetwork& out, std::string& error,
                   const scene_gen::Exclusion* water) {
  out = {};
  if (!ground.present()) return fail(error, "ground (required)");
  if (!layer_in_range(p, error)) return false;
  ChannelNetwork n;
  n.spacing_mm = scene_gen::nearest_mm(wide(p.spacing_m));
  n.x_mm = scene_gen::nearest_mm(p.region_min[0]);
  n.z_mm = scene_gen::nearest_mm(p.region_min[1]);
  const u64 w =
      static_cast<u64>((scene_gen::nearest_mm(p.region_max[0]) - n.x_mm) / n.spacing_mm) + 1;
  const u64 h =
      static_cast<u64>((scene_gen::nearest_mm(p.region_max[1]) - n.z_mm) / n.spacing_mm) + 1;
  if (w * h > p.max_samples) return fail(error, "max_samples (region exceeds the lattice budget)");
  n.width = static_cast<u32>(w);
  n.depth = static_cast<u32>(h);
  const u32 cells = n.width * n.depth;
  // The lattice, a row a job: every row writes its own heights, so the bytes are the same whatever
  // ran them. Heights to the micrometre, as integers, are what every decision below compares.
  n.ground_um.resize(cells);
  Vector<u8> bad(n.depth, 0);
  auto rows = [&](u32 begin, u32 end) {
    for (u32 r = begin; r < end; ++r)
      for (u32 c = 0; c < n.width; ++c) {
        const f64 y = ground.surface_mm(n.x_mm + static_cast<i64>(c) * n.spacing_mm,
                                        n.z_mm + static_cast<i64>(r) * n.spacing_mm);
        if (!in(y, -1e6, 1e6)) {
          bad[r] = 1;
          n.ground_um[r * n.width + c] = 0;
        } else {
          n.ground_um[r * n.width + c] = std::llround(y * 1e6);
        }
      }
  };
  if (jobs != nullptr && n.depth > 1)
    jobs->parallel_for(jobs::Pool::Performance, n.depth, 8,
                       [&](u32 begin, u32 end) { rows(begin, end); });
  else
    rows(0, n.depth);
  for (u8 b : bad)
    if (b) return fail(error, "ground height (finite, +/-1e6 m)");
  // Pits filled by priority flood from the region's edge with a one-micrometre epsilon (Barnes,
  // Lehman & Mulla 2014), so every interior cell has a strictly lower neighbour and drains to the
  // edge; a flat drains along the way the flood came.
  n.filled_um.assign(cells, 0);
  Vector<u8> seen(cells, 0);
  Vector<Node> heap;
  auto push = [&](u32 i, i64 v) {
    n.filled_um[i] = v;
    seen[i] = 1;
    heap.push_back({v, i});
    std::push_heap(heap.begin(), heap.end(), after);
  };
  for (u32 i = 0; i < cells; ++i) {
    const u32 c = i % n.width, r = i / n.width;
    if (c == 0 || r == 0 || c + 1 == n.width || r + 1 == n.depth) push(i, n.ground_um[i]);
  }
  while (!heap.empty()) {
    std::pop_heap(heap.begin(), heap.end(), after);
    const Node top = heap.back();
    heap.pop_back();
    for (u32 d = 0; d < 8; ++d) {
      const u32 j = neighbour(top.i, n.width, n.depth, k_dx[d], k_dz[d]);
      if (j == ~0u || seen[j]) continue;
      push(j, std::max(n.ground_um[j], top.h + 1));
    }
  }
  // D8 (O'Callaghan & Mark 1984): the steepest drop over the filled heights, a diagonal's run
  // sqrt(2) spacings; ties go to the first direction. The region's edge is the outlet.
  n.receiver.assign(cells, ~0u);
  for (u32 i = 0; i < cells; ++i) {
    const u32 c = i % n.width, r = i / n.width;
    if (c == 0 || r == 0 || c + 1 == n.width || r + 1 == n.depth) continue;
    f64 best = 0;
    for (u32 d = 0; d < 8; ++d) {
      const u32 j = neighbour(i, n.width, n.depth, k_dx[d], k_dz[d]);
      const i64 drop = n.filled_um[i] - n.filled_um[j];
      if (drop <= 0) continue;
      const f64 slope = static_cast<f64>(drop) / ((k_dx[d] != 0 && k_dz[d] != 0) ? k_sqrt2 : 1.0);
      if (slope > best) {
        best = slope;
        n.receiver[i] = j;
      }
    }
  }
  // Contributing area: every cell after all of its donors, which the filled heights order.
  Vector<u32> order(cells);
  for (u32 i = 0; i < cells; ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
    return n.filled_um[a] != n.filled_um[b] ? n.filled_um[a] > n.filled_um[b] : a < b;
  });
  n.area_cells.assign(cells, 1);
  for (u32 i : order)
    if (n.receiver[i] != ~0u) n.area_cells[n.receiver[i]] += n.area_cells[i];
  // Heads (Montgomery & Dietrich 1988, 1989): contributing area times the local slope to an
  // exponent at a threshold, above a floor of area; once a channel, a channel all the way down.
  const f64 spacing = static_cast<f64>(n.spacing_mm) / 1000.0;
  const f64 cell_area = spacing * spacing;
  const f64 alpha = wide(p.head_slope_exponent);
  n.channel.assign(cells, 0);
  for (u32 i : order) {
    const u32 r = n.receiver[i];
    if (r == ~0u) continue;
    const f64 area = static_cast<f64>(n.area_cells[i]) * cell_area;
    const bool diagonal = (r % n.width != i % n.width) && (r / n.width != i / n.width);
    const f64 slope = std::max(0.0, static_cast<f64>(n.ground_um[i] - n.ground_um[r]) / 1e6 /
                                        (spacing * (diagonal ? k_sqrt2 : 1.0)));
    const f64 product = alpha == 0 ? area : area * powd(slope, alpha);
    if (area >= wide(p.min_head_area_m2) && product >= wide(p.head_threshold_m2)) n.channel[i] = 1;
    if (n.channel[i]) n.channel[r] = 1;
  }
  Vector<u8> donors(cells, 0);
  for (u32 i = 0; i < cells; ++i)
    if (n.channel[i] && n.receiver[i] != ~0u && donors[n.receiver[i]] < 255)
      ++donors[n.receiver[i]];
  for (u32 i = 0; i < cells; ++i)
    if (n.channel[i] && donors[i] == 0) n.heads.push_back(i);
  // Links: from every head and every junction down to the next junction or the edge.
  for (u32 i = 0; i < cells; ++i) {
    if (!n.channel[i] || donors[i] == 1) continue;
    ChannelLink link;
    link.head = donors[i] == 0;
    link.cells.push_back(i);
    u32 c = i;
    while (n.receiver[c] != ~0u) {
      c = n.receiver[c];
      link.cells.push_back(c);
      if (donors[c] >= 2) break;
    }
    if (link.cells.size() < 2) continue;
    const i64 x = n.x_mm + static_cast<i64>(i % n.width) * n.spacing_mm;
    const i64 z = n.z_mm + static_cast<i64>(i / n.width) * n.spacing_mm;
    link.seed = hash_combine(hash_combine(p.seed, seed),
                             hash_combine(static_cast<u64>(x), static_cast<u64>(z)));
    n.links.push_back(std::move(link));
  }
  for (ChannelLink& link : n.links)
    build_line(p, n, ground, link);
  // W90: the shorelines, the reaches and steps, the cut's bed, the confluences, in that order.
  apply_water(n, ground, water);
  for (ChannelLink& link : n.links) {
    classify(p, link);
    cut_bed(p, link);
    n.length_m += link.length_m;
  }
  confluences(p, n);
  out = std::move(n);
  return true;
}

namespace {
constexpr f64 k_min_water_m = .01;
// The water of a link, point by point down its line (W86's on the ground, W89's in the cut, W90's
// fan and confluence grade): its bed and its surface, metres.
struct Water {
  Vector<f64> bed, surface, thalweg;
};
Water water_of(const ChannelLayer& p, const ChannelLink& link, f32 stage, bool cut) {
  Water w;
  const u32 m = link.line.size();
  if (m < 2) return w;
  const Vector<f64> s = lengths_of(link.line);
  f64 bed = link.line[0].at.y;
  f64 surface = std::numeric_limits<f64>::infinity();
  // The surface at the fan's head, which the fan grades to the body's level.
  f64 fan_head = std::numeric_limits<f64>::quiet_NaN();
  for (u32 i = 0; i < m; ++i) {
    const ChannelPoint& pt = link.line[i];
    const f64 thalweg = round_to(
        section_of(wide(pt.width_m), wide(pt.depth_m), wide(p.bank_slope)).thalweg, 1e3);
    f64 water = 0;
    if (cut) {
      // Laid in the cut (W89): held down the line, except where the cut's bed climbs over a sill,
      // where the water keeps half its depth over it rather than run under the ground.
      bed = link.bed[i];
      water = wide(stage) * thalweg;
      surface = std::max(std::min(surface, round_to(bed + water, 1e3)),
                         round_to(bed + .5 * water, 1e3));
    } else {
      // Laid on the ground (W86): the bed the ground's running minimum, the surface `stage` of the
      // bankfull depth over it, never rising downstream.
      bed = std::min(bed, pt.at.y);
      water = wide(stage) * wide(pt.depth_m);
      surface = std::min(surface, round_to(bed + water, 1e3));
    }
    // The fan (W90): from the fan's head the surface grades to the body's level at the shore, and
    // never stands below it there (the body backs the water up).
    const f64 t = fan_share(p, link, s, i);
    if (t > 0) {
      if (std::isnan(fan_head)) fan_head = surface;
      surface = std::max(round_to(link.mouth_level, 1e3),
                         std::min(surface, round_to(fan_head + t * (link.mouth_level - fan_head), 1e3)));
    }
    // Above the fan too, the body backs the water up: a link that ends in it never stands below its
    // level (the surface still never rises downstream, since it was falling to there).
    if (link.mouth) surface = std::max(surface, round_to(link.mouth_level, 1e3));
    w.bed.push_back(bed);
    w.surface.push_back(surface);
    w.thalweg.push_back(cut ? thalweg : wide(pt.depth_m));
  }
  return w;
}
}  // namespace

JsonValue water_channels_json(const ChannelLayer& p, const ChannelNetwork& n, f64 min_area_m2,
                              f32 stage, bool cut) {
  Vector<Water> waters;
  for (const ChannelLink& link : n.links)
    waters.push_back(water_of(p, link, stage, cut));
  // **Confluences** (W90): a tributary's surface is lowered (never raised) over its last
  // `confluence_widths` of its width to the trunk's at the junction — the surface of the link that
  // begins there — keeping at least a centimetre of water over its bed. Taken in the order
  // `plan_channels` took the junctions: by the downstream link's area, largest first.
  Vector<u32> order;
  for (u32 k = 0; k < n.links.size(); ++k)
    if (n.links[k].trunk != ~0u) order.push_back(k);
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
    const ChannelLink& x = n.links[n.links[a].trunk];
    const ChannelLink& y = n.links[n.links[b].trunk];
    const u32 ax = n.area_cells[x.cells.back()], ay = n.area_cells[y.cells.back()];
    if (ax != ay) return ax > ay;
    return n.links[a].trunk != n.links[b].trunk ? n.links[a].trunk < n.links[b].trunk : a < b;
  });
  for (u32 k : order) {
    const ChannelLink& trib = n.links[k];
    Water& w = waters[k];
    const Water& down = waters[trib.trunk];
    if (w.surface.empty() || down.surface.empty()) continue;
    // The water ends at the trunk's banks (`keep`), so the grade ends there too: the last point
    // the tributary's water keeps stands at the trunk's surface.
    const u32 m = std::min<u32>(trib.keep, trib.line.size());
    if (m < 2) continue;
    const Vector<f64> s = lengths_of(trib.line);
    const f64 grade = wide(p.confluence_widths) * wide(trib.line[m - 1].width_m);
    u32 f = m - 1;
    while (f > 0 && s[m - 1] - s[f - 1] <= grade)
      --f;
    const f64 s0 = w.surface[f], target = down.surface[0], span = s[m - 1] - s[f];
    for (u32 i = f; i < m; ++i) {
      const f64 u = span > 0 ? (s[i] - s[f]) / span : 1.0;
      const f64 lowered = std::min(w.surface[i], round_to(s0 + u * (target - s0), 1e3));
      w.surface[i] =
          std::min(w.surface[i], std::max(lowered, round_to(w.bed[i] + k_min_water_m, 1e3)));
    }
  }
  JsonValue list = JsonValue::array();
  for (u32 k = 0; k < n.links.size(); ++k) {
    const ChannelLink& link = n.links[k];
    const u32 m = std::min<u32>(link.keep, link.line.size());
    if (m < 2 || wide(link.line[m - 1].area_m2) < min_area_m2) continue;
    const Water& w = waters[k];
    JsonValue o = JsonValue::object();
    o["name"] = JsonValue(p.name + " link " + std::to_string(k));
    JsonValue points = JsonValue::array(), widths = JsonValue::array(), depths = JsonValue::array(),
              slopes = JsonValue::array(), reach = JsonValue::array();
    for (u32 i = 0; i < m; ++i) {
      const ChannelPoint& pt = link.line[i];
      const f64 depth = std::max(0.0, w.surface[i] - w.bed[i]);
      JsonValue at = JsonValue::array();
      at.push_back(JsonValue(round_to(pt.at.x, 1e3)));
      at.push_back(JsonValue(w.surface[i]));
      at.push_back(JsonValue(round_to(pt.at.z, 1e3)));
      points.push_back(std::move(at));
      widths.push_back(JsonValue(round_to(wide(pt.width_m), 1e3)));
      depths.push_back(JsonValue(round_to(depth, 1e3)));
      slopes.push_back(JsonValue(round_to(wide(pt.slope), 1e5)));
      reach.push_back(JsonValue(static_cast<u32>(i < link.reach.size() ? link.reach[i] : 0)));
    }
    o["points"] = std::move(points);
    o["width_m"] = std::move(widths);
    o["depth_m"] = std::move(depths);
    o["slope"] = std::move(slopes);
    o["seed"] = JsonValue(link.seed);
    o["reach"] = std::move(reach);
    list.push_back(std::move(o));
  }
  return list;
}

JsonValue terrain_cuts_json(const ChannelLayer& p, const ChannelNetwork& n) {
  JsonValue list = JsonValue::array();
  for (u32 k = 0; k < n.links.size(); ++k) {
    const ChannelLink& link = n.links[k];
    if (link.line.size() < 2) continue;
    const Vector<f64> s = lengths_of(link.line);
    JsonValue o = JsonValue::object();
    o["name"] = JsonValue(p.name + " link " + std::to_string(k));
    JsonValue points = JsonValue::array(), widths = JsonValue::array(), depths = JsonValue::array();
    for (u32 i = 0; i < link.line.size(); ++i) {
      const ChannelPoint& pt = link.line[i];
      const f64 top = round_to(pt.at.y, 1e3);
      JsonValue at = JsonValue::array();
      at.push_back(JsonValue(round_to(pt.at.x, 1e3)));
      at.push_back(JsonValue(top));
      at.push_back(JsonValue(round_to(pt.at.z, 1e3)));
      points.push_back(std::move(at));
      // The fan opens the cut toward the shore (W90).
      const f64 spread = 1 + wide(p.fan_spread) * fan_share(p, link, s, i);
      widths.push_back(JsonValue(round_to(wide(pt.width_m) * spread, 1e3)));
      // The bed below the top: the thalweg, more where the cut runs through a rise, a step's pool
      // or a confluence's grade.
      depths.push_back(JsonValue(std::max(1e-3, round_to(top - link.bed[i], 1e3))));
    }
    o["points"] = std::move(points);
    o["width_m"] = std::move(widths);
    o["depth_m"] = std::move(depths);
    o["bank_slope"] = JsonValue(wide(p.bank_slope));
    list.push_back(std::move(o));
  }
  return list;
}
}  // namespace engine::channels
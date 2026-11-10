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
// How far, in depths, a point of the line may stand over the trough it runs in (the meander's test,
// W86, and every point's, W91): half the depth, the water's own stage.
constexpr f64 k_trough_rise = .5;

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
  Vector<u32> segs(m, 0);
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
    segs[i] = seg;
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
        if (g - g0 <= k_trough_rise * di) break;
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
  // **The line held in the trough** (W91; channels.md, "The planform"): the meander's own test
  // above sees the meander's offset alone, and the low-pass can carry a line out of the trough by
  // itself (it cuts a bend's corner across the valley's side). So every point of the final line is
  // tested against the trough the lattice's cells run down: the ground at the point against the
  // ground at the nearest point of the cells' own polyline, within the low-pass's reach of the
  // point's segment. A point standing more than `k_trough_rise` of the depth over it is drawn back
  // toward it, halving its offset from it up to three times and then onto it. The shares are eroded
  // over three points each way and then smoothed by three binomial passes, which leaves every point
  // drawn back at least as far as its own test asked (each smoothed share is a mean of eroded
  // shares whose windows all hold the point's own).
  {
    Vector<f64> keep(m, 1.0), rx(fx), rz(fz);
    for (u32 i = 1; i + 1 < m; ++i) {
      const u32 c = segs[i];
      const f64 sigma = std::max(wide(p.smooth_min_m), wide(p.smooth_widths) * w[c]);
      const u32 reach = static_cast<u32>(std::ceil(3 * sigma / spacing)) + 2;
      const u32 a = c > reach ? c - reach : 0, b = std::min(count - 2, c + reach);
      f64 best = std::numeric_limits<f64>::infinity();
      for (u32 k = a; k <= b; ++k) {
        const f64 ex = x[k + 1] - x[k], ez = z[k + 1] - z[k], l2 = ex * ex + ez * ez;
        const f64 t =
            l2 > 0 ? std::clamp(((fx[i] - x[k]) * ex + (fz[i] - z[k]) * ez) / l2, 0.0, 1.0) : 0.0;
        const f64 qx0 = x[k] + t * ex, qz0 = z[k] + t * ez;
        const f64 d2 = (fx[i] - qx0) * (fx[i] - qx0) + (fz[i] - qz0) * (fz[i] - qz0);
        if (d2 < best) {
          best = d2;
          rx[i] = qx0;
          rz[i] = qz0;
        }
      }
      const f64 di = hydraulic(wide(p.depth_at_km2_m), wide(p.depth_exponent), pa[i]);
      const f64 floor = ground_at(ground, rx[i], rz[i]);
      f64 k = 1;
      for (u32 tries = 0; tries < 4 && k > 0; ++tries) {
        const f64 g = ground_at(ground, rx[i] + k * (fx[i] - rx[i]), rz[i] + k * (fz[i] - rz[i]));
        if (g - floor <= k_trough_rise * di) break;
        k = tries == 3 ? 0.0 : k * .5;
      }
      keep[i] = k;
    }
    Vector<f64> share(m, 1.0);
    for (u32 i = 0; i < m; ++i) {
      const u32 a = i > 3 ? i - 3 : 0, b = std::min(m - 1, i + 3);
      for (u32 j = a; j <= b; ++j)
        share[i] = std::min(share[i], keep[j]);
    }
    for (u32 pass = 0; pass < 3; ++pass) {
      Vector<f64> next = share;
      for (u32 i = 1; i + 1 < m; ++i)
        next[i] = .25 * share[i - 1] + .5 * share[i] + .25 * share[i + 1];
      share = std::move(next);
    }
    link.trough_pulled = 0;
    for (u32 i = 1; i + 1 < m; ++i) {
      if (share[i] >= 1) continue;
      ++link.trough_pulled;
      fx[i] = rx[i] + share[i] * (fx[i] - rx[i]);
      fz[i] = rz[i] + share[i] * (fz[i] - rz[i]);
    }
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

// **Reaches and steps** (W90, W91; channels.md, "Reaches"): each point's reach from the ground's
// fall per metre along the line over a window centred on it (Montgomery & Buffington 1997 class a
// reach over many widths), and **on step-pool reaches alone** (W91: Montgomery & Buffington's
// cascades are disorganised and their bedrock and colluvial channels have no alluvial steps) a step
// every `L` metres — `L = w clamp(step_pool_widths sqrt(slope_step_pool / S), 1, 4)`, the 1-4
// widths of the step-pool literature, closer on steeper beds — of height `H = step_height_ratio S
// L` (Abrahams et al. 1995), at most 4 m. Below a crest the pool lowers the bed by H over a riser
// that stands at `riser_angle_deg` (a boulder step at its clasts' angle of repose, never a wall)
// and comes back up to the line at the next crest, so the bed is never raised and its mean fall is
// the ground's. **Cascades** carry boulder clusters instead, at irregular places down the reach.
// **Knickpoints** (W91): where the ground's long profile steepens to a fall, on any reach (the
// cut's bed follows the ground there, and the water falls).
constexpr f64 k_step_max_m = 4.0;
constexpr f64 k_step_min_m = .05;
// The fall per metre at which the water leaves its bed (water::k_channel_fall_gradient, 45 deg).
constexpr f64 k_fall_gradient = 1.0;
bool stepped(ChannelReach r) { return r == ChannelReach::StepPool; }
// The ground under the line at a distance along it, linearly between its points.
f64 profile_at(const ChannelLink& link, const Vector<f64>& s, f64 at) {
  const u32 m = link.line.size();
  if (at <= s[0]) return link.line[0].at.y;
  if (at >= s[m - 1]) return link.line[m - 1].at.y;
  const u32 b = static_cast<u32>(std::upper_bound(s.begin(), s.end(), at) - s.begin());
  const u32 a = b - 1;
  const f64 span = s[b] - s[a];
  const f64 t = span > 0 ? (at - s[a]) / span : 0.0;
  return link.line[a].at.y + t * (link.line[b].at.y - link.line[a].at.y);
}
// **Knickpoints** (W91; channels.md, "Reaches"; the grounding page's "Knickpoints"): a knickpoint
// is a convexity of the long profile, a stretch where the channel steepens abruptly (Whipple 2004;
// knickzones found on a DEM as gradients over a fixed window that exceed the reach's, Hayakawa &
// Oguchi 2006). Over a window of `knick_window_widths` widths (at least `knick_window_min_m`) the
// ground's fall per metre downstream of each point, Sd, and upstream of it, Su: a knickpoint is a
// run of points whose Sd is at least 1 (the ground falls at 45 degrees or more over the window, so
// the water falls) and that drops at least `fall_min_m` from its first point to a window past its
// last. Its first point is its lip, where the profile turns convex: there Su < 1 <= Sd, so the
// window's second difference Sd - Su is positive, and the summary reports it (`lip_convexity`);
// a run that begins at a channel head is a face the head lies on. Marked are its points and those
// within that window below it. (A gate on the second difference was tried and refused: a face
// steepening by degrees then had no lip, and the chute's limit cut the whole face deeper.)
void knickpoints(const ChannelLayer& p, ChannelLink& link, const Vector<f64>& s) {
  const u32 m = link.line.size();
  link.knick.assign(m, 0);
  Vector<f64> down(m, 0.0), up(m, 0.0), window(m, 0.0);
  for (u32 i = 0; i < m; ++i) {
    const f64 W = std::max(wide(p.knick_window_min_m),
                           wide(p.knick_window_widths) * wide(link.line[i].width_m));
    window[i] = W;
    const f64 g = link.line[i].at.y;
    const f64 ahead = std::min(W, s[m - 1] - s[i]), behind = std::min(W, s[i]);
    down[i] = ahead > 0 ? (g - profile_at(link, s, s[i] + ahead)) / ahead : 0.0;
    up[i] = behind > 0 ? (profile_at(link, s, s[i] - behind) - g) / behind : down[i];
  }
  for (u32 i = 0; i < m;) {
    if (down[i] < k_fall_gradient) {
      ++i;
      continue;
    }
    u32 j = i;
    while (j + 1 < m && down[j + 1] >= k_fall_gradient)
      ++j;
    const f64 end = std::min(s[m - 1], s[j] + window[j]);
    const f64 drop = link.line[i].at.y - profile_at(link, s, end);
    if (drop >= wide(p.fall_min_m)) {
      ++link.knickpoints;
      link.lip_convexity += down[i] - up[i];
      for (u32 q = i; q < m && s[q] <= end; ++q)
        link.knick[q] = 1;
    }
    i = j + 1;
  }
}
void classify(const ChannelLayer& p, ChannelLink& link) {
  const u32 m = link.line.size();
  link.reach.assign(m, 0);
  link.step_depth.assign(m, 0.0f);
  link.step_at.clear();
  link.step_height.clear();
  link.cluster_at.clear();
  link.cluster_size.clear();
  link.knick.assign(m, 0);
  link.knickpoints = 0;
  link.lip_convexity = 0;
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
    // The riser: H over the run its angle gives (W91; a boulder step's face at the angle of repose
    // of its clasts), at least a stretch of the line, at most half the run to the next crest.
    const f64 angle = wide(p.riser_angle_deg) * k_pi / 180.0;
    const f64 riser = H * det::cos(angle) / det::sin(angle);
    const f64 ramp = std::max(step, std::min(.5 * run, riser));
    for (u32 q = i + 1; q <= j; ++q) {
      const f64 u = (s[q] - s[i]) / run;
      const f64 e = H * std::min(1.0, (s[q] - s[i]) / ramp) * (1 - u);
      link.step_depth[q] = std::max(link.step_depth[q], static_cast<f32>(round_to(e, 1e3)));
    }
    link.step_at.push_back(i);
    link.step_height.push_back(static_cast<f32>(round_to(H, 1e3)));
  }
  // **Cascades** (W91): Montgomery & Buffington's cascade is disorganised, tumbling flow over and
  // round individual boulders with no regular sequence, its clasts protruding through the flow. So
  // no steps: boulder clusters at irregular places, their spacing drawn from an exponential of mean
  // `cascade_boulder_widths` widths (a memoryless spacing has no rhythm), at least half a width,
  // each cluster's clasts one to two bankfull depths across (at least 0.25 m, at most 1.5 m).
  {
    Rng rng(hash_combine(link.seed, 0xca5cull));
    f64 at = -1;
    for (u32 i = 0; i < m; ++i) {
      if (link.reach[i] != static_cast<u8>(ChannelReach::Cascade)) {
        at = -1;
        continue;
      }
      const f64 w = wide(link.line[i].width_m);
      const f64 gap = std::max(
          .5 * w, -wide(p.cascade_boulder_widths) * w * det::log(std::max(1e-9, 1.0 - rng.unit())));
      if (at < 0) at = s[i] + .5 * gap;
      if (s[i] < at) continue;
      const f64 depth = section_of(w, wide(link.line[i].depth_m), wide(p.bank_slope)).thalweg;
      link.cluster_at.push_back(i);
      link.cluster_size.push_back(
          static_cast<f32>(round_to(std::clamp(depth * (1 + rng.unit()), .25, 1.5), 1e3)));
      at = s[i] + gap;
    }
  }
  knickpoints(p, link, s);
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
  link.steep_left = 0;
  f64 bed = std::numeric_limits<f64>::infinity();
  Vector<f64> floor(m, 0.0);
  for (u32 i = 0; i < m; ++i) {
    const ChannelPoint& pt = link.line[i];
    const f64 top = round_to(pt.at.y, 1e3);
    const f64 thalweg =
        round_to(section_of(wide(pt.width_m), wide(pt.depth_m), wide(p.bank_slope)).thalweg, 1e3);
    bed = std::min(bed, round_to(top - thalweg, 1e3));
    const f64 b = std::max(bed, round_to(top - k_cut_depth_thalwegs * thalweg, 1e3));
    link.bed[i] = round_to(b - wide(link.step_depth[i]), 1e3);
    floor[i] = round_to(
        top - k_cut_depth_thalwegs * thalweg - .5 * wide(p.fall_min_m) - wide(link.step_depth[i]),
        1e3);
  }
  // **The chute** (W91; channels.md, "Reaches"): away from a knickpoint no stretch of the bed falls
  // at 45 degrees or more, so the water draws no fall where the ground has none to give it. Taken
  // upstream from the mouth: a stretch that would fall that steeply lowers the bed above it to
  // `chute_gradient_max` per metre, never raising the one below, by no more than half `fall_min_m`
  // past the cut's two thalwegs, and the lowering carries on up while it leaves the next stretch
  // that steep (a drop under `fall_min_m` that is no knickpoint is spread over the chute above
  // it). A stretch still that steep after that is counted (`steep_left`).
  if (m >= 2) {
    const Vector<f64> s = lengths_of(link.line);
    const f64 g = wide(p.chute_gradient_max);
    for (u32 i = m - 1; i-- > 0;) {
      if (link.knick[i] && link.knick[i + 1]) continue;
      const f64 run = s[i + 1] - s[i];
      if (link.bed[i] - link.bed[i + 1] < k_fall_gradient * run) continue;
      const f64 most = round_to(link.bed[i + 1] + g * run, 1e3);
      link.bed[i] = std::max(most, std::min(link.bed[i], floor[i]));
    }
    for (u32 i = 0; i + 1 < m; ++i)
      if (!(link.knick[i] && link.knick[i + 1]) &&
          link.bed[i] - link.bed[i + 1] >= k_fall_gradient * (s[i + 1] - s[i]))
        ++link.steep_left;
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

// **The section along the line** (W93; channels.md, "The section"): each point's asymmetry, -1 to
// 1, positive where the line bends left. A bend's share is the line's mean curvature over a bar's
// length about the point (`bar_length_widths`, three widths) — the turn between its heading half
// that length above the point and half below, over the length, each heading a chord of a width —
// times the width times `bend_radius_widths`: 1 where the radius of curvature is that many widths
// or fewer. Over a bar's length because a bar and its pool are built by the flow along a bend,
// not by a kink: the line the trough test draws back turns left and right again within two or
// three widths (W91), and read point by point (a chord of 0.75 widths each way) those kinks put
// a pool every 2.7 widths on the test valley, alternating sides; over a bar's length they
// cancel, and a meander's bend, half of 10.9 widths long, does not. Where a stretch does not bend
// (the mean bend over a pool-to-pool length either way is small) the asymmetry alternates sides
// every `pool_riffle_widths`, a sine of the link's own phase, to `alternate_asymmetry`. Pool-riffle
// and dune-ripple reaches take both; a plane-bed reach half a bend's and no alternation (Montgomery
// & Buffington's plane bed has no free bars, and a bend still forces a pool); the steeper reaches,
// a knickpoint, a fan, a tributary's last points inside its trunk's banks, a bed narrower than
// `bar_min_width_m` and the first and last two widths of a link take none.
constexpr f64 k_bend_window_widths = 4.0;
// The unit heading of the line at arc length `at`: the chord between the points half a `span`
// either way (clamped to the line), found from index `hint` on.
struct Heading {
  f64 x = 0, z = 0;
  bool ok = false;
};
Heading heading_at(const ChannelLink& link, const Vector<f64>& s, f64 at, f64 span) {
  const u32 m = link.line.size();
  if (at - .5 * span < s[0] || at + .5 * span > s[m - 1]) return {};
  // The first point at or past each end of the chord.
  const auto first_at = [&](f64 v) {
    u32 lo = 0, hi = m - 1;
    while (lo < hi) {
      const u32 mid = (lo + hi) / 2;
      if (s[mid] < v)
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo;
  };
  const u32 a = first_at(at - .5 * span), b = first_at(at + .5 * span);
  if (b <= a) return {};
  const f64 dx = link.line[b].at.x - link.line[a].at.x, dz = link.line[b].at.z - link.line[a].at.z;
  const f64 length = std::sqrt(dx * dx + dz * dz);
  if (!(length > 0)) return {};
  return {dx / length, dz / length, true};
}
void plan_section(const ChannelLayer& p, ChannelLink& link) {
  const u32 m = link.line.size();
  link.asym.assign(m, 0.0f);
  link.swell.assign(m, 0.0f);
  link.flood_left.assign(m, 0.0f);
  link.flood_right.assign(m, 0.0f);
  link.entrenchment.assign(m, 0.0f);
  if (m < 3 || !(p.section_asymmetry > 0)) return;
  const Vector<f64> s = lengths_of(link.line);
  Vector<f64> bend(m, 0.0);
  for (u32 i = 0; i < m; ++i) {
    const f64 w = wide(link.line[i].width_m);
    const f64 bar = std::max(k_bend_window_widths * w, 1e-3);
    const Heading above = heading_at(link, s, s[i] - .5 * bar, w);
    const Heading below = heading_at(link, s, s[i] + .5 * bar, w);
    if (!above.ok || !below.ok) continue;
    // The turn between the two headings as twice the tangent of its half, which grows with the
    // turn to a half circle and needs no arc function; over the bar's length, a curvature.
    const f64 dot = above.x * below.x + above.z * below.z;
    if (!(1 + dot > 1e-6)) continue;
    const f64 kappa = 2 * (above.x * below.z - above.z * below.x) / (1 + dot) / bar;
    bend[i] = std::clamp(kappa * w * wide(p.bend_radius_widths), -1.0, 1.0);
  }
  Rng rng(hash_combine(link.seed, 0x5ec7ull));
  f64 theta = rng.unit() * 2 * k_pi;
  Vector<f64> asym(m, 0.0), swell(m, 0.0);
  const u32 keep = std::min<u32>(link.keep, m);
  u32 from = 0, to = 0;
  f64 sum = 0;  // of |bend| over [from, to)
  for (u32 i = 0; i < m; ++i) {
    const f64 w = wide(link.line[i].width_m);
    const f64 pool = std::max(wide(p.pool_riffle_widths) * w, .5);
    if (i > 0) theta += (s[i] - s[i - 1]) * k_pi / pool;
    // The mean bend over a pool-to-pool length either way: a stretch that meanders has its pools
    // at its bends and its riffles at its crossings, and no bar of its own between them.
    while (to < m && s[to] - s[i] <= pool)
      sum += std::fabs(bend[to++]);
    while (from < i && s[i] - s[from] > pool)
      sum -= std::fabs(bend[from++]);
    const f64 straight =
        1 - smoothstep(.1, .3, to > from ? sum / static_cast<f64>(to - from) : 0.0);
    const ChannelReach reach =
        i < link.reach.size() ? static_cast<ChannelReach>(link.reach[i]) : ChannelReach::PoolRiffle;
    f64 share = 0, alternate = 0;
    if (reach == ChannelReach::DuneRipple || reach == ChannelReach::PoolRiffle) {
      share = 1;
      alternate = wide(p.alternate_asymmetry) * straight;
    } else if (reach == ChannelReach::PlaneBed) {
      share = .5;
    }
    if (i >= keep || (i < link.knick.size() && link.knick[i]) || w < wide(p.bar_min_width_m))
      share = 0;
    share *= smoothstep(0, 2 * w, s[i]) * smoothstep(0, 2 * w, s[m - 1] - s[i]) *
             (1 - fan_share(p, link, s, i));
    const f64 v = bend[i] + alternate * (1 - std::fabs(bend[i])) * det::sin(theta);
    asym[i] = std::clamp(v, -1.0, 1.0) * share * wide(p.section_asymmetry);
    swell[i] = (1 - 2 * std::min(1.0, std::fabs(v))) * share * wide(p.section_asymmetry);
  }
  // A reach's edge and a knickpoint's are steps in the share: two binomial passes ease them.
  for (Vector<f64>* values : {&asym, &swell}) {
    for (u32 pass = 0; pass < 2; ++pass) {
      f64 before = (*values)[0];
      for (u32 i = 1; i + 1 < m; ++i) {
        const f64 here = (*values)[i];
        (*values)[i] = .25 * before + .5 * here + .25 * (*values)[i + 1];
        before = here;
      }
    }
  }
  for (u32 i = 0; i < m; ++i) {
    link.asym[i] = static_cast<f32>(round_to(asym[i], 1e3));
    link.swell[i] = static_cast<f32>(round_to(swell[i], 1e3));
  }
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
  if (!in(wide(p.riser_angle_deg), 20, 44)) return fail(error, "riser_angle_deg");
  if (!in(wide(p.cascade_boulder_widths), .5, 10)) return fail(error, "cascade_boulder_widths");
  if (!in(wide(p.knick_window_widths), 1, 20) || !in(wide(p.knick_window_min_m), .5, 50))
    return fail(error, "knick_window_widths/knick_window_min_m");
  if (!in(wide(p.chute_gradient_max), .2, 1)) return fail(error, "chute_gradient_max");
  if (!in(wide(p.section_asymmetry), 0, 1) || !in(wide(p.bend_radius_widths), 1, 20) ||
      !in(wide(p.cut_bank_slope), .25, 8) || !in(wide(p.alternate_asymmetry), 0, 1) ||
      !in(wide(p.riffle_widening), 0, .5) || !in(wide(p.bank_round), 0, 1))
    return fail(error,
                "section_asymmetry/bend_radius_widths/cut_bank_slope/alternate_asymmetry/"
                "riffle_widening/bank_round");
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
  for (ChannelLink& link : n.links)
    plan_section(p, link);
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
  // The surface at the fan's head, which the fan grades to the body's level, and the head's own
  // share of the fan (the first fan point already lies partway along it).
  f64 fan_head = std::numeric_limits<f64>::quiet_NaN(), fan_t0 = 0;
  for (u32 i = 0; i < m; ++i) {
    const ChannelPoint& pt = link.line[i];
    const f64 thalweg =
        round_to(section_of(wide(pt.width_m), wide(pt.depth_m), wide(p.bank_slope)).thalweg, 1e3);
    f64 water = 0;
    if (cut) {
      // Laid in the cut (W89): held down the line, except where the cut's bed climbs over a sill,
      // where the water keeps half its depth over it rather than run under the ground.
      bed = link.bed[i];
      water = wide(stage) * thalweg;
      surface =
          std::max(std::min(surface, round_to(bed + water, 1e3)), round_to(bed + .5 * water, 1e3));
    } else {
      // Laid on the ground (W86): the bed the ground's running minimum, the surface `stage` of the
      // bankfull depth over it, never rising downstream.
      bed = std::min(bed, pt.at.y);
      water = wide(stage) * wide(pt.depth_m);
      surface = std::min(surface, round_to(bed + water, 1e3));
    }
    // The fan (W90): from the fan's head the surface grades to the body's level at the shore, and
    // never stands below it there (the body backs the water up). The grade starts at the first fan
    // point itself (W91 continuation: anchored there with that point's own share, the head
    // dropped by the share at once — 0.43 m on v36's link 690, a 36-degree chute into the sea with
    // 0.19 m of water — and ran under the cut's bed for five points), and it never runs under the
    // bed: a centimetre of water at least, so every depth the description writes is positive.
    const f64 t = fan_share(p, link, s, i);
    if (t > 0) {
      if (std::isnan(fan_head)) {
        fan_head = surface;
        fan_t0 = t;
      }
      const f64 u = fan_t0 < 1 ? (t - fan_t0) / (1 - fan_t0) : 1.0;
      const f64 graded = std::max(round_to(fan_head + u * (link.mouth_level - fan_head), 1e3),
                                  round_to(bed + k_min_water_m, 1e3));
      surface = std::max(round_to(link.mouth_level, 1e3), std::min(surface, graded));
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

// **The ribbon on the ground** (W91; channels.md, "Invariants"): water never stands over the ground
// at its own edge. At each point the ribbon's two edges — `k_ribbon_half_widths` of the width
// across the line each way, across the mean of its two stretches' headings, as the water's mesher
// lays a section — are tested against the ground (the ground before any cut: a cut only lowers
// it, and the cut's own section rises past the bank's top, so it is the open ground beside the
// channel that is tested). An edge whose ground is below the surface is drawn in to where the
// ground meets the surface (12 halvings), and the point and its width are moved and narrowed to
// the two edges found: the ribbon narrows to the water that can stand there. Where the two leave
// less than `k_min_ribbon_widths` of the width (at least `k_min_ribbon_m`) the bed itself stands
// over the downhill ground: the ribbon keeps that width about the line, and its surface is lowered
// to the lower edge's ground less `k_edge_margin_m` (`fit` 2). A point under a water body is left
// as it is (the body stands over it). Moving points turns the headings a little, so a second pass
// over the fitted line's own headings narrows (about the point, never moving it) or lowers
// whatever still stands over the ground: the line the water's mesher lays is the one tested.
constexpr f64 k_min_ribbon_widths = .25;
constexpr f64 k_min_ribbon_m = .2;
constexpr f64 k_edge_margin_m = .02;
// How far an edge may stand over the ground before it is drawn in: three centimetres, under the
// lattice's height step on a slope and under what the shore's fade shows.
constexpr f64 k_edge_tolerance_m = .03;
struct Fitted {
  Vector<f64> x, z, width, surface;
  Vector<u8> fit;
};
void fit_ribbon(u32 m, const scene_gen::Ground& ground, const scene_gen::Exclusion* water,
                bool move, Fitted& f) {
  auto g_at = [&](f64 x, f64 z) { return ground_at(ground, x, z); };
  const Vector<f64> px = f.x, pz = f.z;  // the headings are the line's as the pass found it
  for (u32 i = 0; i < m; ++i) {
    const f64 x = f.x[i], z = f.z[i], surface = f.surface[i];
    const f64 wet = surface - k_edge_tolerance_m;  // the ground an edge may stand on
    if (water != nullptr && water->body_count() > 0 &&
        water->excluded_mm(ground, scene_gen::nearest_mm(x), scene_gen::nearest_mm(z),
                           scene_gen::k_exclude_water))
      continue;
    f64 hx = 0, hz = 0;
    for (const u32 a : {i > 0 ? i - 1 : 0u, i}) {
      const u32 b = std::min(a + 1, m - 1);
      const f64 dx = px[b] - px[a], dz = pz[b] - pz[a];
      const f64 l = std::sqrt(dx * dx + dz * dz);
      if (a != b && l > 0) {
        hx += dx / l;
        hz += dz / l;
      }
    }
    const f64 hl = std::sqrt(hx * hx + hz * hz);
    if (!(hl > 0)) continue;
    const f64 ax = -hz / hl, az = hx / hl, e = k_ribbon_half_widths * f.width[i];
    f64 reach[2] = {e, e};
    for (u32 side = 0; side < 2; ++side) {
      const f64 sgn = side == 0 ? 1.0 : -1.0;
      if (g_at(x + sgn * ax * e, z + sgn * az * e) >= wet) continue;
      f64 lo = 0, hi = e;
      if (g_at(x, z) < wet) hi = 0;
      for (u32 k = 0; k < 12 && hi > 0; ++k) {
        const f64 mid = .5 * (lo + hi);
        if (g_at(x + sgn * ax * mid, z + sgn * az * mid) >= wet)
          lo = mid;
        else
          hi = mid;
      }
      reach[side] = lo;
    }
    if (reach[0] >= e && reach[1] >= e) continue;
    const f64 half = move ? .5 * (reach[0] + reach[1]) : std::min(reach[0], reach[1]);
    const f64 least =
        k_ribbon_half_widths * std::max(k_min_ribbon_m, k_min_ribbon_widths * f.width[i]);
    if (half >= least) {
      const f64 shift = move ? .5 * (reach[0] - reach[1]) : 0.0;
      f.x[i] = round_to(x + ax * shift, 1e3);
      f.z[i] = round_to(z + az * shift, 1e3);
      // Rounded down to the millimetre, so the rounding never carries an edge back out.
      f.width[i] = std::floor(half / k_ribbon_half_widths * 1e3) / 1e3;
      f.fit[i] = std::max<u8>(f.fit[i], 1);
    } else {
      const f64 lower =
          std::min(g_at(x + ax * least, z + az * least), g_at(x - ax * least, z - az * least));
      f.width[i] = round_to(least / k_ribbon_half_widths, 1e3);
      f.surface[i] = round_to(std::min(surface, lower - k_edge_margin_m), 1e3);
      f.fit[i] = 2;
    }
  }
}
// A ground standing flat at the height its context holds: the covered-ribbon test asks the
// exclusion's water kind whether a body's level stands over a point's surface.
f64 flat_height(const void* context, i64, i64) noexcept {
  return *static_cast<const f64*>(context);
}
}  // namespace

JsonValue water_channels_json(const ChannelLayer& p, const ChannelNetwork& n, f64 min_area_m2,
                              f32 stage, bool cut, const scene_gen::Ground* ground,
                              const scene_gen::Exclusion* water, RibbonTrim* trim) {
  const ChannelLayer bank_defaults;  // the recipe's own defaults, which the description's are
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
    const u32 kept = std::min<u32>(link.keep, link.line.size());
    if (kept < 2 || wide(link.line[kept - 1].area_m2) < min_area_m2) continue;
    const Water& w = waters[k];
    // **Covered by a body** (W91 continuation; channels.md, "Mouths"): a point inside a body's
    // outline whose surface lies under the body's level is under the body's own water, which
    // covers it — drawn, the ribbon is a second surface a few centimetres under the body's, and
    // the body's refraction meets it as a solid floor (v36's link 779, 0.149 m under the east lake
    // inside its outline: item 25's flat shapes). Such points are trimmed off the ribbon's two
    // ends (the body test is the exclusion's water kind against a ground standing at the point's
    // surface, so an open body's sea covers whatever lies under its level).
    u32 a = 0, b = kept;
    if (water != nullptr && water->body_count() > 0) {
      auto covered = [&](u32 i) {
        const f64 height = w.surface[i] + 1e-3;
        scene_gen::Ground flat;
        flat.context = &height;
        flat.surface_fn = &flat_height;
        return water->excluded_mm(flat, scene_gen::nearest_mm(link.line[i].at.x),
                                  scene_gen::nearest_mm(link.line[i].at.z),
                                  scene_gen::k_exclude_water);
      };
      while (a < b && covered(a))
        ++a;
      while (b > a && covered(b - 1))
        --b;
      if (trim != nullptr && (a > 0 || b < kept)) {
        ++trim->blocks;
        trim->points += a + (kept - b);
        if (link.mouth && b < kept) ++trim->mouth_fans;
        for (u32 i = 0; i < kept; ++i)
          if (i < a || i >= b)
            trim->at.push_back(WorldPos{link.line[i].at.x, w.surface[i], link.line[i].at.z});
        if (b - a < 2) ++trim->dropped;
      }
    }
    if (b - a < 2) continue;
    const u32 m = b - a;
    Fitted f;
    for (u32 j = 0; j < m; ++j) {
      const u32 i = a + j;
      f.x.push_back(round_to(link.line[i].at.x, 1e3));
      f.z.push_back(round_to(link.line[i].at.z, 1e3));
      f.width.push_back(round_to(wide(link.line[i].width_m), 1e3));
      f.surface.push_back(w.surface[i]);
      f.fit.push_back(0);
    }
    if (ground != nullptr) {
      fit_ribbon(m, *ground, water, true, f);
      fit_ribbon(m, *ground, water, false, f);
    }
    JsonValue o = JsonValue::object();
    o["name"] = JsonValue(p.name + " link " + std::to_string(k));
    JsonValue points = JsonValue::array(), widths = JsonValue::array(), depths = JsonValue::array(),
              slopes = JsonValue::array(), reach = JsonValue::array(), fits = JsonValue::array();
    for (u32 j = 0; j < m; ++j) {
      const u32 i = a + j;
      const ChannelPoint& pt = link.line[i];
      // A point whose surface the fit lowered under its bed keeps a centimetre of water: the
      // description's depth is positive, and the water lies under the ground there.
      const f64 depth = f.fit[j] == 2 ? std::max(k_min_water_m, f.surface[j] - w.bed[i])
                                      : std::max(0.0, f.surface[j] - w.bed[i]);
      JsonValue at = JsonValue::array();
      at.push_back(JsonValue(f.x[j]));
      at.push_back(JsonValue(f.surface[j]));
      at.push_back(JsonValue(f.z[j]));
      points.push_back(std::move(at));
      widths.push_back(JsonValue(f.width[j]));
      depths.push_back(JsonValue(round_to(depth, 1e3)));
      fits.push_back(JsonValue(static_cast<u32>(f.fit[j])));
      slopes.push_back(JsonValue(round_to(wide(pt.slope), 1e5)));
      reach.push_back(JsonValue(static_cast<u32>(i < link.reach.size() ? link.reach[i] : 0)));
    }
    o["points"] = std::move(points);
    o["width_m"] = std::move(widths);
    o["depth_m"] = std::move(depths);
    o["slope"] = std::move(slopes);
    o["seed"] = JsonValue(link.seed);
    o["reach"] = std::move(reach);
    if (ground != nullptr) o["fit"] = std::move(fits);
    // The bank's reach, which the placements' exclusion reads from the description (T78; it copied
    // the defaults until then). Written only where the recipe moved it, so a description at the
    // defaults is the bytes it was.
    if (p.bank_width_widths != bank_defaults.bank_width_widths ||
        p.bank_width_m != bank_defaults.bank_width_m) {
      o["bank_width_widths"] = JsonValue(wide(p.bank_width_widths));
      o["bank_width_m"] = JsonValue(wide(p.bank_width_m));
    }
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
    JsonValue left = JsonValue::array(), right = JsonValue::array();
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
      // The section here (W93; channels.md, "The section"): the trapezoid at a riffle, the
      // triangle at a bend's apex, the pool that much under the bed's line.
      const BendSection section = bend_section(p, wide(pt.width_m), wide(pt.depth_m),
                                               i < link.asym.size() ? wide(link.asym[i]) : 0.0,
                                               i < link.swell.size() ? wide(link.swell[i]) : 0.0);
      widths.push_back(JsonValue(round_to(section.width * spread, 1e3)));
      // The bed below the top: the thalweg, more where the cut runs through a rise, a step's pool
      // or a confluence's grade, and a bend's pool.
      // A riffle's bed stands that much over the bed's line and a pool's that much under it: the
      // section's own thalweg at its own width against the trapezoid's at the hydraulic
      // geometry's, so the area is kept as the width swells and narrows.
      const f64 line_thalweg =
          section_of(wide(pt.width_m), wide(pt.depth_m), wide(p.bank_slope)).thalweg;
      depths.push_back(JsonValue(round_to(
          std::max(1e-3, round_to(top - link.bed[i], 1e3) + (section.thalweg - line_thalweg)) +
              section.pool,
          1e3)));
      left.push_back(JsonValue(round_to(section.slope_left, 1e4)));
      right.push_back(JsonValue(round_to(section.slope_right, 1e4)));
    }
    o["points"] = std::move(points);
    o["width_m"] = std::move(widths);
    o["depth_m"] = std::move(depths);
    o["bank_slope"] = JsonValue(wide(p.bank_slope));
    // W93's fields only where the layer asks for them, so a layer with none writes W91's bytes.
    if (p.section_asymmetry > 0) {
      o["bank_slope_left"] = std::move(left);
      o["bank_slope_right"] = std::move(right);
    }
    if (p.bank_round > 0) o["bank_round"] = JsonValue(wide(p.bank_round));
    list.push_back(std::move(o));
  }
  return list;
}
}  // namespace engine::channels

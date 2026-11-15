// The network (docs/subsystems/channels.md, "The network"; grounding in
// docs/experiments/channels.md): the lattice, the fill, the routing, the heads, the links and their
// lines. Everything a decision rests on is an integer or IEEE arithmetic with no C library call
// (the det:: functions stand in for exp, log, sin and cos), so the network is the same on every
// toolchain and at every thread count.
#include "detail.h"

#include <core/jobs/job_system.h>

#include <algorithm>
#include <atomic>
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
// How many times the line is moved between its banks (below): the easing takes some of each move
// back, and a third round was measured to undo part of the second.
constexpr u32 k_bank_rounds = 2;
// The steep line's wander (build_line): a knot of its noise every this many widths, and the
// narrowest width it is scaled by; and the sigma, in widths, the kinks are low-passed over.
constexpr f64 k_wander_knot_widths = 5.0;
constexpr f64 k_wander_knot_min_m = 5.0;
constexpr f64 k_wander_min_width_m = 1.0;
constexpr f64 k_kink_sigma_widths = 0.5;

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
  // **A steep line wanders** (W93, continued; channels.md, "The steep reaches"): above the
  // meanders' slope the routing's line down a planar slope is a ruled line, and a gully is not
  // one. Where the meander fades the line takes an irregular offset instead — a value noise of
  // the link's own, a knot every `k_wander_knot_widths` widths (5 m at least), up to
  // `steep_wander_widths` widths (of at least `k_wander_min_width_m`) either way — which the
  // trough test below holds to the trough as it holds a meander: in a gully with sides the wander
  // is drawn back, and on a plane it stays.
  const bool wanders = link.meander < 1 && p.steep_wander_widths > 0;
  if ((link.meander > 0 && p.meander_amplitude_coeff > 0) || wanders) {
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
      if (wanders) {
        const f64 knot = std::max(k_wander_knot_widths * wi, k_wander_knot_min_m);
        const f64 in = smoothstep(0.0, 1.0, std::min(ps[i], length - ps[i]) / knot);
        o += (1 - link.meander) * wide(p.steep_wander_widths) * std::max(wi, k_wander_min_width_m) *
             noise(3, ps[i] / knot) * in;
      }
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
  // **The kinks taken out** (W93, continued; channels.md, "The planform"). The trough test draws
  // single points back, and what it leaves turns left and right again inside two or three widths;
  // the cut's banks follow the line, so they zigzagged. The line is low-passed once more, at the
  // channel's own scale: a Gaussian along the length of sigma `k_kink_sigma_widths` widths (two
  // steps at least), eased to nothing at the two ends, which stay where the junctions are. Half a
  // width is far under a meander's wavelength (10.9 widths: a bend keeps 96 % of its amplitude) and
  // moves a point by a fraction of the width, inside the banks the next test holds.
  {
    Vector<f64> sx(fx), sz(fz);
    for (u32 i = 1; i + 1 < m; ++i) {
      const f64 wi = hydraulic(wide(p.width_at_km2_m), wide(p.width_exponent), pa[i]);
      const f64 sigma = std::max(k_kink_sigma_widths * wi, 2 * wide(p.step_m));
      f64 ax = 0, az = 0, aw = 0;
      const auto add = [&](u32 k) {
        const f64 d = ps[k] - ps[i];
        const f64 weight = det::exp(-d * d / (2 * sigma * sigma));
        ax += weight * fx[k];
        az += weight * fz[k];
        aw += weight;
      };
      for (u32 k = i; ps[i] - ps[k] <= 3 * sigma; --k) {
        add(k);
        if (k == 0) break;
      }
      for (u32 k = i + 1; k < m && ps[k] - ps[i] <= 3 * sigma; ++k)
        add(k);
      const f64 t = std::min(1.0, std::min(ps[i], length - ps[i]) / sigma);
      sx[i] = fx[i] + t * (ax / aw - fx[i]);
      sz[i] = fz[i] + t * (az / aw - fz[i]);
    }
    fx = std::move(sx);
    fz = std::move(sz);
  }
  // **The line between its banks** (W93; channels.md, "A line across a slope"). The trough test
  // above holds the line's centre within half a depth of the trough's floor; a channel is a
  // width wide, and where the ground falls across it one bank's top can stand more than half a
  // depth under the centre's ground while the centre passes: the cut then has a bench there and
  // no bank on the downhill side. A stream does not traverse a slope, so the line is what is
  // wrong, and it is moved: across its own heading toward the lower bank, a quarter of the width
  // at a time and a width at the most, until neither bank's ground (half the width either way)
  // stands more than `k_bank_fall` of the depth under the ground at the point. The moves are
  // eased along the line by three binomial passes, which takes a little of each move back, so
  // the whole is done `k_bank_rounds` times; the two ends stay where the junctions are.
  // Rills under a metre wide are left: the lattice the cut is drawn on is a quarter of that.
  link.bank_moved = 0;
  for (u32 round = 0; round < (p.bank_move ? k_bank_rounds : 0u); ++round) {
    constexpr f64 k_bank_fall = .5;
    Vector<f64> move(m, 0.0), nx(m, 0.0), nz(m, 0.0);
    for (u32 i = 1; i + 1 < m; ++i) {
      const f64 tx = fx[i + 1] - fx[i - 1], tz = fz[i + 1] - fz[i - 1];
      const f64 tl = std::sqrt(tx * tx + tz * tz);
      const f64 wi = hydraulic(wide(p.width_at_km2_m), wide(p.width_exponent), pa[i]);
      if (tl <= 0 || wi < 1.0) continue;
      nx[i] = -tz / tl;
      nz[i] = tx / tl;
      const f64 di = hydraulic(wide(p.depth_at_km2_m), wide(p.depth_exponent), pa[i]);
      // **A move is kept only where it finds a bank** (W116; channels.md, "A line across a
      // slope"): the place that holds both banks, or else the tested place (a width across at
      // the most) whose bank falls least, where that is a tenth of a depth less than at the line.
      // On a hillside every place a width across falls as much or more, and W93's move (which
      // took a width's move untested whenever no place held) carried the line off the trough the
      // routing found and down the slope: on W116's contour stream it kinked the line where the
      // move began and ended, left its ribbon half a metre over the edited ground there and
      // turned the stream's profile into step-pool and cascade. There the line stays, and the
      // section is seated under its lower bank instead (`seat_section`).
      f64 across = 0, best_across = 0, best_fall = 0, first_fall = 0;
      bool found = false;
      for (u32 tries = 0; tries < 5; ++tries) {
        const f64 cx = fx[i] + nx[i] * across, cz = fz[i] + nz[i] * across;
        const f64 left = ground_at(ground, cx + nx[i] * .5 * wi, cz + nz[i] * .5 * wi);
        const f64 right = ground_at(ground, cx - nx[i] * .5 * wi, cz - nz[i] * .5 * wi);
        const f64 fall = ground_at(ground, cx, cz) - std::min(left, right);
        if (tries == 0) first_fall = best_fall = fall;
        if (fall <= k_bank_fall * di) {
          found = true;
          break;
        }
        if (fall < best_fall) {
          best_fall = fall;
          best_across = across;
        }
        if (tries == 4) break;
        across += (left < right ? .25 : -.25) * wi;
      }
      move[i] = found ? across : (best_fall < first_fall - .1 * di ? best_across : 0.0);
    }
    for (u32 pass = 0; pass < 3; ++pass) {
      Vector<f64> next = move;
      for (u32 i = 1; i + 1 < m; ++i)
        next[i] = .25 * move[i - 1] + .5 * move[i] + .25 * move[i + 1];
      move = std::move(next);
    }
    for (u32 i = 1; i + 1 < m; ++i) {
      if (move[i] == 0) continue;
      if (round == 0) ++link.bank_moved;
      fx[i] += nx[i] * move[i];
      fz[i] += nz[i] * move[i];
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
//
// **The plan's own bodies** (W122; channels.md, "Hollows") split a line the same way: a point is
// under one where its nearest lattice cell is the body's (`ChannelNetwork::body_of`) and the
// ground at the point stands under the body's level. A mouth into one carries the body's level
// exactly, and what leaves it carries it as its `outlet_level`.
constexpr f64 k_run_min_m = 4.0;
void apply_water(ChannelNetwork& n, const scene_gen::Ground& ground,
                 const scene_gen::Exclusion* water) {
  const bool scene_bodies = water != nullptr && water->body_count() > 0;
  bool plan_bodies = false;
  for (const Hollow& h : n.basins.hollows)
    plan_bodies = plan_bodies || h.body();
  if (!scene_bodies && !plan_bodies) return;
  // 0 dry; 1 under a scene's body; 2 + k under the plan's body, hollow k.
  auto wet_id = [&](f64 x, f64 z) -> u32 {
    const i64 x_mm = scene_gen::nearest_mm(x), z_mm = scene_gen::nearest_mm(z);
    if (scene_bodies && water->excluded_mm(ground, x_mm, z_mm, scene_gen::k_exclude_water))
      return 1;
    if (!plan_bodies || n.body_of.empty()) return 0;
    const i64 c = floor_div(x_mm - n.x_mm + n.spacing_mm / 2, n.spacing_mm);
    const i64 r = floor_div(z_mm - n.z_mm + n.spacing_mm / 2, n.spacing_mm);
    if (c < 0 || r < 0 || c >= static_cast<i64>(n.width) || r >= static_cast<i64>(n.depth))
      return 0;
    const u32 body = n.body_of[static_cast<u32>(r) * n.width + static_cast<u32>(c)];
    // (A scene's own body under its spill marks its cells too; that one is the exclusion's.)
    if (body == 0 || !n.basins.hollows[body - 1].body() ||
        !(ground_at(ground, x, z) < n.basins.hollows[body - 1].level_m))
      return 0;
    return 1 + body;
  };
  // Under body `id` (as `wet_id` numbers them) at a place between two of a line's points: a
  // scene's body by the exclusion; the plan's by the ground against its level, since a line's
  // points next to one under the body are the body's own shore whichever cell is nearest them.
  auto wet_at = [&](f64 x, f64 z, u32 id) {
    if (id >= 2) return ground_at(ground, x, z) < n.basins.hollows[id - 2].level_m;
    return wet_id(x, z) != 0;
  };
  // The shoreline between a dry point and a wet one: 16 halvings of the stretch (under 0.1 mm
  // for the 0.5 m step), the dry point's numbers, the ground's height there.
  auto shore = [&](const ChannelPoint& dry, const ChannelPoint& wet, u32 id) {
    f64 lo = 0, hi = 1;
    for (u32 k = 0; k < 16; ++k) {
      const f64 mid = .5 * (lo + hi);
      if (wet_at(dry.at.x + mid * (wet.at.x - dry.at.x), dry.at.z + mid * (wet.at.z - dry.at.z),
                 id))
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
    Vector<u32> under(m, 0);
    Vector<u8> wet(m, 0);
    bool any = false;
    for (u32 i = 0; i < m; ++i) {
      under[i] = wet_id(link.line[i].at.x, link.line[i].at.z);
      any = any || under[i] != 0;
    }
    if (!any) continue;
    // A plan's body reaches along the line as far as the ground stands under its level: the
    // lattice's cells end a cell short of the lip, and the lip is where the line leaves.
    const auto reaches = [&](u32 i, u32 from) {
      return under[i] == 0 && under[from] >= 2 &&
             link.line[i].at.y < n.basins.hollows[under[from] - 2].level_m;
    };
    for (u32 i = 1; i < m; ++i)
      if (reaches(i, i - 1)) under[i] = under[i - 1];
    for (u32 i = m - 1; i-- > 0;)
      if (reaches(i, i + 1)) under[i] = under[i + 1];
    for (u32 i = 0; i < m; ++i)
      wet[i] = under[i] != 0 ? 1 : 0;
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
        out.line.push_back(shore(link.line[run.a], link.line[run.a - 1], under[run.a - 1]));
        // The level of the body it leaves (W121), read where the line was last under it.
        const ChannelPoint& last = link.line[run.a - 1];
        f64 level = 0;
        out.outlet_level = out.line.back().at.y;
        if (under[run.a - 1] >= 2)
          // The plan's own body (W122) stands `k_sill_over_m` under the sill its outlet holds,
          // so no rounding of the cut's stamp puts a thread of the lip under the body's level
          // for its flood to follow down the channel.
          out.outlet_level = n.basins.hollows[under[run.a - 1] - 2].level_m + k_sill_over_m;
        else if (water != nullptr && water->water_level_mm(ground, scene_gen::nearest_mm(last.at.x),
                                                           scene_gen::nearest_mm(last.at.z), level))
          out.outlet_level = level;
      }
      for (u32 i = run.a; i < run.b; ++i)
        out.line.push_back(link.line[i]);
      kept += run.b - run.a;
      if (run.b < m) {
        const ChannelPoint end = shore(link.line[run.b - 1], link.line[run.b], under[run.b]);
        out.mouth = true;
        // The body's own level (W121): the ground at the shoreline is it only where the body's
        // outline is the level's contour; an outline standing in water puts the ground under it.
        // The plan's own body's level is the plan's (W122).
        out.mouth_level = end.at.y;
        const ChannelPoint& first = link.line[run.b];
        f64 level = 0;
        if (under[run.b] >= 2)
          out.mouth_level = n.basins.hollows[under[run.b] - 2].level_m;
        else if (water != nullptr &&
                 water->water_level_mm(ground, scene_gen::nearest_mm(first.at.x),
                                       scene_gen::nearest_mm(first.at.z), level))
          out.mouth_level = level;
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

// The cut's bed down a link (channels.md, "The cut", "Hollows", and W90's steps, fan and grade):
// the top of the bank less the thalweg, **held at its running minimum down the line** — a bed
// never rises downstream, so where the ground at the line rises the cut is that much deeper (a
// breach) — and begun no higher than `start`, the lowest bed that ends where the link begins;
// then lowered by the steps' pools; then W121's rules where the link meets a body: an outlet's
// lip held at the level it leaves, a mouth's fan graded to its body's level and no bed under it.
//
// Until W122 the hold reached two thalwegs under the ground and no further, so past a rise of one
// thalweg the bed climbed with the ground: on the forest 110 wet lines climbed out of basins 28
// to 134 m deep. A hollow deeper than `breach_max_m` is no longer crossed by a line at all (it is
// a body of the plan, or a sink), so what is left to hold through is a lip the limit bounds, or a
// rise the line itself strays over; both are counted (`measure_hollows`).
// The top of the bank at point i: the ground at the line less the seat (W116).
f64 top_of(const ChannelLink& link, u32 i) {
  return link.line[i].at.y - (i < link.seat.size() ? wide(link.seat[i]) : 0.0);
}
// **A lip is held by every link that begins on it** (W122; channels.md, "Hollows"). A lake of the
// plan stands a notch under its spill, so the cells between its shore and its lip are dry ground
// inside the hollow, and the lines that cross the water merge there as readily as under it. A
// link that begins at such a junction, or at the spill itself, begins at no shore and is no
// outlet: its cut dug the lip to its own bed, under the lake's level, with only the natural rim
// beside it between that pit and the water (on the forest 0.12 to 0.29 m under the level at the
// spills of its three largest lakes, the rim holding by 1 to 9 cm). So a link that begins within
// a cell and a half of a lake's spill holds that lake's sill from its first point for as long as
// its points stay that near: point `i` of `link` is such a point, and `sill` the level to hold
// where the top stands over it. An outlet holds its own (`cut_bed`).
bool lip_sill(const ChannelNetwork& n, const ChannelLink& link, u32 i, f64& sill) {
  if (link.outlet || link.line.empty() || n.basins.hollows.empty()) return false;
  const f64 reach = 1.5 * static_cast<f64>(n.spacing_mm) / 1000.0;
  const auto near = [&](const ChannelPoint& pt, const Hollow& h) {
    const f64 dx = pt.at.x - h.spill_x, dz = pt.at.z - h.spill_z;
    return dx * dx + dz * dz <= reach * reach;
  };
  for (const Hollow& h : n.basins.hollows) {
    if (h.kind != HollowKind::Lake) continue;
    if (!near(link.line.front(), h) || !near(link.line[i], h)) continue;
    sill = round_to(h.level_m + k_sill_over_m, 1e3);
    return true;
  }
  return false;
}
void cut_bed(const ChannelLayer& p, ChannelLink& link,
             f64 start = std::numeric_limits<f64>::infinity()) {
  const u32 m = link.line.size();
  link.bed.assign(m, 0.0);
  link.steep_left = 0;
  f64 bed = start;
  Vector<f64> floor(m, 0.0);
  for (u32 i = 0; i < m; ++i) {
    const ChannelPoint& pt = link.line[i];
    const f64 top = round_to(top_of(link, i), 1e3);
    const f64 thalweg =
        round_to(section_of(wide(pt.width_m), wide(pt.depth_m), wide(p.bank_slope)).thalweg, 1e3);
    bed = std::min(bed, round_to(top - thalweg, 1e3));
    link.bed[i] = round_to(bed - wide(link.step_depth[i]), 1e3);
    floor[i] = round_to(bed - .5 * wide(p.fall_min_m) - wide(link.step_depth[i]), 1e3);
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
  // **An outlet's start** (W121; channels.md, "The base level, and a fan below a cliff"): the cut
  // of a link that leaves a body stands under that body's level nowhere the ground at the line
  // stood over it. A lake spills over its outlet's sill at its level, so a bed under the level
  // where the ground stands over it is a dry hollow under the waterline beside the body (the
  // owner's mark 8 was one, at the start of an outlet whose line climbed out of the pond). Where
  // the ground falls under the level, as below a sill it does, the bed is the cut's own.
  if (link.outlet) {
    const f64 level = round_to(link.outlet_level, 1e3);
    for (u32 i = 0; i < m; ++i) {
      const f64 top = round_to(top_of(link, i), 1e3);
      if (top >= level)
        link.bed[i] = std::max(link.bed[i], std::min(level, round_to(top - .01, 1e3)));
    }
  }
  if (!link.mouth || m < 2) return;
  const Vector<f64> s = lengths_of(link.line);
  const ChannelPoint& end = link.line[m - 1];
  // **The base level** (W121; channels.md, "Mouths"): the fan grades the bed to the body's level
  // at the shore, and nowhere does the cut stand under it. A stream is graded to the body it ends
  // in and cuts no deeper than that level, its base level (Powell 1875; Mackin 1948); and a body's
  // water ends at its outline, so a bed under the level outside it was a dry hole under the
  // waterline beside the body (the owner's walk of 2026-11-13, mark 8: W90's fan graded the shore
  // to half a thalweg under the level and spread to twice the width, and its cap and spread dug the
  // dry ground beside an oblique shore). Every point but the shore's lies outside the body, so the
  // bed is held at the level wherever the cut's top stands over it, and a centimetre under the top
  // where the ground itself stands under it (an outline standing in water).
  const f64 level = round_to(link.mouth_level, 1e3);
  const f64 fan = wide(p.fan_widths) * wide(end.width_m);
  u32 f = m - 1;
  while (f > 0 && s[m - 1] - s[f - 1] <= fan)
    --f;
  // **A fan is what lies below a cliff** (W121 with W119): where the ground under the line falls
  // at 45 degrees or more between two of the fan's points (a sea cliff's face), the fan begins
  // below it and the bed above follows the cut's own line to the lip, so the water falls there.
  // Graded from above the cliff, the last three widths of bed were an 80-degree ramp cut into the
  // cliff's top.
  for (u32 i = f; i + 1 < m; ++i)
    if (link.line[i].at.y - link.line[i + 1].at.y >= k_fall_gradient * (s[i + 1] - s[i])) f = i + 1;
  const f64 b0 = link.bed[f], span = s[m - 1] - s[f];
  // The fan's bed takes its running minimum too (W122): where the ground dips inside the fan
  // the grade would follow it down and climb back out of the dip.
  f64 low = std::numeric_limits<f64>::infinity();
  for (u32 i = f; i < m; ++i) {
    const f64 t = span > 0 ? (s[i] - s[f]) / span : 1.0;
    const f64 top = round_to(top_of(link, i), 1e3);
    low = std::min(low, std::min(round_to(b0 + t * (level - b0), 1e3), round_to(top - .01, 1e3)));
    link.bed[i] = low;
  }
  for (u32 i = 0; i < m; ++i) {
    const f64 top = round_to(top_of(link, i), 1e3);
    link.bed[i] = std::max(link.bed[i], std::min(level, round_to(top - .01, 1e3)));
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
// How far downstream of a bend's apex its pool lies, in widths: 0.36 of the channel distance from
// the apex to the next inflection (NRCS NEH 654 ch. 12, the 1981 Red River survey's mean; range
// -0.4 to 1.08), which for a meander 10.9 widths long is a quarter of it, 2.7 widths.
constexpr f64 k_pool_lag_widths = 1.0;
// A value noise along a line: knots at the whole numbers of `at`, eased between, -1 to 1.
f64 value_noise(u64 seed, f64 at) {
  const f64 k = std::floor(at);
  const i64 knot = static_cast<i64>(k);
  const f64 t = at - k, e = t * t * (3 - 2 * t);
  const f64 a = place_noise(seed, knot, 0), b = place_noise(seed, knot + 1, 0);
  return a + (b - a) * e;
}
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
// **The section seated under its lower bank** (W116; channels.md, "A line across a slope"; the
// owner's walk of 2026-11-12, marks 3 to 5). The cut's top of bank was the ground at the line, so
// where the ground falls across the line the downhill bank's ground stood under the top, and
// under the bed and the water too past a little cross-slope: the cut, which only lowers, left the
// section standing over the hillside there. Bankfull is the stage at which the water first
// spills, so it is the lower bank's top that bounds it; a stream whose water stood over its
// downhill bank would leave down the fall line. So the section is **cut deeper**: its top is the
// ground at the lower of its two bank tops — read across the line's heading over a width, the
// lowest over the half widths the section's swell can give it (half `riffle_widening` or
// `step_widening` either way) —
// and its bed, its flood bed and its water stand that much lower too (a fan is left: it opens
// into a body whose level stands over its banks, and the measure skips it as it does). The
// seat is rounded up to the millimetre and taken as the most over a point and two neighbours
// either way (a metre of line at the default step), since the cut joins two points' tops by a
// straight line while a bend's outer bank top need not be one, and the heading a bank is read
// across turns with the line. The width is the hydraulic geometry's and is kept: a narrower channel
// would carry its area only deeper. Never a fill: no levee is built, because a levee is the
// overbank deposit of a channel on its floodplain and a channel cut into a hillside has none
// (Brierley et al. 1997).
void seat_section(const ChannelLayer& p, const scene_gen::Ground& ground, ChannelLink& link) {
  const u32 m = link.line.size();
  link.seat.assign(m, 0.0f);
  if (m < 2) return;
  const Vector<f64> s = lengths_of(link.line);
  Vector<f64> seat(m, 0.0);
  for (u32 i = 0; i < m; ++i) {
    const ChannelPoint& pt = link.line[i];
    const f64 w = wide(pt.width_m);
    if (!(w > 0)) continue;
    const ChannelReach reach =
        i < link.reach.size() ? static_cast<ChannelReach>(link.reach[i]) : ChannelReach::PoolRiffle;
    const bool steep = reach == ChannelReach::StepPool || reach == ChannelReach::Cascade ||
                       reach == ChannelReach::Bedrock || reach == ChannelReach::Colluvial;
    // A fan is left as it is: it opens into a water body whose own level stands over its banks,
    // and its bed is graded to that level.
    if (fan_share(p, link, s, i) > 0) continue;
    // The section's half width is not known yet (its swell is planned after the bed), so the
    // bank's ground is read at nine places over the range the swell can give it, and the lowest
    // kept: a bank top past a valley's floor stands higher the further out it is.
    // Across two headings, the lowest kept: the line's over a width (the cut's section turns with
    // it) and its own between the neighbouring points (where a kink turns it within a width).
    const f64 swell = .5 * wide(steep ? p.step_widening : p.riffle_widening);
    Heading local;
    {
      const u32 a = i > 0 ? i - 1 : 0, b = std::min(i + 1, m - 1);
      const f64 dx = link.line[b].at.x - link.line[a].at.x;
      const f64 dz = link.line[b].at.z - link.line[a].at.z;
      const f64 l = std::sqrt(dx * dx + dz * dz);
      if (!(l > 0)) continue;
      local = {dx / l, dz / l, true};
    }
    Heading along = heading_at(link, s, s[i], w);
    if (!along.ok) along = local;
    f64 lower = std::numeric_limits<f64>::infinity();
    for (const Heading& h : {along, local}) {
      for (u32 k = 0; k <= 8; ++k) {
        const f64 half = .5 * w * (1 + swell * (static_cast<f64>(k) / 4 - 1));
        // **A bank is read only where it is one** (W122; channels.md, "The seat, reshaped"):
        // ground that stands further under the line than it lies from it, past a 45-degree fall
        // across, is a face's foot or the ground below a cliff's edge, and seats nothing.
        for (const f64 side : {1.0, -1.0}) {
          const f64 g = ground_at(ground, pt.at.x - side * h.z * half, pt.at.z + side * h.x * half);
          if (pt.at.y - g <= half) lower = std::min(lower, g);
        }
      }
    }
    if (!(lower < std::numeric_limits<f64>::infinity())) lower = pt.at.y;
    seat[i] = std::max(0.0, std::ceil((pt.at.y - lower) * 1e3) / 1e3);
  }
  // **No bank is read across a step** (W122; S104's finding, `seat-eats-step`): at a knickpoint
  // the ground falls along the line, and a line that meets the step at an angle has one bank on
  // the step's top and the other at its foot. Read there, the seat was the step's height: it cut
  // the lip down a step's height a metre above the cliff and dug a pit as deep at the foot (12.9
  // m at a 14 m cliff), which the bed then climbed out of. A knickpoint's points, and those
  // within the read's own reach and the dilation's two points of one along the line, take no
  // seat: the water falls there, and what bounds it is the fall (`falls.h`), not a bank.
  Vector<u8> at_step(m, 0);
  if (link.knick.size() == m) {
    const f64 step = wide(p.step_m);
    for (u32 i = 0; i < m; ++i) {
      if (!link.knick[i]) continue;
      const f64 w = wide(link.line[i].width_m);
      const f64 clear = std::max(1.0, .5 * w * (1 + .5 * wide(p.step_widening))) + 2 * step;
      for (u32 j = i;; --j) {
        if (s[i] - s[j] > clear) break;
        at_step[j] = 1;
        if (j == 0) break;
      }
      for (u32 j = i + 1; j < m && s[j] - s[i] <= clear; ++j)
        at_step[j] = 1;
    }
  }
  for (u32 i = 0; i < m; ++i) {
    f64 most = 0;
    for (u32 j = i > 2 ? i - 2 : 0; j < std::min(i + 3, m); ++j)
      if (!at_step[j]) most = std::max(most, seat[j]);
    link.seat[i] = at_step[i] ? 0.0f : static_cast<f32>(most);
  }
  // **And a seat never reaches under the lip below it.** The bed does not rise (`cut_bed`), so
  // what a seat lowers it by is carried down the line until the ground has fallen that far; a
  // seat a few metres above a knickpoint would notch the fall's lip by itself. Above each
  // knickpoint's first point a seat is at most the ground's own fall from its point to that one.
  if (link.knick.size() == m) {
    f32 largest = 0;
    for (const f32 v : link.seat)
      largest = std::max(largest, v);
    for (u32 lip = 1; lip < m; ++lip) {
      if (!link.knick[lip] || link.knick[lip - 1]) continue;
      for (u32 j = lip; j-- > 0;) {
        const f64 fall = link.line[j].at.y - link.line[lip].at.y;
        if (fall > wide(largest)) break;
        link.seat[j] = std::min(link.seat[j], static_cast<f32>(std::max(0.0, fall)));
      }
    }
  }
}

void plan_section(const ChannelLayer& p, ChannelLink& link) {
  const u32 m = link.line.size();
  link.asym.assign(m, 0.0f);
  link.swell.assign(m, 0.0f);
  link.flood_left.assign(m, 0.0f);
  link.flood_right.assign(m, 0.0f);
  link.entrenchment.assign(m, 0.0f);
  if (m < 3) return;
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
  u32 from = 0, to = 0, crest = 0;
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
        1 - smoothstep(.2, .5, to > from ? sum / static_cast<f64>(to - from) : 0.0);
    const ChannelReach reach =
        i < link.reach.size() ? static_cast<ChannelReach>(link.reach[i]) : ChannelReach::PoolRiffle;
    f64 share = 0, alternate = 0;
    if (reach == ChannelReach::DuneRipple || reach == ChannelReach::PoolRiffle) {
      share = 1;
      alternate = wide(p.alternate_asymmetry) * straight;
    } else if (reach == ChannelReach::PlaneBed) {
      share = .5;
    }
    const bool steep = reach == ChannelReach::StepPool || reach == ChannelReach::Cascade ||
                       reach == ChannelReach::Bedrock || reach == ChannelReach::Colluvial;
    const bool held = i >= keep || (i < link.knick.size() && link.knick[i]);
    if (held || w < wide(p.bar_min_width_m)) share = 0;
    const f64 ends = smoothstep(0, 2 * w, s[i]) * smoothstep(0, 2 * w, s[m - 1] - s[i]) *
                     (1 - fan_share(p, link, s, i));
    share *= ends;
    // **The pool lies downstream of the bend's apex** (channels.md, "The section"): the section
    // at a point is the bend's `k_pool_lag_widths` widths upstream of it.
    const u32 lagged = static_cast<u32>(
        std::lower_bound(s.begin(), s.begin() + i, s[i] - k_pool_lag_widths * w) - s.begin());
    const f64 b = bend[lagged];
    const f64 v = b + alternate * (1 - std::fabs(b)) * det::sin(theta);
    asym[i] = std::clamp(v, -1.0, 1.0) * share;
    swell[i] = .5 * wide(p.riffle_widening) * (1 - 2 * std::min(1.0, std::fabs(v))) * share;
    // **The steep reaches** (channels.md, "The steep reaches"): no bars and no bends' pools, and
    // not one section either. The thalweg wanders between the banks — an irregular asymmetry, a
    // knot of its noise every two widths — and the width follows the steps: narrowest at a step's
    // crest and widest over its pool on a step-pool reach, irregular (a knot every width and a
    // half) where the reach has no steps. A knickpoint, a tributary's last points and a link's two
    // ends take none, as with the bars.
    if (steep && !held && w >= wide(p.bar_min_width_m)) {
      const f64 scale = std::max(w, 1.0);
      asym[i] = wide(p.steep_asymmetry) *
                value_noise(hash_combine(link.seed, 0xa5e1ull), s[i] / (2 * scale)) * ends;
      f64 phase = value_noise(hash_combine(link.seed, 0x57e9ull), s[i] / (1.5 * scale));
      if (reach == ChannelReach::StepPool && !link.step_at.empty()) {
        while (crest + 1 < link.step_at.size() && link.step_at[crest + 1] <= i)
          ++crest;
        const u32 a = link.step_at[crest];
        if (a <= i && crest + 1 < link.step_at.size()) {
          const f64 run = s[link.step_at[crest + 1]] - s[a];
          if (run > 0) phase = -det::cos(2 * k_pi * (s[i] - s[a]) / run);
        }
      }
      swell[i] = .5 * wide(p.step_widening) * phase * ends;
    }
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
    link.swell[i] = static_cast<f32>(round_to(swell[i], 1e4));
  }
}

// **How much wider a breach's top is** (W122; channels.md, "Hollows"), after the bed and the
// section are final. Where the bed is held more than a thalweg under its section's own depth the
// banks would keep their slopes by reaching the top `held / slope` further out each side; the
// profile is one width about the line, so both sides move out by the same share of that, the
// largest of 1, 3/4, 1/2 and 1/4 at which the ground at four places between the section's bank
// top and the widened one, each side, still stands at or over the top. A top
// carried out over ground that falls away would stand in the air there (the cut only lowers), so
// on a hillside, or on a bench narrower than the breach wants, the width stays and the stamp
// steepens the banks.
void plan_widening(const ChannelLayer& p, const scene_gen::Ground& ground, ChannelLink& link) {
  const u32 m = link.line.size();
  link.widen.assign(m, 0.0f);
  if (m < 3 || link.bed.size() != m) return;
  for (u32 i = 1; i + 1 < m; ++i) {
    const ChannelPoint& pt = link.line[i];
    const f64 top = round_to(top_of(link, i), 1e3);
    const f64 thalweg = section_of(wide(pt.width_m), wide(pt.depth_m), wide(p.bank_slope)).thalweg;
    const f64 held = round_to(top - link.bed[i], 1e3) - 2 * thalweg -
                     (i < link.step_depth.size() ? wide(link.step_depth[i]) : 0.0);
    if (!(held > 0)) continue;
    const BendSection section = bend_section(p, wide(pt.width_m), wide(pt.depth_m),
                                             i < link.asym.size() ? wide(link.asym[i]) : 0.0,
                                             i < link.swell.size() ? wide(link.swell[i]) : 0.0);
    const f64 dx = link.line[i + 1].at.x - link.line[i - 1].at.x;
    const f64 dz = link.line[i + 1].at.z - link.line[i - 1].at.z;
    const f64 l = std::sqrt(dx * dx + dz * dz);
    if (!(l > 0) || !(section.width > 0)) continue;
    const f64 nx = -dz / l, nz = dx / l;
    const f64 wider = held * (1 / section.slope_left + 1 / section.slope_right);
    f64 share = 0;
    for (const f64 tried : {1.0, .75, .5, .25}) {
      bool holds = true;
      for (u32 k = 1; k <= 4 && holds; ++k) {
        const f64 d = .5 * section.width + .5 * tried * wider * static_cast<f64>(k) / 4;
        holds = ground_at(ground, pt.at.x + nx * d, pt.at.z + nz * d) >= top &&
                ground_at(ground, pt.at.x - nx * d, pt.at.z - nz * d) >= top;
      }
      if (holds) {
        share = tried;
        break;
      }
    }
    link.widen[i] = static_cast<f32>(round_to(share * wider, 1e3));
  }
  link.widen[0] = link.widen[1];
  link.widen[m - 1] = link.widen[m - 2];
  // A top is as narrow as the narrowest reading near it: the cut joins two points' widths by a
  // straight line and a neighbour's section reaches over a point's own cross-line where the
  // line turns, so each point takes the least over itself and two neighbours either way.
  const Vector<f32> read = link.widen;
  for (u32 i = 0; i < m; ++i)
    for (u32 j = i > 2 ? i - 2 : 0; j < std::min(i + 3, m); ++j)
      link.widen[i] = std::min(link.widen[i], read[j]);
}

// **The flood bed** (W93; channels.md, "The flood bed"): the valley's confinement read from the
// ground, and the bed it leaves room for. At a station (one a bankfull width along the line, on a
// pool-riffle, dune-ripple or plane-bed reach at least `flood_min_width_m` wide, away from a
// knickpoint, a fan and a tributary's last points) the ground is walked across the line each way,
// from the line out, in steps of a sixth of the width (0.5 m at least), to where it first stands
// `flood_height_depths - 1` thalwegs over the ground at the line — Rosgen's flood-prone elevation,
// twice the greatest bankfull depth over the thalweg — the crossing found between the two steps
// that bracket it. A side whose ground falls a thalweg under the line's first is not a valley's
// floor (the line is perched on a slope): its walk ends there. The two distances over the width
// are the entrenchment ratio. Where it is at least `flood_entrenchment_min` the bed reaches, each
// side, the flood-prone distance less the scarp's run, half `flood_bed_widths` widths at the most
// — `k_flood_bend` of that more on the inside of a bend and less on its outside, where the bar
// builds and the bank is cut — and never less than the channel's own bank. Between stations the
// reaches are interpolated, and they close to the channel's width over two widths at a stretch's
// ends.
constexpr f64 k_flood_bend = .6;
void plan_flood(const ChannelLayer& p, const scene_gen::Ground& ground, ChannelLink& link) {
  const u32 m = link.line.size();
  if (m < 3 || link.flood_left.size() != m) return;
  const Vector<f64> s = lengths_of(link.line);
  const u32 keep = std::min<u32>(link.keep, m);
  struct Station {
    u32 at;
    f64 left, right, ratio;
  };
  Vector<Station> stations;
  f64 next = 0;
  for (u32 i = 0; i < keep; ++i) {
    const ChannelPoint& pt = link.line[i];
    const f64 w = wide(pt.width_m);
    if (s[i] < next) continue;
    next = s[i] + std::max(w, wide(p.step_m));
    const ChannelReach reach =
        i < link.reach.size() ? static_cast<ChannelReach>(link.reach[i]) : ChannelReach::PoolRiffle;
    const bool alluvial = reach == ChannelReach::DuneRipple || reach == ChannelReach::PoolRiffle ||
                          reach == ChannelReach::PlaneBed;
    const Heading along = heading_at(link, s, s[i], k_bend_window_widths * w);
    if (!alluvial || w < wide(p.flood_min_width_m) || !along.ok ||
        (i < link.knick.size() && link.knick[i]) || fan_share(p, link, s, i) > 0) {
      stations.push_back({i, 0.0, 0.0, 0.0});
      continue;
    }
    const f64 thalweg = section_of(w, wide(pt.depth_m), wide(p.bank_slope)).thalweg;
    const f64 rise = (wide(p.flood_height_depths) - 1) * thalweg;
    const f64 scarp = rise + wide(p.flood_bed_depth) * thalweg;
    const f64 run = scarp / wide(p.scarp_slope);
    const f64 half = .5 * wide(p.flood_bed_widths) * w;
    const f64 limit = half * (1 + k_flood_bend) + run;
    const f64 step = std::max(.5, w / 6);
    // The top is the seated one (W116): the ground at the line less the seat, so on a seated
    // point the walk begins `seat` over the top.
    const f64 top = top_of(link, i);
    const f64 under = wide(p.flood_bed_depth) * thalweg;
    // The left is the side of (-dz, dx). Each walk gives the flood-prone distance and **how far
    // the ground holds the bed's level** (W116): the first distance at which the ground stands
    // under the bed's surface, `under` below the top. Past it the bed would be a level surface
    // over ground that falls away, which the cut cannot make and which read as a plate in the air
    // on the owner's walk (mark 4), so the bed on that side reaches no further.
    struct Walked {
      f64 prone = 0, hold = 0;
    };
    const auto walk = [&](f64 nx, f64 nz) {
      Walked out;
      out.hold = limit;
      bool held = false;
      f64 before = ground_at(ground, pt.at.x, pt.at.z) - top, before_at = 0;
      for (f64 d = step;; d += step) {
        const f64 at = std::min(d, limit);
        const f64 over = ground_at(ground, pt.at.x + nx * at, pt.at.z + nz * at) - top;
        if (!held && over < -under) {
          held = true;
          out.hold = before > -under
                         ? before_at + (at - before_at) * (-under - before) / (over - before)
                         : before_at;
        }
        if (over >= rise) {
          out.prone = before_at + (at - before_at) * (rise - before) / (over - before);
          return out;
        }
        if (over <= -thalweg || at >= limit) {
          out.prone = at;
          return out;
        }
        before = over;
        before_at = at;
      }
    };
    const Walked left = walk(-along.z, along.x), right = walk(along.z, -along.x);
    const f64 prone_left = left.prone, prone_right = right.prone;
    Station st{i, 0.0, 0.0, (prone_left + prone_right) / w};
    if (st.ratio >= wide(p.flood_entrenchment_min)) {
      const f64 a = i < link.asym.size() ? wide(link.asym[i]) : 0.0;
      const f64 bank = .5 * w;
      st.left = std::max(
          bank, std::min(std::min(half * (1 + k_flood_bend * a), prone_left - run), left.hold));
      st.right = std::max(
          bank, std::min(std::min(half * (1 - k_flood_bend * a), prone_right - run), right.hold));
      // A bed no wider than the channel in it is none.
      if (st.left + st.right < 1.1 * w) st.left = st.right = 0;
    }
    stations.push_back(st);
  }
  // **A bed is as narrow as the narrowest reading near it.** The line kinks (W91's trough test),
  // and a walk across a kinked line can run along the valley and read a floor several times as
  // wide as it is; on W93's scratch scene that put the bed's scarp metres up a 17-degree valley
  // side. Each station's reach is the least over itself and its two neighbours either way.
  {
    Vector<Station> eroded = stations;
    for (u32 q = 0; q < stations.size(); ++q) {
      for (u32 o = q >= 2 ? q - 2 : 0; o < std::min<u32>(stations.size(), q + 3); ++o) {
        if (!(stations[o].left > 0)) continue;
        if (!(eroded[q].left > 0)) break;
        eroded[q].left = std::min(eroded[q].left, stations[o].left);
        eroded[q].right = std::min(eroded[q].right, stations[o].right);
      }
    }
    stations = std::move(eroded);
  }
  // Between stations, and closing over two widths where a stretch of bed begins and ends.
  for (u32 q = 0; q + 1 < stations.size(); ++q) {
    const Station& a = stations[q];
    const Station& b = stations[q + 1];
    for (u32 i = a.at; i < b.at; ++i) {
      const f64 t = (s[i] - s[a.at]) / std::max(s[b.at] - s[a.at], 1e-9);
      link.entrenchment[i] = static_cast<f32>(round_to(a.ratio + t * (b.ratio - a.ratio), 1e3));
      if (!(a.left > 0) || !(b.left > 0)) continue;
      link.flood_left[i] = static_cast<f32>(a.left + t * (b.left - a.left));
      link.flood_right[i] = static_cast<f32>(a.right + t * (b.right - a.right));
    }
  }
  for (u32 i = 0; i < m;) {
    if (!(link.flood_left[i] > 0)) {
      ++i;
      continue;
    }
    u32 e = i;
    while (e + 1 < m && link.flood_left[e + 1] > 0)
      ++e;
    for (u32 q = i; q <= e; ++q) {
      const f64 w = wide(link.line[q].width_m);
      const f64 ease = smoothstep(0, 2 * w, s[q] - s[i]) * smoothstep(0, 2 * w, s[e] - s[q]);
      const f64 bank = .5 * w;
      link.flood_left[q] = static_cast<f32>(
          round_to(bank + ease * std::max(0.0, wide(link.flood_left[q]) - bank), 1e3));
      link.flood_right[q] = static_cast<f32>(
          round_to(bank + ease * std::max(0.0, wide(link.flood_right[q]) - bank), 1e3));
    }
    i = e + 1;
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
  if (!in(wide(p.pool_riffle_widths), 2, 20) || !in(wide(p.step_pool_widths), 1, 10))
    return fail(error, "pool_riffle_widths/step_pool_widths");
  if (!in(wide(p.bar_min_width_m), 0, 100)) return fail(error, "bar_min_width_m");
  if (!in(wide(p.bank_width_widths), 0, 2) || !in(wide(p.bank_width_m), 0, 10))
    return fail(error, "bank_width_widths/bank_width_m");
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
  if (!in(wide(p.bend_radius_widths), 1, 20) || !in(wide(p.cut_bank_slope), .25, 8) ||
      !in(wide(p.alternate_asymmetry), 0, 1) || !in(wide(p.riffle_widening), 0, .5) ||
      !in(wide(p.bank_round), .1, 1))
    return fail(error,
                "bend_radius_widths/cut_bank_slope/alternate_asymmetry/riffle_widening/bank_round");
  if (!in(wide(p.step_widening), 0, .6) || !in(wide(p.steep_asymmetry), 0, 1) ||
      !in(wide(p.steep_wander_widths), 0, 3))
    return fail(error, "step_widening/steep_asymmetry/steep_wander_widths");
  if (!in(wide(p.bed_feather_m), 0, 10)) return fail(error, "bed_feather_m");
  if (!in(wide(p.flood_bed_widths), 1.25, 12) || !in(wide(p.flood_height_depths), 1.25, 4) ||
      !in(wide(p.flood_entrenchment_min), 1, 5) || !in(wide(p.flood_min_width_m), 0, 100) ||
      !in(wide(p.flood_bed_depth), 0, .45) || !in(wide(p.scarp_slope), .25, 8))
    return fail(error,
                "flood_bed_widths/flood_height_depths/flood_entrenchment_min/"
                "flood_min_width_m/flood_bed_depth/scarp_slope");
  if (!in(wide(p.step_m), .1, 4)) return fail(error, "step_m");
  if (!in(wide(p.clast_min_mm), 16, 1024)) return fail(error, "clast_min_mm");
  if (!in(wide(p.rounding_m), 10, 100000)) return fail(error, "rounding_m");
  if (!in(wide(p.breach_max_m), 0, 50)) return fail(error, "breach_max_m");
  if (!in(wide(p.basin_min_area_m2), 0, 1e6)) return fail(error, "basin_min_area_m2");
  if (!in(wide(p.basin_notch_m), .01, 5)) return fail(error, "basin_notch_m");
  if (!in(wide(p.basin_evaporation_mm_y), 0, 5000) || !in(wide(p.basin_seepage_mm_y), 0, 1e6))
    return fail(error, "basin_evaporation_mm_y/basin_seepage_mm_y");
  for (const ChannelBasin& b : p.basins) {
    for (u32 a = 0; a < 2; ++a)
      if (!in(b.at[a], p.region_min[a], p.region_max[a]))
        return fail(error, "basins (`at` inside the region): " + b.name);
    if (!in(wide(b.leak_m3s), 0, 1e4)) return fail(error, "basins (leak_m3s): " + b.name);
    if (!b.dry && !b.infilled && !(b.leak_m3s > 0))
      return fail(error,
                  "basins (an entry says `dry` or `infilled`, or names a `leak_m3s`): " + b.name);
    if (b.infilled && (b.dry || b.leak_m3s > 0))
      return fail(
          error, "basins (`infilled` is ground: it is not `dry` and has no `leak_m3s`): " + b.name);
  }
  if (!in(wide(p.basin_wind_m_s), 0, 40)) return fail(error, "basin_wind_m_s");
  return surface_in_range(p.boulders, error);
}

namespace {
std::atomic<u64> g_plans_made{0};

// ---- The fill, the routing and the hollows (W122; channels.md, "Hollows") ----

bool on_edge(const ChannelNetwork& n, u32 i) {
  const u32 c = i % n.width, r = i / n.width;
  return c == 0 || r == 0 || c + 1 == n.width || r + 1 == n.depth;
}
// Pits filled by priority flood with a one-micrometre epsilon (Barnes, Lehman & Mulla 2014) from
// the region's edge **and from every sink** (their ocean cells; Barnes, Callaghan & Wickert 2020,
// section 3.1), so every other cell has a strictly lower neighbour and drains to one of them; a
// flat drains along the way the flood came.
void flood(ChannelNetwork& n, const Vector<u8>& sink) {
  const u32 cells = n.width * n.depth;
  n.filled_um.assign(cells, 0);
  Vector<u8> seen(cells, 0);
  Vector<Node> heap;
  auto push = [&](u32 i, i64 v) {
    n.filled_um[i] = v;
    seen[i] = 1;
    heap.push_back({v, i});
    std::push_heap(heap.begin(), heap.end(), after);
  };
  for (u32 i = 0; i < cells; ++i)
    if (sink[i] || on_edge(n, i)) push(i, n.ground_um[i]);
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
}
// D8 (O'Callaghan & Mark 1984): the steepest drop over the filled heights, a diagonal's run
// sqrt(2) spacings; ties go to the first direction. The region's edge and the sinks are the
// outlets. Then the contributing area: every cell after all of its donors, which the filled
// heights order.
void route(ChannelNetwork& n, const Vector<u8>& sink, Vector<u32>& order) {
  const u32 cells = n.width * n.depth;
  n.receiver.assign(cells, ~0u);
  for (u32 i = 0; i < cells; ++i) {
    if (sink[i] || on_edge(n, i)) continue;
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
  order.resize(cells);
  for (u32 i = 0; i < cells; ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
    return n.filled_um[a] != n.filled_um[b] ? n.filled_um[a] > n.filled_um[b] : a < b;
  });
  n.area_cells.assign(cells, 1);
  for (u32 i : order)
    if (n.receiver[i] != ~0u) n.area_cells[n.receiver[i]] += n.area_cells[i];
}

// A pit as the fill left it: the raised cells connected through their eight neighbours. The
// flood reached it first at `entry`, from `spill`, whose ground is the pit's level: no lower
// neighbour of `entry` exists, or the flood would have come from it.
struct Pit {
  u32 entry = 0;
  u32 spill = 0;
  u32 floor = 0;      // the lowest cell, by (ground, index)
  u32 under = 0;      // cells whose ground stands under the level
  u32 covered = ~0u;  // the lowest cell a scene's body covers, or none
  i64 level_um = 0;
  f64 volume_um = 0;  // the level less the ground, summed over `under`
  u32 c0 = 0, c1 = 0, r0 = 0, r1 = 0;
  u32 hollow = ~0u;       // its record in `ChannelHollows::hollows`, once it has one
  u32 record = ~0u;       // an authored terminal's record, in the pass that made it one
  u8 decided = 0;         // made a sink in this pass
  u8 terminal = 0;        // 1: its covered cells are sinks; 2: its balance's cells are
  u32 wet = 0;            // cells whose ground stands under the level less the notch
  f64 wet_volume_um = 0;  // the level less the notch less the ground, summed over `wet`
  f64 gain = 0;           // the named sources that drain into it, m3/s
  f64 leak = 0;           // what its author says its floor passes underground, m3/s
};
void label(const ChannelNetwork& n, const Vector<u8>& covered, i64 notch_um, Vector<u32>& pit_of,
           Vector<Pit>& pits) {
  const u32 cells = n.width * n.depth;
  pit_of.assign(cells, ~0u);
  pits.clear();
  Vector<u32> stack;
  const auto raised = [&](u32 i) { return n.filled_um[i] > n.ground_um[i]; };
  const auto lower = [&](const Vector<i64>& h, u32 a, u32 b) {
    return h[a] != h[b] ? h[a] < h[b] : a < b;
  };
  for (u32 i = 0; i < cells; ++i) {
    if (pit_of[i] != ~0u || !raised(i)) continue;
    const u32 id = pits.size();
    Pit pit;
    pit.entry = pit.floor = i;
    pit.c0 = pit.c1 = i % n.width;
    pit.r0 = pit.r1 = i / n.width;
    pit_of[i] = id;
    stack.push_back(i);
    while (!stack.empty()) {
      const u32 c = stack.back();
      stack.pop_back();
      if (lower(n.filled_um, c, pit.entry)) pit.entry = c;
      if (lower(n.ground_um, c, pit.floor)) pit.floor = c;
      if (!covered.empty() && covered[c] &&
          (pit.covered == ~0u || lower(n.ground_um, c, pit.covered)))
        pit.covered = c;
      pit.c0 = std::min(pit.c0, c % n.width);
      pit.c1 = std::max(pit.c1, c % n.width);
      pit.r0 = std::min(pit.r0, c / n.width);
      pit.r1 = std::max(pit.r1, c / n.width);
      for (u32 d = 0; d < 8; ++d) {
        const u32 j = neighbour(c, n.width, n.depth, k_dx[d], k_dz[d]);
        if (j == ~0u || pit_of[j] != ~0u || !raised(j)) continue;
        pit_of[j] = id;
        stack.push_back(j);
      }
    }
    pit.spill = pit.entry;
    bool found = false;
    for (u32 d = 0; d < 8; ++d) {
      const u32 j = neighbour(pit.entry, n.width, n.depth, k_dx[d], k_dz[d]);
      if (j == ~0u || raised(j)) continue;
      if (!found || lower(n.filled_um, j, pit.spill)) pit.spill = j;
      found = true;
    }
    pit.level_um = found ? n.ground_um[pit.spill] : n.filled_um[pit.entry];
    pits.push_back(pit);
  }
  for (u32 i = 0; i < cells; ++i) {
    if (pit_of[i] == ~0u) continue;
    Pit& pit = pits[pit_of[i]];
    const i64 depth = pit.level_um - n.ground_um[i];
    if (depth <= 0) continue;
    ++pit.under;
    pit.volume_um += static_cast<f64>(depth);
    if (depth > notch_um) {
      ++pit.wet;
      pit.wet_volume_um += static_cast<f64>(depth - notch_um);
    }
  }
}
f64 cell_x(const ChannelNetwork& n, u32 i) {
  return static_cast<f64>(n.x_mm + static_cast<i64>(i % n.width) * n.spacing_mm) / 1000.0;
}
f64 cell_z(const ChannelNetwork& n, u32 i) {
  return static_cast<f64>(n.z_mm + static_cast<i64>(i / n.width) * n.spacing_mm) / 1000.0;
}
Hollow hollow_of(const ChannelNetwork& n, const Pit& pit, HollowKind kind) {
  const f64 spacing = static_cast<f64>(n.spacing_mm) / 1000.0;
  Hollow h;
  h.kind = kind;
  h.spill_m = round_to(static_cast<f64>(pit.level_um) / 1e6, 1e3);
  h.level_m = h.spill_m;
  h.floor_m = round_to(static_cast<f64>(n.ground_um[pit.floor]) / 1e6, 1e3);
  h.floor_x = cell_x(n, pit.floor);
  h.floor_z = cell_z(n, pit.floor);
  h.spill_x = cell_x(n, pit.spill);
  h.spill_z = cell_z(n, pit.spill);
  h.area_m2 = static_cast<f64>(pit.under) * spacing * spacing;
  h.volume_m3 = round_to(pit.volume_um / 1e6 * spacing * spacing, 1e3);
  h.catchment_m2 = static_cast<f64>(n.area_cells[pit.spill]) * spacing * spacing;
  const u32 fc = pit.floor % n.width, fr = pit.floor / n.width;
  h.extent_m = spacing * static_cast<f64>(std::max(std::max(fc - pit.c0, pit.c1 - fc),
                                                   std::max(fr - pit.r0, pit.r1 - fr)));
  return h;
}
// A ground standing flat at the height its context holds.
f64 level_plane(const void* context, i64, i64) noexcept {
  return *static_cast<const f64*>(context);
}
// The level of the scene's body that covers a point, found by asking the exclusion's water kind
// about a flat ground raised between the real ground there (covered) and `over` (not): 48
// halvings. The exclusion has no question for a body's level on this branch; W121's
// `water_level_mm` is one, and takes this function's place when the two meet.
bool body_level_at(const scene_gen::Exclusion& water, i64 x_mm, i64 z_mm, f64 under, f64 over,
                   f64& level) {
  f64 height = 0;
  scene_gen::Ground flat;
  flat.context = &height;
  flat.surface_fn = &level_plane;
  const auto covered = [&](f64 h) {
    height = h;
    return water.excluded_mm(flat, x_mm, z_mm, scene_gen::k_exclude_water);
  };
  if (!covered(under) || covered(over)) return false;
  f64 lo = under, hi = over;
  for (u32 k = 0; k < 48; ++k) {
    const f64 mid = .5 * (lo + hi);
    if (covered(mid))
      lo = mid;
    else
      hi = mid;
  }
  level = round_to(hi, 1e3);
  return true;
}

constexpr u32 k_hollow_passes = 16;
// A pit shallower than this is the fill's epsilon over a flat, not a hollow.
constexpr i64 k_hollow_min_um = 1000;

// **What becomes of every pit** (channels.md, "Hollows"). The fill and the routing, then each
// pit by its depth, what stands in it and what the author named; a pit made a sink changes the
// fill, so the whole is taken again until nothing changes. Leaves `n` routed, `n.basins` filled
// and `n.body_of` marking the plan's bodies.
bool route_hollows(const ChannelLayer& p, const scene_gen::Ground& ground,
                   const scene_gen::Exclusion* water, ChannelNetwork& n, Vector<u32>& order,
                   std::string& error) {
  (void)ground;  // every height asked is one the lattice already holds
  const u32 cells = n.width * n.depth;
  const f64 spacing = static_cast<f64>(n.spacing_mm) / 1000.0;
  const f64 cell_area = spacing * spacing;
  const i64 breach_um = std::llround(wide(p.breach_max_m) * 1e6);
  const i64 notch_um = std::llround(wide(p.basin_notch_m) * 1e6);
  // The balance's rates, metres of water a second: the runoff a square metre of catchment sends
  // (1 l/s/km2 is 1e-9 m/s) and what a square metre of standing water loses (a Julian year).
  const f64 runoff = wide(p.flow.runoff_l_s_km2) * 1e-9;
  const f64 rate =
      (wide(p.basin_evaporation_mm_y) + wide(p.basin_seepage_mm_y)) / 1000.0 / 31'557'600.0;
  bool losses = rate > 0;
  for (const ChannelBasin& b : p.basins)
    losses = losses || b.leak_m3s > 0;
  // A hollow's inflow and losses with `wet` cells of it under water, m3/s.
  const auto inflow_of = [&](const Pit& pit, u32 wet) {
    const u32 catchment = n.area_cells[pit.spill];
    return runoff * static_cast<f64>(catchment > wet ? catchment - wet : 0u) * cell_area + pit.gain;
  };
  const auto loss_of = [&](const Pit& pit, u32 wet) {
    return rate * static_cast<f64>(wet) * cell_area + pit.leak;
  };
  n.basins = {};
  n.body_of.assign(cells, 0);
  Vector<u8> sink(cells, 0), covered;
  // The scene's bodies: which cells one covers, and an open body's sea, whose every cell is where
  // water ends (Barnes et al. 2020 seed the ocean first).
  if (water != nullptr && water->body_count() > 0) {
    covered.assign(cells, 0);
    // Asked of the lattice's own heights, which are the ground's at these points.
    scene_gen::Ground real;
    real.context = &n;
    real.surface_fn = +[](const void* context, i64 x_mm, i64 z_mm) noexcept -> f64 {
      const ChannelNetwork& net = *static_cast<const ChannelNetwork*>(context);
      const i64 c = (x_mm - net.x_mm) / net.spacing_mm, r = (z_mm - net.z_mm) / net.spacing_mm;
      return static_cast<f64>(
                 net.ground_um[static_cast<u32>(r) * net.width + static_cast<u32>(c)]) /
             1e6;
    };
    for (u32 i = 0; i < cells; ++i)
      covered[i] = water->excluded_mm(real, n.x_mm + static_cast<i64>(i % n.width) * n.spacing_mm,
                                      n.z_mm + static_cast<i64>(i / n.width) * n.spacing_mm,
                                      scene_gen::k_exclude_water)
                       ? 1
                       : 0;
    // An open body covers ground under its level anywhere: asked far outside the region.
    f64 sea = 0;
    const i64 outside_mm = n.x_mm - 4'000'000'000ll;
    if (body_level_at(*water, outside_mm, outside_mm, -1e6, 1e6, sea)) {
      const i64 sea_um = std::llround(sea * 1e6);
      for (u32 i = 0; i < cells; ++i)
        if (covered[i] && n.ground_um[i] < sea_um) sink[i] = 1;
    }
  }
  // The author's dry basins, by the cell nearest each.
  Vector<u32> dry_at;
  for (const ChannelBasin& b : p.basins) {
    const i64 c =
        floor_div(scene_gen::nearest_mm(b.at[0]) - n.x_mm + n.spacing_mm / 2, n.spacing_mm);
    const i64 r =
        floor_div(scene_gen::nearest_mm(b.at[1]) - n.z_mm + n.spacing_mm / 2, n.spacing_mm);
    const i64 last_r = static_cast<i64>(n.depth) - 1, last_c = static_cast<i64>(n.width) - 1;
    dry_at.push_back(static_cast<u32>(std::clamp<i64>(r, 0, last_r)) * n.width +
                     static_cast<u32>(std::clamp<i64>(c, 0, last_c)));
  }
  Vector<u32> pit_of;
  Vector<Pit> pits;
  Vector<u8> floored(p.basins.size(), u8{0});  // an `infilled` entry whose hollow is ground now
  const auto level_of = [&](const Pit& pit, f64& level) {
    const u32 c = pit.covered;
    return body_level_at(*water, n.x_mm + static_cast<i64>(c % n.width) * n.spacing_mm,
                         n.z_mm + static_cast<i64>(c / n.width) * n.spacing_mm,
                         static_cast<f64>(n.ground_um[c]) / 1e6, 1e6, level);
  };
  for (u32 pass = 0;; ++pass) {
    flood(n, sink);
    route(n, sink, order);
    label(n, covered, notch_um, pit_of, pits);
    n.basins.passes = pass + 1;
    bool changed = false;
    // What the author gave: a source that stands in a hollow's catchment feeds every hollow its
    // water passes through on the way down; a leak is its own hollow's.
    if (losses)
      for (const ChannelSource& source : p.flow.sources) {
        const i64 c =
            floor_div(scene_gen::nearest_mm(source.at.x) - n.x_mm + n.spacing_mm / 2, n.spacing_mm);
        const i64 r =
            floor_div(scene_gen::nearest_mm(source.at.z) - n.z_mm + n.spacing_mm / 2, n.spacing_mm);
        if (c < 0 || r < 0 || c >= static_cast<i64>(n.width) || r >= static_cast<i64>(n.depth))
          continue;
        const f64 flow = wide(source.flow_m3s) *
                         (source.kind == ChannelSourceKind::Melt ? wide(source.season) : 1.0);
        u32 last = ~0u;
        for (u32 at = static_cast<u32>(r) * n.width + static_cast<u32>(c); at != ~0u;
             at = n.receiver[at]) {
          if (pit_of[at] == ~0u || pit_of[at] == last) continue;
          last = pit_of[at];
          pits[last].gain += flow;
        }
      }
    for (u32 k = 0; k < p.basins.size(); ++k) {
      const u32 id = pit_of[dry_at[k]];
      if (id == ~0u || pits[id].level_um - n.ground_um[pits[id].floor] < k_hollow_min_um) {
        if (pass == 0)
          return fail(error, "basins (no hollow of the ground holds `at`): " + p.basins[k].name);
        continue;
      }
      Pit& pit = pits[id];
      if (p.basins[k].infilled) {
        // **A lake that silted up is ground** (channels.md, "A dried lake's floor"): the hollow's
        // cells are raised to its spill on the lattice, its floor kept as a grid for the edit
        // that raises the drawn ground the same way, and the fill is taken again over what is
        // now a flat. Once: the flat's own epsilon is no hollow of this entry's.
        if (floored[k]) continue;
        floored[k] = 1;
        if (pit.decided) continue;
        changed = true;
        pit.decided = 1;
        Hollow h = hollow_of(n, pit, HollowKind::Infilled);
        h.name = p.basins[k].name;
        const u32 c0 = pit.c0 >= 2 ? pit.c0 - 2 : 0u, r0 = pit.r0 >= 2 ? pit.r0 - 2 : 0u;
        const u32 c1 = std::min(pit.c1 + 2, n.width - 1), r1 = std::min(pit.r1 + 2, n.depth - 1);
        h.fill_x_mm = n.x_mm + static_cast<i64>(c0) * n.spacing_mm;
        h.fill_z_mm = n.z_mm + static_cast<i64>(r0) * n.spacing_mm;
        h.fill_nx = c1 - c0 + 1;
        h.fill_nz = r1 - r0 + 1;
        // (A ground edit's grid is at most 1,024 points a side: tileset.md.)
        if (h.fill_nx > 1024 || h.fill_nz > 1024)
          return fail(error, "basins (an infilled hollow over 1,024 lattice points a side): " +
                                 p.basins[k].name);
        h.fill_mm.assign(h.fill_nx * h.fill_nz, k_fill_none_mm);
        const auto in_pit = [&](u32 c, u32 r) { return pit_of[r * n.width + c] == id; };
        for (u32 r = r0; r <= r1; ++r)
          for (u32 c = c0; c <= c1; ++c) {
            i32& out = h.fill_mm[(r - r0) * h.fill_nx + (c - c0)];
            if (in_pit(c, r)) {
              out = 0;
              continue;
            }
            // A cell of the rim, a neighbour of the hollow's, whose ground stands at the level
            // or over it: the floor's level too, so the floor runs flat to its walls (no slot
            // between a deep cell and the wall beside it) and nothing is raised over the level.
            bool rim = false;
            for (u32 d = 0; d < 8 && !rim; ++d) {
              const u32 j = neighbour(r * n.width + c, n.width, n.depth, k_dx[d], k_dz[d]);
              rim = j != ~0u && pit_of[j] == id;
            }
            if (rim) out = 0;
          }
        n.basins.hollows.push_back(std::move(h));
        for (u32 i = 0; i < cells; ++i)
          if (pit_of[i] == id) n.ground_um[i] = std::max(n.ground_um[i], pit.level_um);
        continue;
      }
      pit.leak += wide(p.basins[k].leak_m3s);
      if (!p.basins[k].dry || pit.decided) continue;
      pit.decided = 1;
      sink[pit.floor] = 1;
      changed = true;
      if (pass == 0) {
        Hollow h = hollow_of(n, pit, HollowKind::Dry);
        h.inflow_m3s = inflow_of(pit, 0);
        h.loss_m3s = loss_of(pit, 0);
        h.name = p.basins[k].name;
        n.basins.hollows.push_back(std::move(h));
      }
    }
    for (Pit& pit : pits) {
      if (pit.decided || pit.level_um - n.ground_um[pit.floor] <= breach_um) continue;
      if (pit.covered != ~0u) {
        // A scene's body stands in it. Within the limit of the spill it is the author's lake
        // and keeps the outlet it had; further under, it is where the hollow's water ends.
        f64 level = 0;
        if (!level_of(pit, level)) continue;
        if (pit.level_um - std::llround(level * 1e6) <= breach_um) continue;
        pit.decided = 1;
        pit.terminal = 1;
        changed = true;
        Hollow h = hollow_of(n, pit, HollowKind::AuthoredTerminal);
        h.level_m = level;
        pit.record = n.basins.hollows.size();
        n.basins.hollows.push_back(std::move(h));
        continue;
      }
      // The balance (channels.md, "Hollows"): the hollow's inflow against what its water loses
      // at the level it would stand at with an outlet. Short of it, the hollow has no outlet.
      if (!losses || static_cast<f64>(pit.under) * cell_area < wide(p.basin_min_area_m2)) continue;
      if (inflow_of(pit, pit.wet) < loss_of(pit, pit.wet)) {
        pit.decided = 1;
        pit.terminal = 2;
        changed = true;
      }
    }
    // The sinks of this pass's terminal pits: a scene's body's covered cells; and for a hollow
    // its catchment cannot fill, its cells from the floor up until their area times the ratio
    // is the catchment's (Barnes, Callaghan & Wickert 2021 grow the same queue from the pit
    // cell until it holds the water).
    {
      Vector<std::pair<u32, u32>> balance;  // (pit, cell)
      for (u32 i = 0; i < cells; ++i) {
        if (pit_of[i] == ~0u) continue;
        const Pit& pit = pits[pit_of[i]];
        if (pit.terminal == 1 && covered[i]) {
          sink[i] = 1;
          // The body's cells are the hollow's record's, as a body of the plan's are: a line that
          // ends on one ends in the body (`plan_from_lattice`), at the body's level.
          if (pit.record != ~0u) n.body_of[i] = pit.record + 1;
        }
        if (pit.terminal == 2 && n.ground_um[i] < pit.level_um) balance.push_back({pit_of[i], i});
      }
      std::sort(balance.begin(), balance.end(),
                [&](const std::pair<u32, u32>& a, const std::pair<u32, u32>& b) {
                  if (a.first != b.first) return a.first < b.first;
                  return n.ground_um[a.second] != n.ground_um[b.second]
                             ? n.ground_um[a.second] < n.ground_um[b.second]
                             : a.second < b.second;
                });
      for (u32 a = 0; a < balance.size();) {
        u32 b = a;
        while (b < balance.size() && balance[b].first == balance[a].first)
          ++b;
        const Pit& pit = pits[balance[a].first];
        // The most cells whose losses the inflow still covers: runoff (C - m) a + gain - leak >=
        // rate m a, so m <= (runoff C a + gain - leak) / ((runoff + rate) a).
        const f64 most =
            (runoff * static_cast<f64>(n.area_cells[pit.spill]) * cell_area + pit.gain - pit.leak) /
            ((runoff + rate) * cell_area);
        const u32 holds = most >= 1 ? std::min<u32>(static_cast<u32>(most), b - a - 1) : 0u;
        // The level is the ground of the first cell left dry; cells level with it stay dry too.
        const i64 level_um = n.ground_um[balance[std::min(a + holds, b - 1)].second];
        Hollow h = hollow_of(n, pit, HollowKind::Terminal);
        h.level_m = round_to(static_cast<f64>(level_um) / 1e6, 1e3);
        u32 wet = 0;
        f64 wet_volume_um = 0;
        for (u32 q = a; q < b && n.ground_um[balance[q].second] < level_um; ++q) {
          ++wet;
          wet_volume_um += static_cast<f64>(level_um - n.ground_um[balance[q].second]);
        }
        const bool stands = holds > 0 &&
                            static_cast<f64>(wet) * cell_area >= wide(p.basin_min_area_m2) &&
                            h.level_m > h.floor_m;
        if (!stands) {
          h.kind = HollowKind::Dry;  // the losses take everything, or too little is left to stand
          wet = 0;
        }
        h.inflow_m3s = inflow_of(pit, wet);
        h.loss_m3s = loss_of(pit, wet);
        if (stands) {
          h.water_m2 = static_cast<f64>(wet) * cell_area;
          h.water_m3 = round_to(wet_volume_um / 1e6 * cell_area, 1e3);
        }
        h.name = stands ? p.name + " basin " + std::to_string(n.basins.hollows.size()) : "";
        const u32 body = n.basins.hollows.size() + 1;
        sink[pit.floor] = 1;
        for (u32 q = a; stands && q < a + wet; ++q) {
          sink[balance[q].second] = 1;
          n.body_of[balance[q].second] = body;
        }
        n.basins.hollows.push_back(std::move(h));
        a = b;
      }
    }
    if (!changed) break;
    if (pass + 1 == k_hollow_passes)
      return fail(error, "hollows (the sinks did not settle in 16 fills)");
  }
  // What is left is filled to its spill: breached, the author's own lake, or a body of the plan.
  for (Pit& pit : pits) {
    const i64 depth_um = pit.level_um - n.ground_um[pit.floor];
    if (depth_um < k_hollow_min_um) continue;
    if (depth_um <= breach_um) {
      ++n.basins.breached;
      n.basins.breached_max_m =
          std::max(n.basins.breached_max_m, round_to(static_cast<f64>(depth_um) / 1e6, 1e3));
      n.basins.breached_area_m2 += static_cast<f64>(pit.under) * cell_area;
      continue;
    }
    if (pit.covered != ~0u) {
      Hollow h = hollow_of(n, pit, HollowKind::Authored);
      f64 level = 0;
      if (level_of(pit, level)) h.level_m = level;
      n.basins.hollows.push_back(std::move(h));
      continue;
    }
    const bool stands = static_cast<f64>(pit.under) * cell_area >= wide(p.basin_min_area_m2) &&
                        depth_um > notch_um + k_hollow_min_um;
    Hollow h = hollow_of(n, pit, stands ? HollowKind::Lake : HollowKind::DeepBreach);
    h.inflow_m3s = inflow_of(pit, stands ? pit.wet : 0u);
    h.loss_m3s = loss_of(pit, stands ? pit.wet : 0u);
    if (stands) {
      h.level_m = round_to(static_cast<f64>(pit.level_um - notch_um) / 1e6, 1e3);
      h.water_m2 = static_cast<f64>(pit.wet) * cell_area;
      h.water_m3 = round_to(pit.wet_volume_um / 1e6 * cell_area, 1e3);
      h.name = p.name + " basin " + std::to_string(n.basins.hollows.size());
      pit.hollow = n.basins.hollows.size();
    }
    n.basins.hollows.push_back(std::move(h));
  }
  for (u32 i = 0; i < cells; ++i) {
    if (pit_of[i] == ~0u || pits[pit_of[i]].hollow == ~0u) continue;
    const u32 hollow = pits[pit_of[i]].hollow;
    if (static_cast<f64>(n.ground_um[i]) / 1e6 < n.basins.hollows[hollow].level_m)
      n.body_of[i] = hollow + 1;
  }
  return true;
}
}  // namespace
u64 plans_made() noexcept { return g_plans_made.load(std::memory_order_relaxed); }

bool plan_channels(const ChannelLayer& p, const scene_gen::Ground& ground, u64 seed,
                   jobs::JobSystem* jobs, ChannelNetwork& out, std::string& error,
                   const scene_gen::Exclusion* water) {
  out = {};
  ChannelNetwork n;
  if (!detail::sample_lattice(p, ground, jobs, n, error)) return false;
  if (!detail::plan_from_lattice(p, ground, seed, n, error, water)) return false;
  out = std::move(n);
  return true;
}

// The lattice (channels.md, "The network", step 1): the region's dimensions and the ground at
// every point of it, which is also what a plan's key reads (channels.md, "One plan").
bool detail::sample_lattice(const ChannelLayer& p, const scene_gen::Ground& ground,
                            jobs::JobSystem* jobs, ChannelNetwork& n, std::string& error) {
  n = {};
  if (!ground.present()) return fail(error, "ground (required)");
  if (!layer_in_range(p, error)) return false;
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
  return true;
}

// Steps 2 to 6 over a sampled lattice: the fill, the routing, the heads, the links and their
// lines. The one place the planner runs, counted for `plans_made`.
bool detail::plan_from_lattice(const ChannelLayer& p, const scene_gen::Ground& uncut, u64 seed,
                               ChannelNetwork& n, std::string& error,
                               const scene_gen::Exclusion* water) {
  g_plans_made.fetch_add(1, std::memory_order_relaxed);
  const u32 cells = n.width * n.depth;
  // The fill, the routing and the hollows (W122; channels.md, "Hollows"): the fill and the
  // routing are run until no hollow changes what it is, since a hollow made a sink uncovers the
  // hollows the first fill had drowned in it.
  Vector<u32> order;
  if (!route_hollows(p, uncut, water, n, order, error)) return false;
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
    // No channel begins under a body of the plan (W122): the slope the rule reads there is the
    // lake's floor's. One that comes in from above still runs on, to be cut at the shore.
    if (n.body_of[i] == 0 && area >= wide(p.min_head_area_m2) &&
        product >= wide(p.head_threshold_m2))
      n.channel[i] = 1;
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
  // **From here the ground is the ground with the plan's floors in it** (W122; channels.md, "A
  // dried lake's floor"): an infilled hollow is ground, so every line, seat, section and flood
  // bed below is laid on its floor and not in the bowl under it. With none, the ground itself.
  FlooredGround held;
  const scene_gen::Ground floored = floored_ground(n, uncut, held);
  const scene_gen::Ground& ground = floored;
  for (ChannelLink& link : n.links)
    build_line(p, n, ground, link);
  // W90: the shorelines, the reaches and steps, the cut's bed, the confluences, in that order.
  apply_water(n, ground, water);
  // **A line that ends on a scene's body under its spill ends in it** (W122; the owner's mark 8:
  // two lines ended at such a pond with no mouth, their beds cut 0.45 and 0.75 m under its
  // level). The routing ends at the body's cells, which are a sink; the line's own last point
  // can stand a step short of the outline, where the exclusion finds no water and so no mouth.
  // The link is given the body's level as the base level W121 holds a mouth to: its bed stands
  // nowhere under it, and the solver ends the line at it.
  // Asked of the cell nearest the line's last point, not of the link's last cell: a line that
  // left another body on its way is a piece the shorelines split off, and holds no cells (on the
  // forest one of the two at mark 8 is the outlet of a pool upstream).
  for (ChannelLink& link : n.links) {
    if (link.mouth || link.line.size() < 2) continue;
    const ChannelPoint& last = link.line.back();
    const i64 c =
        floor_div(scene_gen::nearest_mm(last.at.x) - n.x_mm + n.spacing_mm / 2, n.spacing_mm);
    const i64 r =
        floor_div(scene_gen::nearest_mm(last.at.z) - n.z_mm + n.spacing_mm / 2, n.spacing_mm);
    if (c < 0 || r < 0 || c >= static_cast<i64>(n.width) || r >= static_cast<i64>(n.depth))
      continue;
    const u32 body = n.body_of[static_cast<u32>(r) * n.width + static_cast<u32>(c)];
    if (body == 0 || n.basins.hollows[body - 1].kind != HollowKind::AuthoredTerminal) continue;
    link.mouth = true;
    link.mouth_level = n.basins.hollows[body - 1].level_m;
    ++n.water.mouths;
  }
  // **Upstream first** (W122): a link's bed begins no higher than the lowest bed that ends at its
  // first cell, so the bed does not rise across a junction either. A link that enters at a cell
  // has less contributing area at its own first cell than that cell has, so the area orders them.
  {
    Vector<u32> upstream_first(n.links.size());
    for (u32 k = 0; k < n.links.size(); ++k)
      upstream_first[k] = k;
    const auto area_at = [&](u32 k) {
      const ChannelLink& l = n.links[k];
      return l.cells.empty() ? 0u : n.area_cells[l.cells.front()];
    };
    std::sort(upstream_first.begin(), upstream_first.end(), [&](u32 a, u32 b) {
      const u32 x = area_at(a), y = area_at(b);
      return x != y ? x < y : a < b;
    });
    const f64 none = std::numeric_limits<f64>::infinity();
    Vector<f64> arriving(cells, none);
    for (const u32 k : upstream_first) {
      ChannelLink& link = n.links[k];
      classify(p, link);
      seat_section(p, ground, link);  // W116: the section under its lower bank
      // Only a line that begins at its first cell meets what ends there: an outlet begins at a
      // shore.
      const bool joined = !link.cells.empty() && !link.outlet && link.line.size() >= 2;
      cut_bed(p, link, joined ? arriving[link.cells.front()] : none);
      if (!link.cells.empty() && !link.mouth && link.line.size() >= 2) {
        f64& at = arriving[link.cells.back()];
        at = std::min(at, link.bed.back());
      }
    }
  }
  for (const ChannelLink& link : n.links)
    n.length_m += link.length_m;
  confluences(p, n);
  // W122: a lip is held by every link that begins on it (`lip_sill`), after the confluences'
  // grade: a run of points from the link's first, raised to the sill where the top stands over
  // it, so the bed's line steps down past the lip and rises nowhere.
  for (ChannelLink& link : n.links) {
    for (u32 i = 0; i < link.line.size() && i < link.bed.size(); ++i) {
      f64 sill = 0;
      if (!lip_sill(n, link, i, sill)) break;
      const f64 top = round_to(top_of(link, i), 1e3);
      if (top >= sill)
        link.bed[i] = std::max(link.bed[i], std::min(sill, round_to(top - .01, 1e3)));
    }
  }
  for (ChannelLink& link : n.links) {
    link.area_end = link.cells.empty() ? 0u : n.area_cells[link.cells.back()];
    plan_section(p, link);
    plan_widening(p, ground, link);  // W122: a breach's top, where the ground holds it
    plan_flood(p, ground, link);
  }
  // W97: the bed's sediment, from the reaches, the sections and the network's lengths.
  plan_sediment(p, n);
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
    const u32 ax = x.area_end,
              ay = y.area_end;  // not the lattice: a plan read from its file has none
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
    // The flood bed's reach (W93), for the placements' exclusion: the cut's own numbers at the
    // ribbon's points, on a link that has a bed.
    bool flooded = false;
    for (u32 j = 0; j < m && !flooded; ++j)
      flooded = a + j < link.flood_left.size() && link.flood_left[a + j] > 0;
    if (flooded && cut) {
      JsonValue fl = JsonValue::array(), fr = JsonValue::array();
      for (u32 j = 0; j < m; ++j) {
        fl.push_back(JsonValue(round_to(wide(link.flood_left[a + j]), 1e3)));
        fr.push_back(JsonValue(round_to(wide(link.flood_right[a + j]), 1e3)));
      }
      o["flood_left_m"] = std::move(fl);
      o["flood_right_m"] = std::move(fr);
    }
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
      // The seated top (W116): under the ground at the line by the seat, at the lower bank's top.
      f64 top = round_to(top_of(link, i), 1e3);
      // The base level (W121; `cut_bed`): where the ground at the line stands under the level of
      // the body the link ends in (an outline that falls short of the level's contour leaves such
      // ground dry), the section's top is the level, so the cut lowers only ground over it.
      if (link.mouth) top = std::max(top, round_to(link.mouth_level, 1e3));
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
      // **A breach's top** (W122; channels.md, "Hollows"): where the bed is held more than a
      // thalweg under its section's own depth (through a lip, down a chute, to a trunk's bed),
      // the banks keep their slopes and reach the top that much further out, so the cut widens
      // with its depth. At one width the stamp steepens both banks to a V (tileset.md, "Channels
      // cut into the ground"), and a breach two metres deep in a stream a metre wide was a slot.
      // The first thalweg of hold widens nothing, as it never did: the section's own width holds
      // a bed two thalwegs down, and a top carried further out over a cross-slope would stand
      // over the downhill ground the seat was read at.
      // How much wider is the plan's (`plan_widening`), which asked the ground beside the line.
      const f64 line_thalweg =
          section_of(wide(pt.width_m), wide(pt.depth_m), wide(p.bank_slope)).thalweg;
      const f64 wider = i < link.widen.size() ? wide(link.widen[i]) : 0.0;
      widths.push_back(JsonValue(round_to(section.width * spread + wider, 1e3)));
      // The bed below the top: the thalweg, more where the cut runs through a rise, a step's pool
      // or a confluence's grade, and a bend's pool.
      // A riffle's bed stands that much over the bed's line and a pool's that much under it: the
      // section's own thalweg at its own width against the trapezoid's at the hydraulic
      // geometry's, so the area is kept as the width swells and narrows.
      f64 depth =
          std::max(1e-3, round_to(top - link.bed[i], 1e3) + (section.thalweg - line_thalweg)) +
          section.pool;
      // The base level (W121; `cut_bed`): a bend's pool or a riffle's swell takes no point of a
      // link that ends in a body under the body's level.
      if (link.mouth)
        depth = std::min(depth, std::max(1e-3, top - round_to(link.mouth_level, 1e3)));
      // An outlet's start the same, where the ground stands over the level it leaves.
      if (link.outlet && top >= round_to(link.outlet_level, 1e3))
        depth = std::min(depth, std::max(1e-3, top - round_to(link.outlet_level, 1e3)));
      // And a link that begins on a lake's lip (W122; `lip_sill`): no pool or swell under the sill.
      if (f64 sill = 0; lip_sill(n, link, i, sill) && top >= sill)
        depth = std::min(depth, std::max(1e-3, top - sill));
      depths.push_back(JsonValue(round_to(depth, 1e3)));
      left.push_back(JsonValue(round_to(section.slope_left, 1e4)));
      right.push_back(JsonValue(round_to(section.slope_right, 1e4)));
    }
    o["points"] = std::move(points);
    o["width_m"] = std::move(widths);
    o["depth_m"] = std::move(depths);
    o["bank_slope"] = JsonValue(wide(p.bank_slope));
    // Every cut the generator writes is shaped: a slope a bank a point, tops rounded, faces that
    // run out (W93; the trapezoid with a crease and a wall at its end is written no more).
    o["bank_slope_left"] = std::move(left);
    o["bank_slope_right"] = std::move(right);
    o["bank_round"] = JsonValue(wide(p.bank_round));
    // The flood bed (W93; channels.md, "The flood bed"), on a link that has any: its reach each
    // side, its surface `flood_bed_depth` of the thalweg under the top, and its scarp's top at
    // the flood-prone elevation.
    bool flooded = false;
    for (u32 i = 0; i < link.flood_left.size() && !flooded; ++i)
      flooded = link.flood_left[i] > 0;
    if (flooded) {
      JsonValue fl = JsonValue::array(), fr = JsonValue::array(), fd = JsonValue::array(),
                scarp = JsonValue::array();
      for (u32 i = 0; i < link.line.size(); ++i) {
        const ChannelPoint& pt = link.line[i];
        const f64 thalweg =
            section_of(wide(pt.width_m), wide(pt.depth_m), wide(p.bank_slope)).thalweg;
        f64 under = wide(p.flood_bed_depth) * thalweg;
        // The base level (W121; `cut_bed`): on a link that ends in a body the flood bed stands no
        // lower than the body's level either, a wide dry flat under the waterline otherwise.
        if (link.mouth)
          under = std::min(under, std::max(0.0, round_to(top_of(link, i), 1e3) -
                                                    round_to(link.mouth_level, 1e3)));
        if (link.outlet && round_to(top_of(link, i), 1e3) >= round_to(link.outlet_level, 1e3))
          under =
              std::min(under, round_to(top_of(link, i), 1e3) - round_to(link.outlet_level, 1e3));
        if (f64 sill = 0; lip_sill(n, link, i, sill) && round_to(top_of(link, i), 1e3) >= sill)
          under = std::min(under, round_to(top_of(link, i), 1e3) - sill);
        fl.push_back(JsonValue(round_to(wide(link.flood_left[i]), 1e3)));
        fr.push_back(JsonValue(round_to(wide(link.flood_right[i]), 1e3)));
        fd.push_back(JsonValue(round_to(under, 1e3)));
        scarp.push_back(JsonValue(
            round_to(std::max(1e-3, (wide(p.flood_height_depths) - 1) * thalweg + under), 1e3)));
      }
      o["flood_left_m"] = std::move(fl);
      o["flood_right_m"] = std::move(fr);
      o["flood_depth_m"] = std::move(fd);
      o["scarp_m"] = std::move(scarp);
      o["scarp_slope"] = JsonValue(wide(p.scarp_slope));
    }
    list.push_back(std::move(o));
  }
  return list;
}
// **The plan's standing bodies** (W122; channels.md, "Hollows"): a body that fills, seeded at
// its hollow's lowest cell at its level. The extent is the hollow's own reach from that cell and
// two cells and four metres more, so the water's flood ends inside it or the level is wrong and
// the scene says so. Millimetres, so the text is the same from every toolchain.
//
// **Its look is its size's** (channels.md, "A lake's look"; docs/experiments/hollows.md, "A
// lake's look"). The wind: the layer's, times the cube root of Hondzo & Stefan's (1993)
// sheltering by area, 1 - exp(-0.3 A / km2), which scales the wind's energy input, the cube of
// its speed. The wet band: a medium sand's capillary rise (0.15 m), the wave that wind raises
// over the body's own reach (the growth law the water capability draws by, H = 1.6e-3 U
// sqrt(F / g), F the diagonal of the square of its area; its run-up is no more than its height
// on a shore gentler than one in five, Hunt 1959) and half the median yearly range of a lake's
// level (House 1985: 0.24 m with an outlet, 0.34 m without). The shore fade: two hundredths of
// its side, between 0.75 and 2 m (a margin, not a source's). The depth the bottom fades by: its
// mean depth, held to the range of Carlson's (1977) Secchi depths, 0.5 to 8 m. Centimetres.
JsonValue plan_bodies_json(const ChannelLayer& p, const ChannelNetwork& n) {
  JsonValue list = JsonValue::array();
  const f64 spacing = static_cast<f64>(n.spacing_mm) / 1000.0;
  u32 written = 0;
  for (const Hollow& h : n.basins.hollows) {
    if (!h.body()) continue;
    // A plan read from a file older than the body's own area holds none: the hollow's.
    const f64 area = h.water_m2 > 0 ? h.water_m2 : h.area_m2;
    const f64 mean_depth = h.water_m2 > 0 ? h.water_m3 / h.water_m2 : 0.0;
    const f64 shelter = 1.0 - det::exp(-0.3 * area / 1e6);
    const f64 wind = round_to(wide(p.basin_wind_m_s) * powd(shelter, 1.0 / 3.0), 1e2);
    const f64 fetch = std::min(std::sqrt(2.0 * area), 2.2e4 * wind * wind / 9.81);
    const f64 wave = 1.6e-3 * wind * std::sqrt(fetch / 9.81);
    const f64 range = h.kind == HollowKind::Terminal ? .34 : .24;
    JsonValue seed = JsonValue::array();
    seed.push_back(JsonValue(round_to(h.floor_x, 1e3)));
    seed.push_back(JsonValue(round_to(h.level_m, 1e3)));
    seed.push_back(JsonValue(round_to(h.floor_z, 1e3)));
    JsonValue fill = JsonValue::object();
    fill["seed"] = std::move(seed);
    fill["max_extent_m"] = JsonValue(round_to(h.extent_m + 2 * spacing + 4, 1e3));
    JsonValue shape = JsonValue::object();
    shape["fill"] = std::move(fill);
    JsonValue o = JsonValue::object();
    o["name"] = JsonValue(h.name);
    o["level_m"] = JsonValue(round_to(h.level_m, 1e3));
    o["shape"] = std::move(shape);
    if (wind > 0) o["wind_m_s"] = JsonValue(wind);
    o["wet_m"] = JsonValue(round_to(.15 + wave + .5 * range, 1e2));
    o["shore_m"] = JsonValue(round_to(std::clamp(.02 * std::sqrt(area), .75, 2.0), 1e2));
    if (mean_depth > 0)
      o["depth_fade_m"] = JsonValue(round_to(std::clamp(mean_depth, .5, 8.0), 1e2));
    o["seed"] = JsonValue(1u + written);
    ++written;
    list.push_back(std::move(o));
  }
  return list;
}

namespace {
// An infilled hollow's floor over a place, metres: its grid's bilinear surface, by the rule the
// tile set's patch is read by (tileset cuts: held at the grid's edge), or far under any ground
// outside the grid's box.
f64 floor_at(const ChannelNetwork& n, const Hollow& h, i64 x_mm, i64 z_mm) noexcept {
  const f64 s = static_cast<f64>(n.spacing_mm);
  const f64 fx = static_cast<f64>(x_mm - h.fill_x_mm) / s;
  const f64 fz = static_cast<f64>(z_mm - h.fill_z_mm) / s;
  if (fx < 0 || fz < 0 || fx > static_cast<f64>(h.fill_nx - 1) ||
      fz > static_cast<f64>(h.fill_nz - 1))
    return -1e9;
  const i64 i = std::min(static_cast<i64>(std::floor(fx)), static_cast<i64>(h.fill_nx) - 2);
  const i64 j = std::min(static_cast<i64>(std::floor(fz)), static_cast<i64>(h.fill_nz) - 2);
  const f64 u = fx - static_cast<f64>(i), v = fz - static_cast<f64>(j);
  const auto at = [&](i64 a, i64 b) {
    return static_cast<f64>(h.fill_mm[static_cast<u32>(b * h.fill_nx + a)]);
  };
  const f64 near_row = at(i, j) + u * (at(i + 1, j) - at(i, j));
  const f64 far_row = at(i, j + 1) + u * (at(i + 1, j + 1) - at(i, j + 1));
  return h.level_m + (near_row + v * (far_row - near_row)) / 1000.0;
}
f64 over_floors(const FlooredGround& g, f64 y, i64 x_mm, i64 z_mm) noexcept {
  for (const Hollow& h : g.network->basins.hollows)
    if (h.kind == HollowKind::Infilled && h.fill_nx >= 2 && h.fill_nz >= 2)
      y = std::max(y, floor_at(*g.network, h, x_mm, z_mm));
  return y;
}
f64 floored_surface(const void* context, i64 x_mm, i64 z_mm) noexcept {
  const FlooredGround& g = *static_cast<const FlooredGround*>(context);
  return over_floors(g, g.base.surface_mm(x_mm, z_mm), x_mm, z_mm);
}
f64 floored_floor(const void* context, i64 x_mm, i64 z_mm) noexcept {
  const FlooredGround& g = *static_cast<const FlooredGround*>(context);
  return over_floors(g, g.base.floor_mm(x_mm, z_mm), x_mm, z_mm);
}
}  // namespace

scene_gen::Ground floored_ground(const ChannelNetwork& network, const scene_gen::Ground& base,
                                 FlooredGround& held) {
  bool any = false;
  for (const Hollow& h : network.basins.hollows)
    any = any || h.kind == HollowKind::Infilled;
  if (!any) return base;
  held.base = base;
  held.network = &network;
  scene_gen::Ground out;
  out.context = &held;
  out.surface_fn = &floored_surface;
  if (base.floor_fn != nullptr) out.floor_fn = &floored_floor;
  return out;
}

// **A dried lake's floor** (channels.md, "A dried lake's floor"): the hollow's box as the
// patch's outline and its floor as the patch's grid. Outside the hollow and its rim the grid
// stands far under any ground, so the fill raises nothing there and its faces meet nothing.
JsonValue terrain_fills_json(const ChannelLayer& p, const ChannelNetwork& n) {
  JsonValue list = JsonValue::array();
  u32 floors = 0;
  for (const Hollow& h : n.basins.hollows) {
    if (h.kind != HollowKind::Infilled || h.fill_nx < 2 || h.fill_nz < 2) continue;
    const f64 x0 = static_cast<f64>(h.fill_x_mm) / 1000.0;
    const f64 z0 = static_cast<f64>(h.fill_z_mm) / 1000.0;
    const f64 x1 =
        static_cast<f64>(h.fill_x_mm + static_cast<i64>(h.fill_nx - 1) * n.spacing_mm) / 1000.0;
    const f64 z1 =
        static_cast<f64>(h.fill_z_mm + static_cast<i64>(h.fill_nz - 1) * n.spacing_mm) / 1000.0;
    const f64 level = round_to(h.level_m, 1e3);
    const auto corner = [&](f64 x, f64 z) {
      JsonValue at = JsonValue::array();
      at.push_back(JsonValue(x));
      at.push_back(JsonValue(level));
      at.push_back(JsonValue(z));
      return at;
    };
    JsonValue outline = JsonValue::array();
    outline.push_back(corner(x0, z0));
    outline.push_back(corner(x1, z0));
    outline.push_back(corner(x1, z1));
    outline.push_back(corner(x0, z1));
    JsonValue heights = JsonValue::array();
    for (const i32 v : h.fill_mm)
      heights.push_back(JsonValue(v));
    JsonValue o = JsonValue::object();
    o["name"] = JsonValue(p.name + " floor " + (h.name.empty() ? std::to_string(floors) : h.name));
    o["mode"] = JsonValue("Fill");
    o["outline"] = std::move(outline);
    o["grid_origin"] = corner(x0, z0);
    o["grid_spacing_mm"] = JsonValue(n.spacing_mm);
    o["grid_nx"] = JsonValue(h.fill_nx);
    o["grid_nz"] = JsonValue(h.fill_nz);
    o["grid_mm"] = std::move(heights);
    ++floors;
    list.push_back(std::move(o));
  }
  return list;
}

HollowStats measure_hollows(const ChannelLayer& p, const ChannelNetwork& n) {
  HollowStats out;
  for (u32 k = 0; k < n.links.size(); ++k) {
    const ChannelLink& link = n.links[k];
    const u32 m = link.line.size();
    if (m < 2 || link.bed.size() != m) continue;
    ++out.links;
    const Vector<f64> s = lengths_of(link.line);
    const auto line_at = [&](u32 i) {
      return link.bed[i] + (i < link.step_depth.size() ? wide(link.step_depth[i]) : 0.0);
    };
    // An outlet's lip is its body's sill, held at the sill's level: not a bed that climbs.
    u32 lip = 0;
    if (link.outlet)
      for (u32 i = 0; i < m; ++i)
        if (round_to(top_of(link, i), 1e3) >= round_to(link.outlet_level, 1e3)) lip = i;
    bool climbs = false;
    for (u32 i = 0; i < m; ++i) {
      const ChannelPoint& pt = link.line[i];
      // A mouth's bed at or under its body's level is the body's own floor to the stream: W121
      // holds it at the level and lets it follow ground that stands under it, drowned either way.
      const bool drowned = link.mouth && line_at(i) <= round_to(link.mouth_level, 1e3) + 1e-3;
      if (i > lip && !drowned) {
        // The bed's own rise, less what a step-pool's pool comes back up by here (its tail,
        // where the pool's depth shrinks toward the next crest). Read on the bed and not on the
        // bed with the pools added back: the chute lowers a bed above a riser, and added back
        // there the pool's depth read as a rise the bed does not have.
        const f64 tail =
            i < link.step_depth.size()
                ? std::max(0.0, wide(link.step_depth[i - 1]) - wide(link.step_depth[i]))
                : 0.0;
        const f64 rise = link.bed[i] - link.bed[i - 1] - tail;
        if (rise > 1e-3 + 1e-9) {
          climbs = true;
          if (rise > out.rise_max_m) {
            out.rise_max_m = rise;
            out.rise_link = k;
          }
        }
      }
      const f64 thalweg =
          section_of(wide(pt.width_m), wide(pt.depth_m), wide(p.bank_slope)).thalweg;
      const f64 trench = top_of(link, i) - thalweg - line_at(i);
      if (trench > out.trench_max_m) {
        out.trench_max_m = trench;
        out.trench_link = k;
      }
      if (i > 0 && trench > thalweg) out.trench_line_m += s[i] - s[i - 1];
    }
    if (climbs) ++out.climbing;
    // Measured from past an outlet's lip, and not where a mouth's last bed is its body's level.
    const u32 from = std::min(lip + (link.outlet ? 1u : 0u), m - 1);
    const bool at_level = link.mouth && line_at(m - 1) <= round_to(link.mouth_level, 1e3) + 1e-3;
    if (!at_level && line_at(m - 1) > line_at(from) + 1e-3) ++out.ends_higher;
  }
  return out;
}

// **The bed's material** (channels.md, "The flood bed"): the outline of each stretch of bed, for
// the terrain's masks. A stretch is a run of a link's kept points at least `bar_min_width_m` wide
// and off a knickpoint; its outline reaches, each side, the further of the cut's own bank top and
// the flood bed's edge, a corner about every width along the line, the left side downstream and
// the right side back. Centimetres, so the text is the same from every toolchain.
JsonValue bed_regions_json(const ChannelLayer& p, const ChannelNetwork& n) {
  JsonValue list = JsonValue::array();
  if (p.bed_material.empty()) return list;
  for (u32 k = 0; k < n.links.size(); ++k) {
    const ChannelLink& link = n.links[k];
    const u32 m = std::min<u32>(link.keep, link.line.size());
    if (m < 3) continue;
    const Vector<f64> s = lengths_of(link.line);
    const auto in_bed = [&](u32 i) {
      return wide(link.line[i].width_m) >= wide(p.bar_min_width_m) &&
             !(i < link.knick.size() && link.knick[i]);
    };
    u32 stretch = 0;
    for (u32 i = 0; i < m;) {
      if (!in_bed(i)) {
        ++i;
        continue;
      }
      u32 e = i;
      while (e + 1 < m && in_bed(e + 1))
        ++e;
      Vector<f64> lx, lz, rx, rz;
      // W97: with `bar_material`, the bed's outline stops at the cut's bank tops and the flood
      // bed each side is a strip of its own, from the bank top out, in the bars' material.
      const bool bars = !p.bar_material.empty();
      Vector<f64> llx, llz, lrx, lrz;  // the left strip's inner and outer edges
      Vector<f64> rlx, rlz, rrx, rrz;  // the right strip's
      f64 next = s[i];
      for (u32 q = i; q <= e; ++q) {
        if (q != e && s[q] < next) continue;
        const ChannelPoint& pt = link.line[q];
        const f64 w = wide(pt.width_m);
        next = s[q] + std::max(w, 1.0);
        const u32 a = q > 0 ? q - 1 : 0, b = std::min<u32>(link.line.size() - 1, q + 1);
        f64 tx = link.line[b].at.x - link.line[a].at.x, tz = link.line[b].at.z - link.line[a].at.z;
        const f64 tl = std::sqrt(tx * tx + tz * tz);
        if (!(tl > 0)) continue;
        tx /= tl;
        tz /= tl;
        const f64 half = .5 * w * (1 + (q < link.swell.size() ? wide(link.swell[q]) : 0.0));
        const f64 left =
            std::max(half, q < link.flood_left.size() ? wide(link.flood_left[q]) : 0.0);
        const f64 right =
            std::max(half, q < link.flood_right.size() ? wide(link.flood_right[q]) : 0.0);
        // The left is the side of (-dz, dx).
        const f64 bed_left = bars ? half : left, bed_right = bars ? half : right;
        lx.push_back(round_to(pt.at.x - tz * bed_left, 1e2));
        lz.push_back(round_to(pt.at.z + tx * bed_left, 1e2));
        rx.push_back(round_to(pt.at.x + tz * bed_right, 1e2));
        rz.push_back(round_to(pt.at.z - tx * bed_right, 1e2));
        if (bars) {
          llx.push_back(round_to(pt.at.x - tz * half, 1e2));
          llz.push_back(round_to(pt.at.z + tx * half, 1e2));
          lrx.push_back(round_to(pt.at.x - tz * left, 1e2));
          lrz.push_back(round_to(pt.at.z + tx * left, 1e2));
          rlx.push_back(round_to(pt.at.x + tz * half, 1e2));
          rlz.push_back(round_to(pt.at.z - tx * half, 1e2));
          rrx.push_back(round_to(pt.at.x + tz * right, 1e2));
          rrz.push_back(round_to(pt.at.z - tx * right, 1e2));
        }
      }
      const auto strip = [&](const Vector<f64>& ax, const Vector<f64>& az, const Vector<f64>& bx,
                             const Vector<f64>& bz, const char* side) {
        bool wide_enough = false;
        for (u32 q = 0; q < ax.size() && !wide_enough; ++q)
          wide_enough = std::fabs(ax[q] - bx[q]) + std::fabs(az[q] - bz[q]) > .05;
        if (ax.size() < 2 || !wide_enough) return;
        JsonValue polygon = JsonValue::array();
        const auto corner = [&](f64 x, f64 z) {
          JsonValue at = JsonValue::array();
          at.push_back(JsonValue(x));
          at.push_back(JsonValue(0.0));
          at.push_back(JsonValue(z));
          polygon.push_back(std::move(at));
        };
        for (u32 q = 0; q < ax.size(); ++q)
          corner(ax[q], az[q]);
        for (u32 q = bx.size(); q-- > 0;)
          corner(bx[q], bz[q]);
        JsonValue o = JsonValue::object();
        o["name"] = JsonValue(p.name + " link " + std::to_string(k) + " bar " +
                              std::to_string(stretch) + " " + side);
        o["polygon"] = std::move(polygon);
        o["material"] = JsonValue(p.bar_material);
        o["feather_m"] = JsonValue(wide(p.bed_feather_m));
        list.push_back(std::move(o));
      };
      if (bars) {
        strip(llx, llz, lrx, lrz, "left");
        strip(rlx, rlz, rrx, rrz, "right");
      }
      if (lx.size() >= 2) {
        JsonValue polygon = JsonValue::array();
        const auto corner = [&](f64 x, f64 z) {
          JsonValue at = JsonValue::array();
          at.push_back(JsonValue(x));
          at.push_back(JsonValue(0.0));
          at.push_back(JsonValue(z));
          polygon.push_back(std::move(at));
        };
        for (u32 q = 0; q < lx.size(); ++q)
          corner(lx[q], lz[q]);
        for (u32 q = rx.size(); q-- > 0;)
          corner(rx[q], rz[q]);
        JsonValue o = JsonValue::object();
        o["name"] =
            JsonValue(p.name + " link " + std::to_string(k) + " bed " + std::to_string(stretch++));
        o["polygon"] = std::move(polygon);
        o["material"] = JsonValue(p.bed_material);
        o["feather_m"] = JsonValue(wide(p.bed_feather_m));
        list.push_back(std::move(o));
      }
      i = e + 1;
    }
  }
  return list;
}
}  // namespace engine::channels

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
    f64 phase = rng.unit() * 2 * k_pi;
    for (u32 i = 0; i < m; ++i) {
      const f64 wi = hydraulic(wide(p.width_at_km2_m), wide(p.width_exponent), pa[i]);
      const f64 di = hydraulic(wide(p.depth_at_km2_m), wide(p.depth_exponent), pa[i]);
      const f64 lambda = wide(p.meander_wavelength_widths) * powd(wi, 1.01) * jitter;
      if (i > 0) phase += (ps[i] - ps[i - 1]) * 2 * k_pi / lambda;
      const f64 edge = std::min(ps[i], length - ps[i]) / (.5 * lambda);
      const f64 ease = smoothstep(0.0, 1.0, edge);
      f64 o = link.meander * .5 * wide(p.meander_amplitude_coeff) * powd(wi, 1.1) *
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
}  // namespace

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
  if (!in(wide(p.step_m), .1, 4)) return fail(error, "step_m");
  if (!in(wide(p.piece_m), 4, 256) || p.piece_m < 4 * p.step_m) return fail(error, "piece_m");
  for (const ChannelSurface* s : {&p.bed, &p.bars, &p.banks})
    if (!surface_in_range(*s, error)) return false;
  return true;
}

bool plan_channels(const ChannelLayer& p, const scene_gen::Ground& ground, u64 seed,
                   jobs::JobSystem* jobs, ChannelNetwork& out, std::string& error) {
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
  for (ChannelLink& link : n.links) {
    build_line(p, n, ground, link);
    n.length_m += link.length_m;
  }
  out = std::move(n);
  return true;
}

namespace {

// A link's cut down its line (channels.md, "The cut"): at each point the top of the bank (the
// ground under the line), the bed and the section's thalweg depth. The bed is the top less the
// thalweg, held down the line where the ground rises a little (the cut runs through a small rise
// deeper than a depth), but **never more than two thalwegs under the ground at the line**: where the
// line climbs out of a filled depression or over a sill (a lake's outlet; on v35 the running
// minimum alone cut 103 m through one), the bed climbs with it.
constexpr f64 k_cut_depth_thalwegs = 2.0;
struct CutPoint {
  f64 top = 0;
  f64 bed = 0;
  f64 thalweg = 0;
};
Vector<CutPoint> cut_of(const ChannelLayer& p, const ChannelLink& link) {
  Vector<CutPoint> out;
  f64 bed = std::numeric_limits<f64>::infinity();
  for (const ChannelPoint& pt : link.line) {
    CutPoint c;
    c.top = round_to(pt.at.y, 1e3);
    c.thalweg = round_to(section_of(wide(pt.width_m), wide(pt.depth_m), wide(p.bank_slope)).thalweg,
                         1e3);
    bed = std::min(bed, round_to(c.top - c.thalweg, 1e3));
    c.bed = std::max(bed, round_to(c.top - k_cut_depth_thalwegs * c.thalweg, 1e3));
    out.push_back(c);
  }
  return out;
}

}  // namespace

JsonValue water_channels_json(const ChannelLayer& p, const ChannelNetwork& n, f64 min_area_m2,
                              f32 stage, bool cut) {
  JsonValue list = JsonValue::array();
  for (u32 k = 0; k < n.links.size(); ++k) {
    const ChannelLink& link = n.links[k];
    if (link.line.size() < 2 || wide(link.line.back().area_m2) < min_area_m2) continue;
    JsonValue o = JsonValue::object();
    o["name"] = JsonValue(p.name + " link " + std::to_string(k));
    JsonValue points = JsonValue::array(), widths = JsonValue::array(), depths = JsonValue::array(),
              slopes = JsonValue::array();
    // Laid on the ground (W86): the bed is the ground's running minimum down the line and the
    // surface stands `stage` of the bankfull depth over it. In the cut (W89): the bed is the cut's
    // and the surface stands `stage` of the thalweg depth over it, inside the banks. The surface never rises
    // downstream, except in the cut where its bed climbs over a sill; `depth_m` is what is then
    // between it and the bed.
    const Vector<CutPoint> cuts = cut ? cut_of(p, link) : Vector<CutPoint>();
    f64 bed = link.line[0].at.y;
    f64 surface = std::numeric_limits<f64>::infinity();
    for (u32 i = 0; i < link.line.size(); ++i) {
      const ChannelPoint& pt = link.line[i];
      if (cut) {
        // Held down the line, except where the cut's bed climbs over a sill, where the water keeps
        // half its depth over it rather than run under the ground.
        bed = cuts[i].bed;
        const f64 water = wide(stage) * cuts[i].thalweg;
        surface = std::max(std::min(surface, round_to(bed + water, 1e3)), round_to(bed + .5 * water, 1e3));
      } else {
        bed = std::min(bed, pt.at.y);
        surface = std::min(surface, round_to(bed + wide(stage) * wide(pt.depth_m), 1e3));
      }
      const f64 depth = std::max(0.0, surface - bed);
      JsonValue at = JsonValue::array();
      at.push_back(JsonValue(round_to(pt.at.x, 1e3)));
      at.push_back(JsonValue(surface));
      at.push_back(JsonValue(round_to(pt.at.z, 1e3)));
      points.push_back(std::move(at));
      widths.push_back(JsonValue(round_to(wide(pt.width_m), 1e3)));
      depths.push_back(JsonValue(round_to(depth, 1e3)));
      slopes.push_back(JsonValue(round_to(wide(pt.slope), 1e5)));
    }
    o["points"] = std::move(points);
    o["width_m"] = std::move(widths);
    o["depth_m"] = std::move(depths);
    o["slope"] = std::move(slopes);
    o["seed"] = JsonValue(link.seed);
    list.push_back(std::move(o));
  }
  return list;
}

JsonValue terrain_cuts_json(const ChannelLayer& p, const ChannelNetwork& n) {
  JsonValue list = JsonValue::array();
  for (u32 k = 0; k < n.links.size(); ++k) {
    const ChannelLink& link = n.links[k];
    if (link.line.size() < 2) continue;
    const Vector<CutPoint> cuts = cut_of(p, link);
    JsonValue o = JsonValue::object();
    o["name"] = JsonValue(p.name + " link " + std::to_string(k));
    JsonValue points = JsonValue::array(), widths = JsonValue::array(), depths = JsonValue::array();
    for (u32 i = 0; i < link.line.size(); ++i) {
      const ChannelPoint& pt = link.line[i];
      JsonValue at = JsonValue::array();
      at.push_back(JsonValue(round_to(pt.at.x, 1e3)));
      at.push_back(JsonValue(cuts[i].top));
      at.push_back(JsonValue(round_to(pt.at.z, 1e3)));
      points.push_back(std::move(at));
      widths.push_back(JsonValue(round_to(wide(pt.width_m), 1e3)));
      // The bed below the top: the thalweg, more where the cut runs through a rise.
      depths.push_back(JsonValue(std::max(1e-3, round_to(cuts[i].top - cuts[i].bed, 1e3))));
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

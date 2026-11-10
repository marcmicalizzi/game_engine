// What the network places (docs/subsystems/channels.md, "What it places"): each link cut into
// pieces, each piece a bed skin, its bars and its banks, every height decided here from the ground
// and written into the piece's recipe; and the mesh generator "channel" that builds a recipe into a
// closed, welded mesh and a container through the derived-data cache.
#include "detail.h"

#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/assets/mesh_generator.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/material_sampling.h>
#include <domain/materials/materials.h>
#include <domain/texture/material_textures.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace engine::channels {
namespace {
using namespace detail;

// How far a part's top stands off the ground where it meets it, so the two never fight for a
// pixel, and how deep its buried bottom goes.
constexpr f64 k_lift_m = .02;
constexpr f64 k_bury_m = .25;
constexpr u32 k_max_rows = 1025;
constexpr u32 k_max_columns = 33;
constexpr u32 k_max_slabs = 256;

struct Frame {
  f64 x, z;    // the line's point
  f64 nx, nz;  // its left normal
  f64 s;       // metres along the link
  f64 kappa;   // signed curvature, 1/m: positive turns toward the normal
};

Vector<Frame> frames_of(const ChannelLink& link) {
  const u32 m = link.line.size();
  Vector<Frame> f(m);
  for (u32 i = 0; i < m; ++i) {
    const u32 a = i > 0 ? i - 1 : 0, b = std::min(m - 1, i + 1);
    f64 tx = link.line[b].at.x - link.line[a].at.x, tz = link.line[b].at.z - link.line[a].at.z;
    const f64 tl = std::sqrt(tx * tx + tz * tz);
    tx = tl > 0 ? tx / tl : 1.0;
    tz = tl > 0 ? tz / tl : 0.0;
    f[i] = {link.line[i].at.x, link.line[i].at.z, -tz, tx, 0.0, 0.0};
    if (i > 0) {
      const f64 dx = f[i].x - f[i - 1].x, dz = f[i].z - f[i - 1].z;
      f[i].s = f[i - 1].s + std::sqrt(dx * dx + dz * dz);
    }
  }
  for (u32 i = 1; i + 1 < m; ++i) {
    const f64 ax = f[i].x - f[i - 1].x, az = f[i].z - f[i - 1].z;
    const f64 bx = f[i + 1].x - f[i].x, bz = f[i + 1].z - f[i].z;
    const f64 la = std::sqrt(ax * ax + az * az), lb = std::sqrt(bx * bx + bz * bz);
    if (la > 0 && lb > 0) f[i].kappa = (ax * bz - az * bx) / (la * lb) / (.5 * (la + lb));
  }
  return f;
}

// The bed's median grain from the bankfull Shields number, D50 = h S / (R tau*) with the submerged
// specific gravity of quartz R = 1.65 (Parker et al. 2007), between sand and a small boulder; and
// the relief a gravel bed's surface has at that grain, one median grain, a few centimetres.
f64 relief_of(const ChannelLayer& p, const ChannelPoint& pt) {
  const f64 d50 = wide(pt.depth_m) * wide(pt.slope) / (1.65 * wide(p.shields_bankfull));
  return std::clamp(d50, .005, .03);
}

ChannelSurface surface_of(const ChannelSurface& s, std::array<f32, 3> color, f32 tile,
                          std::optional<std::array<f32, 3>> tint) {
  ChannelSurface out = s;
  if (!out.color.has_value()) out.color = color;
  if (!out.tile_m.has_value()) out.tile_m = tile;
  if (!out.tint.has_value()) out.tint = tint;
  return out;
}

struct Builder {
  const ChannelLayer& p;
  const scene_gen::Ground& ground;
  f64 y0 = 0;
  i64 x0 = 0, z0 = 0;
  f64 ground_local(f64 x, f64 z, i64& xm, i64& zm) const {
    xm = scene_gen::nearest_mm(x);
    zm = scene_gen::nearest_mm(z);
    return ground.surface_mm(xm, zm) - y0;
  }
  void vertex(ChannelSlab& slab, f64 x, f64 z, f64 top, f64 bottom) const {
    slab.x.push_back(static_cast<f32>(x - static_cast<f64>(x0) / 1000.0));
    slab.z.push_back(static_cast<f32>(z - static_cast<f64>(z0) / 1000.0));
    slab.top.push_back(static_cast<f32>(top));
    slab.bottom.push_back(static_cast<f32>(bottom));
  }
};
// The bed the pieces lay: the cut's flat bed (detail::section_of), and a quarter of the width where
// the section is a V, so a narrow gully still has a strip of gravel at its bottom.
f64 bed_width(const ChannelLayer& p, const ChannelPoint& pt) {
  const f64 w = wide(pt.width_m);
  return std::max(.25 * w, section_of(w, wide(pt.depth_m), wide(p.bank_slope)).bed);
}
// **Where the gravel lies across the section** (W93; channels.md, "The section"): the flat bed
// between the two banks' toes, which at a bend lies against the outer bank, and up the point bar
// from its toe by `k_gravel_bar` of the bar's run times the asymmetry — to about where the water's
// edge stands on it at the default stage. Never narrower than a quarter of the width. `bar_from`
// and `bar_to` are the dry bar above that, signed distances from the line: where the bars' skin
// lies.
constexpr f64 k_gravel_bar = .55;
struct Gravel {
  f64 centre = 0;  // off the line, toward the left
  f64 width = 0;
  f64 bar_from = 0, bar_to = 0;
};
Gravel gravel_of(const ChannelLayer& p, const ChannelPoint& pt, f64 asym, f64 swell) {
  Gravel g;
  if (!(p.section_asymmetry > 0)) {
    g.width = bed_width(p, pt);
    return g;
  }
  const BendSection s = bend_section(p, wide(pt.width_m), wide(pt.depth_m), asym, swell);
  const f64 depth = s.thalweg + s.pool;
  const f64 run_left = depth / s.slope_left, run_right = depth / s.slope_right;
  const f64 a = std::min(1.0, std::fabs(asym));
  // The toes, and the inner one moved up its bar.
  f64 left = .5 * s.width - run_left, right = -.5 * s.width + run_right;
  if (left < right) left = right = .5 * (left + right);
  const f64 inner = asym > 0 ? 1.0 : -1.0;  // the side the bar is on
  const f64 run = asym > 0 ? run_left : run_right;
  if (asym > 0)
    left += a * k_gravel_bar * run;
  else
    right -= a * k_gravel_bar * run;
  g.width = std::max(.25 * s.width, left - right);
  g.centre = .5 * (left + right);
  const f64 edge = asym > 0 ? left : right;
  g.bar_from = edge;
  g.bar_to = edge + inner * (.95 - k_gravel_bar) * a * run;
  return g;
}
}  // namespace

bool place_pieces(const ChannelLayer& p, const ChannelNetwork& n, const scene_gen::Ground& ground,
                  f32 tile_size, Vector<PlacedPiece>& out, std::string& error) {
  out.clear();
  if (!(tile_size >= .25f && tile_size <= 100000.0f)) return fail(error, "tile_size");
  const i64 tile_mm = scene_gen::nearest_mm(wide(tile_size));
  const ChannelSurface bed_look =
      surface_of(p.bed, {.42f, .40f, .37f}, .35f, std::array<f32, 3>{.62f, .60f, .56f});
  const ChannelSurface bar_look = surface_of(p.bars, {.56f, .51f, .43f}, .8f, std::nullopt);
  u32 id = 0;
  const ChannelSurface boulder_look =
      surface_of(ChannelSurface{}, {.45f, .44f, .42f}, 1.2f, std::array<f32, 3>{.7f, .68f, .65f});
  for (const ChannelLink& link : n.links) {
    // W90: a tributary's pieces stop at the trunk's banks (`keep`), and a reach decides what is
    // laid: the gravel bed on every reach but bedrock and colluvial, bars on dune-ripple and
    // pool-riffle reaches, boulders at the crests of step-pool and cascade steps.
    const u32 m = std::min<u32>(link.keep, link.line.size());
    if (m < 2) continue;
    auto reach_at = [&](u32 i) {
      return i < link.reach.size() ? static_cast<ChannelReach>(link.reach[i])
                                   : ChannelReach::PoolRiffle;
    };
    auto has_bed = [&](u32 i) {
      const ChannelReach r = reach_at(i);
      return r != ChannelReach::Bedrock && r != ChannelReach::Colluvial;
    };
    auto has_bars = [&](u32 i) {
      const ChannelReach r = reach_at(i);
      return r == ChannelReach::DuneRipple || r == ChannelReach::PoolRiffle;
    };
    const Vector<Frame> f = frames_of(link);
    f32 widest = 0;
    for (u32 i = 0; i < m; ++i)
      widest = std::max(widest, link.line[i].width_m);
    const u32 columns =
        std::clamp<u32>(static_cast<u32>(std::lround(wide(widest) / .4)) + 1, 3, 17);
    // The bed's rhythm: pool to pool below the step-pool slope, step to step above it, its phase
    // the link's own.
    Rng rng(hash_combine(link.seed, 0x5eedull));
    const f64 phase = rng.unit() * 2 * k_pi;
    Vector<f64> rhythm(m, 0.0);
    f64 theta = phase;
    for (u32 i = 0; i < m; ++i) {
      const f64 wavelength = (link.slope > wide(p.meander_slope_max) ? wide(p.step_pool_widths)
                                                                     : wide(p.pool_riffle_widths)) *
                             wide(link.line[i].width_m);
      if (i > 0) theta += (f[i].s - f[i - 1].s) * 2 * k_pi / std::max(wavelength, .5);
      rhythm[i] = .5 * (1 + det::cos(theta));
    }
    // W93: the section's own rhythm where it has one — a riffle where the section is symmetric,
    // none at a pool's apex — so the bed's rise and the ground's pools agree.
    const bool shaped = p.section_asymmetry > 0 && link.asym.size() == link.line.size();
    auto asym_at = [&](u32 i) { return shaped ? wide(link.asym[i]) : 0.0; };
    auto swell_at = [&](u32 i) {
      return shaped && i < link.swell.size() ? wide(link.swell[i]) : 0.0;
    };
    if (shaped)
      for (u32 i = 0; i < m; ++i)
        rhythm[i] = 1 - std::min(1.0, std::fabs(asym_at(i)));
    // Bars (Leopold, Wolman & Miller 1964): one every half pool-to-pool length, alternating sides
    // along a straight reach and on the inside of a bend where the line bends.
    struct Bar {
      u32 from, to;
      f64 side;
    };
    Vector<Bar> bars;
    if (shaped) {
      // W93: a bar is the ground's own point bar, so its skin lies where the section's asymmetry
      // is at least `k_bar_asymmetry`, on the bar's side, a run of points each.
      constexpr f64 k_bar_asymmetry = .5;
      for (u32 i = 0; i < m;) {
        const f64 a = asym_at(i);
        if (std::fabs(a) < k_bar_asymmetry || !has_bars(i) ||
            wide(link.line[i].width_m) < wide(p.bar_min_width_m)) {
          ++i;
          continue;
        }
        u32 e = i;
        while (e + 1 < m && asym_at(e + 1) * a > 0 &&
               std::fabs(asym_at(e + 1)) >= k_bar_asymmetry && has_bars(e + 1) &&
               e + 1 - i < k_max_rows - 1)
          ++e;
        if (e >= i + 2) bars.push_back({i, e, a > 0 ? 1.0 : -1.0});
        i = e + 1;
      }
    } else if (widest >= p.bar_min_width_m && p.bar_rise > 0) {
      f64 side = rng.unit() < .5 ? -1.0 : 1.0;
      u32 last = 0;  // bars never share a cross-section: each is its own closed lens
      f64 at = .5 * wide(p.pool_riffle_widths) * wide(link.line[0].width_m);
      while (true) {
        u32 c = 0;
        while (c + 1 < m && f[c].s < at)
          ++c;
        const f64 wc = wide(link.line[c].width_m);
        const f64 half = .5 * wide(p.bar_length_widths) * wc;
        if (at + half >= f[m - 1].s) break;
        u32 a = c, b = c;
        while (a > 0 && f[a].s > at - half)
          --a;
        while (b + 1 < m && f[b].s < at + half)
          ++b;
        f64 bend = 0;
        for (u32 i = a; i <= b; ++i)
          bend += f[i].kappa;
        bend /= static_cast<f64>(b - a + 1);
        const f64 s = std::abs(bend) * wc > .05 ? (bend > 0 ? 1.0 : -1.0) : side;
        if (!bars.empty() && a <= last) a = last + 1;
        bool alluvial = true;
        for (u32 i = a; i <= b && alluvial; ++i)
          alluvial = has_bars(i);
        if (b >= a + 2 && wc >= wide(p.bar_min_width_m) && alluvial) {
          bars.push_back({a, b, s});
          last = b;
        }
        side = -side;
        at += .5 * wide(p.pool_riffle_widths) * wc;
      }
    }
    const u32 pieces = std::max<u32>(1, static_cast<u32>(std::ceil(f[m - 1].s / wide(p.piece_m))));
    // (`f` covers the whole line; the pieces cover its first `m` points.)
    for (u32 k = 0; k < pieces; ++k) {
      const u32 r0 = static_cast<u32>(static_cast<u64>(k) * (m - 1) / pieces);
      const u32 r1 = static_cast<u32>(static_cast<u64>(k + 1) * (m - 1) / pieces);
      if (r1 <= r0) continue;
      Builder b{p, ground, 0.0, 0, 0};
      b.x0 = scene_gen::nearest_mm(f[r0].x);
      b.z0 = scene_gen::nearest_mm(f[r0].z);
      b.y0 = ground.surface_mm(b.x0, b.z0);
      PlacedPiece base;
      base.origin =
          WorldPos{static_cast<f64>(b.x0) / 1000.0, b.y0, static_cast<f64>(b.z0) / 1000.0};
      const i64 tx = floor_div(b.x0, tile_mm), tz = floor_div(b.z0, tile_mm);
      if (tx < std::numeric_limits<i32>::min() || tx > std::numeric_limits<i32>::max() ||
          tz < std::numeric_limits<i32>::min() || tz > std::numeric_limits<i32>::max())
        return fail(error, "tile coordinate (out of range)");
      base.tile = {static_cast<i32>(tx), static_cast<i32>(tz)};
      const f32 uv[2] = {static_cast<f32>(static_cast<f64>(b.x0 % 1024000) / 1000.0),
                         static_cast<f32>(static_cast<f64>(b.z0 % 1024000) / 1000.0)};
      // The bed: the cut's flat bed wide (W89; channels.md, "The cut"), on the ground, its top a
      // gravel relief of about a median grain and the riffles' rise; its two edge columns down at
      // the ground, at the banks' toe.
      {
        PlacedPiece piece = base;
        piece.recipe.part = ChannelPart::Bed;
        piece.recipe.uv_origin = {uv[0], uv[1]};
        piece.recipe.surface = bed_look;
        // One slab a run of rows whose reach has a bed (W90: none on bedrock or colluvial).
        for (u32 a = r0; a <= r1;) {
          if (!has_bed(a)) {
            ++a;
            continue;
          }
          u32 e = a;
          while (e + 1 <= r1 && has_bed(e + 1))
            ++e;
          if (e > a) {
            ChannelSlab slab;
            slab.rows = e - a + 1;
            slab.columns = columns;
            for (u32 i = a; i <= e; ++i) {
              const ChannelPoint& pt = link.line[i];
              const f64 relief = relief_of(p, pt);
              const f64 riffle =
                  has_bars(i) ? wide(p.riffle_rise) * wide(pt.depth_m) * rhythm[i] : 0.0;
              for (u32 c = 0; c < columns; ++c) {
                const f64 v = static_cast<f64>(c) / static_cast<f64>(columns - 1) - .5;
                const Gravel gravel = gravel_of(p, pt, asym_at(i), swell_at(i));
                const f64 lateral = gravel.centre + v * gravel.width;
                const f64 x = f[i].x + f[i].nx * lateral;
                const f64 z = f[i].z + f[i].nz * lateral;
                i64 xm = 0, zm = 0;
                const f64 g = b.ground_local(x, z, xm, zm);
                const bool edge = c == 0 || c + 1 == columns;
                const f64 across =
                    1 - 4 * v * v;  // the riffle crosses the bed, highest mid-channel
                const f64 bump =
                    edge ? 0.0
                         : relief * .5 * (1 + place_noise(link.seed, xm, zm)) + riffle * across;
                b.vertex(slab, x, z, g + k_lift_m + bump, g - k_bury_m);
              }
            }
            piece.recipe.slabs.push_back(std::move(slab));
          }
          a = e + 1;
        }
        if (!piece.recipe.slabs.empty()) {
          piece.id = ++id;
          out.push_back(std::move(piece));
        }
      }
      // The bars of this piece: a low lens on one side of the bed, its outer edge at the bed's,
      // its crown `bar_rise` of the depth above the bed.
      {
        PlacedPiece piece = base;
        piece.recipe.part = ChannelPart::Bar;
        piece.recipe.uv_origin = {uv[0], uv[1]};
        piece.recipe.surface = bar_look;
        for (const Bar& bar : bars) {
          const u32 centre = (bar.from + bar.to) / 2;
          if (centre < r0 || centre >= r1 + (k + 1 == pieces ? 1u : 0u)) continue;
          if (piece.recipe.slabs.size() >= k_max_slabs) break;
          ChannelSlab slab;
          slab.rows = bar.to - bar.from + 1;
          slab.columns = 5;
          for (u32 i = bar.from; i <= bar.to; ++i) {
            const ChannelPoint& pt = link.line[i];
            const f64 u = static_cast<f64>(i - bar.from) / static_cast<f64>(bar.to - bar.from);
            const f64 w = wide(pt.width_m);
            const f64 outer = .5 * bed_width(p, pt) - .02;
            const f64 inner = std::max(0.0, outer - wide(p.bar_width_widths) * w);
            const Gravel gravel = gravel_of(p, pt, asym_at(i), swell_at(i));
            // W93: the skin lies on the dry bar, and the lens is the ground's: no crown of its own
            // at full asymmetry.
            const f64 crown = shaped ? 1 - wide(p.section_asymmetry) : 1.0;
            for (u32 c = 0; c < 5; ++c) {
              const f64 v = static_cast<f64>(c) / 4.0;
              const f64 lateral = shaped ? gravel.bar_from + v * (gravel.bar_to - gravel.bar_from)
                                         : bar.side * (inner + v * (outer - inner));
              const f64 x = f[i].x + f[i].nx * lateral;
              const f64 z = f[i].z + f[i].nz * lateral;
              i64 xm = 0, zm = 0;
              const f64 g = b.ground_local(x, z, xm, zm);
              const f64 lens = det::sin(k_pi * u) * det::sin(k_pi * v);
              const f64 top = g + k_lift_m + 2 * relief_of(p, pt) + .01 +
                              crown * wide(p.bar_rise) * wide(pt.depth_m) * std::max(0.0, lens);
              b.vertex(slab, x, z, top, g - .1);
            }
          }
          piece.recipe.slabs.push_back(std::move(slab));
        }
        if (!piece.recipe.slabs.empty()) {
          piece.id = ++id;
          out.push_back(std::move(piece));
        }
      }
      // The steps' boulders (W90): at each step-pool crest in this piece, a row of domes across
      // the bed, each about the step's height across (the clasts that hold a step up are of its
      // height: Chin 1999), a stated jitter of a fifth of that along and across, their tops 0.6 of
      // their width over the ground at the lip and their bottoms buried a third. A cascade's
      // clusters (W91): one to three clasts of the cluster's size, anywhere across the bed and a
      // size along it, so no two clusters line up.
      {
        PlacedPiece piece = base;
        piece.recipe.part = ChannelPart::Boulder;
        piece.recipe.uv_origin = {uv[0], uv[1]};
        piece.recipe.surface = boulder_look;
        auto dome = [&](u32 i, f64 d, f64 lateral, f64 along) {
          const f64 cx = f[i].x + f[i].nx * lateral + f[i].nz * along;
          const f64 cz = f[i].z + f[i].nz * lateral - f[i].nx * along;
          ChannelSlab slab;
          slab.rows = 7;
          slab.columns = 7;
          for (u32 rr = 0; rr < 7; ++rr)
            for (u32 cc = 0; cc < 7; ++cc) {
              const f64 u = static_cast<f64>(rr) / 3.0 - 1, v = static_cast<f64>(cc) / 3.0 - 1;
              const f64 x = cx + .5 * d * (f[i].nz * u + f[i].nx * v);
              const f64 z = cz + .5 * d * (-f[i].nx * u + f[i].nz * v);
              i64 xm = 0, zm = 0;
              const f64 g = b.ground_local(x, z, xm, zm);
              const f64 top = std::sqrt(std::max(0.0, 1 - .5 * (u * u + v * v)));
              b.vertex(slab, x, z, g + k_lift_m + .6 * d * top, g - d / 3);
            }
          piece.recipe.slabs.push_back(std::move(slab));
        };
        const u32 last = r1 + (k + 1 == pieces ? 1u : 0u);
        for (u32 q = 0; q < link.step_at.size(); ++q) {
          const u32 i = link.step_at[q];
          if (i < r0 || i >= last || i >= m) continue;
          if (reach_at(i) != ChannelReach::StepPool) continue;
          const ChannelPoint& pt = link.line[i];
          const f64 bed_w = gravel_of(p, pt, asym_at(i), swell_at(i)).width;
          const f64 size = std::clamp(wide(link.step_height[q]), .25, 1.5);
          const u32 count = std::max<u32>(1, static_cast<u32>(std::lround(bed_w / size)));
          Rng draw(hash_combine(link.seed, 0xb0u + i));
          for (u32 c = 0; c < count && piece.recipe.slabs.size() < k_max_slabs; ++c) {
            const f64 d = size * (.8 + .4 * draw.unit());
            const f64 across = (static_cast<f64>(c) + .5) / static_cast<f64>(count) - .5;
            const f64 lateral = across * bed_w + .2 * size * (2 * draw.unit() - 1);
            const f64 along = .2 * size * (2 * draw.unit() - 1);
            dome(i, d, lateral, along);
          }
        }
        for (u32 q = 0; q < link.cluster_at.size(); ++q) {
          const u32 i = link.cluster_at[q];
          if (i < r0 || i >= last || i >= m) continue;
          const f64 bed_w = gravel_of(p, link.line[i], asym_at(i), swell_at(i)).width;
          const f64 size = wide(link.cluster_size[q]);
          Rng draw(hash_combine(link.seed, 0xc1u + i));
          const u32 count = 1 + static_cast<u32>(draw.unit() * 3.0);
          for (u32 c = 0; c < count && piece.recipe.slabs.size() < k_max_slabs; ++c) {
            const f64 d = size * (.7 + .6 * draw.unit());
            const f64 lateral = (draw.unit() - .5) * std::max(0.0, bed_w - .5 * d);
            const f64 along = size * (2 * draw.unit() - 1);
            dome(i, d, lateral, along);
          }
        }
        if (!piece.recipe.slabs.empty()) {
          piece.id = ++id;
          out.push_back(std::move(piece));
        }
      }
    }
  }
  for (const PlacedPiece& piece : out)
    if (!piece_in_range(piece.recipe, error)) return false;
  return true;
}

bool piece_in_range(const ChannelPiece& piece, std::string& error) {
  if (piece.slabs.empty() || piece.slabs.size() > k_max_slabs) return fail(error, "piece slabs");
  if (!in(wide(piece.uv_origin[0]), -2048, 2048) || !in(wide(piece.uv_origin[1]), -2048, 2048))
    return fail(error, "piece uv_origin");
  if (!surface_in_range(piece.surface, error)) return false;
  u64 vertices = 0;
  for (const ChannelSlab& s : piece.slabs) {
    if (s.rows < 2 || s.rows > k_max_rows || s.columns < 2 || s.columns > k_max_columns)
      return fail(error, "slab rows/columns");
    const u32 count = s.rows * s.columns;
    if (s.x.size() != count || s.z.size() != count || s.top.size() != count ||
        s.bottom.size() != count)
      return fail(error, "slab array sizes");
    for (u32 i = 0; i < count; ++i)
      if (!in(wide(s.x[i]), -1000, 1000) || !in(wide(s.z[i]), -1000, 1000) ||
          !in(wide(s.top[i]), -10000, 10000) || !in(wide(s.bottom[i]), -10000, 10000) ||
          !(s.top[i] > s.bottom[i]))
        return fail(error, "slab samples (finite, top above bottom)");
    vertices += 2ull * count;
  }
  if (vertices > 4000000) return fail(error, "piece size");
  return true;
}

bool solve_piece(const ChannelPiece& piece, assets::MeshData& out, std::string& error) {
  out = {};
  if (!piece_in_range(piece, error)) return false;
  assets::MeshData mesh;
  assets::Material material;
  material.name = piece.part == ChannelPart::Bed       ? "channel bed"
                  : piece.part == ChannelPart::Bar     ? "channel bar"
                  : piece.part == ChannelPart::Boulder ? "channel boulder"
                                                       : "channel bank";
  const auto& color = *piece.surface.color;
  material.base_color = {color[0], color[1], color[2], 1.0f};
  material.metallic = 0;
  material.roughness = piece.surface.roughness;
  mesh.materials.push_back(material);
  const f32 u0 = piece.uv_origin[0], v0 = piece.uv_origin[1];
  auto tri = [&](u32 a, u32 b, u32 c) {
    mesh.indices.push_back(a);
    mesh.indices.push_back(b);
    mesh.indices.push_back(c);
  };
  for (const ChannelSlab& s : piece.slabs) {
    const u32 count = s.rows * s.columns, first = mesh.positions.size();
    for (u32 layer = 0; layer < 2; ++layer)
      for (u32 i = 0; i < count; ++i) {
        mesh.positions.push_back({s.x[i], layer ? s.bottom[i] : s.top[i], s.z[i]});
        mesh.normals.push_back({0, 0, 0});
        mesh.uvs.push_back({u0 + s.x[i], v0 + s.z[i]});
      }
    // Which way round the grid runs decides the winding: the top faces up.
    f64 turn = 0;
    for (u32 r = 0; r + 1 < s.rows; ++r)
      for (u32 c = 0; c + 1 < s.columns; ++c) {
        const u32 a = r * s.columns + c, b = a + 1, d = a + s.columns;
        const f64 ex = wide(s.x[d]) - wide(s.x[a]), ez = wide(s.z[d]) - wide(s.z[a]);
        const f64 fx = wide(s.x[b]) - wide(s.x[a]), fz = wide(s.z[b]) - wide(s.z[a]);
        turn += ez * fx - ex * fz;  // the y of (d - a) x (b - a)
      }
    const bool flip = turn < 0;
    const u32 top_first = mesh.indices.size();
    for (u32 r = 0; r + 1 < s.rows; ++r)
      for (u32 c = 0; c + 1 < s.columns; ++c) {
        const u32 a = first + r * s.columns + c, b = a + 1, d = a + s.columns, e = d + 1;
        if (!flip) {
          tri(a, d, b);
          tri(b, d, e);
          tri(a + count, b + count, d + count);
          tri(b + count, e + count, d + count);
        } else {
          tri(a, b, d);
          tri(b, e, d);
          tri(a + count, d + count, b + count);
          tri(b + count, d + count, e + count);
        }
      }
    // Smooth normals over the top and over the bottom, area-weighted.
    for (u32 t = top_first; t < mesh.indices.size(); t += 3) {
      const u32 a = mesh.indices[t], b = mesh.indices[t + 1], c = mesh.indices[t + 2];
      const Vec3 normal =
          cross(mesh.positions[b] - mesh.positions[a], mesh.positions[c] - mesh.positions[a]);
      mesh.normals[a] = mesh.normals[a] + normal;
      mesh.normals[b] = mesh.normals[b] + normal;
      mesh.normals[c] = mesh.normals[c] + normal;
    }
    for (u32 v = first; v < mesh.positions.size(); ++v) {
      const Vec3 normal = mesh.normals[v];
      mesh.normals[v] =
          length(normal) > 0 ? normalize(normal) : Vec3{0, v < first + count ? 1.0f : -1.0f, 0};
    }
    // The four walls, from the top's boundary edges in the direction the top runs them: each wall
    // carries the edge backwards, down to the bottom and along it, with vertices of its own so its
    // flat normal does not bend the top's.
    auto wall = [&](u32 a, u32 b) {
      const u32 at = first + a, bt = first + b;
      const u32 w0 = mesh.positions.size();
      const Vec3 corners[4] = {mesh.positions[bt], mesh.positions[at], mesh.positions[at + count],
                               mesh.positions[bt + count]};
      const Vec3 normal = normalize(cross(corners[1] - corners[0], corners[2] - corners[0]));
      for (u32 q = 0; q < 4; ++q) {
        mesh.positions.push_back(corners[q]);
        mesh.normals.push_back(normal);
        mesh.uvs.push_back({u0 + corners[q].x, v0 + corners[q].y});
      }
      tri(w0, w0 + 1, w0 + 2);
      tri(w0, w0 + 2, w0 + 3);
    };
    // The top's boundary, each edge as a top triangle runs it.
    const u32 R = s.rows, C = s.columns;
    auto at = [&](u32 r, u32 c) { return r * C + c; };
    for (u32 c = 0; c + 1 < C; ++c) {
      // first row: the triangle (a, d, b) runs b -> a unflipped; (a, b, d) runs a -> b flipped.
      if (!flip)
        wall(at(0, c + 1), at(0, c));
      else
        wall(at(0, c), at(0, c + 1));
      // last row: (b, d, e) runs d -> e unflipped; (b, e, d) runs e -> d flipped.
      if (!flip)
        wall(at(R - 1, c), at(R - 1, c + 1));
      else
        wall(at(R - 1, c + 1), at(R - 1, c));
    }
    for (u32 r = 0; r + 1 < R; ++r) {
      // first column: (a, d, b) runs a -> d unflipped; (a, b, d) runs d -> a flipped.
      if (!flip)
        wall(at(r, 0), at(r + 1, 0));
      else
        wall(at(r + 1, 0), at(r, 0));
      // last column: (b, d, e) runs e -> b unflipped; (b, e, d) runs b -> e flipped.
      if (!flip)
        wall(at(r + 1, C - 1), at(r, C - 1));
      else
        wall(at(r, C - 1), at(r + 1, C - 1));
    }
  }
  assets::Primitive part;
  part.index_count = mesh.indices.size();
  part.material = 0;
  mesh.primitives.push_back(part);
  out = std::move(mesh);
  return true;
}

std::string piece_json(const ChannelPiece& piece) {
  JsonWriteOptions o;
  o.pretty = false;
  return write_json(schema::to_json(piece), o);
}

u64 piece_source_hash(const ChannelPiece& piece) {
  const std::string text = piece_json(piece);
  return hash_combine(hash_bytes(text.data(), text.size()), k_channels_version);
}

namespace {
// The roles a part samples when its surface names none: the first the pack has.
Vector<std::string_view> roles_of(ChannelPart part) {
  switch (part) {
    case ChannelPart::Bed: return {"gravel/bed", "rock/gravel", "rock/granite"};
    case ChannelPart::Bar: return {"sand/bar", "soil/parent", "soil/subsoil"};
    case ChannelPart::Boulder: return {"rock/boulder", "rock/granite"};
    default: return {"soil/subsoil", "soil/topsoil"};
  }
}
}  // namespace

bool build_piece_container(const ChannelPiece& piece, const geometry::ClusterLodOptions& lod,
                           u32 page_bytes, geometry::ClusterFileData& out, std::string& error,
                           const materials::Pack* pack) {
  out = {};
  assets::MeshData mesh;
  if (!solve_piece(piece, mesh, error)) return false;
  geometry::ClusterLodOptions options = lod;
  options.normal_cones = true;
  if (!geometry::build_cluster_lod(mesh.positions, {mesh.indices.data(), mesh.indices.size()},
                                   options, out.mesh, &error, assets::attribute_source(mesh)))
    return false;
  if (page_bytes > 0) {
    geometry::ClusterPagesOptions pages;
    pages.page_bytes = page_bytes;
    if (!geometry::build_cluster_pages(out.mesh, pages, out.pages, &error)) return false;
  }
  out.cluster_material.assign(out.mesh.mesh.clusters.size(), 0u);
  geometry::ClusterFileMaterial material;
  material.base_color = mesh.materials[0].base_color;
  material.metallic = 0;
  material.roughness = mesh.materials[0].roughness;
  out.materials.push_back(material);
  const materials::MaterialSet* chosen = nullptr;
  if (!piece.surface.role.empty()) {
    chosen = materials::resolve_set(nullptr, pack, piece.surface.role);
  } else {
    for (std::string_view role : roles_of(piece.part))
      if ((chosen = materials::find_role(pack, role)) != nullptr) break;
  }
  if (chosen != nullptr) {
    out.material_sampling.push_back(geometry::encode_material_sampling(
        mesh.materials[0].sampling, mesh.materials[0].occlusion_strength));
    materials::MaterialSet applied = *chosen;
    applied.tile_m = *piece.surface.tile_m;
    applied.stochastic = piece.surface.sampling == ChannelSampling::Stochastic;
    materials::apply_set(applied, materials::UvLayout::metres, out, 0);
    if (piece.surface.tint.has_value()) {
      const auto& tint = *piece.surface.tint;
      out.materials[0].base_color = {tint[0], tint[1], tint[2], 1.0f};
    }
  }
  if (!out.image_paths.empty()) texture::fill_cluster_texture_records(out);
  out.source_hash = piece_source_hash(piece);
  out.build_key = assets::generated_mesh_build_key(
      out.source_hash, assets::MeshGeneratorInputs{pack}, lod, page_bytes);
  out.generator = "channel";
  out.generator_kind = piece.part == ChannelPart::Bed       ? "channel bed"
                       : piece.part == ChannelPart::Bar     ? "channel bar"
                       : piece.part == ChannelPart::Boulder ? "channel boulder"
                                                            : "channel bank";
  out.generator_params = piece_json(piece);
  return true;
}

namespace {
bool read_piece(std::span<const u8> bytes, ChannelPiece& out, std::string& error) {
  JsonValue json;
  const auto parsed = parse_json({reinterpret_cast<const char*>(bytes.data()), bytes.size()}, json);
  if (!parsed.ok) {
    error = parsed.message;
    return false;
  }
  schema::ReadContext ctx;
  if (!schema::from_json(out, json, ctx) || !ctx.ok()) {
    error = "engine.channels.ChannelPiece";
    for (const auto& d : ctx.diagnostics)
      error += "; " + d.path + ": " + d.message;
    return false;
  }
  if (!out.surface.color.has_value()) out.surface.color = std::array<f32, 3>{.5f, .5f, .5f};
  if (!out.surface.tile_m.has_value()) out.surface.tile_m = 1.0f;
  return piece_in_range(out, error);
}
bool source_hash(std::span<const u8> bytes, u64& out, std::string& error) {
  ChannelPiece p;
  if (!read_piece(bytes, p, error)) return false;
  out = piece_source_hash(p);
  return true;
}
bool build(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs,
           const geometry::ClusterLodOptions& lod, u32 pages, geometry::ClusterFileData& out,
           std::string& error) {
  ChannelPiece p;
  return read_piece(bytes, p, error) &&
         build_piece_container(p, lod, pages, out, error, inputs.materials);
}
constexpr assets::MeshGeneratorDesc k_mesh{
    .name = "channel", .source_hash = &source_hash, .build = &build};
const assets::MeshGeneratorRegistrar k_registrar{k_mesh};
}  // namespace

}  // namespace engine::channels

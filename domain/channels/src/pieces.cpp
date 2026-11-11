// What the network places (docs/subsystems/channels.md, "What it places", "The clasts"): every
// clast of the bed's sediment large enough to be drawn as geometry, and the steps' and cascades'
// boulders, decided from the plan at whole millimetres and merged into one recipe a world tile;
// and the mesh generator "channel" that builds a recipe into its mesh and a container through the
// derived-data cache.
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
#include <atomic>
#include <cmath>
#include <limits>
#include <optional>

namespace engine::channels {
namespace {
using namespace detail;

std::atomic<u64> g_pieces_solved{0};

constexpr u32 k_max_clasts = 400000;  // a piece (a tile)
// **The clasts' numbers** (W97; channels.md, "The clasts"; each one's source and status on the
// grounding page, docs/experiments/channels.md, "Bed sediment"):
// A station's strip across the bed is drawn in cells about this wide, each with its own zone and
// grain sizes, and the sizes in bins this wide in psi (log2 of the size).
constexpr f64 k_cell_m = .5;
constexpr f64 k_bin_psi = .25;
// Shape (Zingg 1935; Sneed & Folk 1958, not read: from memory, a design choice inside the usual
// river-gravel range): b/a 0.7 +- 0.15, c/b 0.6 +- 0.2.
constexpr f64 k_ba = .7, k_ba_spread = .15;
constexpr f64 k_cb = .6, k_cb_spread = .2;
// **Burial**: a clast protrudes over the bed around it by 0.36 of its b axis on average (Vazquez-
// Tarrio et al. 2024, read in a secondary: 0.04 to 1, means 0.35 to 0.38), drawn from a triangle
// 0.15 to 0.57; never more than 0.9 of its own height, so every clast is partly buried.
constexpr f64 k_protrusion = .36, k_protrusion_spread = .21;
constexpr f64 k_exposed_max = .9;
// **Imbrication**: the a-b plane dips upstream by 10 to 30 degrees (often quoted; one secondary
// measures 12 to 55, mean 34.5; the primary sources were not read), the a axis across the flow
// within about 20 degrees (rolling clasts).
constexpr f64 k_dip_lo = .17633, k_dip_hi = .57735;  // tan 10, tan 30 degrees
constexpr f64 k_yaw_jitter = .35;
// **Where on the section**: a bank's foot, the lower half of an outer bank or a riffle's bank; a
// bar, the inner bank to 0.9 of its height; nowhere a bed lies more than 1.6 mean depths under the
// bank's top (the deepest of a bend's pool, where the water is twice the mean deep); on the flood
// bed, a quarter metre off the channel's lip and half a metre off the scarp's foot. Design
// choices: none on a bank's lip, none on a scarp.
constexpr f64 k_bank_foot = .5;
constexpr f64 k_bar_top = .1;
constexpr f64 k_pool_depths = 1.6;
constexpr f64 k_lip_m = .25;
constexpr f64 k_scarp_m = .5;
// **The bar's sorting** (Rice & Church 2010, the abstract: the surface median fines a third from
// head to tail): 1.2 of the reach's median at the head, 0.8 at the tail. The flood bed's surface is
// the tail's (a design choice: a flood bed is built of what the bars leave).
constexpr f64 k_bar_head = 1.2, k_bar_tail = .8;
constexpr f64 k_flood_bed = .8;

// **What a stone is made of** (W97b, G165; channels.md, "What a stone is made of"; the sources on
// docs/experiments/channels.md, "The stones' rocks"): a gravel bed carries a handful of
// lithologies from its catchment (Kodama 1994, five in one bed, abstract), so a clast is one of
// four, drawn from its seed by share, in one of two tones. A tile's mesh draws its materials'
// constants only (ADR-0061), so the albedo is the dry rock's own, linear: grey granite 0.18, basalt
// 0.12, sandstone 0.12 and a pale (chalk, limestone, quartzite) 0.25 (a measured table of dry stone
// albedo, read in full; broadband). The shares follow `gravel/bed`'s generated set (matgen) so the
// stones and the bed's texture between them agree; the hues and the tones (0.85 and 1.15 of the
// albedo) are design choices.
struct ClastLithology {
  const char* name;
  f32 albedo[3];
  f32 roughness;
  f64 share;
};
constexpr ClastLithology k_lithologies[] = {
    {"granite", {.19f, .18f, .17f}, .8f, .45},
    {"pale", {.27f, .26f, .24f}, .75f, .25},
    {"basalt", {.12f, .12f, .125f}, .75f, .15},
    {"sandstone", {.16f, .12f, .085f}, .85f, .15},
};
constexpr u32 k_clast_tones = 2;
constexpr f32 k_tone[k_clast_tones] = {.85f, 1.15f};
constexpr u32 k_clast_materials = 4 * k_clast_tones;
static_assert(sizeof(k_lithologies) / sizeof(k_lithologies[0]) * k_clast_tones ==
              k_clast_materials);
// A clast's material from its seed alone: the recipe carries no lithology, so the same recipe is
// the same stones whichever tile or host builds it.
u32 clast_material(u32 seed) {
  const u64 h = hash_combine(0x11740107ull, seed);
  const f64 u = static_cast<f64>(h >> 40) / 16777216.0;
  u32 lith = 0;
  f64 sum = k_lithologies[0].share;
  while (lith + 1 < 4 && u >= sum)
    sum += k_lithologies[++lith].share;
  return lith * k_clast_tones + static_cast<u32>((h >> 8) & 1u);
}

struct Frame {
  f64 x, z;    // the line's point
  f64 nx, nz;  // its left normal
  f64 s;       // metres along the link
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
    f[i] = {link.line[i].at.x, link.line[i].at.z, -tz, tx, 0.0};
    if (i > 0) {
      const f64 dx = f[i].x - f[i - 1].x, dz = f[i].z - f[i - 1].z;
      f[i].s = f[i - 1].s + std::sqrt(dx * dx + dz * dz);
    }
  }
  return f;
}

// The clasts' surface as the recipe carries it: the tile size defaulted, and the colour left as
// authored — absent, the stones are the lithologies' mix (W97b).
ChannelSurface surface_of(const ChannelSurface& s, f32 tile) {
  ChannelSurface out = s;
  if (!out.tile_m.has_value()) out.tile_m = tile;
  return out;
}

// The bed a reach's boulders stand across: the cut's flat bed between its banks' toes
// (detail::bend_section), a quarter of the width where the section is a V, so a narrow gully still
// has room for a clast at its bottom.
f64 bed_width(const ChannelLayer& p, const ChannelPoint& pt, f64 asym, f64 swell) {
  const BendSection s = bend_section(p, wide(pt.width_m), wide(pt.depth_m), asym, swell);
  return std::max(.25 * s.width, s.bed);
}

// Lays one clast: its frame from the flow's, its burial from the ground under it.
struct Layer {
  const scene_gen::Ground& ground;
  Vector<Clast>& out;
  bool lay(Rng& draw, const Frame& f, f64 x, f64 z, f64 b_m, f64 roundness, u8 zone, f32 bar,
           u32 link, u32 point) {
    // Every number the recipe keeps is rounded first (millimetres, ten-thousandths), so what the
    // burial below is decided from is what the mesh is built from.
    const auto mm = [](f64 v) { return static_cast<f64>(scene_gen::nearest_mm(v)) / 1000; };
    const f64 B = std::max(.002, mm(.5 * b_m));
    const f64 A = std::max(B, mm(B / (k_ba + k_ba_spread * (2 * draw.unit() - 1))));
    const f64 C = std::clamp(mm(B * (k_cb + k_cb_spread * (2 * draw.unit() - 1))), .001, B);
    // The a axis across the flow (the left normal), turned a little toward the flow; then b, which
    // the recipe derives as (a.z, -a.x), points downstream.
    const f64 tx = f.nz, tz = -f.nx;  // downstream
    const f64 jitter = k_yaw_jitter * (2 * draw.unit() - 1);
    f64 ax = f.nx + jitter * tx, az = f.nz + jitter * tz;
    const f64 al = std::sqrt(ax * ax + az * az);
    ax = static_cast<f64>(std::llround(ax / al * 10000)) / 10000;
    az = static_cast<f64>(std::llround(az / al * 10000)) / 10000;
    const f64 dip =
        static_cast<f64>(std::llround((k_dip_lo + (k_dip_hi - k_dip_lo) * draw.unit()) * 10000)) /
        10000;
    // The b axis rises downstream by `dip`; its height and c's are what stand the clast up.
    const f64 bl = std::sqrt(1 + dip * dip);
    const f64 by = dip / bl, cy = 1 / bl;  // c = a x b: its y is the b axis's horizontal share
    const f64 hv = std::sqrt(B * by * B * by + C * cy * C * cy);
    const auto at = [&](f64 px, f64 pz) {
      return ground.surface_mm(scene_gen::nearest_mm(px), scene_gen::nearest_mm(pz));
    };
    const i64 xm = scene_gen::nearest_mm(x), zm = scene_gen::nearest_mm(z);
    const f64 g = ground.surface_mm(xm, zm);
    f64 low = g;
    for (const f64 sa : {-1.0, 1.0}) {
      low = std::min(low, at(x + sa * A * ax, z + sa * A * az));
      low = std::min(low, at(x + sa * B * az, z - sa * B * ax));
    }
    if (!std::isfinite(g) || !std::isfinite(low)) return false;
    const f64 share = k_protrusion + k_protrusion_spread * (draw.unit() + draw.unit() - 1);
    const f64 exposed = std::min(share * b_m, k_exposed_max * 2 * hv);
    Clast c;
    c.ground_mm = static_cast<i64>(std::floor(low * 1000));
    // None floating: its lowest point at least 10 mm under the lowest ground beneath it, in the
    // recipe's own millimetres.
    const i64 hv_mm = static_cast<i64>(std::ceil(hv * 1000));
    c.y_mm = std::min(scene_gen::nearest_mm(g + exposed - hv), c.ground_mm - 10 + hv_mm - 1);
    c.x_mm = xm;
    c.z_mm = zm;
    c.a_mm = scene_gen::nearest_mm(A);
    c.b_mm = scene_gen::nearest_mm(B);
    c.c_mm = scene_gen::nearest_mm(C);
    c.ax = ax;
    c.az = az;
    c.dip = dip;
    c.roundness = static_cast<f64>(std::llround(roundness * 1000)) / 1000;
    c.seed = static_cast<u32>(draw.next() >> 33);
    c.zone = zone;
    c.bar = bar;
    c.link = link;
    c.point = point;
    out.push_back(c);
    return true;
  }
};

i32 narrow(i64 v) {
  return static_cast<i32>(
      std::clamp<i64>(v, std::numeric_limits<i32>::min(), std::numeric_limits<i32>::max()));
}
}  // namespace

Clast clast_of(const ChannelPiece& piece, u32 index) {
  Clast c;
  const i32* v = piece.clasts.data() + static_cast<usize>(index) * k_clast_ints;
  c.x_mm = v[0];
  c.y_mm = v[1];
  c.z_mm = v[2];
  c.ground_mm = v[3];
  c.a_mm = v[4];
  c.b_mm = v[5];
  c.c_mm = v[6];
  c.ax = static_cast<f64>(v[7]) / 10000;
  c.az = static_cast<f64>(v[8]) / 10000;
  c.dip = static_cast<f64>(v[9]) / 10000;
  c.roundness = static_cast<f64>(v[10]) / 1000;
  c.seed = static_cast<u32>(v[11]);
  return c;
}

// **What is placed** (W97; channels.md, "What it places", "The clasts"): the clasts of the bed's
// sediment from `clast_min_mm` up, at the density its distribution says, and the boulders of the
// steps and cascades, every one decided from the plan (its station, its cell across the section,
// its size bin and its draw), so a clast is the same whichever tile came first and whatever the
// tile's size; then one piece a world tile.
bool place_pieces(const ChannelLayer& p, const ChannelNetwork& n, const scene_gen::Ground& ground,
                  f32 tile_size, Vector<PlacedPiece>& out, std::string& error, ClastLog* log) {
  out.clear();
  if (log) *log = {};
  if (!(tile_size >= .25f && tile_size <= 100000.0f)) return fail(error, "tile_size");
  const i64 tile_mm = scene_gen::nearest_mm(wide(tile_size));
  const ChannelSurface look = surface_of(p.boulders, 1.2f);
  const f64 psi_min = log2_of(std::max(1.0, wide(p.clast_min_mm)) / 1000.0);
  Vector<Clast> all;
  Layer layer{ground, all};
  f64 area[k_clast_zones] = {};
  for (u32 k = 0; k < n.links.size(); ++k) {
    const ChannelLink& link = n.links[k];
    // W90: a tributary's clasts stop at the trunk's banks (`keep`).
    const u32 m = std::min<u32>(link.keep, link.line.size());
    if (m < 2 || link.sediment.size() != link.line.size()) continue;
    auto reach_at = [&](u32 i) {
      return i < link.reach.size() ? static_cast<ChannelReach>(link.reach[i])
                                   : ChannelReach::PoolRiffle;
    };
    const Vector<Frame> f = frames_of(link);
    const bool shaped = link.asym.size() == link.line.size();
    auto asym_at = [&](u32 i) { return shaped ? wide(link.asym[i]) : 0.0; };
    auto swell_at = [&](u32 i) {
      return shaped && i < link.swell.size() ? wide(link.swell[i]) : 0.0;
    };
    // ---- the bed's sediment, station by station -------------------------------------------------
    for (u32 i = 0; i < m; ++i) {
      if (i < link.knick.size() && link.knick[i]) continue;  // the water falls there
      const ChannelSediment& sd = link.sediment[i];
      const f64 ds = .5 * (f[std::min(i + 1, m - 1)].s - f[i > 0 ? i - 1 : 0].s);
      if (!(ds > 0)) continue;
      const ChannelPoint& pt = link.line[i];
      const f64 asym = asym_at(i);
      const BendSection sec =
          bend_section(p, wide(pt.width_m), wide(pt.depth_m), asym, swell_at(i));
      if (!(sec.width > 0)) continue;
      const f64 half = .5 * sec.width;
      const f64 deep = sec.thalweg + sec.pool;
      const f64 mean = wide(pt.depth_m) * wide(pt.width_m) / sec.width;
      const f64 fl = i < link.flood_left.size() ? wide(link.flood_left[i]) : 0.0;
      const f64 fr = i < link.flood_right.size() ? wide(link.flood_right[i]) : 0.0;
      const f64 lo = -std::max(half, fr), hi = std::max(half, fl);
      const u32 cells = std::max<u32>(1, static_cast<u32>(std::ceil((hi - lo) / k_cell_m)));
      const f64 cw = (hi - lo) / static_cast<f64>(cells);
      const f64 factor = reach_at(i) == ChannelReach::Bedrock ? .3 : 1.0;
      const f64 sigma = std::max(.1, wide(sd.sorting));
      for (u32 c = 0; c < cells; ++c) {
        const f64 o = lo + (static_cast<f64>(c) + .5) * cw;  // left positive
        u8 zone = 0;
        f64 scale = 1;
        if (std::fabs(o) <= half) {
          // The section's depth under the bank's top here: its two banks and the flat bed.
          const f64 depth =
              std::min({deep, (half - o) * sec.slope_left, (o + half) * sec.slope_right});
          if (depth > k_pool_depths * mean) continue;
          const bool bar_side = sd.bar >= 0 && (asym > 0 ? o > 0 : o < 0);
          if (depth < deep * (1 - 1e-9)) {
            if (bar_side) {
              if (depth < k_bar_top * deep) continue;
              zone = 1;
            } else {
              if (depth < k_bank_foot * deep) continue;
              zone = 2;
            }
          } else if (bar_side) {
            zone = 1;
          }
          if (zone == 1) scale = k_bar_head + (k_bar_tail - k_bar_head) * wide(sd.bar);
        } else {
          const f64 reach = o > 0 ? fl : fr;
          if (std::fabs(o) < half + k_lip_m || std::fabs(o) > reach - k_scarp_m) continue;
          zone = 3;
          scale = k_flood_bed;
        }
        area[zone] += cw * ds;
        const f64 psi50 = log2_of(wide(sd.d50_m) * scale);
        const f64 psi_top = psi50 + 2.5 * sigma;
        Rng draw(hash_combine(hash_combine(link.seed, 0x5ed0000ull + i), c));
        for (f64 b0 = psi_min; b0 < psi_top; b0 += k_bin_psi) {
          const f64 b1 = std::min(b0 + k_bin_psi, psi_top);
          const f64 share = normal_cdf((b1 - psi50) / sigma) - normal_cdf((b0 - psi50) / sigma);
          const f64 size = exp2_of(.5 * (b0 + b1));
          // A clast covers pi/4 a b of the bed seen from above, a = b / (b/a).
          const f64 each = .25 * k_pi * size * size / k_ba;
          const f64 expect = share * factor * cw * ds / each;
          const u32 count = static_cast<u32>(expect + draw.unit());
          for (u32 q = 0; q < count; ++q) {
            const f64 b = exp2_of(b0 + (b1 - b0) * draw.unit());
            const f64 lateral = o + (draw.unit() - .5) * cw;
            const f64 along = (draw.unit() - .5) * 2 * ds;
            const f64 tx = f[i].nz, tz = -f[i].nx;
            if (!layer.lay(draw, f[i], f[i].x + f[i].nx * lateral + tx * along,
                           f[i].z + f[i].nz * lateral + tz * along, b, wide(sd.roundness), zone,
                           zone == 1 ? sd.bar : -1.0f, k, i))
              return fail(error, "ground under a clast (not finite)");
          }
        }
      }
    }
    // ---- the steps' keystones and the cascades' clusters (W90, W91; sized since W97 by the
    // reach's distribution: a step's clasts are about its D84 to D97.7, the keystone of the step
    // its D84 or larger, read in a secondary)
    // -------------------------------------------------------
    auto keystone = [&](Rng& draw, u32 i) {
      const ChannelSediment& sd = link.sediment[i];
      const f64 psi84 = log2_of(wide(sd.d84_m));
      const f64 psi98 = log2_of(wide(sd.d50_m)) + 2 * wide(sd.sorting);
      return std::clamp(exp2_of(psi84 + (psi98 - psi84) * draw.unit()), .2, 2.0);
    };
    for (u32 q = 0; q < link.step_at.size(); ++q) {
      const u32 i = link.step_at[q];
      if (i >= m || reach_at(i) != ChannelReach::StepPool) continue;
      const f64 bed_w = bed_width(p, link.line[i], asym_at(i), swell_at(i));
      Rng draw(hash_combine(link.seed, 0xb0u + i));
      const f64 typical = keystone(draw, i);
      const u32 count = std::max<u32>(1, static_cast<u32>(std::lround(bed_w * k_ba / typical)));
      for (u32 c = 0; c < count; ++c) {
        const f64 b = keystone(draw, i);
        const f64 across = (static_cast<f64>(c) + .5) / static_cast<f64>(count) - .5;
        const f64 lateral = across * bed_w + .2 * b * (2 * draw.unit() - 1);
        const f64 along = .2 * b * (2 * draw.unit() - 1);
        const f64 tx = f[i].nz, tz = -f[i].nx;
        if (!layer.lay(draw, f[i], f[i].x + f[i].nx * lateral + tx * along,
                       f[i].z + f[i].nz * lateral + tz * along, b, wide(link.sediment[i].roundness),
                       4, -1.0f, k, i))
          return fail(error, "ground under a clast (not finite)");
      }
      // **The step's ends** (W93): a clast each side, half in the bank: the rocks that break the
      // bank's line.
      for (const f64 side : {-1.0, 1.0}) {
        const f64 b = keystone(draw, i);
        const f64 lateral = side * (.5 * bed_w + .35 * b / k_ba);
        const f64 along = .3 * b * (2 * draw.unit() - 1);
        const f64 tx = f[i].nz, tz = -f[i].nx;
        if (!layer.lay(draw, f[i], f[i].x + f[i].nx * lateral + tx * along,
                       f[i].z + f[i].nz * lateral + tz * along, b, wide(link.sediment[i].roundness),
                       4, -1.0f, k, i))
          return fail(error, "ground under a clast (not finite)");
      }
    }
    for (u32 q = 0; q < link.cluster_at.size(); ++q) {
      const u32 i = link.cluster_at[q];
      if (i >= m) continue;
      const f64 bed_w = bed_width(p, link.line[i], asym_at(i), swell_at(i));
      Rng draw(hash_combine(link.seed, 0xc1u + i));
      const u32 count = 1 + static_cast<u32>(draw.unit() * 3.0);
      const bool against_bank = draw.unit() < .5;
      for (u32 c = 0; c < count + (against_bank ? 1u : 0u); ++c) {
        const f64 b = keystone(draw, i);
        const bool bank = c == count;
        const f64 lateral = bank ? (draw.unit() < .5 ? -1.0 : 1.0) * (.5 * bed_w + .35 * b / k_ba)
                                 : (draw.unit() - .5) * std::max(0.0, bed_w - .5 * b / k_ba);
        const f64 along = b * (2 * draw.unit() - 1);
        const f64 tx = f[i].nz, tz = -f[i].nx;
        if (!layer.lay(draw, f[i], f[i].x + f[i].nx * lateral + tx * along,
                       f[i].z + f[i].nz * lateral + tz * along, b, wide(link.sediment[i].roundness),
                       5, -1.0f, k, i))
          return fail(error, "ground under a clast (not finite)");
      }
    }
  }
  // ---- one piece a tile, the tiles by z then x, each tile's clasts in the order laid ------------
  Vector<u32> order(all.size());
  Vector<i64> tile_x(all.size()), tile_z(all.size());
  for (u32 c = 0; c < all.size(); ++c) {
    order[c] = c;
    tile_x[c] = floor_div(all[c].x_mm, tile_mm);
    tile_z[c] = floor_div(all[c].z_mm, tile_mm);
  }
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
    if (tile_z[a] != tile_z[b]) return tile_z[a] < tile_z[b];
    if (tile_x[a] != tile_x[b]) return tile_x[a] < tile_x[b];
    return a < b;
  });
  u32 id = 0;
  for (u32 first = 0; first < order.size();) {
    u32 end = first;
    while (end < order.size() && tile_x[order[end]] == tile_x[order[first]] &&
           tile_z[order[end]] == tile_z[order[first]])
      ++end;
    const i64 tx = tile_x[order[first]], tz = tile_z[order[first]];
    if (tx < std::numeric_limits<i32>::min() || tx > std::numeric_limits<i32>::max() ||
        tz < std::numeric_limits<i32>::min() || tz > std::numeric_limits<i32>::max())
      return fail(error, "tile coordinate (out of range)");
    const i64 x0 = tx * tile_mm, z0 = tz * tile_mm;
    i64 y0 = std::numeric_limits<i64>::max();
    for (u32 q = first; q < end; ++q)
      y0 = std::min(y0, all[order[q]].y_mm);
    PlacedPiece piece;
    piece.origin = WorldPos{static_cast<f64>(x0) / 1000.0, static_cast<f64>(y0) / 1000.0,
                            static_cast<f64>(z0) / 1000.0};
    piece.tile = {static_cast<i32>(tx), static_cast<i32>(tz)};
    piece.id = ++id;
    piece.recipe.part = ChannelPart::Clasts;
    piece.recipe.uv_origin = {static_cast<f32>(static_cast<f64>(x0 % 1024000) / 1000.0),
                              static_cast<f32>(static_cast<f64>(z0 % 1024000) / 1000.0)};
    piece.recipe.surface = look;
    piece.recipe.clasts.reserve((end - first) * k_clast_ints);
    for (u32 q = first; q < end; ++q) {
      const Clast& c = all[order[q]];
      const i64 v[k_clast_ints] = {c.x_mm - x0,
                                   c.y_mm - y0,
                                   c.z_mm - z0,
                                   c.ground_mm - y0,
                                   c.a_mm,
                                   c.b_mm,
                                   c.c_mm,
                                   std::llround(c.ax * 10000),
                                   std::llround(c.az * 10000),
                                   std::llround(c.dip * 10000),
                                   std::llround(c.roundness * 1000),
                                   static_cast<i64>(c.seed)};
      for (const i64 x : v)
        piece.recipe.clasts.push_back(narrow(x));
      if (log) log->clasts.push_back(c);
    }
    if (!piece_in_range(piece.recipe, error)) return false;
    out.push_back(std::move(piece));
    first = end;
  }
  if (log)
    for (u32 z = 0; z < k_clast_zones; ++z)
      log->area_m2[z] = area[z];
  return true;
}

bool piece_in_range(const ChannelPiece& piece, std::string& error) {
  const u32 count = clast_count(piece);
  if (piece.clasts.size() % k_clast_ints != 0 || count == 0 || count > k_max_clasts)
    return fail(error, "piece clasts (a whole number of clasts, 1 to 400,000)");
  if (!in(wide(piece.uv_origin[0]), -2048, 2048) || !in(wide(piece.uv_origin[1]), -2048, 2048))
    return fail(error, "piece uv_origin");
  if (!surface_in_range(piece.surface, error)) return false;
  for (u32 k = 0; k < count; ++k) {
    const i32* v = piece.clasts.data() + static_cast<usize>(k) * k_clast_ints;
    const auto within = [](i64 x, i64 lo, i64 hi) { return x >= lo && x <= hi; };
    if (!within(v[0], -1000000, 1000000) || !within(v[2], -1000000, 1000000) ||
        !within(v[1], -10000000, 10000000) || !within(v[3], -10000000, 10000000))
      return fail(error, "clast place (within a kilometre of the piece's origin)");
    if (!(v[6] >= 1 && v[5] >= v[6] && v[4] >= v[5] && v[4] <= 5000))
      return fail(error, "clast axes (a >= b >= c >= 1 mm, a at most 5 m)");
    const i64 len = static_cast<i64>(v[7]) * v[7] + static_cast<i64>(v[8]) * v[8];
    if (len < 98000000 || len > 102000000) return fail(error, "clast direction (a unit vector)");
    if (!within(v[9], -20000, 20000) || !within(v[10], 0, 1000))
      return fail(error, "clast dip (-2 to 2) or roundness (0 to 1)");
  }
  return true;
}

namespace {
// A cube-sphere of `q` quads a face edge: unit directions, outward triangles.
struct Sphere {
  u32 q = 0;
  Vector<f64> dx, dy, dz;  // unit directions
  Vector<u32> tris;
};
Sphere sphere_of(u32 q) {
  Sphere s;
  s.q = q;
  const i32 n = static_cast<i32>(q), side = 2 * n + 1;
  Vector<i32> slot(static_cast<u32>(side * side * side), -1);
  const auto vertex = [&](i32 x, i32 y, i32 z) {
    i32& at = slot[static_cast<u32>(((x + n) * side + (y + n)) * side + (z + n))];
    if (at < 0) {
      at = static_cast<i32>(s.dx.size());
      const f64 fx = x, fy = y, fz = z;
      const f64 l = std::sqrt(fx * fx + fy * fy + fz * fz);
      s.dx.push_back(fx / l);
      s.dy.push_back(fy / l);
      s.dz.push_back(fz / l);
    }
    return static_cast<u32>(at);
  };
  for (u32 axis = 0; axis < 3; ++axis)
    for (const i32 sign : {-1, 1})
      for (i32 u = -n; u < n; u += 2)
        for (i32 v = -n; v < n; v += 2) {
          // Lattice steps of 2 from -q to q: q quads a face edge, every corner on the lattice.
          const auto at = [&](i32 a, i32 b) {
            i32 c[3];
            c[axis] = sign * n;
            c[(axis + 1) % 3] = a;
            c[(axis + 2) % 3] = b;
            return vertex(c[0], c[1], c[2]);
          };
          const u32 i0 = at(u, v), i1 = at(u + 2, v), i2 = at(u + 2, v + 2), i3 = at(u, v + 2);
          // Outward: (i1 - i0) x (i3 - i0) is the face's axis for a positive face (y x z = x,
          // z x x = y, x x y = z), so its triangles run i0 i1 i2, i0 i2 i3; a negative face's the
          // other way round.
          if (sign > 0) {
            for (const u32 x : {i0, i1, i2, i0, i2, i3})
              s.tris.push_back(x);
          } else {
            for (const u32 x : {i0, i3, i2, i0, i2, i1})
              s.tris.push_back(x);
          }
        }
  return s;
}
}  // namespace

bool solve_piece(const ChannelPiece& piece, assets::MeshData& out, std::string& error) {
  out = {};
  if (!piece_in_range(piece, error)) return false;
  g_pieces_solved.fetch_add(1, std::memory_order_relaxed);
  assets::MeshData mesh;
  // **One material a lithology and tone** (W97b, G165; channels.md, "What a stone is made of"): a
  // tile's mesh draws its materials' constants only (ADR-0061; texture residency for a tile's
  // meshes is not built yet), so each stone's colour and roughness are the material's own.
  // An authored `boulders.color` makes every stone that one rock, at `boulders.roughness`.
  const bool authored = piece.surface.color.has_value();
  for (u32 m = 0; m < k_clast_materials; ++m) {
    const ClastLithology& l = k_lithologies[m / k_clast_tones];
    const f32 tone = k_tone[m % k_clast_tones];
    assets::Material material;
    material.name = std::string("channel clasts, ") + l.name;
    material.base_color = {l.albedo[0] * tone, l.albedo[1] * tone, l.albedo[2] * tone, 1.0f};
    material.roughness = l.roughness;
    if (authored) {
      const auto& c = *piece.surface.color;
      material.base_color = {c[0] * tone, c[1] * tone, c[2] * tone, 1.0f};
      material.roughness = piece.surface.roughness;
    }
    material.metallic = 0;
    mesh.materials.push_back(material);
  }
  Vector<u32> by_material[k_clast_materials];
  Vector<u32> own;
  // By the b axis: under 128 mm 2 quads a face edge (48 triangles), under 0.5 m 3 (108), under
  // 1 m 4 (192), else 6 (432); the triangles under the ground are then left out.
  const Sphere spheres[4] = {sphere_of(2), sphere_of(3), sphere_of(4), sphere_of(6)};
  const f32 u0 = piece.uv_origin[0], v0 = piece.uv_origin[1];
  Vector<f64> px, py, pz, radius;
  Vector<i32> remap;
  for (u32 k = 0; k < clast_count(piece); ++k) {
    const Clast c = clast_of(piece, k);
    const f64 A = static_cast<f64>(c.a_mm) / 1000, B = static_cast<f64>(c.b_mm) / 1000,
              C = static_cast<f64>(c.c_mm) / 1000;
    const Sphere& sp = spheres[B < .064 ? 0 : B < .25 ? 1 : B < .5 ? 2 : 3];
    // The clast's frame: a across, b downstream rising by the dip, c = a x b.
    const f64 al = std::sqrt(c.ax * c.ax + c.az * c.az);
    const f64 ax = c.ax / al, az = c.az / al;
    const f64 bl = std::sqrt(1 + c.dip * c.dip);
    const f64 bx = az / bl, by = c.dip / bl, bz = -ax / bl;
    // c = a x b with a = (ax, 0, az): (-az by, az bx - ax bz, ax by).
    const f64 cx = -(az * by), ccy = az * bx - ax * bz, cz = ax * by;
    // Facets: planes cut at 0.78 to 0.92 of the ellipsoid's reach along their normals, blended
    // away by roundness (channels.md, "The clasts").
    Rng draw(hash_combine(0xc1a57ull, c.seed));
    constexpr u32 k_planes = 10;
    f64 nu[k_planes], nv[k_planes], nw[k_planes], h[k_planes];
    for (u32 j = 0; j < k_planes; ++j) {
      f64 a = 0, b = 0, w = 0, l = 0;
      do {
        a = 2 * draw.unit() - 1;
        b = 2 * draw.unit() - 1;
        w = 2 * draw.unit() - 1;
        l = a * a + b * b + w * w;
      } while (l < .04 || l > 1);
      l = std::sqrt(l);
      nu[j] = a / l;
      nv[j] = b / l;
      nw[j] = w / l;
      h[j] = std::sqrt(A * nu[j] * A * nu[j] + B * nv[j] * B * nv[j] + C * nw[j] * C * nw[j]) *
             (.78 + .14 * draw.unit());
    }
    const f64 R = std::clamp(c.roundness, 0.0, 1.0);
    const u32 count = sp.dx.size();
    px.resize(count);
    py.resize(count);
    pz.resize(count);
    const f64 ox = static_cast<f64>(c.x_mm) / 1000, oy = static_cast<f64>(c.y_mm) / 1000,
              oz = static_cast<f64>(c.z_mm) / 1000;
    for (u32 v = 0; v < count; ++v) {
      const f64 du = sp.dx[v], dv = sp.dy[v], dw = sp.dz[v];
      const f64 e = 1 / std::sqrt((du / A) * (du / A) + (dv / B) * (dv / B) + (dw / C) * (dw / C));
      f64 cut = e;
      for (u32 j = 0; j < k_planes; ++j) {
        const f64 d = du * nu[j] + dv * nv[j] + dw * nw[j];
        if (d > 1e-6) cut = std::min(cut, h[j] / d);
      }
      const f64 r = R * e + (1 - R) * cut;
      const f64 lu = r * du, lv = r * dv, lw = r * dw;
      px[v] = ox + lu * ax + lv * bx + lw * cx;
      py[v] = oy + lv * by + lw * ccy;
      pz[v] = oz + lu * az + lv * bz + lw * cz;
    }
    // The triangles wholly under the lowest ground beneath the clast are not drawn.
    const f64 under = static_cast<f64>(c.ground_mm) / 1000 - .02;
    remap.assign(count, -1);
    own.clear();
    for (u32 t = 0; t < sp.tris.size(); t += 3) {
      const u32 a = sp.tris[t], b = sp.tris[t + 1], d = sp.tris[t + 2];
      if (py[a] < under && py[b] < under && py[d] < under) continue;
      for (const u32 v : {a, b, d}) {
        if (remap[v] < 0) {
          remap[v] = static_cast<i32>(mesh.positions.size());
          mesh.positions.push_back(
              {static_cast<f32>(px[v]), static_cast<f32>(py[v]), static_cast<f32>(pz[v])});
          mesh.normals.push_back({0, 0, 0});
          const f64 off_u = static_cast<f64>(c.seed & 1023u) / 128.0;
          const f64 off_v = static_cast<f64>((c.seed >> 10) & 1023u) / 128.0;
          mesh.uvs.push_back({u0 + static_cast<f32>(px[v] + .5 * py[v] + off_u),
                              v0 + static_cast<f32>(pz[v] + .5 * py[v] + off_v)});
        }
        own.push_back(static_cast<u32>(remap[v]));
      }
    }
    // Smooth normals over the clast, area-weighted.
    for (u32 t = 0; t < own.size(); t += 3) {
      const u32 a = own[t], b = own[t + 1], d = own[t + 2];
      const Vec3 normal =
          cross(mesh.positions[b] - mesh.positions[a], mesh.positions[d] - mesh.positions[a]);
      mesh.normals[a] = mesh.normals[a] + normal;
      mesh.normals[b] = mesh.normals[b] + normal;
      mesh.normals[d] = mesh.normals[d] + normal;
    }
    Vector<u32>& into = by_material[clast_material(c.seed)];
    for (const u32 index : own)
      into.push_back(index);
  }
  for (Vec3& normal : mesh.normals)
    normal = length(normal) > 0 ? normalize(normal) : Vec3{0, 1, 0};
  // One primitive a material that holds a clast, in the materials' order.
  for (u32 m = 0; m < k_clast_materials; ++m) {
    if (by_material[m].empty()) continue;
    assets::Primitive part;
    part.first_index = mesh.indices.size();
    part.index_count = by_material[m].size();
    part.material = static_cast<i32>(m);
    for (const u32 index : by_material[m])
      mesh.indices.push_back(index);
    mesh.primitives.push_back(part);
  }
  if (mesh.indices.empty()) return fail(error, "piece (every clast wholly under the ground)");
  out = std::move(mesh);
  return true;
}
u64 pieces_solved() noexcept { return g_pieces_solved.load(std::memory_order_relaxed); }

f32 piece_reach_m(const ChannelPiece& piece) {
  i64 most = 0;
  for (u32 k = 0; k < clast_count(piece); ++k) {
    const i32* v = piece.clasts.data() + static_cast<usize>(k) * k_clast_ints;
    const i64 x = v[0], z = v[2];
    // The centre's distance, rounded up to a millimetre, plus the a axis: an integer square root.
    const i64 d2 = x * x + z * z;
    i64 d = static_cast<i64>(std::sqrt(static_cast<f64>(d2)));
    while (d * d < d2)
      ++d;
    most = std::max(most, d + v[4]);
  }
  return static_cast<f32>(static_cast<f64>(most) / 1000.0);
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

bool build_piece_container(const ChannelPiece& piece, const geometry::ClusterLodOptions& lod,
                           u32 page_bytes, geometry::ClusterFileData& out, std::string& error,
                           const materials::Pack* pack) {
  out = {};
  assets::MeshData mesh;
  if (!solve_piece(piece, mesh, error)) return false;
  geometry::ClusterLodOptions options = lod;
  options.normal_cones = true;
  // **A DAG a material** (W97b): each lithology's stones are their own cluster LOD DAG, merged, so
  // a cluster is one material at every level and the far form keeps the mix.
  const auto attributes = assets::attribute_source(mesh);
  Vector<geometry::ClusterLodMesh> parts;
  Vector<u32> owners;
  for (const assets::Primitive& primitive : mesh.primitives) {
    geometry::ClusterLodMesh part;
    if (!geometry::build_cluster_lod(
            mesh.positions, {mesh.indices.data() + primitive.first_index, primitive.index_count},
            options, part, &error, attributes))
      return false;
    parts.push_back(std::move(part));
  }
  if (!geometry::merge_cluster_lod(parts, out.mesh, &owners, &error)) return false;
  for (u32& owner : owners)
    owner = static_cast<u32>(mesh.primitives[owner].material);
  if (page_bytes > 0) {
    geometry::ClusterPagesOptions pages;
    pages.page_bytes = page_bytes;
    Vector<u32> order;
    if (!geometry::build_cluster_pages(out.mesh, pages, out.pages, &error, &order)) return false;
    geometry::permute_cluster_array(order, owners);
  }
  out.cluster_material = std::move(owners);
  // **Constants only** (ADR-0061; scene_gen.md, "A tile's meshes"): a mesh a tile hands draws its
  // materials' constants until texture residency for such meshes lands, so no set is applied and
  // no image named; the pack still keys the build, as every generated mesh's does.
  for (const assets::Material& m : mesh.materials) {
    geometry::ClusterFileMaterial material;
    material.base_color = m.base_color;
    material.metallic = 0;
    material.roughness = m.roughness;
    out.materials.push_back(material);
  }
  out.source_hash = piece_source_hash(piece);
  out.build_key = assets::generated_mesh_build_key(
      out.source_hash, assets::MeshGeneratorInputs{pack}, lod, page_bytes);
  out.generator = "channel";
  out.generator_kind = "channel clasts";
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

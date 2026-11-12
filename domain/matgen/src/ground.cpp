// The ground kind (matgen.md, "Ground sets"; generated-material-sets.md, "A floor" and "A floor
// at standing height"): an underlay of moss and soil split at the moss share's quantile — moss in
// cushions, soil in clods with stones part buried in it — litter over it (old and dark below,
// fresh straw over it, lying with the lodged grass), live leaves in tufts over that, and the whole
// floor lifted on low mounds. A mosaic of patches a few decimetres across sets the tone: a grazed
// sward's short lawns against its taller rejected tufts, a moss carpet's wet and dry, a soil's
// moist and dry, litter fresh and decayed. The parts are the sward's floor bake's (flora.md, "The
// ground under the sward (G141)"), here as sets a pack role can name.
#include "field.h"

#include <algorithm>

namespace engine::matgen::detail {

f64 lay_fragment(Field& field, const Fragment& f, Vector<f32>* cover) {
  const i64 size = static_cast<i64>(field.n);
  const f64 texel = field.texel_m;
  const i64 reach = static_cast<i64>((f.half_m + f.width_m) / texel) + 2;
  const i64 tcx = f.cx / k_sub, tcy = f.cy / k_sub;
  // A row touches only the texels within `band` of the leaf's centre line (further ones have no
  // coverage): the stretch of the line that crosses the band of rows `band` either side of this
  // one, widened by `band`. A leaf is a few texels wide and a hundred long, so walking its whole
  // bounding square would visit a hundred texels for each one it covers. The bound is wider than
  // the leaf, so what is skipped was never written.
  const f64 band = 0.5 * f.width_m + texel;
  const f64 start = static_cast<f64>(f.cx % k_sub) / static_cast<f64>(k_sub);
  f64 added = 0.0;
  for (i64 ty = tcy - reach; ty <= tcy + reach; ++ty) {
    const f64 ly = offset_texels(ty, f.cy, size) * texel;
    const u32 sy = field.stored(ty, field.window.y);
    f64 xa = -f.half_m, xb = f.half_m;
    if (std::fabs(f.dir_y) > 1.0e-9) {
      f64 ta = (ly - band) / f.dir_y, tb = (ly + band) / f.dir_y;
      if (ta > tb) std::swap(ta, tb);
      ta = std::max(ta, -f.half_m);
      tb = std::min(tb, f.half_m);
      if (ta > tb) continue;
      xa = ta * f.dir_x;
      xb = tb * f.dir_x;
      if (xa > xb) std::swap(xa, xb);
    } else if (std::fabs(ly) > band) {
      continue;
    }
    const i64 ka =
        std::max(-reach, static_cast<i64>(std::floor((xa - band) / texel - 0.5 + start)) - 1);
    const i64 kb =
        std::min(reach, static_cast<i64>(std::floor((xb + band) / texel - 0.5 + start)) + 2);
    for (i64 tx = tcx + ka; tx <= tcx + kb; ++tx) {
      const f64 lx = offset_texels(tx, f.cx, size) * texel;
      const f64 along = std::clamp(lx * f.dir_x + ly * f.dir_y, -f.half_m, f.half_m);
      const f64 ex = lx - along * f.dir_x, ey = ly - along * f.dir_y;
      const f64 d = std::sqrt(ex * ex + ey * ey);
      const f64 s = std::fabs(along) / f.half_m;
      const f64 width = f.width_m * (f.taper ? 1.0 - s * s * s : 1.0);
      // The overlap of the leaf's width with the texel's, so a leaf thinner than a texel keeps
      // its area.
      const f64 a = clamp01(
          (std::min(d + 0.5 * texel, 0.5 * width) - std::max(d - 0.5 * texel, -0.5 * width)) /
          texel);
      if (a <= 0.0) continue;
      const u32 i = field.index(field.stored(tx, field.window.x), sy);
      Texel& t = field.texels[i];
      const f64 rib = 1.0 + f.midrib * (1.0 - std::min(1.0, d / (0.15 * width + 0.5 * texel)));
      // A leaf rising from its base: its far end lies higher, and catches more light.
      const f64 rise = f.rise * (0.5 + 0.5 * (lx * f.dir_x + ly * f.dir_y) / f.half_m);
      const f64 lit = 1.0 + rise;
      t.r = static_cast<f32>(mix(static_cast<f64>(t.r), f.colour[0] * rib * lit, a));
      t.g = static_cast<f32>(mix(static_cast<f64>(t.g), f.colour[1] * rib * lit, a));
      t.b = static_cast<f32>(mix(static_cast<f64>(t.b), f.colour[2] * rib * lit, a));
      t.rough = static_cast<f32>(mix(static_cast<f64>(t.rough), f.rough, a));
      const f64 was = static_cast<f64>(t.height);
      t.height = static_cast<f32>(std::max(was, std::min(f.lift_max_m, was + f.lift_m * a)));
      if (cover != nullptr) {
        f32& c = (*cover)[i];
        added += (1.0 - static_cast<f64>(c)) * a;
        c = static_cast<f32>(1.0 - (1.0 - static_cast<f64>(c)) * (1.0 - a));
      }
    }
  }
  return added;
}

constexpr u32 k_bins = 4096;

f64 quantile(const Vector<f32>& values, f64 share) {
  Vector<u32> bins;
  bins.assign(k_bins, 0u);
  for (const f32 v : values)
    ++bins[std::min(k_bins - 1, static_cast<u32>(std::max(0.0, static_cast<f64>(v)) * k_bins))];
  const f64 want = share * static_cast<f64>(values.size());
  f64 below = 0.0;
  for (u32 b = 0; b < k_bins; ++b) {
    below += static_cast<f64>(bins[b]);
    if (below >= want) return (static_cast<f64>(b) + 1.0) / static_cast<f64>(k_bins);
  }
  return 1.0;
}

namespace {

// Linear albedos (generated-material-sets.md, "A floor" and "A floor at standing height"):
// standing litter pale and rising to the red, decayed litter dark, a bryophyte's shaded body and
// its lit tips, a dry moss's brown, a moist organic loam, a grazed lawn's yellow-green leaf and a
// rejected tuft's dark blue-green one, a mountain soil's grey stones.
constexpr f64 k_straw[3] = {0.18, 0.145, 0.09};
constexpr f64 k_grey_straw[3] = {0.14, 0.13, 0.115};
constexpr f64 k_decayed[3] = {0.065, 0.050, 0.036};
constexpr f64 k_moss[3] = {0.045, 0.075, 0.025};
constexpr f64 k_moss_lit[3] = {0.085, 0.115, 0.030};
constexpr f64 k_moss_dry[3] = {0.080, 0.065, 0.035};
constexpr f64 k_loam[3] = {0.085, 0.065, 0.045};
constexpr f64 k_leaf[3] = {0.060, 0.125, 0.032};
constexpr f64 k_leaf_short[3] = {0.095, 0.150, 0.034};
constexpr f64 k_leaf_tall[3] = {0.035, 0.085, 0.035};
constexpr f64 k_stone[3] = {0.21, 0.20, 0.185};

// The cellular field at texel (vx, vy) for period `k`, its lookup warped by half a cell by a
// periodic noise of the same period: outlines that wander instead of a polygon's straight edges.
Cell warped_cell(u64 seed, u32 vx, u32 vy, u32 n, u32 k) noexcept {
  const f64 x = lattice_of(vx, n, k), y = lattice_of(vy, n, k);
  const f64 wx = lattice_noise(seed ^ 0x7778ull, x, y, k) - 0.5;
  const f64 wy = lattice_noise(seed ^ 0x7779ull, x, y, k) - 0.5;
  return cell_noise(seed, x + wx, y + wy, k);
}

// A moss carpet's cushions at texel (vx, vy) for period `k` (G187, version 7): one cushion a cell,
// its centre jittered and its radius 0.7 to 0.98 of a cell, so neighbours overlap. Each is a
// smooth bump (1 - (d / r)^2)^2, and they are joined softly (1 - the product of 1 - bump), so two
// cushions merge where they meet instead of meeting at an edge; a cushion's hue and vigour are
// blended by the bumps' weights, so its tone falls off across it into its neighbours' rather
// than stepping at a cell's boundary (version 6's one tone a Voronoi cell read as a camouflage
// print). `crown` is 1 on a cushion's top and 0 in the hollows between.
struct Cushions {
  f64 crown = 0;
  f64 hue = 0.5;
  f64 vigour = 0.5;
};
Cushions cushions(u64 seed, u32 vx, u32 vy, u32 n, u32 k) noexcept {
  const f64 x0 = lattice_of(vx, n, k), y0 = lattice_of(vy, n, k);
  // Warped by a quarter of a cell: outlines that wander.
  const f64 x = x0 + 0.5 * (lattice_noise(seed ^ 0x7778ull, x0, y0, k) - 0.5);
  const f64 y = y0 + 0.5 * (lattice_noise(seed ^ 0x7779ull, x0, y0, k) - 0.5);
  const f64 fx = std::floor(x), fy = std::floor(y);
  const i64 ix = static_cast<i64>(fx), iy = static_cast<i64>(fy);
  const f64 tx = x - fx, ty = y - fy;
  Cushions out;
  f64 open = 1.0, weight = 0.0, hue = 0.0, vigour = 0.0;
  for (i64 j = -1; j <= 1; ++j) {
    for (i64 i = -1; i <= 1; ++i) {
      const u64 h = hash3(seed, wrap_node(ix + i, k), wrap_node(iy + j, k));
      const f64 dx = (static_cast<f64>(i) + 0.15 + 0.7 * unit_of(h)) - tx;
      const f64 dy = (static_cast<f64>(j) + 0.15 + 0.7 * unit_of(mix64(h))) - ty;
      const f64 r = 0.7 + 0.28 * unit_of(mix64(h ^ 0x72ull));
      const f64 s = (dx * dx + dy * dy) / (r * r);
      if (s >= 1.0) continue;
      const f64 bump = (1.0 - s) * (1.0 - s);
      open *= 1.0 - bump;
      weight += bump;
      hue += bump * unit_of(mix64(h ^ 0x687565ull));
      vigour += bump * unit_of(mix64(h ^ 0x627269ull));
    }
  }
  out.crown = 1.0 - open;
  if (weight > 1.0e-12) {
    out.hue = hue / weight;
    out.vigour = vigour / weight;
  }
  return out;
}

// A dome over a cell of a cellular field: 1 at its point, 0 at its rim, the seam between cells low.
f64 dome(const Cell& c) noexcept {
  const f64 rim = clamp01((c.f2 - c.f1) / 0.35);
  return clamp01(1.0 - c.f1 * c.f1) * smooth01(rim);
}

}  // namespace

void build_ground(const Set& set, Field& field, SetStats& stats, Shade& shade) {
  const Ground& p = set.ground;
  const u32 n = field.n;
  const u32 count = n * n;
  const f64 texel = field.texel_m;
  const f64 tile = texel * static_cast<f64>(n);
  const u32 half = n / 2;
  const auto periods = [&](f64 size_m, u32 lo) {
    return std::clamp(static_cast<u32>(tile / size_m + 0.5), lo, half);
  };
  const u32 broad_k = periods(1.0 / 12.0, 2);
  const u32 mid_k = periods(1.0 / 40.0, 4);
  const u32 tip_k = periods(0.0018, 4);
  const u32 crumb_k = periods(0.005, 4);
  const u32 patch_k = periods(0.6, 1);
  const u32 mound_k = periods(std::max(0.05, static_cast<f64>(p.mound_spacing_mm) * 0.001), 1);
  const f64 patchiness = permille(p.patch_permille);
  const f64 relief = static_cast<f64>(p.relief_um) * 1.0e-6;
  const f64 mound_m = static_cast<f64>(p.mound_um) * 1.0e-6;

  // ---- the broad fields: the patch mosaic (a few decimetres across) and the mounds
  Vector<f32> patch, mound;
  patch.resize(count);
  mound.resize(count);
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x) {
      const u32 vx = field.vx(x), vy = field.vy(y);
      const u32 i = field.index(x, y);
      patch[i] = static_cast<f32>(
          0.65 * lattice_noise(set.seed ^ 0x7061746368ull, lattice_of(vx, n, patch_k),
                               lattice_of(vy, n, patch_k), patch_k) +
          0.35 * lattice_noise(set.seed ^ 0x7061746369ull, lattice_of(vx, n, patch_k * 3),
                               lattice_of(vy, n, patch_k * 3), patch_k * 3));
      mound[i] = static_cast<f32>(fractal_noise(set.seed ^ 0x6d6f756e64ull, vx, vy, n, mound_k, 2));
    }
  }
  // Where the patch field sits on the torus, for a fragment or a tuft placed by sub-texels.
  const auto patch_at = [&](i64 cx, i64 cy) -> f64 {
    const u32 x = field.stored(cx / k_sub, field.window.x);
    const u32 y = field.stored(cy / k_sub, field.window.y);
    return static_cast<f64>(patch[field.index(x, y)]);
  };

  // ---- the underlay: moss where a two-octave field is lowest, exactly its share
  Vector<f32> under;
  under.resize(count);
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x) {
      const u32 vx = field.vx(x), vy = field.vy(y);
      under[field.index(x, y)] = static_cast<f32>(
          0.7 * lattice_noise(set.seed ^ 0x756e646572ull, lattice_of(vx, n, broad_k),
                              lattice_of(vy, n, broad_k), broad_k) +
          0.3 * lattice_noise(set.seed ^ 0x66696e65ull, lattice_of(vx, n, mid_k),
                              lattice_of(vy, n, mid_k), mid_k));
    }
  }
  const f64 parts = static_cast<f64>(p.moss_permille) + static_cast<f64>(p.soil_permille);
  const f64 q = parts > 0.0 ? static_cast<f64>(p.moss_permille) / parts : 0.0;
  const f64 moss_at = quantile(under, q);
  const u32 cushion_k = p.cushion_mm > 0 ? periods(static_cast<f64>(p.cushion_mm) * 0.001, 2) : 0;
  const u32 clod_k = p.clod_mm > 0 ? periods(static_cast<f64>(p.clod_mm) * 0.001, 2) : 0;
  f64 moss_sum = 0.0;
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x) {
      const u32 i = field.index(x, y);
      const u32 vx = field.vx(x), vy = field.vy(y);
      const f64 value = static_cast<f64>(under[i]);
      const f64 moss =
          q <= 0.0 ? 0.0 : (q >= 1.0 ? 1.0 : 1.0 - smooth01((value - moss_at) / 0.04 + 0.5));
      moss_sum += moss;
      const f64 pv = static_cast<f64>(patch[i]);
      // Wet where the patch field is high and in the mounds' hollows: darker (a soil's
      // reflectance falls with its water content; Lobell and Asner 2002).
      const f64 moist =
          clamp01(patchiness * (pv - 0.5) * 2.0 + 0.5 * (0.5 - static_cast<f64>(mound[i])));
      const f64 grain =
          unit_of(hash3(set.seed ^ 0x677261696eull, wrap_node(vx, n), wrap_node(vy, n)));
      // Moss: shoot tips paler than the shade between them, in cushions whose tone is each one's
      // own — yellow-green where lit and growing, dark green where wet, brown where dry.
      const Cell tip = cell_noise(set.seed ^ 0x746970ull, lattice_of(vx, n, tip_k),
                                  lattice_of(vy, n, tip_k), tip_k);
      const f64 lit = clamp01(1.0 - 1.5 * tip.f1);
      // Cushions: a crown lighter and yellower, its margins and the hollows between darker and
      // wetter, the tone blended across neighbours (`cushions`, above); without cushions, the
      // carpet is all crown.
      f64 cushion_h = 0.0, crown = 1.0, cushion_tone = 0.5, vigour = 0.5;
      if (cushion_k > 0) {
        const Cushions c = cushions(set.seed ^ 0x637573686eull, vx, vy, n, cushion_k);
        crown = c.crown;
        cushion_tone = c.hue;
        vigour = c.vigour;
        // The carpet's relief: the cushions' smooth domes over low hummocks (a dome that fell to
        // a crease at a cell's seam drew crazy paving under a low sun, version 5).
        cushion_h = relief * (0.65 * crown + 0.35 * fractal_noise(set.seed ^ 0x68756d6dull, vx, vy,
                                                                  n, cushion_k, 2));
      }
      // A carpet with no cushions (a sward's underlay) keeps the flat tone it had.
      const bool cushioned = cushion_k > 0;
      const f64 hollow = 1.0 - crown;
      const f64 dry = cushioned ? clamp01(patchiness * (0.5 - moist) * 1.0 +
                                          0.25 * (cushion_tone - 0.5) - 0.3 * hollow)
                                : clamp01(patchiness * (0.5 - moist) * 1.6);
      const f64 green = cushioned ? clamp01(0.3 + 0.6 * crown + 0.5 * (cushion_tone - 0.5)) : 0.5;
      const f64 body_shade =
          cushioned ? mix(0.72, 1.08, smooth01(crown)) * (0.85 + 0.3 * vigour) : 1.0;
      f64 moss_rgb[3];
      for (u32 c = 0; c < 3; ++c) {
        const f64 body = mix(k_moss[c], k_moss_lit[c], green * lit);
        moss_rgb[c] = mix(body, k_moss_dry[c], dry) * (0.6 + 0.8 * lit) * body_shade;
      }
      const f64 moss_h = 0.008 * clamp01((moss_at - value) * 6.0 + 0.3) + 0.0012 * lit + cushion_h;
      // Soil: crumbs a few millimetres across, dark between them, a pale grain here and there,
      // gathered into clods with cracks between them.
      const Cell crumb = cell_noise(set.seed ^ 0x6372756d62ull, lattice_of(vx, n, crumb_k),
                                    lattice_of(vy, n, crumb_k), crumb_k);
      const f64 gap = clamp01((crumb.f2 - crumb.f1) / 0.18);
      const bool pale = unit_of(mix64(crumb.id ^ 0x70ull)) < 0.01;
      f64 clod_h = 0.0, crack = 0.0, clod_tone = 1.0;
      if (clod_k > 0) {
        const Cell c = warped_cell(set.seed ^ 0x636c6f64ull, vx, vy, n, clod_k);
        const f64 d = dome(c);
        clod_h = relief * d * (0.5 + 0.5 * unit_of(c.id));
        // A clod parts from its neighbour along some of its edge, not all of it.
        const f64 opens =
            smooth01((lattice_noise(set.seed ^ 0x6f70656eull, lattice_of(vx, n, clod_k * 2),
                                    lattice_of(vy, n, clod_k * 2), clod_k * 2) -
                      0.45) /
                     0.25);
        crack = (1.0 - clamp01((c.f2 - c.f1) / 0.06)) * opens;
        clod_tone = 0.85 + 0.3 * unit_of(mix64(c.id));
      }
      const f64 soil_tone = (0.72 + 0.5 * unit_of(crumb.id)) * (0.6 + 0.4 * gap) *
                            (0.92 + 0.16 * grain) * clod_tone * (1.0 - 0.6 * crack) *
                            (1.0 - 0.45 * moist);
      const f64 soil_h =
          0.002 * gap * (1.0 - crumb.f1) + 0.0006 * grain + clod_h - 0.4 * relief * crack;
      Texel& t = field.texels[i];
      const f64 soil_rgb[3] = {pale ? 0.18 : k_loam[0] * soil_tone,
                               pale ? 0.17 : k_loam[1] * soil_tone,
                               pale ? 0.15 : k_loam[2] * soil_tone};
      t.r = static_cast<f32>(mix(soil_rgb[0], moss_rgb[0], moss));
      t.g = static_cast<f32>(mix(soil_rgb[1], moss_rgb[1], moss));
      t.b = static_cast<f32>(mix(soil_rgb[2], moss_rgb[2], moss));
      t.rough = static_cast<f32>(mix(mix(0.92, 0.82, moist * 0.5), 0.95, moss));
      t.height = static_cast<f32>(mix(soil_h, moss_h, moss));
    }
  }

  const u64 period = static_cast<u64>(n) * static_cast<u64>(k_sub);
  // ---- stones part buried in the soil: ellipses of a few centimetres, their tops above it
  if (p.stones_permille > 0 && p.soil_permille > 0) {
    const f64 r_lo = static_cast<f64>(p.stone_min_mm) * 0.0005;
    const f64 r_hi = static_cast<f64>(p.stone_max_mm) * 0.0005;
    // Radii log-uniform (many small stones, few large); the mean area of an ellipse whose minor
    // axis is 0.55 to 1 of its major: pi E[a^2] 0.775, E[a^2] = (hi^2 - lo^2) / (2 ln(hi / lo)).
    const f64 span = det::log(r_hi / r_lo);
    const f64 mean_area =
        k_pi * 0.775 * (span > 1.0e-9 ? (r_hi * r_hi - r_lo * r_lo) / (2.0 * span) : r_lo * r_lo);
    const f64 s = std::min(permille(p.stones_permille), 0.9) * (1.0 - q);
    const u32 stones = static_cast<u32>(-det::log(1.0 - s) * tile * tile / mean_area + 0.5);
    Rng rng(set.seed ^ 0x73746f6e65ull);
    const i64 size = static_cast<i64>(n);
    for (u32 k = 0; k < stones; ++k) {
      const i64 cx = static_cast<i64>(rng.next() % period);
      const i64 cy = static_cast<i64>(rng.next() % period);
      const f64 a = r_lo * det::exp(rng.unit() * span), b = a * rng.range(0.55, 1.0);
      const f64 angle = rng.unit() * k_pi;
      const f64 ca = det::cos(angle), sa = det::sin(angle);
      const f64 tone = rng.range(0.55, 1.2);
      // Some stones the soil's own colour more than rock's: a film of it, or a browner rock.
      const f64 dirt = rng.range(0.1, 0.55);
      const f64 top = rng.range(0.25, 0.6) * b;  // above the soil: the rest is buried
      const i64 reach = static_cast<i64>(a / texel) + 2;
      const i64 tcx = cx / k_sub, tcy = cy / k_sub;
      for (i64 ty = tcy - reach; ty <= tcy + reach; ++ty) {
        const f64 ly = offset_texels(ty, cy, size) * texel;
        const u32 sy = field.stored(ty, field.window.y);
        for (i64 tx = tcx - reach; tx <= tcx + reach; ++tx) {
          const f64 lx = offset_texels(tx, cx, size) * texel;
          const f64 u = (lx * ca + ly * sa) / a, v = (ly * ca - lx * sa) / b;
          const f64 r2 = u * u + v * v;
          const f64 cov = clamp01((1.0 - std::sqrt(r2)) * b / texel + 0.5);
          if (cov <= 0.0) continue;
          const u32 i = field.index(field.stored(tx, field.window.x), sy);
          Texel& t = field.texels[i];
          const f64 crown = std::sqrt(std::max(0.0, 1.0 - r2));
          const f64 shade_rim = 0.7 + 0.3 * crown;
          // The soil's film thickest at the stone's foot.
          const f64 film = dirt + (1.0 - dirt) * 0.6 * (1.0 - crown);
          const f64 speck = 0.9 + 0.2 * unit_of(hash3(set.seed ^ k, static_cast<u64>(tx + size),
                                                      static_cast<u64>(ty + size)));
          t.r = static_cast<f32>(mix(static_cast<f64>(t.r),
                                     mix(k_stone[0] * tone, k_loam[0], film) * shade_rim * speck,
                                     cov));
          t.g = static_cast<f32>(mix(static_cast<f64>(t.g),
                                     mix(k_stone[1] * tone, k_loam[1], film) * shade_rim * speck,
                                     cov));
          t.b = static_cast<f32>(mix(static_cast<f64>(t.b),
                                     mix(k_stone[2] * tone, k_loam[2], film) * shade_rim * speck,
                                     cov));
          t.rough = static_cast<f32>(mix(static_cast<f64>(t.rough), 0.72, cov));
          t.height = static_cast<f32>(std::max(static_cast<f64>(t.height), top * crown * cov));
        }
      }
    }
  }

  // ---- litter and leaves
  const f64 len_lo = static_cast<f64>(p.leaf_min_mm) * 0.001;
  const f64 len_hi = static_cast<f64>(p.leaf_max_mm) * 0.001;
  const f64 wid_lo = static_cast<f64>(p.leaf_width_min_um) * 1.0e-6;
  const f64 wid_hi = static_cast<f64>(p.leaf_width_max_um) * 1.0e-6;
  const f64 mean_area = 0.5 * (len_lo + len_hi) * 0.5 * (wid_lo + wid_hi);
  const f64 texels_f = static_cast<f64>(count);
  const f64 aligned = permille(p.aligned_permille);
  const u32 flow_k = periods(0.8, 1);
  const u32 decay_k = periods(0.22, 2);
  Vector<f32> thatch_cover, live_cover;
  thatch_cover.assign(count, 0.0f);
  live_cover.assign(count, 0.0f);
  // A litter fragment lies with the lodged grass round it where the litter is aligned: the flow's
  // direction is a field a few decimetres across, so the straw lies in swirls.
  const auto litter = [&](Rng& rng, Fragment& f, bool decayed) {
    f.cx = static_cast<i64>(rng.next() % period);
    f.cy = static_cast<i64>(rng.next() % period);
    const f64 own = rng.unit() * k_pi;
    const f64 flow =
        k_pi * lattice_noise(set.seed ^ 0x666c6f77ull,
                             (static_cast<f64>(f.cx) / static_cast<f64>(k_sub) + 0.5) * flow_k / n,
                             (static_cast<f64>(f.cy) / static_cast<f64>(k_sub) + 0.5) * flow_k / n,
                             flow_k);
    const f64 angle =
        aligned > 0.0 && rng.unit() < aligned ? flow + 0.35 * (rng.unit() - 0.5) : own;
    f.dir_x = det::cos(angle);
    f.dir_y = det::sin(angle);
    f.half_m = 0.5 * rng.range(len_lo, len_hi);
    f.width_m = rng.range(wid_lo, wid_hi);
    const f64 tone = rng.range(0.6, 1.3);
    const bool grey = !decayed && rng.unit() < 0.2;
    const f64* base = decayed ? k_decayed : (grey ? k_grey_straw : k_straw);
    for (u32 c = 0; c < 3; ++c)
      f.colour[c] = base[c] * tone;
    f.rough = 0.80 + 0.06 * (rng.unit() - 0.5);
    f.lift_m = 0.0008;
    f.lift_max_m = decayed ? 0.008 : 0.015;
    f.taper = false;
    f.midrib = 0.0;
    f.rise = 0.0;
  };
  // Lays fragments until the layer's cover reaches `share` (or a cap of four times the Poisson
  // count, which a share under 0.98 never meets): the share asked is the share drawn whatever the
  // fragments' clustering, which a count from the Poisson law would be only for scattered ones.
  const auto poisson_count = [&](f64 share, f64 each) {
    return static_cast<u32>(-det::log(1.0 - std::min(share, 0.98)) * tile * tile / each + 0.5);
  };
  const f64 thatch_share = std::min(permille(p.thatch_permille), 0.98);
  if (thatch_share > 0.0) {
    const f64 decay = permille(p.decay_permille);
    f64 covered = 0.0;
    Rng rng(set.seed ^ 0x746861746368ull);
    // Old litter first, below: the decayed share of the layer.
    const u32 cap = 4 * poisson_count(thatch_share, mean_area) + 16;
    for (u32 k = 0; k < cap && covered < decay * thatch_share * texels_f; ++k) {
      Fragment f;
      litter(rng, f, true);
      covered += lay_fragment(field, f, &thatch_cover);
    }
    // Fresh straw over it, where the patch field lets it lie: the mosaic of fresh and decayed.
    for (u32 k = 0; k < cap && covered < thatch_share * texels_f; ++k) {
      Fragment f;
      litter(rng, f, false);
      // Where the fresh straw lies: a mosaic of its own, a decimetre or two across, so a tile holds
      // many decayed patches rather than one.
      const f64 litter_patch = lattice_noise(
          set.seed ^ 0x64656361ull,
          (static_cast<f64>(f.cx) / static_cast<f64>(k_sub) + 0.5) * decay_k / n,
          (static_cast<f64>(f.cy) / static_cast<f64>(k_sub) + 0.5) * decay_k / n, decay_k);
      const f64 keep = clamp01(1.0 - patchiness * clamp01((litter_patch - 0.4) / 0.4));
      if (rng.unit() > keep) continue;
      covered += lay_fragment(field, f, &thatch_cover);
    }
  }
  const f64 live_share = std::min(permille(p.live_permille), 0.98);
  if (live_share > 0.0) {
    const f64 dead = permille(p.dead_permille);
    const f64 tuft_m = static_cast<f64>(p.tuft_mm) * 0.001;
    const u32 cap = 4 * poisson_count(live_share, mean_area * 0.75) + 16;
    Rng rng(set.seed ^ 0x6c697665ull);
    f64 covered = 0.0;
    u32 laid = 0;
    while (laid < cap && covered < live_share * texels_f) {
      // A tuft: its leaves fan out from a centre (a tiller's), long and dark in a tall rejected
      // tuft, short and yellow-green in a grazed lawn; without tufts, one leaf anywhere.
      const i64 cx = static_cast<i64>(rng.next() % period);
      const i64 cy = static_cast<i64>(rng.next() % period);
      const f64 tall =
          tuft_m > 0.0 ? clamp01(0.5 + patchiness * (patch_at(cx, cy) - 0.5) * 2.0) : 0.5;
      const u32 leaves = tuft_m > 0.0 ? 6 + static_cast<u32>(rng.unit() * 14.0) : 1;
      const f64 spread = tuft_m * 0.18;
      for (u32 l = 0; l < leaves && laid < cap; ++l, ++laid) {
        Fragment f;
        const f64 angle = rng.unit() * 2.0 * k_pi;
        f.dir_x = det::cos(angle);
        f.dir_y = det::sin(angle);
        f.half_m = 0.5 * rng.range(len_lo, len_hi) * mix(0.7, 1.35, tall);
        f.width_m = rng.range(wid_lo, wid_hi);
        // Its base near the tuft's centre, its blade out along its direction.
        const f64 jx = spread * rng.normal(), jy = spread * rng.normal();
        const f64 out = tuft_m > 0.0 ? 0.85 * f.half_m : 0.0;
        f.cx = cx + static_cast<i64>(std::floor((jx + out * f.dir_x) / texel * k_sub + 0.5));
        f.cy = cy + static_cast<i64>(std::floor((jy + out * f.dir_y) / texel * k_sub + 0.5));
        f.cx = static_cast<i64>(wrap_node(f.cx, static_cast<u32>(period)));
        f.cy = static_cast<i64>(wrap_node(f.cy, static_cast<u32>(period)));
        const bool is_dead = rng.unit() < dead * mix(0.5, 1.6, tall);
        const f64 tone = rng.range(0.75, 1.25);
        if (is_dead) {
          const f64* base = rng.unit() < 0.3 ? k_grey_straw : k_straw;
          for (u32 c = 0; c < 3; ++c)
            f.colour[c] = base[c] * tone;
          f.rough = 0.80;
          f.midrib = 0.0;
        } else {
          const f64 warm = rng.range(0.9, 1.25);
          for (u32 c = 0; c < 3; ++c) {
            const f64 leaf = tuft_m > 0.0 ? mix(k_leaf_short[c], k_leaf_tall[c], tall) : k_leaf[c];
            f.colour[c] = leaf * tone * (c == 0 ? warm : 1.0);
          }
          f.rough = 0.55 + 0.1 * rng.unit();
          f.midrib = 0.25;
        }
        f.lift_m = 0.0012;
        f.lift_max_m = tuft_m > 0.0 ? mix(0.015, 0.04, tall) : 0.025;
        f.taper = true;
        f.rise = tuft_m > 0.0 ? 0.25 : 0.0;
        covered += lay_fragment(field, f, &live_cover);
      }
    }
  }

  // ---- the mounds under everything
  if (mound_m > 0.0) {
    for (u32 i = 0; i < count; ++i)
      field.texels[i].height = static_cast<f32>(static_cast<f64>(field.texels[i].height) +
                                                mound_m * 2.0 * (static_cast<f64>(mound[i]) - 0.5));
  }

  f64 thatch_sum = 0.0, live_sum = 0.0;
  for (u32 i = 0; i < count; ++i) {
    thatch_sum += static_cast<f64>(thatch_cover[i]);
    live_sum += static_cast<f64>(live_cover[i]);
  }
  stats.moss_share = moss_sum / texels_f;
  stats.thatch_cover = thatch_sum / texels_f;
  stats.live_cover = live_sum / texels_f;
  // A hollow is in full shade at a centimetre under its surroundings (a tuft's or a cushion's
  // seam), read over a couple of centimetres round it.
  shade.cavity_m = p.tuft_mm > 0 || p.cushion_mm > 0 ? 0.010 : 0.005;
  shade.cavity_px = std::clamp(static_cast<u32>(0.02 / texel + 0.5), 2u, std::max(2u, n / 8));
}

}  // namespace engine::matgen::detail

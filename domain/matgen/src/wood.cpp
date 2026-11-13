// The wood kind (matgen.md, "Wood sets"; generated-material-sets.md, "Weathered wood"): a face of
// softwood with its grain along the image's u, as a sawn board shows it or as the outside of a
// log does, weathered toward grey. The growth rings are a phase field across the tile (bands of
// earlywood and latewood) that wanders along the grain, so flat-sawn figure rises into arches
// where the face runs nearly along a ring; knots deflect it; checks open along it; a log keeps
// patches of fissured bark.
//
// Every coordinate is the torus texel (the window reduced first), so a build from another start is
// the same picture rotated, to the byte (matgen.md, "A set tiles by construction").
#include "field.h"

#include <algorithm>

namespace engine::matgen::detail {

namespace {

// Linear albedos (generated-material-sets.md, "Weathered wood"): the dark of a sound knot, a
// bark's outer plates and the red-brown in its fissures.
constexpr f64 k_knot_fresh[3] = {0.20, 0.10, 0.045};
constexpr f64 k_knot_grey[3] = {0.075, 0.068, 0.060};
constexpr f64 k_bark[3] = {0.085, 0.070, 0.058};
constexpr f64 k_bark_fissure[3] = {0.048, 0.026, 0.016};

// The latewood a box of width `g` bands centred on phase `p` holds, as a share of the box: the
// band indicator 1 on [1 - lw, 1) of every band, filtered over the texel's footprint so a band
// narrower than a texel keeps its share instead of aliasing.
f64 latewood_integral(f64 t, f64 lw) noexcept {
  const f64 band = std::floor(t);
  return band * lw + std::clamp(t - band - (1.0 - lw), 0.0, lw);
}
f64 latewood_share(f64 p, f64 g, f64 lw) noexcept {
  const f64 w = std::max(g, 1.0e-6);
  return clamp01((latewood_integral(p + 0.5 * w, lw) - latewood_integral(p - 0.5 * w, lw)) / w);
}

}  // namespace

void build_wood(const Set& set, Field& field, SetStats& stats, Shade& shade) {
  const Wood& p = set.wood;
  const bool is_log = p.form == WoodForm::log;
  const u32 n = field.n;
  const f64 texel = field.texel_m;
  const f64 tile = texel * static_cast<f64>(n);
  const u32 count = n * n;
  const f64 figure_m = static_cast<f64>(p.figure_um) * 1.0e-6;
  // Whole bands across the tile, and the spiral grain's whole bands along it (a log's fibres
  // wind round the stem a few degrees: over the tile's length they climb tile * tan(angle)).
  const u32 bands = std::max(1u, static_cast<u32>(tile / figure_m + 0.5));
  const f64 angle = static_cast<f64>(p.spiral_ddeg) * 0.1 * k_pi / 180.0;
  const f64 climb = tile * det::sin(angle) / det::cos(angle);
  const u32 spiral = static_cast<u32>(climb / figure_m + 0.5);
  // The wander's half range in bands.
  const f64 wander = static_cast<f64>(p.wander_mm) * 0.001 / figure_m;
  // The figure's wander: long along the grain, a few centimetres across it, three octaves so the
  // bands fold into arches of every size rather than one wave.
  const u32 along_k = std::max(1u, static_cast<u32>(tile / 0.6 + 0.5));
  const u32 across_k = std::max(2u, static_cast<u32>(tile / 0.15 + 0.5));

  // ---- the phase: bands across, wandering along
  Vector<f64> phase;
  phase.resize(count);
  for (u32 y = 0; y < n; ++y) {
    const u32 ty = field.vy(y) % n;
    for (u32 x = 0; x < n; ++x) {
      const u32 tx = field.vx(x) % n;
      const f64 slow =
          lattice_noise2(set.seed ^ 0x77616e64ull, tx, ty, n, along_k, across_k) +
          0.5 * lattice_noise2(set.seed ^ 0x77616e65ull, tx, ty, n, along_k * 2, across_k * 2) +
          0.25 * lattice_noise2(set.seed ^ 0x77616e66ull, tx, ty, n, along_k * 5, across_k * 4);
      phase[field.index(x, y)] =
          (static_cast<f64>(ty) + 0.5) * static_cast<f64>(bands) / static_cast<f64>(n) +
          (static_cast<f64>(tx) + 0.5) * static_cast<f64>(spiral) / static_cast<f64>(n) +
          wander * (2.0 * slow / 1.75 - 1.0);
    }
  }

  // ---- knots: the rings flow round each one (its own bump in the phase), then its body
  struct Knot {
    i64 cx, cy;
    f64 radius_m;  // across the grain
    f64 stretch;   // along the grain over across
    f64 deflect;   // its radius in bands: how far the rings beside it are pushed
    f64 tone;
    bool encased;  // a dead knot with a dark rim
  };
  Vector<Knot> knots;
  {
    const u64 period = static_cast<u64>(n) * static_cast<u64>(k_sub);
    const u32 wanted = static_cast<u32>(static_cast<f64>(p.knots_per_m2) * tile * tile + 0.5);
    Rng rng(set.seed ^ 0x6b6e6f74ull);
    for (u32 k = 0; k < wanted; ++k) {
      Knot kn;
      kn.cx = static_cast<i64>(rng.next() % period);
      kn.cy = static_cast<i64>(rng.next() % period);
      kn.radius_m =
          0.0005 * rng.range(static_cast<f64>(p.knot_min_mm), static_cast<f64>(p.knot_max_mm));
      kn.stretch = is_log ? rng.range(1.0, 1.25) : rng.range(1.3, 2.2);
      kn.deflect = kn.radius_m / figure_m;
      kn.tone = rng.range(0.75, 1.2);
      kn.encased = rng.unit() < 0.35;
      knots.push_back(kn);
    }
  }
  const auto each_texel_near = [&](const Knot& kn, f64 reach_m, auto&& visit) {
    const i64 size = static_cast<i64>(n);
    const i64 rx = static_cast<i64>(reach_m * kn.stretch / texel) + 2;
    const i64 ry = static_cast<i64>(reach_m / texel) + 2;
    const i64 tcx = kn.cx / k_sub, tcy = kn.cy / k_sub;
    for (i64 ty = tcy - ry; ty <= tcy + ry; ++ty) {
      const f64 ly = offset_texels(ty, kn.cy, size) * texel;
      const u32 sy = field.stored(ty, field.window.y);
      for (i64 tx = tcx - rx; tx <= tcx + rx; ++tx) {
        const f64 lx = offset_texels(tx, kn.cx, size) * texel;
        // The distance in knot radii, the ellipse long along the grain.
        const f64 u = lx / (kn.radius_m * kn.stretch), v = ly / kn.radius_m;
        visit(field.index(field.stored(tx, field.window.x), sy), u, v);
      }
    }
  };
  for (const Knot& kn : knots) {
    // The rings flow round the knot as a stream round a cylinder (the stream function of
    // potential flow, v (1 - 1 / r^2) in the knot's radii): a ring that would cross it is pushed
    // aside by a radius at its edge, less and less out to three radii, where the push fades out.
    each_texel_near(kn, 3.0 * kn.radius_m, [&](u32 i, f64 u, f64 v) {
      const f64 r2 = std::max(1.0, u * u + v * v);
      if (r2 >= 9.0) return;
      const f64 fall = 1.0 - (r2 - 1.0) / 8.0;
      phase[i] -= kn.deflect * (v / r2) * fall * fall;
    });
  }

  // ---- bark, a log's: where a two-octave field is lowest, exactly its share
  const f64 bark_share_asked = static_cast<f64>(p.bark_permille) * 0.001;
  Vector<f32> bark_field;
  f64 bark_at = 0.0;
  if (bark_share_asked > 0.0) {
    bark_field.resize(count);
    const u32 bk_x = std::max(1u, static_cast<u32>(tile / 0.5 + 0.5));
    const u32 bk_y = std::max(2u, static_cast<u32>(tile / 0.18 + 0.5));
    for (u32 y = 0; y < n; ++y) {
      const u32 ty = field.vy(y) % n;
      for (u32 x = 0; x < n; ++x) {
        const u32 tx = field.vx(x) % n;
        bark_field[field.index(x, y)] = static_cast<f32>(
            0.75 * lattice_noise2(set.seed ^ 0x6261726bull, tx, ty, n, bk_x, bk_y) +
            0.25 * lattice_noise2(set.seed ^ 0x6261726cull, tx, ty, n, bk_x * 4, bk_y * 3));
      }
    }
    bark_at = quantile(bark_field, bark_share_asked);
  }

  // ---- the face
  const f64 lw = static_cast<f64>(p.latewood_permille) * 0.001;
  const f64 relief = static_cast<f64>(p.relief_um) * 1.0e-6;
  const f64 grey = static_cast<f64>(p.grey_permille) * 0.001;
  const f64 grey_patch = static_cast<f64>(p.grey_patch_permille) * 0.001;
  const f64 stain = static_cast<f64>(p.stain_permille) * 0.001;
  const f64 fresh[3] = {static_cast<f64>(p.fresh_r_permille) * 0.001,
                        static_cast<f64>(p.fresh_g_permille) * 0.001,
                        static_cast<f64>(p.fresh_b_permille) * 0.001};
  const f64 aged[3] = {static_cast<f64>(p.grey_r_permille) * 0.001,
                       static_cast<f64>(p.grey_g_permille) * 0.001,
                       static_cast<f64>(p.grey_b_permille) * 0.001};
  const u32 patch_k = std::max(1u, static_cast<u32>(tile / 0.25 + 0.5));
  const u32 stain_x = std::max(1u, static_cast<u32>(tile / 0.3 + 0.5));
  const u32 fibre_x = std::max(4u, static_cast<u32>(tile / 0.04 + 0.5));
  const u32 plate_k =
      std::max(4u, static_cast<u32>(tile / (static_cast<f64>(p.bark_plate_mm) * 0.001) + 0.5));
  f64 late_sum = 0.0, bark_sum = 0.0;
  Vector<f32> gradient;  // bands per metre across the grain, for the checks
  gradient.resize(count);
  for (u32 y = 0; y < n; ++y) {
    const u32 ty = field.vy(y) % n;
    const u32 yd = (y + 1) % n;
    const bool wraps_y = field.vy(y) % n == n - 1;
    for (u32 x = 0; x < n; ++x) {
      const u32 tx = field.vx(x) % n;
      const u32 i = field.index(x, y);
      const f64 pv = phase[i];
      // The phase's step to the next texel across and along, unwrapped.
      const f64 dy = phase[field.index(x, yd)] - pv + (wraps_y ? static_cast<f64>(bands) : 0.0);
      const u32 xr = (x + 1) % n;
      const f64 dx =
          phase[field.index(xr, y)] - pv + (tx == n - 1 ? static_cast<f64>(spiral) : 0.0);
      const f64 g = std::sqrt(dx * dx + dy * dy);
      gradient[i] = static_cast<f32>(g / texel);
      // Each year's ring its own: its latewood share and tone drawn for its band.
      const u64 year =
          hash3(set.seed ^ 0x79656172ull, wrap_node(static_cast<i64>(std::floor(pv)), bands), 0);
      const f64 late = latewood_share(pv, g, lw * mix(0.55, 1.45, unit_of(year)));
      const f64 year_tone = 0.9 + 0.2 * unit_of(mix64(year));
      late_sum += late;
      // Fibres: fine streaks along the grain, following the bands.
      const f64 fibre =
          0.6 * periodic_noise(set.seed ^ 0x666962ull,
                               (static_cast<f64>(tx) + 0.5) * fibre_x / static_cast<f64>(n),
                               pv * 4.0, fibre_x, bands * 4) +
          0.4 * periodic_noise(set.seed ^ 0x666963ull,
                               (static_cast<f64>(tx) + 0.5) * fibre_x * 3 / static_cast<f64>(n),
                               pv * 11.0, fibre_x * 3, bands * 11);
      const f64 patch = lattice_noise(set.seed ^ 0x67726579ull, lattice_of(tx, n, patch_k),
                                      lattice_of(ty, n, patch_k), patch_k);
      const f64 g_amount = clamp01(grey + grey_patch * (2.0 * patch - 1.0));
      // Earlywood weathers to a pale silver, latewood to a darker grey; unweathered latewood is
      // the darker, redder band.
      const f64 late_dark = mix(0.62, 0.72, g_amount);
      f64 rgb[3];
      for (u32 c = 0; c < 3; ++c) {
        const f64 fresh_c = fresh[c] * mix(1.0, late_dark * (c == 0 ? 1.08 : 1.0), late);
        const f64 aged_c = aged[c] * mix(1.04, late_dark, late);
        rgb[c] = mix(fresh_c, aged_c, g_amount) * (0.78 + 0.44 * fibre) * year_tone;
      }
      // Fungal and dirt staining: dark grey streaks along the grain.
      const f64 s = periodic_noise(set.seed ^ 0x7374616eull,
                                   (static_cast<f64>(tx) + 0.5) * stain_x / static_cast<f64>(n),
                                   pv * 0.25, stain_x, std::max(1u, bands / 4));
      const f64 stained = stain * smooth01((s - 0.55) / 0.3);
      for (f64& c : rgb)
        c *= 1.0 - 0.55 * stained;
      Texel& t = field.texels[i];
      t.r = static_cast<f32>(rgb[0]);
      t.g = static_cast<f32>(rgb[1]);
      t.b = static_cast<f32>(rgb[2]);
      t.rough = static_cast<f32>(mix(0.84, 0.74, late));
      // Earlywood erodes faster than latewood: the latewood stands proud by `relief`.
      t.height = static_cast<f32>(relief * late + 0.00012 * fibre);

      if (bark_share_asked > 0.0 && static_cast<f64>(bark_field[i]) < bark_at + 0.01) {
        // Bark plates, long along the stem, fissured between; the patch's edge is torn.
        const f64 inside = smooth01((bark_at - static_cast<f64>(bark_field[i])) / 0.01 + 0.5);
        // Plates three times as long up the stem as across, their tops scaled and weathered paler,
        // the fissures between them deep and red-brown.
        const Cell pl =
            cell_noise2(set.seed ^ 0x706c617465ull, tx, ty, n, std::max(2u, plate_k / 3), plate_k);
        const f64 fissure = 1.0 - smooth01((pl.f2 - pl.f1) / 0.32);
        const f64 tone = 0.7 + 0.6 * unit_of(pl.id);
        const f64 scale =
            lattice_noise2(set.seed ^ 0x7363616cull, tx, ty, n, plate_k * 2, plate_k * 5);
        f64 brgb[3];
        for (u32 c = 0; c < 3; ++c)
          brgb[c] = mix(k_bark[c] * tone * (0.8 + 0.4 * scale), k_bark_fissure[c], fissure);
        const f64 thick =
            0.006 * (1.0 - 0.85 * fissure) * (0.8 + 0.4 * unit_of(mix64(pl.id))) + 0.0008 * scale;
        t.r = static_cast<f32>(mix(static_cast<f64>(t.r), brgb[0], inside));
        t.g = static_cast<f32>(mix(static_cast<f64>(t.g), brgb[1], inside));
        t.b = static_cast<f32>(mix(static_cast<f64>(t.b), brgb[2], inside));
        t.rough = static_cast<f32>(mix(static_cast<f64>(t.rough), 0.9, inside));
        t.height = static_cast<f32>(static_cast<f64>(t.height) + thick * inside);
        bark_sum += inside;
      }
    }
  }

  // ---- checks: splits along the grain, each in one band of one stretch of the tile's length
  const f64 check_max_m = static_cast<f64>(p.check_max_mm) * 0.001;
  const u32 stretches = std::max(1u, static_cast<u32>(tile / check_max_m + 0.5));
  const f64 stretch_m = tile / static_cast<f64>(stretches);
  // A band and a stretch hold a check with this probability: checks per square metre times the
  // band's mean width times the stretch.
  const f64 chance = std::min(
      0.9, static_cast<f64>(p.checks_per_m2) * (tile / static_cast<f64>(bands)) * stretch_m);
  const f64 len_lo = std::min(static_cast<f64>(p.check_min_mm) * 0.001, stretch_m);
  const f64 len_hi = std::min(check_max_m, stretch_m);
  const f64 width_max = static_cast<f64>(p.check_width_um) * 1.0e-6;
  const f64 depth_max = static_cast<f64>(p.check_depth_um) * 1.0e-6;
  f64 check_sum = 0.0;
  if (chance > 0.0) {
    for (u32 y = 0; y < n; ++y) {
      for (u32 x = 0; x < n; ++x) {
        const u32 tx = field.vx(x) % n;
        const u32 i = field.index(x, y);
        const f64 pv = phase[i];
        const f64 fb = std::floor(pv);
        const u32 band = wrap_node(static_cast<i64>(fb), bands);
        const u32 stretch = tx * stretches / n;
        const u64 h = hash3(set.seed ^ 0x636865636bull, band, stretch);
        if (unit_of(h) >= chance) continue;
        const u64 h2 = mix64(h);
        const f64 len = mix(len_lo, len_hi, unit_of(h2));
        const f64 start =
            static_cast<f64>(stretch) * stretch_m + unit_of(mix64(h2)) * (stretch_m - len);
        const f64 along = (static_cast<f64>(tx) + 0.5) * texel - start;
        if (along <= 0.0 || along >= len) continue;
        const f64 s = along / len;
        const f64 open = 4.0 * s * (1.0 - s);
        const f64 width = width_max * mix(0.3, 1.0, unit_of(mix64(h2 ^ 0x77ull))) * open;
        const f64 at = mix(0.15, 0.55, unit_of(mix64(h2 ^ 0x61ull)));
        // Metres across the grain from the check's line: the phase's offset over its gradient.
        const f64 d = std::fabs(pv - fb - at) / std::max(1.0e-9, static_cast<f64>(gradient[i]));
        const f64 a = clamp01((0.5 * width - d) / texel + 0.5);
        if (a <= 0.0) continue;
        Texel& t = field.texels[i];
        const f64 dark = 1.0 - 0.85 * a;
        t.r = static_cast<f32>(static_cast<f64>(t.r) * dark);
        t.g = static_cast<f32>(static_cast<f64>(t.g) * dark);
        t.b = static_cast<f32>(static_cast<f64>(t.b) * dark);
        t.rough = static_cast<f32>(mix(static_cast<f64>(t.rough), 0.95, a));
        t.height = static_cast<f32>(static_cast<f64>(t.height) - depth_max * open * a);
        check_sum += a;
      }
    }
  }

  // ---- the knots' bodies: dark, ringed, harder (they stand proud once the wood round them wears)
  for (const Knot& kn : knots) {
    each_texel_near(kn, 1.25 * kn.radius_m, [&](u32 i, f64 u, f64 v) {
      const f64 r = std::sqrt(u * u + v * v);
      const f64 a = clamp01((1.0 - r) * kn.radius_m / texel + 0.5);
      if (a <= 0.0 && !(kn.encased && r < 1.15)) return;
      Texel& t = field.texels[i];
      const f64 rings = 0.5 + 0.5 * det::cos(r * kn.radius_m / figure_m * 2.0 * k_pi * 2.0);
      const f64 g_amount = clamp01(grey * 0.7);
      f64 rgb[3];
      for (u32 c = 0; c < 3; ++c)
        rgb[c] = mix(k_knot_fresh[c], k_knot_grey[c], g_amount) * kn.tone * (0.85 + 0.3 * rings);
      // A dead knot's bark ring: a dark rim round it.
      const f64 rim =
          kn.encased ? clamp01(1.0 - std::fabs(r - 1.03) * kn.radius_m / (0.6 * texel + 0.0006))
                     : 0.0;
      t.r = static_cast<f32>(mix(static_cast<f64>(t.r), rgb[0], a) * (1.0 - 0.7 * rim));
      t.g = static_cast<f32>(mix(static_cast<f64>(t.g), rgb[1], a) * (1.0 - 0.7 * rim));
      t.b = static_cast<f32>(mix(static_cast<f64>(t.b), rgb[2], a) * (1.0 - 0.7 * rim));
      t.rough = static_cast<f32>(mix(static_cast<f64>(t.rough), 0.62, a));
      t.height = static_cast<f32>(static_cast<f64>(t.height) + a * 0.6 * relief - rim * 0.0008);
    });
  }

  const f64 texels = static_cast<f64>(count);
  stats.latewood_share = late_sum / texels;
  stats.check_share = check_sum / texels;
  stats.bark_share = bark_sum / texels;
  stats.knots = static_cast<u32>(knots.size());
  shade.cavity_m = is_log ? 0.004 : 0.0015;
  shade.cavity_px = std::clamp(static_cast<u32>(0.004 / texel + 0.5), 2u, std::max(2u, n / 8));
}

}  // namespace engine::matgen::detail

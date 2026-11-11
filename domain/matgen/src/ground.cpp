// The ground kind (matgen.md, "Ground sets"; generated-material-sets.md, "A floor"): an underlay
// of moss and soil split at the moss share's quantile, dead leaf fragments over it as a Poisson
// cover of the thatch's share, and live leaves over those. The parts are the sward's floor bake's
// (flora.md, "The ground under the sward (G141)"), here as sets a pack role can name.
#include "field.h"

#include <algorithm>

namespace engine::matgen::detail {

void lay_fragment(Field& field, const Fragment& f, Vector<f32>* cover) {
  const i64 size = static_cast<i64>(field.n);
  const f64 texel = field.texel_m;
  const i64 reach = static_cast<i64>((f.half_m + f.width_m) / texel) + 2;
  const i64 tcx = f.cx / k_sub, tcy = f.cy / k_sub;
  for (i64 ty = tcy - reach; ty <= tcy + reach; ++ty) {
    const f64 ly = offset_texels(ty, f.cy, size) * texel;
    const u32 sy = field.stored(ty, field.window.y);
    for (i64 tx = tcx - reach; tx <= tcx + reach; ++tx) {
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
      t.r = static_cast<f32>(mix(static_cast<f64>(t.r), f.colour[0] * rib, a));
      t.g = static_cast<f32>(mix(static_cast<f64>(t.g), f.colour[1] * rib, a));
      t.b = static_cast<f32>(mix(static_cast<f64>(t.b), f.colour[2] * rib, a));
      t.rough = static_cast<f32>(mix(static_cast<f64>(t.rough), f.rough, a));
      const f64 was = static_cast<f64>(t.height);
      t.height = static_cast<f32>(std::max(was, std::min(f.lift_max_m, was + f.lift_m * a)));
      if (cover != nullptr) {
        f32& c = (*cover)[i];
        c = static_cast<f32>(1.0 - (1.0 - static_cast<f64>(c)) * (1.0 - a));
      }
    }
  }
}

namespace {

constexpr u32 k_bins = 4096;

// The value below which `share` of the texels' field lies.
f64 quantile(const Vector<f32>& values, f64 share) {
  Vector<u32> bins;
  bins.assign(k_bins, 0u);
  for (const f32 v : values)
    ++bins[std::min(k_bins - 1, static_cast<u32>(static_cast<f64>(v) * k_bins))];
  const f64 want = share * static_cast<f64>(values.size());
  f64 below = 0.0;
  for (u32 b = 0; b < k_bins; ++b) {
    below += static_cast<f64>(bins[b]);
    if (below >= want) return (static_cast<f64>(b) + 1.0) / static_cast<f64>(k_bins);
  }
  return 1.0;
}

// Linear albedos (generated-material-sets.md, "A floor"): standing litter pale and rising to the
// red, a bryophyte dark, a moist organic loam between, a live grass leaf.
constexpr f64 k_straw[3] = {0.18, 0.145, 0.09};
constexpr f64 k_grey_straw[3] = {0.14, 0.13, 0.115};
constexpr f64 k_moss[3] = {0.045, 0.075, 0.025};
constexpr f64 k_loam[3] = {0.085, 0.065, 0.045};
constexpr f64 k_leaf[3] = {0.055, 0.12, 0.035};

}  // namespace

void build_ground(const Set& set, Field& field, SetStats& stats, Shade& shade) {
  const Ground& p = set.ground;
  const u32 n = field.n;
  const f64 texel = field.texel_m;
  const f64 tile = texel * static_cast<f64>(n);
  const u32 half = n / 2;
  const u32 broad_k = std::clamp(static_cast<u32>(12.0 * tile + 0.5), 2u, half);
  const u32 mid_k = std::clamp(static_cast<u32>(40.0 * tile + 0.5), 4u, half);
  const u32 tip_k = std::clamp(static_cast<u32>(tile / 0.0018 + 0.5), 4u, half);
  const u32 crumb_k = std::clamp(static_cast<u32>(tile / 0.005 + 0.5), 4u, half);

  // ---- the underlay: moss where a two-octave field is lowest, exactly its share
  Vector<f32> under;
  under.resize(n * n);
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
  f64 moss_sum = 0.0;
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x) {
      const u32 i = field.index(x, y);
      const u32 vx = field.vx(x), vy = field.vy(y);
      const f64 value = static_cast<f64>(under[i]);
      const f64 moss =
          q <= 0.0 ? 0.0 : (q >= 1.0 ? 1.0 : 1.0 - smooth01((value - moss_at) / 0.04 + 0.5));
      moss_sum += moss;
      const f64 grain =
          unit_of(hash3(set.seed ^ 0x677261696eull, wrap_node(vx, n), wrap_node(vy, n)));
      // Moss: shoot tips paler than the shade between them, in cushions up to 8 mm.
      const Cell tip = cell_noise(set.seed ^ 0x746970ull, lattice_of(vx, n, tip_k),
                                  lattice_of(vy, n, tip_k), tip_k);
      const f64 lit = clamp01(1.0 - 1.5 * tip.f1);
      const f64 moss_tone = (0.55 + 0.9 * lit) * (0.85 + 0.3 * unit_of(tip.id));
      const f64 moss_h = 0.008 * clamp01((moss_at - value) * 6.0 + 0.3) + 0.0012 * lit;
      // Soil: crumbs a few millimetres across, dark between them, a pale grain here and there.
      const Cell crumb = cell_noise(set.seed ^ 0x6372756d62ull, lattice_of(vx, n, crumb_k),
                                    lattice_of(vy, n, crumb_k), crumb_k);
      const f64 gap = clamp01((crumb.f2 - crumb.f1) / 0.18);
      const bool pale = unit_of(mix64(crumb.id ^ 0x70ull)) < 0.03;
      const f64 soil_tone =
          (0.72 + 0.5 * unit_of(crumb.id)) * (0.6 + 0.4 * gap) * (0.92 + 0.16 * grain);
      const f64 soil_h = 0.002 * gap * (1.0 - crumb.f1) + 0.0006 * grain;
      Texel& t = field.texels[i];
      const f64 soil_rgb[3] = {pale ? 0.30 : k_loam[0] * soil_tone,
                               pale ? 0.28 : k_loam[1] * soil_tone,
                               pale ? 0.25 : k_loam[2] * soil_tone};
      t.r = static_cast<f32>(mix(soil_rgb[0], k_moss[0] * moss_tone, moss));
      t.g = static_cast<f32>(mix(soil_rgb[1], k_moss[1] * moss_tone, moss));
      t.b = static_cast<f32>(mix(soil_rgb[2], k_moss[2] * moss_tone, moss));
      t.rough = static_cast<f32>(mix(0.92, 0.95, moss));
      t.height = static_cast<f32>(mix(soil_h, moss_h, moss));
    }
  }

  // ---- the thatch: N fragments of mean area L w cover a share s with probability
  // 1 - exp(-N L w / A) (a Poisson cover), so N = -ln(1 - s) A / (L w).
  const u64 period = static_cast<u64>(n) * static_cast<u64>(k_sub);
  const f64 len_lo = static_cast<f64>(p.leaf_min_mm) * 0.001;
  const f64 len_hi = static_cast<f64>(p.leaf_max_mm) * 0.001;
  const f64 wid_lo = static_cast<f64>(p.leaf_width_min_um) * 1.0e-6;
  const f64 wid_hi = static_cast<f64>(p.leaf_width_max_um) * 1.0e-6;
  const f64 mean_area = 0.5 * (len_lo + len_hi) * 0.5 * (wid_lo + wid_hi);
  const auto lay = [&](f64 share, u64 salt, bool live, Vector<f32>& cover) {
    cover.assign(n * n, 0.0f);
    const f64 s = std::min(share, 0.98);
    if (s <= 0.0) return;
    // A live leaf tapers to its tips: three quarters of the capsule's area.
    const f64 each = mean_area * (live ? 0.75 : 1.0);
    const u32 fragments = static_cast<u32>(-det::log(1.0 - s) * tile * tile / each + 0.5);
    Rng rng(set.seed ^ salt);
    for (u32 k = 0; k < fragments; ++k) {
      Fragment f;
      f.cx = static_cast<i64>(rng.next() % period);
      f.cy = static_cast<i64>(rng.next() % period);
      const f64 angle = rng.unit() * k_pi;
      f.dir_x = det::cos(angle);
      f.dir_y = det::sin(angle);
      f.half_m = 0.5 * rng.range(len_lo, len_hi);
      f.width_m = rng.range(wid_lo, wid_hi);
      const f64 tone = live ? rng.range(0.75, 1.25) : rng.range(0.6, 1.3);
      const bool grey = !live && rng.unit() < 0.2;
      const f64* base = live ? k_leaf : (grey ? k_grey_straw : k_straw);
      // A live leaf yellows a little toward its own variation; a dead one only pales or darkens.
      const f64 warm = live ? rng.range(0.9, 1.25) : 1.0;
      f.colour[0] = base[0] * tone * warm;
      f.colour[1] = base[1] * tone;
      f.colour[2] = base[2] * tone;
      f.rough = live ? 0.55 + 0.1 * rng.unit() : 0.80 + 0.06 * (rng.unit() - 0.5);
      f.lift_m = live ? 0.0012 : 0.0008;
      f.lift_max_m = live ? 0.025 : 0.015;
      f.taper = live;
      f.midrib = live ? 0.25 : 0.0;
      lay_fragment(field, f, &cover);
    }
  };
  Vector<f32> thatch_cover, live_cover;
  lay(permille(p.thatch_permille), 0x746861746368ull, false, thatch_cover);
  lay(permille(p.live_permille), 0x6c697665ull, true, live_cover);
  f64 thatch_sum = 0.0, live_sum = 0.0;
  for (u32 i = 0; i < n * n; ++i) {
    thatch_sum += static_cast<f64>(thatch_cover[i]);
    live_sum += static_cast<f64>(live_cover[i]);
  }
  const f64 texels = static_cast<f64>(n) * static_cast<f64>(n);
  stats.moss_share = moss_sum / texels;
  stats.thatch_cover = thatch_sum / texels;
  stats.live_cover = live_sum / texels;
  shade.cavity_m = 0.004;
  shade.cavity_px = std::clamp(static_cast<u32>(0.006 / texel + 0.5), 2u, std::max(2u, n / 8));
}

}  // namespace engine::matgen::detail

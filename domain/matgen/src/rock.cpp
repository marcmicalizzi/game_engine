// The rock kind (matgen.md, "Rock sets"; generated-material-sets.md, "A rock face"): crystals at
// the grain's scale by modal share, micro-relief and joints at the decimetre scale, a weathering
// rind against fresh breaks by a share, crustose lichen on the rind by a cover parameter, pits
// and bedding laminae for the rocks that have them.
#include "field.h"

#include <algorithm>

namespace engine::matgen::detail {

namespace {

constexpr u32 k_bins = 4096;

// The value above which `share` of `values` (each in [0, 1]) lies, by a histogram.
f64 share_threshold(const Vector<f32>& values, f64 share) {
  Vector<u32> bins;
  bins.assign(k_bins, 0u);
  for (const f32 v : values) {
    const u32 b = std::min(k_bins - 1, static_cast<u32>(static_cast<f64>(v) * k_bins));
    ++bins[b];
  }
  const f64 want = share * static_cast<f64>(values.size());
  f64 above = 0.0;
  for (u32 b = k_bins; b-- > 0;) {
    above += static_cast<f64>(bins[b]);
    if (above >= want) return static_cast<f64>(b) / static_cast<f64>(k_bins);
  }
  return 0.0;
}

// The four crustose lichens a thallus is one of (linear albedo): a yellow-green map lichen with
// its black margin, a grey crust, a dark crust and a white one.
constexpr f64 k_lichen[4][3] = {
    {0.20, 0.23, 0.07}, {0.33, 0.34, 0.31}, {0.06, 0.06, 0.055}, {0.46, 0.46, 0.42}};

}  // namespace

void build_rock(const Set& set, Field& field, SetStats& stats, Shade& shade) {
  const Rock& p = set.rock;
  const u32 n = field.n;
  const i64 size = static_cast<i64>(n);
  const f64 texel = field.texel_m;
  const f64 tile = texel * static_cast<f64>(n);
  const f64 grain = static_cast<f64>(p.grain_um) * 1.0e-6;
  const f64 relief = static_cast<f64>(p.relief_um) * 1.0e-6;
  const u32 half = n / 2, eighth = std::max(2u, n / 8);
  const u32 grain_k = std::clamp(static_cast<u32>(tile / grain + 0.5), 4u, half);
  const u32 relief_k = std::clamp(static_cast<u32>(tile / 0.25 + 0.5), 2u, eighth);
  const u32 patch_k = std::clamp(static_cast<u32>(tile / 0.18 + 0.5), 2u, eighth);
  const u32 stain_k = std::clamp(static_cast<u32>(tile / 0.15 + 0.5), 2u, eighth);
  const u32 fine_k = std::clamp(static_cast<u32>(tile / 0.02 + 0.5), 4u, half);
  const u32 lobe_k = std::clamp(static_cast<u32>(tile / 0.008 + 0.5), 4u, half);
  const u32 pit_k = std::clamp(static_cast<u32>(tile / 0.012 + 0.5), 4u, half);
  const u32 areole_k = std::min(lobe_k * 2, half);
  const f64 fresh_share = 1.0 - clamp01(permille(p.rind_permille));

  // The rock's mean colour: what the rind pulls each crystal toward.
  f64 mean[3] = {0, 0, 0}, shares = 0.0;
  for (const Constituent& m : p.minerals) {
    const f64 w = static_cast<f64>(m.share_permille);
    mean[0] += w * permille(m.r_permille);
    mean[1] += w * permille(m.g_permille);
    mean[2] += w * permille(m.b_permille);
    shares += w;
  }
  for (f64& c : mean)
    c /= std::max(shares, 1.0);

  // ---- fresh breaks: the patches where a low field is highest, exactly the asked share
  Vector<f32> patch;
  patch.resize(n * n);
  for (u32 y = 0; y < n; ++y)
    for (u32 x = 0; x < n; ++x)
      patch[field.index(x, y)] = static_cast<f32>(
          fractal_noise(set.seed ^ 0x6672657368ull, field.vx(x), field.vy(y), n, patch_k, 3));
  const f64 fresh_at = fresh_share > 0.0 ? share_threshold(patch, fresh_share) : 2.0;

  // ---- lichen: thalli as lobed discs on the torus, as many as a Poisson cover of the asked share
  Vector<u8> thallus;  // 0 none, else species + 1, +4 on its margin
  thallus.assign(n * n, static_cast<u8>(0));
  const f64 lichen = std::min(permille(p.lichen_permille), 0.95);
  if (lichen > 0.0) {
    const f64 r_lo = std::max(0.006, 2.0 * texel), r_hi = std::max(0.03, 4.0 * texel);
    const f64 mean_r2 = (r_lo * r_lo + r_lo * r_hi + r_hi * r_hi) / 3.0;
    const u32 thalli = static_cast<u32>(-det::log(1.0 - lichen) * tile * tile / (k_pi * mean_r2));
    Rng rng(set.seed ^ 0x6c696368656eull);
    const u64 period = static_cast<u64>(size * k_sub);
    for (u32 k = 0; k < thalli; ++k) {
      const i64 cx = static_cast<i64>(rng.next() % period);
      const i64 cy = static_cast<i64>(rng.next() % period);
      const f64 radius = rng.range(r_lo, r_hi);
      const f64 kind = rng.unit();
      const u32 species = kind < 0.22 ? 1u : (kind < 0.62 ? 2u : (kind < 0.86 ? 3u : 4u));
      const i64 reach = static_cast<i64>(1.55 * radius / texel) + 2;
      const i64 tcx = cx / k_sub, tcy = cy / k_sub;
      for (i64 ty = tcy - reach; ty <= tcy + reach; ++ty) {
        const f64 ly = offset_texels(ty, cy, size) * texel;
        const u32 sy = field.stored(ty, field.window.y);
        for (i64 tx = tcx - reach; tx <= tcx + reach; ++tx) {
          const f64 lx = offset_texels(tx, cx, size) * texel;
          // The margin's lobes are a noise of the tile, so they wrap as everything does.
          const u32 wx = wrap_node(tx, n), wy = wrap_node(ty, n);
          const f64 lobe = lattice_noise(set.seed ^ 0x6c6f6265ull, lattice_of(wx, n, lobe_k),
                                         lattice_of(wy, n, lobe_k), lobe_k);
          const f64 g = std::sqrt(lx * lx + ly * ly) / (radius * (0.45 + 1.1 * lobe));
          if (g >= 1.0) continue;
          thallus[field.index(field.stored(tx, field.window.x), sy)] =
              static_cast<u8>(species + (g > 0.85 ? 4 : 0));
        }
      }
    }
  }

  // ---- the texels
  const f64 tint[3] = {permille(p.rind_r_permille), permille(p.rind_g_permille),
                       permille(p.rind_b_permille)};
  const f64 pit_chance = std::min(1.0, permille(p.pits_permille) / (k_pi * 0.3 * 0.3));
  const f64 joint_w = std::max(static_cast<f64>(p.joint_width_um) * 1.0e-6, 0.75 * texel);
  const u32 winding = std::clamp(p.joint_winding, 1u, 6u);
  const f64 spacing = tile / std::sqrt(static_cast<f64>(winding) * static_cast<f64>(winding) + 1.0);
  u32 fresh_texels = 0, lichen_texels = 0;
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x) {
      const u32 i = field.index(x, y);
      const u32 vx = field.vx(x), vy = field.vy(y);
      const Cell crystal = cell_noise(set.seed ^ 0x6372797374616cull, lattice_of(vx, n, grain_k),
                                      lattice_of(vy, n, grain_k), grain_k);
      const Constituent& m = p.minerals[pick_constituent(p.minerals, unit_of(crystal.id))];
      const f64 seam = clamp01((crystal.f2 - crystal.f1) / 0.15);
      const f64 tone =
          (1.0 + permille(m.tone_permille) * (2.0 * unit_of(mix64(crystal.id)) - 1.0)) *
          (0.88 + 0.12 * seam);
      f64 colour[3] = {permille(m.r_permille) * tone, permille(m.g_permille) * tone,
                       permille(m.b_permille) * tone};
      f64 rough = permille(m.roughness_permille);
      const f64 broad = fractal_noise(set.seed ^ 0x72656c696566ull, vx, vy, n, relief_k, 5) - 0.5;
      const f64 fine = fractal_noise(set.seed ^ 0x66696e65ull, vx, vy, n, fine_k, 2) - 0.5;
      f64 height = relief * (2.0 * broad + 0.3 * fine);

      // The rind: the crystals' contrast muted toward the rock's mean, tinted, and stained in
      // broad patches; a fresh break keeps the crystals and sits a flake's depth lower.
      const f64 fresh = smooth01((static_cast<f64>(patch[i]) - fresh_at) / 0.012 + 0.5);
      const f64 stain = fractal_noise(set.seed ^ 0x737461696eull, vx, vy, n, stain_k, 3);
      for (u32 k = 0; k < 3; ++k) {
        const f64 rind = mix(mean[k], colour[k], 0.8) * tint[k] * (0.8 + 0.4 * stain);
        colour[k] = mix(rind, colour[k], fresh);
      }
      rough = mix(0.88, rough, fresh);
      height -= 0.35 * relief * fresh;
      if (fresh >= 0.5) ++fresh_texels;

      // Laminae: beds across the tile, each its own tone and a little proud or shy.
      if (p.laminae > 0) {
        const f64 across = (static_cast<f64>(vy) + 0.5) / static_cast<f64>(n);
        const f64 bed =
            (across - std::floor(across)) * static_cast<f64>(p.laminae) + 0.35 * (stain - 0.5);
        const u64 h = hash3(set.seed ^ 0x626564ull,
                            wrap_node(static_cast<i64>(std::floor(bed)), p.laminae), 0);
        const f64 within = bed - std::floor(bed);
        const f64 edge = smooth01(std::min(within, 1.0 - within) / 0.12);
        const f64 band = (unit_of(h) - 0.5) * edge;
        for (f64& c : colour)
          c *= 1.0 + 0.22 * band;
        height += 0.4 * relief * band;
      }

      // Pits: one to a cell of a coarse lattice, by chance.
      if (pit_chance > 0.0) {
        const Cell pit = cell_noise(set.seed ^ 0x70697473ull, lattice_of(vx, n, pit_k),
                                    lattice_of(vy, n, pit_k), pit_k);
        if (unit_of(mix64(pit.id ^ 0x77ull)) < pit_chance) {
          const f64 radius = 0.18 + 0.2 * unit_of(mix64(pit.id ^ 0x99ull));
          if (pit.f1 < radius) {
            const f64 depth = 1.0 - (pit.f1 / radius) * (pit.f1 / radius);
            height -= 0.6 * relief * depth;
            for (f64& c : colour)
              c *= 1.0 - 0.45 * depth;
          }
        }
      }

      // Joints: each set's traces are the half-integer levels of a phase that winds the torus a
      // whole number of times, so they close on themselves; a set shows where its own low field
      // lets it (a joint's persistence).
      for (u32 s = 0; s < std::min(p.joint_sets, 2u); ++s) {
        const f64 a = s == 0 ? static_cast<f64>(winding) : -1.0;
        const f64 b = s == 0 ? 1.0 : static_cast<f64>(winding);
        const f64 phase = a * (static_cast<f64>(vx) + 0.5) / static_cast<f64>(n) +
                          b * (static_cast<f64>(vy) + 0.5) / static_cast<f64>(n);
        const f64 wander =
            lattice_noise(set.seed ^ (0x6a6f696e74ull + s), lattice_of(vx, n, relief_k),
                          lattice_of(vy, n, relief_k), relief_k);
        const f64 at =
            (phase - std::floor(phase)) + 0.08 * wander + unit_of(hash3(set.seed, 0x6a70ull, s));
        const f64 d = std::fabs((at - std::floor(at)) - 0.5) * spacing;
        if (d >= 3.0 * joint_w) continue;
        const f64 shows =
            smooth01((lattice_noise(set.seed ^ (0x70657273ull + s), lattice_of(vx, n, patch_k),
                                    lattice_of(vy, n, patch_k), patch_k) -
                      0.3) /
                     0.1);
        const f64 groove = shows * det::exp(-(d / joint_w) * (d / joint_w));
        height -= 2.0 * joint_w * groove;
        for (f64& c : colour)
          c *= 1.0 - 0.6 * groove;
        rough = mix(rough, 0.92, groove);
      }

      // Lichen, on the rind only: a crust cracked into areoles, darker at its margin.
      if (thallus[i] != 0 && fresh < 0.5) {
        const u32 species = (thallus[i] - 1u) & 3u;
        const bool margin = thallus[i] > 4;
        const Cell areole = cell_noise(set.seed ^ 0x6172656f6c65ull, lattice_of(vx, n, areole_k),
                                       lattice_of(vy, n, areole_k), areole_k);
        const f64 crack = clamp01((areole.f2 - areole.f1) / 0.2);
        const f64 shade_of = (0.55 + 0.45 * crack) * (0.85 + 0.3 * unit_of(areole.id)) *
                             (margin && species == 0 ? 0.25 : 1.0);
        // A crust thins toward its margin: the rock shows through a little there.
        const f64 hold = margin && species != 0 ? 0.7 : 1.0;
        for (u32 k = 0; k < 3; ++k)
          colour[k] = mix(colour[k], k_lichen[species][k] * shade_of, hold);
        rough = 0.93;
        height += 0.0003;
        ++lichen_texels;
      }

      Texel& t = field.texels[i];
      t.r = static_cast<f32>(colour[0]);
      t.g = static_cast<f32>(colour[1]);
      t.b = static_cast<f32>(colour[2]);
      t.rough = static_cast<f32>(clamp01(rough));
      t.height = static_cast<f32>(height);
    }
  }
  const f64 texels = static_cast<f64>(n) * static_cast<f64>(n);
  stats.fresh_share = static_cast<f64>(fresh_texels) / texels;
  stats.lichen_cover = static_cast<f64>(lichen_texels) / texels;
  shade.cavity_m = std::max(relief, 2.0 * texel);
  shade.cavity_px = std::clamp(static_cast<u32>(0.02 / texel + 0.5), 2u, eighth);
}

}  // namespace engine::matgen::detail

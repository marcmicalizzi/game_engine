// The rock kind (matgen.md, "Rock sets"; generated-material-sets.md, "A rock face"): crystals at
// the grain's scale by modal share, micro-relief at the decimetre scale and form above it
// (scallops, ribs, one broken step), joints as worn gaps, a weathering rind against fresh breaks
// by a share, run-off streaks below the joints and the step, crustose lichen colonies grown from
// a centre on the rind by a cover parameter, moss in a wet face's joints, pits and bedding
// laminae for the rocks that have them.
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

// The crustose lichens a thallus is one of (linear albedo, chosen from their hand-lens colours):
// the yellow-green map lichen with its black prothallus, the orange of Xanthoria elegans (the
// reels' r04 boulders), a grey crust, a dark crust and a white one; and how often each is drawn.
constexpr u32 k_species = 5;
constexpr f64 k_lichen[k_species][3] = {{0.27, 0.30, 0.075},
                                        {0.40, 0.13, 0.025},
                                        {0.33, 0.34, 0.31},
                                        {0.06, 0.06, 0.055},
                                        {0.46, 0.46, 0.42}};
constexpr f64 k_species_odds[k_species] = {0.26, 0.18, 0.38, 0.08, 0.10};

// What a joint's floor holds (fines and soil) and a cushion of moss (the sward floor's moss).
constexpr f64 k_joint_floor[3] = {0.10, 0.085, 0.062};
constexpr f64 k_moss[3] = {0.045, 0.075, 0.025};
// An ink stripe's colour: near black, as the cyanobacterial and lichen films of a run-off streak.
constexpr f64 k_streak[3] = {0.035, 0.036, 0.034};

struct Thallus {
  u32 species = 0;
  f64 tint = 1.0;
  f64 radius_m = 0;
};

}  // namespace

void build_rock(const Set& set, Field& field, SetStats& stats, Shade& shade) {
  const Rock& p = set.rock;
  const u32 n = field.n;
  const i64 size = static_cast<i64>(n);
  const f64 texel = field.texel_m;
  const f64 tile = texel * static_cast<f64>(n);
  const f64 grain = static_cast<f64>(p.grain_um) * 1.0e-6;
  const f64 relief = static_cast<f64>(p.relief_um) * 1.0e-6;
  const f64 form = static_cast<f64>(p.form_um) * 1.0e-6;
  const u32 half = n / 2, eighth = std::max(2u, n / 8);
  const u32 grain_k = std::clamp(static_cast<u32>(tile / grain + 0.5), 4u, half);
  const u32 relief_k = std::clamp(static_cast<u32>(tile / 0.25 + 0.5), 2u, eighth);
  const u32 patch_k = std::clamp(static_cast<u32>(tile / 0.18 + 0.5), 2u, eighth);
  const u32 stain_k = std::clamp(static_cast<u32>(tile / 0.15 + 0.5), 2u, eighth);
  const u32 fine_k = std::clamp(static_cast<u32>(tile / 0.02 + 0.5), 4u, half);
  const u32 lobe_k = std::clamp(static_cast<u32>(tile / 0.006 + 0.5), 4u, half);
  const u32 lobe_coarse_k = std::clamp(static_cast<u32>(tile / 0.025 + 0.5), 4u, half);
  const u32 pit_k = std::clamp(static_cast<u32>(tile / 0.012 + 0.5), 4u, half);
  const u32 areole_k = std::clamp(static_cast<u32>(tile / 0.003 + 0.5), 4u, half);
  // Form above the decimetre: scallops a third of a metre across, ribs half a metre apart, a
  // step's line from a field one tile in period.
  const u32 scallop_k = std::clamp(static_cast<u32>(tile / 0.33 + 0.5), 2u, eighth);
  const u32 rib_k = std::clamp(static_cast<u32>(tile / 0.5 + 0.5), 1u, eighth);
  const u32 wide_k = 2;
  // A streak's noise: fine across (3 cm), long down the face (60 cm).
  const u32 streak_x_k = std::clamp(static_cast<u32>(tile / 0.03 + 0.5), 4u, half);
  const u32 streak_y_k = std::clamp(static_cast<u32>(tile / 0.6 + 0.5), 1u, eighth);
  const f64 fresh_share = 1.0 - clamp01(permille(p.rind_permille));
  const f64 streak_strength = clamp01(permille(p.streak_permille));
  const f64 moss_share = clamp01(permille(p.moss_permille));

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

  // ---- lichen: colonies grown out from a centre, lobed at the margin, as many as a Poisson
  // cover of the asked share. A thallus' size runs from 2 mm across up to `lichen_max_mm`, small
  // ones the most (a log-uniform draw, skewed small): an old face holds a few large colonies
  // among many young ones. Where two meet, the texel is the one nearer its own centre's, and the
  // line between them is a margin: neighbouring thalli stop at each other.
  Vector<u32> owner;  // 0 none, else thallus index + 1
  owner.assign(n * n, 0u);
  Vector<f32> reach_of;  // the texel's distance from its owner's centre over that owner's reach
  reach_of.assign(n * n, 2.0f);
  Vector<Thallus> thalli;
  const f64 lichen = std::min(permille(p.lichen_permille), 0.95);
  if (lichen > 0.0) {
    const f64 r_lo = std::max(0.001, 1.5 * texel);
    const f64 r_hi = std::max(r_lo * 1.5, static_cast<f64>(p.lichen_max_mm) * 0.0005);
    const f64 target = -det::log(1.0 - lichen) * tile * tile;
    const f64 span = det::log(r_hi / r_lo);
    Rng rng(set.seed ^ 0x6c696368656eull);
    const u64 period = static_cast<u64>(size * k_sub);
    f64 area = 0.0;
    const u32 cluster_k = std::clamp(static_cast<u32>(tile / 0.3 + 0.5), 2u, eighth);
    u32 draws = 0;
    while (area < target && thalli.size() < 200000u && ++draws < 4000000u) {
      const i64 cx = static_cast<i64>(rng.next() % period);
      const i64 cy = static_cast<i64>(rng.next() % period);
      const f64 u = rng.unit();
      // Colonies cluster where a face suits them (a ledge's lip, a facet the weather reaches):
      // a centre is kept by a low field's chance, so the cover gathers into patches.
      const f64 suits = lattice_noise(
          set.seed ^ 0x7375697473ull, lattice_of(wrap_node(cx / k_sub, n), n, cluster_k),
          lattice_of(wrap_node(cy / k_sub, n), n, cluster_k), cluster_k);
      if (rng.unit() > smooth01((suits - 0.45) / 0.3)) continue;
      Thallus th;
      th.radius_m = r_lo * det::exp(span * u);
      const f64 kind = rng.unit();
      f64 odds = 0.0;
      th.species = k_species - 1;
      for (u32 s = 0; s < k_species; ++s) {
        odds += k_species_odds[s];
        if (kind < odds) {
          th.species = s;
          break;
        }
      }
      th.tint = rng.range(0.8, 1.15);
      // The lobes' mean reach is the radius: (0.55 + 0.9 * 0.5) = 1.
      area += k_pi * th.radius_m * th.radius_m;
      thalli.push_back(th);
      const u32 id = static_cast<u32>(thalli.size());
      const f64 radius = th.radius_m;
      const i64 reach = static_cast<i64>(1.45 * radius / texel) + 2;
      const i64 tcx = cx / k_sub, tcy = cy / k_sub;
      for (i64 ty = tcy - reach; ty <= tcy + reach; ++ty) {
        const f64 ly = offset_texels(ty, cy, size) * texel;
        const u32 sy = field.stored(ty, field.window.y);
        const u32 wy = wrap_node(ty, n);
        for (i64 tx = tcx - reach; tx <= tcx + reach; ++tx) {
          const f64 lx = offset_texels(tx, cx, size) * texel;
          // The margin's lobes are noises of the tile, so they wrap as everything does; a fine
          // one for the lobes and a coarse one so a large colony's outline is not a circle.
          const u32 wx = wrap_node(tx, n);
          const f64 lobe = lattice_noise(set.seed ^ 0x6c6f6265ull, lattice_of(wx, n, lobe_k),
                                         lattice_of(wy, n, lobe_k), lobe_k);
          const f64 coarse =
              lattice_noise(set.seed ^ 0x636f61727365ull, lattice_of(wx, n, lobe_coarse_k),
                            lattice_of(wy, n, lobe_coarse_k), lobe_coarse_k);
          const f64 g = std::sqrt(lx * lx + ly * ly) /
                        (radius * (0.55 + 0.9 * (0.55 * lobe + 0.45 * coarse)));
          if (g >= 1.0) continue;
          const u32 i = field.index(field.stored(tx, field.window.x), sy);
          if (owner[i] != 0 && static_cast<f64>(reach_of[i]) <= g) continue;
          owner[i] = id;
          reach_of[i] = static_cast<f32>(g);
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
  // Where water leaves the face's relief and runs down it: a joint's floor or the step's lip.
  Vector<u8> source;
  source.assign(n * n, static_cast<u8>(0));
  u32 fresh_texels = 0, lichen_texels = 0, moss_texels = 0;
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

      // Form above the decimetre: scallops (dishes between sharp rims, as a face that spalls in
      // flakes), ribs along the face, and one broken step where a low field crosses its middle.
      bool lip = false;
      if (form > 0.0) {
        const Cell dish = cell_noise(set.seed ^ 0x7363616c6c6f70ull, lattice_of(vx, n, scallop_k),
                                     lattice_of(vy, n, scallop_k), scallop_k);
        const f64 rim = dish.f1;
        const f64 ribs = lattice_noise2(set.seed ^ 0x726962ull, vx, vy, n, rib_k * 2 + 1, 1);
        const f64 wide = lattice_noise(set.seed ^ 0x77696465ull, lattice_of(vx, n, wide_k),
                                       lattice_of(vy, n, wide_k), wide_k);
        const f64 step = smooth01((wide - 0.5) / 0.03);
        height += form * (0.6 * rim * rim + 0.5 * (ribs - 0.5) + 1.4 * step);
        // Just under the step's edge water drips off.
        lip = step > 0.05 && step < 0.5;
        // The step's face is a fracture, rougher and darker for its shade.
        if (step > 0.02 && step < 0.98) {
          for (f64& c : colour)
            c *= 0.9;
        }
      }

      // The rind: the crystals' contrast muted toward the rock's mean, tinted, and stained in
      // broad patches; a fresh break keeps more of the crystals' contrast, is paler but stained
      // too (it is not new, only newer), and sits a flake's depth lower behind a sharp edge.
      const f64 fresh = smooth01((static_cast<f64>(patch[i]) - fresh_at) / 0.006 + 0.5);
      const f64 stain = fractal_noise(set.seed ^ 0x737461696eull, vx, vy, n, stain_k, 3);
      for (u32 k = 0; k < 3; ++k) {
        const f64 rind = mix(mean[k], colour[k], 0.8) * tint[k] * (0.8 + 0.4 * stain);
        const f64 newer = mix(rind, colour[k], 0.7) * (0.92 + 0.16 * stain);
        colour[k] = mix(rind, newer, fresh);
      }
      rough = mix(0.88, rough, fresh);
      height -= 0.8 * relief * fresh;
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
      // lets it (a joint's persistence). A trace is a worn gap, not a line: a floor of fines its
      // own width (which wanders along it), shoulders rounded over two and a half widths, and
      // four widths deep.
      f64 in_joint = 0.0;
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
        const f64 open =
            0.5 * joint_w *
            (0.5 + 1.2 * lattice_noise(set.seed ^ (0x6f70656eull + s), lattice_of(vx, n, patch_k),
                                       lattice_of(vy, n, patch_k), patch_k));
        const f64 shoulder = 2.5 * joint_w;
        if (d >= open + shoulder) continue;
        const f64 shows =
            smooth01((lattice_noise(set.seed ^ (0x70657273ull + s), lattice_of(vx, n, patch_k),
                                    lattice_of(vy, n, patch_k), patch_k) -
                      0.15) /
                     0.1);
        if (shows <= 0.0) continue;
        // 1 on the floor, falling over the shoulder as a quarter circle's profile.
        const f64 over = d <= open ? 0.0 : (d - open) / shoulder;
        const f64 depth =
            shows * (1.0 - std::sqrt(std::max(0.0, 1.0 - (1.0 - over) * (1.0 - over))));
        const f64 floor_k = shows * (1.0 - smooth01(over / 0.15));
        height -= 4.0 * joint_w * depth;
        for (u32 k = 0; k < 3; ++k)
          colour[k] = mix(colour[k] * (1.0 - 0.2 * depth), k_joint_floor[k], 0.6 * floor_k);
        rough = mix(rough, 0.95, depth);
        in_joint = std::max(in_joint, std::max(floor_k, 0.6 * depth));
        if (floor_k > 0.5) lip = true;
      }

      // Moss in the joints that hold water, overflowing their shoulders, where a low field along
      // them says this stretch holds it.
      bool mossy = false;
      if (moss_share > 0.0 && in_joint > 0.0) {
        const f64 where = lattice_noise(set.seed ^ 0x6d6f7373ull, lattice_of(vx, n, patch_k * 2),
                                        lattice_of(vy, n, patch_k * 2), patch_k * 2);
        if (where < moss_share) {
          const Cell shoot = cell_noise(set.seed ^ 0x73686f6f74ull, lattice_of(vx, n, areole_k),
                                        lattice_of(vy, n, areole_k), areole_k);
          const f64 tip = 0.7 + 0.5 * clamp01(1.0 - shoot.f1);
          const f64 k = smooth01((in_joint - 0.1) / 0.3);
          for (u32 c = 0; c < 3; ++c)
            colour[c] = mix(colour[c], k_moss[c] * tip, k);
          rough = mix(rough, 0.97, k);
          height += 3.0 * joint_w * k;
          if (k > 0.5) {
            mossy = true;
            ++moss_texels;
          }
        }
      }
      if (lip) source[i] = 1;

      // Lichen, on the rind only: a crust cracked into areoles. A colony is older at its centre
      // (darker, more cracked, eroded through in places on a large one) and young at its margin
      // (brighter, entire); the map lichen's margin is its black prothallus.
      if (owner[i] != 0 && fresh < 0.5 && !mossy) {
        const Thallus& th = thalli[owner[i] - 1];
        const f64 g = static_cast<f64>(reach_of[i]);
        const Cell areole = cell_noise(set.seed ^ 0x6172656f6c65ull, lattice_of(vx, n, areole_k),
                                       lattice_of(vy, n, areole_k), areole_k);
        const f64 crack = clamp01((areole.f2 - areole.f1) / (0.12 + 0.25 * (1.0 - g)));
        const bool eroded = th.radius_m > 0.02 && g < 0.35 && unit_of(areole.id) < 0.35;
        if (!eroded) {
          const bool margin = g > 0.88;
          const f64 age = mix(0.78, 1.08, smooth01(g));
          f64 shade_of = (0.55 + 0.45 * crack) * (0.88 + 0.24 * unit_of(areole.id)) * age * th.tint;
          if (margin && th.species == 0) shade_of *= 0.25;
          // A crust thins toward its margin: the rock shows through a little there.
          const f64 hold = margin && th.species != 0 ? 0.65 : 0.97;
          for (u32 k = 0; k < 3; ++k)
            colour[k] = mix(colour[k], k_lichen[th.species][k] * shade_of, hold);
          rough = 0.93;
          height += 0.0004 * (1.0 - g);
          ++lichen_texels;
        }
      }

      Texel& t = field.texels[i];
      t.r = static_cast<f32>(colour[0]);
      t.g = static_cast<f32>(colour[1]);
      t.b = static_cast<f32>(colour[2]);
      t.rough = static_cast<f32>(clamp01(rough));
      t.height = static_cast<f32>(height);
    }
  }

  // ---- run-off streaks ("Tintenstriche": dark stripes of cyanobacteria and lichen films where
  // water runs down a face from a joint or a ledge). Down the image is down the face. For each
  // texel, how far below the nearest source above it lies, in texels, capped at the streak's
  // length: an integer, found by two sweeps down each column round the torus, so it does not
  // depend on where the image starts. The stripe fades with that distance and is split across
  // into strands by a noise fine across and long down.
  u32 streak_texels = 0;
  if (streak_strength > 0.0) {
    const u32 length = std::max(2u, static_cast<u32>(0.45 / texel + 0.5));
    Vector<u32> below;
    below.assign(n * n, length);
    for (u32 x = 0; x < n; ++x) {
      u32 run = length;
      for (u32 pass = 0; pass < 2; ++pass) {
        for (u32 y = 0; y < n; ++y) {
          const u32 i = field.index(x, y);
          run = source[i] != 0 ? 0u : std::min(length, run + 1);
          below[i] = run;
        }
      }
    }
    for (u32 y = 0; y < n; ++y) {
      for (u32 x = 0; x < n; ++x) {
        const u32 i = field.index(x, y);
        if (below[i] >= length) continue;
        const u32 vx = field.vx(x), vy = field.vy(y);
        const f64 strand =
            lattice_noise2(set.seed ^ 0x737472616e64ull, vx, vy, n, streak_x_k, streak_y_k);
        const f64 along = 1.0 - static_cast<f64>(below[i]) / static_cast<f64>(length);
        const f64 k = streak_strength * 0.6 * along * along * smooth01((strand - 0.35) / 0.3);
        if (k <= 0.0) continue;
        Texel& t = field.texels[i];
        t.r = static_cast<f32>(mix(static_cast<f64>(t.r), k_streak[0], k));
        t.g = static_cast<f32>(mix(static_cast<f64>(t.g), k_streak[1], k));
        t.b = static_cast<f32>(mix(static_cast<f64>(t.b), k_streak[2], k));
        t.rough = static_cast<f32>(mix(static_cast<f64>(t.rough), 0.8, k));
        if (k > 0.15) ++streak_texels;
      }
    }
  }

  const f64 texels = static_cast<f64>(n) * static_cast<f64>(n);
  stats.fresh_share = static_cast<f64>(fresh_texels) / texels;
  stats.lichen_cover = static_cast<f64>(lichen_texels) / texels;
  stats.streak_share = static_cast<f64>(streak_texels) / texels;
  stats.moss_cover = static_cast<f64>(moss_texels) / texels;
  shade.cavity_m = std::max(relief, 2.0 * texel);
  shade.cavity_px = std::clamp(static_cast<u32>(0.02 / texel + 0.5), 2u, eighth);
}

}  // namespace engine::matgen::detail

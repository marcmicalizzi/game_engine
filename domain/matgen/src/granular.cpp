// The granular kind (matgen.md, "Granular sets"; generated-material-sets.md, "A bed of clasts"): a
// gravel bed, a sand bar's pebbles, a scree's blocks.
//
// A pebble count (Wolman 1954) lays a grid on the bed and measures the intermediate axis of the
// clast under each point, so what it states — D16, D50, D84 — is the size distribution **weighted
// by the area each clast shows**, and Kellerhals and Bray (1971) showed that is the one surface
// count comparable to a bulk sieve analysis. The recipe states that distribution (a normal in
// phi about the median, cut `truncate` phi either side). The generator needs the distribution
// **by number**, because it draws clasts one at a time: a lognormal weighted by D^2 is the same
// lognormal moved up by 2 s^2 in ln D, so the draw is the stated one moved down by that much.
// Clasts are drawn until their plan areas sum to the asked cover, sorted, and placed largest
// first at hashed positions on the torus, each accepted where little of it is already taken; one
// that finds no place in `k_attempts` tries goes to the layer below, seen through the gaps.
// The surface is the highest clast at each texel.
#include "field.h"

#include <algorithm>

namespace engine::matgen::detail {

namespace {

// Eight sides at most, and one more for a broken face.
constexpr u32 k_max_edges = 9;
constexpr u32 k_no_edge = 0xffu;
constexpr u32 k_attempts = 24;
constexpr u32 k_max_clasts = 400000;
constexpr u32 k_no_clast = 0xffffffffu;
// How much of an angular clast's radius its bevel takes, and how many of the thirteen probes of a
// candidate's place may already be taken (two of the outer ring: clasts lean on each other).
constexpr f64 k_bevel = 0.3;
constexpr u32 k_taken_limit = 2;

struct Clast {
  i64 cx = 0, cy = 0;  // centre, sub-texels on the torus
  f64 a_m = 0, b_m = 0, c_m = 0;
  f64 cos_t = 1, sin_t = 0;  // the a-axis in the tile's frame
  f64 z0_m = 0;
  f64 tilt_x = 0, tilt_y = 0;
  f64 rounded = 0.5;
  f64 smooth = 0.5;  // how far its outline and top are a pebble's and not a block's
  f64 tone = 1;
  f64 edge_cos[k_max_edges] = {};
  f64 edge_sin[k_max_edges] = {};
  f64 edge_d[k_max_edges] = {};
  f64 ridge = 0, ridge_x = 1, ridge_y = 0, ridge_o = 0;
  // A block's second fracture face across its top, so a top is a roof and not a plate.
  f64 ridge2 = 0, ridge2_x = 0, ridge2_y = 1, ridge2_o = 0;
  f64 vein_x = 1, vein_y = 0, vein_o = 0, vein_w = 0;
  f64 film = 0;  // how much biofilm its top carries, if it lies on top
  u32 edges = 0;
  u32 broken = k_no_edge;  // the edge that is a broken face
  u32 lith = 0;
  u32 layer = 0;       // 0 on top; 1.. how many clasts down it lies
  u32 shown = 0;       // texels it is the surface of
  bool under = false;  // drawn for a lower layer of an openwork bed
};

// The clast's top at (lx, ly) metres from its centre; false outside its outline. The outline is
// the unit circle of its own frame cut by a polygon whose edges stand at `edge_d`: at the circle
// for a well rounded clast (no cut), at the inscribed polygon for a very angular one. The top is
// an ellipsoid's cap for a rounded clast, a bevelled plateau broken by one ridge for an angular
// one, and leans by the clast's tilt.
bool clast_top(const Clast& c, f64 lx, f64 ly, f64& z, f64& rise, bool& on_break) noexcept {
  const f64 u = (lx * c.cos_t + ly * c.sin_t) / (0.5 * c.a_m);
  const f64 v = (ly * c.cos_t - lx * c.sin_t) / (0.5 * c.b_m);
  const f64 r2 = u * u + v * v;
  if (r2 >= 1.0) return false;
  f64 g = std::sqrt(r2);
  u32 by = k_no_edge;
  for (u32 e = 0; e < c.edges; ++e) {
    const f64 s = (u * c.edge_cos[e] + v * c.edge_sin[e]) / c.edge_d[e];
    if (s > g) {
      g = s;
      by = e;
    }
  }
  if (g >= 1.0) return false;
  const f64 dome = std::sqrt(1.0 - g * g);
  f64 facet = std::min(1.0, (1.0 - g) / k_bevel);
  const f64 cut = 1.0 - c.ridge * std::max(0.0, u * c.ridge_x + v * c.ridge_y + c.ridge_o);
  const f64 cut2 = 1.0 - c.ridge2 * std::max(0.0, u * c.ridge2_x + v * c.ridge2_y + c.ridge2_o);
  facet = std::min(facet, std::max(std::min(cut, cut2), 0.15));
  // A broken face is a fresh fracture: its bevel stays sharp however rounded the rest is.
  on_break = by == c.broken && by != k_no_edge;
  const f64 shape = on_break ? std::min(facet, dome) : mix(facet, dome, c.smooth);
  rise = shape;
  z = c.z0_m + 0.5 * c.c_m * shape + c.tilt_x * lx + c.tilt_y * ly;
  return true;
}

}  // namespace

void build_granular(const Set& set, Field& field, SetStats& stats, Shade& shade) {
  const Granular& p = set.granular;
  const u32 n = field.n;
  const i64 size = static_cast<i64>(n);
  const f64 texel = field.texel_m;
  const f64 tile = texel * static_cast<f64>(n);
  const f64 ln2 = det::log(2.0);
  const f64 d50 = static_cast<f64>(p.d50_um) * 1.0e-6;
  const f64 s = permille(p.sorting_milliphi) * ln2;
  const f64 cut = permille(p.truncate_milliphi) * ln2;
  const f64 lo = std::max(d50 * det::exp(-cut), 3.0 * texel);
  // The largest clast is a sixth of the tile at most: a larger one is a landmark the repeat shows.
  const f64 hi = std::min(d50 * det::exp(cut), tile / 6.0);
  const f64 buried = clamp01(permille(p.buried_permille));
  const f64 film = clamp01(permille(p.film_permille));
  const f64 broken = clamp01(permille(p.broken_permille));
  const u32 depth_layers = std::clamp(p.depth_layers, 1u, 3u);
  const f64 roundness = clamp01(permille(p.roundness_permille));
  const f64 openwork = clamp01(permille(p.openwork_permille));
  const f64 cb_mid = 0.5 * (permille(p.cb_min_permille) + permille(p.cb_max_permille));
  const f64 c50 = d50 * cb_mid;

  // ---- the draw: clasts by number until their plan areas reach the asked cover
  Vector<Clast> clasts;
  f64 sphericity = 0.0;
  if (lo < hi && p.cover_permille > 0) {
    Rng rng(set.seed ^ 0x636c61737473ull);
    const f64 mu = det::log(d50) - 2.0 * s * s;
    const f64 ln_lo = det::log(lo), ln_hi = det::log(hi);
    const f64 target = permille(p.cover_permille) * tile * tile;
    f64 area = 0.0;
    u32 rejected = 0;
    while (area < target && clasts.size() < k_max_clasts && rejected < 4000000u) {
      const f64 x = mu + s * rng.normal();
      if (x < ln_lo || x > ln_hi) {
        ++rejected;
        continue;
      }
      Clast c;
      c.b_m = det::exp(x);
      c.a_m = c.b_m / rng.range(permille(p.ba_min_permille), permille(p.ba_max_permille));
      c.c_m = c.b_m * rng.range(permille(p.cb_min_permille), permille(p.cb_max_permille));
      // Sneed and Folk's maximum projection sphericity, (c^2 / (a b))^(1/3).
      sphericity += det::exp(det::log(c.c_m * c.c_m / (c.a_m * c.b_m)) / 3.0);
      // The a-axis: across the flow (the set's +u) by `alignment`, else any way round.
      const f64 across = 0.5 * k_pi + (rng.unit() - 0.5) * 0.6;
      const f64 any = rng.unit() * k_pi;
      const f64 angle = rng.unit() < permille(p.alignment_permille) ? across : any;
      c.cos_t = det::cos(angle);
      c.sin_t = det::sin(angle);
      c.rounded = clamp01(roundness + 0.15 * (rng.unit() - 0.5));
      // Powers' classes are logarithmic: by "subrounded" (0.35 to 0.49) the corners are gone and
      // by "rounded" the faces are, so the form goes from block to pebble between 0.15 and 0.65.
      c.smooth = clamp01((c.rounded - 0.15) / 0.5);
      // An angular block has few sides (four to six); a rounded pebble's flats are many and slight.
      c.edges = (c.rounded < 0.4 ? 4u : 6u) + static_cast<u32>(rng.unit() * 2.999);
      const f64 inscribed = det::cos(k_pi / static_cast<f64>(c.edges));
      for (u32 e = 0; e < c.edges; ++e) {
        const f64 at = 2.0 * k_pi * (static_cast<f64>(e) + 0.85 * (rng.unit() - 0.5)) /
                       static_cast<f64>(c.edges);
        c.edge_cos[e] = det::cos(at);
        c.edge_sin[e] = det::sin(at);
        c.edge_d[e] = mix(inscribed, 1.0, c.smooth) * (1.0 - 0.3 * (1.0 - c.smooth) * rng.unit());
      }
      // A broken face: one straight side across the outline, well inside the circle.
      if (broken > 0.0 && rng.unit() < broken && c.edges < k_max_edges) {
        const f64 at = rng.unit() * 2.0 * k_pi;
        c.broken = c.edges;
        c.edge_cos[c.edges] = det::cos(at);
        c.edge_sin[c.edges] = det::sin(at);
        c.edge_d[c.edges] = rng.range(0.4, 0.75);
        ++c.edges;
      }
      const f64 ridge_at = rng.unit() * 2.0 * k_pi;
      c.ridge = (1.0 - c.smooth) * rng.range(0.6, 1.5);
      c.ridge_x = det::cos(ridge_at);
      c.ridge_y = det::sin(ridge_at);
      c.ridge_o = rng.range(-0.3, 0.3);
      const f64 ridge2_at = ridge_at + rng.range(0.6, 2.5);
      c.ridge2 = (1.0 - c.smooth) * rng.range(0.3, 1.1);
      c.ridge2_x = det::cos(ridge2_at);
      c.ridge2_y = det::sin(ridge2_at);
      c.ridge2_o = rng.range(-0.2, 0.4);
      // The lean: the a-b plane dips against the flow, so the top rises down it, and a random
      // tilt about any axis beside it. A lean is kept under what the clast's thickness can show.
      const f64 dip = static_cast<f64>(p.imbrication_deg) * rng.range(0.5, 1.5) * k_pi / 180.0;
      const f64 stray = static_cast<f64>(p.scatter_deg) * rng.unit() * k_pi / 180.0;
      const f64 stray_at = rng.unit() * 2.0 * k_pi;
      const f64 lean = det::sin(dip) / det::cos(dip);
      const f64 off = det::sin(stray) / det::cos(stray);
      const f64 most = 0.8 * c.c_m / c.a_m;
      c.tilt_x = std::clamp(lean + off * det::cos(stray_at), -most, most);
      c.tilt_y = std::clamp(off * det::sin(stray_at), -most, most);
      c.lith = pick_constituent(p.lithologies, rng.unit());
      const Constituent& rock = p.lithologies[c.lith];
      // A clast's tone strays from its lithology's less the larger it is (G153): the few largest
      // in a tile are its landmarks, and a pale or dark one among them is what the eye finds
      // again every period. Past D50 the stray falls as the square root of D50 / b.
      const f64 stray_by_size = c.b_m > d50 ? std::sqrt(d50 / c.b_m) : 1.0;
      c.tone = 1.0 + permille(rock.tone_permille) * stray_by_size * (2.0 * rng.unit() - 1.0);
      // A stable top carries the bed's film; a fifth of the clasts were turned or scoured clean.
      const f64 film_draw = rng.unit();
      c.film = film * (film_draw < 0.2 ? 0.0 : 0.4 + 0.75 * film_draw);
      if (rng.unit() < permille(rock.vein_permille)) {
        const f64 vein_at = rng.unit() * k_pi;
        c.vein_x = det::cos(vein_at);
        c.vein_y = det::sin(vein_at);
        c.vein_o = rng.range(-0.3, 0.3) * c.b_m;
        c.vein_w = rng.range(0.02, 0.06) * c.b_m + 0.5 * texel;
      }
      c.z0_m = 0.15 * c.c_m * (rng.unit() - 0.5);
      // Embeddedness: a clast settles into the matrix by up to the bed's buried share of its
      // thickness.
      c.z0_m -= buried * c.c_m * rng.unit();
      area += 0.25 * k_pi * c.a_m * c.b_m;
      clasts.push_back(c);
    }
  }
  const u32 drawn = static_cast<u32>(clasts.size());
  stats.clasts_drawn = drawn;
  stats.mean_sphericity = drawn > 0 ? sphericity / static_cast<f64>(drawn) : 0.0;
  // Openwork: nothing fills between the blocks, so what shows down a gap is more blocks. A share
  // of the drawn ones (all of them at openwork 1) lies again, elsewhere, a layer down.
  // With `depth_layers`, a twin for each layer down, so a gap shows blocks wedged at every depth
  // and not one floor of them.
  if (openwork > 0.0) {
    clasts.reserve(drawn * (depth_layers + 1));
    for (u32 d = 1; d <= depth_layers; ++d) {
      for (u32 i = 0; i < drawn; ++i) {
        if (unit_of(hash3(set.seed ^ 0x6c6f776572ull, i, d - 1)) >= openwork) continue;
        Clast twin = clasts[i];
        twin.under = true;
        twin.layer = d;
        twin.film = 0.0;
        clasts.push_back(twin);
      }
    }
  }
  const u32 count = static_cast<u32>(clasts.size());

  // ---- the placing: largest first, each where little of it is taken
  Vector<u32> order;
  order.resize(count);
  for (u32 i = 0; i < count; ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(), [&](u32 l, u32 r) {
    return clasts[l].b_m != clasts[r].b_m ? clasts[l].b_m > clasts[r].b_m : l < r;
  });
  Vector<u8> taken;
  taken.assign(n * n, static_cast<u8>(0));
  Vector<f32> top;
  top.assign(n * n, -1.0e9f);
  Vector<u32> ids;
  ids.assign(n * n, k_no_clast);
  // How far up its own top the surface is (0 at the outline, 1 at the crown), and whether it is a
  // broken face: where the film lies and where the rock is fresh.
  Vector<f32> rises;
  rises.assign(n * n, 0.0f);
  Vector<u8> breaks;
  breaks.assign(n * n, static_cast<u8>(0));
  static constexpr f64 k_probe[13][2] = {
      {0.0, 0.0},      {0.5, 0.0},      {0.25, 0.433}, {-0.25, 0.433}, {-0.5, 0.0},
      {-0.25, -0.433}, {0.25, -0.433},  {0.9, 0.0},    {0.45, 0.779},  {-0.45, 0.779},
      {-0.9, 0.0},     {-0.45, -0.779}, {0.45, -0.779}};
  const i64 period = size * k_sub;
  const f64 drop = (0.6 + 0.6 * openwork) * c50;
  for (u32 o = 0; o < count; ++o) {
    Clast& c = clasts[order[o]];
    const u32 down = c.under ? c.layer : 1u;
    c.layer = down;
    for (u32 attempt = 0; attempt < k_attempts; ++attempt) {
      const u64 h = hash3(set.seed ^ 0x706c616365ull, order[o], attempt);
      c.cx = static_cast<i64>(h % static_cast<u64>(period));
      c.cy = static_cast<i64>(mix64(h) % static_cast<u64>(period));
      if (c.under) break;
      u32 hits = 0;
      for (const auto& probe : k_probe) {
        const f64 pu = probe[0] * 0.5 * c.a_m, pv = probe[1] * 0.5 * c.b_m;
        const f64 px = (pu * c.cos_t - pv * c.sin_t) / texel;
        const f64 py = (pu * c.sin_t + pv * c.cos_t) / texel;
        const i64 tx =
            (c.cx + period + static_cast<i64>(std::floor(px * static_cast<f64>(k_sub)))) / k_sub;
        const i64 ty =
            (c.cy + period + static_cast<i64>(std::floor(py * static_cast<f64>(k_sub)))) / k_sub;
        const u32 sx = field.stored(tx, field.window.x), sy = field.stored(ty, field.window.y);
        hits += taken[field.index(sx, sy)];
      }
      if (hits <= k_taken_limit) {
        c.layer = 0;
        break;
      }
    }
    if (c.layer > 0) c.z0_m -= drop * static_cast<f64>(c.layer);
    if (c.layer > 0) c.film = 0.0;
    // Rasterize it: the surface is the highest top at each texel.
    const i64 reach = static_cast<i64>(0.5 * c.a_m / texel) + 2;
    const i64 tcx = c.cx / k_sub, tcy = c.cy / k_sub;
    for (i64 ty = tcy - reach; ty <= tcy + reach; ++ty) {
      const f64 ly = offset_texels(ty, c.cy, size) * texel;
      const u32 sy = field.stored(ty, field.window.y);
      for (i64 tx = tcx - reach; tx <= tcx + reach; ++tx) {
        const f64 lx = offset_texels(tx, c.cx, size) * texel;
        f64 z = 0.0, rise = 0.0;
        bool on_break = false;
        if (!clast_top(c, lx, ly, z, rise, on_break)) continue;
        const u32 i = field.index(field.stored(tx, field.window.x), sy);
        if (c.layer == 0) taken[i] = 1;
        if (static_cast<f32>(z) > top[i]) {
          top[i] = static_cast<f32>(z);
          ids[i] = order[o];
          rises[i] = static_cast<f32>(rise);
          breaks[i] = on_break ? 1 : 0;
        }
      }
    }
  }

  // ---- the texels: a clast's lithology, tone, grain and vein, or the matrix between
  const f64 grain_px = std::max(1.0, static_cast<f64>(p.matrix_um) * 1.0e-6 / texel);
  const u32 grain_k = std::clamp(static_cast<u32>(static_cast<f64>(n) / grain_px + 0.5), 4u, n);
  const u32 speck_k = std::clamp(static_cast<u32>(tile / 0.0015 + 0.5), 4u, n / 2);
  const u32 mottle_k = std::clamp(static_cast<u32>(tile / 0.02 + 0.5), 2u, n / 2);
  const u32 pit_k = std::clamp(static_cast<u32>(tile / 0.006 + 0.5), 4u, n / 2);
  // The matrix's surface: a quarter of a clast's thickness down in a filled bed, raised by the
  // embeddedness; in an openwork bed the floor of the voids, below every layer of blocks — except
  // where fines have filled them (an old talus), cut from a low field at the fines' share.
  const f64 fines = clamp01(permille(p.fines_permille)) * openwork;
  const f64 void_z =
      -(0.25 + 1.5 * openwork + 1.2 * openwork * static_cast<f64>(depth_layers - 1)) * c50;
  const f64 filled_z = (-0.25 + 0.5 * buried) * c50;
  const u32 fines_k = std::clamp(static_cast<u32>(tile / 0.35 + 0.5), 2u, n / 8);
  // Silt and sand: the matrix's own grains pale toward a silt where the bed is embedded.
  constexpr f64 k_silt[3] = {0.30, 0.27, 0.19};
  // A bed's periphyton on its stable tops: a thin brown-olive film (green, gold or brown films and
  // dark brown mats under stable flow, by New Zealand's monitoring guides; the tint chosen).
  constexpr f64 k_film[3] = {0.13, 0.115, 0.045};
  u32 matrix_texels = 0, buried_texels = 0;
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x) {
      const u32 i = field.index(x, y);
      const u32 vx = field.vx(x), vy = field.vy(y);
      Texel& t = field.texels[i];
      f64 fill = 0.0;
      if (fines > 0.0) {
        // One octave: a value noise's values are near uniform there, so the share filled is about
        // the asked one (a pocket's edge is a few centimetres of grading).
        const f64 f = lattice_noise(set.seed ^ 0x66696e6573ull, lattice_of(vx, n, fines_k),
                                    lattice_of(vy, n, fines_k), fines_k);
        fill = smooth01((f - (1.0 - 1.6 * fines)) / 0.05);
      }
      const f64 swell = (fractal_noise(set.seed ^ 0x7377656c6cull, vx, vy, n, 4, 3) - 0.5) * 0.5 *
                        c50 * (1.0 - openwork + fill);
      const f64 matrix_z = (openwork > 0.0 ? mix(void_z, -0.35 * c50, fill) : filled_z) + swell;
      const bool matrix_here = ids[i] == k_no_clast || static_cast<f64>(top[i]) < matrix_z;
      if (matrix_here) {
        ++matrix_texels;
        if (ids[i] != k_no_clast) ++buried_texels;
        // Grains as cells (a grain's edge darker) and a mottle of the grains' mix at 2 cm: a matrix
        // is sand and grit, not a paste.
        const Cell grain = cell_noise(set.seed ^ 0x6d6174726978ull, lattice_of(vx, n, grain_k),
                                      lattice_of(vy, n, grain_k), grain_k);
        const u64 h = grain.id;
        const f64 seam = clamp01((grain.f2 - grain.f1) / 0.3);
        const f64 blotch = lattice_noise(set.seed ^ 0x626c6f74ull, lattice_of(vx, n, mottle_k),
                                         lattice_of(vy, n, mottle_k), mottle_k);
        const Constituent& rock = p.lithologies[pick_constituent(p.lithologies, unit_of(h))];
        const f64 dark = mix(1.0 - 0.75 * openwork, 0.85, fill);
        const f64 tone =
            (0.72 + 0.45 * unit_of(mix64(h))) * (0.7 + 0.3 * seam) * (0.88 + 0.24 * blotch) * dark;
        const f64 silt = 0.6 * buried + 0.7 * fill;
        t.r = static_cast<f32>(mix(permille(rock.r_permille), k_silt[0], silt) * tone);
        t.g = static_cast<f32>(mix(permille(rock.g_permille), k_silt[1], silt) * tone);
        t.b = static_cast<f32>(mix(permille(rock.b_permille), k_silt[2], silt) * tone);
        t.rough = 0.92f;
        t.height = static_cast<f32>(matrix_z + 0.4 * grain_px * texel * seam *
                                                   (0.5 + 0.5 * unit_of(mix64(h ^ 0x5bd1e995ull))));
        continue;
      }
      Clast& c = clasts[ids[i]];
      ++c.shown;
      const Constituent& rock = p.lithologies[c.lith];
      const Cell speck = cell_noise(set.seed ^ 0x737065636bull, lattice_of(vx, n, speck_k),
                                    lattice_of(vy, n, speck_k), speck_k);
      const f64 mottle = lattice_noise(set.seed ^ 0x6d6f74746c65ull, lattice_of(vx, n, mottle_k),
                                       lattice_of(vy, n, mottle_k), mottle_k);
      f64 tone = c.tone * (0.85 + 0.3 * mottle) *
                 (0.94 + 0.12 * unit_of(mix64(speck.id ^ 0x33ull))) *
                 (1.0 + permille(rock.speckle_permille) * (2.0 * unit_of(speck.id) - 1.0));
      // A clast in a lower layer is seen down a gap, in the shade of the ones above, deeper
      // darker; a broken face is fresh rock, paler than the stained outside.
      for (u32 d = 0; d < c.layer; ++d)
        tone *= 0.72;
      if (breaks[i] != 0) tone *= 1.18;
      f64 colour[3] = {permille(rock.r_permille) * tone, permille(rock.g_permille) * tone,
                       permille(rock.b_permille) * tone};
      // The film: strongest on the crown, none down the sides where the flow and the saltating
      // sand scour; and silt draped on a clast's foot where the matrix nearly reaches it.
      if (c.film > 0.0) {
        const f64 k = c.film * smooth01((static_cast<f64>(rises[i]) - 0.25) / 0.5);
        for (u32 j = 0; j < 3; ++j)
          colour[j] = mix(colour[j], k_film[j] * (0.8 + 0.4 * mottle), 0.75 * k);
      }
      if (openwork <= 0.0 && buried > 0.0) {
        const f64 above = (static_cast<f64>(top[i]) - filled_z) / (0.12 * c50);
        const f64 drape = 0.55 * buried * (1.0 - smooth01(above));
        for (u32 j = 0; j < 3; ++j)
          colour[j] = mix(colour[j], k_silt[j], drape);
      }
      if (c.vein_w > 0.0) {
        const i64 tx = static_cast<i64>(vx), ty = static_cast<i64>(vy);
        const f64 lx = offset_texels(tx, c.cx, size) * texel;
        const f64 ly = offset_texels(ty, c.cy, size) * texel;
        const f64 d = std::fabs(lx * c.vein_x + ly * c.vein_y - c.vein_o);
        if (d < c.vein_w) {
          const f64 k = 0.85 * smooth01(2.0 * (1.0 - d / c.vein_w));
          colour[0] = mix(colour[0], 0.62, k);
          colour[1] = mix(colour[1], 0.61, k);
          colour[2] = mix(colour[2], 0.58, k);
        }
      }
      t.r = static_cast<f32>(colour[0]);
      t.g = static_cast<f32>(colour[1]);
      t.b = static_cast<f32>(colour[2]);
      t.rough = static_cast<f32>(clamp01(permille(rock.roughness_permille) - 0.1 * c.rounded +
                                         0.08 * (unit_of(mix64(speck.id)) - 0.5)));
      const f64 pit = fractal_noise(set.seed ^ 0x706974ull, vx, vy, n, pit_k, 2) - 0.5;
      t.height =
          static_cast<f32>(static_cast<f64>(top[i]) + 0.04 * c.c_m * (1.2 - c.rounded) * pit);
    }
  }

  // ---- what a pebble count over every texel reads
  Vector<u32> by_size;
  by_size.reserve(count);
  u32 top_count = 0, under_count = 0;
  u64 shown = 0;
  for (u32 i = 0; i < count; ++i) {
    if (clasts[i].layer == 0) {
      ++top_count;
    } else {
      ++under_count;
    }
    if (clasts[i].shown > 0) {
      by_size.push_back(i);
      shown += clasts[i].shown;
    }
  }
  std::sort(by_size.begin(), by_size.end(), [&](u32 l, u32 r) {
    return clasts[l].b_m != clasts[r].b_m ? clasts[l].b_m < clasts[r].b_m : l < r;
  });
  const f64 marks[3] = {0.16, 0.50, 0.84};
  f64 found[3] = {0, 0, 0};
  u64 run = 0;
  u32 mark = 0;
  for (u32 k = 0; k < by_size.size() && mark < 3; ++k) {
    run += clasts[by_size[k]].shown;
    while (mark < 3 && static_cast<f64>(run) >= marks[mark] * static_cast<f64>(shown)) {
      found[mark] = clasts[by_size[k]].b_m * 1000.0;
      ++mark;
    }
  }
  stats.clasts_top = top_count;
  stats.clasts_under = under_count;
  stats.d16_mm = found[0];
  stats.d50_mm = found[1];
  stats.d84_mm = found[2];
  const f64 texels = static_cast<f64>(n) * static_cast<f64>(n);
  stats.matrix_share = static_cast<f64>(matrix_texels) / texels;
  stats.buried_share = static_cast<f64>(buried_texels) / texels;
  stats.clast_cover = static_cast<f64>(shown) / texels;

  // The hollows' full shade is a clast's thickness down; an openwork bed's voids go further, and
  // their own darkness (and the layers' 0.72 each) carries the rest.
  shade.cavity_m = std::max(0.6 * c50, 2.0 * texel);
  shade.cavity_px = std::clamp(static_cast<u32>(0.5 * d50 / texel + 0.5), 2u, n / 8);
}

}  // namespace engine::matgen::detail

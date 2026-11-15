// Laid work (docs/subsystems/matgen.md, "Masonry"): units in courses along the image's u, each
// course its own height and each unit its own length, the joints between them recessed and
// filled. A flagged floor and a coursed rubble wall are two recipes of it. What a jointed rock
// face cannot say is that somebody laid this: its joints wander and cross, a laid course's run
// level and its perpends stop at the course.
//
// Everything is on the torus of the tile: the courses' heights sum to the tile's side, each
// course's lengths sum to it and start at the course's own phase, and the wander is a periodic
// noise, so the tile repeats without a seam and a windowed build is the same picture rotated.

#include "field.h"

#include <algorithm>

namespace engine::matgen::detail {

namespace {

// Lengths drawn between `lo` and `hi` and scaled so that they sum to `total` exactly enough: as
// many as the range's middle fits.
void parts_of(Rng& rng, f64 lo, f64 hi, f64 total, Vector<f64>& out) {
  out.clear();
  u32 count = static_cast<u32>(total / (0.5 * (lo + hi)) + 0.5);
  if (count < 1) count = 1;
  f64 sum = 0.0;
  for (u32 i = 0; i < count; ++i) {
    out.push_back(rng.range(lo, hi));
    sum += out[i];
  }
  for (f64& v : out)
    v *= total / sum;
}

f64 wrap_m(f64 v, f64 tile) noexcept {
  if (v < 0.0) v += tile;
  if (v >= tile) v -= tile;
  return v;
}

struct Row {
  f64 lo = 0, hi = 0;  // the course's two bed joints, metres up the tile
  f64 phase = 0;       // where its first unit starts along it
  u32 first = 0;       // its units' ends in `ends`, measured from the phase
  u32 count = 0;
};

}  // namespace

void build_masonry(const Set& set, Field& field, SetStats& stats, Shade& shade) {
  const Masonry& m = set.masonry;
  const u32 n = field.n;
  const f64 texel = field.texel_m;
  const f64 tile = texel * static_cast<f64>(n);
  Rng rng(set.seed ^ 0x6d61736f6e7279ull);

  Vector<f64> heights, lengths, ends;
  Vector<Row> rows;
  parts_of(rng, static_cast<f64>(m.course_min_mm) * 0.001,
           static_cast<f64>(m.course_max_mm) * 0.001, tile, heights);
  f64 at = 0.0;
  u32 units = 0;
  for (u32 c = 0; c < heights.size(); ++c) {
    Row row;
    row.lo = at;
    at += heights[c];
    row.hi = c + 1 == heights.size() ? tile : at;
    parts_of(rng, static_cast<f64>(m.unit_min_mm) * 0.001, static_cast<f64>(m.unit_max_mm) * 0.001,
             tile, lengths);
    row.phase = rng.unit() * tile;
    row.first = ends.size();
    row.count = lengths.size();
    f64 run = 0.0;
    for (u32 j = 0; j < row.count; ++j) {
      run += lengths[j];
      ends.push_back(j + 1 == row.count ? tile : run);
    }
    units += row.count;
    rows.push_back(row);
  }

  const f64 half = 0.5e-6 * static_cast<f64>(m.joint_um);
  const f64 depth = 1.0e-6 * static_cast<f64>(m.joint_depth_um);
  const f64 arris = std::max(1.0e-6 * static_cast<f64>(m.arris_um), texel);
  const f64 drop = 1.0e-6 * static_cast<f64>(m.arris_drop_um);
  const f64 corner = 1.0e-6 * static_cast<f64>(m.round_um);
  const f64 wander = 1.0e-6 * static_cast<f64>(m.wander_um);
  const f64 lip = 1.0e-6 * static_cast<f64>(m.lip_um);
  const f64 relief = 1.0e-6 * static_cast<f64>(m.relief_um);
  // The wander's swell is about a hand across: the lattice's nodes across the tile, a power of
  // two so the coordinate is exact.
  u32 wander_k = 4;
  while (wander_k * 2 <= n / 8 && tile / static_cast<f64>(wander_k) > 0.12)
    wander_k *= 2;
  const u32 mottle_k = std::min(n / 4, 16u);
  const f64 joint_colour[3] = {permille(m.joint_r_permille), permille(m.joint_g_permille),
                               permille(m.joint_b_permille)};
  u32 joint_texels = 0;

  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x) {
      const u32 vx = field.vx(x), vy = field.vy(y);
      const f64 u = (static_cast<f64>(vx % n) + 0.5) * texel;
      const f64 v = (static_cast<f64>(vy % n) + 0.5) * texel;
      const f64 wu = (fractal_noise(set.seed + 11, vx, vy, n, wander_k, 2) - 0.5) * 2.0 * wander;
      const f64 wv = (fractal_noise(set.seed + 23, vx, vy, n, wander_k, 2) - 0.5) * 2.0 * wander;
      const f64 pv = wrap_m(v + wv, tile);
      u32 c = 0;
      while (c + 1 < rows.size() && pv >= rows[c].hi)
        ++c;
      const Row& row = rows[c];
      const f64 dv = std::min(pv - row.lo, row.hi - pv);
      const f64 pu = wrap_m(wrap_m(u + wu, tile) - row.phase, tile);
      u32 j = 0;
      while (j + 1 < row.count && pu >= ends[row.first + j])
        ++j;
      const f64 begin = j == 0 ? 0.0 : ends[row.first + j - 1];
      const f64 du = std::min(pu - begin, ends[row.first + j] - pu);
      // The distance in from the unit's outline, its corners rounded: negative past the corner's
      // arc, where the joints of a crossing widen.
      f64 d = std::min(du, dv);
      if (du < corner && dv < corner) {
        const f64 a = corner - du, b = corner - dv;
        d = corner - std::sqrt(a * a + b * b);
      }
      const u64 id = hash3(set.seed, c, j);
      const f64 speck = unit_of(hash3(set.seed + 7, vx % n, vy % n));
      const f64 mottle = fractal_noise(set.seed + 9, vx, vy, n, mottle_k, 3);
      Texel& t = field.texels[field.index(x, y)];
      if (d < half) {
        // The joint's filling: level, a little rough, darker toward its middle where it holds
        // the dirt.
        const f64 in = clamp01(1.0 - d / half);
        const f64 tone = (0.92 + 0.16 * speck) * (1.0 - 0.15 * in);
        t.r = static_cast<f32>(joint_colour[0] * tone);
        t.g = static_cast<f32>(joint_colour[1] * tone);
        t.b = static_cast<f32>(joint_colour[2] * tone);
        t.rough = static_cast<f32>(permille(m.joint_roughness_permille));
        t.height = static_cast<f32>(-depth + (speck - 0.5) * 0.0006);
        ++joint_texels;
        continue;
      }
      const Constituent& stone = m.stones[pick_constituent(m.stones, unit_of(id))];
      const f64 top = smooth01((d - half) / arris);  // 0 at the unit's edge, 1 on its top
      const f64 tone = 1.0 + (unit_of(mix64(id)) * 2.0 - 1.0) * permille(stone.tone_permille);
      const f64 grain =
          1.0 + (mottle - 0.5) * 0.3 + (speck - 0.5) * 2.0 * permille(stone.speckle_permille);
      // The arris holds what the joint does: a little darker toward the edge.
      const f64 shade_k = tone * grain * (0.8 + 0.2 * top);
      t.r = static_cast<f32>(clamp01(permille(stone.r_permille) * shade_k));
      t.g = static_cast<f32>(clamp01(permille(stone.g_permille) * shade_k));
      t.b = static_cast<f32>(clamp01(permille(stone.b_permille) * shade_k));
      t.rough = static_cast<f32>(permille(stone.roughness_permille));
      const f64 stand = (unit_of(mix64(id ^ 0x5555555555555555ull)) * 2.0 - 1.0) * lip;
      t.height = static_cast<f32>(stand + (mottle - 0.5) * 2.0 * relief +
                                  (speck - 0.5) * 0.3 * relief - drop * (1.0 - top) * (1.0 - top));
    }
  }
  stats.clasts_drawn = units;
  stats.clasts_top = units;
  stats.clast_cover =
      1.0 - static_cast<f64>(joint_texels) / (static_cast<f64>(n) * static_cast<f64>(n));
  shade.cavity_m = std::max(depth, 0.002);
  shade.cavity_px =
      std::max(2u, static_cast<u32>(1.0e-6 * static_cast<f64>(m.joint_um) / texel + 0.5));
}

}  // namespace engine::matgen::detail

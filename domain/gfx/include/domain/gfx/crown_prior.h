#pragma once

// **A crown's shade as a prior** (docs/subsystems/renderer.md, "Indirect light: the world-space
// cache", "Stage 7"; the sources in docs/experiments/crown-shade-grounding.md; roadmap R69 stage
// 7).
//
// What is fine inside a tree's crown is the crown's own shade, and it is known from the tree alone:
// the diffuse light that reaches a point inside foliage is what the leaves between it and the open
// let through, exp(-G u s) along a path of length s through a leaf area density u (Monsi and Saeki
// 1953; Ross 1981; G = 0.5 for leaves of no preferred direction), averaged over the directions the
// light comes from. So each mesh that has a crown carries a small grid over its own bounds, two
// bytes a cell: the mean of that transmitted fraction over the upper hemisphere and over the lower
// one, from the mesh's own leaves — their area gathered into the grid's cells, marched through
// along a fixed set of directions. The cache then holds, for foliage, the light **over** this
// prior — the field outside the crown's own shade, which is smooth — and a foliage pixel
// multiplies its own prior back (gi_cache.slang, `gi_crown_prior`).
//
// The grid is per mesh, not per vertex: it costs no byte of any vertex, it is the same for every
// LOD and every instance of the mesh (optical depth does not change under a uniform scale: the
// density falls as the path grows), and a trunk under the crown reads the same field its leaves do.
//
// Everything here is plain IEEE arithmetic, the square root, and `engine::det`'s exp, sin and
// cos: the same bytes
// from every toolchain.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/deterministic.h>
#include <core/math/math.h>

#include <cmath>
#include <span>

namespace engine::gfx {

// One per mesh of the scene, GPU-mirrored (gi_cache.slang, `CrownPrior`): where the mesh's grid is
// in mesh space and in the table's bytes. `offset` is from the table's start; `k_no_crown` for a
// mesh with no crown.
struct CrownPriorDesc {
  f32 origin[3] = {0.0f, 0.0f, 0.0f};  // mesh space: the centre of the grid's first cell
  u32 offset = 0xffffffffu;
  f32 inv_cell[3] = {0.0f, 0.0f, 0.0f};  // cells a metre, each axis
  u32 dims = 0;                          // x | y << 8 | z << 16
};
static_assert(sizeof(CrownPriorDesc) == 32);
inline constexpr u32 k_no_crown = 0xffffffffu;

// The projection of unit leaf area on a plane across the path, for leaves of no preferred
// direction (a spherical leaf angle distribution): Ross 1981's G; Campbell and Norman 1998,
// eq. 15.4 with x = 1. The transmitted fraction along a path of s metres through u square metres of
// one-sided leaf a cubic metre is exp(-k_crown_g * u * s).
inline constexpr f64 k_crown_g = 0.5;
// The share of a leaf card's triangle that is leaf: the cards are alpha-tested, and the tree
// solver gives a card `leaf_width` of its square as blade (0.7 on its default broadleaf;
// flora.md, "The budget of a crown").
inline constexpr f64 k_crown_card_fill = 0.7;
// A grid's cells an axis at most, and the smallest cell: 16 cells of at least a quarter metre, so
// a 10 m crown has 0.6 m cells and a mesh's grid is at most 8 KiB.
inline constexpr u32 k_crown_cells = 16;
inline constexpr f32 k_crown_cell_min_m = 0.25f;
// A mesh's leaves must stand this tall for a grid to be made: a crown, not a sward or a litter of
// leaves on the ground (whose shade their own solvers already paint).
inline constexpr f32 k_crown_min_height_m = 1.5f;
// ... and its leaf triangles must be leaves: under this mean area, square metres. A leaf card is
// centimetres (a 30 cm card's triangle is 0.045 m2); a card past that is a picture of many leaves
// — a merged card of a far form, a far forest's impostor — whose area is not leaf area: counted
// as leaf it gave the forest crowns of 10 to 170 m2 of leaf a cubic metre, every one of them on
// the floor.
inline constexpr f32 k_crown_card_max_m2 = 0.05f;
// The prior is never read below this: a sample divides by it, so its noise grows as the prior
// falls, and past exp(-2.8) the light deep in a crown is the leaves' own scattering more than the
// sky's, which no transmitted fraction describes (gi_cache.slang, `k_gi_crown_floor`).
inline constexpr f32 k_crown_floor = 1.0f / 16.0f;
// The directions a hemisphere's mean is taken over: `k_crown_rings` rings of equal solid angle,
// `k_crown_spokes` round each, every other ring turned half a step.
inline constexpr u32 k_crown_rings = 3;
inline constexpr u32 k_crown_spokes = 4;

// What one grid was built from, for a log line and a page's table.
struct CrownPriorStats {
  f64 leaf_area_m2 = 0.0;    // one-sided, the cards' triangles times `k_crown_card_fill`
  f64 leaf_volume_m3 = 0.0;  // of the cells that hold a leaf
  f32 mean_up = 1.0f;        // over the cells that hold a leaf, by leaf area
  f32 min_up = 1.0f;
};

// The grid's shape over a mesh's bounds: the cells of each axis and their size.
struct CrownGrid {
  u32 dims[3] = {1, 1, 1};
  f32 cell[3] = {1.0f, 1.0f, 1.0f};
  Vec3 lo{};
  constexpr u32 cells() const noexcept { return dims[0] * dims[1] * dims[2]; }
  constexpr u32 index(u32 x, u32 y, u32 z) const noexcept {
    return (z * dims[1] + y) * dims[0] + x;
  }
};

inline CrownGrid crown_grid(Vec3 lo, Vec3 hi) noexcept {
  CrownGrid g;
  g.lo = lo;
  const f32 extent[3] = {hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
  f32 widest = extent[0] > extent[1] ? extent[0] : extent[1];
  widest = widest > extent[2] ? widest : extent[2];
  const f32 target = widest / static_cast<f32>(k_crown_cells) > k_crown_cell_min_m
                         ? widest / static_cast<f32>(k_crown_cells)
                         : k_crown_cell_min_m;
  for (u32 a = 0; a < 3; ++a) {
    u32 n = static_cast<u32>(extent[a] / target + 0.999f);
    n = n < 1 ? 1 : (n > k_crown_cells ? k_crown_cells : n);
    g.dims[a] = n;
    g.cell[a] = extent[a] > 0.0f ? extent[a] / static_cast<f32>(n) : target;
  }
  return g;
}

// The grid of one mesh: `corners` holds its leaf triangles, three corners each, in mesh space, and
// `lo`..`hi` the bounds of the whole mesh (its trunk too, which reads the grid under the crown).
// Appends `grid.cells()` pairs of bytes to `bytes` — the upper hemisphere's mean transmitted
// fraction and the lower one's, each times 255 — padded to a multiple of four, and fills `desc`
// but for its `offset`, which is the caller's (where in its table the bytes went).
inline void build_crown_prior(std::span<const Vec3> corners, Vec3 lo, Vec3 hi, CrownPriorDesc& desc,
                              Vector<u8>& bytes, CrownPriorStats* stats = nullptr) {
  const CrownGrid g = crown_grid(lo, hi);
  const u32 cells = g.cells();
  // The leaf area of each cell, at each triangle's centroid: a card is centimetres and a cell
  // decimetres at the least.
  Vector<f32> density;
  density.resize(cells, 0.0f);
  f64 area_total = 0.0;
  for (usize t = 0; t + 2 < corners.size(); t += 3) {  // a span: usize
    const Vec3 a = corners[t];
    const Vec3 b = corners[t + 1];
    const Vec3 c = corners[t + 2];
    const Vec3 n = cross(b - a, c - a);
    const f64 area = 0.5 *
                     std::sqrt(static_cast<f64>(n.x) * static_cast<f64>(n.x) +
                               static_cast<f64>(n.y) * static_cast<f64>(n.y) +
                               static_cast<f64>(n.z) * static_cast<f64>(n.z)) *
                     k_crown_card_fill;
    const Vec3 m = (a + b + c) * (1.0f / 3.0f);
    const f32 at[3] = {(m.x - g.lo.x) / g.cell[0], (m.y - g.lo.y) / g.cell[1],
                       (m.z - g.lo.z) / g.cell[2]};
    u32 cell[3];
    for (u32 k = 0; k < 3; ++k) {
      const i32 v = static_cast<i32>(at[k]);
      cell[k] =
          v < 0 ? 0u : (static_cast<u32>(v) >= g.dims[k] ? g.dims[k] - 1 : static_cast<u32>(v));
    }
    density[g.index(cell[0], cell[1], cell[2])] += static_cast<f32>(area);
    area_total += area;
  }
  const f32 cell_volume = g.cell[0] * g.cell[1] * g.cell[2];
  u32 leafy = 0;
  for (u32 i = 0; i < cells; ++i) {
    if (density[i] > 0.0f) ++leafy;
    density[i] /= cell_volume;  // square metres of leaf a cubic metre
  }
  // The directions of the upper hemisphere, equal solid angle each: the lower one's are these
  // mirrored. A ring's height is uniform in the cosine, which is what makes its solid angle equal.
  Vec3 directions[k_crown_rings * k_crown_spokes];
  for (u32 r = 0; r < k_crown_rings; ++r) {
    const f64 y = (static_cast<f64>(r) + 0.5) / static_cast<f64>(k_crown_rings);
    const f64 s = std::sqrt(1.0 - y * y);
    for (u32 k = 0; k < k_crown_spokes; ++k) {
      const f64 phi = 6.283185307179586 * (static_cast<f64>(k) + 0.5 * static_cast<f64>(r & 1u)) /
                      static_cast<f64>(k_crown_spokes);
      directions[r * k_crown_spokes + k] =
          Vec3{static_cast<f32>(s * det::cos(phi)), static_cast<f32>(y),
               static_cast<f32>(s * det::sin(phi))};
    }
  }
  f32 smallest = g.cell[0] < g.cell[1] ? g.cell[0] : g.cell[1];
  smallest = smallest < g.cell[2] ? smallest : g.cell[2];
  const f32 step = 0.5f * smallest;
  const u32 first = static_cast<u32>(bytes.size());
  bytes.resize(first + ((cells * 2 + 3) & ~3u), u8{0});
  f64 up_sum = 0.0;
  f64 up_weight = 0.0;
  f32 up_min = 1.0f;
  // The optical depth from `p` along `d` to the grid's edge: the cells' densities at half steps.
  const auto depth = [&](Vec3 p, Vec3 d) noexcept {
    f32 sum = 0.0f;
    for (u32 i = 0; i < 2048; ++i) {
      const f32 t = (static_cast<f32>(i) + 0.5f) * step;
      const f32 x = (p.x + d.x * t - g.lo.x) / g.cell[0];
      const f32 y = (p.y + d.y * t - g.lo.y) / g.cell[1];
      const f32 z = (p.z + d.z * t - g.lo.z) / g.cell[2];
      if (x < 0.0f || y < 0.0f || z < 0.0f) break;
      const u32 cx = static_cast<u32>(x);
      const u32 cy = static_cast<u32>(y);
      const u32 cz = static_cast<u32>(z);
      if (cx >= g.dims[0] || cy >= g.dims[1] || cz >= g.dims[2]) break;
      sum += density[g.index(cx, cy, cz)];
    }
    return static_cast<f64>(sum) * static_cast<f64>(step) * k_crown_g;
  };
  for (u32 z = 0; z < g.dims[2]; ++z) {
    for (u32 y = 0; y < g.dims[1]; ++y) {
      for (u32 x = 0; x < g.dims[0]; ++x) {
        const Vec3 p{g.lo.x + (static_cast<f32>(x) + 0.5f) * g.cell[0],
                     g.lo.y + (static_cast<f32>(y) + 0.5f) * g.cell[1],
                     g.lo.z + (static_cast<f32>(z) + 0.5f) * g.cell[2]};
        f64 up = 0.0;
        f64 down = 0.0;
        for (const Vec3& d : directions) {
          up += det::exp(-depth(p, d));
          down += det::exp(-depth(p, Vec3{d.x, -d.y, d.z}));
        }
        up /= static_cast<f64>(k_crown_rings * k_crown_spokes);
        down /= static_cast<f64>(k_crown_rings * k_crown_spokes);
        const u32 i = g.index(x, y, z);
        bytes[first + i * 2] = static_cast<u8>(up * 255.0 + 0.5);
        bytes[first + i * 2 + 1] = static_cast<u8>(down * 255.0 + 0.5);
        if (density[i] > 0.0f) {
          up_sum += up * static_cast<f64>(density[i]);
          up_weight += static_cast<f64>(density[i]);
          up_min = static_cast<f32>(up) < up_min ? static_cast<f32>(up) : up_min;
        }
      }
    }
  }
  desc.origin[0] = g.lo.x + 0.5f * g.cell[0];
  desc.origin[1] = g.lo.y + 0.5f * g.cell[1];
  desc.origin[2] = g.lo.z + 0.5f * g.cell[2];
  desc.inv_cell[0] = 1.0f / g.cell[0];
  desc.inv_cell[1] = 1.0f / g.cell[1];
  desc.inv_cell[2] = 1.0f / g.cell[2];
  desc.dims = g.dims[0] | (g.dims[1] << 8) | (g.dims[2] << 16);
  if (stats != nullptr) {
    stats->leaf_area_m2 = area_total;
    stats->leaf_volume_m3 = static_cast<f64>(leafy) * static_cast<f64>(cell_volume);
    stats->mean_up = up_weight > 0.0 ? static_cast<f32>(up_sum / up_weight) : 1.0f;
    stats->min_up = up_min;
  }
}

// The two means at a mesh-space point, trilinear between the cells' centres and held at the grid's
// edge: the shader's read (`gi_crown_fetch`), on the bytes `build_crown_prior` wrote for `desc`.
struct CrownPriorSample {
  f32 up = 1.0f;
  f32 down = 1.0f;
};
inline CrownPriorSample crown_prior_sample(const CrownPriorDesc& desc, const u8* grid,
                                           Vec3 p) noexcept {
  const u32 dims[3] = {desc.dims & 0xffu, (desc.dims >> 8) & 0xffu, (desc.dims >> 16) & 0xffu};
  const f32 at[3] = {(p.x - desc.origin[0]) * desc.inv_cell[0],
                     (p.y - desc.origin[1]) * desc.inv_cell[1],
                     (p.z - desc.origin[2]) * desc.inv_cell[2]};
  u32 base[3];
  u32 next[3];
  f32 f[3];
  for (u32 a = 0; a < 3; ++a) {
    const f32 top = static_cast<f32>(dims[a] - 1);
    const f32 v = at[a] < 0.0f ? 0.0f : (at[a] > top ? top : at[a]);
    base[a] = static_cast<u32>(v);
    next[a] = base[a] + 1 < dims[a] ? base[a] + 1 : base[a];
    f[a] = v - static_cast<f32>(base[a]);
  }
  CrownPriorSample out;
  out.up = 0.0f;
  out.down = 0.0f;
  for (u32 corner = 0; corner < 8; ++corner) {
    const u32 x = (corner & 1u) != 0 ? next[0] : base[0];
    const u32 y = (corner & 2u) != 0 ? next[1] : base[1];
    const u32 z = (corner & 4u) != 0 ? next[2] : base[2];
    const f32 w = ((corner & 1u) != 0 ? f[0] : 1.0f - f[0]) *
                  ((corner & 2u) != 0 ? f[1] : 1.0f - f[1]) *
                  ((corner & 4u) != 0 ? f[2] : 1.0f - f[2]);
    const usize i = (static_cast<usize>(z) * dims[1] + y) * dims[0] + x;
    out.up += w * static_cast<f32>(grid[i * 2]) * (1.0f / 255.0f);
    out.down += w * static_cast<f32>(grid[i * 2 + 1]) * (1.0f / 255.0f);
  }
  return out;
}

// The prior of a surface whose normal rises `normal_y` (the shader's `gi_crown_prior`): a surface
// takes its light from the hemisphere about its normal, of which (1 + n.y) / 2 is above the
// horizon, never under `k_crown_floor`.
constexpr f32 crown_prior(CrownPriorSample s, f32 normal_y) noexcept {
  const f32 t = 0.5f + 0.5f * normal_y;
  const f32 v = s.down + (s.up - s.down) * t;
  return v > k_crown_floor ? v : k_crown_floor;
}

}  // namespace engine::gfx

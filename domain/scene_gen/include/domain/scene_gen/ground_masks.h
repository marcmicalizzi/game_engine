#pragma once

// **A ground's material masks** (docs/subsystems/tileset.md, "The mask level"; renderer.md, "Ground
// materials by place"): the seam a ground provider hands the renderer its per-tile material weights
// through, as `tile_source` hands it heights (ADR-0057), so the renderer reads an authored ground's
// masks without naming the capability that built them.
//
// A mask level is a window of the world's lattice at `spacing_mm`, cut into tiles of `cells` cells
// a side. Tile (tx, tz) holds its own points, `cells + 1` a side from point (tx * cells, tz *
// cells):
// **its edge points are its neighbours' edge points**, stored in both, and identical in palette
// space — which is what makes a seam exact (below). Each tile names up to four palette layers (its
// table) and carries, per point, four 8-bit weights in that table's order and an 8-bit wetness, at
// every mip of the tile down to two points a side.
//
// The words of one tile, the layout the GPU reads as it is (`terrain_material.slang` mirrors it):
//   [0]                    the table: palette index of channel c in byte c, 0xFF for none
//   [1, 1 + P)             RGBA8 weights per point, mip 0's rows of x in order of z, then mip
//   1's... [1 + P, 1 + P + W)     wetness, one byte a point in the same order, four to a word
// with P the points of every mip and W = ceil(P / 4).
//
// **Why one tile answers a pixel.** A point's cell lies in one tile, and its four corners are that
// tile's own points, so a lookup reads one table. Just across a seam the other tile's corners are
// read; on the seam both tiles hold the same edge points in palette space (the builder refuses a
// tile whose footprint names more layers than its table can hold, so no edge point loses a layer),
// so the weights are continuous across it, and the same at it from either side, bit for bit. A mip
// point on a tile's edge is filtered along the edge alone, from edge points both tiles hold, so the
// seam stays exact at every mip.

#include <core/base/types.h>

#include <span>

namespace engine::scene_gen {

inline constexpr u32 k_mask_channels = 4;
inline constexpr u32 k_mask_no_layer = 0xFFu;
inline constexpr u32 k_mask_max_palette = 254;

// Points a side of mip `mip` of a tile of `cells` (a power of two): cells / 2^mip + 1.
constexpr u32 mask_mip_side(u32 cells, u32 mip) noexcept { return (cells >> mip) + 1u; }
// Mips a tile of `cells` holds: down to two points a side.
constexpr u32 mask_mips(u32 cells) noexcept {
  u32 m = 1;
  while ((cells >> m) >= 1u)
    ++m;
  return m;
}
// The first point of mip `mip` in a tile's point order.
constexpr u32 mask_mip_offset(u32 cells, u32 mip) noexcept {
  u32 off = 0;
  for (u32 m = 0; m < mip; ++m)
    off += mask_mip_side(cells, m) * mask_mip_side(cells, m);
  return off;
}
constexpr u32 mask_points(u32 cells) noexcept { return mask_mip_offset(cells, mask_mips(cells)); }
constexpr u32 mask_tile_words(u32 cells) noexcept {
  return 1u + mask_points(cells) + (mask_points(cells) + 3u) / 4u;
}

struct GroundMaskInfo {
  i64 spacing_mm = 0;  // between points
  u32 cells = 0;       // a tile side, a power of two
  i32 tile_min_x = 0, tile_min_z = 0;
  i32 tile_max_x = -1, tile_max_z = -1;  // inclusive: the tiles the level may hold
  u32 palette = 0;                       // palette layers the tables index
  u32 budget = 0;                        // layers a tile may name, 1 to 4
  u64 hash = 0;                          // the level's identity: its key and its tiles' hashes
  f32 wetness = 0;                       // the scene's, everywhere
  f32 height_blend = 0;                  // the depth of the height blend; 0 linear
  u32 resident_mib = 256;
};

struct GroundMaskOps {
  // Manifest presence only, no payload I/O. A resident upload counts this before reading tiles.
  bool (*present)(const void* state, i32 tx, i32 tz) noexcept = nullptr;
  bool (*info)(const void* state, GroundMaskInfo& out) noexcept = nullptr;
  // Tile (tx, tz)'s words (`mask_tile_words(cells)` of them). False: no tile there (no layer is
  // authored on it, or it lies past the level), and nothing written.
  bool (*tile)(const void* state, i32 tx, i32 tz, std::span<u32> words) noexcept = nullptr;
};

// Non-owning: valid while the provider that returned it lives.
struct GroundMasks {
  const GroundMaskOps* ops = nullptr;
  const void* state = nullptr;
  bool valid() const noexcept { return ops != nullptr; }
};

// **The CPU's reading of a tile**, the shader's formula (`terrain_mask_sample` in
// terrain_material.slang): the weights of the table's four channels and the wetness at
// (`x_mm`, `z_mm`) given as a whole millimetre and a fraction of one, at mip level `level` (its
// fraction blends two mips), from the words of the tile that holds the point — `mask_tile_of`.
struct MaskSample {
  f32 weight[k_mask_channels]{};
  f32 wetness = 0;
};
struct MaskTileRef {
  i32 tx = 0, tz = 0;
  i64 point_x = 0, point_z = 0;  // the cell's corner, in the tile's mip-0 points (0..cells-1)
  f64 frac_x = 0, frac_z = 0;    // the place in that cell, [0, 1)
};
inline i64 mask_floor_div(i64 a, i64 b) noexcept {
  const i64 q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
// Which tile, cell and place (`x_mm` + `x_frac` millimetres): the floor's tile, so a point exactly
// on a seam is the tile after it at fraction zero, whose answer is the tile before's at one.
inline MaskTileRef mask_tile_of(const GroundMaskInfo& info, i64 x_mm, f64 x_frac, i64 z_mm,
                                f64 z_frac) noexcept {
  MaskTileRef r;
  const i64 s = info.spacing_mm;
  const i64 c = info.cells;
  const auto axis = [&](i64 mm, f64 frac, i32& tile, i64& point, f64& f) {
    const i64 q = mask_floor_div(mm, s);
    const f64 t = (static_cast<f64>(mm - q * s) + frac) / static_cast<f64>(s);
    i64 whole = q;
    f64 part = t;
    if (part >= 1.0) {
      whole += 1;
      part -= 1.0;
    }
    tile = static_cast<i32>(mask_floor_div(whole, c));
    point = whole - static_cast<i64>(tile) * c;
    f = part;
  };
  axis(x_mm, x_frac, r.tx, r.point_x, r.frac_x);
  axis(z_mm, z_frac, r.tz, r.point_z, r.frac_z);
  return r;
}
inline MaskSample mask_sample(const GroundMaskInfo& info, std::span<const u32> words,
                              const MaskTileRef& at, f32 level) noexcept {
  MaskSample out;
  const u32 mips = mask_mips(info.cells);
  const f32 lv = level < 0.0f ? 0.0f : level;
  u32 m0 = static_cast<u32>(lv);
  if (m0 > mips - 1) m0 = mips - 1;
  const u32 m1 = m0 + 1 < mips ? m0 + 1 : m0;
  const f32 blend = m0 == mips - 1 ? 0.0f : lv - static_cast<f32>(m0);
  const u32 points = mask_points(info.cells);
  for (u32 k = 0; k < 2; ++k) {
    const u32 m = k == 0 ? m0 : m1;
    const f32 share = k == 0 ? 1.0f - blend : blend;
    if (share == 0.0f) continue;
    const u32 side = mask_mip_side(info.cells, m);
    const f64 scale = 1.0 / static_cast<f64>(1u << m);
    const f64 px = (static_cast<f64>(at.point_x) + at.frac_x) * scale;
    const f64 pz = (static_cast<f64>(at.point_z) + at.frac_z) * scale;
    u32 cx = static_cast<u32>(px);
    u32 cz = static_cast<u32>(pz);
    if (cx > side - 2) cx = side - 2;
    if (cz > side - 2) cz = side - 2;
    const f32 fx = static_cast<f32>(px - cx);
    const f32 fz = static_cast<f32>(pz - cz);
    const u32 base = mask_mip_offset(info.cells, m);
    const f32 wq[4] = {(1 - fx) * (1 - fz), fx * (1 - fz), (1 - fx) * fz, fx * fz};
    const u32 idx[4] = {base + cz * side + cx, base + cz * side + cx + 1,
                        base + (cz + 1) * side + cx, base + (cz + 1) * side + cx + 1};
    for (u32 q = 0; q < 4; ++q) {
      const u32 w = words[1 + idx[q]];
      for (u32 ch = 0; ch < k_mask_channels; ++ch)
        out.weight[ch] += share * wq[q] * static_cast<f32>((w >> (8 * ch)) & 0xFFu) / 255.0f;
      const u32 wet = words[1 + points + idx[q] / 4] >> (8 * (idx[q] % 4));
      out.wetness += share * wq[q] * static_cast<f32>(wet & 0xFFu) / 255.0f;
    }
  }
  return out;
}

}  // namespace engine::scene_gen

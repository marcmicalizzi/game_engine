#pragma once

// **Indirect light: the world-space cache** (docs/subsystems/renderer.md, "Indirect light: the
// world-space cache"; roadmap R69 stage 1). Mirrors `domain/gfx/shaders/gi_cache.slang`.
//
// A hash grid of cells keyed by where a surface is in the world — never by an absolute float
// position (ADR-0053): the eye's integer `WorldCell` and the surface's offset from the eye, rounded
// to whole millimetres in 64-bit integers as the terrain stage forms its lattice — at a cell size
// that grows with the distance from the eye, and by the face of the normal's dominant axis, so a
// wall's two sides and a floor and the wall above it never share a cell. Each cell holds what the
// hemisphere above its surfaces returns from the environment through the frame's acceleration
// structures, as a ratio to the sky's own nine-harmonic ambient at the same normal: the resolve
// reads it back multiplied by that ambient's luminance, so in open space the read is the harmonics
// and under a canopy it is what the canopy lets through.
//
// What a frame does with it (`systems/renderer`, two compute passes before the resolve):
//   - **gi sample** (`gi_sample_main`): one pixel in every `ResolveParams::gi_stride` of a view's
//     visibility region, rotating with the frame, is reconstructed as the resolve does it, finds
//     its cell at its level and at the next coarser one (inserting them), traces one
//     cosine-weighted ray against the frame's top-level structure with the shadow rays' masks and
//     micromaps, and adds the ratio, in fixed point, to both cells' sums for this frame. Integer
//     sums are commutative, so the order threads reach them in changes nothing.
//   - **gi fold** (`gi_fold_main`): one thread a cell folds the frame's sum into the cell's
//     running mean — an exponential average whose rate the sample count sets — clears the sum, and
//     frees a cell nothing has touched for `k_gi_max_age` frames.
// The resolve then reads the mean trilinearly over the eight cells round the pixel at its level
// (the next coarser level where those are empty, the harmonics where both are).

#include <core/base/types.h>

#include <cmath>

namespace engine::gfx {

// One cell, eight words:
//   [0] key: the hash's upper word with its low bit set (0 is an empty slot)
//   [1] stamp: the last frame a sample reached it
//   [2] samples this frame
//   [3..5] this frame's sums of the ratio, r g b, `k_gi_fraction_bits` of fraction
//   [6] the mean ratio's r and g as two halves
//   [7] the mean ratio's b as a half in the low 16 bits, the samples it averages in the high 16
struct GiCell {
  u32 key = 0;
  u32 stamp = 0;
  u32 frame_count = 0;
  u32 frame_sum[3] = {0, 0, 0};
  u32 mean_rg = 0;
  u32 mean_b_count = 0;
};
static_assert(sizeof(GiCell) == 32);

inline constexpr u32 k_gi_cell_bytes = 32;
// Slots a key may land in: the eight cells of its bucket, which a lookup scans whole, so a freed
// slot never breaks another key's chain.
inline constexpr u32 k_gi_bucket = 8;
// The levels: 25 mm at level 0, doubling to 6.4 m at level 8. Every size divides the world's 64 m
// cell (64,000 mm), so a scene moved by whole cells moves every key by whole cells of its own.
inline constexpr u32 k_gi_levels = 9;
inline constexpr i64 k_gi_cell_min_mm = 25;
inline constexpr f32 k_gi_cell_min_m = 0.025f;
// The sums' fixed point: 10 bits of fraction under a ratio held below 16, so a cell takes up to
// 2^18 samples in one frame before a word could carry.
inline constexpr u32 k_gi_fraction_bits = 10;
inline constexpr f32 k_gi_ratio_max = 15.99f;
// The running mean's history: a cell's average weighs this many samples at most, so a change in
// the scene or the sky reaches it within a few frames of samples.
inline constexpr u32 k_gi_max_samples = 256;
// Frames without a sample after which the fold frees a cell.
inline constexpr u32 k_gi_max_age = 240;
inline constexpr u32 k_gi_workgroup = 64;
// The passes' dispatches are two-dimensional past this many groups in x (a device need take no
// more than 65,535): thread `id.y * k_gi_dispatch_pitch * k_gi_workgroup + id.x`.
inline constexpr u32 k_gi_dispatch_pitch = 32768;

// `ResolveParams::gi_flags`.
inline constexpr u32 k_gi_read = 1u;    // the resolve reads the cache for its ambient
inline constexpr u32 k_gi_bounce = 2u;  // a ray that hits takes the bounce (stage 1b; unused yet)
inline constexpr u32 k_gi_reset = 4u;   // the fold clears every cell (the cache's first frame)

// The cells a budget of `bytes` holds: a power of two, at least one bucket.
constexpr u32 gi_cell_count(u64 bytes) noexcept {
  u64 cells = bytes / k_gi_cell_bytes;
  if (cells > (u64{1} << 31)) cells = u64{1} << 31;
  u64 count = k_gi_bucket;
  while (count * 2 <= cells)
    count *= 2;
  return static_cast<u32>(count);
}

// The level of a surface `distance_m` from the eye: the smallest whose cell is at least
// `distance_m * cell_scale` wide, at most the last. Compared against the levels' sizes rather than
// through a logarithm, so the shader's rule and this one are the same comparisons.
inline u32 gi_level(f32 distance_m, f32 cell_scale) noexcept {
  const f32 size = std::fmax(distance_m * cell_scale, k_gi_cell_min_m);
  u32 level = 0;
  while (level + 1 < k_gi_levels && size > k_gi_cell_min_m * static_cast<f32>(1u << level))
    ++level;
  return level;
}

constexpr i64 gi_cell_mm(u32 level) noexcept { return k_gi_cell_min_mm << level; }

// The face of a normal: its dominant axis and that axis's sign, 0..5 for +x -x +y -y +z -z.
inline u32 gi_face(f32 x, f32 y, f32 z) noexcept {
  const f32 ax = std::fabs(x);
  const f32 ay = std::fabs(y);
  const f32 az = std::fabs(z);
  if (ax >= ay && ax >= az) return x >= 0.0f ? 0u : 1u;
  if (ay >= az) return y >= 0.0f ? 2u : 3u;
  return z >= 0.0f ? 4u : 5u;
}

// One axis of a surface point in whole millimetres of the world: the eye's cell times the cell's
// 64,000 mm plus the eye's offset in it and the point's offset from the eye, rounded. The shader's
// `gi_axis_mm`, operation for operation.
inline i64 gi_axis_mm(i32 eye_cell, f32 eye_local, f32 from_eye) noexcept {
  const f32 local = eye_local + from_eye;
  return i64{eye_cell} * 64000 + static_cast<i64>(std::floor(local * 1000.0f + 0.5f));
}

// Floor division, as `terrain_floor_div` in scene.slang.
constexpr i64 gi_floor_div(i64 a, i64 b) noexcept {
  const i64 q = a / b;
  return (q * b != a && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// The hash of a cell: its three coordinates at `level`, the level and the face. The shader's
// `gi_hash`; the bucket is its low word, the key its high word with the low bit set.
constexpr u64 gi_mix(u64 h) noexcept {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ull;
  h ^= h >> 33;
  return h;
}
constexpr u64 gi_hash(i64 x, i64 y, i64 z, u32 level, u32 face) noexcept {
  u64 h = gi_mix(static_cast<u64>(x) ^ (static_cast<u64>(level * 6u + face) << 58));
  h = gi_mix(h ^ static_cast<u64>(y));
  return gi_mix(h ^ static_cast<u64>(z));
}
constexpr u32 gi_key_of(u64 hash) noexcept { return static_cast<u32>(hash >> 32) | 1u; }
constexpr u32 gi_bucket_of(u64 hash, u32 count) noexcept {
  return static_cast<u32>(hash) & (count - 1u) & ~(k_gi_bucket - 1u);
}

// The sample pass's pixel stride for a frame of `pixels` pixels and a budget of `rays` rays: one
// pixel in every stride, so a frame traces at most the budget. 0 rays is no pass.
constexpr u32 gi_stride(u64 pixels, u64 rays) noexcept {
  if (rays == 0) return 0;
  const u64 stride = (pixels + rays - 1) / rays;
  return static_cast<u32>(stride < 1 ? 1 : stride);
}

}  // namespace engine::gfx

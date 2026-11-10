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
// The resolve then reads the mean trilinearly over the eight cells round the pixel at its level,
// blended with the next coarser level's by the cells' confidence (stage 2: a young cell reads its
// parent), on the normal's face and, near a face's edge, the next face's too; the harmonics where
// no level has samples.

#include <core/base/types.h>

#include <cmath>

namespace engine::gfx {

// **The buffer** (stage 2): a 64-byte header, then the slots' keys, one u32 a slot (the hash's
// upper word with its low bit set; 0 is an empty slot), so a bucket's eight keys are one 32-byte
// block a lookup reads in one load, then the cells, 32 bytes a slot. Stage 1 kept the key in the
// cell's first word, which spread a bucket's keys over 256 bytes and made a lookup eight scattered
// loads (renderer.md, "Indirect light: the world-space cache").
//
// The header's words (`k_gi_stat_*`): what the frame's sample pass and fold counted, copied into
// the frame's statistics and cleared behind the fold.
//
// One cell, eight words:
//   [0] the bucket's pressure: the last frame an insert found the bucket full (the bucket's first
//       cell only; the fold frees a stale cell of a bucket under pressure)
//   [1] stamp: the last frame a sample reached it
//   [2] samples this frame
//   [3..5] this frame's sums of the ratio, r g b, `k_gi_fraction_bits` of fraction
//   [6] the mean ratio's r and g as two halves
//   [7] the mean ratio's b as a half in the low 16 bits, the samples it averages in the high 16
struct GiCell {
  u32 pressure = 0;
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

// **Stage 2's rules** (renderer.md, "Indirect light: the world-space cache", "Stage 2").
// A cell's own mean is read whole once it averages this many samples; below it the read blends
// it with the next coarser level's by `samples / k_gi_confident` (the young cell reads its parent).
inline constexpr u32 k_gi_confident = 32;
// A cell under this many samples is young: the sample pass's second candidate pixel takes a ray
// only where its cell is young, and the fill is measured as the share of pixels whose cell is.
inline constexpr u32 k_gi_young = 8;
// Frames a cell must have gone without a sample before the fold frees it for a full bucket.
inline constexpr u32 k_gi_evict_age = 16;
// The levels a pixel's sample is added to (its own and the coarser ones), and the levels a
// bounce's hit reads before it takes the bounded fallback.
inline constexpr u32 k_gi_sample_levels = 3;
inline constexpr u32 k_gi_hit_levels = 4;
// The faces' blend: where the second-largest component of the normal is at least this share of
// the largest, the read takes the second face's cells too, by up to half at the boundary
// (0.6: 31 degrees from the axis in that plane, the boundary at 45).
inline constexpr f32 k_gi_face_blend = 0.6f;
// The bounded fallback at a hit with no cells: the harmonics times the ray's openness, nothing
// within `k_gi_open_near_m` of its origin, all of it past `k_gi_open_far_m`, smooth between.
inline constexpr f32 k_gi_open_near_m = 1.0f;
inline constexpr f32 k_gi_open_far_m = 10.0f;

// The header: 64 bytes, its first eight words counted by a frame and cleared behind its fold.
inline constexpr u32 k_gi_header_bytes = 64;
inline constexpr u32 k_gi_stat_words = 8;
inline constexpr u32 k_gi_stat_rays = 0;         // rays the sample pass traced (the budget's count)
inline constexpr u32 k_gi_stat_samples = 1;      // stride pixels with a surface
inline constexpr u32 k_gi_stat_young = 2;        // ... whose own cell was young
inline constexpr u32 k_gi_stat_dropped = 3;      // inserts that found the bucket full
inline constexpr u32 k_gi_stat_cells = 4;        // cells holding a key after the fold
inline constexpr u32 k_gi_stat_young_cells = 5;  // ... of them young
inline constexpr u32 k_gi_stat_evicted = 6;      // stale cells the fold freed for a full bucket
inline constexpr u32 k_gi_stat_expired = 7;      // cells the fold freed for their age
// The ninth word: the frame's young claims on the half of the ray budget (`ResolveParams::gi_rays`)
// the trickle leaves, cleared with the counts. The whole 64 bytes are cleared behind the fold.
inline constexpr u32 k_gi_ray_count = 8;

// The buffer's bytes for `count` slots: the header, the keys and the cells.
constexpr u64 gi_buffer_bytes(u32 count) noexcept {
  return k_gi_header_bytes + u64{count} * (4 + k_gi_cell_bytes);
}

// **The young cell reads its parent** (the shader's `gi_read_face`, the same arithmetic): one
// level's read is the trilinear sum of its cells' means over the weight of the cells that had
// samples, and its confidence the trilinear weights times each cell's `min(samples /
// k_gi_confident, 1)`. Finest first, each level with samples takes its confidence's share of what
// is left (the last level tried takes all of it) and passes the rest on; a level without samples
// takes nothing. What the levels took is the answer, over the share they took — so a young cell
// whose parent is empty is read whole. False where no level has samples (the caller keeps the
// harmonics). The shader stops reading once nothing is left, so a converged neighbourhood costs
// one level's eight lookups.
struct GiLevelRead {
  f32 sum[3] = {0.0f, 0.0f, 0.0f};
  f32 weight = 0.0f;
  f32 confidence = 0.0f;
};
inline bool gi_blend_levels(const GiLevelRead* levels, u32 count, f32 out[3]) noexcept {
  f32 acc[3] = {0.0f, 0.0f, 0.0f};
  f32 remaining = 1.0f;
  bool any = false;
  for (u32 i = 0; i < count; ++i) {
    const GiLevelRead& l = levels[i];
    if (!(l.weight > 1e-4f)) continue;
    any = true;
    const f32 take = i + 1 == count ? 1.0f : std::fmin(l.confidence, 1.0f);
    for (u32 c = 0; c < 3; ++c)
      acc[c] += remaining * take * (l.sum[c] / l.weight);
    remaining *= 1.0f - take;
    if (remaining <= 0.0f) break;
  }
  if (!any) return false;
  for (u32 c = 0; c < 3; ++c)
    out[c] = remaining < 1.0f ? acc[c] / (1.0f - remaining) : acc[c];
  return true;
}
// The faces' blend weight for the second face: 0 below `k_gi_face_blend`, a half where the two
// largest components are equal, smooth between.
inline f32 gi_face_blend(f32 largest, f32 second) noexcept {
  if (!(largest > 0.0f)) return 0.0f;
  const f32 r = second / largest;
  const f32 t = std::fmin(std::fmax((r - k_gi_face_blend) / (1.0f - k_gi_face_blend), 0.0f), 1.0f);
  return 0.5f * t * t * (3.0f - 2.0f * t);
}

// The openness of a bounce's ray `t_m` metres long, for the fallback at a hit with no cells.
inline f32 gi_openness(f32 t_m) noexcept {
  const f32 x = std::fmin(
      std::fmax((t_m - k_gi_open_near_m) / (k_gi_open_far_m - k_gi_open_near_m), 0.0f), 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

// `ResolveParams::gi_flags`.
inline constexpr u32 k_gi_read = 1u;    // the resolve reads the cache for its ambient
inline constexpr u32 k_gi_bounce = 2u;  // a ray that hits takes the hit's light (stage 1b)
inline constexpr u32 k_gi_reset = 4u;   // the fold clears every cell (the cache's first frame)
inline constexpr u32 k_gi_faces = 8u;  // the read blends the next face near a face's edge (stage 2)
inline constexpr u32 k_gi_young_rays = 16u;    // the sample pass's young candidates (stage 2)
inline constexpr u32 k_gi_hit_fallback = 32u;  // a hit reads coarser levels, then the fallback
// Two of stage 2's numbers ride in the flags' upper bytes, so each lever can be judged against the
// stage before it without a new row in the resolve block: the samples at which a cell is read
// whole (`k_gi_confident` by default; 1 reads any cell with a sample whole, as stage 1 did) and
// the frames without a sample after which a cell of a full bucket is freed (`k_gi_evict_age` by
// default; 0 never, as stage 1).
inline constexpr u32 k_gi_confident_shift = 8u;
inline constexpr u32 k_gi_evict_shift = 16u;
// And the far lift (`renderer.gi.far_lift`, 0..1 in 255ths): how far a coarse cell's read is
// lifted towards the harmonics at the last level.
inline constexpr u32 k_gi_far_lift_shift = 24u;
constexpr u32 gi_flag_numbers(u32 confident, u32 evict_age, f32 far_lift = 0.0f) noexcept {
  const f32 lift = far_lift < 0.0f ? 0.0f : (far_lift > 1.0f ? 1.0f : far_lift);
  return ((confident < 1 ? 1u : (confident > 255 ? 255u : confident)) << k_gi_confident_shift) |
         ((evict_age > 255 ? 255u : evict_age) << k_gi_evict_shift) |
         (static_cast<u32>(lift * 255.0f + 0.5f) << k_gi_far_lift_shift);
}

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
// pixel in every stride, so a frame traces at most the budget. 0 rays is no pass. Stage 2 calls it
// with half the frame's budget: the stride pixels are the trickle every cell keeps, and the other
// half goes to young cells (`gi_young_stride`).
constexpr u32 gi_stride(u64 pixels, u64 rays) noexcept {
  if (rays == 0) return 0;
  const u64 stride = (pixels + rays - 1) / rays;
  return static_cast<u32>(stride < 1 ? 1 : stride);
}
// Stage 2's trickle stride: the stride of half the budget, so the young cells' rays have the
// other half (the sample pass's atomic count stops them at the whole budget).
constexpr u32 gi_trickle_stride(u64 pixels, u64 rays) noexcept {
  return rays == 0 ? 0 : gi_stride(pixels, rays > 1 ? rays / 2 : 1);
}

}  // namespace engine::gfx

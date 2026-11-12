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
// ... or this many frames of samples, whichever is longer (stage 5). A coarse level of the ladder
// takes hundreds of samples a frame, and a history of 256 samples was then under one frame: the
// cell kept none of its past, and its mean, quiet as it was, was a fresh draw every frame —
// flicker at sixty a second in whatever read it (a filling surface, a crown whose leaf-sized own
// cells never fill, a bounce's hit). Sixteen frames is a little under what an own cell's 256
// samples are at the trickle's eleven a frame, so no level remembers less time than the finest.
inline constexpr u32 k_gi_history_frames = 16;
// The fold's history for a cell that averaged `previous` samples and takes `n` this frame (the
// shader's rule): the new count, and `n` over it the rate the frame's mean enters at.
constexpr u32 gi_history(u32 previous, u32 n) noexcept {
  const u32 frames = n * k_gi_history_frames < 0xffffu ? n * k_gi_history_frames : 0xffffu;
  const u32 cap = k_gi_max_samples > frames ? k_gi_max_samples : frames;
  return previous + n < cap ? previous + n : cap;
}
// The rate the frame's mean enters at (the shader's rule): `n` over the new count — or, for a
// cell whose count has filled its sixteen bits, over the whole history it stands for. A cell that
// takes over 4,095 a frame (a crown's coarse cell, whose samples count in sixteenths: stage 7)
// fills the field in under sixteen frames, and by the count alone would then remember only what
// fits; a full field is read as the sixteen frames it is the cap of.
constexpr f32 gi_history_rate(u32 previous, u32 n) noexcept {
  const u32 full =
      k_gi_max_samples > n * k_gi_history_frames ? k_gi_max_samples : n * k_gi_history_frames;
  return static_cast<f32>(n) /
         static_cast<f32>(previous >= 0xffffu ? full : gi_history(previous, n));
}
// Frames without a sample after which the fold frees a cell.
inline constexpr u32 k_gi_max_age = 240;
inline constexpr u32 k_gi_workgroup = 64;
// The passes' dispatches are two-dimensional past this many groups in x (a device need take no
// more than 65,535): thread `id.y * k_gi_dispatch_pitch * k_gi_workgroup + id.x`.
inline constexpr u32 k_gi_dispatch_pitch = 32768;

// **Stage 2's rules** (renderer.md, "Indirect light: the world-space cache", "Stage 2"), as stage 5
// left them.
// A cell's own mean is read whole once it averages this many samples; below it the read takes it
// by `samples / k_gi_confident` and the rest from the coarser levels (the walk up the ladder,
// below). 255 since stage 5 (it was 32), the history's own length as the flags' byte holds it: a
// cell read whole at n samples shows the noise of a mean of n, a sample's spread over the square
// root of n, and the eye took a 32-sample mean arriving for the light filling in. At 255 a filling
// cell never shows more than sqrt(511 / 255) = 1.42 times the noise the converged average keeps
// (`k_gi_max_samples`, an exponential average of 256: a spread over the square root of 511), and
// one more sample moves its share of the read by 1/255, which is what one more sample moves a
// converged cell's mean by: frame to frame, a cell that is filling changes as one that has filled.
inline constexpr u32 k_gi_confident = 255;
// A cell under this many samples is young: the sample pass's second candidate pixel takes a ray
// only where its cell is young.
inline constexpr u32 k_gi_young = 8;
// Frames a cell must have gone without a sample before the fold frees it for a full bucket.
inline constexpr u32 k_gi_evict_age = 16;
// **Stage 5: the ladder** (renderer.md, "Indirect light", "Stage 5"). The levels a sample is added
// to and a read walks, finest first: the surface's own and the three coarser. Consecutive, so a
// surface whose level changes as the eye comes nearer reads the cells that were its own and its
// parents' the frame before; four, because a surface's cells are a quarter as many a level up, so
// the fourth takes 64 times the own cell's samples — about 700 in the one frame a surface is first
// seen at 3840x2160, where its own cell takes 11 — and is read whole on that frame.
inline constexpr u32 k_gi_ladder = 4;
// A walk stops once this share or less is left to the coarser levels.
inline constexpr f32 k_gi_read_stop = 1.0f / 32.0f;
// A level whose cells hold less than an eighth of the trilinear weight between them (in shares:
// weight times confidence) is taken by that much less: a point's own cell holds an eighth at
// least, so this is a mean extrapolated from far corners while the point's own cell is empty or
// young, and taking it whole made the own cell's first sample a step.
inline constexpr f32 k_gi_read_cover = 8.0f;
// Under this much confidence over all the levels the read leans on the caller's prior in
// proportion (`gi_confidence_weight`).
inline constexpr f32 k_gi_read_floor =
    1.0f / 16.0f;  // The faces' blend: where the second-largest component of the normal is at least
                   // this share of
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
// **Stage 4's counts** (the shell's samples, `ResolveParams::gi_shell_rays`), behind the ray count
// and copied with the eight above (the copy takes `k_gi_stat_copy_words`, the ray count riding
// along unread): the shell's directions outside the view's frustum that were traced from the eye,
// those that met a surface, and those that met one within `k_gi_shell_near_m` — the count that says
// how much of the sphere round the eye the frame's structures hold.
inline constexpr u32 k_gi_stat_shell = 9;
inline constexpr u32 k_gi_stat_shell_hits = 10;
inline constexpr u32 k_gi_stat_shell_near = 11;
inline constexpr u32 k_gi_stat_copy_words = 12;
inline constexpr f32 k_gi_shell_near_m = 50.0f;

// The buffer's bytes for `count` slots: the header, the keys and the cells.
constexpr u64 gi_buffer_bytes(u32 count) noexcept {
  return k_gi_header_bytes + u64{count} * (4 + k_gi_cell_bytes);
}

// **The walk up the ladder** (the shader's `gi_read_level` and `gi_read_face`, the same
// arithmetic). One level's read is three sums over its eight cells, with each cell's share `s` its
// trilinear weight times its confidence `min(samples / confident, 1)`: the shares times the cells'
// means, the shares, the shares squared, and the shares squared over the cells' samples.
// `square / spread` is the samples a cell of the level's mean `sum / confidence` holds: the cells'
// counts averaged as the mean's variance weighs them (harmonically, by the shares squared) — one
// cell's count at its centre, eleven between eight cells of eleven. Finest first, a level takes
// that count over `confident` (at most all) of what the finer levels left and passes the rest up;
// the walk ends once `k_gi_read_stop` or less is left. So a level sharpens at one rate wherever the
// point stands among its cells, and adds no more noise to the read than a mean of `confident`
// samples carries: with m samples a cell it takes m / confident and adds at most a sample's spread
// times sqrt(m) / confident (less where several cells share the point; the variance of the mean
// is `spread / confidence^2`, Kish's effective sample size inverted, with each cell's mean a
// sample's variance over its count). Returns what the levels took in all, 0 to 1, with `out` their
// mean over that share; 0 where no level has a sample (the caller keeps its prior, and leans on it
// below `k_gi_read_floor`). Every term is continuous in every cell's sample count: no cell's first
// sample and no level's hand-over steps.
struct GiLevelRead {
  f32 sum[3] = {0.0f, 0.0f, 0.0f};
  f32 confidence = 0.0f;
  f32 square = 0.0f;
  f32 spread = 0.0f;
  f32 weight = 0.0f;  // the trilinear weights of the cells that hold a sample (stage 6)
};
//
// **Stage 6: what a difference between two levels shows** (renderer.md, "Indirect light", "Stage
// 6"). The count's rule alone treats a level under `confident` samples as if nothing were known of
// it but its count, and that is the rule of a surface with no detail between the levels: there the
// coarser level's mean is as good and quieter. In a crown the levels differ — the coarse cell is
// the mean of leaves in the open and leaves in the shade — and waiting for 255 samples before the
// difference is shown is what stood as a band after a turn: the same light, flattened, which the
// display's curve makes a brighter picture (the shade lifted by more steps than the lit leaves
// lose). So a level that is not yet read whole by its count is compared with the next one up:
// with means `m` and `m_up` over `n` and `n_up` samples, a sample's variance taken as
// `k_gi_sample_cv2` times the square of the larger mean, the square of their difference less what
// that noise explains of it is the detail between the two levels, and the level takes that detail
// over itself plus its own mean's noise — the shrinkage weight of a noisy mean towards a prior of
// known spread, with the spread estimated by its positive part (James and Stein) — where that is
// more than its count gives it. A level is taken early only where it differs from the one above by
// more than noise explains, and then the noise it adds (`take * s / sqrt(n)`) is less than the
// difference it shows; where the levels agree the count's rule stands to the bit. Continuous in
// both means and both counts, so a cell's first sample is still no step (at one sample the noise
// explains any difference). What the difference shows is taken by the cells' trilinear weight
// where they hold under an eighth of it (`GiLevelRead::weight`: a mean extrapolated from a far
// corner is not the point's), where the count's share is by their weight times their confidence.
//
// A sample's spread: a cosine ray from a half-occluded surface escapes to the sky or meets a leaf
// in the shade or one in the sun — a mixture whose variance is one to two times its mean's square
// (v = 0.3 of the rays escaping at 1.2, the rest at 0.2: 0.84; a fifth of the hits sunlit at 1.7:
// 1.6). Two, the upper figure, is the constant: on the grove's turn four showed a tenth of the
// band's shade early and two a quarter, with the frames' change the same (the experiment page).
inline constexpr f32 k_gi_sample_cv2 = 2.0f;
inline f32 gi_reveal(f32 m, f32 n, f32 m_up, f32 n_up) noexcept {
  const f32 top = std::fmax(m, m_up);
  const f32 s2 = k_gi_sample_cv2 * top * top;
  const f32 d = m - m_up;
  const f32 d2 = std::fmax(d * d - s2 * (1.0f / n + 1.0f / n_up), 0.0f);
  const f32 den = d2 + s2 / n;
  return den > 0.0f ? d2 / den : 0.0f;
}
// The luminance the reveal compares levels by (the shader's `gi_luminance`).
inline f32 gi_level_luminance(const GiLevelRead& l) noexcept {
  return (0.2126f * l.sum[0] + 0.7152f * l.sum[1] + 0.0722f * l.sum[2]) / l.confidence;
}
inline f32 gi_blend_levels(const GiLevelRead* levels, u32 count, f32 out[3],
                           u32 confident = k_gi_confident, bool reveal = false) noexcept {
  f32 acc[3] = {0.0f, 0.0f, 0.0f};
  f32 remaining = 1.0f;
  const GiLevelRead* held = nullptr;  // a level whose share waits for the one above it
  auto take_level = [&](const GiLevelRead& l, f32 take) {
    for (u32 c = 0; c < 3; ++c)
      acc[c] += (remaining * take / l.confidence) * l.sum[c];
    remaining *= 1.0f - take;
  };
  auto by_count = [&](const GiLevelRead& l) {
    return std::fmin(l.square / (l.spread * static_cast<f32>(confident)), 1.0f);
  };
  auto cover = [](const GiLevelRead& l) { return std::fmin(k_gi_read_cover * l.confidence, 1.0f); };
  for (u32 i = 0; i < count; ++i) {
    const GiLevelRead& l = levels[i];
    if (!(l.confidence > 1e-6f)) continue;
    if (held != nullptr) {
      const f32 shown = gi_reveal(gi_level_luminance(*held), held->square / held->spread,
                                  gi_level_luminance(l), l.square / l.spread) *
                        std::fmin(k_gi_read_cover * held->weight, 1.0f);
      take_level(*held, std::fmax(by_count(*held) * cover(*held), shown));
      held = nullptr;
      if (remaining <= k_gi_read_stop) break;
    }
    const f32 take = by_count(l) * cover(l);
    // A level that takes all that is left by its count needs no look at the one above it, nor
    // does one after which the count's walk would stop: the reveal reads no level the count's
    // walk does not.
    if (!reveal || remaining * (1.0f - take) <= k_gi_read_stop) {
      take_level(l, take);
      if (remaining <= k_gi_read_stop) break;
      continue;
    }
    held = &l;
  }
  if (held != nullptr) take_level(*held, by_count(*held) * cover(*held));
  const f32 taken = 1.0f - remaining;
  if (!(taken > 0.0f)) return 0.0f;
  for (u32 c = 0; c < 3; ++c)
    out[c] = acc[c] / taken;
  return taken;
}
// How much of a read's value stands against the caller's prior: all of it from `k_gi_read_floor`
// of confidence up, in proportion below.
inline f32 gi_confidence_weight(f32 taken) noexcept {
  return std::fmin(std::fmax(taken / k_gi_read_floor, 0.0f), 1.0f);
}  // The faces' blend weight for the second face: 0 below `k_gi_face_blend`, a half where the two
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

// **The frame's steps** (stage 5; renderer.md, "Indirect light", "Stage 5"): the push block of the
// cache's two passes (the shader's `ResolvePush` under RESOLVE_GI_STEP). A frame records one sample
// pass and one fold, or — the cache's first frame, and a frame whose eye jumped —
// `gi_preroll_steps` pairs more before them against the same visibility buffer and structures: the
// pre-roll, so the first picture after a load or a cut is lit by a cache that has had its samples.
// Step `step` is frame `ResolveParams::gi_frame + step` of the cache's own clock.
struct GiStepPush {
  u64 params = 0;  // the view's ResolveParams
  u32 step = 0;
  u32 flags = 0;  // k_gi_step_*
};
static_assert(sizeof(GiStepPush) == 16);
inline constexpr u32 k_gi_step_reset = 1u;  // the fold clears every cell and the header
inline constexpr u32 k_gi_step_count = 2u;  // the fold counts what it keeps: the frame's last step

// `ResolveParams::gi_flags`.
inline constexpr u32 k_gi_read = 1u;    // the resolve reads the cache for its ambient
inline constexpr u32 k_gi_bounce = 2u;  // a ray that hits takes the hit's light (stage 1b)
inline constexpr u32 k_gi_faces = 8u;  // the read blends the next face near a face's edge (stage 2)
inline constexpr u32 k_gi_young_rays = 16u;    // the sample pass's young candidates (stage 2)
inline constexpr u32 k_gi_hit_fallback = 32u;  // a hit walks the ladder, then the fallback
// Stage 6's diagnostic (`renderer.gi.show_level`): the resolve's read takes one level of the
// ladder whole — the surface's own plus `(flags >> k_gi_show_shift) & 3` — and nothing else, with
// the cache filled as ever. It is how the count by level is made (what each level holds against
// the walk); a bounce's hit never sees it.
inline constexpr u32 k_gi_show = 4u;
inline constexpr u32 k_gi_show_shift = 6u;
// Stage 6: a level is taken by what its difference from the next one up shows, where that is more
// than its count gives it (`gi_reveal`). The evict age's top bit.
inline constexpr u32 k_gi_reveal = 1u << 23;
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
         ((evict_age > 127 ? 127u : evict_age) << k_gi_evict_shift) |
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
//
// **A crown's cells** (stage 7; renderer.md, "Indirect light", "Stage 7"): foliage on a mesh with a
// crown prior (crown_prior.h) is keyed by no face — `k_gi_face_crown` — because a crown's leaves
// face every way at every depth, so a face splits their samples six ways and describes nothing
// (what a leaf's own facing does to its light is in its prior). Its tags are the nine past the six
// faces' (9 levels of 6 faces end at 53), so a crown's cell never shares a key with a face's.
inline constexpr u32 k_gi_face_crown = 6;
// The levels above a foliage surface's own that its samples are added from, the one its read
// starts at, and the levels that read walks (gi_cache.slang's `k_gi_crown_*`).
inline constexpr u32 k_gi_crown_first = 1;
inline constexpr u32 k_gi_crown_read = 2;
inline constexpr u32 k_gi_crown_read_levels = 2;
// **A crown's cell holds a ratio of sums**: a foliage sample adds its light undivided and its own
// prior to the cell's count, in these sixteenths (one to sixteen: the prior's floor is a
// sixteenth, crown_prior.h), so the cell's mean is its samples' light over the sum of their
// priors. The mean of each sample's own quotient would multiply a deep sample's noise by up to
// sixteen and the cell's light by whatever does not follow the prior; the ratio estimator's
// noise is the samples' own, and over the pixels that read the cell with their own priors it
// gives back the light the samples brought. A crown's cell's counts — the samples a level is
// read whole at, the young test — are in the same sixteenths (gi_cache.slang, `gi_crown_weight`).
inline constexpr u32 k_gi_crown_weight = 16;
constexpr u32 gi_crown_weight(f32 crown) noexcept {
  const f32 v = crown * static_cast<f32>(k_gi_crown_weight) + 0.5f;
  return v < 1.0f
             ? 1u
             : (v > static_cast<f32>(k_gi_crown_weight) ? k_gi_crown_weight : static_cast<u32>(v));
}
constexpr u64 gi_hash(i64 x, i64 y, i64 z, u32 level, u32 face) noexcept {
  const u32 tag = face == k_gi_face_crown ? 54u + level : level * 6u + face;
  u64 h = gi_mix(static_cast<u64>(x) ^ (static_cast<u64>(tag) << 58));
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

// **Stage 4: the shell's samples** (renderer.md, "Indirect light", "Stage 4"; `ResolveParams::
// gi_shell_rays`). The sample pass's threads past a view's trickle windows, rounded up to whole
// workgroups so a wave is all windows or all shell, are that view's shell directions: thread
// `gi_shell_base(windows) + i` takes direction `i` of `gi_shell_rays`. A direction is a cell of a
// stratified equal-area grid on the sphere — `gi_shell_columns` columns of longitude by enough
// rows of the height to hold them — jittered inside its cell by a hash of the cache's frame count
// (never by a position, so a scene moved by whole cells traces the same directions), so a still
// eye's directions cover the sphere evenly and densely over frames. Each costs two rays from the
// young half of the budget (the primary from the eye and the sample's own), so the young half is
// `gi_shell_rays * 2` smaller when they run.
constexpr u32 gi_shell_base(u32 windows) noexcept {
  return (windows + k_gi_workgroup - 1) / k_gi_workgroup * k_gi_workgroup;
}
// Columns of the grid for `count` directions: about sqrt(pi * count), so a cell's width in
// longitude at the equator (2 pi / columns) is about its height (2 / rows radians).
inline u32 gi_shell_columns(u32 count) noexcept {
  if (count == 0) return 1;
  const f32 c = std::floor(std::sqrt(3.14159265f * static_cast<f32>(count)) + 0.5f);
  const u32 columns = c < 1.0f ? 1u : static_cast<u32>(c);
  return columns > count ? count : columns;
}
// The rays the shell's samples take out of the young half: two a direction (an upper bound: a
// direction inside the view's frustum, or one that meets nothing, takes fewer).
constexpr u64 gi_shell_ray_cost(u32 shell_rays) noexcept { return u64{shell_rays} * 2; }

}  // namespace engine::gfx

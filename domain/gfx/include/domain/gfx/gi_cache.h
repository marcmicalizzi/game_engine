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
#include <cstring>

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
//   [0] the cell's measured spread as a half in the low 16 bits (R171: `gi_spread_fold`), and in
//       the high 16 the bucket's pressure: the last frame an insert found the bucket full, as
//       `gi_pressure_stamp` (the bucket's first cell only; the fold frees a stale cell of a bucket
//       under pressure). The pressure took the whole word until R171; only a bucket's first cell
//       used it, and fifteen bits of the frame are enough for a test against this frame.
//   [1] stamp: the last frame a sample reached it, fifteen bits and the foliage bit in the low 16
//       (`k_gi_stamp_crown`); in the high 16 the short mean's luminance as a half, its sign bit
//       `k_gi_cell_changed` (R171's third leg, `gi_memory_fold`) and its lowest bit
//       `k_gi_cell_following` (the fourth leg)
//   [2] samples this frame (30 bits; the top bit `k_gi_frame_drop`, the next `k_gi_frame_follow`)
//   [3..5] this frame's sums of the ratio, r g b, `k_gi_fraction_bits` of fraction
//   [6] the mean ratio's r and g as two halves
//   [7] the mean ratio's b as a half in the low 16 bits, the samples it averages in the high 16
struct GiCell {
  u32 spread_pressure = 0;
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
// **A foliage cell remembers four times as long** (stage 8; renderer.md, "Indirect light", "Stage
// 8"). A face's cell is read by the pixels of its own patch and its neighbours' means move
// independently, so its sixteen frames show as grain. A crown's cell has no face and is read two
// levels up: every pixel of a far crown reads the same few cells, and each frame's step of their
// means moved the whole crown together — flicker, 0.136 of an 8-bit step a frame over far crowns
// against main's 0.076. What such a cell holds is the light outside the crown's own shade, which
// follows the sun and the sky through the harmonics its ratio multiplies (the same frame) and the
// bounce off what stands round the tree (minutes): nothing a second's memory hides.
inline constexpr u32 k_gi_crown_history_frames = 64;
// The fold has a cell's words and not its key's tag, so a foliage sample sets its cell's stamp's
// top bit. **The stamp is the low half of the cell's second word since R171's third leg**: fifteen
// bits of the cache's frame (`k_gi_stamp_frames`) and this bit; the age is the frame less the stamp
// modulo 2^15, which is exact because the fold frees every cell `k_gi_max_age` frames after its
// last sample. The high half is the cell's short mean (`gi_memory_fold`).
inline constexpr u32 k_gi_stamp_crown = 0x8000u;
inline constexpr u32 k_gi_stamp_frames = 0x7fffu;
constexpr u32 gi_stamp_age(u32 now, u32 word) noexcept { return (now - word) & k_gi_stamp_frames; }
// The fold's history for a cell that averaged `previous` samples and takes `n` this frame (the
// shader's rule): the new count, and `n` over it the rate the frame's mean enters at. `frames` is
// the cell's memory: `k_gi_history_frames`, or a foliage cell's `k_gi_crown_history_frames`.
constexpr u32 gi_history(u32 previous, u32 n, u32 frames = k_gi_history_frames) noexcept {
  const u32 held = n * frames < 0xffffu ? n * frames : 0xffffu;
  const u32 cap = k_gi_max_samples > held ? k_gi_max_samples : held;
  return previous + n < cap ? previous + n : cap;
}
// The rate the frame's mean enters at (the shader's rule): `n` over the new count — or, for a
// cell whose count has filled its sixteen bits, over the whole history it stands for. A cell that
// takes over 4,095 a frame (a crown's coarse cell, whose samples count in sixteenths: stage 7)
// fills the field in under sixteen frames, and by the count alone would then remember only what
// fits; a full field is read as the sixteen frames it is the cap of.
constexpr f32 gi_history_rate(u32 previous, u32 n, u32 frames = k_gi_history_frames) noexcept {
  const u32 full = k_gi_max_samples > n * frames ? k_gi_max_samples : n * frames;
  return static_cast<f32>(n) /
         static_cast<f32>(previous >= 0xffffu ? full : gi_history(previous, n, frames));
}
// Today's history as a count: `k_gi_max_samples`, or `frames` frames of `n`, in the sixteen bits.
constexpr u32 gi_history_cap(u32 n, u32 frames = k_gi_history_frames) noexcept {
  const u32 held = n * frames < 0xffffu ? n * frames : 0xffffu;
  return k_gi_max_samples > held ? k_gi_max_samples : held;
}
// **R171, third leg: a cell's memory by its own spread** (renderer.md, "Indirect light", "R171: a
// cell's memory by its own spread"). A face's cell whose light holds still may remember up to
// `k_gi_memory_times` its history (`gi_history_cap`): 4,096 samples for an own cell at the
// trickle's eleven a frame (372 frames), 256 frames where it takes sixteen or more, and never past
// `k_gi_memory_count_max`, under the field's full mark, which stands for a whole history. A cell
// whose history already fills the field (4,096 a frame) remembers what it did.
inline constexpr u32 k_gi_memory_times = 16;
inline constexpr u32 k_gi_memory_count_max = 0xfff0u;
constexpr u32 gi_memory_cap(u32 n) noexcept {
  const u32 cap = gi_history_cap(n);
  if (cap >= k_gi_memory_count_max) return cap;
  return cap * k_gi_memory_times < k_gi_memory_count_max ? cap * k_gi_memory_times
                                                         : k_gi_memory_count_max;
}
// The deviations between the long mean and the short one (today's history) at which a cell's light
// is taken to have changed: the short mean's own noise, from the cell's measured spread, four
// times over. A coarse cell's short mean averages eleven thousand samples, so its noise is near
// normal, and the long mean's difference from it is 0.91 of the short mean's own deviation (the
// two averages share their samples), so four of them are left by chance about once in 90,000
// frames. Five were tried first: a coarse cell whose long mean remembers 93 frames and whose short
// one 16 holds them at most 0.58 of a step apart, so five missed the moved sun's 13 % (a
// simulation of sixteen coarse cells over 1,024 own ones: settled in 333 frames against today's
// 50); four see it.
inline constexpr f32 k_gi_change_sigmas = 4.0f;
// **R171, fourth leg: the deviations at which a tested cell says its light is moving**, under the
// four that let it go: it lets go of nothing itself, and every finer cell of a ladder through it
// folds its next frame at today's rate instead of its long one (`k_gi_frame_follow`). A change too
// small to be told from the noise at four deviations (under about 11 % of a coarse cell's light)
// still crosses two and a half within a few frames and is followed at today's pace while it lasts,
// where leg 3's rule left it to the long memory (a 10 % step settled in 258 frames against today's
// 84). Chosen by a simulation of the fold (sixteen coarse cells over 1,024 own ones, the plain wall's
// heavy samples; renderer.md has the table): at 2.5 a 10 % step settles in 122 frames, at 3 in 303,
// and the still wall's variance rises 13 % at 2.5 (the frames a parent's chance excursion lets its
// children step at today's rate), its 99th percentile change between frames by 0.2 %.
inline constexpr f32 k_gi_follow_sigmas = 2.5f;
// **R171, fourth leg: only a cell that takes this many samples a frame tests itself.** An own cell
// taking the trickle's eleven tests a short mean of 10 % noise, and across a 4K view's fifty
// thousand own cells one leaves four deviations by chance every frame or two: each let go and
// showed today's noise again as a round blob a cell wide (leg 3's crops). Its coarser cells pool 4,
// 16 and 64 times its samples and hand down what they see, so the own cell loses nothing it could
// have seen; the threshold is the third level of the ladder at the trickle (16 x 11 = 176) and the
// second at a near pixel's denser rays, not the first.
inline constexpr u32 k_gi_test_samples = 64;
// A cell that lets go keeps this fraction of today's history (a quarter: 64 samples for an own
// cell), so the part of a change its short mean had not yet followed is followed four times as
// fast as today, and the detection's own frames are made up.
inline constexpr u32 k_gi_memory_back = 4;
// Word 2's top bit: a coarser cell of a ladder through this one saw its light change last frame
// (the sample pass sets it; the count of the frame's samples is the other 30 bits).
inline constexpr u32 k_gi_frame_drop = 0x80000000u;
// Word 2's next bit: a coarser cell of a ladder through this one saw its light moving
// (`k_gi_follow_sigmas`), so this frame folds at today's rate (R171's fourth leg).
inline constexpr u32 k_gi_frame_follow = 0x40000000u;
inline constexpr u32 k_gi_frame_flags = k_gi_frame_drop | k_gi_frame_follow;
// Word 1's top bit: this cell saw its light change in the last fold (the sign of its short mean's
// half, which is never negative).
inline constexpr u32 k_gi_cell_changed = 0x80000000u;
// Word 1's bit 16: this cell saw its light moving in the last fold (`k_gi_follow_sigmas`): the
// short mean's half gives up its lowest bit for it (2^-10 of the short mean, which only the test
// reads). Not in the low half, which every sample rewrites with the stamp before the ladder's
// flags are read.
inline constexpr u32 k_gi_cell_following = 0x00010000u;
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
// Stage 8: the directions that met a surface whose cells are already whole and stopped at the
// eye's ray (gi_update.slang, `gi_shell_keep`): their cells stamped, their bounce taken one frame
// in eight.
inline constexpr u32 k_gi_stat_shell_kept = 12;
inline constexpr u32 k_gi_stat_copy_words = 13;
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
  // R171: the shares squared times each cell's measured spread over its samples
  // (`gi_spread_fold`), so `noise / confidence^2` is the variance of the level's mean in luminance
  // as the cells measured it — where `spread / confidence^2` is that variance in units of one
  // sample's, which the count's rule and stage 6's model must assume.
  f32 noise = 0.0f;
  // R171, the cells of the level as a population, by their shares (trilinear weight times
  // confidence): the shares, and the shares times their means' luminances, the squares and their
  // means' variances (the measured spread over the samples). What the read pools the detail
  // between two levels over.
  f32 cells = 0.0f;
  f32 lum = 0.0f;
  f32 lum2 = 0.0f;
  f32 cell_noise = 0.0f;
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

// **R171: a cell's measured spread** (renderer.md, "Indirect light", "R171: the grain on a plain
// wall"). Stage 6's model takes a sample's variance as twice its mean's square; indoors, where the
// light comes through an opening a cosine ray seldom finds, it is about five times, and a mean of
// 256 such samples is the grain the owner saw. The fold has a frame's sum and not its squares, so
// it measures the spread between frames: a frame's mean of `n` samples differs from the history's
// mean by a sample's variance over `n` plus the history's own, so `n (m_frame - m)^2` times
// `previous / (previous + n)` estimates a sample's variance (the history's variance taken as a
// sample's over `previous`, its count). The estimates are averaged at `n / previous`, which is
// their plain mean while the count grows and the cell's own rate once it is at its cap.
//
// What is kept is that variance **as the cell's count sees it**: the read divides it by the
// samples the cell averages, and that is the variance of its mean while the mean is a plain one
// (`samples` of them); once the count is at its cap the mean is an exponential average, whose
// variance is a sample's over `2 total - n`, so each estimate is scaled by `total / (2 total - n)`
// there. The scale enters through the average, so it arrives at the cell's own rate, which is the
// rate the mean's variance relaxes at. Luminance of the ratio, in a half: 0 for a cell's first
// frame (no history to differ from) and for a cell the fold has just made.
inline f32 gi_spread_fold(f32 spread, u32 previous, u32 n, u32 total, f32 lum_old,
                          f32 lum_frame) noexcept {
  if (previous == 0 || n == 0) return previous == 0 ? 0.0f : spread;
  const f32 d = lum_frame - lum_old;
  const f32 nf = static_cast<f32>(n);
  const f32 pf = static_cast<f32>(previous);
  const f32 tf = static_cast<f32>(total);
  const f32 at_cap = previous + n > total ? tf / (2.0f * tf - nf) : 1.0f;
  const f32 estimate = nf * d * d * (pf / (pf + nf)) * at_cap;
  const f32 rate = std::fmin(nf / pf, 1.0f);
  const f32 out = spread + (estimate - spread) * rate;
  return std::fmin(out, 65504.0f);
}
// The pressure's half of a bucket's first word: the frame's low fifteen bits with the top bit set,
// so a bucket never stamped (0) never reads as under pressure, and a stamp matches its frame
// again only 32,768 frames on.
inline constexpr u32 k_gi_pressure_shift = 16;
constexpr u32 gi_pressure_stamp(u32 frame) noexcept { return (frame & 0x7fffu) | 0x8000u; }

// **R171, third leg: the fold with a memory by the cell's own spread**, in luminance (the shader
// folds the long mean in colour, the same arithmetic; `renderer.gi.memory`). A cell keeps two
// means of its frames: the long one it is read by, which averages every frame since the light last
// changed up to `gi_memory_cap`, and a short one at today's history (`gi_history_cap`), kept as a
// half in its stamp word's high half. They hold the same light while it is still; the short one
// follows a change within today's frames and the long one does not, so their difference is the
// test, in the band the short mean's own noise makes: a sample's variance from the cell's measured
// spread over the short mean's effective count, 2 T - n (the variance of an exponential average).
// Out of it by `k_gi_change_sigmas`, the cell takes its long mean to the short one's level and its
// count back to a quarter of today's history at once (`k_gi_memory_back`), and sets `changed`,
// which the sample pass hands to every finer cell of a ladder through it the next frame (`drop`): a
// coarse cell pools sixty-four of its own level's samples and sees a change a fine one's noise
// hides, and a single heavy sample moves it by a sixty-fourth as much. That is A-SVGF's rule
// (Schied et al. 2018: a history cut back where a temporal gradient beyond its noise is seen), with
// the gradient taken between two histories of one cell instead of from a re-traced sample, and
// pooled up the ladder instead of over the screen.
//
// **The fourth leg** (renderer.md, "R171: the memory made right"): only a cell taking
// `k_gi_test_samples` or more a frame tests itself (an own cell's chance let-go was a blob), and a
// tested cell between `k_gi_follow_sigmas` and `k_gi_change_sigmas` sets `following`, which the
// sample pass hands down as `follow`: that frame the cell folds at today's rate, so a change too
// small to let go for is followed at today's pace while a coarser cell still sees it move.
struct GiMemoryCell {
  f32 mean = 0.0f;  // the long mean, the one the read shows
  f32 short_mean = 0.0f;
  f32 spread = 0.0f;  // `gi_spread_fold`'s
  u32 count = 0;
  bool changed = false;
  bool following = false;  // set with `changed` too
};
inline void gi_memory_fold(GiMemoryCell& c, u32 n, f32 frame_mean, bool drop, bool memory,
                           bool follow = false) noexcept {
  if (n == 0) return;
  const u32 previous = c.count;
  const u32 cap = gi_history_cap(n);
  const u32 ceiling = memory ? gi_memory_cap(n) : cap;
  const u32 total = previous + n < ceiling ? previous + n : ceiling;
  const u32 full =
      k_gi_max_samples > n * k_gi_history_frames ? k_gi_max_samples : n * k_gi_history_frames;
  const f32 nf = static_cast<f32>(n);
  const u32 short_total = previous + n < cap ? previous + n : cap;
  const f32 short_rate = nf / static_cast<f32>(previous >= 0xffffu ? full : short_total);
  const f32 long_rate = nf / static_cast<f32>(previous >= 0xffffu ? full : total);
  // Told that a coarser cell sees its light moving, the cell folds this frame at today's rate.
  const f32 rate = memory && follow && short_rate > long_rate ? short_rate : long_rate;
  if (previous == 0) {
    c = GiMemoryCell{frame_mean, frame_mean, 0.0f, total, false, false};
    return;
  }
  const f32 spread = gi_spread_fold(c.spread, previous, n, total, c.mean, frame_mean);
  const f32 s = c.short_mean + (frame_mean - c.short_mean) * short_rate;
  f32 m = c.mean + (frame_mean - c.mean) * rate;
  u32 count = total;
  bool changed = false;
  bool following = false;
  if (memory && n >= k_gi_test_samples && spread > 0.0f) {
    // A sample's variance: the spread is kept as the long count sees it (at its cap, scaled by
    // T / (2 T - n)), the short mean's variance is that over 2 cap - n.
    const f32 tf = static_cast<f32>(total);
    const f32 sample = previous + n > total ? spread * (2.0f * tf - nf) / tf : spread;
    const f32 var_short = sample / (2.0f * static_cast<f32>(cap) - nf);
    const f32 apart2 = (m - s) * (m - s);
    changed = apart2 > k_gi_change_sigmas * k_gi_change_sigmas * var_short;
    following = apart2 > k_gi_follow_sigmas * k_gi_follow_sigmas * var_short;
  }
  const u32 back = cap / k_gi_memory_back;
  if (memory && (drop || changed) && previous > back) {
    // Back at once to a quarter of today's history, from the short mean's level: what the short
    // mean has followed of the change is taken whole, and the rest four times as fast as today.
    const f32 base = c.short_mean;
    m = base + (frame_mean - base) * (nf / static_cast<f32>(back + n));
    count = back + n;
  }
  c = GiMemoryCell{m, s, spread, count, changed, following};
}

// **R171, fourth leg: the long mean is rounded to its half without a bias** (renderer.md, "R171:
// the memory made right"). The mean is kept as three halves, eleven bits of mantissa, and a long
// memory moves it by a small share of each frame's difference: at 4,096 samples an own cell's step
// is a 372nd of a frame mean that indoors is mostly a little under the light and now and then far
// over it (a few heavy samples). Rounded to nearest, the frequent small steps down are lost under
// half a unit of the half and the rare large ones up are kept, so the mean climbs: +9 % in a
// simulation of the plain wall's samples (+0.2 % with the mean in float), which is the level's rise
// leg 3 measured (0.2 to 1.8 %) and could not place. Rounded up or down with the chance its distance
// from each says, a half is the float in expectation. The chance is drawn from a hash of the cell's
// own words (its frame's sums and its count and old mean), which the same samples make the same
// wherever the scene stands, so the bytes still follow the scene and not the slot.
inline u32 gi_round_hash(u32 a, u32 b, u32 c, u32 d) noexcept {
  u32 h = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u);
  h = (h ^ (h >> 16)) * 0x85ebca6bu;
  h ^= c * 0xc2b2ae35u;
  h = (h ^ (h >> 13)) * 0x27d4eb2fu;
  h ^= d * 0x165667b1u;
  return h ^ (h >> 16);
}
// A finite non-negative float as a half's bits, rounded to nearest even (the shader's f32tof16 on
// the devices measured), and back.
inline u32 gi_half_bits(f32 x) noexcept {
  u32 f = 0;
  std::memcpy(&f, &x, 4);
  const u32 sign = (f >> 16) & 0x8000u;
  const i32 exp = static_cast<i32>((f >> 23) & 0xffu) - 127 + 15;
  u32 mant = f & 0x7fffffu;
  if (exp >= 31) return sign | 0x7bffu;  // clamped to the largest finite half
  if (exp <= 0) {
    if (exp < -10) return sign;
    mant |= 0x800000u;
    const u32 shift = static_cast<u32>(14 - exp);
    u32 h = mant >> shift;
    const u32 rem = mant & ((1u << shift) - 1u);
    const u32 halfway = 1u << (shift - 1u);
    if (rem > halfway || (rem == halfway && (h & 1u) != 0u)) ++h;
    return sign | h;
  }
  u32 h = (static_cast<u32>(exp) << 10) | (mant >> 13);
  const u32 rem = mant & 0x1fffu;
  if (rem > 0x1000u || (rem == 0x1000u && (h & 1u) != 0u)) ++h;
  return sign | (h > 0x7bffu ? 0x7bffu : h);
}
inline f32 gi_half_value(u32 h) noexcept {
  const u32 exp = (h >> 10) & 0x1fu;
  const u32 mant = h & 0x3ffu;
  const f32 sign = (h & 0x8000u) != 0u ? -1.0f : 1.0f;
  if (exp == 0) return sign * std::ldexp(static_cast<f32>(mant), -24);
  return sign * std::ldexp(static_cast<f32>(mant | 0x400u), static_cast<i32>(exp) - 25);
}
// `x` (finite, not negative) as the half below or above it, the upper with the chance `u` (in
// [0, 1)) falls under x's distance from the lower over the gap between them: the half in
// expectation is `x`. The shader's `gi_half_unbiased` is the same arithmetic.
inline u32 gi_half_unbiased(f32 x, f32 u) noexcept {
  if (!(x > 0.0f)) return 0u;
  const u32 h = gi_half_bits(x);
  const f32 taken = gi_half_value(h);
  if (taken == x || h >= 0x7bffu) return h;
  const u32 other = taken > x ? h - 1u : h + 1u;
  const f32 beyond = gi_half_value(other);
  const f32 t = (x - taken) / (beyond - taken);  // in (0, 0.5]: rounding took the nearer
  return u < t ? other : h;
}

// **R171: the read by its cells' measured spread.** The walk (below) shows a level whole once its
// cells hold `confident` samples: a mean of 256, which indoors carries 4 to 5 % of noise at the
// cell's own size. So once the walk has its answer (variance `var_r`, from the measured spreads of
// the levels it took), it is shrunk towards the ladder's last level `u` (the surface's own plus
// three: 64 times its samples, variance `var_u`) by James and Stein's rule: the answer keeps
// (detail + var_u) / (detail + var_u + var_r), where `detail` is the spread of the true light about
// the last level at the own level's scale. Where there is none the last level is shown, whose
// noise is a sixty-fourth of the own level's; where the light has detail at that scale (a corner,
// a contact, a patch of sun) the answer is kept, and the noise it brings is under the detail.
//
// **The detail is pooled over the own level's cells round the point** (`gi_spread_detail`): their
// means' spread about `u`, less what their measured noise and the last level's explain, positive
// part. Stage 6 estimates its difference from the one pair of means at the point, and that is
// the weak form: a single difference cannot tell a cell's detail from its noise, so a cell two of
// its noise off is kept three-quarters whole, and over a plain wall the read kept 0.6 of the
// grain (measured on the village, 2026-11-16). Pooled over the cells round the point, by their
// shares, it is a population's estimate between cells, as James and Stein's is, and one cell's at
// a cell's centre. Counting each cell once pools more (1.9 to 2.2 % on the same walls) and was
// built and taken out: the set of cells round a point changes at every half cell, so the detail
// stepped there and the picture showed the cells' squares. By the shares it is continuous in
// space and in every cell's count. All of the answer where nothing is measured, so a cell the
// fold has no spread for reads as it did.
inline f32 gi_spread_keep(f32 detail, f32 var_r, f32 var_u) noexcept {
  const f32 prior = detail + var_u;
  const f32 den = prior + var_r;
  return den > 0.0f ? prior / den : 1.0f;
}
// The detail at the own level's scale about the last level's mean `u`: the cells' squared
// differences from it, less their own measured noise and the last level's, positive part; a
// negative number where the own level has no cell (the walk's answer is then kept).
inline f32 gi_spread_detail(const GiLevelRead& own, f32 u, f32 var_u) noexcept {
  if (!(own.cells > 0.0f)) return -1.0f;
  const f32 spread_about = (own.lum2 - 2.0f * u * own.lum + own.cells * u * u) / own.cells;
  return std::fmax(spread_about - own.cell_noise / own.cells - var_u, 0.0f);
}

inline f32 gi_blend_levels(const GiLevelRead* levels, u32 count, f32 out[3],
                           u32 confident = k_gi_confident, bool reveal = false,
                           bool spread = false) noexcept {
  f32 acc[3] = {0.0f, 0.0f, 0.0f};
  f32 remaining = 1.0f;
  f32 variance = 0.0f;                // the answer's variance, times its share squared (R171)
  const GiLevelRead* held = nullptr;  // a level whose share waits for the one above it
  auto take_level = [&](const GiLevelRead& l, f32 take) {
    const f32 share = remaining * take / l.confidence;
    for (u32 c = 0; c < 3; ++c)
      acc[c] += share * l.sum[c];
    variance += share * share * l.noise;
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
  // R171: the answer against the ladder's last level, by their measured spreads
  // (`gi_spread_keep`); the last level stands in by its own count's share, so a young one is
  // leaned on only as far as its samples allow.
  const GiLevelRead& top = levels[count - 1];
  if (spread && count > 1 && top.confidence > 1e-6f) {
    const f32 var_r = variance / (taken * taken);
    const f32 u = gi_level_luminance(top);
    const f32 var_u = top.noise / (top.confidence * top.confidence);
    const f32 detail = gi_spread_detail(levels[0], u, var_u);
    const f32 keep = detail < 0.0f ? 1.0f : gi_spread_keep(detail, var_r, var_u);
    const f32 lean = (1.0f - keep) * by_count(top) * cover(top);
    for (u32 c = 0; c < 3; ++c)
      out[c] += (top.sum[c] / top.confidence - out[c]) * lean;
  }
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
// R171: the read's answer is compared with the ladder's last level by the cells' measured spreads
// (`gi_spread_keep`, `renderer.gi.spread`). The evict age's sixth bit until R171, which left it
// six (a full bucket's stale cells are freed after at most 63 frames).
inline constexpr u32 k_gi_spread = 1u << 22;
// R171, third leg: a face's cell remembers up to `k_gi_memory_times` its history while its light
// holds still (`gi_memory_fold`, `renderer.gi.memory`). The evict age's fifth bit until then, which
// left it five (a full bucket's stale cells are freed after at most 31 frames).
inline constexpr u32 k_gi_memory = 1u << 21;
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
         ((evict_age > 31 ? 31u : evict_age) << k_gi_evict_shift) |
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
//
// **Stage 8: the directions are the pass's first threads** (`gi_shell_word`'s bit 24,
// `renderer.gi.shell_first`). A direction's rays are long — from the eye through whatever stands
// round it, then a bounce — and a workgroup of them takes as long as its longest. Dispatched
// behind the windows they ran when the windows' work was done, with the device otherwise idle,
// and the pass waited for the slowest of them: a quarter of the directions cost three quarters
// as much (renderer.md, "Stage 8"). Dispatched first they run beside the windows. With the bit,
// thread `i` under `gi_shell_base(gi_shell_rays)` takes direction `i` and the threads from there
// take the windows; without it the windows come first, as stage 4 had them. Either way the pass
// dispatches `gi_shell_base(windows) + gi_shell_base(gi_shell_rays)` threads.
constexpr u32 gi_shell_base(u32 count) noexcept {
  return (count + k_gi_workgroup - 1) / k_gi_workgroup * k_gi_workgroup;
}
inline constexpr u32 k_gi_shell_first = 1u << 24;
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

// **Stage 8: what a direction round the eye stops at** (`ResolveParams::gi_shell_word`;
// renderer.md, "Indirect light", "Stage 8"). The low half is the reach of the eye's ray in whole
// metres, rounded up (0: no stop): the radius of the volume that put the geometry round the eye
// into the frame's structures, past which they hold a part of what is there. The third byte is the
// frames between the bounces of a direction whose surface's cells are whole
// (`renderer.gi.shell_refresh`): such a direction stops at the eye's ray, stamps its cells so the
// fold keeps them, and takes its bounce one frame in that many — 1 is every frame (stage 7's pass),
// 0 never. Bit 24
// (`k_gi_shell_first`): the directions are the pass's first threads.
constexpr u32 gi_shell_word(f32 reach_m, u32 refresh, bool first = true) noexcept {
  const u32 whole = reach_m > 0.0f ? static_cast<u32>(reach_m) : 0u;
  const u32 reach = static_cast<f32>(whole) < reach_m ? whole + 1 : whole;
  return (reach < 0xffffu ? reach : 0xffffu) | ((refresh < 0xffu ? refresh : 0xffu) << 16) |
         (first ? k_gi_shell_first : 0u);
}

}  // namespace engine::gfx

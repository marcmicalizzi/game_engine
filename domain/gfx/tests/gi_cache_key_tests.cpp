// The world-space cache's key (gi_cache.h; R69; docs/subsystems/renderer.md, "Indirect light: the
// world-space cache"): no device. The shader's key is the same functions operation for operation
// (gi_cache.slang), and `gi cache: a scene moved by whole cells draws the same bytes with the cache
// on` (systems/renderer) holds the two together on the GPU.

#include <domain/gfx/crown_prior.h>
#include <domain/gfx/gi_cache.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace engine;
using namespace engine::gfx;

TEST_CASE("gi cache key: every level's cell divides the world's cell") {
  for (u32 level = 0; level < k_gi_levels; ++level) {
    CHECK(64000 % gi_cell_mm(level) == 0);
  }
  CHECK(gi_cell_mm(0) == 25);
  CHECK(gi_cell_mm(k_gi_levels - 1) == 6400);
}

TEST_CASE(
    "gi cache key: a point and its mirror a whole number of cells away fall in mirrored cells") {
  // The same offset from an eye in cell c and in cell c + k: the millimetres differ by exactly
  // k cells, so every level's cell index differs by a whole number of its cells, and the key is
  // that cell's — the same cell of the scene, wherever the scene stands.
  const f32 locals[] = {0.0f, 0.001f, 31.9995f, 63.999f};
  const f32 offsets[] = {-1234.5678f, -0.0125f, 0.0f, 0.0124f, 2.5f, 4096.25f};
  const i32 cells[] = {0, -1, 7, -6548, 156250, -1562500};
  for (const f32 local : locals) {
    for (const f32 offset : offsets) {
      const i64 home = gi_axis_mm(0, local, offset);
      for (const i32 k : cells) {
        const i64 moved = gi_axis_mm(k, local, offset);
        CHECK(moved - home == i64{k} * 64000);
        for (u32 level = 0; level < k_gi_levels; ++level) {
          const i64 size = gi_cell_mm(level);
          CHECK(gi_floor_div(moved, size) - gi_floor_div(home, size) == i64{k} * (64000 / size));
        }
      }
    }
  }
}

TEST_CASE("gi cache key: the cell below zero is the floor's, not the truncation's") {
  CHECK(gi_floor_div(-1, 25) == -1);
  CHECK(gi_floor_div(-25, 25) == -1);
  CHECK(gi_floor_div(-26, 25) == -2);
  CHECK(gi_floor_div(0, 25) == 0);
  CHECK(gi_floor_div(24, 25) == 0);
  CHECK(gi_floor_div(25, 25) == 1);
}

TEST_CASE("gi cache key: the level grows with distance and stops at the last") {
  const f32 scale = 0.004f;
  CHECK(gi_level(0.0f, scale) == 0);
  CHECK(gi_level(6.0f, scale) == 0);    // 24 mm
  CHECK(gi_level(7.0f, scale) == 1);    // 28 mm: a 50 mm cell
  CHECK(gi_level(20.0f, scale) == 2);   // 80 mm: 100 mm
  CHECK(gi_level(100.0f, scale) == 4);  // 400 mm
  CHECK(gi_level(1.0e6f, scale) == k_gi_levels - 1);
  u32 last = 0;
  for (f32 d = 0.0f; d < 5000.0f; d += 0.37f) {
    const u32 level = gi_level(d, scale);
    CHECK(level >= last);
    // The cell is at least the rule's size, until the last level.
    if (level + 1 < k_gi_levels) {
      CHECK(static_cast<f32>(gi_cell_mm(level)) * 0.001f >= d * scale * 0.9999f);
    }
    last = level;
  }
}

TEST_CASE("gi cache key: faces, buckets and keys") {
  CHECK(gi_face(0.0f, 1.0f, 0.0f) == 2);
  CHECK(gi_face(0.01f, 0.99f, -0.02f) == 2);
  CHECK(gi_face(0.0f, -1.0f, 0.0f) == 3);
  CHECK(gi_face(-0.9f, 0.1f, 0.3f) == 1);
  CHECK(gi_face(0.1f, 0.2f, -0.95f) == 5);
  // A wall's two sides, a level's neighbours and the faces of one cell are different keys.
  const u64 a = gi_hash(10, 2, -3, 1, 0);
  CHECK(gi_hash(10, 2, -3, 1, 1) != a);
  CHECK(gi_hash(10, 2, -3, 2, 0) != a);
  CHECK(gi_hash(11, 2, -3, 1, 0) != a);
  CHECK(gi_hash(10, 2, -2, 1, 0) != a);
  CHECK((gi_key_of(a) & 1u) == 1u);  // never the empty slot's 0
  const u32 count = gi_cell_count(u64{32} << 20);
  CHECK(count == (1u << 20));
  CHECK(gi_bucket_of(a, count) % k_gi_bucket == 0);
  CHECK(gi_bucket_of(a, count) < count);
  CHECK(gi_cell_count(100) == k_gi_bucket);
}

TEST_CASE("gi cache key: the stride keeps a frame inside its rays") {
  CHECK(gi_stride(3840ull * 2160ull, 2000000) == 5);
  CHECK(gi_stride(1920ull * 1080ull, 2000000) == 2);
  CHECK(gi_stride(1000, 2000000) == 1);
  CHECK(gi_stride(1000, 0) == 0);
  const u64 pixels = 11520ull * 2160ull;
  const u32 stride = gi_stride(pixels, 2000000);
  CHECK((pixels + stride - 1) / stride <= 2000000);
}

TEST_CASE("gi cache key: stage 2's trickle is half the budget and the buffer holds keys apart") {
  // 3840x2160 at two million rays: the trickle takes one pixel in nine, and its young candidates
  // the rest of the budget.
  CHECK(gi_trickle_stride(3840ull * 2160ull, 2000000) == 9);
  CHECK(gi_trickle_stride(3840ull * 2160ull, 0) == 0);
  CHECK(gi_trickle_stride(1000, 1) == 1000);
  const u64 pixels = 11520ull * 2160ull;
  const u32 stride = gi_trickle_stride(pixels, 2000000);
  CHECK((pixels + stride - 1) / stride <= 1000000);
  // The header, a 4-byte key a slot, then the 32-byte cells: a bucket's keys are one 32-byte block.
  CHECK(gi_buffer_bytes(1u << 20) == 64 + (u64{1} << 20) * 36);
  CHECK((k_gi_header_bytes % 32) == 0);
  CHECK(k_gi_bucket * 4 == 32);
}

namespace {

// A level as its cells: each of the eight corners' trilinear weight, sample count and mean.
struct LevelCells {
  f64 weight[8];
  u32 samples[8];
  f64 mean[8];
};

// What the shader's `gi_read_level` sums for a level, in float as it does.
GiLevelRead level_read(const LevelCells& cells, u32 confident) {
  GiLevelRead out;
  for (u32 i = 0; i < 8; ++i) {
    if (cells.samples[i] == 0) continue;
    const f32 w = static_cast<f32>(cells.weight[i]);
    const f32 c =
        w * std::fmin(static_cast<f32>(cells.samples[i]) / static_cast<f32>(confident), 1.0f);
    for (u32 k = 0; k < 3; ++k)
      out.sum[k] += static_cast<f32>(cells.mean[i]) * c;
    out.confidence += c;
    out.square += c * c;
    out.spread += c * c / static_cast<f32>(cells.samples[i]);
    out.weight += w;
  }
  return out;
}

// The walk as its definition reads, in double and from the cells: each level's mean is its cells'
// means by their shares (weight times confidence); the samples a cell of that mean holds are the
// cells' counts averaged harmonically by the shares squared; the level takes that count over
// `confident` of what the finer levels left, and the answer is the takes' weighted mean. Returns
// the share taken in all, and in `noise` the variance of the answer in units of one sample's
// (every cell's mean carrying a sample's variance over its count), which the walk's rule bounds.
//
// With `reveal` (stage 6) a level its count does not read whole waits for the next one up, and
// takes what their difference shows where that is more: the difference's square less the noise of
// the two means (a sample's variance `k_gi_sample_cv2` times the larger mean's square), over itself
// plus the level's own noise.
f64 brute_reveal(f64 m, f64 n, f64 m_up, f64 n_up) {
  const f64 top = std::fmax(m, m_up);
  const f64 s2 = static_cast<f64>(k_gi_sample_cv2) * top * top;
  const f64 d2 = std::fmax((m - m_up) * (m - m_up) - s2 * (1.0 / n + 1.0 / n_up), 0.0);
  return d2 + s2 / n > 0.0 ? d2 / (d2 + s2 / n) : 0.0;
}

f64 brute_walk(const LevelCells* levels, u32 count, u32 confident, f64& value, f64* noise = nullptr,
               bool reveal = false) {
  f64 left = 1.0;
  f64 acc = 0.0;
  f64 variance_sum = 0.0;
  struct Level {
    f64 mean = 0.0;
    f64 confidence = 0.0;
    f64 a_cell = 0.0;
    f64 inverse = 0.0;
    f64 weight = 0.0;
    bool waiting = false;
  };
  Level held;
  auto take = [&](const Level& level, f64 share_of_left) {
    const f64 share = left * share_of_left;
    acc += share * level.mean;
    variance_sum += share * share * level.inverse / (level.confidence * level.confidence);
    left -= share;
  };
  auto by_count = [&](const Level& level) {
    return std::fmin(level.a_cell / static_cast<f64>(confident), 1.0);
  };
  auto cover = [](const Level& level) {
    return std::fmin(static_cast<f64>(k_gi_read_cover) * level.confidence, 1.0);
  };
  for (u32 l = 0; l < count; ++l) {
    f64 mean_sum = 0.0;
    f64 confidence = 0.0;
    f64 square = 0.0;
    f64 inverse = 0.0;
    f64 weight = 0.0;
    for (u32 i = 0; i < 8; ++i) {
      const f64 n = static_cast<f64>(levels[l].samples[i]);
      if (n == 0.0) continue;
      const f64 c = levels[l].weight[i] * std::fmin(n / static_cast<f64>(confident), 1.0);
      mean_sum += c * levels[l].mean[i];
      confidence += c;
      square += c * c;
      inverse += c * c / n;
      weight += levels[l].weight[i];
    }
    if (!(confidence > 1e-6)) continue;
    Level now;
    now.mean = mean_sum / confidence;
    now.confidence = confidence;
    now.a_cell = square / inverse;  // the harmonic mean of the counts
    now.inverse = inverse;
    now.weight = weight;
    if (held.waiting) {
      const f64 shown = brute_reveal(held.mean, held.a_cell, now.mean, now.a_cell) *
                        std::fmin(static_cast<f64>(k_gi_read_cover) * held.weight, 1.0);
      take(held, std::fmax(by_count(held) * cover(held), shown));
      held.waiting = false;
      if (left <= static_cast<f64>(k_gi_read_stop)) break;
    }
    const f64 share = by_count(now) * cover(now);
    if (!reveal || left * (1.0 - share) <= static_cast<f64>(k_gi_read_stop)) {
      take(now, share);
      if (left <= static_cast<f64>(k_gi_read_stop)) break;
      continue;
    }
    held = now;
    held.waiting = true;
  }
  if (held.waiting) take(held, by_count(held) * cover(held));
  const f64 taken = 1.0 - left;
  value = taken > 0.0 ? acc / taken : 0.0;
  if (noise != nullptr) *noise = taken > 0.0 ? variance_sum / (taken * taken) : 0.0;
  return taken;
}

u32 next(u32& state) {
  state = state * 1664525u + 1013904223u;
  return state >> 8;
}

// Eight trilinear weights from a point's three fractions.
void trilinear(f64 fx, f64 fy, f64 fz, f64 out[8]) {
  for (u32 i = 0; i < 8; ++i)
    out[i] = ((i & 1u) ? fx : 1.0 - fx) * ((i & 2u) ? fy : 1.0 - fy) * ((i & 4u) ? fz : 1.0 - fz);
}

f32 walk(const LevelCells* levels, u32 count, u32 confident, f32& value, bool reveal = false) {
  GiLevelRead reads[k_gi_ladder];
  for (u32 l = 0; l < count; ++l)
    reads[l] = level_read(levels[l], confident);
  f32 out[3] = {0.0f, 0.0f, 0.0f};
  const f32 taken = gi_blend_levels(reads, count, out, confident, reveal);
  value = out[0];
  return taken;
}

}  // namespace

TEST_CASE("gi cache key: the walk up the ladder is the brute-force blend of its cells") {
  u32 state = 12345u;
  u32 compared = 0;
  for (u32 trial = 0; trial < 4000; ++trial) {
    LevelCells levels[k_gi_ladder];
    const u32 count = 1 + next(state) % k_gi_ladder;
    for (u32 l = 0; l < count; ++l) {
      trilinear(static_cast<f64>(next(state) % 1000) / 1000.0,
                static_cast<f64>(next(state) % 1000) / 1000.0,
                static_cast<f64>(next(state) % 1000) / 1000.0, levels[l].weight);
      // A level is empty a quarter of the time; otherwise each corner has no cell a third of the
      // time and from one sample to past the history's length the rest.
      const bool empty = next(state) % 4 == 0;
      for (u32 i = 0; i < 8; ++i) {
        levels[l].samples[i] = empty || next(state) % 3 == 0 ? 0u : 1u + next(state) % 300;
        levels[l].mean[i] = static_cast<f64>(next(state) % 4000) / 1000.0;
      }
    }
    f32 got = 0.0f;
    f64 want = 0.0;
    const f32 taken = walk(levels, count, k_gi_confident, got);
    const f64 brute = brute_walk(levels, count, k_gi_confident, want);
    CHECK(std::fabs(static_cast<f64>(taken) - brute) <= 1e-4);
    if (!(brute > 1e-3)) continue;
    ++compared;
    CHECK(std::fabs(static_cast<f64>(got) - want) <= 2e-4 * (1.0 + std::fabs(want)));
    // And with stage 6's reveal, the same cells.
    const f32 taken_shown = walk(levels, count, k_gi_confident, got, true);
    const f64 brute_shown = brute_walk(levels, count, k_gi_confident, want, nullptr, true);
    CHECK(std::fabs(static_cast<f64>(taken_shown) - brute_shown) <= 2e-4);
    CHECK(std::fabs(static_cast<f64>(got) - want) <= 4e-4 * (1.0 + std::fabs(want)));
  }
  CHECK(compared > 2000);
}

// **Stage 6: what a difference between two levels shows** (gi_cache.h, `gi_reveal`). The count's
// rule waits 255 samples before a level is read whole, which is right where the level above holds
// the same light and wrong in a crown, where it holds the mean of the open and the shade: there
// the difference is the picture, and the reveal takes a level by what the difference shows beyond
// the two means' noise.
TEST_CASE("gi cache key: a level is taken early by what its difference shows, and only by that") {
  // Two levels, the point at a cell's centre of each.
  LevelCells levels[2];
  for (LevelCells& level : levels) {
    trilinear(0.0, 0.0, 0.0, level.weight);
    for (u32 i = 0; i < 8; ++i) {
      level.samples[i] = 0;
      level.mean[i] = 0.0;
    }
  }
  const f32 cv2 = k_gi_sample_cv2;
  // The levels agree: the reveal adds nothing and the count's rule stands, to the bit.
  levels[0].samples[0] = 16;
  levels[0].mean[0] = 0.5;
  levels[1].samples[0] = k_gi_max_samples;
  levels[1].mean[0] = 0.5;
  f32 plain = 0.0f;
  f32 shown = 0.0f;
  CHECK(walk(levels, 2, k_gi_confident, plain) == walk(levels, 2, k_gi_confident, shown, true));
  CHECK(plain == shown);
  // A difference the noise explains — a sample's spread is sqrt(cv2) of the larger mean, so the
  // two means' noise is that over sqrt(n) and sqrt(n_up) — shows nothing either.
  const f32 within = 0.5f * std::sqrt(cv2 * (1.0f / 16.0f + 1.0f / 256.0f)) * 0.99f;
  levels[0].mean[0] = static_cast<f64>(0.5f - within);
  CHECK(walk(levels, 2, k_gi_confident, plain) == walk(levels, 2, k_gi_confident, shown, true));
  CHECK(plain == shown);
  CHECK(gi_reveal(0.5f - within, 16.0f, 0.5f, 256.0f) == 0.0f);
  // Shade under a level five times brighter, sixteen samples in: the count gives it 16 / 255 of
  // the read; the difference gives it the detail over the detail plus its own noise.
  levels[0].mean[0] = 0.1;
  const f32 d2 = 0.4f * 0.4f - cv2 * 0.25f * (1.0f / 16.0f + 1.0f / 256.0f);
  const f32 want = d2 / (d2 + cv2 * 0.25f / 16.0f);
  CHECK(gi_reveal(0.1f, 16.0f, 0.5f, 256.0f) == doctest::Approx(want));
  CHECK(want > 0.35f);
  CHECK(walk(levels, 2, k_gi_confident, shown, true) == doctest::Approx(1.0f));
  CHECK(shown == doctest::Approx(want * 0.1f + (1.0f - want) * 0.5f));
  // The count alone shows a thirtieth of it (its 16 / 255, by the half its cells' confidence
  // covers of an eighth).
  (void)walk(levels, 2, k_gi_confident, plain);
  const f32 counted = (16.0f / 255.0f) * (k_gi_read_cover * 16.0f / 255.0f);
  CHECK(plain == doctest::Approx(counted * 0.1f + (1.0f - counted) * 0.5f));
  // A far corner alone (a sixteenth of the weight) is shown by half as much.
  trilinear(0.5, 0.5, 0.75, levels[0].weight);
  for (u32 i = 1; i < 8; ++i)
    levels[0].weight[i] = 0.0;
  const f32 far = static_cast<f32>(levels[0].weight[0]);
  CHECK(far == doctest::Approx(1.0f / 16.0f));
  (void)walk(levels, 2, k_gi_confident, shown, true);
  CHECK(shown == doctest::Approx(0.5f * want * 0.1f + (1.0f - 0.5f * want) * 0.5f));
  trilinear(0.0, 0.0, 0.0, levels[0].weight);
  // Over random pairs: the share is under one, rises with the level's samples, is the same
  // whichever of the two is the brighter by the same step, and where it is above zero the noise the
  // level adds to the read — its share times its mean's noise — is under the difference it shows.
  u32 state = 4242u;
  u32 early = 0;
  for (u32 trial = 0; trial < 4000; ++trial) {
    const f32 m = 0.01f + static_cast<f32>(next(state) % 3000) / 1000.0f;
    const f32 m_up = 0.01f + static_cast<f32>(next(state) % 3000) / 1000.0f;
    const f32 n = 1.0f + static_cast<f32>(next(state) % 254);
    const f32 n_up = n * (1.0f + static_cast<f32>(next(state) % 16));
    const f32 share = gi_reveal(m, n, m_up, n_up);
    CHECK(share >= 0.0f);
    CHECK(share < 1.0f);
    CHECK(gi_reveal(m, n + 1.0f, m_up, n_up) >= share);
    CHECK(gi_reveal(m_up, n, m, n_up) == doctest::Approx(share));
    if (share > 0.0f) {
      ++early;
      const f32 noise = std::sqrt(cv2) * std::fmax(m, m_up) / std::sqrt(n);
      CHECK(share * noise < std::fabs(m - m_up));
    }
  }
  CHECK(early > 500);
  // The lever off is stage 5's walk: `gi_blend_levels` without it is the count's rule alone.
  CHECK(k_gi_reveal == (1u << 23));
  CHECK((gi_flag_numbers(255, 127, 1.0f) & k_gi_reveal) == 0u);
  CHECK((gi_flag_numbers(255, 255, 0.0f) >> k_gi_evict_shift & 0xffu) == 127u);
}

TEST_CASE("gi cache key: a cell remembers 256 samples or sixteen frames of them") {
  // An own cell at the trickle's eleven a frame: a plain mean to 256, then that history.
  CHECK(gi_history(0, 11) == 11);
  CHECK(gi_history(250, 11) == 256);
  CHECK(gi_history(256, 11) == 256);
  // A coarse cell at 711 a frame: sixteen frames of them, where 256 was under one frame's and the
  // frame's mean replaced the cell's (a rate of 1).
  CHECK(gi_history(0, 711) == 711);
  CHECK(gi_history(711, 711) == 1422);
  CHECK(gi_history(16 * 711, 711) == 16 * 711);
  // The view moves on and the cell takes a few: back to the samples' history, never above it.
  CHECK(gi_history(16 * 711, 5) == 256);
  // The count is sixteen bits in the cell.
  CHECK(gi_history(60000, 60000) == 0xffffu);
}

TEST_CASE("gi cache key: a converged level decides alone and a young one hands the rest up") {
  // The point at a cell's centre of every level, so one corner carries all the weight.
  LevelCells levels[2];
  for (LevelCells& level : levels) {
    trilinear(0.0, 0.0, 0.0, level.weight);
    for (u32 i = 0; i < 8; ++i) {
      level.samples[i] = 0;
      level.mean[i] = 0.0;
    }
  }
  levels[0].samples[0] = k_gi_max_samples;
  levels[0].mean[0] = 0.2;
  levels[1].samples[0] = k_gi_max_samples;
  levels[1].mean[0] = 1.0;
  f32 value = 0.0f;
  CHECK(walk(levels, 2, k_gi_confident, value) == doctest::Approx(1.0f));
  CHECK(value == doctest::Approx(0.2f));
  // A quarter of the samples it is read whole at: a quarter of the read, the parent the rest.
  levels[0].samples[0] = k_gi_confident / 4;
  const f32 quarter = static_cast<f32>(k_gi_confident / 4) / static_cast<f32>(k_gi_confident);
  CHECK(walk(levels, 2, k_gi_confident, value) == doctest::Approx(1.0f));
  CHECK(value == doctest::Approx(quarter * 0.2f + (1.0f - quarter) * 1.0f));
  // The noise that quarter adds: with m samples behind its mean a level takes m / confident, so
  // it adds a sample's spread times sqrt(m) / confident — at most 1 / sqrt(confident), which is
  // sqrt(511 / 255) = 1.42 times what an exponential average of 256 keeps. And a sample moves a
  // filling cell's share by 1 / confident, a converged cell's mean by 1 / 256: the same.
  CHECK(std::sqrt((2.0 * k_gi_max_samples - 1.0) / k_gi_confident) < 1.42);
  CHECK(k_gi_confident + 1 == k_gi_max_samples);
  // No cell at all: nothing taken, and the caller keeps its prior.
  levels[0].samples[0] = 0;
  levels[1].samples[0] = 0;
  CHECK(walk(levels, 2, k_gi_confident, value) == 0.0f);
  // One level, young, and nothing above it: its mean, at the confidence it has, so the caller
  // leans on its prior below the floor and not at all from the floor up.
  levels[0].samples[0] = 2;
  const f32 taken = walk(levels, 1, k_gi_confident, value);
  const f32 two = 2.0f / static_cast<f32>(k_gi_confident);
  CHECK(taken == doctest::Approx(two * std::fmin(k_gi_read_cover * two, 1.0f)));
  trilinear(0.5, 0.5, 0.5, levels[0].weight);
  for (u32 i = 0; i < 8; ++i) {
    levels[0].samples[i] = 2;
    levels[0].mean[i] = 0.2;
  }
  // Eight young cells at their shared corner are taken as one of them is at its centre: a level
  // sharpens at one rate wherever the point stands among its cells.
  CHECK(walk(levels, 1, k_gi_confident, value) ==
        doctest::Approx(two * std::fmin(k_gi_read_cover * two, 1.0f)));
  trilinear(0.0, 0.0, 0.0, levels[0].weight);
  for (u32 i = 1; i < 8; ++i)
    levels[0].samples[i] = 0;
  CHECK(value == doctest::Approx(0.2f));
  CHECK(gi_confidence_weight(0.0f) == 0.0f);
  CHECK(gi_confidence_weight(taken) == doctest::Approx(taken / k_gi_read_floor));
  CHECK(gi_confidence_weight(k_gi_read_floor) == 1.0f);
  CHECK(gi_confidence_weight(1.0f) == 1.0f);
}

// **What the eye must not see** (renderer.md, "Indirect light", "Stage 5"): a cell's first samples
// arriving, or a level handing over. Whatever the neighbourhood, one more sample in one cell moves
// the read by a bounded step: where a cell stands alone its share grows by 1 / k_gi_confident of
// the read a sample; in the worst neighbourhood — the cell a point is in taking its first sample
// beside a corner that held an eighth of the weight with just the samples to be read whole — the
// level hands a fifth of its read up at once (the samples behind its mean fall by a fifth), so
// the bound here is a quarter of the spread of the means. Stage 2's read moved by all of it: a
// cell appearing beside none took the whole read.
TEST_CASE("gi cache key: no cell's next sample steps the read") {
  u32 state = 99u;
  f64 worst = 0.0;
  f64 worst_noise = 0.0;
  for (u32 trial = 0; trial < 3000; ++trial) {
    LevelCells levels[k_gi_ladder];
    for (u32 l = 0; l < k_gi_ladder; ++l) {
      trilinear(static_cast<f64>(next(state) % 1000) / 1000.0,
                static_cast<f64>(next(state) % 1000) / 1000.0,
                static_cast<f64>(next(state) % 1000) / 1000.0, levels[l].weight);
      for (u32 i = 0; i < 8; ++i) {
        // Young at the fine levels, filling towards the coarse, as a surface just seen is; and the
        // coarsest level always holds a frame's samples, as the ladder gives it.
        const u32 most = 1u << (2 * l + 2);
        levels[l].samples[i] =
            next(state) % 3 == 0 && l + 1 < k_gi_ladder ? 0u : next(state) % most;
        if (l + 1 == k_gi_ladder) levels[l].samples[i] += 64;
        levels[l].mean[i] = static_cast<f64>(next(state) % 1000) / 1000.0;  // a spread of 1
      }
    }
    f64 before = 0.0;
    f64 noise = 0.0;
    const f64 taken = brute_walk(levels, k_gi_ladder, k_gi_confident, before, &noise);
    REQUIRE(taken > static_cast<f64>(k_gi_read_floor));
    // The read's variance, whatever its cells hold: where the walk took everything, no more than
    // a mean of `k_gi_confident` samples' over the share taken — a level that takes a share t of
    // what is left adds at most t over `k_gi_confident`, and the shares sum to what was taken.
    if (taken >= 1.0 - static_cast<f64>(k_gi_read_stop)) {
      worst_noise = std::fmax(worst_noise, noise * static_cast<f64>(k_gi_confident));
    }
    const u32 l = next(state) % k_gi_ladder;
    const u32 i = next(state) % 8;
    levels[l].samples[i] += 1;
    f64 after = 0.0;
    (void)brute_walk(levels, k_gi_ladder, k_gi_confident, after);
    worst = std::fmax(worst, std::fabs(after - before));
  }
  MESSAGE("the largest step one sample made, of a spread of 1: "
          << worst << "; the read's largest variance, in those of a mean of " << k_gi_confident
          << " samples: " << worst_noise);
  CHECK(worst_noise <= 1.1);
  CHECK(worst <= 0.25);
}
TEST_CASE("gi cache key: the faces' blend and the fallback's openness are smooth and bounded") {
  CHECK(gi_face_blend(1.0f, 0.0f) == 0.0f);
  CHECK(gi_face_blend(1.0f, k_gi_face_blend) == 0.0f);
  CHECK(gi_face_blend(1.0f, 1.0f) == doctest::Approx(0.5f));
  CHECK(gi_face_blend(0.0f, 0.0f) == 0.0f);
  f32 previous = 0.0f;
  for (u32 i = 0; i <= 100; ++i) {
    const f32 w = gi_face_blend(1.0f, static_cast<f32>(i) / 100.0f);
    CHECK(w >= previous);
    CHECK(w <= 0.5f);
    previous = w;
  }
  CHECK(gi_openness(0.0f) == 0.0f);
  CHECK(gi_openness(k_gi_open_near_m) == 0.0f);
  CHECK(gi_openness(k_gi_open_far_m) == 1.0f);
  CHECK(gi_openness(100.0f) == 1.0f);
  previous = 0.0f;
  for (u32 i = 0; i <= 120; ++i) {
    const f32 o = gi_openness(static_cast<f32>(i) * 0.1f);
    CHECK(o >= previous);
    previous = o;
  }
}

// ---- stage 7: a crown's shade as a prior (crown_prior.h; renderer.md, "Indirect light",
// "Stage 7"; docs/experiments/crown-shade-grounding.md) ------------------------------------------

TEST_CASE("gi cache key: a crown's cells share no key with a face's") {
  // One cell's coordinates under every level and face, and under every level of the crown's own:
  // sixty-three hashes, all different (the crown's tags are the nine past the six faces').
  u64 seen[k_gi_levels * 7];
  u32 count = 0;
  for (u32 level = 0; level < k_gi_levels; ++level) {
    for (u32 face = 0; face <= k_gi_face_crown; ++face) {
      seen[count++] = gi_hash(1234, -77, 4096, level, face);
    }
  }
  for (u32 a = 0; a < count; ++a) {
    for (u32 b = a + 1; b < count; ++b)
      CHECK(seen[a] != seen[b]);
  }
}

namespace {

// A ball of uniform leaf: one small triangle at every point of a lattice inside `radius`, sized so
// that the leaf area a cubic metre (after the cards' fill) is `density`.
void leaf_ball(f32 radius, f32 spacing, f64 density, Vector<Vec3>& corners) {
  const f64 area = density * static_cast<f64>(spacing) * static_cast<f64>(spacing) *
                   static_cast<f64>(spacing) / k_crown_card_fill;
  const f32 leg = static_cast<f32>(std::sqrt(2.0 * area));
  const i32 n = static_cast<i32>(radius / spacing) + 1;
  for (i32 z = -n; z < n; ++z) {
    for (i32 y = -n; y < n; ++y) {
      for (i32 x = -n; x < n; ++x) {
        const Vec3 p{(static_cast<f32>(x) + 0.5f) * spacing, (static_cast<f32>(y) + 0.5f) * spacing,
                     (static_cast<f32>(z) + 0.5f) * spacing};
        if (dot(p, p) > radius * radius) continue;
        corners.push_back(p);
        corners.push_back(Vec3{p.x + leg, p.y, p.z});
        corners.push_back(Vec3{p.x, p.y, p.z + leg});
      }
    }
  }
}

// The fraction a ball of `radius` and uniform leaf lets through to the point `p` inside it along
// the unit direction `d`: Beer-Lambert over the chord from the point to the ball's surface.
f64 ball_transmitted(Vec3 p, Vec3 d, f64 radius, f64 density) {
  const f64 b = static_cast<f64>(dot(p, d));
  const f64 c = static_cast<f64>(dot(p, p)) - radius * radius;
  const f64 chord = -b + std::sqrt(b * b - c);
  return std::exp(-k_crown_g * density * chord);
}

}  // namespace

// **The prior against the closed form.** In a ball of uniform leaf area density u the fraction
// that reaches a point along a direction is exp(-G u s) over the chord s to the surface, and the
// prior is its mean over a hemisphere's directions. Held two ways at points across the ball:
// against that closed form along the prior's own twelve directions a hemisphere, where what is left
// is the grid — a path's leaf is off by at most half a cell at the ball's surface and half a step
// of the march, (0.25 + 0.125) m here, 0.094 of optical depth at G u = 0.25 a metre, so under 0.094
// of the fraction itself, itself under 0.65 at these points: 0.06 — and, in the message, against
// a fine quadrature of the same closed form over the hemisphere (64 by 128), which says what
// twelve directions cost. At the centre every chord is the radius, so the prior is exp(-G u R)
// whatever the directions.
TEST_CASE("gi cache key: a crown's prior in a ball of uniform leaf is Beer-Lambert's closed form") {
  const f32 radius = 4.0f;
  const f64 density = 0.5;  // square metres of leaf a cubic metre: a broadleaf crown's
  Vector<Vec3> corners;
  leaf_ball(radius, 0.125f, density, corners);
  CrownPriorDesc desc;
  Vector<u8> bytes;
  CrownPriorStats stats;
  build_crown_prior({corners.data(), corners.size()}, Vec3{-radius, -radius, -radius},
                    Vec3{radius, radius, radius}, desc, bytes, &stats);
  CHECK((desc.dims & 0xffu) == k_crown_cells);
  // The leaf it gathered is the ball's: u times the ball's volume, within the lattice's edge.
  const f64 volume = 4.0 / 3.0 * 3.14159265358979 * 64.0;
  CHECK(stats.leaf_area_m2 == doctest::Approx(density * volume).epsilon(0.02));
  const Vec3 points[] = {Vec3{0.0f, 0.0f, 0.0f},  Vec3{0.0f, 2.0f, 0.0f},  Vec3{0.0f, -2.0f, 0.0f},
                         Vec3{2.5f, 0.0f, 0.0f},  Vec3{0.0f, 3.25f, 0.0f}, Vec3{1.5f, 1.5f, -1.5f},
                         Vec3{-1.0f, -2.5f, 2.0f}};
  f64 worst_same = 0.0;
  f64 worst_fine = 0.0;
  for (const Vec3& p : points) {
    const CrownPriorSample got = crown_prior_sample(desc, bytes.data(), p);
    // The closed form along the prior's own directions.
    f64 up = 0.0;
    f64 down = 0.0;
    for (u32 r = 0; r < k_crown_rings; ++r) {
      const f64 y = (static_cast<f64>(r) + 0.5) / static_cast<f64>(k_crown_rings);
      const f64 s = std::sqrt(1.0 - y * y);
      for (u32 k = 0; k < k_crown_spokes; ++k) {
        const f64 phi = 6.283185307179586 * (static_cast<f64>(k) + 0.5 * static_cast<f64>(r & 1u)) /
                        static_cast<f64>(k_crown_spokes);
        const Vec3 d{static_cast<f32>(s * std::cos(phi)), static_cast<f32>(y),
                     static_cast<f32>(s * std::sin(phi))};
        up += ball_transmitted(p, d, static_cast<f64>(radius), density);
        down += ball_transmitted(p, Vec3{d.x, -d.y, d.z}, static_cast<f64>(radius), density);
      }
    }
    up /= static_cast<f64>(k_crown_rings * k_crown_spokes);
    down /= static_cast<f64>(k_crown_rings * k_crown_spokes);
    CHECK(std::abs(static_cast<f64>(got.up) - up) <= 0.06);
    CHECK(std::abs(static_cast<f64>(got.down) - down) <= 0.06);
    worst_same = std::max({worst_same, std::abs(static_cast<f64>(got.up) - up),
                           std::abs(static_cast<f64>(got.down) - down)});
    // The same closed form over the whole hemisphere, equal solid angles.
    f64 fine_up = 0.0;
    f64 fine_down = 0.0;
    for (u32 i = 0; i < 64; ++i) {
      const f64 y = (static_cast<f64>(i) + 0.5) / 64.0;
      const f64 s = std::sqrt(1.0 - y * y);
      for (u32 k = 0; k < 128; ++k) {
        const f64 phi = 6.283185307179586 * (static_cast<f64>(k) + 0.5) / 128.0;
        const Vec3 d{static_cast<f32>(s * std::cos(phi)), static_cast<f32>(y),
                     static_cast<f32>(s * std::sin(phi))};
        fine_up += ball_transmitted(p, d, static_cast<f64>(radius), density);
        fine_down += ball_transmitted(p, Vec3{d.x, -d.y, d.z}, static_cast<f64>(radius), density);
      }
    }
    fine_up /= 64.0 * 128.0;
    fine_down /= 64.0 * 128.0;
    worst_fine = std::max({worst_fine, std::abs(static_cast<f64>(got.up) - fine_up),
                           std::abs(static_cast<f64>(got.down) - fine_down)});
  }
  // The centre: exp(-G u R), whatever the directions.
  const CrownPriorSample centre = crown_prior_sample(desc, bytes.data(), Vec3{0.0f, 0.0f, 0.0f});
  const f64 closed = std::exp(-k_crown_g * density * static_cast<f64>(radius));
  CHECK(std::abs(static_cast<f64>(centre.up) - closed) <= 0.094 * closed);
  CHECK(std::abs(static_cast<f64>(centre.down) - closed) <= 0.094 * closed);
  MESSAGE("a ball of 4 m at 0.5 m2/m3: the centre "
          << centre.up << " against exp(-1) = " << closed << "; over seven points the grid stands "
          << worst_same << " at most from the closed form along its own directions and "
          << worst_fine << " from the hemisphere's fine quadrature; leaf " << stats.leaf_area_m2
          << " m2 in " << stats.leaf_volume_m3 << " m3 of cells");
  // The same bytes from every toolchain: the grid's FNV-1a, taken on MSVC.
  u64 hash = 1469598103934665603ull;
  for (const u8 b : bytes)
    hash = (hash ^ b) * 1099511628211ull;
  MESSAGE("the ball's grid: " << bytes.size() << " bytes, hash " << hash);
  CHECK(hash == 13891853578010254563ull);
}

TEST_CASE("gi cache key: a surface's prior is its hemisphere's share of the two means, floored") {
  const CrownPriorSample s{0.6f, 0.2f};
  CHECK(crown_prior(s, 1.0f) == doctest::Approx(0.6f));   // facing up: the upper hemisphere's
  CHECK(crown_prior(s, -1.0f) == doctest::Approx(0.2f));  // facing down: the lower one's
  CHECK(crown_prior(s, 0.0f) == doctest::Approx(0.4f));   // on edge: half of each
  // Never under the floor, which bounds what a sample's division can do to its noise: sixteen
  // times at most.
  CHECK(crown_prior(CrownPriorSample{0.01f, 0.0f}, 0.3f) == k_crown_floor);
  // Open air: nothing between the surface and the light, and the read is the cache's as it was.
  CHECK(crown_prior(CrownPriorSample{1.0f, 1.0f}, 0.7f) == doctest::Approx(1.0f));
}

TEST_CASE("gi cache key: a crown's cell counts in sixteenths, and a full count is sixteen frames") {
  // A sample in the open counts sixteen, one at the prior's floor one, and none counts nothing.
  CHECK(gi_crown_weight(1.0f) == k_gi_crown_weight);
  CHECK(gi_crown_weight(k_crown_floor) == 1u);
  CHECK(gi_crown_weight(0.5f) == 8u);
  CHECK(gi_crown_weight(0.001f) == 1u);
  // A cell that takes 24,000 a frame (1,500 samples in the open) fills its sixteen bits in three
  // frames: until it has, the rate is the plain mean's; once it has, a frame's share of sixteen —
  // not the 24,000 over 65,535 the capped count alone would give, a memory of under three frames.
  CHECK(gi_history_rate(0u, 24000u) == doctest::Approx(1.0f));
  CHECK(gi_history_rate(24000u, 24000u) == doctest::Approx(0.5f));
  CHECK(gi_history(48000u, 24000u) == 0xffffu);
  CHECK(gi_history_rate(0xffffu, 24000u) == doctest::Approx(1.0f / 16.0f));
  // A cell under 4,096 a frame never fills the field, and its rate is the count's as it was.
  CHECK(gi_history(60000u, 700u) == 700u * k_gi_history_frames);
  CHECK(gi_history_rate(60000u, 700u) == doctest::Approx(1.0f / 16.0f));
  CHECK(gi_history_rate(100u, 12u) == doctest::Approx(12.0f / 112.0f));
}

// Stage 8: a foliage cell's memory is sixty-four frames, by the same rule. What the longer memory
// buys is the size of a frame's step in the cell's mean once it has filled — every pixel of a far
// crown takes that step together — and what it must not cost is the fill: until the history is
// full the rate is the plain mean's, whatever the memory.
TEST_CASE("gi cache key: a foliage cell remembers sixty-four frames, and fills as fast as ever") {
  CHECK(k_gi_crown_history_frames == 4u * k_gi_history_frames);
  // The stamp's flag is above any frame count the cache's clock reaches.
  CHECK((k_gi_stamp_crown & 0x7fffffffu) == 0u);
  // The fill: the same counts and rates as a face's cell while the history is short of either cap.
  for (const u32 n : {16u, 160u, 2400u}) {
    u32 face = 0;
    u32 crown = 0;
    for (u32 frame = 0; frame < 16 && (crown + n) < 0xffffu; ++frame) {
      CHECK(gi_history_rate(crown, n, k_gi_crown_history_frames) ==
            doctest::Approx(gi_history_rate(face, n)));
      face = gi_history(face, n);
      crown = gi_history(crown, n, k_gi_crown_history_frames);
      CHECK(crown == face);
    }
  }
  // Filled: a frame's share of sixty-four, a quarter of a face's step, under the field's cap ...
  CHECK(gi_history(160u * 64u, 160u, k_gi_crown_history_frames) == 160u * 64u);
  CHECK(gi_history_rate(160u * 64u, 160u, k_gi_crown_history_frames) ==
        doctest::Approx(1.0f / 64.0f));
  CHECK(gi_history_rate(160u * 16u, 160u) == doctest::Approx(1.0f / 16.0f));
  // ... and past it, where the count has filled its sixteen bits and stands for the whole history.
  CHECK(gi_history(0xffffu, 24000u, k_gi_crown_history_frames) == 0xffffu);
  CHECK(gi_history_rate(0xffffu, 24000u, k_gi_crown_history_frames) ==
        doctest::Approx(1.0f / 64.0f));
  // A cell that takes little keeps the 256 samples every cell keeps.
  CHECK(gi_history(256u, 2u, k_gi_crown_history_frames) == 256u);
}

// Stage 8: the word the sample pass reads a direction round the eye's stops from.
TEST_CASE("gi cache key: the shell's word holds the reach in whole metres and the refresh") {
  CHECK(gi_shell_word(48.0f, 8u) == (48u | (8u << 16) | k_gi_shell_first));
  CHECK(gi_shell_word(48.0f, 8u, false) == (48u | (8u << 16)));
  // The reach rounds up, so the ray never stops short of the volume.
  CHECK((gi_shell_word(47.2f, 8u) & 0xffffu) == 48u);
  CHECK((gi_shell_word(0.0f, 8u) & 0xffffu) == 0u);  // no stop
  CHECK((gi_shell_word(1.0e9f, 1u) & 0xffffu) == 0xffffu);
  CHECK(((gi_shell_word(48.0f, 0u) >> 16) & 0xffu) == 0u);  // never refreshed
  CHECK(((gi_shell_word(48.0f, 1u) >> 16) & 0xffu) == 1u);  // every frame: stage 7's pass
  CHECK(((gi_shell_word(48.0f, 4000u) >> 16) & 0xffu) == 0xffu);
  CHECK((gi_shell_word(48.0f, 255u, false) >> 24) == 0u);
  // The pass's threads: the directions and the windows each in whole workgroups, so a wave is
  // all of one kind whichever comes first.
  CHECK(gi_shell_base(65536u) == 65536u);
  CHECK(gi_shell_base(65537u) == 65536u + k_gi_workgroup);
  CHECK(gi_shell_base(0u) == 0u);
}

// The world-space cache's key (gi_cache.h; R69; docs/subsystems/renderer.md, "Indirect light: the
// world-space cache"): no device. The shader's key is the same functions operation for operation
// (gi_cache.slang), and `gi cache: a scene moved by whole cells draws the same bytes with the cache
// on` (systems/renderer) holds the two together on the GPU.

#include <domain/gfx/gi_cache.h>

#include <doctest/doctest.h>

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
  }
  return out;
}

// The walk as its definition reads, in double and from the cells: each level's mean is its cells'
// means by their shares (weight times confidence); the samples a cell of that mean holds are the
// cells' counts averaged harmonically by the shares squared; the level takes that count over
// `confident` of what the finer levels left, and the answer is the takes' weighted mean. Returns
// the share taken in all, and in `noise` the variance of the answer in units of one sample's
// (every cell's mean carrying a sample's variance over its count), which the walk's rule bounds.
f64 brute_walk(const LevelCells* levels, u32 count, u32 confident, f64& value,
               f64* noise = nullptr) {
  f64 left = 1.0;
  f64 acc = 0.0;
  f64 variance_sum = 0.0;
  for (u32 l = 0; l < count; ++l) {
    f64 mean_sum = 0.0;
    f64 confidence = 0.0;
    f64 square = 0.0;
    f64 inverse = 0.0;
    for (u32 i = 0; i < 8; ++i) {
      const f64 n = static_cast<f64>(levels[l].samples[i]);
      if (n == 0.0) continue;
      const f64 c = levels[l].weight[i] * std::fmin(n / static_cast<f64>(confident), 1.0);
      mean_sum += c * levels[l].mean[i];
      confidence += c;
      square += c * c;
      inverse += c * c / n;
    }
    if (!(confidence > 1e-6)) continue;
    const f64 a_cell = square / inverse;  // the harmonic mean of the counts
    const f64 share = left * std::fmin(a_cell / static_cast<f64>(confident), 1.0) *
                      std::fmin(static_cast<f64>(k_gi_read_cover) * confidence, 1.0);
    acc += share * (mean_sum / confidence);
    variance_sum += share * share * inverse / (confidence * confidence);
    left -= share;
    if (left <= static_cast<f64>(k_gi_read_stop)) break;
  }
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

f32 walk(const LevelCells* levels, u32 count, u32 confident, f32& value) {
  GiLevelRead reads[k_gi_ladder];
  for (u32 l = 0; l < count; ++l)
    reads[l] = level_read(levels[l], confident);
  f32 out[3] = {0.0f, 0.0f, 0.0f};
  const f32 taken = gi_blend_levels(reads, count, out, confident);
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
  }
  CHECK(compared > 2000);
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

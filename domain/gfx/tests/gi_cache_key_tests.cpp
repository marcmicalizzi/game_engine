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

// The young-cell blend as its definition reads, in double: every level's share of the whole —
// its confidence times what the finer levels left, all of what is left at the last level, nothing
// for a level without samples — then the shares' weighted mean of the levels' means.
bool brute_blend(const GiLevelRead* levels, u32 count, u32, f64 out[3]) {
  f64 left = 1.0;
  f64 total = 0.0;
  f64 acc[3] = {0.0, 0.0, 0.0};
  for (u32 i = 0; i < count; ++i) {
    const GiLevelRead& l = levels[i];
    if (!(l.weight > 1e-4f)) continue;
    const f64 c = i + 1 == count ? 1.0 : std::fmin(static_cast<f64>(l.confidence), 1.0);
    const f64 share = left * c;
    for (u32 k = 0; k < 3; ++k)
      acc[k] += share * static_cast<f64>(l.sum[k]) / static_cast<f64>(l.weight);
    total += share;
    left -= share;
  }
  if (!(total > 0.0)) {
    // Levels with samples whose confidence was zero and nothing after them: none took a share.
    bool any = false;
    for (u32 i = 0; i < count; ++i)
      any = any || levels[i].weight > 1e-4f;
    if (!any) return false;
    for (u32 k = 0; k < 3; ++k)
      out[k] = 0.0;
    return true;
  }
  for (u32 k = 0; k < 3; ++k)
    out[k] = acc[k] / total;
  return true;
}
u32 next(u32& state) {
  state = state * 1664525u + 1013904223u;
  return state >> 8;
}

}  // namespace

TEST_CASE("gi cache key: the young cell's read is the brute-force blend of its levels") {
  u32 state = 12345u;
  u32 compared = 0;
  for (u32 trial = 0; trial < 4000; ++trial) {
    GiLevelRead levels[4];
    const u32 count = 1 + next(state) % 4;
    for (u32 i = 0; i < count; ++i) {
      // A level is empty a quarter of the time, otherwise some of its eight corners had samples.
      if (next(state) % 4 == 0) continue;
      levels[i].weight = 0.05f + 0.95f * static_cast<f32>(next(state) % 1000) / 1000.0f;
      for (u32 c = 0; c < 3; ++c)
        levels[i].sum[c] = levels[i].weight * static_cast<f32>(next(state) % 4000) / 1000.0f;
      levels[i].confidence =
          levels[i].weight * static_cast<f32>(next(state) % 1500) / 1000.0f;  // past 1 too
    }
    f32 got[3];
    f64 want[3];
    const bool has = gi_blend_levels(levels, count, got);
    const bool brute = brute_blend(levels, count, 0, want);
    REQUIRE(has == brute);
    if (!has) continue;
    ++compared;
    for (u32 c = 0; c < 3; ++c)
      CHECK(std::fabs(static_cast<f64>(got[c]) - want[c]) <= 1e-4 * (1.0 + std::fabs(want[c])));
  }
  CHECK(compared > 2000);
  // A confident level alone decides; a young one hands the rest to its parent.
  GiLevelRead fine;
  fine.weight = 1.0f;
  fine.sum[0] = fine.sum[1] = fine.sum[2] = 0.2f;
  fine.confidence = 1.0f;
  GiLevelRead coarse;
  coarse.weight = 1.0f;
  coarse.sum[0] = coarse.sum[1] = coarse.sum[2] = 1.0f;
  coarse.confidence = 1.0f;
  GiLevelRead pair[2] = {fine, coarse};
  f32 out[3];
  REQUIRE(gi_blend_levels(pair, 2, out));
  CHECK(out[0] == doctest::Approx(0.2f));
  pair[0].confidence = 0.25f;  // eight samples of the thirty-two
  REQUIRE(gi_blend_levels(pair, 2, out));
  CHECK(out[0] == doctest::Approx(0.25f * 0.2f + 0.75f * 1.0f));
  GiLevelRead none[2];
  CHECK_FALSE(gi_blend_levels(none, 2, out));
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

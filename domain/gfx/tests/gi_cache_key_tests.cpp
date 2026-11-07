// The world-space cache's key (gi_cache.h; R69; docs/subsystems/renderer.md, "Indirect light: the
// world-space cache"): no device. The shader's key is the same functions operation for operation
// (gi_cache.slang), and `gi cache: a scene moved by whole cells draws the same bytes with the cache
// on` (systems/renderer) holds the two together on the GPU.

#include <domain/gfx/gi_cache.h>

#include <doctest/doctest.h>

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

// Opacity micromaps (gfx/opacity_micromap.h; docs/subsystems/renderer.md, "Opacity micromaps";
// roadmap R120): the arithmetic the bake and the budget rest on, the micro-triangle order, the
// conservative rule on the CPU, and the cluster build record with and without a micromap.

#include <domain/gfx/cluster_acceleration.h>
#include <domain/gfx/opacity_micromap.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

using namespace engine;

namespace {

f32 area(const f32 t[6]) {
  return 0.5f * std::fabs((t[2] - t[0]) * (t[5] - t[1]) - (t[3] - t[1]) * (t[4] - t[0]));
}

bool contains(const f32 t[6], f32 x, f32 y) {
  auto edge = [](f32 ax, f32 ay, f32 bx, f32 by, f32 px, f32 py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
  };
  const f32 e0 = edge(t[0], t[1], t[2], t[3], x, y);
  const f32 e1 = edge(t[2], t[3], t[4], t[5], x, y);
  const f32 e2 = edge(t[4], t[5], t[0], t[1], x, y);
  return (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
}

// An 8x8 mask of 2x2-texel blocks in a checker, 255 kept and 0 cut (alpha_mask_tests' image).
std::vector<u8> checker() {
  std::vector<u8> alpha(64);
  for (u32 y = 0; y < 8; ++y)
    for (u32 x = 0; x < 8; ++x)
      alpha[y * 8 + x] = ((x / 2 + y / 2) & 1u) == 0 ? 255 : 0;
  return alpha;
}

}  // namespace

TEST_CASE("micromap: the 4-state arithmetic is the page's") {
  // 4^L micro-triangles at two bits each: 16 bytes at level 3, 64 at level 4.
  CHECK(gfx::micromap_state_bytes(3) == 16);
  CHECK(gfx::micromap_state_bytes(4) == 64);
  CHECK(gfx::micromap_state_bytes(0) == 1);
  // The engine pads an entry to whole words, so the bake writes a word a lane.
  CHECK(gfx::micromap_entry_bytes(0) == 4);
  CHECK(gfx::micromap_entry_bytes(1) == 4);
  CHECK(gfx::micromap_entry_bytes(2) == 4);
  CHECK(gfx::micromap_entry_bytes(3) == 16);
  CHECK(gfx::micromap_entry_bytes(5) == 256);
}

TEST_CASE("micromap: the micro-triangles tile the triangle in the format's order") {
  for (u32 level = 0; level <= 4; ++level) {
    const u32 count = 1u << (2 * level);
    f32 total = 0.0f;
    for (u32 m = 0; m < count; ++m) {
      f32 t[6];
      gfx::micromap_micro_triangle(m, level, t);
      CHECK(area(t) == doctest::Approx(0.5f / static_cast<f32>(count)));
      for (u32 c = 0; c < 3; ++c) {
        CHECK(t[2 * c] >= 0.0f);
        CHECK(t[2 * c + 1] >= 0.0f);
        CHECK(t[2 * c] + t[2 * c + 1] <= 1.0f + 1e-6f);
      }
      total += area(t);
    }
    CHECK(total == doctest::Approx(0.5f));
  }
  // Level 1: the child nearest vertex 0, the middle one, the one nearest vertex 1, the one
  // nearest vertex 2 (meshoptimizer's `rasterizeOpacityRec`, the format's bird order).
  f32 t[6];
  gfx::micromap_micro_triangle(0, 1, t);
  CHECK(contains(t, 0.1f, 0.1f));
  gfx::micromap_micro_triangle(1, 1, t);
  CHECK(contains(t, 1.0f / 3.0f, 1.0f / 3.0f));
  gfx::micromap_micro_triangle(2, 1, t);
  CHECK(contains(t, 0.8f, 0.1f));
  gfx::micromap_micro_triangle(3, 1, t);
  CHECK(contains(t, 0.1f, 0.8f));
  // Level 2 recurses inside the parent: micro-triangle 9 is child 1 of child 2.
  gfx::micromap_micro_triangle(9, 2, t);
  f32 parent[6];
  gfx::micromap_micro_triangle(2, 1, parent);
  const f32 cx = (t[0] + t[2] + t[4]) / 3.0f;
  const f32 cy = (t[1] + t[3] + t[5]) / 3.0f;
  CHECK(contains(parent, cx, cy));
}

TEST_CASE("micromap: a micro-triangle is known only where every texel it can read agrees") {
  const std::vector<u8> alpha = checker();
  const std::span<const u8> mask(alpha.data(), alpha.size());
  auto small = [&](f32 tx, f32 ty) {
    // A triangle a tenth of a texel across around texel-space point (tx, ty).
    const f32 d = 0.05f / 8.0f;
    const f32 u = tx / 8.0f;
    const f32 v = ty / 8.0f;
    const f32 a[2] = {u - d, v - d};
    const f32 b[2] = {u + d, v - d};
    const f32 c[2] = {u, v + d};
    return gfx::micromap_reference_state(mask, 8, 8, 1.0f, 0.5f, a, b, c);
  };
  // The middle of a kept block reads only that block's four texels: opaque.
  CHECK(small(1.0f, 1.0f) == gfx::k_micromap_opaque);
  // The middle of a cut block: transparent.
  CHECK(small(3.0f, 1.0f) == gfx::k_micromap_transparent);
  // Where four blocks meet the lookup reads both kinds: unknown.
  const u32 corner = small(2.0f, 2.0f);
  CHECK(
      (corner == gfx::k_micromap_unknown_opaque || corner == gfx::k_micromap_unknown_transparent));
  // The factor scales the alpha first: a kept block at half alpha against a 0.5 cutoff is within
  // the margin of the cutoff, so it is unknown, never opaque.
  const f32 d = 0.05f / 8.0f;
  const f32 a[2] = {1.0f / 8.0f - d, 1.0f / 8.0f - d};
  const f32 b[2] = {1.0f / 8.0f + d, 1.0f / 8.0f - d};
  const f32 c[2] = {1.0f / 8.0f, 1.0f / 8.0f + d};
  const u32 half = gfx::micromap_reference_state(mask, 8, 8, 0.5f, 0.5f, a, b, c);
  CHECK(half != gfx::k_micromap_opaque);
  CHECK(half != gfx::k_micromap_transparent);
  // The whole texture's footprint reads everything: unknown.
  const f32 w0[2] = {0.0f, 0.0f};
  const f32 w1[2] = {1.0f, 0.0f};
  const f32 w2[2] = {0.0f, 1.0f};
  const u32 whole = gfx::micromap_reference_state(mask, 8, 8, 1.0f, 0.5f, w0, w1, w2);
  CHECK(whole != gfx::k_micromap_opaque);
  CHECK(whole != gfx::k_micromap_transparent);
}

TEST_CASE("micromap: a cluster record names no micromap unless it is given one") {
  gfx::ClusterBuildInput plain;
  plain.cluster_id = 7;
  plain.triangle_count = 2;
  plain.vertex_count = 4;
  plain.vertices = 0x1000;
  plain.indices = 0x2000;
  plain.opaque = false;
  u8 before[gfx::k_cluster_build_record_bytes];
  gfx::write_cluster_build_records(std::span<const gfx::ClusterBuildInput>(&plain, 1), before);
  // The layout cluster_acceleration.h documents, with the micromap's fields all zero: the record
  // a cluster with no micromap was before R120.
  u8 expected[gfx::k_cluster_build_record_bytes] = {};
  auto put32 = [&](u32 offset, u32 v) { std::memcpy(expected + offset, &v, 4); };
  auto put64 = [&](u32 offset, u64 v) { std::memcpy(expected + offset, &v, 8); };
  put32(0, 7);
  put32(4, 0);
  put32(8, 2u | (4u << 9) | (gfx::k_cluster_index_type_8bit << 24));
  put32(12, 7u);  // geometry index 7, no flag: alpha-tested, not opaque
  put32(16, 1u | (12u << 16));
  put32(20, 0);
  put64(24, 0x2000);
  put64(32, 0x1000);
  put64(40, 0);
  put64(48, 0);
  put64(56, 0);
  CHECK(std::memcmp(before, expected, sizeof(expected)) == 0);

  gfx::ClusterBuildInput with = plain;
  with.omm_array = 0x30000;
  with.omm_indices = 0x40010;
  u8 after[gfx::k_cluster_build_record_bytes];
  gfx::write_cluster_build_records(std::span<const gfx::ClusterBuildInput>(&with, 1), after);
  put32(8, 2u | (4u << 9) | (gfx::k_cluster_index_type_8bit << 24) |
               (gfx::k_cluster_index_type_32bit << 28));
  put32(20, 4u << 16);  // a 32-bit index a triangle
  put64(48, 0x30000);
  put64(56, 0x40010);
  CHECK(std::memcmp(after, expected, sizeof(expected)) == 0);
}

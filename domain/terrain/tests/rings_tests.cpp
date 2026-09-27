// The terrain rings (docs/subsystems/terrain.md, "Rings"): which ring a point is in, that the
// rings' meshes cover the outer square with no gap and no overlap, the re-centre rule step by step,
// the skirts' winding, vertices that stay on the world's grid when a ring moves, and a rebuild that
// is the same bytes on one, two, three and sixteen workers, and every skirt vertex of the chunks
// the clamps clip thin against the grid vertex it hangs from.
#include <core/containers/detail/raw_storage.h>
#include <core/jobs/job_system.h>
#include <domain/terrain/fixed.h>
#include <domain/terrain/terrain.h>

#include <doctest/doctest.h>

#include <bit>
#include <string>
#include <unordered_map>

using namespace engine;
using namespace engine::terrain;

namespace {

// Rings small enough for a debug build: an inner ring 32 m either side at 50 cm, a middle ring
// 128 m either side at 1 m, and an outer 512 m either side at 2 m round (0, 0).
RingParams small_rings() {
  RingParams p;
  p.count = 3;
  p.ring[0] = RingSpec{32'000, 500, 4'000};
  p.ring[1] = RingSpec{128'000, 1'000, 8'000};
  p.ring[2] = RingSpec{512'000, 2'000, 16'000};
  p.uv_x0_mm = -512'000;
  p.uv_z0_mm = -512'000;
  p.uv_size_mm = 1'024'000;
  return p;
}

const DuneField& field() {
  static const DuneField f = [] {
    FieldDesc d;
    d.seed = 2026;
    d.wind.seed = 2026;
    return DuneField(d);
  }();
  return f;
}

bool inside(const Ring& inner, const Ring& outer) {
  return inner.cx - inner.half >= outer.cx - outer.half &&
         inner.cx + inner.half <= outer.cx + outer.half &&
         inner.cz - inner.half >= outer.cz - outer.half &&
         inner.cz + inner.half <= outer.cz + outer.half;
}

// A vertex's (x, z) as one key: a skirt vertex copies its top's x and z bit for bit.
u64 column_key(Vec3 p) noexcept {
  return (static_cast<u64>(std::bit_cast<u32>(p.x)) << 32) | std::bit_cast<u32>(p.z);
}

// The capacity `pushes` pushes onto an empty `Vector<T, u32>` leave it with, by the containers'
// growth policy (1.5x from a floor of four): where the next push reallocates.
u32 capacity_after(u32 pushes) noexcept {
  u32 cap = 0;
  for (u32 n = 0; n < pushes; ++n) {
    if (n == cap) cap = containers::detail::grow_capacity(cap, n + 1, ~0u);
  }
  return cap;
}

// Every skirt vertex of `mesh` against the grid vertex it hangs from: the same x and z, `drop`
// below it, the same normal and UV, and locked. Counts in `wrong` the ones it could not match or
// that differ, and returns how many it checked.
u32 check_skirt_vertices(const RingMesh& mesh, f32 drop, u32& wrong) {
  std::unordered_map<u64, u32> tops;
  for (u32 v = 0; v < mesh.grid_vertices; ++v)
    tops.emplace(column_key(mesh.positions[v]), v);
  u32 checked = 0;
  for (u32 v = mesh.grid_vertices; v < mesh.positions.size(); ++v) {
    const auto it = tops.find(column_key(mesh.positions[v]));
    if (it == tops.end()) {
      ++wrong;
      continue;
    }
    const u32 t = it->second;
    const bool same = mesh.positions[v].y == mesh.positions[t].y - drop &&
                      mesh.normals[v].x == mesh.normals[t].x &&
                      mesh.normals[v].y == mesh.normals[t].y &&
                      mesh.normals[v].z == mesh.normals[t].z && mesh.uvs[v].x == mesh.uvs[t].x &&
                      mesh.uvs[v].y == mesh.uvs[t].y && mesh.locked[v] == 1;
    wrong += same ? 0u : 1u;
    ++checked;
  }
  return checked;
}

}  // namespace

TEST_CASE("terrain rings: valid parameters, and the ones the rules refuse") {
  std::string error;
  CHECK(validate_rings(small_rings(), &error));
  RingParams p = small_rings();
  p.ring[1].half_mm = 64'000;  // not three times the inner ring
  CHECK_FALSE(validate_rings(p, &error));
  CHECK(error.find("three times") != std::string::npos);
  p = small_rings();
  p.ring[0].half_mm = 32'500;  // off the step
  CHECK_FALSE(validate_rings(p, &error));
  p = small_rings();
  p.ring[1].spacing_mm = 400;  // finer than the ring inside it
  CHECK_FALSE(validate_rings(p, &error));
  p = small_rings();
  p.outer_cx_mm = 1'000;  // the outer corner off its own grid
  CHECK_FALSE(validate_rings(p, &error));
  // The tunables' default: 250 m at 50 cm, 1 km at 1 m, and the erg's grid, 3,072 m at 1.5 m.
  const RingParams erg = ring_params_from_tunables(0, 0, 3'072'000, 1'500);
  CHECK(validate_rings(erg, &error));
  REQUIRE(erg.count == 3);
  CHECK(erg.ring[0].half_mm == 250'000);
  CHECK(erg.ring[1].half_mm == 999'000);  // rounded down to a multiple of lcm(1 m, 1.5 m) = 3 m
  CHECK(ring_snap_mm(erg, 1) == 3'000);
}

TEST_CASE("terrain rings: which ring a point is in, and every point in exactly one") {
  const RingParams p = small_rings();
  RingLayout layout;
  place_rings(p, 7'300, -12'100, layout);
  REQUIRE(layout.count == 3);
  CHECK(layout.ring[0].cx == 7'000);  // the camera snapped to lcm(0.5 m, 1 m)
  CHECK(layout.ring[0].cz == -12'000);
  CHECK(layout.ring[1].cx == 8'000);  // to lcm(1 m, 2 m)
  CHECK(layout.ring[2].cx == 0);
  CHECK(ring_of(layout, 7'300, -12'100) == 0);
  CHECK(ring_of(layout, 7'000 + 31'999, -12'000) == 0);
  CHECK(ring_of(layout, 7'000 + 32'000, -12'000) == 1);  // half-open: the edge is the next ring's
  CHECK(ring_of(layout, 100'000, 0) == 1);
  CHECK(ring_of(layout, 400'000, 0) == 2);
  CHECK(ring_of(layout, 511'999, -512'000) == 2);
  CHECK(ring_of(layout, 512'000, 0) == -1);
  CHECK(ring_of(layout, 600'000, 0) == -1);
}

TEST_CASE("terrain rings: the meshes cover the outer square with no gap and no overlap") {
  const RingParams p = small_rings();
  const FieldRingHeights heights(field(), 0);
  // At the middle, and pressed against the outer ring's corner, where the clamps bite.
  for (const i64 camera : {i64{3'700}, i64{-505'000}}) {
    RingLayout layout;
    place_rings(p, camera, -camera / 2, layout);
    for (u32 k = 0; k + 1 < layout.count; ++k)
      CHECK(inside(layout.ring[k], layout.ring[k + 1]));
    i64 area_m2 = 0;
    u32 wrong = 0, down = 0, tilted = 0, skirts = 0, unlocked_border = 0;
    for (u32 k = 0; k < layout.count; ++k) {
      Vector<RingChunkCoord> chunks;
      ring_chunks(layout.ring[k], chunks);
      const i64 s = layout.ring[k].spacing;
      for (const RingChunkCoord chunk : chunks) {
        RingMesh mesh;
        build_ring_chunk_mesh(layout.ring[k], chunk, p, heights, nullptr, mesh);
        // Every grid cell is this ring's by `ring_of` (so no two rings share one) and inside
        // this chunk (so no two chunks do), and the cells add up to the outer square (so no ground
        // is left out).
        const i64 c = k_ring_chunk_cells * s;
        for (u32 t = 0; t < mesh.grid_triangles; t += 2) {
          const Vec3 a = mesh.positions[mesh.indices[t * 3]];
          const i64 x = static_cast<i64>(std::llround(a.x * 1000.0f)) + s / 2;
          const i64 z = static_cast<i64>(std::llround(a.z * 1000.0f)) + s / 2;
          wrong += ring_of(layout, x, z) != static_cast<i32>(k);
          wrong += fx::floor_div(x, c) != chunk.i || fx::floor_div(z, c) != chunk.j;
        }
        area_m2 += static_cast<i64>(mesh.grid_triangles / 2) * s * s;
        // Grid triangles face up; skirt triangles are vertical.
        for (u32 t = 0; t < mesh.indices.size() / 3; ++t) {
          const Vec3 a = mesh.positions[mesh.indices[t * 3]];
          const Vec3 b = mesh.positions[mesh.indices[t * 3 + 1]];
          const Vec3 cc = mesh.positions[mesh.indices[t * 3 + 2]];
          const Vec3 n = cross(b - a, cc - a);
          if (t < mesh.grid_triangles)
            down += n.y <= 0.0f;
          else
            tilted += std::fabs(n.y) > 1e-3f * length(n);
        }
        skirts += mesh.skirt_triangles;
        // Every vertex a neighbouring chunk also has is locked.
        for (u32 v = 0; v < mesh.grid_vertices; ++v) {
          const i64 x = std::llround(static_cast<f64>(mesh.positions[v].x) * 1000.0);
          const i64 z = std::llround(static_cast<f64>(mesh.positions[v].z) * 1000.0);
          const bool on_line = fx::floor_mod(x, c) == 0 || fx::floor_mod(z, c) == 0;
          unlocked_border += on_line && mesh.locked[v] == 0;
        }
      }
    }
    CHECK(wrong == 0u);
    CHECK(down == 0u);
    CHECK(tilted == 0u);
    CHECK(skirts > 0u);
    CHECK(unlocked_border == 0u);
    CHECK(area_m2 == i64{1'024'000} * 1'024'000);
  }
}

TEST_CASE("terrain rings: the skirts face away from the ring's own ground") {
  const RingParams p = small_rings();
  RingLayout layout;
  place_rings(p, 0, 0, layout);
  // A skirt on the outer border faces out of the square; one on the hole's border faces into the
  // hole. Either way, away from the ring's ground: its centroid pushed along its normal leaves the
  // ring.
  u32 wrong = 0, skirts = 0;
  Vector<RingChunkCoord> chunks;
  ring_chunks(layout.ring[1], chunks);
  for (const RingChunkCoord chunk : chunks) {
    RingMesh mesh;
    build_ring_chunk_mesh(layout.ring[1], chunk, p, FieldRingHeights(field(), 0), nullptr, mesh);
    for (u32 t = mesh.grid_triangles; t < mesh.indices.size() / 3; ++t) {
      const Vec3 a = mesh.positions[mesh.indices[t * 3]];
      const Vec3 b = mesh.positions[mesh.indices[t * 3 + 1]];
      const Vec3 c = mesh.positions[mesh.indices[t * 3 + 2]];
      const Vec3 n = normalize(cross(b - a, c - a));
      const Vec3 m = (a + b + c) * (1.0f / 3.0f) + n * 0.25f;
      const i64 x = static_cast<i64>(std::llround(m.x * 1000.0f));
      const i64 z = static_cast<i64>(std::llround(m.z * 1000.0f));
      wrong += ring_of(layout, x, z) == 1;
      ++skirts;
    }
  }
  CHECK(skirts == 2u * 4u * (256u + 64u));  // two triangles an edge, the square's and the hole's
  CHECK(wrong == 0u);
}

TEST_CASE("terrain rings: a skirt vertex keeps its top's attributes in a chunk clipped thin") {
  // The crash of 2026-09-26 (terrain.md, "Rings"): a skirt vertex took its top's normal and UV
  // by reference into the vector the push appended to, and `Vector` frees its old buffer before
  // it reads the argument, so the push that grew the vector read freed memory — a segfault when
  // the freed block had been unmapped, silently the top's stale bytes (or the allocator's own)
  // when it had not. The grid's pushes leave a capacity a full chunk's skirts stay inside (16,641
  // vertices, capacity 18,207, at most 516 skirt vertices) and a chunk clipped to a few rows along
  // a ring's border does not: 129 x 3 vertices are 387 and a capacity of 474, and the border's
  // 129 skirt vertices cross it. Every layout has such chunks, since a ring's edge falls where the
  // camera put it, not on a chunk line. So: every skirt vertex against its top, bit for bit, over
  // the coverage test's layouts and a ring whose corner lies one cell into a chunk — that chunk's
  // four grid vertices are exactly the capacity four pushes leave, so the first skirt vertex's
  // push is the one that grows the vector, and what it copies is vertex 0, the first bytes of the
  // freed buffer, the ones an allocator overwrites first. Under ASan the old code is a
  // use-after-free report on the first such chunk; `grew` says the reallocation happened.
  const RingParams p = small_rings();
  const FieldRingHeights heights(field(), 0);
  u32 wrong = 0, checked = 0, grew = 0;
  const auto check_layout = [&](const RingLayout& layout) {
    for (u32 k = 0; k < layout.count; ++k) {
      Vector<RingChunkCoord> chunks;
      ring_chunks(layout.ring[k], chunks);
      const f32 drop = static_cast<f32>(static_cast<f64>(layout.ring[k].skirt) / 1000.0);
      for (const RingChunkCoord chunk : chunks) {
        RingMesh mesh;
        build_ring_chunk_mesh(layout.ring[k], chunk, p, heights, nullptr, mesh);
        checked += check_skirt_vertices(mesh, drop, wrong);
        grew += mesh.positions.size() > capacity_after(mesh.grid_vertices) ? 1u : 0u;
      }
    }
  };
  for (const i64 camera : {i64{3'700}, i64{-505'000}}) {
    RingLayout layout;
    place_rings(p, camera, -camera / 2, layout);
    check_layout(layout);
  }
  CHECK(grew > 0u);
  CHECK(wrong == 0u);
  // The corner: a ring 64 m either side of (191, 191) m at a metre, its low corner at 127 m, one
  // cell before the chunk line at 128 m.
  RingLayout corner;
  corner.count = 1;
  corner.ring[0] =
      Ring{.cx = 191'000, .cz = 191'000, .half = 64'000, .spacing = 1'000, .skirt = 8'000};
  Vector<RingChunkCoord> chunks;
  ring_chunks(corner.ring[0], chunks);
  REQUIRE(chunks.size() == 4u);
  RingMesh one;
  build_ring_chunk_mesh(corner.ring[0], RingChunkCoord{0, 0}, p, heights, nullptr, one);
  CHECK(one.grid_vertices == 4u);
  CHECK(capacity_after(4) == 4u);
  CHECK(one.grid_triangles == 2u);
  CHECK(one.skirt_triangles == 4u);  // two border edges, two triangles each
  const u32 before = grew;
  check_layout(corner);
  CHECK(grew > before);
  CHECK(wrong == 0u);
  CHECK(checked > 0u);
}

TEST_CASE("terrain rings: the re-centre rule fires when the camera crosses half a ring") {
  const RingParams p = small_rings();
  RingLayout layout;
  place_rings(p, 0, 0, layout);
  // Walk east a metre at a time: the inner ring (32 m either side) moves the first time the camera
  // is more than 16 m from its centre, and then 16 m after each move; each move rebuilds it and the
  // middle ring (its hole moved), and the middle ring itself moves only past 64 m.
  u32 inner_moves = 0, middle_moves = 0;
  i64 inner_centre = 0;
  for (i64 x = 1'000; x <= 200'000; x += 1'000) {
    const i64 before_inner = layout.ring[0].cx;
    const i64 before_middle = layout.ring[1].cx;
    const bool inner_due = x - before_inner > 16'000;
    const bool middle_due = x - before_middle > 64'000;
    const u32 mask = recentre_rings(p, x, 0, layout);
    CHECK(((mask & 1u) != 0) == inner_due);
    CHECK(((mask & 4u) != 0) == middle_due);
    if (inner_due) {
      ++inner_moves;
      CHECK(layout.ring[0].cx == x);  // snapped to a metre, the camera's own position here
      CHECK((mask & 2u) != 0);        // its hole moved
      CHECK(x - inner_centre == 17'000);
      inner_centre = x;
    }
    middle_moves += middle_due;
    for (u32 k = 0; k + 1 < layout.count; ++k)
      CHECK(inside(layout.ring[k], layout.ring[k + 1]));
    if (!inner_due && !middle_due) CHECK(mask == 0u);
  }
  CHECK(inner_moves == 11u);
  CHECK(middle_moves == 3u);  // at 65, 131 and 197 m
  // Past the outer ring's edge the rings stay clamped inside it and stop moving.
  u32 late = 0;
  for (i64 x = 600'000; x <= 700'000; x += 1'000)
    late += recentre_rings(p, x, 0, layout) != 0;
  CHECK(late == 1u);
  CHECK(layout.ring[1].cx + layout.ring[1].half == 512'000);
}

TEST_CASE("terrain rings: a rebuild is the same bytes on any number of threads, moved or fresh") {
  const RingParams p = small_rings();
  const FieldRingHeights heights(field(), 3 * 365 * k_us_per_day);
  const geometry::ClusterLodOptions options;
  std::string error;
  TerrainRings serial;
  REQUIRE(serial.reset(p, 0, 0, heights, options, nullptr, &error));
  // Every grid vertex on the ring's spacing counted from the world's origin, so a ring that
  // moves samples the same points.
  u32 off_grid = 0;
  for (u32 k = 0; k < serial.count(); ++k) {
    const i64 s = serial.layout().ring[k].spacing;
    for (const Vec3 v : serial.lod(k).mesh.vertices) {
      const i64 x = std::llround(static_cast<f64>(v.x) * 1000.0);
      const i64 z = std::llround(static_cast<f64>(v.z) * 1000.0);
      off_grid += x % s != 0 || z % s != 0;
    }
    CHECK(serial.lod(k).level_cluster_counts.size() > 1u);  // a DAG, not one level
  }
  CHECK(off_grid == 0u);
  // One worker, two, three (the chunks split unevenly), and sixteen: more workers than the inner
  // ring has chunks, so most of them wait on a build they have no part in.
  jobs::JobSystem one(jobs::JobSystemConfig{.performance_workers = 1, .pin_threads = false});
  jobs::JobSystem two(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  jobs::JobSystem three(jobs::JobSystemConfig{.performance_workers = 3, .pin_threads = false});
  jobs::JobSystem sixteen(jobs::JobSystemConfig{.performance_workers = 16, .pin_threads = false});
  for (jobs::JobSystem* pool : {&one, &two, &three, &sixteen}) {
    TerrainRings again;
    REQUIRE(again.reset(p, 0, 0, heights, options, pool, &error));
    for (u32 k = 0; k < again.count(); ++k)
      CHECK(again.hash(k) == serial.hash(k));
  }
  // Move: the inner ring re-centres and the middle ring's hole with it; the middle ring (192 m
  // either side here, so most of its chunks miss the hole) keeps the chunks it still has, and both
  // end as the same bytes a build from scratch of the same layout gives.
  RingParams wide = p;
  wide.ring[0] = RingSpec{16'000, 500, 2'000};
  wide.ring[1] = RingSpec{192'000, 1'000, 8'000};
  REQUIRE(validate_rings(wide, &error));
  TerrainRings moving;
  REQUIRE(moving.reset(wide, 0, 0, heights, options, &three, &error));
  u32 rebuilt = 0;
  REQUIRE(moving.update(20'000, 0, &three, rebuilt, &error));
  CHECK(rebuilt == 3u);  // the inner ring, and the middle ring whose hole it is
  const u32 built = moving.last_built(), kept = moving.last_reused();
  CHECK(kept > 0u);
  u64 incremental[k_max_rings] = {};
  for (u32 k = 0; k < moving.count(); ++k)
    incremental[k] = moving.hash(k);
  moving.drop_chunks();
  REQUIRE(moving.rebuild((1u << moving.count()) - 1, &one, &error));
  for (u32 k = 0; k < moving.count(); ++k)
    CHECK(moving.hash(k) == incremental[k]);
  MESSAGE("rings: " << moving.chunks(0).size() << " + " << moving.chunks(1).size() << " + "
                    << moving.chunks(2).size() << " chunks, " << moving.lod(1).mesh.clusters.size()
                    << " clusters in the middle ring; a 20 m move rebuilt " << built
                    << " chunks and kept " << kept << "; hashes " << serial.hash(0) << " "
                    << serial.hash(1) << " " << serial.hash(2) << " moved " << incremental[0] << " "
                    << incremental[1] << " " << incremental[2]);
}

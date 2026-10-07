// The rings' chunks at their corners, on the CPU (docs/subsystems/renderer.md, "The ground's tiles
// are placed at their corners", "The rings in the scene"). The ground provider builds a ring's
// chunk in its corner's frame (`scene_gen::RingChunkRef`): positions metres from the corner and UVs
// measured from it, so a chunk's DAG does not depend on where it stands. The set stands the chunk
// at that corner, reads its rest heights off the DAG at the ring's lattice points — the corner's
// lattice index plus each vertex's own place from it — and nothing else moves it. Here: every
// chunk's corner is its chunk indices' corner, every grid vertex of its DAG lies within the chunk
// on the ring's lattice, every skirt vertex hangs below one, and every rest height is the ground's
// at the lattice point the set filed it under.
//
// Compiled only where the terrain capability is: the dunes are the ground with rings.
#include <core/jobs/job_system.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_rings.h>

#include <doctest/doctest.h>

#include <cmath>
#include <span>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

// The rings shrunk to a 128 m terrain at a metre, as the ring tests elsewhere have them: an inner
// ring 6 m either side at 25 cm (32 m chunks) and a middle one 20 m either side at 50 cm (64 m
// chunks). The tunables are the process's, so they are put back when the case ends.
struct SmallRings {
  SmallRings() {
    set("terrain.rings.inner_half_m", "6");
    set("terrain.rings.inner_spacing_cm", "25");
    set("terrain.rings.middle_half_m", "20");
    set("terrain.rings.middle_spacing_cm", "50");
  }
  ~SmallRings() {
    for (const char* name : {"terrain.rings.inner_half_m", "terrain.rings.inner_spacing_cm",
                             "terrain.rings.middle_half_m", "terrain.rings.middle_spacing_cm"}) {
      if (tunables::Tunable* t = tunables::find(name)) t->reset();
    }
  }
  static void set(const char* name, const char* value) {
    tunables::Tunable* t = tunables::find(name);
    REQUIRE_MESSAGE(t != nullptr, name);
    std::string error;
    REQUIRE_MESSAGE(t->set_from_text(value, &error), error);
  }
};

TerrainDesc ring_terrain() {
  TerrainDesc t;
  t.enabled = true;
  t.size = 129;
  t.extent = 64.0f;
  t.seed = 5;
  t.dune_height = 2.0f;
  t.dune_wavelength = 20.0f;
  t.generator = TerrainGenerator::dunes;
  t.time_s = 94'608'000.0;
  return t;
}

}  // namespace

TEST_CASE("terrain rings: a chunk stands at its corner, its DAG from it, its rest on the lattice") {
  const SmallRings small;
  const TerrainDesc terrain = ring_terrain();
  jobs::JobSystem jobs(jobs::JobSystemConfig{.performance_workers = 4, .pin_threads = false});
  TerrainRingSet rings;
  std::string error;
  // Off the lattices, so the rings' centres snap, and either side of no edge in particular.
  REQUIRE_MESSAGE(rings.build(terrain, WorldPos{5.3, 0.0, -7.9}, &jobs, &error), error);
  REQUIRE(rings.level_count() == 3);
  const TerrainSampler sampler(terrain);
  REQUIRE(sampler.ok());
  u32 chunks = 0, corners_wrong = 0, off_chunk = 0, off_lattice = 0, skirts_wrong = 0;
  u32 rests = 0, rests_wrong = 0, window_wrong = 0;
  for (u32 level = 1; level < rings.level_count(); ++level) {
    const TerrainLattice& lattice = rings.lattice(level);
    const i64 s = lattice.spacing_mm;
    const i64 chunk_mm = 128 * s;  // terrain::k_ring_chunk_cells spacings
    const f64 chunk_m = static_cast<f64>(chunk_mm) / 1000.0;
    for (const TerrainChunk& chunk : rings.chunks(level)) {
      const geometry::ClusterMesh& mesh = chunk.lod.mesh;
      if (mesh.clusters.empty()) continue;
      ++chunks;
      corners_wrong += chunk.corner_x_mm == i64{chunk.i} * chunk_mm &&
                               chunk.corner_z_mm == i64{chunk.j} * chunk_mm
                           ? 0u
                           : 1u;
      // Every vertex of every level of the DAG is a copy of a source vertex, so a grid vertex lies
      // on the ring's lattice inside the chunk, measured from its corner, and a skirt's below one.
      for (u32 v = 0; v < mesh.vertices.size(); ++v) {
        const Vec3 p = mesh.vertices[v];
        const f64 x = static_cast<f64>(p.x);
        const f64 z = static_cast<f64>(p.z);
        off_chunk += x >= 0.0 && x <= chunk_m && z >= 0.0 && z <= chunk_m ? 0u : 1u;
        const f64 fi = x * 1000.0 / static_cast<f64>(s);
        const f64 fj = z * 1000.0 / static_cast<f64>(s);
        off_lattice += fi == std::round(fi) && fj == std::round(fj) ? 0u : 1u;
        const bool skirt =
            v < mesh.vertex_source.size() && mesh.vertex_source[v] >= chunk.grid_vertices;
        if (skirt) {
          // A skirt hangs below the rest height of the lattice point it is under.
          const i64 i = chunk.corner_x_mm / s + static_cast<i64>(std::llround(fi));
          const i64 j = chunk.corner_z_mm / s + static_cast<i64>(std::llround(fj));
          const gfx::TerrainField& w = chunk.rest_window;
          const bool in =
              i >= w.i0 && j >= w.j0 && i < i64{w.i0} + i64{w.nx} && j < i64{w.j0} + i64{w.nz};
          const auto at = [&] { return static_cast<u32>((j - w.j0) * i64{w.nx} + (i - w.i0)); };
          skirts_wrong += in && p.y < chunk.rest[at()] ? 0u : 1u;
        }
      }
      // The rest window is inside the chunk's lattice square, and every rest height is the
      // ground's at its lattice point, as the set's source handed it over: in whole micrometres.
      const gfx::TerrainField& w = chunk.rest_window;
      const i64 ci = chunk.corner_x_mm / s;
      const i64 cj = chunk.corner_z_mm / s;
      window_wrong += w.i0 >= ci && w.j0 >= cj && i64{w.i0} + i64{w.nx} <= ci + 129 &&
                              i64{w.j0} + i64{w.nz} <= cj + 129
                          ? 0u
                          : 1u;
      Vector<f32> ground(w.nx * w.nz);
      REQUIRE(evaluate_terrain_window(sampler, terrain.time_s, lattice, w.i0, w.j0, w.nx, w.nz, 0,
                                      terrain_window_blocks(w.nx, w.nz),
                                      std::span<f32>(ground.data(), ground.size())));
      for (u32 k = 0; k < ground.size(); ++k) {
        const f32 rest = chunk.rest[k];
        if (std::isnan(rest)) continue;
        ++rests;
        const f32 want = static_cast<f32>(
            static_cast<f64>(std::llround(static_cast<f64>(ground[k]) * 1.0e6)) / 1.0e6);
        rests_wrong += rest == want ? 0u : 1u;
      }
    }
  }
  MESSAGE(chunks << " chunks over two rings, " << rests << " rest heights");
  CHECK(chunks >= 4u);
  CHECK(corners_wrong == 0u);
  CHECK(off_chunk == 0u);
  CHECK(off_lattice == 0u);
  CHECK(skirts_wrong == 0u);
  CHECK(window_wrong == 0u);
  CHECK(rests > 1000u);
  CHECK(rests_wrong == 0u);
}

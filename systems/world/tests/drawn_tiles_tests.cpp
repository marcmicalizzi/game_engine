// The drawn ground (docs/subsystems/world.md, "The consumers"; drawn_tiles.h): the world's ring
// hands the renderer's tile set exactly the tiles it holds, at their rings; its first update from
// nothing is the renderer's first layout, so nothing is rebuilt; a moving observer changes the set
// and the renderer takes the change; an update that changes nothing hands nothing over. No device
// and no generator: heights come from a flat tile source of the test's own.
#include <core/memory/memory.h>
#include <domain/scene_gen/tile_source.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_tiles.h>
#include <systems/world/drawn_tiles.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

using namespace engine;
using namespace engine::world;

namespace {

bool flat_heights(const void*, f64, i64, i32, i32, u32 nx, u32 nz, u32, u32,
                  std::span<f32> out) noexcept {
  if (out.size() != static_cast<usize>(nx) * nz) return false;
  std::fill(out.begin(), out.end(), 0.0f);
  return true;
}
constexpr scene_gen::TileSourceOps k_flat_ops{.heights = &flat_heights};
const scene_gen::TileSource k_flat{&k_flat_ops, nullptr};

sim::ObserverSet at(Vec3 p) {
  sim::ObserverSet set;
  set.add(p, 1.0f);
  return set;
}

}  // namespace

TEST_CASE("world drawn ground: the ring hands the renderer the tiles it holds, at their rings") {
  renderer::TerrainDesc grid;
  grid.enabled = true;
  grid.size = 3;
  grid.extent = 64.0f;
  renderer::TerrainTilesDesc t;
  t.tile_size = 8.0f;
  t.ring_count = 3;
  t.radius[0] = 1.5f;
  t.radius[1] = 3.0f;
  t.radius[2] = 6.0f;
  t.cells[0] = 8;
  t.cells[1] = 4;
  t.cells[2] = 2;
  renderer::TerrainTileSet set;
  std::string error;
  const Vec3 camera{5.0f, 40.0f, -3.0f};
  REQUIRE_MESSAGE(set.build(grid, t, k_flat, camera.x, camera.z, nullptr, &error), error);
  const u64 first_generation = set.generation();

  World world;
  RingParams params = DrawnTiles::ring_params(t);
  params.max_activations = 0;  // the tests' updates are whole
  params.max_deactivations = 0;
  REQUIRE(world.configure(params));
  DrawnTiles drawn;
  drawn.create(set);
  world.add_consumer(drawn.consumer());

  // The world's first update from nothing is the renderer's first layout: handed over, and taken
  // as no change at all.
  world.update(at(camera), 0, true);
  CHECK(drawn.stats().commits == 1);
  CHECK(drawn.stats().changed == 0);
  CHECK(set.generation() == first_generation);
  Vector<renderer::TerrainTile> round;
  renderer::terrain_tiles_round(t, camera.x, camera.z, round);
  CHECK(drawn.held().size() == round.size());
  CHECK(set.wanted().size() == round.size());

  // Nothing moves: no commit.
  world.update(at(camera), 1);
  CHECK(drawn.stats().commits == 1);

  // The observer walks two tiles east: the ring's events reach the set as one change, and what the
  // set wants is what the ring holds, tile for tile, ring for ring.
  const Vec3 east{camera.x + 16.0f, camera.y, camera.z};
  world.update(at(east), 2);
  CHECK(drawn.stats().commits == 2);
  CHECK(drawn.stats().changed == 1);
  CHECK(set.generation() == first_generation + 1);
  u32 mismatched = 0;
  for (const renderer::TerrainTile& tile : set.wanted()) {
    const u8* ring = drawn.held().find_value(tile_key(TileCoord{tile.x, tile.z}));
    mismatched += ring == nullptr || *ring != tile.ring ? 1u : 0u;
  }
  CHECK(mismatched == 0);
  CHECK(set.wanted().size() == drawn.held().size());
  // With hysteresis a tile the ring still holds past its radius is in the set too, at the ring it
  // is in: the set is the world's, not a band round the camera.
  CHECK(drawn.stats().deactivated > 0);
  CHECK(drawn.stats().activated > round.size());

  // **A frame's hand-over allocates nothing in steady state**: the observer flies on east, and over
  // the second half of the flight the ring's update, this consumer's events and commit and the tile
  // set's `set_tiles` make no allocation on this thread (a tag is the calling thread's). Counted
  // where the build tracks tags (Debug).
  static const mem::TagId k_tag = mem::register_tag("drawn-tiles-frames");
  u64 allocations = 0;
  const u64 commits_before = drawn.stats().commits;
  constexpr u32 k_updates = 240;
  sim::ObserverSet observers;  // kept, as a host keeps its observers
  for (u32 u = 0; u < k_updates; ++u) {
    const Vec3 p{east.x + 0.75f * static_cast<f32>(u), east.y,
                 east.z + 0.25f * static_cast<f32>(u)};
    observers.clear();
    observers.add(p, 1.0f);
    const u64 before = mem::stats(k_tag).allocation_count;
    {
      const mem::TagScope scope(k_tag);
      world.update(observers, 3 + u);
    }
    if (u >= k_updates / 2) allocations += mem::stats(k_tag).allocation_count - before;
  }
  CHECK(drawn.stats().commits > commits_before + 20);
  if (mem::tracking_enabled()) {
    MESSAGE("the hand-over's second half: " << allocations << " allocations over " << k_updates / 2
                                            << " updates");
    CHECK(allocations == 0);
  }
}

TEST_CASE("world drawn ground: a budgeted ring behind a fast camera keeps the tile set whole") {
  // engine-view's frame, with the renderer's half on the CPU: the world's ring updated under its
  // default budget from a camera flying 1.67 m a frame, and then what the time-lapse does with the
  // tile set — the layout for the camera, the tiles taken, the rebuild. The set must never draw
  // fewer tiles than the ring is about to hold minus what its budget defers, and never lose a level.
  renderer::TerrainDesc grid;
  grid.enabled = true;
  grid.size = 3;
  grid.extent = 3072.0f;
  renderer::TerrainTilesDesc t;
  t.tile_size = 32.0f;
  t.ring_count = 4;
  t.radius[0] = 1.5f;
  t.radius[1] = 8.0f;
  t.radius[2] = 24.0f;
  t.radius[3] = 64.0f;
  t.cells[0] = 8;
  t.cells[1] = 4;
  t.cells[2] = 2;
  t.cells[3] = 1;
  const Vec3 start{-2000.0f, 100.0f, 0.0f};
  renderer::TerrainTileSet set;
  std::string error;
  REQUIRE_MESSAGE(set.build(grid, t, k_flat, start.x, start.z, nullptr, &error), error);
  World world;
  RingParams params = DrawnTiles::ring_params(t);
  REQUIRE(world.configure(params));
  DrawnTiles drawn;
  drawn.create(set);
  world.add_consumer(drawn.consumer());
  sim::ObserverSet observers;
  renderer::TerrainRingLayout shown = set.layout();
  u32 fewest[renderer::k_max_terrain_levels];
  for (u32 l = 0; l < renderer::k_max_terrain_levels; ++l)
    fewest[l] = ~0u;
  u32 worst_withheld = 0;
  for (u32 f = 0; f < 120; ++f) {
    const Vec3 p{start.x + 1.6667f * static_cast<f32>(f), start.y, start.z};
    observers.clear();
    observers.add(p, 1.0f);
    if (f == 0) world.clear(0);
    world.update(observers, f, f == 0);
    const renderer::TerrainRingLayout next = set.next_layout(p.x, p.z, shown);
    if (next == shown) continue;
    set.prepare(next);
    u32 moved = 0;
    REQUIRE_MESSAGE(set.update(p.x, p.z, 0.0, next, {}, nullptr, moved, &error), error);
    shown = set.layout();
    worst_withheld = std::max(worst_withheld, set.withheld());
    for (u32 l = 1; l < set.level_count(); ++l)
      fewest[l] = std::min<u32>(fewest[l], set.chunks(l).size());
  }
  MESSAGE("fewest tiles a level drew: " << fewest[1] << ", " << fewest[2] << ", " << fewest[3]
                                        << ", " << fewest[4] << "; most withheld "
                                        << worst_withheld);
  for (u32 l = 1; l < set.level_count(); ++l)
    CHECK(fewest[l] > 0);
}

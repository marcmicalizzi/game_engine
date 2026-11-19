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

bool flat_heights(const void*, f64, i64, i64, i32, i32, u32 nx, u32 nz, u32, u32,
                  std::span<f32> out) noexcept {
  if (out.size() != static_cast<usize>(nx) * nz) return false;
  std::fill(out.begin(), out.end(), 0.0f);
  return true;
}
constexpr scene_gen::TileSourceOps k_flat_ops{.heights = &flat_heights};
const scene_gen::TileSource k_flat{&k_flat_ops, nullptr};

sim::ObserverSet at(WorldPos p) {
  sim::ObserverSet set;
  set.add(p, 1.0f);
  return set;
}

// Where the tile set is asked about: the camera on the ground plane, as the ring sees it.
WorldPos ground(WorldPos p) { return WorldPos{p.x, 0.0, p.z}; }

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
  const WorldPos camera{5.0, 40.0, -3.0};
  REQUIRE_MESSAGE(set.build(grid, t, k_flat, ground(camera), nullptr, &error), error);
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
  renderer::terrain_tiles_round(t, ground(camera), round);
  CHECK(drawn.held().size() == round.size());
  CHECK(set.held_count() == round.size());

  // Nothing moves: no commit.
  world.update(at(camera), 1);
  CHECK(drawn.stats().commits == 1);

  // The observer walks two tiles east: the ring's events reach the set as one change — the tiles
  // that entered, changed ring or left, not the whole set — and what the set holds is what the
  // ring holds, tile for tile, ring for ring.
  const WorldPos east{camera.x + 16.0, camera.y, camera.z};
  world.update(at(east), 2);
  CHECK(drawn.stats().commits == 2);
  CHECK(drawn.stats().changed == 1);
  CHECK(set.generation() == first_generation + 1);
  u32 mismatched = 0;
  for (u32 i = 0; i < set.held_count(); ++i) {
    const renderer::TerrainTile tile = set.held_at(i);
    const u8* ring = drawn.held().find_value(tile_key(TileCoord{tile.x, tile.z}));
    mismatched += ring == nullptr || *ring != tile.ring ? 1u : 0u;
  }
  CHECK(mismatched == 0);
  CHECK(set.held_count() == drawn.held().size());
  // With hysteresis a tile the ring still holds past its radius is in the set too, at the ring it
  // is in: the set is the world's, not a band round the camera.
  CHECK(drawn.stats().deactivated > 0);
  CHECK(drawn.stats().activated > round.size());

  // **A frame's hand-over allocates nothing in steady state**: the observer flies on east, and over
  // the second half of the flight the ring's update, this consumer's events and commit and the tile
  // set's `change_tiles` make no allocation on this thread (a tag is the calling thread's) — with
  // the set's changes taken after each, as the renderer's next rebuild takes them (`prepare`).
  // Counted where the build tracks tags (Debug).
  static const mem::TagId k_tag = mem::register_tag("drawn-tiles-frames");
  u64 allocations = 0;
  const u64 commits_before = drawn.stats().commits;
  constexpr u32 k_updates = 240;
  sim::ObserverSet observers;  // kept, as a host keeps its observers
  for (u32 u = 0; u < k_updates; ++u) {
    const WorldPos p{east.x + 0.75 * static_cast<f64>(u), east.y,
                     east.z + 0.25 * static_cast<f64>(u)};
    observers.clear();
    observers.add(p, 1.0f);
    const u64 before = mem::stats(k_tag).allocation_count;
    {
      const mem::TagScope scope(k_tag);
      world.update(observers, 3 + u);
      set.prepare(set.layout());
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

TEST_CASE(
    "world drawn ground: a camera that stands after a walk settles, and then nothing is "
    "activated, moved, laid out or rebuilt") {
  // Roadmap W149 (world.md, "A camera that stands"): engine-view's frame on the CPU — the world's
  // ring from the camera, then the time-lapse's half for the drawn ground (the layout the camera
  // asks for, the tiles taken, a rebuild when it differs) — over a world with far levels past its
  // rings, as the forest's staging is loaded (`--terrain-tiles --terrain-far`). The camera walks
  // 200 m at 100 m/s and stops; once a frame has asked for nothing, ten seconds of frames standing
  // (600 at 60 Hz) activate, move and deactivate no tile, ask for no new layout and rebuild no
  // chunk. The surface's time is the scene's own and does not pass (`--time-rate 0`): nothing the
  // two decisions read is a clock (`World::update` takes the observers alone, `next_layout` the
  // camera and the tiles held), so the world clock running changes neither. Both rings: under the
  // world's default budget (eight promotions an update, what a budgeted host runs), and unbudgeted
  // (engine-view's drawn ground).
  renderer::TerrainDesc grid;
  grid.enabled = true;
  grid.size = 3;
  grid.extent = 3072.0f;
  renderer::TerrainTilesDesc t;
  t.tile_size = 32.0f;
  t.ring_count = 3;
  t.radius[0] = 1.5f;
  t.radius[1] = 4.0f;
  t.radius[2] = 8.0f;
  t.cells[0] = 8;
  t.cells[1] = 4;
  t.cells[2] = 2;
  t.far_levels = 2;
  t.far_cells = 4;
  t.far_ratio = 2;
  std::string error;
  REQUIRE_MESSAGE(renderer::validate_terrain_tiles(t, &error), error);
  const WorldPos start{-100.0, 60.0, 37.0};
  constexpr u32 k_walk = 120;    // frames at 1.6667 m a frame: 200 m
  constexpr u32 k_stand = 600;   // ten seconds at 60 Hz
  constexpr u32 k_settle = 120;  // the most frames the place may take to settle
  struct Counts {
    u32 events = 0;
    u32 layouts = 0;  // frames whose camera asked for another layout than the one drawn
    u32 rebuilds = 0;
    u32 chunks_built = 0;
    u32 commits = 0;
    u64 generations = 0;
  };
  const auto stand = [&](bool budgeted, Counts& walking, Counts& standing, u32& settled_at) {
    renderer::TerrainTileSet set;
    REQUIRE_MESSAGE(set.build(grid, t, k_flat, ground(start), nullptr, &error), error);
    REQUIRE(set.far_levels() == 2);
    World world;
    RingParams params = DrawnTiles::ring_params(t);
    if (!budgeted) {
      params.max_activations = 0;
      params.max_deactivations = 0;
    }
    REQUIRE(world.configure(params));
    DrawnTiles drawn;
    drawn.create(set);
    world.add_consumer(drawn.consumer());
    sim::ObserverSet observers;
    renderer::TerrainRingLayout shown = set.layout();
    const WorldPos stop{start.x + 1.6667 * static_cast<f64>(k_walk), start.y, start.z};
    settled_at = ~0u;
    u32 standing_from = ~0u;
    for (u32 f = 0; f < k_walk + k_settle + k_stand; ++f) {
      const WorldPos p =
          f < k_walk ? WorldPos{start.x + 1.6667 * static_cast<f64>(f), start.y, start.z} : stop;
      observers.clear();
      observers.add(p, 1.0f);
      const u64 generation = set.generation();
      const u64 commits = drawn.stats().commits;
      world.update(observers, f, f == 0);
      const renderer::TerrainRingLayout next = set.next_layout(ground(p), shown);
      const bool asked = !(next == shown);
      Counts& c = f >= standing_from ? standing : walking;
      c.events += world.last().events;
      c.commits += static_cast<u32>(drawn.stats().commits - commits);
      c.generations += set.generation() - generation;
      if (asked) {
        ++c.layouts;
        set.prepare(next);
        u32 moved = 0;
        REQUIRE_MESSAGE(set.update(ground(p), 0.0, next, {}, nullptr, moved, &error), error);
        shown = set.layout();
        ++c.rebuilds;
        c.chunks_built += set.last_built();
      }
      // Settled: the camera has stopped, and a frame changed nothing and asked for nothing. The
      // standing count starts on the next frame and runs `k_stand` frames.
      if (f >= k_walk && standing_from == ~0u && world.last().events == 0 && !asked &&
          set.generation() == generation) {
        settled_at = f - k_walk;
        standing_from = f + 1;
      }
      if (standing_from != ~0u && f + 1 >= standing_from + k_stand) break;
    }
  };
  for (const bool budgeted : {true, false}) {
    Counts walking;
    Counts standing;
    u32 settled_at = ~0u;
    stand(budgeted, walking, standing, settled_at);
    MESSAGE(std::string(budgeted ? "budgeted" : "unbudgeted")
            << ": walking " << walking.events << " tile events, " << walking.rebuilds
            << " rebuilds, " << walking.chunks_built << " chunks built; settled " << settled_at
            << " frames after the stop; standing " << k_stand << " frames: " << standing.events
            << " events, " << standing.layouts << " layouts asked, " << standing.rebuilds
            << " rebuilds, " << standing.chunks_built << " chunks built");
    // The walk did move the place: the premise of what follows.
    CHECK(walking.events > 0);
    CHECK(walking.rebuilds > 0);
    REQUIRE(settled_at != ~0u);
    CHECK(settled_at <= k_settle);
    CHECK(standing.events == 0);
    CHECK(standing.commits == 0);
    CHECK(standing.generations == 0);
    CHECK(standing.layouts == 0);
    CHECK(standing.rebuilds == 0);
    CHECK(standing.chunks_built == 0);
  }
}

TEST_CASE(
    "world drawn ground: an unbudgeted ring keeps up with a fast camera, a budgeted one not") {
  // engine-view's frame with the renderer's half on the CPU, over desert-endless's rings: the
  // world's ring updated from a camera flying 1.67 m a frame (100 m/s at 60 Hz), and then what the
  // time-lapse does with the tile set — the layout for the camera, the tiles taken, the rebuild.
  // Under the world's default budget (eight promotions an update) the ring falls behind by the
  // promotions it defers, about two a frame here, and the outer level ends hundreds of tiles short;
  // with no budget — what engine-view's drawn ground takes (world_view.cpp) — every level keeps at
  // least what its first fill held, within the few tiles a ring's boundary crosses in a frame.
  // Neither ever empties a level.
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
  const WorldPos start{-2000.0, 100.0, 0.0};
  struct Flight {
    u32 first[renderer::k_max_terrain_levels] = {};
    u32 fewest[renderer::k_max_terrain_levels] = {};
    u32 last[renderer::k_max_terrain_levels] = {};
    u32 withheld = 0;
  };
  const auto fly = [&](bool budgeted, Flight& out) {
    renderer::TerrainTileSet set;
    std::string error;
    REQUIRE_MESSAGE(set.build(grid, t, k_flat, ground(start), nullptr, &error), error);
    World world;
    RingParams params = DrawnTiles::ring_params(t);
    if (!budgeted) {
      params.max_activations = 0;
      params.max_deactivations = 0;
    }
    REQUIRE(world.configure(params));
    DrawnTiles drawn;
    drawn.create(set);
    world.add_consumer(drawn.consumer());
    sim::ObserverSet observers;
    renderer::TerrainRingLayout shown = set.layout();
    for (u32 l = 1; l < set.level_count(); ++l) {
      out.first[l] = set.chunks(l).size();
      out.fewest[l] = out.first[l];
    }
    for (u32 f = 0; f < 360; ++f) {
      const WorldPos p{start.x + 1.6667 * static_cast<f64>(f), start.y, start.z};
      observers.clear();
      observers.add(p, 1.0f);
      if (f == 0) world.clear(0);
      world.update(observers, f, f == 0);
      const renderer::TerrainRingLayout next = set.next_layout(ground(p), shown);
      if (next == shown) continue;
      set.prepare(next);
      u32 moved = 0;
      REQUIRE_MESSAGE(set.update(ground(p), 0.0, next, {}, nullptr, moved, &error), error);
      shown = set.layout();
      out.withheld = std::max(out.withheld, set.withheld());
      for (u32 l = 1; l < set.level_count(); ++l)
        out.fewest[l] = std::min<u32>(out.fewest[l], set.chunks(l).size());
    }
    for (u32 l = 1; l < set.level_count(); ++l)
      out.last[l] = set.chunks(l).size();
  };
  Flight budgeted;
  Flight free;
  fly(true, budgeted);
  fly(false, free);
  MESSAGE("600 m at 100 m/s, the outer level's tiles, first fill / fewest / last: budgeted "
          << budgeted.first[1] << " / " << budgeted.fewest[1] << " / " << budgeted.last[1]
          << ", unbudgeted " << free.first[1] << " / " << free.fewest[1] << " / " << free.last[1]
          << "; the finest level's " << free.first[4] << " / " << free.fewest[4]);
  for (u32 l = 1; l < 5; ++l) {
    CHECK(budgeted.fewest[l] > 0);
    CHECK(free.fewest[l] > 0);
  }
  // Unbudgeted, the two outer levels (thousands of tiles) never hold fewer than their first fill
  // but for what a boundary crosses in a frame; the inner two are a handful, and step by whole
  // tiles as the camera crosses one.
  for (u32 l = 1; l < 3; ++l)
    CHECK(free.fewest[l] * 100 >= free.first[l] * 97);
  CHECK(free.withheld == 0);
  // The budget's deficit shows: the outer level ends well short of the unbudgeted ring's. (Both
  // end above their first fill: a moving ring also holds the band its tiles leave by, past the
  // radius behind the camera, which a first fill from standing does not.)
  CHECK(budgeted.last[1] * 100 < free.last[1] * 97);
}

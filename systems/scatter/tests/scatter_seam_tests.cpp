// The exclusion by earlier placements and the moving ground (scatter.md, "Exclusion by earlier
// placements" and "A moving ground"; ADR-0046 decision 4, as built 2026-10-12): the placer handed
// the footprints of an earlier entry keeps every kind its `clear` beyond them, the same bytes on
// any number of threads and, handed none, the bytes it gave before the seam; the world's
// placements consumer hands a scatter entry the footprints of a synthetic earlier generator of the
// same tile; and a placement standing on a surface that moved keeps its place, turn and scale and
// stands on the new surface — through the placer, and through the consumer's `restand` on the
// dunes.
#include <core/jobs/job_system.h>
#include <core/math/math.h>
#include <core/schema/json_reflect.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>
#include <systems/scatter/scatter.h>
#include <systems/world/placement_tiles.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstdio>
#include <string>

using namespace engine;
using namespace engine::scatter;

namespace {

f64 slope(const void*, i64 x_mm, i64 z_mm) noexcept {
  return 0.02 * static_cast<f64>(x_mm) / 1000.0 - 0.01 * static_cast<f64>(z_mm) / 1000.0;
}
// The same ground "a while later": raised 0.75 m and tilted the other way in x.
f64 slope_later(const void*, i64 x_mm, i64 z_mm) noexcept {
  return 0.75 - 0.03 * static_cast<f64>(x_mm) / 1000.0 - 0.01 * static_cast<f64>(z_mm) / 1000.0;
}

scene_gen::Ground ground_of(scene_gen::HeightFn fn) {
  scene_gen::Ground g;
  g.surface_fn = fn;
  return g;
}

Layer layer() {
  Layer out;
  out.name = "seam";
  out.seed = 77;
  out.tile_size = 16.0f;
  out.region.kind = RegionKind::Tiles;
  out.region.tile_min = {0, 0};
  out.region.tile_max = {7, 7};
  out.density.per_m2 = 0.2;
  Kind rock;
  rock.name = "rock";
  rock.rock = Rock{};
  rock.spacing = 1.5f;
  rock.clear = 0.5f;
  rock.align = true;
  rock.sink = 0.1f;
  rock.scale_min = 0.8f;
  rock.scale_max = 1.2f;
  out.kinds.push_back(rock);
  return out;
}

// Thirty earlier footprints of 2 m over the layer's 128 m square, at hashed places.
Vector<scene_gen::Footprint> walls() {
  Vector<scene_gen::Footprint> out;
  u64 h = 0x5eed;
  for (u32 i = 0; i < 30; ++i) {
    h = h * 6364136223846793005ull + 1442695040888963407ull;
    out.push_back(scene_gen::Footprint{static_cast<i64>((h >> 20) % 128000),
                                       static_cast<i64>((h >> 40) % 128000), 2.0});
  }
  return out;
}

Vector<Point> place_all(const Placer& placer, const Site& site) {
  Vector<Point> out;
  for (i32 z = 0; z < 8; ++z) {
    for (i32 x = 0; x < 8; ++x)
      placer.place_tile(scene_gen::TileCoord{x, z}, site, ~0u, out);
  }
  return out;
}

struct Pool {
  jobs::JobSystem system{jobs::JobSystemConfig{.performance_workers = 3, .pin_threads = false}};
  static jobs::JobSystem* get(void* self) { return &static_cast<Pool*>(self)->system; }
};

}  // namespace

TEST_CASE("scatter seam: earlier placements keep a layer clear of them, on any thread count") {
  Placer placer;
  std::string error;
  REQUIRE_MESSAGE(placer.open(layer(), "", 1, &error), error);
  const Vector<scene_gen::Footprint> earlier = walls();
  Site bare;
  bare.ground = ground_of(&slope);
  Site seen = bare;
  seen.earlier = std::span<const scene_gen::Footprint>(earlier.data(), earlier.size());
  const Vector<Point> alone = place_all(placer, bare);
  const Vector<Point> kept = place_all(placer, seen);
  // Nothing within a footprint's radius plus the kind's clear, measured in whole millimetres.
  u32 inside = 0;
  for (const Point& p : kept) {
    for (const scene_gen::Footprint& f : earlier) {
      const f64 dx = static_cast<f64>(p.x_mm - f.x_mm);
      const f64 dz = static_cast<f64>(p.z_mm - f.z_mm);
      inside += dx * dx + dz * dz < 2500.0 * 2500.0 ? 1u : 0u;
    }
  }
  MESSAGE(alone.size() << " placements alone, " << kept.size() << " beside the earlier entry's");
  CHECK(inside == 0);
  CHECK(kept.size() < alone.size());
  CHECK(kept.size() > alone.size() / 2);
  // Every placement kept is one the layer makes alone: the footprints only take some away.
  u32 found = 0;
  for (const Point& p : kept) {
    for (const Point& q : alone)
      found += p.x_mm == q.x_mm && p.z_mm == q.z_mm && p.y == q.y ? 1u : 0u;
  }
  CHECK(found == kept.size());
  // Handed none, the bytes are the ones the layer gave before the seam (the ground's overload).
  Vector<Point> before;
  for (i32 z = 0; z < 8; ++z) {
    for (i32 x = 0; x < 8; ++x)
      placer.place_tile(scene_gen::TileCoord{x, z}, ground_of(&slope), ~0u, before);
  }
  CHECK(hash_points(std::span<const Point>(alone.data(), alone.size())) ==
        hash_points(std::span<const Point>(before.data(), before.size())));

  // Through the registry's `expand`, the whole read: 64 tiles go to the job system's pool when it
  // is offered, and the bytes are the one thread's.
  const scene_gen::PlacementGeneratorDesc* scatter =
      scene_gen::GeneratorRegistry::global().find_placement(k_placement_generator);
  REQUIRE(scatter != nullptr);
  const JsonValue params = schema::to_json(layer());
  scene_gen::Context context;
  context.world_seed = 1;
  context.ground = ground_of(&slope);
  context.earlier = std::span<const scene_gen::Footprint>(earlier.data(), earlier.size());
  void* state = nullptr;
  REQUIRE_MESSAGE(scatter->open(params, context, &state, &error), error);
  scene_gen::Placements one;
  REQUIRE(scatter->expand(state, context, one, &error));
  Pool pool;
  context.jobs_fn = &Pool::get;
  context.jobs_context = &pool;
  scene_gen::Placements many;
  REQUIRE(scatter->expand(state, context, many, &error));
  scatter->close(state);
  REQUIRE(one.instances.size() == kept.size());
  REQUIRE(many.instances.size() == one.instances.size());
  u32 differ = 0;
  for (u32 i = 0; i < one.instances.size(); ++i) {
    const scene_gen::Placement& a = one.instances[i];
    const scene_gen::Placement& b = many.instances[i];
    differ += a.position.x != b.position.x || a.position.y != b.position.y ||
                      a.position.z != b.position.z || a.rotation.x != b.rotation.x ||
                      a.rotation.w != b.rotation.w || a.scale.x != b.scale.x
                  ? 1u
                  : 0u;
  }
  CHECK(differ == 0);
}

TEST_CASE("scatter seam: a placement on a moved surface keeps its place, turn and scale") {
  Placer placer;
  std::string error;
  REQUIRE_MESSAGE(placer.open(layer(), "", 1, &error), error);
  Site then;
  then.ground = ground_of(&slope);
  Site now = then;
  now.stand = ground_of(&slope_later);
  const Vector<Point> a = place_all(placer, then);
  const Vector<Point> b = place_all(placer, now);
  REQUIRE(a.size() == b.size());
  REQUIRE(!a.empty());
  u32 moved = 0;
  u32 tilted = 0;
  for (u32 i = 0; i < a.size(); ++i) {
    CHECK(a[i].x_mm == b[i].x_mm);
    CHECK(a[i].z_mm == b[i].z_mm);
    CHECK(a[i].yaw == b[i].yaw);
    CHECK(a[i].scale == b[i].scale);
    const f64 want =
        now.stand.surface_mm(b[i].x_mm, b[i].z_mm) - 0.1 * static_cast<f64>(b[i].scale);
    CHECK(std::fabs(b[i].y - want) < 1e-6);
    moved += b[i].y != a[i].y ? 1u : 0u;
    tilted += b[i].rotation.x != a[i].rotation.x || b[i].rotation.z != a[i].rotation.z ? 1u : 0u;
  }
  CHECK(moved == a.size());
  CHECK(tilted == a.size());  // aligned to the new normal
}

#if defined(ENGINE_SCATTER_TESTS_DUNES)
TEST_CASE("scatter seam: the world's consumer hands the earlier entry's tile and re-stands it") {
  // A small erg of dunes with a world block and two streamed entries: the procedural rock layer
  // twice — the first stands in for an earlier generator (a ruin's blocks), the second keeps
  // `clear` from its placements — read by the renderer's reader and loaded, then driven through
  // `world::PlacementTiles` tile by tile.
  test::TempDir tmp{"engine_scatter_seam"};
  const std::string earlier_layer =
      R"({"name":"big","seed":5,"tile_size":16,"region":{"kind":"Scene"},"density":{"per_m2":0.02},)"
      R"("meshes":"rocks","kinds":[{"name":"boulder","spacing":5,"scale_min":1.5,"scale_max":2,)"
      R"("rock":{"seed":3,"size_x":1.2,"size_y":0.8,"size_z":1.0,"detail":1}}]})";
  const std::string later_layer =
      R"({"name":"small","seed":9,"tile_size":16,"region":{"kind":"Scene"},"density":{"per_m2":0.3},)"
      R"("meshes":"rocks","kinds":[{"name":"pebble","spacing":1,"clear":0.25,"align":true,)"
      R"("rock":{"seed":4,"size_x":0.3,"size_y":0.2,"size_z":0.3,"detail":1}}]})";
  const std::string text =
      R"({"format":"engine.scene.v1","name":"seam",)"
      R"("terrain":{"size":33,"extent":64,"seed":5,"dune_height":2.0,"dune_wavelength":20,)"
      R"("generator":"Dunes","time":94608000.0},)"
      R"("world":{"tile_size":16,"rings":[{"radius":2},{"radius":4}]},)"
      R"("placements":[{"generator":"scatter","params":)" +
      earlier_layer + R"(},{"generator":"scatter","params":)" + later_layer + "}]}";
  const std::string path = (tmp.native() / "scene.json").generic_string();
  REQUIRE(io::write_file(path, text) == io::Status::Ok);
  renderer::SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(renderer::read_scene_file(path, desc, error), error);
  desc.ddc = (tmp.native() / "ddc").generic_string();
  renderer::SceneData data;
  REQUIRE_MESSAGE(renderer::load_scene(desc, data, error), error);
  REQUIRE(data.streamed.size() == 2);
  world::PlacementTilesConfig config;
  config.tile_size = 16.0f;
  config.ring_count = 2;
  world::PlacementTiles tiles;
  REQUIRE_MESSAGE(tiles.create(data, config, &error), error);
  const world::TileConsumer consumer = tiles.consumer();
  for (i32 z = -2; z < 2; ++z) {
    for (i32 x = -2; x < 2; ++x) {
      world::TileEvent event;
      event.tile = world::TileCoord{x, z};
      event.to = 0;
      REQUIRE(consumer.activate(consumer.context, event));
    }
  }
  consumer.commit(consumer.context);
  const u32 big = data.streamed[0].meshes[0];
  const u32 small = data.streamed[1].meshes[0];
  const f32 big_reach = renderer::ground_radius(data.lod, data.parts[big].first_cluster,
                                                data.parts[big].cluster_count);
  u32 bigs = 0;
  u32 smalls = 0;
  u32 inside = 0;
  for (i32 z = -2; z < 2; ++z) {
    for (i32 x = -2; x < 2; ++x) {
      const auto held = tiles.tile_instances(world::TileCoord{x, z});
      for (const renderer::SceneInstance& a : held) {
        if (a.mesh != big) continue;
        ++bigs;
        const f64 r = static_cast<f64>(big_reach * a.transform.scale.x) + 0.25;
        for (const renderer::SceneInstance& b : held) {
          if (b.mesh != small) continue;
          const f64 dx = b.origin.x - a.origin.x;
          const f64 dz = b.origin.z - a.origin.z;
          inside += dx * dx + dz * dz < (r - 0.002) * (r - 0.002) ? 1u : 0u;
        }
      }
      for (const renderer::SceneInstance& b : held)
        smalls += b.mesh == small ? 1u : 0u;
    }
  }
  MESSAGE(bigs << " boulders and " << smalls << " pebbles on 16 tiles");
  CHECK(bigs > 0);
  CHECK(smalls > 0);
  CHECK(inside == 0);

  // **The moving ground**: the sand a day later; every held tile is stood on it again.
  Vector<renderer::SceneInstance> before;
  for (const renderer::SceneInstance& i : tiles.tail())
    before.push_back(i);
  const f64 later = data.terrain.time_s + 86400.0 * 30.0;
  CHECK(tiles.restand(later, 1000) == 16);
  CHECK(tiles.restand(later, 1000) == 0);  // nothing left standing on another time
  REQUIRE(tiles.tail().size() == before.size());
  renderer::TerrainDesc at = data.terrain;
  at.time_s = later;
  const renderer::TerrainSampler sand(at);
  const scene_gen::Ground surface = sand.provider().view();
  u32 rose = 0;
  for (u32 i = 0; i < before.size(); ++i) {
    const renderer::SceneInstance& a = before[i];
    const renderer::SceneInstance& b = tiles.tail()[i];
    CHECK(a.origin.x == b.origin.x);
    CHECK(a.origin.z == b.origin.z);
    CHECK(a.transform.scale.x == b.transform.scale.x);
    CHECK(std::fabs(b.origin.y - surface.surface(b.origin)) < 1e-9);  // no sink in either layer
    rose += a.origin.y != b.origin.y ? 1u : 0u;
  }
  MESSAGE(rose << " of " << before.size() << " placements moved with the sand; "
               << static_cast<f64>(tiles.stats().restand_ns) / 1.0e6 / 16.0
               << " ms a tile stood again");
  CHECK(rose > before.size() / 2);
}
#endif  // ENGINE_SCATTER_TESTS_DUNES

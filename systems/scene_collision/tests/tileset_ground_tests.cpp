// **A walker stands on an authored ground** (docs/subsystems/tileset.md; scene_collision.md): the
// `tiles` ground provider made through the registry from a built tile set, its set's own source
// handed to the collision as the renderer's is, the ring's heightfields that source's heights on
// its lattice, and a character walked across a tile's edge and down into a bowl with its feet on
// the set's surface. The tile-set capability reaches the collision through the registry, so this
// test links it as a host does (CMakeLists.txt).
#include <domain/physics/character.h>
#include <domain/physics/physics.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/tileset.h>
#include <systems/renderer/scene.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <schemas/scene.h>
#include <string>

using namespace engine;
using namespace engine::scene_collision;

namespace {

world::RingParams one_ring() {
  world::RingParams p;
  p.tile_size = 32.0f;
  p.ring_count = 1;
  for (f32& r : p.radius)
    r = 0.0f;
  p.radius[0] = 1.5f;
  return p;
}

physics::WorldOptions physics_options() {
  physics::WorldOptions o;
  o.max_bodies = 1024;
  o.max_body_pairs = 4096;
  o.max_contact_constraints = 4096;
  return o;
}

}  // namespace

TEST_CASE("scene_collision: a walker stands on a tiles ground, the set's surface") {
  test::TempDir tmp{"engine_scene_collision_tileset"};
  // A bowl 129 points a side at 50 cm — 64 m, its corner 32 m west and north of the origin — a
  // quarter as deep: 5 m down at its centre from a 1.25 m rim, with a few centimetres of roughness.
  const tileset::Heightfield field =
      tileset::synth_heightfield(tileset::SynthKind::basin, 129, 500, 5, -32000, -32000);
  std::string error;
  REQUIRE_MESSAGE(tileset::write_heightfield(tmp.file("bowl.json"), field, error), error);
  tileset::BuildOptions options;
  options.cells = 32;
  options.vertical_scale = 0.25;
  options.far_levels = 2;
  tileset::BuildResult built;
  REQUIRE_MESSAGE(tileset::ensure_tileset(tmp.file("bowl.json"), options, tmp.file("set"), "",
                                          false, built, error),
                  error);
  const scene_gen::GroundProviderDesc* desc =
      scene_gen::GeneratorRegistry::global().find_ground("tiles");
  REQUIRE(desc != nullptr);
  scene::Terrain entry;
  entry.provider = "tiles";
  entry.tiles = scene::TerrainTileSet{};
  entry.tiles->manifest = built.manifest;
  scene_gen::GroundProvider ground;
  REQUIRE_MESSAGE(desc->make(entry, scene_gen::Context{}, ground, &error), error);
  const tileset::TileSet* set = tileset::tile_set_of(ground);
  REQUIRE(set != nullptr);
  const scene_gen::TileSource source = set->source();

  renderer::SceneData scene;
  physics::World physics;
  REQUIRE(physics.init(physics_options()) == physics::Status::Ok);
  SceneCollision collision;
  REQUIRE_MESSAGE(collision.create(physics, scene, &ground, Config{}, &error, &source), error);
  world::World ring(one_ring());
  ring.add_consumer(collision.consumer());
  sim::ObserverSet observer;
  observer.add(WorldPos{16.0, 0.0, 16.0}, 1.0f);
  ring.update(observer, 0, true);
  REQUIRE(collision.stats().tiles == 9);
  // On the collision's lattice (whole metres: every other point of the set's) the ground is the
  // set's to the bit; between them its heightfield's own interpolation of those points, never
  // outside the four round it.
  u32 off = 0;
  f32 worst = 0.0f;
  for (i32 j = -30; j <= 30; j += 3) {
    for (i32 i = -30; i <= 30; i += 3) {
      f32 held = 0.0f;
      REQUIRE(
          collision.ground_height(WorldPos{static_cast<f64>(i), 0.0, static_cast<f64>(j)}, held));
      off += held == set->height_mm(i64{i} * 1000, i64{j} * 1000) ? 0u : 1u;
      REQUIRE(collision.ground_height(
          WorldPos{static_cast<f64>(i) + 0.375, 0.0, static_cast<f64>(j) + 0.625}, held));
      const f32 c[4] = {set->height_mm(i64{i} * 1000, i64{j} * 1000),
                        set->height_mm(i64{i} * 1000 + 1000, i64{j} * 1000),
                        set->height_mm(i64{i} * 1000, i64{j} * 1000 + 1000),
                        set->height_mm(i64{i} * 1000 + 1000, i64{j} * 1000 + 1000)};
      const f32 lo = std::min(std::min(c[0], c[1]), std::min(c[2], c[3]));
      const f32 hi = std::max(std::max(c[0], c[1]), std::max(c[2], c[3]));
      worst = std::max(worst, std::max(lo - held, held - hi));
    }
  }
  CHECK(off == 0);
  CHECK(worst <= 0.001f);

  // A character from the rim down into the bowl and east across the tiles' edge at x = 0, its feet
  // on the surface the whole way.
  physics::CharacterBody body;
  physics::CharacterConfig c;
  c.step_hz = 240;
  const WorldPos start{-11.0, 0.0, 6.0};
  const f32 y0 = set->height_mm(-11000, 6000);
  REQUIRE(body.create(physics, c, WorldPos{start.x, static_cast<f64>(y0) + 0.02, start.z}) ==
          physics::Status::Ok);
  physics::CharacterInput east;
  east.move = Vec3{1.0f, 0.0f, 0.0f};
  f32 farthest = 0.0f;
  for (u32 k = 0; k < 2400; ++k) {
    REQUIRE(body.step(east) == physics::Status::Ok);
    const DVec3 feet = body.feet() - WorldPos::origin();
    if (k > 240) {
      const f32 surface =
          set->height_mm(scene_gen::nearest_mm(feet.x), scene_gen::nearest_mm(feet.z));
      farthest = std::max(farthest, std::fabs(static_cast<f32>(feet.y) - surface));
    }
  }
  const DVec3 end = body.feet() - WorldPos::origin();
  MESSAGE("walked to x " << end.x << ", feet within " << farthest * 100.0f
                         << " cm of the set's surface");
  CHECK(end.x > 1.0);  // across the bowl's floor and the tiles' edge at x = 0
  CHECK(farthest < 0.10f);
}

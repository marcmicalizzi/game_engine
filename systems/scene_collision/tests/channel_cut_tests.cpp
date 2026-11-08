// **A walker stands in a channel cut into the ground** (docs/subsystems/tileset.md, "Channels cut
// into the ground"; scene_collision.md; roadmap W89): a `tiles` ground built from a heightfield
// with a cut, made through the registry as a scene's is, its set's source handed to the collision.
// The collision reads the set's heights, so its ground is the cut ground with no change of its
// own: the ground query inside the cut answers the bed, the floodplain past the bank its own
// height, and a character walked across the channel goes down to the bed and up the far bank.
#include <domain/physics/character.h>
#include <domain/physics/physics.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/tileset.h>
#include <systems/renderer/scene.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
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

TEST_CASE("scene_collision: the ground inside a channel's cut is its bed") {
  test::TempDir tmp{"engine_scene_collision_cut"};
  // A flat 64 m square at 50 cm, its corner 32 m west and north of the origin, 2 m up.
  tileset::Heightfield field =
      tileset::synth_heightfield(tileset::SynthKind::flat, 129, 500, 0, -32000, -32000);
  for (f32& h : field.data)
    h = 2.0f;
  std::string error;
  REQUIRE_MESSAGE(tileset::write_heightfield(tmp.file("flat.json"), field, error), error);
  // A channel 4 m wide and 0.8 m deep along z = 0, its top on the ground, its banks at a rise of
  // 0.5 (27 degrees, under the walker's 40) so a character can climb out: a flat bed 0.8 m wide.
  scene::TerrainCut cut;
  cut.name = "run";
  cut.points = {WorldPos{-30.0, 2.0, 0.0}, WorldPos{30.0, 2.0, 0.0}};
  cut.width_m = {4.0f, 4.0f};
  cut.depth_m = {0.8f, 0.8f};
  cut.bank_slope = 0.5f;
  scene::Terrain entry;
  entry.provider = "tiles";
  entry.tiles = scene::TerrainTileSet{};
  entry.tiles->heightfield = "flat.json";
  entry.tiles->ddc = "ddc";
  entry.tiles->cells = 32;
  entry.tiles->far_levels = 2;
  entry.tiles->cuts.push_back(cut);
  scene_gen::Context context;
  const std::string dir = tmp.path();
  context.dir = dir;
  const scene_gen::GroundProviderDesc* desc =
      scene_gen::GeneratorRegistry::global().find_ground("tiles");
  REQUIRE(desc != nullptr);
  scene_gen::GroundProvider ground;
  REQUIRE_MESSAGE(desc->make(entry, context, ground, &error), error);
  const tileset::TileSet* set = tileset::tile_set_of(ground);
  REQUIRE(set != nullptr);

  renderer::SceneData scene;
  physics::World physics;
  REQUIRE(physics.init(physics_options()) == physics::Status::Ok);
  SceneCollision collision;
  REQUIRE_MESSAGE(collision.create(physics, scene, &ground, Config{}, &error), error);
  world::World ring(one_ring());
  ring.add_consumer(collision.consumer());
  sim::ObserverSet observer;
  observer.add(WorldPos{16.0, 0.0, 16.0}, 1.0f);
  ring.update(observer, 0, true);
  REQUIRE(collision.stats().tiles == 9);
  // On the collision's lattice (whole metres): the bed under the line, the banks' faces a metre
  // either side (0.5 m below the top), the top of the bank and the floodplain.
  for (const f64 x : {-20.0, -3.0, 0.0, 7.0, 20.0}) {
    f32 h = 0.0f;
    REQUIRE(collision.ground_height(WorldPos{x, 0.0, 0.0}, h));
    CHECK(h == doctest::Approx(1.2f).epsilon(1e-6));
    REQUIRE(collision.ground_height(WorldPos{x, 0.0, 1.0}, h));
    CHECK(h == doctest::Approx(1.5f).epsilon(1e-6));
    REQUIRE(collision.ground_height(WorldPos{x, 0.0, -1.0}, h));
    CHECK(h == doctest::Approx(1.5f).epsilon(1e-6));
    REQUIRE(collision.ground_height(WorldPos{x, 0.0, 2.0}, h));
    CHECK(h == doctest::Approx(2.0f).epsilon(1e-6));
    REQUIRE(collision.ground_height(WorldPos{x, 0.0, 6.0}, h));
    CHECK(h == doctest::Approx(2.0f).epsilon(1e-6));
    // The collision's ground is the set's, to the bit, on its lattice.
    REQUIRE(collision.ground_height(WorldPos{x, 0.0, 1.0}, h));
    CHECK(h == set->height_mm(scene_gen::nearest_mm(x), 1000));
  }

  // A character walked across the channel from the north: down the bank to the bed, and up the far
  // bank onto the floodplain, its feet on the cut ground the whole way.
  physics::CharacterBody body;
  physics::CharacterConfig c;
  c.step_hz = 240;
  REQUIRE(body.create(physics, c, WorldPos{4.0, 2.02, -6.0}) == physics::Status::Ok);
  physics::CharacterInput south;
  south.move = Vec3{0.0f, 0.0f, 1.0f};
  f64 lowest = 10.0;
  for (u32 k = 0; k < 2400; ++k) {  // 10 s
    REQUIRE(body.step(south) == physics::Status::Ok);
    const DVec3 feet = body.feet() - WorldPos::origin();
    if (std::fabs(feet.z) < 1.0) lowest = std::min(lowest, feet.y);
  }
  const DVec3 end = body.feet() - WorldPos::origin();
  MESSAGE("walked to z " << end.z << ", lowest in the bed " << lowest);
  CHECK(end.z > 3.0);
  CHECK(lowest == doctest::Approx(1.2).epsilon(0.03));
  CHECK(end.y == doctest::Approx(2.0).epsilon(0.03));
}

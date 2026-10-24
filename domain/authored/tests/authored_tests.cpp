// The `authored` placement generator (docs/subsystems/authored.md): a scene's own instances filed
// by the tile their origin is on, each tile's in the order of their ids, in the form its ring
// draws.

#include <core/schema/json_reflect.h>
#include <domain/authored/authored.h>
#include <domain/scene_gen/scene_gen.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <schemas/scene.h>
#include <string>

using namespace engine;

namespace {

const scene_gen::PlacementGeneratorDesc& generator() {
  const scene_gen::PlacementGeneratorDesc* g =
      scene_gen::GeneratorRegistry::global().find_placement(authored::k_generator);
  REQUIRE(g != nullptr);
  return *g;
}

// Two form sets — a tree drawn as resident meshes 0, 1, 2 by ring, a rock as 3 at every ring — and
// instances on four tiles of 32 m, two of them on tile (0, 0) listed out of id order.
scene::AuthoredSet fixture() {
  scene::AuthoredSet set;
  set.tile_size = 32.0f;
  scene::AuthoredForms tree;
  tree.rings = {0, 1, 2};
  scene::AuthoredForms rock;
  rock.rings = {3};
  set.forms = {tree, rock};
  const auto add = [&](u32 forms, f64 x, f64 z, u32 id) {
    scene::AuthoredInstance i;
    i.forms = forms;
    i.translation = WorldPos{x, 1.5, z};
    i.rotation = Quat::identity();
    i.scale = Vec3{1.0f, 1.0f, 1.0f};
    i.id = id;
    set.instances.push_back(i);
  };
  add(0, 5.0, 7.0, 4);
  add(1, 40.0, 3.0, 2);
  add(0, 31.9, 0.1, 1);     // tile (0, 0) too, and first by id
  add(0, -0.5, -64.0, 3);   // tile (-1, -2)
  add(1, 1.0e6, 2.0e6, 5);  // far: tile (31250, 62500)
  return set;
}

struct Opened {
  const scene_gen::PlacementGeneratorDesc* g = nullptr;
  void* state = nullptr;
  ~Opened() {
    if (state != nullptr) g->close(state);
  }
};

bool open_set(const scene::AuthoredSet& set, Opened& out, std::string& error, f32 tile = 32.0f) {
  out.g = &generator();
  scene_gen::Context context;
  context.tile_size = tile;
  context.where = "test: authored";
  return out.g->open(schema::to_json(set), context, &out.state, &error);
}

Vector<scene_gen::Placement> tile_of(const Opened& o, scene_gen::TileCoord t, u8 ring) {
  scene_gen::Context context;
  context.tile_size = 32.0f;
  context.ring = ring;
  scene_gen::Placements out;
  std::string error;
  REQUIRE(o.g->tile(o.state, t, context, out, &error));
  return out.instances;
}

}  // namespace

TEST_CASE("authored: an instance is filed under the tile its origin is on") {
  scene_gen::TileCoord t;
  REQUIRE(authored::anchor_tile(WorldPos{-0.5, 0.0, -64.0}, 32.0f, t));
  CHECK(t == scene_gen::TileCoord{-1, -2});
  REQUIRE(authored::anchor_tile(WorldPos{31.999, 0.0, 32.0}, 32.0f, t));
  CHECK(t == scene_gen::TileCoord{0, 1});
  CHECK_FALSE(authored::anchor_tile(WorldPos{std::nan(""), 0.0, 0.0}, 32.0f, t));
  CHECK_FALSE(authored::anchor_tile(WorldPos{1.0e12, 0.0, 0.0}, 32.0f, t));

  Opened o;
  std::string error;
  REQUIRE_MESSAGE(open_set(fixture(), o, error), error);
  CHECK(o.g->occupies(o.state, scene_gen::TileCoord{0, 0}));
  CHECK(o.g->occupies(o.state, scene_gen::TileCoord{1, 0}));
  CHECK(o.g->occupies(o.state, scene_gen::TileCoord{-1, -2}));
  CHECK(o.g->occupies(o.state, scene_gen::TileCoord{31250, 62500}));
  CHECK_FALSE(o.g->occupies(o.state, scene_gen::TileCoord{0, -1}));
  CHECK(tile_of(o, scene_gen::TileCoord{0, -1}, 0).empty());

  const Vector<scene_gen::Placement> home = tile_of(o, scene_gen::TileCoord{0, 0}, 0);
  REQUIRE(home.size() == 2);
  CHECK(home[0].id == 1);  // by id, not by the list's order
  CHECK(home[1].id == 4);
  CHECK(home[0].position.x == 31.9);
  CHECK(home[1].position.z == 7.0);
  CHECK(home[1].position.y == 1.5);
  const Vector<scene_gen::Placement> far = tile_of(o, scene_gen::TileCoord{31250, 62500}, 2);
  REQUIRE(far.size() == 1);
  CHECK(far[0].position.x == 1.0e6);  // in f64, where the file stood it
}

TEST_CASE("authored: each ring draws its form, and a ring past the last draws the last") {
  Opened o;
  std::string error;
  REQUIRE_MESSAGE(open_set(fixture(), o, error), error);
  const scene_gen::TileCoord home{0, 0};
  CHECK(tile_of(o, home, 0)[0].mesh == 0);
  CHECK(tile_of(o, home, 1)[0].mesh == 1);
  CHECK(tile_of(o, home, 2)[0].mesh == 2);
  CHECK(tile_of(o, home, 5)[0].mesh == 2);
  CHECK(tile_of(o, home, scene_gen::k_no_ring)[0].mesh == 0);
  for (const u8 ring : {u8{0}, u8{1}, u8{2}, u8{4}})
    CHECK(tile_of(o, scene_gen::TileCoord{1, 0}, ring)[0].mesh == 3);  // the rock: one mesh
  scene_gen::Context context;
  context.tile_size = 32.0f;
  context.ring = 1;
  const u8 one = o.g->representation(o.state, context);
  context.ring = 3;
  const u8 three = o.g->representation(o.state, context);
  context.ring = 6;
  CHECK(one != three);
  CHECK(o.g->representation(o.state, context) == three);  // past the forms, one representation
  scene_gen::Placements whole;
  REQUIRE(o.g->expand(o.state, context, whole, &error));
  CHECK(whole.instances.size() == 5);
  CHECK(whole.meshes.empty());
}

TEST_CASE("authored: a tile's placements and their ids survive a reload in any order") {
  scene::AuthoredSet set = fixture();
  Opened a;
  std::string error;
  REQUIRE_MESSAGE(open_set(set, a, error), error);
  std::reverse(set.instances.begin(), set.instances.end());
  Opened b;
  REQUIRE_MESSAGE(open_set(set, b, error), error);
  for (const scene_gen::TileCoord t :
       {scene_gen::TileCoord{0, 0}, scene_gen::TileCoord{1, 0}, scene_gen::TileCoord{-1, -2}}) {
    const Vector<scene_gen::Placement> x = tile_of(a, t, 1);
    const Vector<scene_gen::Placement> y = tile_of(b, t, 1);
    REQUIRE(x.size() == y.size());
    for (u32 i = 0; i < x.size(); ++i) {
      CHECK(x[i].id == y[i].id);
      CHECK(x[i].mesh == y[i].mesh);
      CHECK(x[i].position.x == y[i].position.x);
      CHECK(x[i].position.z == y[i].position.z);
    }
  }
}

TEST_CASE("authored: a set the world cannot hold is refused with a sentence") {
  std::string error;
  {
    Opened o;
    CHECK_FALSE(open_set(fixture(), o, error, 24.0f));
    CHECK(error.find("tiles") != std::string::npos);
  }
  {
    scene::AuthoredSet set = fixture();
    set.instances[2].forms = 7;
    Opened o;
    CHECK_FALSE(open_set(set, o, error));
    CHECK(error.find("instance 2 names form set 7") != std::string::npos);
  }
  {
    scene::AuthoredSet set = fixture();
    set.forms[1].rings.clear();
    Opened o;
    CHECK_FALSE(open_set(set, o, error));
    CHECK(error.find("form set 1 has no ring") != std::string::npos);
  }
  {
    scene::AuthoredSet set = fixture();
    set.instances[0].translation.x = 1.0e11;  // a cell reaches it, an i32 tile of 32 m does not
    Opened o;
    CHECK_FALSE(open_set(set, o, error));
    CHECK_MESSAGE(error.find("no tile reaches") != std::string::npos, error);
  }
}

// The scene-generator registry (docs/subsystems/scene_gen.md; ADR-0046): a generator registered
// from a source of its own is found by the name a scene gives, a name the executable does not carry
// is refused with a sentence that names it and what the executable has, and a second generator
// under one name is refused — the same descriptor twice is not a second one. And the handles: a
// ground with no grid of its own is sampled a point at a time, the same heights as its point
// function, and the view a placement generator stands on is that function.
#include <domain/scene_gen/scene_gen.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;
using namespace engine::scene_gen;

namespace {

// A ground of this test's own: a plane tilted along x, with a floor a metre under it.
struct Tilt {
  f32 slope = 0.0f;
};

f32 tilt_height(const void* state, f32 x, f32) noexcept {
  return static_cast<const Tilt*>(state)->slope * x;
}
f32 tilt_floor(const void* state, f32 x, f32 z) noexcept { return tilt_height(state, x, z) - 1.0f; }
void tilt_destroy(void* state) noexcept { delete static_cast<Tilt*>(state); }

constexpr GroundOps k_tilt_ops{
    .destroy = &tilt_destroy, .height = &tilt_height, .floor = &tilt_floor};

bool tilt_make(const scene::Terrain& entry, const Context&, GroundProvider& out, std::string*) {
  out = GroundProvider(&k_tilt_ops, new Tilt{entry.dune_height});
  return true;
}

constexpr GroundProviderDesc k_tilt{.name = "scene-gen-test-tilt", .make = &tilt_make};
const Registrar k_tilt_registrar{k_tilt};

// A placement generator of this test's own: one instance of one mesh at the origin.
bool one_open(const JsonValue&, const Context&, void** state, std::string*) {
  *state = nullptr;
  return true;
}
void one_close(void*) noexcept {}
bool one_expand(void*, const Context& context, Placements& out, std::string*) {
  out.meshes.push_back(PlacementMesh{"one.glb", "one", 0});
  Placement p;
  p.transform.position = Vec3{0.0f, context.ground.floor(0.0f, 0.0f), 0.0f};
  out.instances.push_back(p);
  out.things = 1;
  return true;
}

constexpr PlacementGeneratorDesc k_one{
    .name = "scene-gen-test-one", .open = &one_open, .close = &one_close, .expand = &one_expand};
const Registrar k_one_registrar{k_one};

}  // namespace

TEST_CASE("scene_gen: a generator registered from its own source is found by its name") {
  const GeneratorRegistry& registry = GeneratorRegistry::global();
  CHECK(registry.find_ground("scene-gen-test-tilt") == &k_tilt);
  CHECK(registry.find_placement("scene-gen-test-one") == &k_one);
  // The two kinds are separate tables: a ground's name is not a placement's.
  CHECK(registry.find_placement("scene-gen-test-tilt") == nullptr);
  CHECK(registry.find_ground("scene-gen-test-one") == nullptr);
  bool listed = false;
  for (const std::string_view name : registry.ground_names())
    listed = listed || name == "scene-gen-test-tilt";
  CHECK(listed);

  // Made through the registry, the provider is the test's ground, and its view stands a placement
  // on the floor.
  scene::Terrain entry;
  entry.dune_height = 0.5f;
  GroundProvider ground;
  std::string error;
  REQUIRE(registry.find_ground("scene-gen-test-tilt")->make(entry, Context{}, ground, &error));
  CHECK(ground.height(4.0f, 9.0f) == 2.0f);
  CHECK(ground.floor(4.0f, 9.0f) == 1.0f);
  CHECK_FALSE(ground.moves());
  CHECK_FALSE(ground.has_rings());
  CHECK_FALSE(ground.has_tiles());
  Context context;
  context.ground = ground.view();
  Placements placed;
  void* state = nullptr;
  const PlacementGeneratorDesc& one = *registry.find_placement("scene-gen-test-one");
  REQUIRE(one.open(JsonValue::object(), context, &state, &error));
  REQUIRE(one.expand(state, context, placed, &error));
  one.close(state);
  REQUIRE(placed.instances.size() == 1);
  CHECK(placed.instances[0].transform.position.y == -1.0f);
}

TEST_CASE("scene_gen: a name the executable does not carry is refused with a sentence") {
  const GeneratorRegistry& registry = GeneratorRegistry::global();
  CHECK(registry.find_ground("no-such-ground") == nullptr);
  CHECK(registry.find_placement("no-such-generator") == nullptr);
  const std::string ground = registry.unknown_ground("no-such-ground");
  CHECK(ground ==
        "names the ground provider \"no-such-ground\", which this build does not have: its "
        "capability is switched off or not linked into this executable (it has " +
            [&] {
              std::string list;
              for (const std::string_view name : registry.ground_names())
                list += (list.empty() ? "" : ", ") + std::string(name);
              return list;
            }() +
            ")");
  const std::string placement = registry.unknown_placement("no-such-generator");
  CHECK(placement.find("\"no-such-generator\"") != std::string::npos);
  CHECK(placement.find("scene-gen-test-one") != std::string::npos);
  // An empty registry says it has none.
  const GeneratorRegistry empty;
  CHECK(empty.unknown_placement("ruins") ==
        "names the placement generator \"ruins\", which this build does not have: its capability "
        "is switched off or not linked into this executable (it has none)");
}

TEST_CASE("scene_gen: two registrations of one name are refused, and one descriptor twice is not") {
  GeneratorRegistry registry;
  constexpr GroundProviderDesc a{.name = "twice", .make = &tilt_make};
  constexpr GroundProviderDesc b{.name = "twice", .make = &tilt_make};
  CHECK(registry.add(a));
  CHECK(registry.add(a));  // the same descriptor: a library linked into two images
  CHECK_FALSE(registry.add(b));
  CHECK(registry.find_ground("twice") == &a);
  CHECK(registry.ground_names().size() == 1);

  constexpr PlacementGeneratorDesc p{
      .name = "twice", .open = &one_open, .close = &one_close, .expand = &one_expand};
  constexpr PlacementGeneratorDesc q{
      .name = "twice", .open = &one_open, .close = &one_close, .expand = &one_expand};
  CHECK(registry.add(p));  // the kinds are separate tables: a placement may share a ground's name
  CHECK_FALSE(registry.add(q));
  CHECK(registry.find_placement("twice") == &p);

  // An incomplete descriptor is refused too: a name and the entry points every caller uses.
  constexpr GroundProviderDesc nameless{.name = nullptr, .make = &tilt_make};
  constexpr GroundProviderDesc unmade{.name = "unmade", .make = nullptr};
  constexpr PlacementGeneratorDesc unexpanded{
      .name = "unexpanded", .open = &one_open, .close = &one_close, .expand = nullptr};
  CHECK_FALSE(registry.add(nameless));
  CHECK_FALSE(registry.add(unmade));
  CHECK_FALSE(registry.add(unexpanded));
  CHECK(registry.find_ground("unmade") == nullptr);
}

TEST_CASE("scene_gen: a ground with no grid of its own is its point function on the lattice") {
  scene::Terrain entry;
  entry.dune_height = 0.25f;
  GroundProvider ground;
  REQUIRE(k_tilt.make(entry, Context{}, ground, nullptr));
  const Lattice scene = scene_lattice(40.0f, 17);
  Vector<f32> heights(17 * 17);
  ground.grid(scene, 0, 0, 17, 17, std::span<f32>(heights.data(), heights.size()));
  u32 bad = 0;
  for (u32 j = 0; j < 17; ++j) {
    for (u32 i = 0; i < 17; ++i)
      bad += heights[j * 17 + i] !=
             ground.height(scene.x(static_cast<i32>(i)), scene.z(static_cast<i32>(j)));
  }
  CHECK(bad == 0);
  // The scene lattice's ends are the grid's corners, and a ring lattice counts millimetres from the
  // world's origin.
  CHECK(scene.x(0) == -40.0f);
  CHECK(scene.x(16) == 40.0f);
  const Lattice ring = ring_lattice(500);
  CHECK(ring.x(-3) == -1.5f);
  CHECK(ring.spacing == 0.5);
  // A ground with no time does not evaluate at another.
  CHECK_FALSE(ground.evaluate(10.0, scene, 0, 0, 17, 17, 0, 1,
                              std::span<f32>(heights.data(), heights.size())));
  CHECK(ground.travel_m(0.0, 1.0e6) == 0.0);
  // Blocks are 64 x 64, rounded up on each side.
  CHECK(window_blocks(64, 64) == 1);
  CHECK(window_blocks(65, 64) == 2);
  CHECK(window_blocks(129, 130) == 9);
  // A provider that is moved from owns nothing, and destroying it is harmless.
  GroundProvider moved = std::move(ground);
  CHECK(moved.valid());
  CHECK_FALSE(ground.valid());
}

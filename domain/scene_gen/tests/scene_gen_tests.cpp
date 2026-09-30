// The scene-generator registry (docs/subsystems/scene_gen.md; ADR-0046): a generator registered
// from a source of its own is found by the name a scene gives, a name the executable does not carry
// is refused with a sentence that names it and what the executable has, and a second generator
// under one name is refused — the same descriptor twice is not a second one. And the handles: a
// ground with no grid of its own is sampled a point at a time, the same heights as its point
// function, and the view a placement generator stands on is that function.
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/sky.h>

#include <doctest/doctest.h>

#include <algorithm>
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

namespace {

// A sky of this test's own (sky.h): the sun overhead whatever the time, Earth's air and two stars.
struct Still {
  Atmosphere air;
  Star stars[2];
};
void still_destroy(void* state) noexcept { delete static_cast<Still*>(state); }
void still_state(const void*, f64 time_s, SkyState& out) noexcept {
  out = SkyState{};
  out.sun = Vec3{0.0f, 1.0f, 0.0f};
  out.hour = time_s / 3600.0;
}
const Atmosphere* still_air(const void* state) noexcept {
  return &static_cast<const Still*>(state)->air;
}
std::span<const Star> still_stars(const void* state) noexcept {
  return std::span<const Star>(static_cast<const Still*>(state)->stars, 2);
}
constexpr SkyOps k_still_ops{.destroy = &still_destroy,
                             .state = &still_state,
                             .atmosphere = &still_air,
                             .stars = &still_stars};
bool still_make(const scene::Sky&, SkyProvider& out, std::string*) {
  out = SkyProvider(&k_still_ops, new Still{});
  return true;
}
constexpr SkyProviderDesc k_still{.name = "scene-gen-test-still", .make = &still_make};
const Registrar k_still_registrar{k_still};

}  // namespace

TEST_CASE("scene_gen: a sky provider is the third kind, found by its name and made from an entry") {
  const GeneratorRegistry& registry = GeneratorRegistry::global();
  CHECK(registry.find_sky("scene-gen-test-still") == &k_still);
  // A table of its own: a sky's name is not a ground's, nor a ground's a sky's.
  CHECK(registry.find_ground("scene-gen-test-still") == nullptr);
  CHECK(registry.find_sky("scene-gen-test-tilt") == nullptr);
  bool listed = false;
  for (const std::string_view name : registry.sky_names())
    listed = listed || name == "scene-gen-test-still";
  CHECK(listed);
  scene::Sky entry;
  entry.provider = "scene-gen-test-still";
  CHECK(sky_provider_name(entry) == "scene-gen-test-still");
  entry.provider.clear();
  CHECK(sky_provider_name(entry) == k_default_sky);  // "earth", the capability's, when empty
  SkyProvider sky;
  std::string error;
  REQUIRE(registry.find_sky("scene-gen-test-still")->make(entry, sky, &error));
  SkyState state;
  sky.state(7200.0, state);
  CHECK(state.hour == 2.0);
  CHECK(sky.stars().size() == 2);
  CHECK(sky.atmosphere().bottom_radius_km == 6360.0f);
  SkyProvider moved = std::move(sky);
  CHECK(moved.valid());
  CHECK_FALSE(sky.valid());
  // Refused by name, with the sentence the other kinds have; one name, one descriptor.
  CHECK(registry.unknown_sky("mars").find("names the sky provider \"mars\"") == 0);
  GeneratorRegistry local;
  constexpr SkyProviderDesc again{.name = "scene-gen-test-still", .make = &still_make};
  constexpr SkyProviderDesc unmade{.name = "unmade", .make = nullptr};
  CHECK(local.add(k_still));
  CHECK(local.add(k_still));
  CHECK_FALSE(local.add(again));
  CHECK_FALSE(local.add(unmade));
  CHECK(local.unknown_sky("earth") ==
        "names the sky provider \"earth\", which this build does not have: its capability is "
        "switched off or not linked into this executable (it has scene-gen-test-still)");
}

namespace {

// A ground of this test's own that moves: a ramp along x that slides a metre a game second.
f32 slide_at(f64 time_s, f32 x) noexcept { return 0.5f * x - static_cast<f32>(0.5 * time_s); }
f32 slide_height(const void*, f32 x, f32) noexcept { return slide_at(0.0, x); }
bool slide_evaluate(const void*, f64 time_s, const Lattice& lattice, i32 i0, i32 j0, u32 nx, u32 nz,
                    u32 begin, u32 end, std::span<f32> out) noexcept {
  if (out.size() != static_cast<usize>(nx) * nz) return false;
  const u32 per_side = (nx + k_height_block - 1) / k_height_block;
  for (u32 b = begin; b < end; ++b) {
    const u32 bx = (b % per_side) * k_height_block;
    const u32 bz = (b / per_side) * k_height_block;
    for (u32 z = bz; z < std::min(nz, bz + k_height_block); ++z) {
      for (u32 x = bx; x < std::min(nx, bx + k_height_block); ++x)
        out[static_cast<usize>(z) * nx + x] = slide_at(time_s, lattice.x(i0 + static_cast<i32>(x)));
    }
  }
  (void)j0;  // a ramp along x: the same along z
  return true;
}
f64 slide_travel(const void*, f64 from_s, f64 to_s) noexcept { return to_s - from_s; }
constexpr GroundOps k_slide_ops{
    .height = &slide_height, .evaluate = &slide_evaluate, .travel_m = &slide_travel};

}  // namespace

TEST_CASE("scene_gen: a ground seen as tiles is its heights on the world's lattice") {
  // A still ground (the tilt): its grid at its own time, whatever time is asked for, and the same
  // bytes whether the window is asked for whole or a block at a time.
  scene::Terrain entry;
  entry.dune_height = 0.25f;
  GroundProvider tilt;
  REQUIRE(k_tilt.make(entry, Context{}, tilt, nullptr));
  const TileSource still = tilt.tiles();
  REQUIRE(still.valid());
  CHECK_FALSE(still.moves());
  CHECK(still.travel_m(0.0, 1.0e6) == 0.0);
  constexpr u32 nx = 130;
  constexpr u32 nz = 70;
  Vector<f32> whole(nx * nz);
  Vector<f32> later(nx * nz);
  Vector<f32> blocks(nx * nz, -1.0f);
  REQUIRE(still.heights(0.0, 250, -65, 12, nx, nz, std::span<f32>(whole.data(), whole.size())));
  REQUIRE(still.heights(1.0e7, 250, -65, 12, nx, nz, std::span<f32>(later.data(), later.size())));
  const u32 count = window_blocks(nx, nz);
  CHECK(count == 6);
  for (u32 b = count; b-- > 0;) {
    REQUIRE(still.heights(0.0, 250, -65, 12, nx, nz, b, b + 1,
                          std::span<f32>(blocks.data(), blocks.size())));
  }
  const Lattice lattice = ring_lattice(250);
  u32 off = 0;
  for (u32 j = 0; j < nz; ++j) {
    for (u32 i = 0; i < nx; ++i) {
      const f32 want =
          tilt.height(lattice.x(-65 + static_cast<i32>(i)), lattice.z(12 + static_cast<i32>(j)));
      off += whole[j * nx + i] != want || later[j * nx + i] != want || blocks[j * nx + i] != want;
    }
  }
  CHECK(off == 0);
  CHECK(whole[0] == 0.25f * -16.25f);  // lattice point -65 at 25 cm is -16.25 m, exactly

  // A moving ground: its `evaluate` at the time asked for, and its travel.
  const GroundProvider slide(&k_slide_ops, nullptr);
  const TileSource moving = slide.tiles();
  REQUIRE(moving.valid());
  CHECK(moving.moves());
  CHECK(moving.travel_m(10.0, 13.5) == 3.5);
  Vector<f32> at(4 * 3);
  REQUIRE(moving.heights(4.0, 1000, 7, -2, 4, 3, std::span<f32>(at.data(), at.size())));
  CHECK(at[0] == slide_at(4.0, 7.0f));
  CHECK(at[11] == slide_at(4.0, 10.0f));
  // The source is the provider's: two views of one provider are one source.
  CHECK(slide.tiles() == moving);
  CHECK_FALSE(GroundProvider{}.tiles().valid());
}

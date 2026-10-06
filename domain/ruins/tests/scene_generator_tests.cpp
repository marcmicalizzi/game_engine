// The ruins as the placement generator "ruins" (scene_generator.h; docs/subsystems/scene_gen.md;
// ADR-0046), through the registry and with no scene reader: the whole entry is the assembler's
// buildings on its chosen tiles, in sections or in blocks; a tile is exactly the whole entry's
// building on that tile, in the representation its ring draws; the meshes a streamed scene holds
// are the kit's then the block kit's; and an entry with no kit, or on another grid than a streamed
// world's, is refused with the sentence the scene reader always used.
#include <core/json/json.h>
#include <domain/ruins/assembler.h>
#include <domain/ruins/blocks.h>
#include <domain/ruins/kit.h>
#include <domain/ruins/scene_generator.h>
#include <domain/ruins/synthetic_kit.h>
#include <domain/scene_gen/scene_gen.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <schemas/scene.h>
#include <string>

using namespace engine;

namespace {

JsonValue entry_json(const std::string& text) {
  JsonValue v;
  REQUIRE(parse_json(text, v).ok);
  return v;
}

// A gentle slope, so a building's pieces stand at heights a flat ground would not give them.
// The scene's ground speaks whole millimetres (scene_gen.md, "Placements far from the origin").
f64 slope(const void*, i64 x_mm, i64 z_mm) noexcept {
  return 0.05 * (static_cast<f64>(x_mm) / 1000.0) - 0.02 * (static_cast<f64>(z_mm) / 1000.0);
}

struct Opened {
  const scene_gen::PlacementGeneratorDesc* generator = nullptr;
  void* state = nullptr;
  ~Opened() {
    if (generator != nullptr && state != nullptr) generator->close(state);
  }
};

}  // namespace

TEST_CASE("ruins generator: a tile is the whole entry's building on it, in its ring's form") {
  const test::TempDir tmp("ruins_scene_generator");
  std::string error;
  REQUIRE_MESSAGE(ruins::write_synthetic_kit(tmp.file("kit"), ruins::SyntheticKitOptions{}, &error),
                  error);
  REQUIRE_MESSAGE(
      ruins::write_synthetic_block_kit(tmp.file("blocks"), ruins::SyntheticBlockOptions{}, &error),
      error);
  const scene_gen::PlacementGeneratorDesc* generator =
      scene_gen::GeneratorRegistry::global().find_placement(ruins::k_placement_generator);
  REQUIRE(generator != nullptr);
  REQUIRE(generator->tile != nullptr);
  REQUIRE(generator->meshes != nullptr);

  scene_gen::Context context;
  context.dir = tmp.path();
  context.where = "test: ruins 0";
  context.ground = scene_gen::Ground{&slope, nullptr, nullptr};
  context.tile_size = 24.0f;
  for (const char* representation : {"Sections", "Blocks"}) {
    CAPTURE(representation);
    const JsonValue params = entry_json(
        std::string(R"({"kit":"kit/kit.json","seed":11,"tile_size":24,"tile_min":[-3,-3],)"
                    R"("tile_max":[2,2],"count":12,"wind_deg":45,"representation":")") +
        representation + R"(","block_kit":"blocks/block-kit.json"})");
    // The whole entry.
    Opened whole{generator, nullptr};
    REQUIRE_MESSAGE(generator->open(params, context, &whole.state, &error), error);
    scene_gen::Placements expanded;
    REQUIRE_MESSAGE(generator->expand(whole.state, context, expanded, &error), error);
    CHECK(expanded.things == 12);
    const bool blocks = std::string(representation) == "Blocks";
    CHECK(expanded.kind == (blocks ? ruins::k_drawn_blocks : ruins::k_drawn_sections));

    // The same entry streamed: every ring draws what the entry is drawn in, so the tiles, in the
    // order the entry chose them, are the whole expansion piece for piece.
    scene::WorldRings world;
    world.tile_size = 24.0f;
    scene::WorldRing ring;
    ring.radius = 100.0f;
    ring.ruins = blocks ? scene::RingRuins::Blocks : scene::RingRuins::Sections;
    world.rings.push_back(ring);
    scene_gen::Context streamed = context;
    streamed.world = &world;
    streamed.ring = 0;
    Opened tiles{generator, nullptr};
    REQUIRE_MESSAGE(generator->open(params, streamed, &tiles.state, &error), error);
    Vector<scene_gen::PlacementMesh> resident;
    REQUIRE(generator->meshes(tiles.state, streamed, resident, &error));
    // The resident meshes are the kit's, then the block kit's; the whole expansion names only the
    // ones it draws, so its mesh m is the resident list's `offset + m`.
    ruins::Kit kit;
    REQUIRE(ruins::read_kit_file(tmp.file("kit/kit.json"), kit, error));
    const u32 offset = blocks ? kit.meshes.size() : 0;
    REQUIRE(resident.size() >= offset + expanded.meshes.size());
    for (u32 m = 0; m < expanded.meshes.size(); ++m) {
      CHECK(resident[offset + m].path == expanded.meshes[m].path);
      CHECK(resident[offset + m].name == expanded.meshes[m].name);
    }
    Vector<ruins::TileCoord> chosen;
    ruins::choose_tiles(11, ruins::TileCoord{-3, -3}, ruins::TileCoord{2, 2}, 12, 0.05f, chosen);
    REQUIRE(chosen.size() == 12);
    u32 at = 0;
    u32 mismatched = 0;
    for (const ruins::TileCoord& t : chosen) {
      CHECK(generator->occupies(tiles.state, scene_gen::TileCoord{t.x, t.z}));
      scene_gen::Placements one;
      REQUIRE(generator->tile(tiles.state, scene_gen::TileCoord{t.x, t.z}, streamed, one, &error));
      CHECK(one.things == 1);
      for (const scene_gen::Placement& p : one.instances) {
        REQUIRE(at < expanded.instances.size());
        const scene_gen::Placement& q = expanded.instances[at++];
        mismatched += p.mesh != offset + q.mesh || !(p.position == q.position) ||
                      !(p.rotation == q.rotation) || !(p.scale == q.scale) || p.tag != q.tag;
      }
    }
    CHECK(at == expanded.instances.size());
    CHECK(mismatched == 0);
    // A tile with no building has nothing on it.
    CHECK_FALSE(generator->occupies(tiles.state, scene_gen::TileCoord{40, 40}));
    scene_gen::Placements none;
    REQUIRE(generator->tile(tiles.state, scene_gen::TileCoord{40, 40}, streamed, none, &error));
    CHECK(none.instances.empty());
  }
}

TEST_CASE("ruins generator: an entry is refused with the reader's sentences") {
  const test::TempDir tmp("ruins_scene_generator_refuse");
  std::string error;
  REQUIRE_MESSAGE(ruins::write_synthetic_kit(tmp.file("kit"), ruins::SyntheticKitOptions{}, &error),
                  error);
  const scene_gen::PlacementGeneratorDesc* generator =
      scene_gen::GeneratorRegistry::global().find_placement("ruins");
  REQUIRE(generator != nullptr);
  scene_gen::Context context;
  context.dir = tmp.path();
  context.where = "scene.json: ruins 2";
  void* state = nullptr;
  CHECK_FALSE(generator->open(entry_json(R"({"count":1})"), context, &state, &error));
  CHECK(error == "scene.json: ruins 2 names no kit");
  CHECK_FALSE(generator->open(entry_json(R"({"kit":"kit/kit.json","count":1,"colour":3})"), context,
                              &state, &error));
  CHECK(error.find("colour") != std::string::npos);
  CHECK_FALSE(
      generator->open(entry_json(R"({"kit":"kit/kit.json","count":1,"representation":"Blocks"})"),
                      context, &state, &error));
  CHECK(error == "scene.json: ruins 2 is drawn in blocks and names no block_kit");
  // A streamed world's tiles are one grid with the entry's.
  scene::WorldRings world;
  context.world = &world;
  context.tile_size = 32.0f;
  CHECK_FALSE(generator->open(entry_json(R"({"kit":"kit/kit.json","count":1,"tile_size":24})"),
                              context, &state, &error));
  CHECK(error.find("must be one grid") != std::string::npos);
}

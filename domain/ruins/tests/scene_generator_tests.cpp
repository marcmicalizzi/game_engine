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

#include <atomic>
#include <bit>
#include <schemas/scene.h>
#include <string>
#include <thread>
#include <vector>

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

// **A tile asked beside others is the tile asked alone** (T169; scene_gen.md, "A tile asked on
// several threads"): the generator says its `tile` is reentrant, because each call assembles in a
// scratch of its own. One entry's tiles — every chosen tile in sections, walls and blocks — asked
// on one thread, and then all of them at once from eight threads on one state, a dozen rounds:
// every placement the same to the bit, in every round. Before T169 the calls shared the entry's
// one assembler and its output, which eight threads at once would have torn.
TEST_CASE("ruins generator: one state's tiles asked on eight threads are the tiles asked alone") {
  const test::TempDir tmp("ruins_scene_generator_threads");
  std::string error;
  REQUIRE_MESSAGE(ruins::write_synthetic_kit(tmp.file("kit"), ruins::SyntheticKitOptions{}, &error),
                  error);
  REQUIRE_MESSAGE(
      ruins::write_synthetic_block_kit(tmp.file("blocks"), ruins::SyntheticBlockOptions{}, &error),
      error);
  const scene_gen::PlacementGeneratorDesc* generator =
      scene_gen::GeneratorRegistry::global().find_placement(ruins::k_placement_generator);
  REQUIRE(generator != nullptr);
  CHECK(generator->tile_reentrant);
  scene::WorldRings world;
  world.tile_size = 24.0f;
  for (const scene::RingRuins r :
       {scene::RingRuins::Blocks, scene::RingRuins::Sections, scene::RingRuins::Walls}) {
    scene::WorldRing ring;
    ring.radius = 100.0f * static_cast<f32>(world.rings.size() + 1);
    ring.ruins = r;
    world.rings.push_back(ring);
  }
  scene_gen::Context context;
  context.dir = tmp.path();
  context.where = "test: ruins 0";
  context.ground = scene_gen::Ground{&slope, nullptr, nullptr};
  context.tile_size = 24.0f;
  context.world = &world;
  const JsonValue params = entry_json(
      R"({"kit":"kit/kit.json","seed":11,"tile_size":24,"tile_min":[-3,-3],"tile_max":[2,2],)"
      R"("count":12,"wind_deg":45,"representation":"Blocks","block_kit":"blocks/block-kit.json"})");
  Opened opened{generator, nullptr};
  REQUIRE_MESSAGE(generator->open(params, context, &opened.state, &error), error);
  Vector<ruins::TileCoord> chosen;
  ruins::choose_tiles(11, ruins::TileCoord{-3, -3}, ruins::TileCoord{2, 2}, 12, 0.05f, chosen);
  REQUIRE(chosen.size() == 12);
  // Every (tile, ring) pair: the tile's building in blocks, in sections and as walls.
  struct Ask {
    scene_gen::TileCoord tile;
    u8 ring = 0;
  };
  Vector<Ask> asks;
  for (const ruins::TileCoord& t : chosen)
    for (u8 r = 0; r < 3; ++r)
      asks.push_back(Ask{scene_gen::TileCoord{t.x, t.z}, r});
  const auto words = [](const scene_gen::Placements& p) {
    std::vector<u64> out;  // the placements' every field, to the bit
    out.push_back(p.kind);
    out.push_back(p.things);
    for (const scene_gen::Placement& q : p.instances) {
      out.push_back(q.mesh);
      out.push_back(q.tag);
      out.push_back(std::bit_cast<u64>(q.position.x));
      out.push_back(std::bit_cast<u64>(q.position.y));
      out.push_back(std::bit_cast<u64>(q.position.z));
      for (const f32 f : {q.rotation.x, q.rotation.y, q.rotation.z, q.rotation.w, q.scale.x,
                          q.scale.y, q.scale.z})
        out.push_back(std::bit_cast<u32>(f));
    }
    return out;
  };
  const auto ask = [&](const Ask& a, std::vector<u64>& out, std::string& why) {
    scene_gen::Context c = context;
    c.ring = a.ring;
    scene_gen::Placements placed;
    if (!generator->tile(opened.state, a.tile, c, placed, &why)) return false;
    out = words(placed);
    return true;
  };
  std::vector<std::vector<u64>> alone(asks.size());
  for (u32 k = 0; k < asks.size(); ++k) {
    REQUIRE_MESSAGE(ask(asks[k], alone[k], error), error);
    CHECK(alone[k].size() > 2);  // a building on every chosen tile, in every form
  }
  constexpr u32 k_threads = 8;
  constexpr u32 k_rounds = 12;
  std::atomic<u32> differ{0};
  std::atomic<u32> failed{0};
  for (u32 round = 0; round < k_rounds; ++round) {
    std::vector<std::thread> threads;
    std::atomic<u32> next{0};
    for (u32 t = 0; t < k_threads; ++t) {
      threads.emplace_back([&] {
        std::string why;
        std::vector<u64> got;
        // Each thread takes the next ask, so the eight run through the list side by side and
        // every tile is asked beside others of the same state.
        for (u32 k = next.fetch_add(1); k < asks.size(); k = next.fetch_add(1)) {
          if (!ask(asks[(k + round) % asks.size()], got, why)) {
            failed.fetch_add(1);
            continue;
          }
          if (got != alone[(k + round) % asks.size()]) differ.fetch_add(1);
        }
      });
    }
    for (std::thread& t : threads)
      t.join();
  }
  MESSAGE(asks.size() << " tiles in three forms, asked on " << k_threads << " threads at once, "
                      << k_rounds << " rounds: " << differ.load() << " differ");
  CHECK(failed.load() == 0);
  CHECK(differ.load() == 0);
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

TEST_CASE("ruins generator hostile: a scatter's square and tile size are bounded before the walk") {
  // T191: every tile of [tile_min, tile_max] is ranked whatever `count` is, and both corners are
  // i32s a client's scene names. One tile past 4,096 a side chooses nothing (it ranked 16.8 million
  // tiles before), and the generator says the bound.
  Vector<ruins::TileCoord> chosen;
  ruins::choose_tiles(11, ruins::TileCoord{0, 0}, ruins::TileCoord{4096, 4095}, 4, 0.0f, chosen);
  CHECK(chosen.empty());
  ruins::choose_tiles(11, ruins::TileCoord{-3, -3}, ruins::TileCoord{2, 2}, 4, 0.0f, chosen);
  CHECK(chosen.size() == 4);

  const test::TempDir tmp("ruins_scene_generator_hostile");
  std::string error;
  REQUIRE_MESSAGE(ruins::write_synthetic_kit(tmp.file("kit"), ruins::SyntheticKitOptions{}, &error),
                  error);
  const scene_gen::PlacementGeneratorDesc* generator =
      scene_gen::GeneratorRegistry::global().find_placement("ruins");
  REQUIRE(generator != nullptr);
  scene_gen::Context context;
  context.dir = tmp.path();
  context.where = "scene.json: ruins 0";
  void* state = nullptr;
  // One tile past the bound, so that the code before the mend ranks 16.8 million tiles (about
  // 200 MB) and stops: the full i32 square, as a hostile scene names it, walked without end and
  // took 29 GB before it was stopped (T191's log). Its count is checked as arithmetic alone.
  CHECK_FALSE(generator->open(entry_json(R"({"kit":"kit/kit.json","count":1,)"
                                         R"("tile_min":[0,0],"tile_max":[4096,4095]})"),
                              context, &state, &error));
  CHECK(error.find("4097 x 4096") != std::string::npos);
  CHECK(error.find("16777216") != std::string::npos);
  constexpr i32 k_first = -2147483647 - 1;
  constexpr i32 k_last = 2147483647;
  CHECK(ruins::chosen_span(ruins::TileCoord{k_first, k_first}, ruins::TileCoord{k_last, k_last}) ==
        ~u64{0});  // saturated
  CHECK(ruins::chosen_span(ruins::TileCoord{k_first, 0}, ruins::TileCoord{k_last, 0}) == u64{1}
                                                                                             << 32);
  CHECK_FALSE(generator->open(entry_json(R"({"kit":"kit/kit.json","count":1,"tile_size":1e30})"),
                              context, &state, &error));
  CHECK(error.find("at most 10000 m") != std::string::npos);
}

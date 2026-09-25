// The ruins consumer (systems/world/ruins_tiles.h; docs/subsystems/world.md, "The consumers"):
// a tile's building assembled when the ring activates it, in the representation its ring draws —
// blocks in the inner ring, sections beyond, walls only past that — exactly the assembler's
// building for that tile; dropped when it goes; the tail handed on in tile order; and the pair
// budget drawing the nearest tiles first before the renderer is asked. No device: the scene is
// read, not loaded, and its parts are laid out by hand, one cluster a mesh, which is what the kit
// of boxes and the low block kit are.
#include <domain/ruins/assembler.h>
#include <domain/ruins/blocks.h>
#include <domain/ruins/kit.h>
#include <domain/ruins/synthetic_kit.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>
#include <systems/world/ruins_tiles.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <fstream>
#include <string>

using namespace engine;
using namespace engine::world;

namespace {

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

// A 200 m terrain, and a ruins entry over the 8 x 8 tiles round the origin (24 m tiles, every tile
// a building), drawn in blocks where the ring says so. `world` is the scene's world block, or none.
std::string scene_text(const std::string& world) {
  return R"({"format":"engine.scene.v1","name":"streamed",)"
         R"("terrain":{"size":65,"extent":200,"seed":3,"dune_height":1.5},)"
         R"("ruins":[{"name":"town","kit":"kit/kit.json","seed":11,"tile_size":24,)"
         R"("tile_min":[-4,-4],"tile_max":[3,3],"count":64,"wind_deg":45,)"
         R"("representation":"Blocks","block_kit":"blocks/block-kit.json"}])" +
         world + "}";
}

// What `load_scene` would produce that the consumer reads: the parts (one cluster a mesh here), the
// streamed entries, the terrain, and a prefix of pairs (the terrain's, say).
void fake_load(const renderer::SceneDesc& desc, renderer::SceneData& out) {
  out.parts.resize(desc.meshes.size());
  for (u32 m = 0; m < desc.meshes.size(); ++m) {
    out.parts[m].first_cluster = m;
    out.parts[m].cluster_count = 1;
  }
  out.streamed_ruins = desc.streamed_ruins;
  out.terrain = desc.terrain;
  out.world = desc.world;
  out.dynamic = desc.world.enabled;
  out.pair_count = 100;
}

struct Kits {
  ruins::Kit kit;
  ruins::BlockKit blocks;
};

// The instances the scene reader would have made of one tile's building, in `detail`, and how many
// of them are rubble (debris members, fallen blocks).
Vector<renderer::SceneInstance> expected(const Kits& kits, const renderer::SceneData& scene,
                                         TileCoord tile, RingRuins detail, u32* rubble = nullptr) {
  const renderer::StreamedRuins& entry = scene.streamed_ruins[0];
  renderer::TerrainSampler ground(scene.terrain);
  ruins::Placement placement;
  placement.world_seed = entry.seed;
  placement.tile_cm = ruins::to_cm(entry.tile_size);
  placement.wind_step = ruins::yaw_step_from_degrees(entry.wind_deg);
  placement.ground =
      ruins::Ground{[](const void* c, f32 x, f32 z) noexcept {
                      return static_cast<const renderer::TerrainSampler*>(c)->height(x, z);
                    },
                    &ground};
  Vector<renderer::SceneInstance> out;
  std::string error;
  if (detail == RingRuins::Blocks) {
    ruins::BlockAssembler layer(kits.kit, kits.blocks);
    ruins::BlockOutput built;
    REQUIRE(layer.assemble(placement, ruins::TileCoord{tile.x, tile.z}, built, &error));
    for (const ruins::Block& block : built.blocks) {
      renderer::SceneInstance i;
      i.mesh = entry.block_first_mesh + kits.blocks.blocks[block.block].mesh_index;
      i.transform.position = ruins::block_translation(kits.blocks, block);
      out.push_back(i);
      if (rubble != nullptr && (block.flags & ruins::k_block_fallen) != 0) ++*rubble;
    }
    return out;
  }
  placement.detail = detail == RingRuins::Walls ? ruins::Detail::walls : ruins::Detail::full;
  ruins::Assembler assembler(kits.kit);
  ruins::Output built;
  REQUIRE(assembler.assemble(placement, ruins::TileCoord{tile.x, tile.z}, built, &error));
  for (const ruins::Instance& piece : built.instances) {
    renderer::SceneInstance i;
    i.mesh = entry.kit_first_mesh + kits.kit.members[piece.member].mesh_index;
    i.transform.position = ruins::instance_translation(kits.kit, piece);
    out.push_back(i);
    if (rubble != nullptr && piece.kind == static_cast<u8>(ruins::PieceKind::debris)) ++*rubble;
  }
  return out;
}

bool same(std::span<const renderer::SceneInstance> a, std::span<const renderer::SceneInstance> b) {
  if (a.size() != b.size()) return false;
  for (u32 i = 0; i < a.size(); ++i) {
    if (a[i].mesh != b[i].mesh || !(a[i].transform.position == b[i].transform.position))
      return false;
  }
  return true;
}

struct Sink {
  u32 calls = 0;
  Vector<renderer::SceneInstance> last;
  static bool take(void* c, std::span<const renderer::SceneInstance> tail, std::string*) {
    auto* self = static_cast<Sink*>(c);
    ++self->calls;
    self->last.assign(tail.begin(), tail.end());
    return true;
  }
};

sim::ObserverSet at(f32 x, f32 z) {
  sim::ObserverSet set;
  set.add(Vec3{x, 30.0f, z}, 1.0f);
  return set;
}

RingParams rings() {
  RingParams p;
  p.tile_size = 24.0f;
  p.ring_count = 3;
  p.radius[0] = 1.5f;
  p.radius[1] = 3.0f;
  p.radius[2] = 5.0f;
  p.max_activations = 0;
  p.max_deactivations = 0;
  return p;
}

struct Fixture {
  test::TempDir tmp{"engine_world_ruins"};
  Kits kits;
  renderer::SceneDesc desc;
  renderer::SceneData scene;

  explicit Fixture(const std::string& world = R"(,"world":{"tile_size":24})") {
    std::string error;
    std::string kit_path;
    std::string block_path;
    REQUIRE_MESSAGE(ruins::write_synthetic_kit(tmp.file("kit"), ruins::SyntheticKitOptions{},
                                               &error, &kit_path),
                    error);
    REQUIRE_MESSAGE(ruins::write_synthetic_block_kit(
                        tmp.file("blocks"), ruins::SyntheticBlockOptions{}, &error, &block_path),
                    error);
    REQUIRE(ruins::read_kit_file(kit_path, kits.kit, error));
    REQUIRE(ruins::read_block_kit_file(block_path, kits.blocks, error));
    const std::string path = tmp.file("scene.json");
    REQUIRE(write_text(path, scene_text(world)));
    REQUIRE_MESSAGE(renderer::read_scene_file(path, desc, error), error);
    fake_load(desc, scene);
  }
};

}  // namespace

TEST_CASE("world ruins: each ring's tiles draw the assembler's building in the ring's form") {
  Fixture f;
  REQUIRE(f.scene.streamed_ruins.size() == 1);
  RuinsTilesConfig config;
  config.tile_size = 24.0f;
  RuinsTiles ruins;
  std::string error;
  REQUIRE_MESSAGE(ruins.create(f.scene, config, &error), error);
  Sink sink;
  ruins.set_sink(&Sink::take, &sink);
  World world(rings());
  world.add_consumer(ruins.consumer());

  // From the centre of tile (0, 0): the 3 x 3 round it in blocks, then sections, then walls.
  world.update(at(12.0f, 12.0f), 0);
  CHECK(sink.calls == 1);
  u32 checked[3] = {};
  u32 rubble = 0;
  for (u32 i = 0; i < world.ring().active_count(); ++i) {
    const TileCoord tile = tile_of_key(world.ring().active_keys()[i]);
    const u8 ring = world.ring().active_rings()[i];
    CHECK(ruins.has_building(tile) == (tile.x >= -4 && tile.x <= 3 && tile.z >= -4 && tile.z <= 3));
    if (!ruins.has_building(tile)) {
      CHECK(ruins.tile_instances(tile).empty());
      continue;
    }
    const Vector<renderer::SceneInstance> want =
        expected(f.kits, f.scene, tile, config.ruins[ring], &rubble);
    CHECK(same(ruins.tile_instances(tile),
               std::span<const renderer::SceneInstance>(want.data(), want.size())));
    ++checked[ring];
  }
  CHECK(checked[0] == 9);
  CHECK(checked[1] > 10);
  CHECK(checked[2] > 10);
  // The tail is every tile's instances, in tile order.
  Vector<renderer::SceneInstance> joined;
  for (const u64 key : world.ring().active_keys()) {
    for (const renderer::SceneInstance& i : ruins.tile_instances(tile_of_key(key)))
      joined.push_back(i);
  }
  CHECK(same(std::span<const renderer::SceneInstance>(sink.last.data(), sink.last.size()),
             std::span<const renderer::SceneInstance>(joined.data(), joined.size())));
  CHECK(ruins.stats().instances == joined.size());
  CHECK(ruins.stats().pairs == joined.size());  // one cluster a mesh
  CHECK(ruins.stats().laid == 9);
  // Each of the tail's instances says whether it is rubble: as many as the tiles' debris members
  // and fallen blocks.
  REQUIRE(ruins.tail_rubble().size() == ruins.tail().size());
  u32 marked = 0;
  for (const u8 r : ruins.tail_rubble())
    marked += r;
  CHECK(marked == rubble);
  CHECK(rubble > 0);

  // A step east: tile (-1, 0) leaves the inner ring for sections, (2, 0) arrives in blocks, and
  // the tiles that went are gone from the tail.
  world.update(at(36.0f, 12.0f), 1);
  CHECK(sink.calls == 2);
  const Vector<renderer::SceneInstance> sections =
      expected(f.kits, f.scene, TileCoord{-1, 0}, RingRuins::Sections);
  CHECK(same(ruins.tile_instances(TileCoord{-1, 0}),
             std::span<const renderer::SceneInstance>(sections.data(), sections.size())));
  const Vector<renderer::SceneInstance> blocks =
      expected(f.kits, f.scene, TileCoord{2, 0}, RingRuins::Blocks);
  CHECK(same(ruins.tile_instances(TileCoord{2, 0}),
             std::span<const renderer::SceneInstance>(blocks.data(), blocks.size())));
  u32 tail = 0;
  for (const u64 key : world.ring().active_keys())
    tail += static_cast<u32>(ruins.tile_instances(tile_of_key(key)).size());
  CHECK(sink.last.size() == tail);

  // Away from everything: every building dropped, and the tail is empty.
  world.update(at(5000.0f, 0.0f), 2);
  CHECK(sink.last.empty());
  CHECK(ruins.stats().buildings == 0);
  CHECK(ruins.stats().dropped > 40);
  CHECK(ruins.stats().pairs == 0);
}

TEST_CASE(
    "world ruins: the pair budget draws the nearest tiles first, and the tail stays under it") {
  Fixture f;
  // How many pairs each ring's tiles take with no budget (one cluster a mesh: a pair an instance).
  u32 ring_pairs[3] = {};
  {
    RuinsTilesConfig config;
    config.tile_size = 24.0f;
    RuinsTiles ruins;
    std::string error;
    REQUIRE_MESSAGE(ruins.create(f.scene, config, &error), error);
    World world(rings());
    world.add_consumer(ruins.consumer());
    world.update(at(12.0f, 12.0f), 0);
    CHECK(ruins.stats().withheld == 0);
    for (u32 i = 0; i < world.ring().active_count(); ++i) {
      const TileCoord tile = tile_of_key(world.ring().active_keys()[i]);
      ring_pairs[world.ring().active_rings()[i]] +=
          static_cast<u32>(ruins.tile_instances(tile).size());
    }
  }
  REQUIRE(ring_pairs[0] > 0);
  REQUIRE(ring_pairs[1] > 0);
  // Room for the inner ring and half the middle one. Decided per event, in tile order, the budget
  // went to whichever tiles came first, and a first fill refused buildings beside the observer
  // for ones at the edge (E35's first run of the ashlar kit under the default rings).
  RuinsTilesConfig config;
  config.tile_size = 24.0f;
  config.max_pairs = f.scene.pair_count + ring_pairs[0] + ring_pairs[1] / 2;
  RuinsTiles ruins;
  std::string error;
  REQUIRE_MESSAGE(ruins.create(f.scene, config, &error), error);
  Sink sink;
  ruins.set_sink(&Sink::take, &sink);
  World world(rings());
  world.add_consumer(ruins.consumer());
  ruins.set_ring(&world.ring());
  // Round the middle of the town, where the budget cannot draw every tile.
  const f32 path[4][2] = {{12.0f, 12.0f}, {0.0f, 0.0f}, {-12.0f, -6.0f}, {6.0f, -12.0f}};
  for (u32 step = 0; step < 4; ++step) {
    const UpdateStats& s = world.update(at(path[step][0], path[step][1]), step);
    CAPTURE(step);
    CHECK(s.refused == 0);  // withheld is the budget's, not a refusal of the event
    CHECK(ruins.stats().withheld > 0);
    CHECK(f.scene.pair_count + ruins.stats().pairs <= config.max_pairs);
    CHECK(sink.last.size() == ruins.stats().pairs);
    // Every tile drawn outranks every tile withheld: by ring, then by the ring's score.
    f32 worst_drawn[3] = {-1.0f, -1.0f, -1.0f};
    u8 best_withheld_ring = k_inactive;
    f32 best_withheld_score = 1.0e30f;
    for (u32 i = 0; i < world.ring().active_count(); ++i) {
      const TileCoord tile = tile_of_key(world.ring().active_keys()[i]);
      const u8 ring = world.ring().active_rings()[i];
      if (ruins.tile_instances(tile).empty()) continue;
      const f32 score = world.ring().score_of(tile);
      if (ruins.withheld(tile)) {
        CHECK(ring != 0);  // the inner ring fits by construction
        if (ring < best_withheld_ring ||
            (ring == best_withheld_ring && score < best_withheld_score)) {
          best_withheld_ring = ring;
          best_withheld_score = score;
        }
      } else if (score > worst_drawn[ring]) {
        worst_drawn[ring] = score;
      }
    }
    REQUIRE(best_withheld_ring != k_inactive);
    for (u8 r = 0; r < 3; ++r) {
      if (worst_drawn[r] < 0.0f) continue;
      CHECK(r <= best_withheld_ring);
      if (r == best_withheld_ring) CHECK(worst_drawn[r] <= best_withheld_score);
    }
  }
  CHECK(ruins.stats().refused >= ruins.stats().withheld);
  // Out at the town's edge the ring holds a column of it, the budget has room, and nothing is
  // withheld: a tile left out comes back when there is room, with no event of its own.
  world.update(at(200.0f, 12.0f), 4);
  CHECK(ruins.stats().withheld == 0);
  CHECK(sink.last.size() == ruins.stats().pairs);
  CHECK(ruins.stats().pairs > 0);
}
TEST_CASE("world ruins: a streamed scene keeps its kits' meshes and none of its buildings") {
  // The scene reader's half (renderer::read_scene_file): with a world, the ruins entry is left for
  // the ring — both kits' meshes appended, no instance — and without one, today's whole read.
  Fixture streamed;
  CHECK(streamed.desc.world.enabled);
  CHECK(streamed.desc.instances.size() == 1);  // the terrain's only
  REQUIRE(streamed.desc.streamed_ruins.size() == 1);
  const renderer::StreamedRuins& entry = streamed.desc.streamed_ruins[0];
  CHECK(entry.blocks);
  CHECK(entry.kit_meshes == streamed.kits.kit.meshes.size());
  CHECK(entry.block_meshes == streamed.kits.blocks.meshes.size());
  CHECK(entry.block_first_mesh == entry.kit_first_mesh + entry.kit_meshes);
  CHECK(streamed.desc.meshes.size() == entry.kit_meshes + entry.block_meshes + 1);
  CHECK(streamed.desc.ruin_instances == 0);

  Fixture whole{""};
  CHECK_FALSE(whole.desc.world.enabled);
  CHECK(whole.desc.streamed_ruins.empty());
  CHECK(whole.desc.ruin_buildings == 64);

  // `--world` on a file with no world block streams it with the defaults, at the entries' own grid
  // only when that is the world's: 24 m tiles against the default 32 m are refused.
  renderer::SceneFileOptions options;
  options.world = true;
  renderer::SceneDesc forced;
  std::string error;
  CHECK_FALSE(renderer::read_scene_file(whole.tmp.file("scene.json"), options, forced, error));
  CHECK(error.find("one grid") != std::string::npos);
}

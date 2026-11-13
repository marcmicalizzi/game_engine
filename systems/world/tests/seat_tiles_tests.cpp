// A streamed tile's trees are seated as a whole read's are (G201 stage 2; world.md, "A tile's trees
// are seated"; renderer.md, "A tree seated by its footprint"): a placement generator and a ground
// of this test's own — a bank a metre high across a gentle slope, and a grid of trees over it whose
// mesh's container records a seat — read whole and streamed. Every tree of a tile stands where the
// whole read's load seated it, to the bit, with the same slope, whichever tile is built first; and
// the tile's build time with its seating, for a tile of a known count of trees.
#include <core/math/world.h>
#include <domain/geometry/cluster_file.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/seat.h>
#include <systems/renderer/scene.h>
#include <systems/world/placement_tiles.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
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

f32 bank_height(const void*, f32 x, f32 z) noexcept { return 0.2f * z - (x >= 0.0f ? 1.0f : 0.0f); }
void bank_destroy(void*) noexcept {}
constexpr scene_gen::GroundOps k_bank_ops{.destroy = &bank_destroy, .height = &bank_height};
int g_bank_state = 0;
bool bank_make(const scene::Terrain&, const scene_gen::Context&, scene_gen::GroundProvider& out,
               std::string*) {
  out = scene_gen::GroundProvider(&k_bank_ops, &g_bank_state);
  return true;
}
constexpr scene_gen::GroundProviderDesc k_bank{.name = "seat-tiles-bank", .make = &bank_make};
const scene_gen::Registrar k_bank_registrar{k_bank};

// A grid of trees `step` apart over [-span, span) in x and z, the bank's edge among them; one mesh,
// resident, named by its path.
struct Grove {
  std::string path;
  f64 span = 8.0;
  f64 step = 0.5;
};
bool grove_open(const JsonValue& params, const scene_gen::Context&, void** state,
                std::string* error) {
  const JsonValue* path = params.find("path");
  std::string_view text;
  if (path == nullptr || !path->get_string(text)) {
    if (error != nullptr) *error = "seat-tiles-grove needs a path";
    return false;
  }
  auto* g = new Grove;
  g->path = std::string(text);
  *state = g;
  return true;
}
void grove_close(void* state) noexcept { delete static_cast<Grove*>(state); }
// The trees whose place falls in [x0, x1) by [z0, z1), standing on the ground.
void grove_place(const Grove& g, const scene_gen::Context& context, f64 x0, f64 x1, f64 z0, f64 z1,
                 scene_gen::Placements& out) {
  const i32 n = static_cast<i32>(2.0 * g.span / g.step);
  for (i32 j = 0; j < n; ++j) {
    for (i32 i = 0; i < n; ++i) {
      // A quarter step off the lattice, so a tree stands a little before the bank's edge at x 0.
      const f64 x = -g.span + g.step * (static_cast<f64>(i) + 0.25);
      const f64 z = -g.span + g.step * (static_cast<f64>(j) + 0.5);
      if (x < x0 || x >= x1 || z < z0 || z >= z1) continue;
      scene_gen::Placement p;
      p.mesh = 0;
      p.position = WorldPos{x, 0.0, z};
      p.position.y = context.ground.surface(p.position);
      p.id = static_cast<u32>(j * n + i + 1);
      out.instances.push_back(p);
    }
  }
}
bool grove_expand(void* state, const scene_gen::Context& context, scene_gen::Placements& out,
                  std::string*) {
  const Grove& g = *static_cast<const Grove*>(state);
  out.meshes.push_back(scene_gen::PlacementMesh{g.path, "seat-tiles-tree"});
  grove_place(g, context, -1e9, 1e9, -1e9, 1e9, out);
  return true;
}
bool grove_meshes(void* state, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
                  std::string*) {
  out.push_back(
      scene_gen::PlacementMesh{static_cast<const Grove*>(state)->path, "seat-tiles-tree"});
  return true;
}
bool grove_tile(void* state, scene_gen::TileCoord tile, const scene_gen::Context& context,
                scene_gen::Placements& out, std::string*) {
  const f64 t = static_cast<f64>(context.tile_size);
  grove_place(*static_cast<const Grove*>(state), context, t * tile.x, t * (tile.x + 1), t * tile.z,
              t * (tile.z + 1), out);
  return true;
}
constexpr scene_gen::PlacementGeneratorDesc k_grove{.name = "seat-tiles-grove",
                                                    .open = &grove_open,
                                                    .close = &grove_close,
                                                    .expand = &grove_expand,
                                                    .meshes = &grove_meshes,
                                                    .tile = &grove_tile};
const scene_gen::Registrar k_grove_registrar{k_grove};

std::string json_path(std::string path) {
  for (char& c : path)
    if (c == '\\') c = '/';
  return path;
}

std::string scene_text(const std::string& tree, bool world) {
  return std::string(R"({"format":"engine.scene.v1","name":"seat-tiles",)"
                     R"("meshes":[{"name":"marker","path":")") +
         json_path(tree) +
         R"("}],"instances":[{"mesh":0,"translation":[30,0,30],"ground":true}],)"
         R"("placements":[{"generator":"seat-tiles-grove","params":{"path":")" +
         json_path(tree) + R"("}}],)" + (world ? R"("world":{"tile_size":8},)" : "") +
         R"("terrain":{"size":17,"extent":40,"provider":"seat-tiles-bank"}})";
}

}  // namespace

TEST_CASE("seat tiles: a streamed tile's trees are seated where the whole read seats them") {
  const test::TempDir tmp("world_seat_tiles");
  REQUIRE(tmp.ok());
  std::string error;
  // The tree: the procedural grid's DAG with a seat, a rim of 0.4 m and a skirt of 0.55 m.
  {
    renderer::SceneDesc grid;
    grid.meshes.push_back("");
    grid.heightfield_grid = 9;
    grid.cache = false;
    renderer::SceneData made;
    REQUIRE_MESSAGE(renderer::load_scene(grid, made, error), error);
    geometry::ClusterFileData file;
    file.mesh = std::move(made.lod);
    file.seat = geometry::ClusterFileSeat{0.4f, 0.02f, -0.55f};
    REQUIRE_MESSAGE(geometry::write_cluster_file(tmp.file("tree.clusters"), file, &error), error);
  }
  const std::string tree = tmp.file("tree.clusters");

  // The whole read: the grid's 1,024 trees seated by the load.
  REQUIRE(write_text(tmp.file("whole.json"), scene_text(tree, false)));
  renderer::SceneDesc whole;
  REQUIRE_MESSAGE(renderer::read_scene_file(tmp.file("whole.json"), whole, error), error);
  whole.cache = false;
  renderer::SceneData whole_data;
  REQUIRE_MESSAGE(renderer::load_scene(whole, whole_data, error), error);
  Vector<renderer::SceneInstance> seated = whole.instances;
  renderer::SceneLoadReport report;
  renderer::seat_scene_instances(whole.terrain, {seated.data(), seated.size()},
                                 {whole_data.sources.data(), whole_data.sources.size()},
                                 {whole.mesh_info.data(), whole.mesh_info.size()}, report);
  CHECK(report.seated == 1u + 1024u);
  CHECK(report.lowered == whole_data.load.lowered);
  // The column a quarter step before the bank's edge, and nothing else, hangs over it: 32 trees.
  CHECK(report.lowered == 32u);

  // Streamed: every tile of the grid built for a query (the ring's innermost representation), in
  // an order that is not the whole read's.
  REQUIRE(write_text(tmp.file("streamed.json"), scene_text(tree, true)));
  renderer::SceneDesc streamed;
  REQUIRE_MESSAGE(renderer::read_scene_file(tmp.file("streamed.json"), streamed, error), error);
  streamed.cache = false;
  renderer::SceneData scene;
  REQUIRE_MESSAGE(renderer::load_scene(streamed, scene, error), error);
  REQUIRE(scene.streamed.size() >= 1u);
  PlacementTilesConfig config;
  config.tile_size = 8.0f;
  PlacementTiles tiles;
  REQUIRE_MESSAGE(tiles.create(scene, config, &error), error);
  u32 found = 0;
  u32 lowered = 0;
  for (i32 tz = 0; tz >= -1; --tz) {
    for (i32 tx = 0; tx >= -1; --tx) {
      const std::span<const renderer::SceneInstance> got = tiles.query_tile(TileCoord{tx, tz}, ~0u);
      for (const renderer::SceneInstance& s : got) {
        if (!(scene.sources[s.mesh].seat.radius_m > 0.0f)) continue;
        // The whole read's tree at the same place (x and z are the generator's, unseated).
        for (u32 i = 1; i < seated.size(); ++i) {
          if (seated[i].origin.x != s.origin.x || seated[i].origin.z != s.origin.z) continue;
          ++found;
          CHECK(seated[i].origin.y == s.origin.y);
          CHECK(seated[i].slope == s.slope);
          if (s.origin.y < whole.instances[i].origin.y) ++lowered;
          break;
        }
      }
    }
  }
  CHECK(found == 1024u);
  CHECK(lowered == 32u);
  const PlacementTilesStats& stats = tiles.stats();
  CHECK(stats.seated >= 1024u);
  CHECK(stats.lowered == 32u);
  MESSAGE("four 8 m tiles of 256 trees each: the tiles' build "
          << stats.query_ns / 1000 << " us, of which seating " << stats.seat_ns / 1000 << " us ("
          << stats.seated << " trees, 17 queries each, the test's analytic ground)");
}
